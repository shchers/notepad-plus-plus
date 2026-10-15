// Phase 2 tests: documents independent of views, two views with clones,
// session.xml, snapshots, recent files, outside-change detection, backup on
// save and config.xml import. Drives a real MainWindow with dialogs replaced
// by a scripted prompt handler.

#include "core/FileManager.h"
#include "core/NppConfig.h"
#include "core/RecentFiles.h"
#include "core/Session.h"
#include "platform/FileIO.h"
#include "platform/Paths.h"
#include "platform/TextCodec.h"
#include "ui/MainWindow.h"

#include "menuCmdID.h"

#include <gtk/gtk.h>

#include <chrono>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace npp;
using Answer = ui::MainWindow::Answer;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        ++g_checks;                                                                 \
        if (!(cond)) {                                                              \
            ++g_failures;                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                           \
    } while (0)

namespace {

fs::path g_dir; // scratch files

void writeText(const fs::path& p, const std::string& s)
{
    std::ofstream(p, std::ios::binary) << s;
}

std::string readText(const fs::path& p)
{
    std::string s;
    platform::readFile(p, s, nullptr);
    return s;
}

void pump()
{
    while (g_main_context_iteration(nullptr, FALSE)) {
    }
}

// Spins the main loop until `cond` holds (file monitor events are async).
template <class F>
bool waitFor(F cond, int ms = 5000)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!cond()) {
        if (std::chrono::steady_clock::now() > end)
            return false;
        pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return true;
}

// Scripted answers; records the questions asked.
struct Prompter {
    std::deque<Answer> answers;
    std::vector<std::string> asked;
    ui::MainWindow::PromptHandler handler()
    {
        return [this](const std::string& primary, const std::string&, bool) {
            asked.push_back(primary);
            if (answers.empty())
                return Answer::No;
            Answer a = answers.front();
            answers.pop_front();
            return a;
        };
    }
};

// Runs a menu command through its window action, as the menus do.
void activate(ui::MainWindow& w, int id)
{
    const std::string name = ui::CommandTable::actionName(id);
    CHECK(g_action_group_has_action(G_ACTION_GROUP(w.window()), name.c_str()));
    g_action_group_activate_action(G_ACTION_GROUP(w.window()), name.c_str(), nullptr);
    pump();
}

void typeText(ui::MainWindow& w, const char* text)
{
    w.activeView()->execute(SCI_DOCUMENTEND);
    w.activeView()->execute(SCI_REPLACESEL, 0, reinterpret_cast<sptr_t>(text));
}

// ---------------------------------------------------------------------------

