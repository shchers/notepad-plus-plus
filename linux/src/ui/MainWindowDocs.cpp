// MainWindow: tabs and buffers, file I/O, sessions, snapshots and
// outside-change detection — the document side of upstream NppIO.cpp and
// Notepad_plus.cpp (loadSession, getCurrentOpenedFiles, docGotoAnotherEditView,
// checkModifiedDocument).

#include "ui/MainWindow.h"

#include "core/FileManager.h"
#include "platform/Paths.h"

#include <windows.h> // plugin SDK: NPPN_* codes
#include "Notepad_plus_msgs.h"

#include <algorithm>
#include <cstring>
#include <set>
#include <system_error>

namespace fs = std::filesystem;

namespace npp::ui {

namespace {

struct TabCtx {
    MainWindow* win;
    GtkWidget* page;
};

fs::path normalisedPath(const fs::path& f)
{
    std::error_code ec;
    fs::path abs = fs::absolute(f, ec);
    if (ec)
        abs = f;
    fs::path p = fs::weakly_canonical(abs, ec);
    return ec ? abs.lexically_normal() : p;
}

} // namespace

// ============================================================================
// Tabs
// ============================================================================

MainWindow::Tab* MainWindow::addTab(std::shared_ptr<core::Buffer> buf, int viewIdx, const Tab* cloneOf)
{
    auto owned = std::make_unique<Tab>();
    Tab* t = owned.get();
    t->view = std::make_unique<EditorView>();
    t->buf = std::move(buf);
    t->viewIdx = viewIdx;
    EditorView& v = *t->view;
    if (cloneOf) // share the Scintilla document, like upstream SCI_SETDOCPOINTER
        v.execute(SCI_SETDOCPOINTER, 0, cloneOf->view->execute(SCI_GETDOCPOINTER));
    applyBufferLanguage(v, *t->buf);
    applyViewSettings(v);
    if (cloneOf)
        v.restorePosition(cloneOf->view->position());

    core::Buffer* b = t->buf.get();
    v.onUpdateUI = [this, t] {
        if (!closing_ && t == activeTab()) {
            refreshStatus();
            if (docMap_ && dock_->isShown("documentMap"))
                docMap_->sync();
        }
    };
    // Every view of a shared document gets the save point notification.
    v.onSavePoint = [this, t](bool) {
        if (closing_)
            return;
        refreshTab(*t);
        if (t == activeTab())
            refreshTitle();
    };
    v.onTextModified = [this, b, t] {
        b->snapshotStale = true;
        // Preferences > New Document: an untitled document is named after its first line.
        if (settings_.firstLineAsTabName && b->isUntitled() && !b->untitledRenamed && !closing_ && t->view->currentPos() <= t->view->sci().lineEnd(0) + 1)
            nameAfterFirstLine(*b, *t->view);
        // Upstream re-parses the Function List after edits.
        if (funcList_ && !closing_ && activeBuffer() == b && dock_->isShown("functionList"))
            funcList_->scheduleReload();
    };
    v.onFocusIn = [this, t] {
        if (!closing_ && activeView_ != t->viewIdx) {
            activeView_ = t->viewIdx;
            refreshAll();
        }
    };
    v.onUriDropped = [this](std::string_view uris) { openUriList(uris); };
    v.onMacroRecord = [this](int msg, uptr_t w, sptr_t l) { recordScintilla(msg, w, l); };
    v.onScintillaNotify = [this, t](const SCNotification& n) { forwardScintillaNotification(*t, n); };
    v.setOptions(&editorOpts_);
    v.largeFile = t->buf->largeFile;
    v.otherView = [this, t]() -> EditorView* {
        Tab* o = gtk_widget_get_visible(notebooks_[1 - t->viewIdx]) ? currentTabOf(1 - t->viewIdx) : nullptr;
        return o ? o->view.get() : nullptr;
    };
    v.onLinkClicked = [this](const std::string& url) { openUri(url.rfind("www.", 0) == 0 ? "http://" + url : url); };
    if (modEventFlags_)
        v.setModEventMask(modEventFlags_);
    if (recording_)
        v.execute(SCI_STARTRECORD);

    tabs_.push_back(std::move(owned));
    gtk_widget_show(v.widget());
    attachLabel(*t);
    return t;
}

void MainWindow::attachLabel(Tab& t)
{
    GtkWidget* page = t.view->widget();
    // Tab label: [name][x]; middle click closes, right click opens the tab menu.
    GtkWidget* ebox = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(ebox), FALSE);
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    t.labelText = gtk_label_new(nullptr);
    GtkWidget* close = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
    gtk_button_set_relief(GTK_BUTTON(close), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click(close, FALSE);
    gtk_widget_set_tooltip_text(close, "Close");
    gtk_box_pack_start(GTK_BOX(box), t.labelText, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), close, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(ebox), box);
    // The label's and the button's input windows pass the wheel on to the
    // notebook (MainWindow::onTabStripScroll).
    for (GtkWidget* w : {ebox, close})
        gtk_widget_add_events(w, GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);
    gtk_widget_show_all(ebox);
    t.label = ebox;
    t.closeButton = close;

