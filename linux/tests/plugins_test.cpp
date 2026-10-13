// Phase 5 tests: plugins. A live MainWindow loads tests/plugins/TestPlugin
// (built against the SDK only) and the test checks both sides: what the
// plugin saw (testPlugin_get) and what it did to the window.

#include "core/LanguageCatalog.h"
#include "core/PluginFolders.h"
#include "platform/Zip.h"
#include "platform/FileIO.h"
#include "platform/Paths.h"
#include "ui/MainWindow.h"

#include "LangTable.generated.h"

#include <windows.h>
#include "PluginInterface.h"
#include "menuCmdID.h"
#include "resource.h"

#include <gio/gio.h>
#include <gtk/gtk.h>

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace fs = std::filesystem;
using namespace npp;

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

fs::path g_dir;
const char* (*g_get)(const char*) = nullptr;

std::string get(const std::string& key)
{
    return g_get ? g_get(key.c_str()) : std::string();
}

int nppn(int code)
{
    return std::atoi(get("nppn:" + std::to_string(code)).c_str());
}

void writeText(const fs::path& p, const std::string& s)
{
    fs::create_directories(p.parent_path());
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

template <class F>
bool waitFor(F cond, int ms = 5000)
{
    const gint64 end = g_get_monotonic_time() + ms * 1000;
    while (!cond()) {
        if (g_get_monotonic_time() > end)
            return false;
        pump();
        g_usleep(10000);
    }
    return true;
}

// The command's only accelerator is key + mods.
bool accelIs(GtkApplication* app, int cmdId, guint key, GdkModifierType mods)
{
    gchar** a = gtk_application_get_accels_for_action(app, ui::CommandTable::detailedName(cmdId).c_str());
    guint k = 0;
    GdkModifierType m = GdkModifierType(0);
    const bool one = a && a[0] && !a[1];
    if (one)
        gtk_accelerator_parse(a[0], &k, &m);
    g_strfreev(a);
    return one && k == key && m == mods;
}

std::unique_ptr<ui::MainWindow> makeWindow(GtkApplication* app, core::LanguageCatalog& langs)
{
    auto w = std::make_unique<ui::MainWindow>(app, langs, false);
    // "Save changes?" on exit: no (an untitled document would need a file dialog).
    w->setPromptHandler([](const std::string&, const std::string&, bool) { return ui::MainWindow::Answer::No; });
    w->loadPlugins({fs::path(NPP_TEST_PLUGINS_DIR)});
    w->newDocument();
    w->present();
    pump();
    w->pluginsReady();
    pump();
    return w;
}

// Command IDs, as upstream numbers them: separators get none.
constexpr int kSayHello = ID_PLUGINS_CMD;
constexpr int kToggle = ID_PLUGINS_CMD + 1;
constexpr int kPanel = ID_PLUGINS_CMD + 2;
constexpr int kShortcut = ID_PLUGINS_CMD + 3;
constexpr int kAsk = ID_PLUGINS_CMD + 4;
constexpr int kThread = ID_PLUGINS_CMD + 5;

void testLoading(ui::MainWindow& w)
{
    ui::PluginManager* pm = w.plugins();
    CHECK(pm != nullptr);
    if (!pm)
        return;
    CHECK(pm->plugins().size() == 1);
    // NotAPlugin/NotAPlugin.so is reported, not loaded.
    CHECK(w.pluginErrors().size() == 1 && w.pluginErrors()[0].find("NotAPlugin.so") != std::string::npos
          && w.pluginErrors()[0].find("setInfo") != std::string::npos);
    const auto& p = *pm->plugins().front();
    CHECK(p.moduleName == "TestPlugin.so" && p.folder == "TestPlugin" && p.displayName == "Test Plugin");
    CHECK(p.root == fs::path(NPP_TEST_PLUGINS_DIR));
    void* sym = dlsym(p.lib, "testPlugin_get");
    CHECK(sym != nullptr);
    g_get = reinterpret_cast<const char* (*)(const char*)>(sym);

    CHECK(pm->commands().size() == 6);
    CHECK(pm->commands()[1].internalId == 2 && pm->commands()[1].cmdId == kToggle);
    for (int id : {kSayHello, kToggle, kPanel, kShortcut, kAsk, kThread})
        CHECK(w.hasCommand(id));
    CHECK(!w.hasCommand(ID_PLUGINS_CMD + 6));
    // The IDs are in the plugin's FuncItems by NPPN_TBMODIFICATION; a separator keeps 0.
    CHECK(get("cmdIdsAtTb") == std::to_string(kSayHello) + "," + std::to_string(kToggle) + ",0");
    CHECK(nppn(NPPN_TBMODIFICATION) == 1);
    CHECK(get("setInfo") == "yes");
}

void testReady(ui::MainWindow& w)
{
    CHECK(nppn(NPPN_READY) == 1);
    const std::string v = NPP_VERSION; // "8.9.6.4" -> 8.964 with zero padding
    std::string aux;
    for (char c : v.substr(v.find('.') + 1))
        if (c != '.')
            aux += c;
    aux.resize(3, '0');
    CHECK(get("version") == v.substr(0, v.find('.')) + "." + std::to_string(std::atoi(aux.c_str())));
    CHECK(get("gtkVersion") == "1");
    CHECK(get("mainWindow") == "yes");
    CHECK(get("path") == "new 1");
    CHECK(get("openFiles") == "1");
    CHECK(get("posOfBuffer") == "0");
    const fs::path conf = platform::userConfigDir() / "plugins" / "config";
    CHECK(get("configDir") == conf.string());
    CHECK(fs::is_directory(conf));
    CHECK(get("configDirCopied") == "1"); // upstream: TRUE once copied
    CHECK(get("configDirLen") == std::to_string(conf.string().size()) + "/" + std::to_string(conf.string().size()));
    CHECK(get("tooSmall") == "0");
    CHECK(get("home") == fs::path(NPP_TEST_PLUGINS_DIR).string());
    CHECK(get("langName") == "C++");
    CHECK(get("fileNameOk") == "1" && get("fileName") == "new 1");
    CHECK(get("cmdIds") == std::to_string(ID_PLUGINS_CMD_DYNAMIC));
    CHECK(get("markers") == std::to_string(MARKER_PLUGINS));
    CHECK(get("indicator") == std::to_string(INDICATOR_PLUGINS));
    CHECK(get("tooManyMarkers") == "0,0");
    CHECK(get("msgToPlugin") == "1" && get("received") == "4242");
    CHECK(get("msgToNobody") == "0");
    CHECK(get("postToNowhere") == "0");
    pump();
    CHECK(w.activeView()->execute(SCI_GETEXTRAASCENT) == 3); // the PostMessage arrived
    CHECK(nppn(NPPN_BUFFERACTIVATED) >= 1 && get("last" + std::to_string(NPPN_BUFFERACTIVATED - NPPN_FIRST)) == "new 1");
}

void testCommands(GtkApplication* app, ui::MainWindow& w)
{
    // Text through the main view's handle and its direct function.
    CHECK(w.runCommand(kSayHello));
    CHECK(w.activeView()->text() == "hello from plugin");
    CHECK(std::atoi(get("scn").c_str()) > 0); // SCN_* of the main view reached the plugin
    CHECK(get("beforeInsert") == "yes");      // NPPM_ADDSCNMODIFIEDFLAGS

    // _init2Check items start checked; NPPM_SETMENUITEMCHECK changes them.
    CHECK(w.commandChecked(kToggle));
    CHECK(w.runCommand(kToggle));
    CHECK(!w.commandChecked(kToggle));
    CHECK(w.runCommand(kToggle));
    CHECK(w.commandChecked(kToggle));

    // Default key from _pShKey; an item gains a check mark when asked.
    CHECK(!w.commandChecked(kShortcut));
    CHECK(accelIs(app, kShortcut, GDK_KEY_t, GdkModifierType(GDK_CONTROL_MASK | GDK_MOD1_MASK | GDK_SHIFT_MASK)));
    CHECK(w.runCommand(kShortcut));
    CHECK(get("shortcut") == "111+84");
    CHECK(w.commandChecked(kShortcut));

    // shortcuts.xml <PluginCommands> overrides the default, by module and index.
    writeText(platform::userConfigDir() / "shortcuts.xml",
              "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<NotepadPlus><InternalCommands/><Macros/><UserDefinedCommands/>"
              "<PluginCommands><PluginCommand moduleName=\"TestPlugin.so\" internalID=\"4\" Ctrl=\"no\" Alt=\"no\" Shift=\"no\" Key=\"120\" />"
              "<PluginCommand moduleName=\"Gone.so\" internalID=\"1\" Ctrl=\"yes\" Alt=\"no\" Shift=\"no\" Key=\"65\" /></PluginCommands>"
              "<ScintillaKeys/></NotepadPlus>\n");
    w.reloadShortcuts();
    CHECK(accelIs(app, kShortcut, GDK_KEY_F9, GdkModifierType(0)));
    CHECK(w.runCommand(kShortcut));
    CHECK(get("shortcut") == "000+120");
    const auto list = w.pluginCommandList();
    CHECK(list.size() == 6 && list[3].name == "Shortcut Command" && list[3].plugin == "Test Plugin" && list[3].key.vk == 120);
    w.notifyShortcutRemapped(kShortcut);
    CHECK(get("remapped") == std::to_string(kShortcut) + ":120");
    // Saving keeps the entry of a plugin that is not loaded.
    CHECK(w.saveShortcuts());
    CHECK(readText(platform::userConfigDir() / "shortcuts.xml").find("Gone.so") != std::string::npos);

    // MessageBox from a plugin.
    std::string asked;
    ui::PluginManager::messageBoxHook = [&](const std::string& text, const std::string& caption, unsigned type) {
        asked = caption + ": " + text + " " + std::to_string(type & 0xF);
        return IDYES;
    };
    CHECK(w.runCommand(kAsk));
    CHECK(asked == "Test Plugin: Continue? 4" && get("messagebox") == std::to_string(IDYES));
    ui::PluginManager::messageBoxHook = nullptr;

    // SendMessage from another thread waits for the GTK thread, as on Windows.
    CHECK(w.runCommand(kThread));
    CHECK(waitFor([] { return get("thread") != "-1"; }));
    CHECK(get("thread") == "1");

}

void testNotifications(ui::MainWindow& w)
{
    const fs::path f = g_dir / "plugin.cpp";
    writeText(f, "int x;\n");
    const int opened = nppn(NPPN_FILEOPENED);
    CHECK(w.openFiles({f}) == 1);
    CHECK(nppn(NPPN_FILEBEFORELOAD) == 1 && nppn(NPPN_FILEBEFOREOPEN) == 1 && nppn(NPPN_FILEOPENED) == opened + 1);
    CHECK(get("last" + std::to_string(NPPN_FILEOPENED - NPPN_FIRST)) == f.string());
    pump();
    CHECK(get("last" + std::to_string(NPPN_BUFFERACTIVATED - NPPN_FIRST)) == f.string());

    // Buffer messages.
    const auto id = static_cast<uptr_t>(w.pluginMessage(NPPM_GETCURRENTBUFFERID, 0, 0));
    CHECK(w.pluginMessage(NPPM_GETBUFFERLANGTYPE, id, 0) == L_CPP);
    CHECK(w.pluginMessage(NPPM_GETBUFFERLANGTYPE, 12345, 0) == -1); // not a buffer
    CHECK(w.pluginMessage(NPPM_SETBUFFERLANGTYPE, id, L_PYTHON) == TRUE);
    CHECK(w.activeBuffer()->language && w.activeBuffer()->language->name == "python");
    CHECK(get("last" + std::to_string(NPPN_LANGCHANGED - NPPN_FIRST)) == f.string());
    CHECK(w.pluginMessage(NPPM_GETBUFFERENCODING, id, 0) == 4); // UTF-8 without BOM
    CHECK(w.pluginMessage(NPPM_GETBUFFERFORMAT, id, 0) == 2);   // Unix
    CHECK(w.pluginMessage(NPPM_GETNBOPENFILES, 0, ALL_OPEN_FILES) == 2);
    CHECK(w.pluginMessage(NPPM_GETPOSFROMBUFFERID, id, MAIN_VIEW) == 1);
    CHECK(static_cast<uptr_t>(w.pluginMessage(NPPM_GETBUFFERIDFROMPOS, 1, MAIN_VIEW)) == id);
    CHECK(w.pluginMessage(NPPM_GETBUFFERIDFROMPOS, 5, MAIN_VIEW) == 0);
    CHECK(w.pluginMessage(NPPM_GETCURRENTDOCINDEX, 0, MAIN_VIEW) == 1);
    CHECK(w.pluginMessage(NPPM_GETCURRENTDOCINDEX, 0, SUB_VIEW) == -1);

    // NPPN_FILEBEFORESAVE comes before the text is written: the plugin's "!" is saved.
    w.activeView()->execute(SCI_APPENDTEXT, 1, reinterpret_cast<sptr_t>("y"));
    CHECK(w.pluginMessage(NPPM_SAVECURRENTFILE, 0, 0) == TRUE);
    CHECK(readText(f) == "int x;\ny!");
    CHECK(nppn(NPPN_FILEBEFORESAVE) == 1 && nppn(NPPN_FILESAVED) == 1);
    CHECK(w.pluginMessage(NPPM_SAVECURRENTFILE, 0, 0) == FALSE); // nothing to save

    w.runCommand(IDM_EDIT_TOGGLEREADONLY);
    CHECK(get("readOnly") == std::to_string(DOCSTATUS_READONLY));
    w.runCommand(IDM_EDIT_TOGGLEREADONLY);

    // Strings in the plugin's buffer: the length first, then the text.
    const std::wstring wf = ui::toWide(f.string());
    std::wstring path(static_cast<std::size_t>(w.pluginMessage(NPPM_GETFULLPATHFROMBUFFERID, id, 0)), L'\0');
    w.pluginMessage(NPPM_GETFULLPATHFROMBUFFERID, id, reinterpret_cast<sptr_t>(path.data()));
    CHECK(path == wf);
    wchar_t small[4];
    CHECK(w.pluginMessage(NPPM_GETFULLCURRENTPATH, 4, reinterpret_cast<sptr_t>(small)) == FALSE);
    wchar_t buf[MAX_PATH];
    CHECK(w.pluginMessage(NPPM_GETEXTPART, MAX_PATH, reinterpret_cast<sptr_t>(buf)) == TRUE && std::wstring(buf) == L".cpp");
    CHECK(w.pluginMessage(NPPM_GETNAMEPART, MAX_PATH, reinterpret_cast<sptr_t>(buf)) == TRUE && std::wstring(buf) == L"plugin");
    CHECK(w.pluginMessage(NPPM_GETCURRENTDIRECTORY, MAX_PATH, reinterpret_cast<sptr_t>(buf)) == TRUE && std::wstring(buf) == ui::toWide(g_dir.string()));
    w.activeView()->gotoLine(1, 1);
    CHECK(w.pluginMessage(NPPM_GETCURRENTLINE, 0, 0) == 1 && w.pluginMessage(NPPM_GETCURRENTCOLUMN, 0, 0) == 1);

    // Switching and activating documents.
    CHECK(w.pluginMessage(NPPM_ACTIVATEDOC, MAIN_VIEW, 0) == TRUE);
    CHECK(w.activeBuffer()->isUntitled());
    CHECK(w.pluginMessage(NPPM_SWITCHTOFILE, 0, reinterpret_cast<sptr_t>(wf.c_str())) == TRUE);
    CHECK(w.activeBuffer()->path == f);
    CHECK(w.pluginMessage(NPPM_MENUCOMMAND, 0, IDM_FILE_CLOSE) == TRUE);
    CHECK(nppn(NPPN_FILEBEFORECLOSE) == 1 && nppn(NPPN_FILECLOSED) == 1);
    CHECK(get("last" + std::to_string(NPPN_FILECLOSED - NPPN_FIRST)).empty()); // the ID is gone by then
    CHECK(w.pluginMessage(NPPM_DOOPEN, 0, reinterpret_cast<sptr_t>(wf.c_str())) == TRUE);
    CHECK(w.activeBuffer()->path == f);

    // Untitled names and the language list.
    w.newDocument();
    const auto untitled = static_cast<uptr_t>(w.pluginMessage(NPPM_GETCURRENTBUFFERID, 0, 0));
    CHECK(w.pluginMessage(NPPM_SETUNTITLEDNAME, untitled, reinterpret_cast<sptr_t>(L"scratch")) == TRUE);
    CHECK(w.activeBuffer()->name() == "scratch");
    CHECK(w.pluginMessage(NPPM_SETUNTITLEDNAME, static_cast<uptr_t>(w.pluginMessage(NPPM_GETBUFFERIDFROMPOS, 1, MAIN_VIEW)),
                          reinterpret_cast<sptr_t>(L"x")) == FALSE); // a file, not untitled
    CHECK(w.pluginMessage(NPPM_GETLANGUAGEDESC, L_PYTHON, 0) == static_cast<sptr_t>(std::strlen(gen::kLangTable[L_PYTHON].longName)));
    CHECK(w.pluginMessage(NPPM_GETBOOKMARKID, 0, 0) == ui::EditorView::kBookmarkMarker);

    // Replace All: NPPN_GLOBALMODIFIED.
    w.activeView()->setText("aaa");
    core::FindOptions o;
    o.what = "a";
    o.with = "b";
    CHECK(w.findDialog().doReplaceAll(o) == 3);
    CHECK(nppn(NPPN_GLOBALMODIFIED) == 1);
}

void testPanel(ui::MainWindow& w)
{
    const std::string id = "plugin:TestPlugin.so:3";
    CHECK(w.runCommand(kPanel));
    CHECK(w.dock().has(id) && w.dock().isShown(id));
    CHECK(w.dock().areaOf(id) == ui::DockManager::Area::Right);
    // NPPM_DMMGETPLUGINHWNDBYNAME finds it by module (and name).
    CHECK(w.pluginMessage(NPPM_DMMGETPLUGINHWNDBYNAME, 0, reinterpret_cast<sptr_t>(L"testplugin.so")) != 0);
    CHECK(w.pluginMessage(NPPM_DMMGETPLUGINHWNDBYNAME, reinterpret_cast<uptr_t>(L"Test Panel"), reinterpret_cast<sptr_t>(L"TestPlugin.so")) != 0);
    CHECK(w.pluginMessage(NPPM_DMMGETPLUGINHWNDBYNAME, reinterpret_cast<uptr_t>(L"Other"), reinterpret_cast<sptr_t>(L"TestPlugin.so")) == 0);
    // Closed by the user: the plugin hears DMN_CLOSE.
    w.dock().hide(id);
    CHECK(get("dmnClose") == "3");
    CHECK(w.runCommand(kPanel));
    CHECK(w.dock().isShown(id));
}

// ---- Plugins Admin: zip, plugin folders ------------------------------------

std::uint32_t crc32(const std::string& d)
{
    std::uint32_t c = 0xFFFFFFFF;
    for (unsigned char b : d) {
        c ^= b;
        for (int k = 0; k < 8; ++k)
            c = (c >> 1) ^ (0xEDB88320 & (0 - (c & 1)));
    }
    return ~c;
}

std::string deflateRaw(const std::string& in)
{
    GZlibCompressor* z = g_zlib_compressor_new(G_ZLIB_COMPRESSOR_FORMAT_RAW, -1);
    std::string out(in.size() + 1024, '\0');
    gsize read = 0, written = 0;
    g_converter_convert(G_CONVERTER(z), in.data(), in.size(), out.data(), out.size(), G_CONVERTER_INPUT_AT_END, &read, &written, nullptr);
    g_object_unref(z);
    out.resize(written);
    return out;
}

// A zip as zip tools write it: local headers, then the central directory.
void writeZip(const fs::path& file, const std::vector<std::tuple<std::string, std::string, bool>>& entries)
{
    std::string out, central;
    auto le = [](std::string& s, std::uint32_t v, int bytes) {
        for (int i = 0; i < bytes; ++i)
            s += static_cast<char>((v >> (8 * i)) & 0xFF);
    };
    for (const auto& [name, data, deflated] : entries) {
        const std::string body = deflated ? deflateRaw(data) : data;
        const auto offset = static_cast<std::uint32_t>(out.size());
        const std::uint32_t crc = crc32(data), method = deflated ? 8 : 0;
        le(out, 0x04034b50, 4); le(out, 20, 2); le(out, 0, 2); le(out, method, 2); le(out, 0, 4);
        le(out, crc, 4); le(out, static_cast<std::uint32_t>(body.size()), 4); le(out, static_cast<std::uint32_t>(data.size()), 4);
        le(out, static_cast<std::uint32_t>(name.size()), 2); le(out, 0, 2);
        out += name + body;
        le(central, 0x02014b50, 4); le(central, 20, 2); le(central, 20, 2); le(central, 0, 2); le(central, method, 2); le(central, 0, 4);
        le(central, crc, 4); le(central, static_cast<std::uint32_t>(body.size()), 4); le(central, static_cast<std::uint32_t>(data.size()), 4);
        le(central, static_cast<std::uint32_t>(name.size()), 2); le(central, 0, 2); le(central, 0, 2); le(central, 0, 2); le(central, 0, 2);
        le(central, 0, 4); le(central, offset, 4);
        central += name;
    }
    const auto cdOffset = static_cast<std::uint32_t>(out.size());
    out += central;
    le(out, 0x06054b50, 4); le(out, 0, 2); le(out, 0, 2);
    le(out, static_cast<std::uint32_t>(entries.size()), 2); le(out, static_cast<std::uint32_t>(entries.size()), 2);
    le(out, static_cast<std::uint32_t>(central.size()), 4); le(out, cdOffset, 4); le(out, 0, 2);
    writeText(file, out);
}

void testPluginFolders()
{
    const fs::path so = fs::path(NPP_TEST_PLUGINS_DIR) / "TestPlugin" / "TestPlugin.so";
    const std::string lib = readText(so);
    const std::string text(5000, 'x');
    const fs::path root = g_dir / "plugin-root";

    // Zip extraction: stored and deflated, Windows separators, unsafe names.
    const fs::path z1 = g_dir / "z1.zip";
    writeZip(z1, {{"a/", "", false}, {"a\\b.txt", text, true}, {"c.bin", "stored", false}, {"empty.txt", "", true}});
    std::string err;
    CHECK(platform::extractZip(z1, g_dir / "z1", &err));
    CHECK(readText(g_dir / "z1" / "a" / "b.txt") == text && readText(g_dir / "z1" / "c.bin") == "stored");
    CHECK(fs::is_regular_file(g_dir / "z1" / "empty.txt"));
    writeZip(g_dir / "evil.zip", {{"../evil.txt", "x", false}});
    CHECK(!platform::extractZip(g_dir / "evil.zip", g_dir / "evil", &err) && err.find("unsafe") != std::string::npos);
    CHECK(!fs::exists(g_dir / "evil.txt"));
    writeText(g_dir / "notzip.zip", "not a zip");
    CHECK(!platform::extractZip(g_dir / "notzip.zip", g_dir / "nz", &err));

    // A .so, as Import plugin(s) takes it.
    CHECK(core::installPlugin(so, root, &err) == std::optional<std::string>("TestPlugin"));
    CHECK(readText(root / "TestPlugin" / "TestPlugin.so") == lib);
    // A zip with the folder inside (and an extra file), and one with the .so at the top.
    writeZip(g_dir / "Zipped-1.0.zip", {{"Zipped/Zipped.so", lib, true}, {"Zipped/doc/readme.txt", text, true}});
    CHECK(core::installPlugin(g_dir / "Zipped-1.0.zip", root, &err) == std::optional<std::string>("Zipped"));
    CHECK(readText(root / "Zipped" / "Zipped.so") == lib && readText(root / "Zipped" / "doc" / "readme.txt") == text);
    writeZip(g_dir / "top.zip", {{"Top.so", lib, false}, {"Top.txt", "t", true}});
    CHECK(core::installPlugin(g_dir / "top.zip", root, &err) == std::optional<std::string>("Top"));
    CHECK(fs::is_regular_file(root / "Top" / "Top.so") && fs::is_regular_file(root / "Top" / "Top.txt"));
    // Not one plugin: refused, nothing left behind.
    writeZip(g_dir / "two.zip", {{"A.so", "a", false}, {"B.so", "b", false}});
    CHECK(!core::installPlugin(g_dir / "two.zip", root, &err) && !fs::exists(root / ".installing"));
    CHECK(!core::installPlugin(g_dir / "z1.zip", root, &err));
    // A folder holding <Name>.so.
    CHECK(core::installPlugin(root / "Top", g_dir / "root2", &err) == std::optional<std::string>("Top"));

    auto list = core::listPlugins({root});
    CHECK(list.size() == 3 && list[0].name == "TestPlugin" && list[1].name == "Top" && list[2].name == "Zipped");
    CHECK(list[0].writable && !list[0].disabled && list[0].file == root / "TestPlugin" / "TestPlugin.so");
    // Deactivate (upstream's <plugins>/disabled), activate, remove.
    CHECK(core::deactivatePlugin(list[1], &err));
    list = core::listPlugins({root});
    CHECK(list.size() == 3 && list[2].name == "Top" && list[2].disabled && list[2].dir == root / "disabled" / "Top");
    CHECK(core::activatePlugin(list[2], &err));
    CHECK(fs::is_regular_file(root / "Top" / "Top.so") && !fs::exists(root / "disabled" / "Top"));
    CHECK(core::deactivatePlugin(core::listPlugins({root})[2], &err));
    // Installing a plugin again replaces its deactivated copy.
    CHECK(core::installPlugin(g_dir / "Zipped-1.0.zip", root, &err));
    CHECK(core::listPlugins({root}).size() == 3);
    CHECK(core::listPlugins({root})[0].version == "2.5.1"); // read from the file, not loaded
    CHECK(core::removePlugin(core::listPlugins({root})[0], &err) && !fs::exists(root / "TestPlugin"));
    CHECK(core::listPlugins({root}).size() == 2);
    // A .so copied without its folder is listed (never loaded); Remove deletes just it.
    fs::copy_file(so, root / "Loose.so");
    list = core::listPlugins({root});
    CHECK(list.size() == 3 && list[0].name == "Loose" && list[0].misplaced && list[0].version == "2.5.1");
    CHECK(!core::deactivatePlugin(list[0], &err));
    CHECK(core::removePlugin(list[0], &err) && !fs::exists(root / "Loose.so") && fs::is_directory(root / "Zipped"));
}

void testAdminStatus(ui::MainWindow& w)
{
    CHECK(w.plugins()->plugins().front()->version == "2.5.1");
    const auto rows = core::listPlugins({fs::path(NPP_TEST_PLUGINS_DIR)});
    CHECK(rows.size() == 2 && rows[0].name == "NotAPlugin" && rows[1].name == "TestPlugin");
    CHECK(w.pluginFolderStatus(rows[1], rows) == "Loaded");
    CHECK(w.pluginFolderStatus(rows[0], rows).rfind("Not loaded: not a Notepad++ plugin", 0) == 0);

    // Install into the user's directory (NPP_GTK_PLUGINS_DIR first); a second
    // time asks before replacing (the test answers No).
    const fs::path user = g_dir / "user-plugins";
    g_setenv("NPP_GTK_PLUGINS_DIR", user.c_str(), TRUE);
    const fs::path so = fs::path(NPP_TEST_PLUGINS_DIR) / "TestPlugin" / "TestPlugin.so";
    CHECK(w.installPlugins({so}) == 1);
    CHECK(w.installPlugins({so}) == 0);
    auto mine = core::listPlugins({user});
    CHECK(mine.size() == 1 && w.pluginFolderStatus(mine[0], mine) == "Loads at the next start" && mine[0].version == "2.5.1");
    // The same name in a later directory is hidden by the first.
    const fs::path later = g_dir / "later-plugins";
    CHECK(core::installPlugin(so, later));
    auto all = core::listPlugins({user, later});
    CHECK(all.size() == 2 && w.pluginFolderStatus(all[1], all).find("has a plugin of this name") != std::string::npos);
    CHECK(core::deactivatePlugin(mine[0]));
    mine = core::listPlugins({user});
    CHECK(w.pluginFolderStatus(mine[0], mine) == "Deactivated");
    g_unsetenv("NPP_GTK_PLUGINS_DIR");
}

} // namespace