void testCoreModels()
{
    core::RecentFiles r(3);
    r.add("/a");
    r.add("/b");
    r.add("/a"); // moves to the front
    r.add("/c");
    r.add("/d"); // drops the oldest
    CHECK((r.items() == std::vector<std::string>{"/d", "/c", "/a"}));
    r.remove("/c");
    CHECK((r.items() == std::vector<std::string>{"/d", "/a"}));

    CHECK(platform::codepageForCharset("windows-1252") == 1252);
    CHECK(platform::codepageForCharset("ISO-8859-1") == 28591);
    CHECK(platform::codepageForCharset("UTF-8") == -1);
    CHECK(platform::charsetForCodepage(1251) == "WINDOWS-1251");
    auto d = platform::decodeAs("caf\xE9", "WINDOWS-1252");
    CHECK(d && d->utf8 == "caf\xC3\xA9" && d->encoding.charset == "WINDOWS-1252");

    const std::int64_t us = 1700000000123456;
    CHECK(platform::fromFileTime(platform::toFileTime(us)) == us);

    // session.xml round trip, including attributes the port only carries.
    core::Session s;
    s.activeView = 1;
    s.activeIndex[0] = 1;
    core::SessionFile f;
    f.filename = "/tmp/x.cpp";
    f.lang = "C++";
    f.encoding = 1252;
    f.userReadOnly = true;
    f.firstVisibleLine = 10;
    f.startPos = 5;
    f.endPos = 9;
    f.marks = {1, 4};
    f.folds = {7};
    f.originalFileLastModifTimestamp = platform::toFileTime(us);
    f.tabColourId = 2;
    s.views[0] = {f, f};
    s.views[0][1].filename = "new 3";
    s.views[1] = {f};
    const fs::path sx = g_dir / "rt-session.xml";
    CHECK(core::writeSession(sx, s));
    core::Session back;
    CHECK(core::readSession(sx, back));
    CHECK(back == s);
    CHECK(core::writeSession(sx, s)); // second write keeps a backup copy, as upstream
    CHECK(fs::exists(sx.string() + ".inCaseOfCorruption.bak"));
    CHECK(readText(sx).find("originalFileLastModifTimestampHigh=") != std::string::npos);

    // Snapshots stay inside the backup folder.
    CHECK(core::isSnapshotPath(core::snapshotDir() / "new 1@2026-01-01_000000"));
    CHECK(!core::isSnapshotPath(g_dir / "elsewhere"));
    CHECK(!core::isSnapshotPath(core::snapshotDir() / ".." / "session.xml"));

    // Backup on save, upstream naming.
    const fs::path doc = g_dir / "bak.txt";
    writeText(doc, "v1");
    auto simple = core::backupBeforeSave(doc, platform::SaveBackup::Simple, {}, nullptr);
    CHECK(simple && *simple == g_dir / "bak.txt.bak" && readText(*simple) == "v1");
    auto verbose = core::backupBeforeSave(doc, platform::SaveBackup::Verbose, {}, nullptr);
    CHECK(verbose && verbose->parent_path() == g_dir / "nppBackup" && verbose->extension() == ".bak");
    auto none = core::backupBeforeSave(g_dir / "missing.txt", platform::SaveBackup::Simple, {}, nullptr);
    CHECK(none && none->empty());
}

void testConfigImport()
{
    const fs::path existing = g_dir / "recent-existing.txt";
    writeText(existing, "x");
    const fs::path cfg = g_dir / "config.xml";
    writeText(cfg, "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n"
                   "<NotepadPlus>\n"
                   "  <FindHistory nbMaxFindHistoryPath=\"10\" />\n"
                   "  <History nbMaxFile=\"15\" inSubMenu=\"no\" customLength=\"-1\">\n"
                   "    <File filename=\"C:\\Users\\me\\notes.txt\" />\n"
                   "    <File filename=\"" + existing.string() + "\" />\n"
                   "  </History>\n"
                   "  <GUIConfigs>\n"
                   "    <GUIConfig name=\"Auto-detection\">auto</GUIConfig>\n"
                   "    <GUIConfig name=\"MaintainIndent\">0</GUIConfig>\n"
                   "    <GUIConfig name=\"RememberLastSession\">no</GUIConfig>\n"
                   "    <GUIConfig name=\"TabSetting\" replaceBySpace=\"yes\" size=\"2\" backspaceUnindent=\"no\" />\n"
                   "    <GUIConfig name=\"ScintillaPrimaryView\" lineNumberMargin=\"show\" indentGuideLine=\"hide\" Wrap=\"yes\" zoom=\"3\" whiteSpaceShow=\"show\" eolShow=\"show\" />\n"
                   "    <GUIConfig name=\"Backup\" action=\"2\" useCustumDir=\"yes\" dir=\"D:\\bak\" isSnapshotMode=\"no\" snapshotBackupTiming=\"9000\" />\n"
                   "    <GUIConfig name=\"DarkMode\" enable=\"yes\" darkThemeName=\"DarkModeDefault.xml\" lightThemeName=\"\" />\n"
                   "  </GUIConfigs>\n"
                   "</NotepadPlus>\n");
    std::string err;
    auto c = core::readNppConfig(cfg, &err);
    CHECK(c.has_value());
    if (!c)
        return;
    platform::Settings s;
    CHECK(core::applyNppConfig(*c, s) > 10);
    CHECK(s.changeDetection == platform::ChangeDetection::AutoUpdate);
    CHECK(!s.autoIndent);
    CHECK(!s.rememberSession);
    CHECK(s.tabSize == 2 && s.replaceTabsBySpace);
    CHECK(s.wordWrap && !s.showIndentGuides && s.showSpaces && s.showEol && s.zoom == 3);
    CHECK(s.saveBackup == platform::SaveBackup::Verbose && s.saveBackupDir.empty()); // Windows dir dropped
    CHECK(!s.snapshot && s.snapshotIntervalMs == 9000);
    CHECK(s.theme == "DarkModeDefault.xml"); // shipped theme
    CHECK(s.maxRecentFiles == 15);
    CHECK((s.recentFiles == std::vector<std::string>{existing.string()})); // Windows path skipped
    CHECK(!core::readNppConfig(g_dir / "rt-session.xml")); // not a config.xml
}