    auto freeCtx = +[](gpointer p, GClosure*) { delete static_cast<TabCtx*>(p); };
    g_signal_connect_data(close, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) {
                              auto* c = static_cast<TabCtx*>(p);
                              if (Tab* tab = c->win->tabForPage(c->page))
                                  c->win->closeTab(*tab);
                          }),
                          new TabCtx{this, page}, freeCtx, GConnectFlags(0));
    g_signal_connect_data(ebox, "button-press-event", G_CALLBACK(+[](GtkWidget*, GdkEventButton* ev, gpointer p) -> gboolean {
                              auto* c = static_cast<TabCtx*>(p);
                              Tab* tab = c->win->tabForPage(c->page);
                              // Preferences > Tab Bar: "Double click to close document".
                              if (tab && ev->type == GDK_2BUTTON_PRESS && ev->button == 1 && c->win->settings_.tabDoubleClickClose) {
                                  c->win->closeTab(*tab);
                                  return TRUE;
                              }
                              if (!tab || ev->type != GDK_BUTTON_PRESS)
                                  return FALSE;
                              if (ev->button == 2) {
                                  c->win->closeTab(*tab);
                                  return TRUE;
                              }
                              if (ev->button == 3) {
                                  c->win->showTabMenu(*tab, reinterpret_cast<GdkEvent*>(ev));
                                  return TRUE;
                              }
                              // Left click: this view becomes the active one; GtkNotebook switches the page.
                              if (c->win->activeView_ != tab->viewIdx) {
                                  c->win->activeView_ = tab->viewIdx;
                                  if (tab == c->win->currentTabOf(tab->viewIdx))
                                      c->win->activateTab(*tab);
                              }
                              return FALSE;
                          }),
                          new TabCtx{this, page}, freeCtx, GConnectFlags(0));

    GtkNotebook* nb = GTK_NOTEBOOK(notebooks_[t.viewIdx]);
    gtk_notebook_append_page(nb, page, ebox);
    gtk_notebook_set_tab_reorderable(nb, page, !settings_.tabLock);
    gtk_widget_show(notebooks_[t.viewIdx]);
    refreshTab(t);
}

void MainWindow::removeTab(Tab& t)
{
    std::shared_ptr<core::Buffer> buf = t.buf; // alive (and its ID valid) until the end
    if (!closing_)
        notifyPlugins(NPPN_FILEBEFORECLOSE, buf.get());
    GtkNotebook* nb = GTK_NOTEBOOK(notebooks_[t.viewIdx]);
    const int page = gtk_notebook_page_num(nb, t.view->widget());
    auto it = std::find_if(tabs_.begin(), tabs_.end(), [&](const auto& p) { return p.get() == &t; });
    std::unique_ptr<Tab> owned = std::move(*it); // keep alive while GTK detaches it
    tabs_.erase(it);
    if (panelSource_ == owned->view.get()) { // the Document Map must let go of it
        if (docMap_)
            docMap_->attach(nullptr);
        panelSource_ = nullptr;
    }
    if (page >= 0)
        gtk_notebook_remove_page(nb, page);
    owned.reset();
    if (tabsOf(*buf).empty())
        releaseBuffer(*buf);
    if (!closing_) {
        updateViewVisibility();
        notifyPlugins(NPPN_FILECLOSED, buf.get());
    }
}

void MainWindow::releaseBuffer(core::Buffer& b)
{
    monitors_.erase(&b);
    if (activatedBuffer_ == &b)
        activatedBuffer_ = nullptr;
    mru_.erase(std::remove(mru_.begin(), mru_.end(), &b), mru_.end());
    if (closing_)
        return; // exit: snapshots stay for the next start
    if (!b.isUntitled()) {
        recent_.add(b.path.string()); // upstream: closed files enter the history
        refreshRecentMenu();
    }
    core::deleteSnapshot(b);
}

MainWindow::Tab* MainWindow::currentTabOf(int viewIdx) const
{
    if (!notebooks_[viewIdx]) // still constructing
        return nullptr;
    GtkNotebook* nb = GTK_NOTEBOOK(notebooks_[viewIdx]);
    const int page = gtk_notebook_get_current_page(nb);
    return page < 0 ? nullptr : tabForPage(gtk_notebook_get_nth_page(nb, page));
}

MainWindow::Tab* MainWindow::activeTab() const
{
    if (Tab* t = currentTabOf(activeView_))
        return t;
    return currentTabOf(1 - activeView_);
}

MainWindow::Tab* MainWindow::tabForPage(GtkWidget* page) const
{
    for (const auto& t : tabs_)
        if (t->view->widget() == page)
            return t.get();
    return nullptr;
}

MainWindow::Tab* MainWindow::tabOf(const core::Buffer& b, int viewIdx) const
{
    for (const auto& t : tabs_)
        if (t->buf.get() == &b && t->viewIdx == viewIdx)
            return t.get();
    return nullptr;
}

std::vector<MainWindow::Tab*> MainWindow::tabsOf(const core::Buffer& b) const
{
    std::vector<Tab*> out;
    for (const auto& t : tabs_)
        if (t->buf.get() == &b)
            out.push_back(t.get());
    return out;
}

std::vector<MainWindow::Tab*> MainWindow::tabsInOrder(int viewIdx) const
{
    std::vector<Tab*> out;
    GtkNotebook* nb = GTK_NOTEBOOK(notebooks_[viewIdx]);
    for (int i = 0, n = gtk_notebook_get_n_pages(nb); i < n; ++i)
        if (Tab* t = tabForPage(gtk_notebook_get_nth_page(nb, i)))
            out.push_back(t);
    return out;
}

std::vector<core::Buffer*> MainWindow::buffers() const
{
    std::vector<core::Buffer*> out;
    for (const auto& t : tabs_)
        if (std::find(out.begin(), out.end(), t->buf.get()) == out.end())
            out.push_back(t->buf.get());
    return out;
}

std::size_t MainWindow::documentCount() const
{
    return buffers().size();
}

std::size_t MainWindow::tabCount(int view) const
{
    return static_cast<std::size_t>(gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebooks_[view])));
}

EditorView* MainWindow::activeView() const
{
    Tab* t = activeTab();
    return t ? t->view.get() : nullptr;
}

core::Buffer* MainWindow::activeBuffer() const
{
    Tab* t = activeTab();
    return t ? t->buf.get() : nullptr;
}

bool MainWindow::isModified(const core::Buffer& b) const
{
    if (b.forcedDirty)
        return true;
    for (const auto& t : tabs_)
        if (t->buf.get() == &b)
            return t->view->isModified(); // document state: any view answers
    return false;
}