int main(int argc, char** argv)
{
    char tmpl[] = "/tmp/nppgtk-plugins-XXXXXX";
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
    GtkApplication* app = gtk_application_new("org.notepad_plus_plus.GtkPortPluginTests", G_APPLICATION_NON_UNIQUE);
    CHECK(g_application_register(G_APPLICATION(app), nullptr, nullptr));

    {
        auto w = makeWindow(app, langs);
        std::printf("loading...\n");
        testLoading(*w);
        std::printf("NPPN_READY...\n");
        testReady(*w);
        std::printf("commands...\n");
        testCommands(app, *w);
        std::printf("notifications and messages...\n");
        testNotifications(*w);
        std::printf("docking...\n");
        testPanel(*w);
        std::printf("plugins admin...\n");
        testPluginFolders();
        testAdminStatus(*w);
        w->settings().noSessionPlugins = true; // -nosession window: plugin panels come back only when ticked
        w->sendPluginCommandLine("TestPlugin=1");
        CHECK(get("cmdline") == "TestPlugin=1");
        CHECK(w->requestQuit());
        CHECK(nppn(NPPN_BEFORESHUTDOWN) == 1 && nppn(NPPN_CANCELSHUTDOWN) == 0);
    }
    std::printf("shutdown and restart...\n");
    CHECK(nppn(NPPN_SHUTDOWN) == 1);
    {
        // The panel open at exit comes back through its plugin's command.
        auto w = makeWindow(app, langs);
        CHECK(get("setInfo") == "yes" && nppn(NPPN_READY) == 1);
        CHECK(w->dock().isShown("plugin:TestPlugin.so:3"));
    }
    {
        // -noPlugin: nothing loaded, the menu keeps "Open Plugins Folder".
        ui::MainWindow w(app, langs, false);
        CHECK(w.plugins() == nullptr);
        CHECK(w.hasCommand(IDM_SETTING_OPENPLUGINSDIR));
        CHECK(!w.hasCommand(ID_PLUGINS_CMD));
        CHECK(w.pluginMessage(NPPM_GETCURRENTVIEW, 0, 0) == 0);
    }

    g_object_unref(app);
    std::error_code ec;
    fs::remove_all(root, ec);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