std::unique_ptr<ui::MainWindow> makeWindow(GtkApplication* app, const core::LanguageCatalog& langs, Prompter& p,
                                           bool useSession = true)
{
    auto w = std::make_unique<ui::MainWindow>(app, const_cast<core::LanguageCatalog&>(langs), useSession);
    w->setPromptHandler(p.handler());
    w->settings().changeDetection = platform::ChangeDetection::Notify;
    return w;
}

void testViewsAndClones(GtkApplication* app, const core::LanguageCatalog& langs)
{
    Prompter p;
    auto w = makeWindow(app, langs, p, false);
    const fs::path a = g_dir / "a.cpp", b = g_dir / "b.txt";
    writeText(a, "int a;\n");
    writeText(b, "bee\n");
    w->newDocument();
    CHECK(w->openFiles({a, b}) == 2);
    CHECK(w->documentCount() == 2); // pristine "new 1" was replaced
    CHECK(w->tabCount(0) == 2 && w->tabCount(1) == 0);

    // Clone b: two tabs, one buffer, one Scintilla document.
    core::Buffer* bb = w->activeBuffer();
    ui::EditorView* mainView = w->activeView();
    w->cloneToOtherView();
    CHECK(w->activeViewIndex() == 1 && w->tabCount(1) == 1);
    CHECK(w->documentCount() == 2 && w->activeBuffer() == bb);
    ui::EditorView* subView = w->activeView();
    CHECK(subView != mainView);
    CHECK(subView->execute(SCI_GETDOCPOINTER) == mainView->execute(SCI_GETDOCPOINTER));
    typeText(*w, "more");
    CHECK(mainView->text() == "bee\nmore");
    CHECK(w->isModified(*bb) && mainView->isModified());

    // Closing one side of a clone asks nothing and keeps the buffer.
    activate(*w, IDM_FILE_CLOSE);
    CHECK(p.asked.empty());
    CHECK(w->tabCount(1) == 0 && w->activeViewIndex() == 0); // empty view hides, focus moves
    CHECK(w->documentCount() == 2 && w->isModified(*bb));

    // Move a.cpp to the sub view, then back.
    w->openFiles({a});
    w->moveToOtherView();
    CHECK(w->tabCount(0) == 1 && w->tabCount(1) == 1 && w->activeViewIndex() == 1);
    CHECK(w->activeBuffer()->language && w->activeBuffer()->language->name == "cpp");
    w->switchToOtherView();
    CHECK(w->activeViewIndex() == 0 && w->activeBuffer() == bb);

    // Closing the last tab of a modified buffer asks; Cancel keeps it.
    p.answers = {Answer::Cancel};
    activate(*w, IDM_FILE_CLOSE);
    CHECK(p.asked.size() == 1 && w->documentCount() == 2);
    p.answers = {Answer::No};
    activate(*w, IDM_FILE_CLOSE);
    CHECK(w->documentCount() == 1);
    // ... and closed files enter the recent list; reopening takes them out.
    CHECK(!w->recentFiles().empty() && w->recentFiles().items().front() == b.string());
    activate(*w, IDM_FILE_RESTORELASTCLOSEDFILE);
    CHECK(w->documentCount() == 2 && w->activeBuffer()->path == b);
    CHECK(w->recentFiles().empty() || w->recentFiles().items().front() != b.string());

    // Edit > Read-Only is per buffer and shared by clones.
    // A check item toggles through "change-state", as GtkModelMenu does.
    const std::string ro = ui::CommandTable::actionName(IDM_EDIT_TOGGLEREADONLY);
    g_action_group_change_action_state(G_ACTION_GROUP(w->window()), ro.c_str(), g_variant_new_boolean(TRUE));
    CHECK(w->activeBuffer()->userReadOnly && w->activeView()->execute(SCI_GETREADONLY));
    CHECK(w->commandChecked(IDM_EDIT_TOGGLEREADONLY));
    g_action_group_change_action_state(G_ACTION_GROUP(w->window()), ro.c_str(), g_variant_new_boolean(FALSE));
    CHECK(!w->activeView()->execute(SCI_GETREADONLY));
    CHECK(!w->commandChecked(IDM_EDIT_TOGGLEREADONLY));
}