core::Buffer* MainWindow::findByPath(const fs::path& p) const
{
    for (const auto& t : tabs_)
        if (!t->buf->isUntitled() && t->buf->path == p)
            return t->buf.get();
    return nullptr;
}

void MainWindow::activateTab(Tab& t, bool focus)
{
    activeView_ = t.viewIdx;
    GtkNotebook* nb = GTK_NOTEBOOK(notebooks_[t.viewIdx]);
    gtk_notebook_set_current_page(nb, gtk_notebook_page_num(nb, t.view->widget()));
    if (focus)
        gtk_widget_grab_focus(t.view->widget());
    refreshAll();
}

void MainWindow::updateViewVisibility()
{
    // Upstream hides a view that has no documents left.
    for (int vi : {kMainView, kSubView})
        gtk_widget_set_visible(notebooks_[vi], tabCount(vi) > 0);
    if (tabCount(activeView_) == 0 && tabCount(1 - activeView_) > 0) {
        activeView_ = 1 - activeView_;
        if (Tab* t = currentTabOf(activeView_))
            activateTab(*t);
    }
}

std::string MainWindow::nextUntitledName() const
{
    for (int n = 1;; ++n) {
        std::string name = "new " + std::to_string(n);
        if (std::none_of(tabs_.begin(), tabs_.end(), [&](const auto& t) { return t->buf->isUntitled() && t->buf->untitledName == name; }))
            return name;
    }
}

void MainWindow::newDocument()
{
    auto buf = std::make_shared<core::Buffer>();
    buf->untitledName = nextUntitledName();
    // Preferences > New Document: language, encoding and line ending.
    const core::Language* lang = settings_.newDocLanguage.empty() ? nullptr : langs_.byName(settings_.newDocLanguage);
    buf->language = lang ? lang : &langs_.plainText();
    static const platform::TextEncoding kEnc[] = {{"UTF-8", platform::Bom::None}, {"UTF-8", platform::Bom::Utf8},
                                                  {"UTF-16BE", platform::Bom::Utf16BE}, {"UTF-16LE", platform::Bom::Utf16LE}};
    buf->encoding = kEnc[std::clamp(settings_.newDocEncoding, 0, 3)];
    Tab* t = addTab(std::move(buf), activeView_);
    t->view->setEolMode(std::clamp(settings_.newDocEol, 0, 2), false);
    activateTab(*t);
}

// ============================================================================
// Views
// ============================================================================

void MainWindow::moveToOtherView()
{
    Tab* t = activeTab();
    if (!t)
        return;
    const int other = 1 - t->viewIdx;
    if (Tab* there = tabOf(*t->buf, other)) {
        // Already cloned there: moving just drops this side (no question, the buffer stays open).
        removeTab(*t);
        activateTab(*there);
        return;
    }
    if (tabs_.size() == 1)
        return; // the only document: nothing to split
    const ViewPosition pos = t->view->position();
    GtkNotebook* from = GTK_NOTEBOOK(notebooks_[t->viewIdx]);
    gtk_notebook_remove_page(from, gtk_notebook_page_num(from, t->view->widget())); // EditorView keeps the widget alive
    t->viewIdx = other;
    attachLabel(*t);
    t->view->restorePosition(pos);
    updateViewVisibility();
    activateTab(*t);
}

void MainWindow::cloneToOtherView()
{
    Tab* t = activeTab();
    if (!t)
        return;
    const int other = 1 - t->viewIdx;
    Tab* there = tabOf(*t->buf, other);
    if (!there)
        there = addTab(t->buf, other, t);
    activateTab(*there);
}

void MainWindow::switchToOtherView()
{
    if (Tab* t = currentTabOf(1 - activeView_))
        activateTab(*t);
}

// ============================================================================
// Loading and saving
// ============================================================================

void MainWindow::applyLoaded(Tab& t, const fs::path& p, core::LoadedFile&& lf)
{
    core::Buffer& b = *t.buf;
    b.path = p;
    b.encoding = lf.text.encoding;
    b.diskStamp = lf.stamp;
    b.forcedDirty = false;
    b.externalChangePending = false;
    b.snapshotStale = false;
    EditorView& v = *t.view;
    v.execute(SCI_SETREADONLY, 0);
    v.setText(lf.text.utf8); // document-level: clones see it too
    v.setEolMode(v.detectEolMode(SC_EOL_LF), false);
    if (b.userReadOnly)
        v.execute(SCI_SETREADONLY, 1);
    // Preferences > Performance: a large file opens without syntax highlighting.
    b.largeFile = settings_.largeFileRestrict && lf.stamp.size >= static_cast<std::int64_t>(settings_.largeFileMB) * 1024 * 1024;
    for (Tab* x : tabsOf(b))
        x->view->largeFile = b.largeFile;
    if (b.largeFile) {
        setLanguage(b, langs_.plainText());
        applyViewSettings(v);
    }
    if (!p.empty()) {
        if (!b.largeFile)
            detectLanguage(b, p);
        lastDir_ = p.parent_path();
        recent_.remove(p.string());
        refreshRecentMenu();
        watch(b);
    }
}

void MainWindow::openFolder(const fs::path& dir)
{
    // Upstream: a dropped folder goes to Folder as Workspace, or with
    // Preferences > Default Directory's option all its files are opened.
    if (!settings_.dropFolderOpensFiles) {
        folder_->addRoot(dir);
        dock_->show("folderAsWorkspace", false);
        return;
    }
    std::vector<fs::path> files;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec)
            break;
        const std::string name = it->path().filename().string();
        if (!name.empty() && name[0] == '.') {
            if (it->is_directory(ec))
                it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(ec))
            files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    openFiles(files);
}