void testSessionRoundTrip(GtkApplication* app, const core::LanguageCatalog& langs)
{
    Prompter p;
    const fs::path a = g_dir / "s1.py", b = g_dir / "s2.txt";
    std::string many;
    for (int i = 0; i < 200; ++i)
        many += "line " + std::to_string(i) + "\n";
    writeText(a, "def f():\n    return 1\n");
    writeText(b, many);
    const fs::path sessionFile = g_dir / "my-session.xml";
    {
        auto w = makeWindow(app, langs, p);
        w->newDocument();
        w->openFiles({a, b});
        ui::EditorView* v = w->activeView(); // s2.txt
        v->toggleBookmark(3);
        v->toggleBookmark(120);
        v->execute(SCI_SETSEL, 50, 60);
        w->cloneToOtherView();
        CHECK(w->saveSession(sessionFile));
    }
    core::Session s;
    CHECK(core::readSession(sessionFile, s));
    CHECK(s.views[0].size() == 2 && s.views[1].size() == 1);
    CHECK(s.activeView == 1 && s.activeIndex[0] == 1);
    CHECK(s.views[0][0].lang == "Python");
    CHECK(s.views[1][0].filename == b.string());
    CHECK((s.views[0][1].marks == std::vector<std::int64_t>{3, 120}));
    CHECK(s.views[0][1].startPos == 50 && s.views[0][1].endPos == 60);

    auto w = makeWindow(app, langs, p);
    w->newDocument();
    CHECK(w->loadSession(sessionFile));
    CHECK(w->documentCount() == 2); // "new 1" made way; the clone shares its buffer
    CHECK(w->tabCount(0) == 2 && w->tabCount(1) == 1);
    CHECK(w->activeViewIndex() == 1 && w->activeBuffer()->path == b);
    ui::EditorView* v = w->activeView();
    CHECK(v->execute(SCI_GETANCHOR) == 50 && v->execute(SCI_GETCURRENTPOS) == 60);
    CHECK((v->bookmarkLines() == std::vector<sptr_t>{3, 120}));
    core::Buffer* subBuf = w->activeBuffer();
    w->switchToOtherView();
    CHECK(w->activeViewIndex() == 0 && w->activeBuffer() == subBuf);
    CHECK(w->activeView() != v && w->activeView()->execute(SCI_GETDOCPOINTER) == v->execute(SCI_GETDOCPOINTER));

    // Missing files are dropped, as upstream.
    fs::remove(a);
    auto w2 = makeWindow(app, langs, p);
    w2->newDocument();
    CHECK(w2->loadSession(sessionFile));
    CHECK(w2->documentCount() == 1);
}

void testSnapshots(GtkApplication* app, const core::LanguageCatalog& langs)
{
    Prompter p;
    const fs::path f = g_dir / "snap.txt";
    writeText(f, "on disk\n");
    fs::remove(ui::MainWindow::lastSessionPath());
    fs::path untitledSnap;
    {
        auto w = makeWindow(app, langs, p);
        w->settings().rememberSession = true;
        w->settings().snapshot = true;
        w->restoreLastSession();
        w->newDocument();
        typeText(*w, "unsaved draft");
        w->openFiles({f});
        typeText(*w, "edited ");
        w->snapshotNow();
        CHECK(fs::exists(ui::MainWindow::lastSessionPath()));
        core::Session s;
        CHECK(core::readSession(ui::MainWindow::lastSessionPath(), s));
        CHECK(s.views[0].size() == 2);
        if (s.views[0].size() == 2) {
            untitledSnap = s.views[0][0].backupFilePath;
            CHECK(s.views[0][0].filename == "new 1" && core::isSnapshotPath(untitledSnap));
            CHECK(readText(untitledSnap) == "unsaved draft");
            CHECK(!s.views[0][1].backupFilePath.empty() && s.views[0][1].originalFileLastModifTimestamp != 0);
            // The undo history is kept beside each snapshot.
            CHECK(fs::exists(core::undoStatePath(untitledSnap)));
            CHECK(fs::exists(core::undoStatePath(s.views[0][1].backupFilePath)));
        }
        // Snapshot mode closes without asking about unsaved documents.
        CHECK(w->requestQuit());
        CHECK(p.asked.empty());
    }
    {
        auto w = makeWindow(app, langs, p);
        w->settings().rememberSession = true;
        w->settings().snapshot = true;
        w->restoreLastSession();
        CHECK(w->documentCount() == 2);
        CHECK(w->activeBuffer() && w->activeBuffer()->path == f);
        CHECK(w->activeView()->text() == "on disk\nedited ");
        CHECK(w->isModified(*w->activeBuffer()));
        CHECK(readText(f) == "on disk\n"); // the file itself was never touched
        // The undo history came back with the text: undoing reaches the
        // saved file, clean again, and redo the edit.
        CHECK(w->activeView()->execute(SCI_CANUNDO));
        activate(*w, IDM_EDIT_UNDO);
        CHECK(w->activeView()->text() == "on disk\n" && !w->isModified(*w->activeBuffer()));
        activate(*w, IDM_EDIT_REDO);
        CHECK(w->activeView()->text() == "on disk\nedited " && w->isModified(*w->activeBuffer()));
        // Saving makes it clean and drops its snapshot.
        const fs::path snap = w->activeBuffer()->snapshotPath;
        activate(*w, IDM_FILE_SAVE);
        CHECK(!w->isModified(*w->activeBuffer()) && readText(f) == "on disk\nedited ");
        CHECK(!fs::exists(snap) && !fs::exists(core::undoStatePath(snap)));
        // Closing the untitled one (answer No) deletes its snapshot.
        activate(*w, IDM_VIEW_TAB_PREV);
        CHECK(w->activeBuffer()->isUntitled() && w->activeView()->text() == "unsaved draft");
        activate(*w, IDM_EDIT_UNDO);
        CHECK(w->activeView()->text().empty());
        activate(*w, IDM_EDIT_REDO);
        CHECK(w->activeView()->text() == "unsaved draft");
        p.answers = {Answer::No};
        activate(*w, IDM_FILE_CLOSE);
        CHECK(!fs::exists(untitledSnap) && !fs::exists(core::undoStatePath(untitledSnap)));
    }
    // Every way a document goes away takes its undo history along.
    {
        auto w = makeWindow(app, langs, p);
        w->settings().rememberSession = true;
        w->settings().snapshot = true;
        auto dirtyDoc = [&](const char* text) {
            w->newDocument();
            typeText(*w, text);
            w->snapshotNow();
            const fs::path snap = w->activeBuffer()->snapshotPath;
            CHECK(fs::exists(snap) && fs::exists(core::undoStatePath(snap)));
            return snap;
        };
        auto gone = [](const fs::path& snap) { return !fs::exists(snap) && !fs::exists(core::undoStatePath(snap)); };
        // Close, answering No.
        fs::path s1 = dirtyDoc("one");
        p.answers = {Answer::No};
        activate(*w, IDM_FILE_CLOSE);
        CHECK(gone(s1));
        // Close All, answering No to each.
        s1 = dirtyDoc("two");
        const fs::path s2 = dirtyDoc("three");
        p.answers = {Answer::No, Answer::No};
        activate(*w, IDM_FILE_CLOSEALL);
        CHECK(gone(s1) && gone(s2));
        // Undone back to unmodified: the next tick drops both.
        s1 = dirtyDoc("x");
        activate(*w, IDM_EDIT_UNDO);
        CHECK(!w->isModified(*w->activeBuffer()));
        w->snapshotNow();
        CHECK(gone(s1));
        // A history left without its snapshot (a crash) is swept at start.
        const fs::path orphan = core::undoStatePath(core::snapshotDir() / "lost@2026-01-01_000000");
        writeText(orphan, "x");
        w->restoreLastSession();
        CHECK(!fs::exists(orphan));
        fs::remove(ui::MainWindow::lastSessionPath());
    }
    // An undo history that does not match its snapshot is ignored: the text
    // comes back, dirty, without undo.
    {
        fs::remove(ui::MainWindow::lastSessionPath());
        fs::path snap;
        {
            auto w = makeWindow(app, langs, p);
            w->settings().rememberSession = true;
            w->settings().snapshot = true;
            w->restoreLastSession();
            w->newDocument();
            typeText(*w, "draft");
            CHECK(w->requestQuit());
            snap = w->activeBuffer()->snapshotPath;
        }
        writeText(snap, "drafts"); // changed behind the history's back
        auto w = makeWindow(app, langs, p);
        w->settings().rememberSession = true;
        w->settings().snapshot = true;
        w->restoreLastSession();
        CHECK(w->activeView()->text() == "drafts" && w->isModified(*w->activeBuffer()));
        CHECK(!w->activeView()->execute(SCI_CANUNDO));
        fs::remove(ui::MainWindow::lastSessionPath());
    }
    // Without snapshots, quitting asks about unsaved documents.
    {
        auto w = makeWindow(app, langs, p);
        w->settings().snapshot = false;
        w->newDocument();
        typeText(*w, "x");
        p.asked.clear();
        p.answers = {Answer::Cancel};
        CHECK(!w->requestQuit());
        CHECK(p.asked.size() == 1);
        p.answers = {Answer::No};
        CHECK(w->requestQuit());
    }
}