int MainWindow::openFiles(const std::vector<fs::path>& files, const OpenOptions& opts)
{
    int opened = 0;
    auto hasExt = [](const fs::path& p, std::string ext) {
        if (!ext.empty() && ext[0] == '.')
            ext.erase(0, 1);
        return !ext.empty() && g_ascii_strcasecmp(p.extension().c_str(), ("." + ext).c_str()) == 0;
    };
    for (const fs::path& f : files) {
        const fs::path p = normalisedPath(f);
        std::error_code dirEc;
        if (fs::is_directory(p, dirEc)) {
            openFolder(p);
            continue;
        }
        // Preferences > MISC.: session and workspace files by extension.
        if (hasExt(p, settings_.sessionExt)) {
            std::string err;
            if (!loadSession(p, &err))
                showError("Cannot load session", err);
            continue;
        }
        if (hasExt(p, settings_.workspaceExt) && projects_[0]) {
            projects_[0]->openWorkspace(p);
            dock_->show("project1", false);
            continue;
        }
        Tab* t = nullptr;
        if (core::Buffer* existing = findByPath(p)) {
            t = tabOf(*existing, activeView_);
            if (!t)
                t = tabsOf(*existing).front();
        } else {
            std::string err;
            notifyPlugins(NPPN_FILEBEFORELOAD);
            auto lf = core::loadFile(p, settings_.autodetectEncoding ? -1 : 65001, &err);
            if (!lf) {
                notifyPlugins(NPPN_FILELOADFAILED);
                showError("Cannot open file \"" + p.filename().string() + "\"", err);
                continue;
            }
            // Replace a lone pristine "new 1", as upstream does.
            if (tabs_.size() == 1 && tabs_[0]->buf->isUntitled() && !isModified(*tabs_[0]->buf) && tabs_[0]->view->length() == 0)
                t = tabs_[0].get();
            else
                t = addTab(std::make_shared<core::Buffer>(), activeView_);
            // A path that does not exist yet opens as an empty document that
            // will be created on save.
            notifyPlugins(NPPN_FILEBEFOREOPEN, t->buf.get());
            applyLoaded(*t, p, std::move(*lf));
            refreshTab(*t);
            notifyPlugins(NPPN_FILEOPENED, t->buf.get());
            ++opened;
        }
        activateTab(*t);
        if (opts.language)
            setLanguage(*t->buf, *opts.language);
        if (opts.line || opts.column)
            t->view->gotoLine(opts.line.value_or(1) - 1, opts.column.value_or(1) - 1);
        if (opts.readOnly)
            setReadOnly(*t->buf, true);
    }
    refreshAll();
    return opened;
}

bool MainWindow::reloadBuffer(core::Buffer& b, std::string* error, int codepage)
{
    const std::vector<Tab*> tabs = tabsOf(b);
    if (tabs.empty() || b.isUntitled())
        return false;
    auto lf = core::loadFile(b.path, codepage, error);
    if (!lf)
        return false;
    std::vector<ViewPosition> positions;
    for (Tab* t : tabs)
        positions.push_back(t->view->position());
    const core::Language* lang = b.language;
    applyLoaded(*tabs.front(), b.path, std::move(*lf));
    if (lang) // keep a language the user picked
        setLanguage(b, *lang);
    for (std::size_t i = 0; i < tabs.size(); ++i)
        tabs[i]->view->restorePosition(positions[i]);
    core::deleteSnapshot(b); // the document matches the disk again
    refreshBufferTabs(b);
    refreshAll();
    return true;
}

void MainWindow::applyBufferLanguage(EditorView& v, const core::Buffer& b)
{
    if (b.userLanguage) {
        const long id = static_cast<long>(b.userLanguage - udls_.data()) + 1;
        v.applyUserLanguage(*b.userLanguage, styles_, id);
    } else if (b.language) {
        v.applyLanguage(*b.language, langs_, styles_);
    }
    languageExtras(v, b);
}

void MainWindow::languageExtras(EditorView& v, const core::Buffer& b)
{
    v.setApi(apiFor(b));
    // Preferences > Language: backslash as escape character for SQL.
    if (b.language && (b.language->name == "sql" || b.language->name == "mssql")) {
        v.execute(SCI_SETPROPERTY, reinterpret_cast<uptr_t>("sql.backslash.escapes"),
                  reinterpret_cast<sptr_t>(settings_.sqlBackslashEscape ? "1" : "0"));
        v.execute(SCI_COLOURISE, 0, -1);
    }
}

void MainWindow::nameAfterFirstLine(core::Buffer& b, EditorView& v)
{
    // Upstream Buffer::normalizeTabName: trimmed, without the characters a
    // file name cannot have, at most 63 characters.
    std::string line = v.sci().lineText(0);
    std::string name;
    for (char c : line)
        if (static_cast<unsigned char>(c) >= 0x20 && !std::strchr("\\/:*?\"<>|", c))
            name += c;
    const auto first = name.find_first_not_of(" \t");
    name = first == std::string::npos ? std::string() : name.substr(first, name.find_last_not_of(" \t") - first + 1);
    if (g_utf8_strlen(name.c_str(), -1) > 63) {
        gchar* cut = g_utf8_substring(name.c_str(), 0, 63);
        name = cut;
        g_free(cut);
    }
    if (name.empty() || name == b.untitledName)
        return;
    for (core::Buffer* o : buffers())
        if (o != &b && o->name() == name)
            return;
    notifyPlugins(NPPN_FILEBEFORERENAME, &b);
    b.untitledName = name;
    refreshBufferTabs(b);
    refreshTitle();
    notifyPlugins(NPPN_FILERENAMED, &b);
}

void MainWindow::setUserLanguage(core::Buffer& b, const core::UserLanguage& udl)
{
    b.language = &langs_.plainText();
    b.userLanguage = &udl;
    for (Tab* t : tabsOf(b)) {
        applyBufferLanguage(*t->view, b);
        applyViewSettings(*t->view);
    }
    if (activeBuffer() == &b) {
        syncActionStates();
        refreshStatus();
        notifyPlugins(NPPN_LANGCHANGED, &b);
    }
}