void testExternalChanges(GtkApplication* app, const core::LanguageCatalog& langs)
{
    Prompter p;
    auto w = makeWindow(app, langs, p, false);
    const fs::path f = g_dir / "watched.txt";
    writeText(f, "one\n");
    w->newDocument();
    w->openFiles({f});
    core::Buffer* b = w->activeBuffer();
    w->activeView()->execute(SCI_GOTOPOS, 2);

    // Our own save must not count as an outside change.
    typeText(*w, "x");
    activate(*w, IDM_FILE_SAVE);
    w->activeView()->execute(SCI_GOTOPOS, 2);
    waitFor([] { return false; }, 300);
    CHECK(!b->externalChangePending);

    std::this_thread::sleep_for(std::chrono::milliseconds(20)); // distinct mtime
    writeText(f, "two\n");
    CHECK(waitFor([&] { return b->externalChangePending; }));
    p.answers = {Answer::Yes};
    w->checkExternalChanges();
    CHECK(p.asked.size() == 1 && w->activeView()->text() == "two\n");
    CHECK(!w->isModified(*b));
    CHECK(w->activeView()->currentPos() == 2); // caret kept across reload

    // Declining keeps the text, and the same change is not reported again.
    writeText(f, "three\n");
    CHECK(waitFor([&] { return b->externalChangePending; }));
    p.answers = {Answer::No};
    w->checkExternalChanges();
    CHECK(w->activeView()->text() == "two\n");
    CHECK(!b->externalChangePending);

    // Silent update for unmodified documents.
    w->settings().changeDetection = platform::ChangeDetection::AutoUpdate;
    p.asked.clear();
    writeText(f, "four\n");
    CHECK(waitFor([&] { return b->externalChangePending; }));
    w->checkExternalChanges();
    CHECK(p.asked.empty() && w->activeView()->text() == "four\n");

    // Deleted: "keep in editor?" No closes it.
    fs::remove(f);
    CHECK(waitFor([&] { return b->externalChangePending; }));
    p.answers = {Answer::No};
    w->checkExternalChanges();
    CHECK(w->documentCount() == 1 && w->activeBuffer()->isUntitled());
}