void MainWindow::reapplyLanguage(core::Buffer& b)
{
    if (b.userLanguage)
        setUserLanguage(b, *b.userLanguage);
    else if (b.language)
        setLanguage(b, *b.language);
}

void MainWindow::detectLanguage(core::Buffer& b, const fs::path& p)
{
    const std::string ext = p.extension().string();
    // A UDL first; with light and dark variants for the same extension, the
    // one that suits the theme (upstream getUserDefinedLangNameFromExt).
    const core::UserLanguage* found = nullptr;
    for (const core::UserLanguage& u : udls_)
        if (u.matchesExtension(ext) && (!found || (u.darkModeTheme == darkTheme_ && found->darkModeTheme != darkTheme_)))
            found = &u;
    if (found) {
        setUserLanguage(b, *found);
        return;
    }
    for (const core::StyleCatalog::LexerInfo& l : styles_.lexers()) {
        if (l.ext.empty() || ext.size() < 2)
            continue;
        const std::string list = " " + l.ext + " ";
        if (list.find(" " + ext.substr(1) + " ") != std::string::npos)
            if (const core::Language* lang = langs_.byName(l.name)) {
                setLanguage(b, *lang);
                return;
            }
    }
    setLanguage(b, langs_.forFile(p));
}

void MainWindow::setLanguage(core::Buffer& b, const core::Language& lang)
{
    b.language = &lang;
    b.userLanguage = nullptr;
    for (Tab* t : tabsOf(b)) {
        t->view->applyLanguage(lang, langs_, styles_);
        languageExtras(*t->view, b);
        applyViewSettings(*t->view);
    }
    if (activeBuffer() == &b) {
        syncActionStates();
        refreshStatus();
        notifyPlugins(NPPN_LANGCHANGED, &b);
    }
}

void MainWindow::setReadOnly(core::Buffer& b, bool ro)
{
    b.userReadOnly = ro;
    for (Tab* t : tabsOf(b))
        t->view->execute(SCI_SETREADONLY, ro); // document-level, set on each for clarity
    refreshBufferTabs(b);
    if (activeBuffer() == &b)
        syncActionStates();
    // Upstream: hwndFrom is the buffer, idFrom its DOCSTATUS_* flags.
    notifyPlugins(NPPN_READONLYCHANGED, &b, (ro ? DOCSTATUS_READONLY : 0) | (isModified(b) ? DOCSTATUS_BUFFERDIRTY : 0));
}

void MainWindow::markForcedDirty(core::Buffer& b, bool dirty)
{
    b.forcedDirty = dirty;
    b.snapshotStale = true;
    refreshBufferTabs(b);
    refreshTitle();
}

bool MainWindow::save(core::Buffer& b, bool saveAs, const fs::path& to)
{
    const std::vector<Tab*> tabs = tabsOf(b);
    if (tabs.empty())
        return false;
    fs::path target = b.path;
    if (!to.empty()) {
        target = normalisedPath(to);
        if (core::Buffer* other = findByPath(target); other && other != &b)
            return false;
    } else if (b.isUntitled() || saveAs) {
        GtkFileChooserNative* dlg = gtk_file_chooser_native_new("Save As", GTK_WINDOW(window_), GTK_FILE_CHOOSER_ACTION_SAVE, "_Save", "_Cancel");
        gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
        if (!b.isUntitled()) {
            gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dlg), b.path.c_str());
        } else {
            if (const fs::path dir = dialogFolder(); !dir.empty())
                gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), dir.c_str());
            gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), b.untitledName.c_str());
        }
        const gint res = gtk_native_dialog_run(GTK_NATIVE_DIALOG(dlg));
        target.clear();
        if (res == GTK_RESPONSE_ACCEPT) {
            if (gchar* fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg))) {
                target = fn;
                g_free(fn);
            }
        }
        g_object_unref(dlg);
        if (target.empty())
            return false;
        target = normalisedPath(target);
        if (core::Buffer* other = findByPath(target); other && other != &b) {
            showError("Cannot save \"" + b.name() + "\"", target.string() + " is open in another tab. Close it first.");
            return false;
        }
    }

    // A plugin may still change the text (trim trailing blanks...).
    notifyPlugins(NPPN_FILEBEFORESAVE, &b);
    std::string err;
    if (!core::backupBeforeSave(target, settings_.saveBackup, settings_.saveBackupDir, &err)) {
        if (ask("The previous version of the file could not be saved into the backup directory.",
                err + "\n\nDo you want to save the current file anyway?", false) != Answer::Yes)
            return false;
    }
    bool unrepresentable = false;
    auto stamp = core::saveFile(target, tabs.front()->view->text(), b.encoding, &err, &unrepresentable);
    if (!stamp) {
        if (unrepresentable)
            showError("Cannot save \"" + b.name() + "\" as " + b.encoding.displayName(),
                      "The text contains characters this encoding cannot represent (" + err +
                          "). Choose Encoding > UTF-8 and save again.");
        else
            showError("Cannot save \"" + target.filename().string() + "\"", err);
        return false;
    }

    const bool wasUntitled = b.isUntitled();
    const bool nameChanged = b.path != target;
    b.path = target;
    b.diskStamp = *stamp;
    b.forcedDirty = false;
    b.externalChangePending = false;
    lastDir_ = target.parent_path();
    tabs.front()->view->execute(SCI_SETSAVEPOINT); // document-level
    core::deleteSnapshot(b);
    if (nameChanged) {
        watch(b);
        recent_.remove(target.string());
        refreshRecentMenu();
    }
    // Upstream re-detects the language when an untitled/plain doc gets a name.
    if ((wasUntitled || nameChanged) && !b.userLanguage && (!b.language || b.language->isPlainText()))
        detectLanguage(b, target);
    refreshBufferTabs(b);
    refreshAll();
    notifyPlugins(NPPN_FILESAVED, &b);
    return true;
}

bool MainWindow::confirmSave(core::Buffer& b)
{
    if (!isModified(b))
        return true;
    if (Tab* t = tabOf(b, activeView_) ? tabOf(b, activeView_) : tabsOf(b).front())
        activateTab(*t);
    switch (ask("Save file \"" + b.fullName() + "\"?", "", true)) {
    case Answer::Yes: return save(b, false);
    case Answer::No: return true;
    case Answer::Cancel: break;
    }
    return false;
}

bool MainWindow::closeTab(Tab& t, bool keepOne)
{
    // Closing one side of a clone keeps the buffer open: nothing to ask.
    if (tabsOf(*t.buf).size() == 1 && !confirmSave(*t.buf))
        return false;
    removeTab(t);
    // Preferences > Tab Bar: "Exit on close the last tab".
    if (tabs_.empty() && keepOne && !closing_ && settings_.tabExitOnLast) {
        g_idle_add(+[](gpointer w) -> gboolean {
            gtk_window_close(GTK_WINDOW(w));
            return G_SOURCE_REMOVE;
        }, window_);
        newDocument(); // until the window goes (or if leaving is cancelled)
        return true;
    }
    if (tabs_.empty() && keepOne && !closing_)
        newDocument(); // upstream always keeps one document open
    refreshAll();
    return true;
}

void MainWindow::closeTabsWhere(const std::function<bool(Tab& t, int index, int activeIndex)>& pick)
{
    Tab* active = activeTab();
    if (!active)
        return;
    const std::vector<Tab*> tabs = tabsInOrder(active->viewIdx);
    const int activeIndex = static_cast<int>(std::find(tabs.begin(), tabs.end(), active) - tabs.begin());
    for (int i = static_cast<int>(tabs.size()) - 1; i >= 0; --i)
        if (tabs[i] != active && pick(*tabs[i], i, activeIndex) && !closeTab(*tabs[i], false))
            break;
    if (tabs_.empty())
        newDocument();
    refreshAll();
}

bool MainWindow::closeAll()
{
    for (Tab* t : tabsInOrder(kSubView))
        if (!closeTab(*t, false))
            return false;
    for (Tab* t : tabsInOrder(kMainView))
        if (!closeTab(*t, false))
            return false;
    if (tabs_.empty())
        newDocument();
    return true;
}

// ============================================================================
// Outside changes (upstream Notepad_plus::checkModifiedDocument)
// ============================================================================

void MainWindow::watch(core::Buffer& b)
{
    monitors_.erase(&b);
    if (b.isUntitled() || settings_.changeDetection == platform::ChangeDetection::Off)
        return;
    monitors_[&b] = std::make_unique<platform::FileMonitor>(b.path, [this, &b] { onFileEvent(b); });
}

void MainWindow::rewatchAll()
{
    for (core::Buffer* b : buffers())
        watch(*b);
}

void MainWindow::onFileEvent(core::Buffer& b)
{
    if (closing_ || platform::statFile(b.path).sameAs(b.diskStamp))
        return; // includes our own saves
    b.externalChangePending = true;
    scheduleChangeCheck();
}

void MainWindow::scheduleChangeCheck()
{
    // Like upstream, ask only while the window is active; otherwise the
    // "is-active" handler runs the check when the user comes back.
    if (changeCheckIdle_ || !gtk_window_is_active(GTK_WINDOW(window_)))
        return;
    changeCheckIdle_ = g_idle_add(+[](gpointer p) -> gboolean {
        auto* w = static_cast<MainWindow*>(p);
        w->changeCheckIdle_ = 0;
        w->checkExternalChanges();
        return G_SOURCE_REMOVE;
    }, this);
}

void MainWindow::checkExternalChanges()
{
    if (checkingChanges_ || closing_)
        return; // dialogs below run a nested main loop
    checkingChanges_ = true;
    std::vector<std::shared_ptr<core::Buffer>> pending;
    for (const auto& t : tabs_)
        if (t->buf->externalChangePending && std::find(pending.begin(), pending.end(), t->buf) == pending.end())
            pending.push_back(t->buf);

    for (const std::shared_ptr<core::Buffer>& bp : pending) {
        core::Buffer& b = *bp;
        b.externalChangePending = false;
        if (tabsOf(b).empty() || b.isUntitled())
            continue; // closed meanwhile
        const platform::FileStamp st = platform::statFile(b.path);
        if (st.sameAs(b.diskStamp))
            continue;
        activateTab(*(tabOf(b, activeView_) ? tabOf(b, activeView_) : tabsOf(b).front()));
        if (!st.exists) {
            if (ask(b.path.string() + "\n\nThis file doesn't exist anymore.", "Keep this file in the editor?", false) == Answer::Yes) {
                b.diskStamp = st;
                markForcedDirty(b, true);
            } else {
                for (Tab* t : tabsOf(b))
                    removeTab(*t);
                if (tabs_.empty())
                    newDocument();
            }
            continue;
        }
        const bool modified = isModified(b);
        bool reload = settings_.changeDetection == platform::ChangeDetection::AutoUpdate && !modified;
        if (!reload)
            reload = ask(b.path.string() + "\n\nThis file has been modified by another program.",
                         modified ? "Do you want to reload it and lose the changes made in Notepad++?" : "Do you want to reload it?",
                         false) == Answer::Yes;
        std::string err;
        if (!reload)
            b.diskStamp = st; // ask again only on the next change
        else if (!reloadBuffer(b, &err))
            showError("Cannot reload \"" + b.name() + "\"", err);
        else if (settings_.scrollToEndAfterUpdate) // Preferences > MISC., like tail -f
            for (Tab* t : tabsOf(b))
                t->view->execute(SCI_DOCUMENTEND);
    }
    checkingChanges_ = false;
    refreshAll();
}