void testBackupOnSave(GtkApplication* app, const core::LanguageCatalog& langs)
{
    Prompter p;
    auto w = makeWindow(app, langs, p, false);
    w->settings().saveBackup = platform::SaveBackup::Simple;
    const fs::path f = g_dir / "kept.txt";
    writeText(f, "old");
    w->openFiles({f});
    typeText(*w, " new");
    activate(*w, IDM_FILE_SAVE);
    CHECK(readText(f) == "old new");
    CHECK(readText(g_dir / "kept.txt.bak") == "old");

    // Session encoding attribute: legacy code pages survive a restart.
    const fs::path latin = g_dir / "latin.txt";
    writeText(latin, "caf\xE9 cr\xE8me\n");
    w->openFiles({latin});
    const core::Session s = w->currentSession();
    const auto& files = s.views[0];
    CHECK(!files.empty() && files.back().encoding > 0);
    if (!files.empty()) {
        std::string err;
        auto lf = core::loadFile(latin, files.back().encoding, &err);
        CHECK(lf && lf->text.utf8 == "caf\xC3\xA9 cr\xC3\xA8me\n");
    }
}

// Closing like a user (delete-event): GTK destroys the window before
// ~MainWindow runs, which must cope with the destroyed widgets.
void testWindowClose(GtkApplication* app, const core::LanguageCatalog& langs)
{
    Prompter p;
    auto w = makeWindow(app, langs, p, false);
    const fs::path a = g_dir / "close-a.txt";
    writeText(a, "a\n");
    w->newDocument();
    w->openFiles({a});
    w->cloneToOtherView();
    gtk_widget_show(GTK_WIDGET(w->window()));
    pump();
    gtk_window_close(w->window());
    CHECK(waitFor([&] { return !gtk_widget_get_visible(GTK_WIDGET(w->window())); }, 3000));
    CHECK(p.asked.empty());
    w.reset();
    pump();
    platform::Settings s;
    CHECK(s.load()); // written on the way out
}

} // namespace

int main(int argc, char** argv)
{
    // Isolated config dir: must be set before GLib caches the user dirs.
    char tmpl[] = "/tmp/nppgtk-test-XXXXXX";
    const char* root = g_mkdtemp(tmpl);
    if (!root)
        return 2;
    g_setenv("XDG_CONFIG_HOME", (fs::path(root) / "config").c_str(), TRUE);
    g_setenv("NPP_GTK_DATA_DIR", NPP_TEST_DATA_DIR, TRUE);
    g_dir = fs::path(root) / "files";
    fs::create_directories(g_dir);

    gtk_init(&argc, &argv);
    core::LanguageCatalog langs;
    std::string err;
    CHECK(langs.load(fs::path(NPP_TEST_DATA_DIR) / "langs.model.xml", &err));

    GtkApplication* app = gtk_application_new("org.notepad_plus_plus.GtkPortTests", G_APPLICATION_NON_UNIQUE);
    CHECK(g_application_register(G_APPLICATION(app), nullptr, nullptr));

    std::printf("core models...\n");
    testCoreModels();
    std::printf("config.xml import...\n");
    testConfigImport();
    std::printf("views and clones...\n");
    testViewsAndClones(app, langs);
    std::printf("session round trip...\n");
    testSessionRoundTrip(app, langs);
    std::printf("snapshots...\n");
    testSnapshots(app, langs);
    std::printf("outside changes...\n");
    testExternalChanges(app, langs);
    std::printf("backup on save, encodings...\n");
    testBackupOnSave(app, langs);
    std::printf("window close...\n");
    testWindowClose(app, langs);

    g_object_unref(app);
    std::error_code ec;
    fs::remove_all(root, ec);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