// ============================================================================
// Sessions and snapshots
// ============================================================================

fs::path MainWindow::lastSessionPath()
{
    return platform::userConfigDir() / "session.xml";
}

core::Session MainWindow::currentSession() const
{
    // Upstream Notepad_plus::getCurrentOpenedFiles.
    core::Session s;
    s.activeView = activeView_;
    for (int vi : {kMainView, kSubView}) {
        const Tab* current = currentTabOf(vi);
        for (const Tab* t : tabsInOrder(vi)) {
            const core::Buffer& b = *t->buf;
            std::error_code ec;
            const bool withSnapshot = snapshotsOn() && isModified(b) && core::isSnapshotPath(b.snapshotPath) &&
                                      fs::is_regular_file(b.snapshotPath, ec);
            if (b.isUntitled() && !withSnapshot)
                continue; // nothing to restore it from
            core::SessionFile f;
            f.filename = b.fullName();
            f.lang = b.userLanguage ? b.userLanguage->name : b.language ? b.language->shortName : langs_.plainText().shortName;
            f.encoding = b.encoding.bom == platform::Bom::None ? platform::codepageForCharset(b.encoding.charset) : -1;
            f.userReadOnly = b.userReadOnly;
            if (withSnapshot)
                f.backupFilePath = b.snapshotPath.string();
            if (!b.isUntitled() && b.diskStamp.exists)
                f.originalFileLastModifTimestamp = platform::toFileTime(b.diskStamp.mtimeUs);
            const ViewPosition p = t->view->position();
            f.firstVisibleLine = p.firstVisibleLine;
            f.xOffset = p.xOffset;
            f.scrollWidth = p.scrollWidth;
            f.startPos = p.startPos;
            f.endPos = p.endPos;
            f.selMode = p.selMode;
            for (const sptr_t l : t->view->bookmarkLines())
                f.marks.push_back(l);
            for (const sptr_t l : t->view->contractedFoldLines())
                f.folds.push_back(l);
            if (t == current)
                s.activeIndex[vi] = s.views[vi].size();
            s.views[vi].push_back(std::move(f));
        }
    }
    if (s.views[s.activeView].empty())
        s.activeView = 1 - s.activeView;
    if (s.views[s.activeView].empty())
        s.activeView = kMainView;
    // Folder as Workspace goes into the session while it is open, as upstream.
    if (folder_ && dock_ && dock_->isShown("folderAsWorkspace")) {
        s.includeFileBrowser = true;
        folder_->saveTo(s);
    }
    return s;
}

bool MainWindow::saveSession(const fs::path& file, std::string* error)
{
    return core::writeSession(file, currentSession(), error);
}

void MainWindow::restoreLastSession()
{
    std::error_code ec;
    if (sessionOn() && fs::is_regular_file(lastSessionPath(), ec)) {
        std::string err;
        if (!loadSession(lastSessionPath(), &err))
            g_warning("Cannot restore the last session: %s", err.c_str());
    }
    restartSnapshotTimer();
}

MainWindow::Tab* MainWindow::openSessionFile(const core::SessionFile& f, int viewIdx)
{
    const bool untitled = !fs::path(f.filename).is_absolute();
    const fs::path backup = f.backupFilePath;
    std::error_code ec;
    const bool hasBackup = core::isSnapshotPath(backup) && fs::is_regular_file(backup, ec);

    // Already open: the same file in both views of a session is a clone.
    core::Buffer* existing = nullptr;
    if (!untitled) {
        existing = findByPath(normalisedPath(f.filename));
    } else if (hasBackup) {
        for (core::Buffer* b : buffers())
            if (b->isUntitled() && b->snapshotPath == backup)
                existing = b;
    }
    if (existing) {
        Tab* t = tabOf(*existing, viewIdx);
        if (!t) {
            const Tab* src = tabsOf(*existing).front();
            t = addTab(src->buf, viewIdx, src);
        }
        t->view->restorePosition({f.firstVisibleLine, f.xOffset, f.startPos, f.endPos, f.selMode, f.scrollWidth});
        return t;
    }
    // Upstream drops entries whose file is gone, unless Preferences > Backup >
    // "Remember inaccessible files" (KeepSessionAbsentFileEntries): then the
    // document opens empty and is created on save.
    if (!hasBackup && (untitled || (!fs::is_regular_file(f.filename, ec) && !settings_.keepAbsentSessionFiles)))
        return nullptr;

    std::string err;
    auto lf = core::loadFile(hasBackup ? backup : fs::path(f.filename), f.encoding, &err);
    if (!lf) {
        g_warning("Session: cannot load %s: %s", f.filename.c_str(), err.c_str());
        return nullptr;
    }
    auto buf = std::make_shared<core::Buffer>();
    core::Buffer& b = *buf;
    if (untitled) {
        b.untitledName = f.filename;
        if (std::any_of(tabs_.begin(), tabs_.end(), [&](const auto& t) { return t->buf->isUntitled() && t->buf->untitledName == f.filename; }))
            b.untitledName = nextUntitledName();
    }
    b.userReadOnly = f.userReadOnly;
    Tab* t = addTab(buf, viewIdx);
    notifyPlugins(NPPN_FILEBEFOREOPEN, &b);
    applyLoaded(*t, untitled ? fs::path{} : normalisedPath(f.filename), std::move(*lf));

    if (hasBackup) {
        // Unsaved text came back from the snapshot: dirty until saved.
        b.snapshotPath = backup;
        b.forcedDirty = true;
        b.snapshotStale = false;
        if (!untitled) {
            // Compare against the file as it was when the snapshot was taken,
            // so outside edits made meanwhile are reported.
            b.diskStamp = f.originalFileLastModifTimestamp
                              ? platform::FileStamp{true, platform::fromFileTime(f.originalFileLastModifTimestamp), -1}
                              : platform::statFile(b.path);
            if (!platform::statFile(b.path).sameAs(b.diskStamp)) {
                b.externalChangePending = true;
                scheduleChangeCheck();
            }
        }
    }
    if (const core::UserLanguage* u = userLanguage(f.lang)) {
        setUserLanguage(b, *u);
    } else if (const core::Language* lang = langs_.byAnyName(f.lang)) {
        setLanguage(b, *lang);
    } else if (untitled) {
        setLanguage(b, langs_.plainText());
    } else {
        detectLanguage(b, b.path);
    }
    std::vector<sptr_t> marks(f.marks.begin(), f.marks.end());
    std::vector<sptr_t> folds(f.folds.begin(), f.folds.end());
    t->view->setBookmarks(marks);
    t->view->contractFolds(folds);
    t->view->restorePosition({f.firstVisibleLine, f.xOffset, f.startPos, f.endPos, f.selMode, f.scrollWidth});
    refreshTab(*t);
    notifyPlugins(NPPN_FILEOPENED, &b);
    if (hasBackup)
        notifyPlugins(NPPN_SNAPSHOTDIRTYFILELOADED, nullptr, reinterpret_cast<uptr_t>(&b));
    return t;
}

bool MainWindow::loadSession(const fs::path& file, std::string* error)
{
    core::Session s;
    if (!core::readSession(file, s, error))
        return false;
    // Folder as Workspace comes back with the session that had it open.
    if (s.includeFileBrowser && folder_) {
        folder_->restoreFrom(s);
        dock_->show("folderAsWorkspace", false);
    }

    // A lone pristine "new 1" makes way for the session, as upstream.
    if (!s.empty() && tabs_.size() == 1 && tabs_[0]->buf->isUntitled() && !isModified(*tabs_[0]->buf) &&
        tabs_[0]->view->length() == 0) {
        closing_ = true; // no view juggling for a moment
        removeTab(*tabs_[0]);
        closing_ = false;
    }

    std::vector<Tab*> opened[2];
    for (int vi : {kMainView, kSubView})
        for (const core::SessionFile& f : s.views[vi])
            opened[vi].push_back(openSessionFile(f, vi));

    if (tabs_.empty()) {
        newDocument();
        return true;
    }
    for (int vi : {kMainView, kSubView}) {
        const std::size_t idx = s.activeIndex[vi];
        if (idx < opened[vi].size() && opened[vi][idx]) {
            GtkNotebook* nb = GTK_NOTEBOOK(notebooks_[vi]);
            gtk_notebook_set_current_page(nb, gtk_notebook_page_num(nb, opened[vi][idx]->view->widget()));
        }
    }
    activeView_ = tabCount(s.activeView) > 0 ? s.activeView : (tabCount(kMainView) > 0 ? kMainView : kSubView);
    updateViewVisibility();
    if (Tab* t = currentTabOf(activeView_))
        activateTab(*t);
    return true;
}

void MainWindow::snapshotNow()
{
    if (!snapshotsOn())
        return;
    for (core::Buffer* b : buffers()) {
        std::error_code ec;
        if (isModified(*b)) {
            if (b->snapshotStale || !fs::is_regular_file(b->snapshotPath, ec)) {
                std::string err;
                if (!core::writeSnapshot(*b, tabsOf(*b).front()->view->text(), &err))
                    g_warning("Snapshot of %s failed: %s", b->fullName().c_str(), err.c_str());
            }
        } else if (!b->snapshotPath.empty()) {
            core::deleteSnapshot(*b);
        }
    }
    core::Session s = currentSession();
    if (lastWrittenSession_ && *lastWrittenSession_ == s)
        return; // nothing moved: spare the disk
    std::string err;
    if (core::writeSession(lastSessionPath(), s, &err))
        lastWrittenSession_ = std::move(s);
    else
        g_warning("Cannot write the session: %s", err.c_str());
}

void MainWindow::restartSnapshotTimer()
{
    if (snapshotTimer_) {
        g_source_remove(snapshotTimer_);
        snapshotTimer_ = 0;
    }
    if (!snapshotsOn())
        return;
    snapshotTimer_ = g_timeout_add(static_cast<guint>(settings_.snapshotIntervalMs), +[](gpointer p) -> gboolean {
        static_cast<MainWindow*>(p)->snapshotNow();
        return G_SOURCE_CONTINUE;
    }, this);
}

bool MainWindow::requestQuit()
{
    notifyPlugins(NPPN_BEFORESHUTDOWN);
    const bool ok = quitConfirmed();
    if (!ok)
        notifyPlugins(NPPN_CANCELSHUTDOWN);
    return ok;
}

bool MainWindow::quitConfirmed()
{
    if (!exitSessionFile_.empty()) {
        std::string err;
        if (!saveSession(exitSessionFile_, &err))
            g_warning("Cannot write the session: %s", err.c_str());
    }
    // A changed workspace in a Project Panel is offered for saving first.
    for (auto& p : projects_)
        if (p && !p->confirmClose())
            return false;
    if (snapshotsOn()) {
        // Upstream snapshot mode: unsaved documents are kept, nothing is asked.
        for (core::Buffer* b : buffers())
            b->snapshotStale = b->snapshotStale || isModified(*b);
        lastWrittenSession_.reset();
        snapshotNow();
        return true;
    }
    for (core::Buffer* b : buffers())
        if (!confirmSave(*b))
            return false;
    for (core::Buffer* b : buffers())
        core::deleteSnapshot(*b); // snapshots are off: none must linger
    if (sessionOn()) {
        std::string err;
        if (!saveSession(lastSessionPath(), &err))
            g_warning("Cannot write the session: %s", err.c_str());
    }
    return true;
}

} // namespace npp::ui
