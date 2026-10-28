// Plugins: what SendMessage(nppData._nppHandle, NPPM_...) does — upstream
// NppBigSwitch.cpp, message by message, with the same return values — and the
// plugins' place in the window: the Plugins menu, their shortcuts, toolbar
// buttons and docking panels, and the notifications they get (NPPN_* and the
// editors' SCN_*).
//
// Buffer IDs are core::Buffer pointers, checked against the open documents
// before use, as upstream checks its BufferIDs.

#include "ui/MainWindow.h"

#include "core/EditOps.h"
#include "core/Session.h"
#include "platform/Paths.h"

#include "LangTable.generated.h"

#include <ILexer.h> // must precede Lexilla.h
#include <Lexilla.h>

// The plugin SDK: upstream's plugin headers on the port's <windows.h> shim.
#include <windows.h>
#include "PluginInterface.h"
#include "Docking.h"
#include "dockingResource.h"
#include "NppGtk.h"
#include "menuCmdID.h"
#include "resource.h"

#include <algorithm>
#include <cstring>

namespace fs = std::filesystem;

namespace npp::ui {

namespace {

constexpr const char* kPluginPanelPrefix = "plugin:";

// Messages with (size_t strLen, wchar_t* str): a null buffer asks for the
// length; a buffer too small gets nothing and 0; else the length.
sptr_t copyOrLength(const std::wstring& w, uptr_t size, sptr_t buf)
{
    if (buf) {
        if (w.size() >= size)
            return 0;
        std::wmemcpy(reinterpret_cast<wchar_t*>(buf), w.c_str(), w.size() + 1);
    }
    return static_cast<sptr_t>(w.size());
}

// RUNCOMMAND_USER messages: size 0 means "large enough" (compatibility);
// TRUE when copied.
sptr_t copyRunVariable(const std::wstring& w, uptr_t size, sptr_t buf)
{
    if (!buf || (size != 0 && w.size() >= size))
        return FALSE;
    std::wmemcpy(reinterpret_cast<wchar_t*>(buf), w.c_str(), w.size() + 1);
    return TRUE;
}

std::string str(sptr_t wide)
{
    return fromWide(reinterpret_cast<const wchar_t*>(wide));
}

std::string str(uptr_t wide)
{
    return fromWide(reinterpret_cast<const wchar_t*>(wide));
}

// UniMode (NPPM_GETBUFFERENCODING) <-> the port's encodings.
int uniMode(const platform::TextEncoding& e)
{
    switch (e.bom) {
    case platform::Bom::Utf8: return 1;
    case platform::Bom::Utf16BE: return 2;
    case platform::Bom::Utf16LE: return 3;
    case platform::Bom::None: break;
    }
    if (e.charset == "UTF-8")
        return 4;
    if (e.charset == "UTF-16BE")
        return 6;
    if (e.charset == "UTF-16LE")
        return 7;
    return 0; // a legacy code page: upstream "ANSI"
}

std::optional<platform::TextEncoding> encodingForUniMode(int mode)
{
    using platform::Bom;
    switch (mode) {
    case 0: case 4: case 5: return platform::TextEncoding{"UTF-8", Bom::None}; // ANSI is UTF-8 on Linux
    case 1: return platform::TextEncoding{"UTF-8", Bom::Utf8};
    case 2: return platform::TextEncoding{"UTF-16BE", Bom::Utf16BE};
    case 3: return platform::TextEncoding{"UTF-16LE", Bom::Utf16LE};
    case 6: return platform::TextEncoding{"UTF-16BE", Bom::None};
    case 7: return platform::TextEncoding{"UTF-16LE", Bom::None};
    default: return std::nullopt;
    }
}

// The file name around the caret (upstream NPPM_GETFILENAMEATCURSOR).
std::string fileNameAtCursor(const EditorView& v)
{
    std::string sel = v.selectedText();
    if (sel.empty()) {
        const sptr_t p = v.currentPos();
        sel = v.sci().range(v.execute(SCI_WORDSTARTPOSITION, static_cast<uptr_t>(p), true),
                            v.execute(SCI_WORDENDPOSITION, static_cast<uptr_t>(p), true));
    }
    if (sel.find_first_of("\\/") != std::string::npos)
        return sel;
    const sptr_t line = v.lineFromPos(v.currentPos());
    const std::string text = v.sci().lineText(line);
    std::size_t col = static_cast<std::size_t>(v.currentPos() - v.execute(SCI_POSITIONFROMLINE, static_cast<uptr_t>(line)));
    col = std::min(col, text.size());
    std::size_t start = col;
    while (start > 0 && std::strchr(" \t[(\"<>", text[start - 1]) == nullptr)
        --start;
    std::size_t end = col;
    while (end < text.size() && std::strchr(" \t:()[]<>\"\r\n", text[end]) == nullptr)
        ++end;
    return text.substr(start, end - start);
}

std::string panelTitle(const wchar_t* name, const wchar_t* addInfo, unsigned mask)
{
    std::string t = fromWide(name);
    if ((mask & DWS_ADDINFO) && addInfo && addInfo[0])
        t += " - " + fromWide(addInfo);
    return t;
}

} // namespace

// ============================================================================
// Loading, menu, keys
// ============================================================================

void MainWindow::loadPlugins()
{
    loadPlugins(PluginManager::defaultDirs());
}

void MainWindow::loadPlugins(const std::vector<fs::path>& dirs)
{
    if (plugins_)
        return;
    PluginManager::Host host;
    host.nppMessage = [this](unsigned msg, uptr_t w, sptr_t l) { return pluginMessage(msg, w, l); };
    host.viewEditor = [this](int view) -> EditorView* {
        Tab* t = closing_ ? nullptr : currentTabOf(view);
        return t ? t->view.get() : nullptr;
    };
    host.window = [this] { return GTK_WINDOW(window_); };
    plugins_ = std::make_unique<PluginManager>(std::move(host));
    std::error_code ec;
    fs::create_directories(platform::userConfigDir() / "plugins" / "config", ec);

    pluginErrors_ = plugins_->load(dirs);
    for (const std::string& e : pluginErrors_)
        g_warning("Plugin not loaded: %s", e.c_str());

    for (const PluginManager::Command& c : plugins_->commands()) {
        const int id = c.cmdId;
        pluginChecked_[id] = c.initChecked;
        cmds_.add(id, [this, id] { plugins_->run(id); },
                  c.initChecked ? CommandTable::State([this, id] { return pluginChecked_[id]; }) : CommandTable::State());
    }
    cmds_.install(G_ACTION_MAP(window_));
    refreshPluginMenu();
    applyShortcuts();
    notifyPlugins(NPPN_TBMODIFICATION);
}

void MainWindow::sendPluginCommandLine(const std::string& message)
{
    if (!plugins_ || message.empty())
        return;
    pluginMessageArg_ = toWide(message);
    notifyPlugins(NPPN_CMDLINEPLUGINMSG, plugins_->nppHandle(), reinterpret_cast<uptr_t>(pluginMessageArg_.c_str()));
}

void MainWindow::pluginsReady()
{
    if (!plugins_)
        return;
    notifyPlugins(NPPN_READY);
    checkBufferActivated();
    // Upstream reopens the plugin panels of the last session by running the
    // plugin command whose index is the panel's dlgID.
    for (const std::string& id : dock_->pendingShown()) {
        if (id.rfind(kPluginPanelPrefix, 0) != 0)
            continue;
        const std::string rest = id.substr(std::strlen(kPluginPanelPrefix));
        const std::size_t colon = rest.rfind(':');
        if (colon == std::string::npos)
            continue;
        const std::string module = rest.substr(0, colon);
        const int dlgId = std::atoi(rest.c_str() + colon + 1);
        for (const PluginManager::Command& c : plugins_->commands())
            if (c.internalId == dlgId && strcasecmp(plugins_->pluginOf(c)->moduleName.c_str(), module.c_str()) == 0) {
                if (!dock_->has(id))
                    cmds_.run(c.cmdId);
                break;
            }
    }
}

void MainWindow::refreshPluginMenu()
{
    if (!dyn_.plugins || !plugins_)
        return;
    g_menu_remove_all(dyn_.plugins);
    const auto& list = plugins_->plugins();
    for (std::size_t i = 0; i < list.size(); ++i) {
        const PluginManager::Plugin& p = *list[i];
        GMenu* sub = g_menu_new();
        GMenu* sec = g_menu_new();
        g_menu_append_section(sub, nullptr, G_MENU_MODEL(sec));
        for (int j = 0; j < p.nbItems; ++j) {
            const FuncItem& it = p.items[j];
            if (!it._pFunc) { // upstream: a separator
                g_object_unref(sec);
                sec = g_menu_new();
                g_menu_append_section(sub, nullptr, G_MENU_MODEL(sec));
                continue;
            }
            if (const PluginManager::Command* c = plugins_->command(it._cmdID); c && c->plugin == i)
                appendItem(sec, core::Translator::toMnemonic(c->name), CommandTable::detailedName(c->cmdId));
        }
        g_object_unref(sec);
        g_menu_append_submenu(dyn_.plugins, core::Translator::toMnemonic(p.displayName).c_str(), G_MENU_MODEL(sub));
        g_object_unref(sub);
    }
}

core::KeyCombo MainWindow::pluginKey(const PluginManager::Command& c) const
{
    if (const core::PluginCommandKey* k = shortcuts_.pluginKey(plugins_->pluginOf(c)->moduleName, c.internalId))
        return k->key;
    return c.defaultKey;
}

std::vector<MainWindow::PluginCommandInfo> MainWindow::pluginCommandList() const
{
    std::vector<PluginCommandInfo> out;
    if (!plugins_)
        return out;
    for (const PluginManager::Command& c : plugins_->commands()) {
        const PluginManager::Plugin* p = plugins_->pluginOf(c);
        out.push_back({c.cmdId, c.name, p->displayName, p->moduleName, c.internalId, pluginKey(c)});
    }
    return out;
}

void MainWindow::notifyShortcutRemapped(int cmdId)
{
    if (!plugins_)
        return;
    const PluginManager::Command* c = plugins_->command(cmdId);
    if (!c)
        return;
    const core::KeyCombo k = pluginKey(*c);
    ShortcutKey sk{k.ctrl, k.alt, k.shift, static_cast<UCHAR>(k.vk)};
    notifyPlugins(NPPN_SHORTCUTREMAPPED, &sk, static_cast<uptr_t>(cmdId));
}

void MainWindow::setPluginCheck(int cmdId, bool on)
{
    pluginChecked_[cmdId] = on;
    if (!cmds_.isCheckable(cmdId)) {
        cmds_.makeCheckable(cmdId, [this, cmdId] { return pluginChecked_[cmdId]; }, G_ACTION_MAP(window_));
        refreshPluginToolbarButton(cmdId);
    }
    cmds_.syncStates();
}

// ============================================================================
// Notifications
// ============================================================================

void MainWindow::notifyPlugins(unsigned code, const void* hwndFrom, uptr_t idFrom)
{
    if (plugins_)
        plugins_->notify(code, hwndFrom, idFrom);
}

void MainWindow::notifyPlugins(unsigned code, const core::Buffer* b)
{
    if (plugins_)
        plugins_->notify(code, plugins_->nppHandle(), reinterpret_cast<uptr_t>(b));
}

void MainWindow::forwardScintillaNotification(const Tab& t, const SCNotification& n)
{
    // Plugins hear from the two editors they know: the document each view shows.
    if (!plugins_ || closing_ || currentTabOf(t.viewIdx) != &t)
        return;
    SCNotification copy = n;
    copy.nmhdr.hwndFrom = plugins_->viewHandle(t.viewIdx);
    plugins_->notify(copy);
}

void MainWindow::checkBufferActivated()
{
    if (!plugins_ || closing_)
        return;
    const core::Buffer* b = activeBuffer();
    if (b && b != activatedBuffer_) {
        activatedBuffer_ = b;
        notifyPlugins(NPPN_BUFFERACTIVATED, b);
    }
}

void MainWindow::shutdownPlugins()
{
    if (!plugins_ || pluginsShutDown_)
        return;
    pluginsShutDown_ = true;
    // Upstream closes the documents before NPPN_SHUTDOWN.
    for (core::Buffer* b : buffers()) {
        notifyPlugins(NPPN_FILEBEFORECLOSE, b);
        notifyPlugins(NPPN_FILECLOSED, b);
    }
    notifyPlugins(NPPN_SHUTDOWN);
}

// ============================================================================
// Toolbar
// ============================================================================

void MainWindow::addPluginToolbarButton(int cmdId, const char* icon, const char* darkIcon)
{
    if (!plugins_ || !plugins_->command(cmdId) || pluginButtons_.count(cmdId))
        return;
    if (pluginButtons_.empty()) // upstream: plugin buttons after a separator
        gtk_toolbar_insert(GTK_TOOLBAR(toolbar_), gtk_separator_tool_item_new(), -1);
    pluginIcons_[cmdId] = {icon ? icon : "", darkIcon ? darkIcon : ""};
    pluginButtons_[cmdId] = nullptr;
    refreshPluginToolbarButton(cmdId);
}

// (Re)creates a plugin's button: a toggle button once the command has a check mark.
void MainWindow::refreshPluginToolbarButton(int cmdId)
{
    auto it = pluginButtons_.find(cmdId);
    if (it == pluginButtons_.end())
        return;
    const PluginManager::Command* c = plugins_->command(cmdId);
    int pos = -1;
    if (it->second) {
        pos = gtk_toolbar_get_item_index(GTK_TOOLBAR(toolbar_), it->second);
        gtk_widget_destroy(GTK_WIDGET(it->second));
    }
    const auto& [light, dark] = pluginIcons_[cmdId];
    const std::string& icon = darkTheme_ && !dark.empty() ? dark : light;
    GtkWidget* image = nullptr;
    std::error_code ec;
    if (!icon.empty() && icon.find('/') != std::string::npos && fs::is_regular_file(icon, ec)) {
        if (GdkPixbuf* pb = gdk_pixbuf_new_from_file_at_size(icon.c_str(), 16, 16, nullptr)) {
            image = gtk_image_new_from_pixbuf(pb);
            g_object_unref(pb);
        }
    } else if (!icon.empty()) {
        image = gtk_image_new_from_icon_name(icon.c_str(), GTK_ICON_SIZE_SMALL_TOOLBAR);
    }
    if (!image)
        image = gtk_image_new_from_icon_name("application-x-addon", GTK_ICON_SIZE_SMALL_TOOLBAR);
    gtk_widget_show(image);
    GtkToolItem* ti = cmds_.isCheckable(cmdId) ? gtk_toggle_tool_button_new() : gtk_tool_button_new(nullptr, nullptr);
    gtk_tool_button_set_icon_widget(GTK_TOOL_BUTTON(ti), image);
    gtk_tool_button_set_label(GTK_TOOL_BUTTON(ti), c->name.c_str());
    gtk_widget_set_tooltip_text(GTK_WIDGET(ti), core::Translator::plain(c->name).c_str());
    gtk_actionable_set_action_name(GTK_ACTIONABLE(ti), CommandTable::detailedName(cmdId).c_str());
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar_), ti, pos);
    gtk_widget_show(GTK_WIDGET(ti));
    it->second = ti;
}

// ============================================================================
// Docking panels (upstream DockingManager through NPPM_DMM*)
// ============================================================================

MainWindow::PluginPanel* MainWindow::pluginPanel(const void* widget)
{
    for (PluginPanel& p : pluginPanels_)
        if (p.widget == widget)
            return &p;
    return nullptr;
}

void MainWindow::onPluginPanelShown(const std::string& id, bool shown)
{
    if (shown || hidingPluginPanel_ || closing_ || !plugins_)
        return;
    // Closed by the user: upstream sends DMN_CLOSE to the panel's window. The
    // port has no window to send it to and tells the owning plugin instead,
    // through beNotified (NppGtk.h).
    for (const PluginPanel& p : pluginPanels_)
        if (p.dockId == id) {
            if (const PluginManager::Plugin* pl = plugins_->findModule(p.module)) {
                SCNotification n{};
                n.nmhdr.code = DMN_CLOSE;
                n.nmhdr.hwndFrom = p.widget;
                n.nmhdr.idFrom = static_cast<uptr_t>(p.dlgId);
                plugins_->notify(*pl, n);
            }
            break;
        }
}

sptr_t MainWindow::pluginDockMessage(unsigned msg, uptr_t wParam, sptr_t lParam)
{
    switch (msg) {
    case NPPM_DMMREGASDCKDLG: {
        auto* d = reinterpret_cast<DockedWidgetData*>(lParam);
        if (!d || !d->hClient || !GTK_IS_WIDGET(d->hClient))
            return FALSE;
        if (pluginPanel(d->hClient))
            return TRUE;
        PluginPanel p;
        p.module = d->pszModuleName ? fromWide(d->pszModuleName) : std::string();
        if (p.module.empty() && plugins_->calling())
            p.module = plugins_->calling()->moduleName;
        p.dlgId = d->dlgID;
        p.dockId = kPluginPanelPrefix + p.module + ":" + std::to_string(p.dlgId);
        p.widget = GTK_WIDGET(d->hClient);
        p.name = d->pszName;
        p.addInfo = d->pszAddInfo;
        p.mask = d->uMask;
        DockManager::Area area = (d->uMask & DWS_DF_FLOATING) ? DockManager::Area::Float
                                                             : static_cast<DockManager::Area>((d->uMask >> 28) & 3);
        if (dock_->has(p.dockId)) // the same panel id from another widget
            return FALSE;
        dock_->add(p.dockId, panelTitle(p.name, p.addInfo, p.mask), p.widget, area);
        pluginPanels_.push_back(std::move(p));
        return TRUE;
    }
    case NPPM_DMMSHOW:
        if (PluginPanel* p = pluginPanel(reinterpret_cast<const void*>(lParam)))
            dock_->show(p->dockId);
        return TRUE;
    case NPPM_DMMHIDE:
        if (PluginPanel* p = pluginPanel(reinterpret_cast<const void*>(lParam))) {
            hidingPluginPanel_ = true;
            dock_->hide(p->dockId);
            hidingPluginPanel_ = false;
        }
        return TRUE;
    case NPPM_DMMUPDATEDISPINFO:
        if (PluginPanel* p = pluginPanel(reinterpret_cast<const void*>(lParam)))
            dock_->setTitle(p->dockId, panelTitle(p->name, p->addInfo, p->mask));
        return TRUE;
    case NPPM_DMMVIEWOTHERTAB: {
        const std::string name = str(lParam);
        for (const PluginPanel& p : pluginPanels_)
            if (fromWide(p.name) == name) {
                dock_->show(p.dockId);
                break;
            }
        return TRUE;
    }
    case NPPM_DMMGETPLUGINHWNDBYNAME: {
        if (!lParam)
            return 0;
        const std::string module = str(lParam);
        const std::string window = wParam ? str(wParam) : std::string();
        for (const PluginPanel& p : pluginPanels_)
            if (strcasecmp(p.module.c_str(), module.c_str()) == 0 && (window.empty() || fromWide(p.name) == window))
                return reinterpret_cast<sptr_t>(p.widget);
        return 0;
    }
    default:
        return 0;
    }
}

// ============================================================================
// Documents by buffer ID and language type
// ============================================================================

core::Buffer* MainWindow::bufferFromId(uptr_t id) const
{
    for (core::Buffer* b : buffers())
        if (reinterpret_cast<uptr_t>(b) == id)
            return b;
    return nullptr;
}

int MainWindow::langTypeOf(const core::Buffer& b) const
{
    if (b.userLanguage)
        return L_USER;
    if (!b.language)
        return L_TEXT;
    for (std::size_t i = 0; i < std::size(gen::kLangTable); ++i)
        if (b.language->name == gen::kLangTable[i].name)
            return static_cast<int>(i);
    return L_TEXT;
}

bool MainWindow::setLangType(core::Buffer& b, int langType)
{
    if (langType < L_TEXT || langType >= L_EXTERNAL || langType == L_USER || langType >= static_cast<int>(std::size(gen::kLangTable)))
        return false;
    const core::Language* lang = langs_.byName(gen::kLangTable[langType].name);
    if (!lang)
        lang = &langs_.plainText();
    setLanguage(b, *lang);
    return true;
}

// ============================================================================
// The messages
// ============================================================================

sptr_t MainWindow::pluginMessage(unsigned msg, uptr_t wParam, sptr_t lParam)
{
    if (!plugins_) // -noPlugin: nobody can send anything
        return 0;
    if (msg == WM_COMMAND) // ::SendMessage(nppHandle, WM_COMMAND, IDM_..., 0)
        return cmds_.run(LOWORD(wParam)) ? 0 : 1;
    if (msg >= NPPM_DMMSHOW && msg <= NPPM_DMMREGASDCKDLG)
        return pluginDockMessage(msg, wParam, lParam);
    if (msg == NPPM_DMMVIEWOTHERTAB || msg == NPPM_DMMGETPLUGINHWNDBYNAME)
        return pluginDockMessage(msg, wParam, lParam);

    core::Buffer* active = activeBuffer();
    EditorView* view = activeView();
    auto byPath = [this](sptr_t wide) -> core::Buffer* {
        if (!wide)
            return nullptr;
        const fs::path p = str(wide);
        std::error_code ec;
        const fs::path n = fs::weakly_canonical(p, ec);
        return findByPath(ec ? p : n);
    };
    auto view2 = [this](uptr_t v) -> int { return v == SUB_VIEW ? kSubView : kMainView; };

    switch (msg) {
    // ---- Views and documents --------------------------------------------------
    case NPPM_GETCURRENTSCINTILLA:
        if (!lParam)
            return FALSE;
        *reinterpret_cast<int*>(lParam) = activeView_;
        return TRUE;
    case NPPM_GETCURRENTVIEW:
        return activeView_;
    case NPPM_GETCURRENTLANGTYPE:
        if (!lParam)
            return FALSE;
        *reinterpret_cast<int*>(lParam) = active ? langTypeOf(*active) : L_TEXT;
        return TRUE;
    case NPPM_SETCURRENTLANGTYPE:
        if (active)
            setLangType(*active, static_cast<int>(lParam));
        return TRUE;
    case NPPM_GETNBOPENFILES:
        switch (lParam) {
        case ALL_OPEN_FILES: return static_cast<sptr_t>(tabCount(kMainView) + tabCount(kSubView));
        case PRIMARY_VIEW: return static_cast<sptr_t>(tabCount(kMainView));
        case SECOND_VIEW: return static_cast<sptr_t>(tabCount(kSubView));
        default: return 0;
        }
    case NPPM_GETOPENFILENAMES_DEPRECATED:
    case NPPM_GETOPENFILENAMESPRIMARY_DEPRECATED:
    case NPPM_GETOPENFILENAMESSECOND_DEPRECATED: {
        if (!wParam)
            return 0;
        auto** names = reinterpret_cast<wchar_t**>(wParam);
        const auto max = static_cast<std::size_t>(lParam);
        std::size_t j = 0;
        for (int vi : {kMainView, kSubView}) {
            if ((vi == kMainView && msg == NPPM_GETOPENFILENAMESSECOND_DEPRECATED) || (vi == kSubView && msg == NPPM_GETOPENFILENAMESPRIMARY_DEPRECATED))
                continue;
            for (Tab* t : tabsInOrder(vi))
                if (j < max)
                    std::wcscpy(names[j++], toWide(t->buf->fullName()).c_str());
        }
        return static_cast<sptr_t>(j);
    }
    case NPPM_GETCURRENTDOCINDEX: {
        const int vi = view2(static_cast<uptr_t>(lParam));
        Tab* t = currentTabOf(vi);
        if (!t || !gtk_widget_get_visible(notebooks_[vi]))
            return -1;
        const std::vector<Tab*> tabs = tabsInOrder(vi);
        return std::find(tabs.begin(), tabs.end(), t) - tabs.begin();
    }
    case NPPM_ACTIVATEDOC:
    case NPPM_TRIGGERTABBARCONTEXTMENU: {
        const int vi = (wParam != MAIN_VIEW && wParam != SUB_VIEW) ? activeView_ : view2(wParam);
        const std::vector<Tab*> tabs = tabsInOrder(vi);
        if (lParam >= 0 && lParam < static_cast<sptr_t>(tabs.size())) {
            Tab* t = tabs[static_cast<std::size_t>(lParam)];
            if (msg == NPPM_TRIGGERTABBARCONTEXTMENU)
                showTabMenu(*t, nullptr);
            else
                activateTab(*t);
        }
        return TRUE;
    }
    case NPPM_GETCURRENTBUFFERID:
        return reinterpret_cast<sptr_t>(active);
    case NPPM_GETBUFFERIDFROMPOS: {
        if (lParam != MAIN_VIEW && lParam != SUB_VIEW)
            return 0;
        const std::vector<Tab*> tabs = tabsInOrder(view2(static_cast<uptr_t>(lParam)));
        return wParam < tabs.size() ? reinterpret_cast<sptr_t>(tabs[wParam]->buf.get()) : 0;
    }
    case NPPM_GETPOSFROMBUFFERID: {
        core::Buffer* b = bufferFromId(wParam);
        if (!b)
            return -1;
        const int first = lParam == SUB_VIEW ? kSubView : kMainView;
        for (int vi : {first, 1 - first}) {
            const std::vector<Tab*> tabs = tabsInOrder(vi);
            for (std::size_t i = 0; i < tabs.size(); ++i)
                if (tabs[i]->buf.get() == b)
                    return (static_cast<sptr_t>(vi) << 30) | static_cast<sptr_t>(i);
        }
        return -1;
    }
    case NPPM_GETFULLPATHFROMBUFFERID: {
        core::Buffer* b = bufferFromId(wParam);
        if (!b)
            return FALSE; // as upstream's code (its header says -1)
        const std::wstring w = toWide(b->fullName());
        if (lParam)
            std::wmemcpy(reinterpret_cast<wchar_t*>(lParam), w.c_str(), w.size() + 1);
        return static_cast<sptr_t>(w.size());
    }
    case NPPM_SWITCHTOFILE: {
        core::Buffer* b = byPath(lParam);
        if (!b)
            return FALSE;
        Tab* t = tabOf(*b, activeView_) ? tabOf(*b, activeView_) : tabsOf(*b).front();
        activateTab(*t);
        return TRUE;
    }
    case NPPM_DOOPEN: {
        if (!lParam)
            return FALSE;
        openFiles({fs::path(str(lParam))});
        return byPath(lParam) ? TRUE : FALSE;
    }
    case NPPM_RELOADFILE:
    case NPPM_RELOADBUFFERID: {
        core::Buffer* b = msg == NPPM_RELOADFILE ? byPath(lParam) : bufferFromId(wParam);
        const bool alert = msg == NPPM_RELOADFILE ? wParam != 0 : lParam != 0;
        if (!b || b->isUntitled())
            return FALSE;
        if (alert && ask(trMessage("DocReloadWarning", "message", "Are you sure you want to reload the current file and lose the changes made in Notepad++?"),
                         b->path.string(), false) != Answer::Yes)
            return FALSE;
        std::string err;
        return reloadBuffer(*b, &err) ? TRUE : FALSE;
    }
    case NPPM_MAKECURRENTBUFFERDIRTY:
        if (active)
            markForcedDirty(*active, true);
        return TRUE;
    case NPPM_SETUNTITLEDNAME: {
        core::Buffer* b = static_cast<sptr_t>(wParam) == -1 ? active : bufferFromId(wParam);
        if (!b || !b->isUntitled() || !lParam)
            return FALSE;
        const std::string name = str(lParam);
        if (name.empty())
            return FALSE;
        b->untitledName = name;
        refreshBufferTabs(*b);
        refreshTitle();
        return TRUE;
    }
    case NPPM_GETTABCOLORID:
        return -1; // the port's tabs have no colours

    // ---- Saving -----------------------------------------------------------------
    case NPPM_SAVECURRENTFILE:
        return active && isModified(*active) && save(*active, false) ? TRUE : FALSE;
    case NPPM_SAVEALLFILES: {
        bool any = false;
        for (core::Buffer* b : buffers())
            if (isModified(*b) && save(*b, false))
                any = true;
        return any ? TRUE : FALSE;
    }
    case NPPM_SAVECURRENTFILEAS: {
        if (!lParam || !active)
            return FALSE;
        const fs::path p = str(lParam);
        if (wParam == TRUE) { // a copy: the document keeps its name
            std::string err;
            bool unrepresentable = false;
            return core::saveFile(p, view->text(), active->encoding, &err, &unrepresentable) ? TRUE : FALSE;
        }
        return save(*active, true, p) ? TRUE : FALSE;
    }
    case NPPM_SAVEFILE: {
        core::Buffer* b = byPath(lParam);
        return b && save(*b, false) ? TRUE : FALSE;
    }

    // ---- Buffers ----------------------------------------------------------------
    case NPPM_GETBUFFERLANGTYPE: {
        core::Buffer* b = bufferFromId(wParam);
        return b ? langTypeOf(*b) : -1;
    }
    case NPPM_SETBUFFERLANGTYPE: {
        core::Buffer* b = bufferFromId(wParam);
        return b && setLangType(*b, static_cast<int>(lParam)) ? TRUE : FALSE;
    }
    case NPPM_GETBUFFERENCODING: {
        core::Buffer* b = bufferFromId(wParam);
        return b ? uniMode(b->encoding) : -1;
    }
    case NPPM_SETBUFFERENCODING: {
        core::Buffer* b = bufferFromId(wParam);
        const auto e = encodingForUniMode(static_cast<int>(lParam));
        // Upstream: only a new, unedited document.
        if (!b || !e || !b->isUntitled() || isModified(*b))
            return FALSE;
        b->encoding = *e;
        if (b == active)
            refreshStatus();
        return TRUE;
    }
    case NPPM_GETBUFFERFORMAT: {
        core::Buffer* b = bufferFromId(wParam);
        if (!b)
            return -1;
        return tabsOf(*b).front()->view->execute(SCI_GETEOLMODE); // SC_EOL_* has upstream's EolType numbers
    }
    case NPPM_SETBUFFERFORMAT: {
        core::Buffer* b = bufferFromId(wParam);
        if (!b || lParam < SC_EOL_CRLF || lParam > SC_EOL_LF)
            return FALSE;
        for (Tab* t : tabsOf(*b))
            t->view->setEolMode(static_cast<int>(lParam), false);
        if (b == active)
            refreshStatus();
        return TRUE;
    }

    // ---- Sessions ---------------------------------------------------------------
    case NPPM_LOADSESSION:
        if (lParam)
            loadSession(str(lParam));
        return TRUE;
    case NPPM_SAVECURRENTSESSION:
        return lParam && saveSession(str(lParam)) ? lParam : 0;
    case NPPM_SAVESESSION: {
        if (!lParam)
            return FALSE;
        const auto* si = reinterpret_cast<const sessionInfo*>(lParam);
        if (!si->sessionFilePathName)
            return 0;
        core::Session s;
        for (int i = 0; i < si->nbFile; ++i)
            if (si->files && si->files[i]) {
                core::SessionFile f;
                f.filename = fromWide(si->files[i]);
                s.views[0].push_back(std::move(f));
            }
        return core::writeSession(fromWide(si->sessionFilePathName), s) ? reinterpret_cast<sptr_t>(si->sessionFilePathName) : 0;
    }
    case NPPM_GETNBSESSIONFILES:
    case NPPM_GETSESSIONFILES: {
        auto* valid = msg == NPPM_GETNBSESSIONFILES ? reinterpret_cast<BOOL*>(wParam) : nullptr;
        if (valid)
            *valid = FALSE;
        if (!lParam || !reinterpret_cast<const wchar_t*>(lParam)[0])
            return msg == NPPM_GETNBSESSIONFILES ? 0 : FALSE;
        core::Session s;
        if (!core::readSession(str(lParam), s))
            return msg == NPPM_GETNBSESSIONFILES ? 0 : FALSE;
        if (msg == NPPM_GETNBSESSIONFILES) {
            if (valid)
                *valid = TRUE;
            return static_cast<sptr_t>(s.views[0].size() + s.views[1].size());
        }
        auto** out = reinterpret_cast<wchar_t**>(wParam);
        if (!out)
            return FALSE;
        std::size_t i = 0;
        for (const auto& v : s.views)
            for (const core::SessionFile& f : v)
                std::wcscpy(out[i++], toWide(f.filename).c_str());
        return TRUE;
    }

    // ---- Commands, menus, shortcuts -----------------------------------------------
    case NPPM_MENUCOMMAND:
        cmds_.run(static_cast<int>(lParam));
        return TRUE;
    case NPPM_SETMENUITEMCHECK:
        if (plugins_->command(static_cast<int>(wParam)))
            setPluginCheck(static_cast<int>(wParam), lParam != 0);
        return TRUE;
    case NPPM_GETMENUHANDLE:
        return 0; // GTK menus are not HMENUs
    case NPPM_GETSHORTCUTBYCMDID: {
        const PluginManager::Command* c = plugins_->command(static_cast<int>(wParam));
        if (!c || !lParam)
            return FALSE;
        const core::KeyCombo k = pluginKey(*c);
        auto* sk = reinterpret_cast<ShortcutKey*>(lParam);
        *sk = ShortcutKey{k.ctrl, k.alt, k.shift, static_cast<UCHAR>(k.vk)};
        return k.empty() ? FALSE : TRUE;
    }
    case NPPM_REMOVESHORTCUTBYCMDID: {
        const PluginManager::Command* c = plugins_->command(static_cast<int>(wParam));
        if (!c)
            return FALSE;
        shortcuts_.setPluginKey(plugins_->pluginOf(*c)->moduleName, c->internalId, {});
        std::string err;
        if (!saveShortcuts(&err))
            g_warning("Cannot save shortcuts.xml: %s", err.c_str());
        applyShortcuts();
        return TRUE;
    }
    case NPPM_ALLOCATESUPPORTED_DEPRECATED:
        return TRUE;
    case NPPM_ALLOCATECMDID:
    case NPPM_ALLOCATEMARKER:
    case NPPM_ALLOCATEINDICATOR: {
        if (!lParam)
            return FALSE;
        const int n = static_cast<int>(wParam);
        const int start = msg == NPPM_ALLOCATECMDID ? plugins_->allocateCmdIds(n)
                        : msg == NPPM_ALLOCATEMARKER ? plugins_->allocateMarkers(n) : plugins_->allocateIndicators(n);
        if (start < 0) {
            if (msg != NPPM_ALLOCATEINDICATOR) // upstream leaves an indicator start untouched
                *reinterpret_cast<int*>(lParam) = 0;
            return FALSE;
        }
        *reinterpret_cast<int*>(lParam) = start;
        return TRUE;
    }
    case NPPM_MSGTOPLUGIN:
        if (!wParam || !reinterpret_cast<const wchar_t*>(wParam)[0] || !lParam)
            return FALSE;
        return plugins_->relay(str(wParam), msg, wParam, lParam) ? TRUE : FALSE;
    case NPPM_GETCURRENTMACROSTATUS:
        if (recording_)
            return static_cast<sptr_t>(MacroStatus::RecordInProgress);
        if (playing_)
            return static_cast<sptr_t>(MacroStatus::PlayingBack);
        return static_cast<sptr_t>(macro_.empty() ? MacroStatus::Idle : MacroStatus::RecordingStopped);

    // ---- Toolbar ------------------------------------------------------------------
    case NPPM_ADDTOOLBARICON_FORDARKMODE: {
        const auto* icons = reinterpret_cast<const toolbarIconsWithDarkMode*>(lParam);
        if (!icons)
            return FALSE;
        // NppGtk.h: the handles carry an icon name or an image path.
        const char* light = reinterpret_cast<const char*>(icons->hToolbarIcon ? static_cast<const void*>(icons->hToolbarIcon)
                                                                               : static_cast<const void*>(icons->hToolbarBmp));
        addPluginToolbarButton(static_cast<int>(wParam), light, reinterpret_cast<const char*>(icons->hToolbarIconDarkMode));
        return TRUE;
    }
    case NPPM_ADDTOOLBARICON_DEPRECATED: {
        const auto* icons = reinterpret_cast<const toolbarIcons*>(lParam);
        if (!icons)
            return FALSE;
        const char* light = reinterpret_cast<const char*>(icons->hToolbarIcon ? static_cast<const void*>(icons->hToolbarIcon)
                                                                               : static_cast<const void*>(icons->hToolbarBmp));
        addPluginToolbarButton(static_cast<int>(wParam), light, nullptr);
        return TRUE;
    }
    case NPPM_GETTOOLBARICONSETCHOICE:
        return settings_.toolbarIconSet;

    // ---- Window parts ---------------------------------------------------------------
    case NPPM_HIDETABBAR: {
        const bool old = tabBarHidden_;
        tabBarHidden_ = lParam != 0;
        for (GtkWidget* nb : notebooks_)
            gtk_notebook_set_show_tabs(GTK_NOTEBOOK(nb), !tabBarHidden_ && !settings_.tabHide);
        return old;
    }
    case NPPM_ISTABBARHIDDEN:
        return tabBarHidden_ || settings_.tabHide;
    case NPPM_HIDETOOLBAR:
    case NPPM_HIDEMENU:
    case NPPM_HIDESTATUSBAR: {
        GtkWidget* w = msg == NPPM_HIDETOOLBAR ? toolbar_ : msg == NPPM_HIDEMENU ? menubar_ : statusBar_;
        const bool wasHidden = !gtk_widget_get_visible(w);
        gtk_widget_set_visible(w, lParam != TRUE);
        if (msg == NPPM_HIDETOOLBAR)
            settings_.hideToolbar = lParam == TRUE;
        else if (msg == NPPM_HIDESTATUSBAR)
            settings_.hideStatusBar = lParam == TRUE;
        else
            settings_.hideMenu = lParam == TRUE;
        return wasHidden;
    }
    case NPPM_ISTOOLBARHIDDEN:
        return !gtk_widget_get_visible(toolbar_);
    case NPPM_ISMENUHIDDEN:
        return !gtk_widget_get_visible(menubar_);
    case NPPM_ISSTATUSBARHIDDEN:
        return !gtk_widget_get_visible(statusBar_);
    case NPPM_SETSTATUSBAR: {
        const auto* s = reinterpret_cast<const wchar_t*>(lParam);
        GtkWidget* fields[] = {stLang_, stStats_, stPos_, stEol_, stEnc_, stIns_};
        if (!s || !s[0] || wParam > STATUSBAR_TYPING_MODE)
            return FALSE;
        gtk_label_set_text(GTK_LABEL(fields[wParam]), fromWide(s).c_str());
        return TRUE;
    }
    case NPPM_SHOWDOCLIST:
        if (lParam)
            dock_->show("documentList", false);
        else
            dock_->hide("documentList");
        return TRUE;
    case NPPM_ISDOCLISTSHOWN:
        return dock_->isShown("documentList");
    case NPPM_DOCLISTDISABLEEXTCOLUMN:
    case NPPM_DOCLISTDISABLEPATHCOLUMN:
    case NPPM_SETEDITORBORDEREDGE:
    case NPPM_DISABLEAUTOUPDATE:
        return TRUE;
    case NPPM_LAUNCHFINDINFILESDLG:
        if (wParam)
            findDlg_->setDirectory(str(wParam));
        if (lParam)
            findDlg_->setFilters(str(lParam));
        findDlg_->show(FindDialog::Tab::FindInFiles);
        return TRUE;
    case NPPM_MODELESSDIALOG:
        return lParam; // GTK dialogs handle their own keys
    case NPPM_GTKGETMAINWINDOW:
        return reinterpret_cast<sptr_t>(window_);
    case NPPM_GTKGETSCINTILLAWIDGET: {
        const void* h = reinterpret_cast<const void*>(lParam);
        if (h == plugins_->viewHandle(kMainView) || h == plugins_->viewHandle(kSubView)) {
            Tab* t = currentTabOf(h == plugins_->viewHandle(kMainView) ? kMainView : kSubView);
            return t ? reinterpret_cast<sptr_t>(t->view->widget()) : 0;
        }
        return PluginManager::isHandle(h) && h != plugins_->nppHandle() ? lParam : 0;
    }
    case NPPM_GTKGETPORTVERSION:
        return NPPGTK_INTERFACE_VERSION;
    case NPPM_GTKGETSAVETARGET: {
        core::Buffer* b = bufferFromId(wParam);
        if (!b || b != savingBuffer_)
            return 0;
        const std::wstring w = toWide(saveTarget_.string());
        if (lParam)
            std::wmemcpy(reinterpret_cast<wchar_t*>(lParam), w.c_str(), w.size() + 1);
        return static_cast<sptr_t>(w.size());
    }
    case NPPM_GTKWRITEBUFFER: {
        core::Buffer* b = bufferFromId(wParam);
        if (!b || !lParam || tabsOf(*b).empty())
            return FALSE;
        std::string err;
        bool unrepresentable = false;
        return core::saveFile(fs::path(str(lParam)), tabsOf(*b).front()->view->text(), b->encoding, &err, &unrepresentable) ? TRUE : FALSE;
    }
    case NPPM_GTKCANCELSAVE: {
        core::Buffer* b = bufferFromId(wParam);
        if (!b || b != savingBuffer_)
            return FALSE;
        saveCancelled_ = true;
        return TRUE;
    }

    // ---- Editors ----------------------------------------------------------------------
    case NPPM_CREATESCINTILLAHANDLE: {
        GtkWidget* w = plugins_->createScintilla();
        // As upstream's ScintillaEditView::init: Notepad++'s look by default.
        scintilla_send_message(SCINTILLA(w), SCI_SETCODEPAGE, SC_CP_UTF8, 0);
        return reinterpret_cast<sptr_t>(w);
    }
    case NPPM_DESTROYSCINTILLAHANDLE_DEPRECATED:
        return TRUE;
    case NPPM_CREATELEXER:
        return lParam ? reinterpret_cast<sptr_t>(CreateLexer(str(lParam).c_str())) : 0;
    case NPPM_ADDSCNMODIFIEDFLAGS:
        modEventFlags_ |= static_cast<int>(lParam);
        for (auto& t : tabs_)
            t->view->setModEventMask(modEventFlags_);
        return TRUE;
    case NPPM_GETBOOKMARKID:
        return EditorView::kBookmarkMarker;
    case NPPM_SETSMOOTHFONT:
        settings_.smoothFont = lParam != 0;
        applySettings();
        return TRUE;
    case NPPM_SETLINENUMBERWIDTHMODE:
        if (lParam != LINENUMWIDTH_DYNAMIC && lParam != LINENUMWIDTH_CONSTANT)
            return FALSE;
        settings_.lineNumberDynamic = lParam == LINENUMWIDTH_DYNAMIC;
        applySettings();
        return TRUE;
    case NPPM_GETLINENUMBERWIDTHMODE:
        return settings_.lineNumberDynamic ? LINENUMWIDTH_DYNAMIC : LINENUMWIDTH_CONSTANT;
    case NPPM_ISAUTOINDENTON:
        return settings_.autoIndentMode != 0;
    case NPPM_GETEXTERNALLEXERAUTOINDENTMODE:
    case NPPM_SETEXTERNALLEXERAUTOINDENTMODE:
        return FALSE; // no external lexers
    case NPPM_GETEDITORDEFAULTFOREGROUNDCOLOR:
    case NPPM_GETEDITORDEFAULTBACKGROUNDCOLOR: {
        const core::Style* def = styles_.globalById(STYLE_DEFAULT);
        if (msg == NPPM_GETEDITORDEFAULTFOREGROUNDCOLOR)
            return def && def->fg ? *def->fg : 0x000000;
        return def && def->bg ? *def->bg : 0xFFFFFF;
    }
    case NPPM_ISDARKMODEENABLED:
        return darkTheme_;
    case NPPM_GETDARKMODECOLORS: {
        constexpr std::size_t kColours = 12; // NppDarkMode::Colors
        if (wParam != kColours * sizeof(COLORREF) || !lParam)
            return FALSE;
        const core::Style* def = styles_.globalById(STYLE_DEFAULT);
        const COLORREF bg = def && def->bg ? *def->bg : 0x202020;
        const COLORREF fg = def && def->fg ? *def->fg : 0xE0E0E0;
        const COLORREF c[kColours] = {bg, 0x404040, 0x404040, bg, 0x00006E, fg, 0xC0C0C0, 0x808080, 0xFFC000, 0x646464, 0x9B9B9B, 0x484848};
        std::memcpy(reinterpret_cast<void*>(lParam), c, sizeof c);
        return TRUE;
    }
    case NPPM_DARKMODESUBCLASSANDTHEME:
        return static_cast<sptr_t>(wParam); // GTK themes the plugin's widgets already

    // ---- Paths and strings ---------------------------------------------------------------
    case NPPM_GETPLUGINSCONFIGDIR: {
        const std::wstring w = toWide((platform::userConfigDir() / "plugins" / "config").string());
        if (!lParam)
            return static_cast<sptr_t>(w.size());
        if (w.size() >= wParam)
            return 0;
        std::wmemcpy(reinterpret_cast<wchar_t*>(lParam), w.c_str(), w.size() + 1);
        return TRUE; // upstream: TRUE, not the length, once copied
    }
    case NPPM_GETPLUGINHOMEPATH: {
        // The directory the calling plugin was loaded from (there are two).
        fs::path home;
        if (const PluginManager::Plugin* p = plugins_->calling())
            home = p->root;
        else if (!plugins_->plugins().empty())
            home = plugins_->plugins().front()->root;
        else if (const auto dirs = PluginManager::defaultDirs(); !dirs.empty())
            home = dirs.back();
        return copyOrLength(toWide(home.string()), wParam, lParam);
    }
    case NPPM_GETSETTINGSONCLOUDPATH:
        return 0; // not set
    case NPPM_GETNPPSETTINGSDIRPATH:
        return copyOrLength(toWide(platform::userConfigDir().string()), wParam, lParam);
    case NPPM_GETCURRENTCMDLINE:
        return copyOrLength(toWide(cmdLine_), wParam, lParam);
    case NPPM_GETNATIVELANGFILENAME: {
        if (settings_.localization.empty())
            return 0;
        const std::string& f = settings_.localization;
        if (lParam) {
            if (f.size() >= wParam)
                return 0;
            std::memcpy(reinterpret_cast<char*>(lParam), f.c_str(), f.size() + 1);
        }
        return static_cast<sptr_t>(f.size());
    }
    case NPPM_GETCURRENTNATIVELANGENCODING:
        return 65001; // CP_UTF8
    case NPPM_GETLANGUAGENAME:
    case NPPM_GETLANGUAGEDESC: {
        int lt = static_cast<int>(wParam);
        if (lt < L_TEXT || lt >= static_cast<int>(std::size(gen::kLangTable)))
            lt = L_TEXT;
        std::string s = msg == NPPM_GETLANGUAGENAME ? gen::kLangTable[lt].shortName : gen::kLangTable[lt].longName;
        if (lt == L_USER && active && active->userLanguage)
            s += " - " + active->userLanguage->name;
        const std::wstring w = toWide(s);
        if (lParam)
            std::wmemcpy(reinterpret_cast<wchar_t*>(lParam), w.c_str(), w.size() + 1);
        return static_cast<sptr_t>(w.size());
    }
    case NPPM_GETNBUSERLANG:
        if (lParam)
            *reinterpret_cast<int*>(lParam) = IDM_LANG_USER;
        return static_cast<sptr_t>(udls_.size());
    case NPPM_GETNPPVERSION: {
        // "8.9.6.4" -> HIWORD 8, LOWORD 964; ADD_ZERO_PADDING pads "9" to 900.
        const std::string v = NPP_VERSION;
        const std::size_t dot = v.find('.');
        const int major = std::atoi(v.c_str());
        std::string aux;
        if (dot != std::string::npos)
            for (char ch : v.substr(dot + 1))
                if (ch >= '0' && ch <= '9')
                    aux += ch;
        if (aux.size() > 3)
            aux.resize(3);
        if (wParam == TRUE && !aux.empty())
            aux.append(3 - aux.size(), '0');
        return MAKELONG(aux.empty() ? 0 : std::atoi(aux.c_str()), major);
    }
    case NPPM_GETWINDOWSVERSION:
        return WV_UNKNOWN;
    case NPPM_GETAPPDATAPLUGINSALLOWED:
        return TRUE; // the user's plugin directory is always searched
    case NPPM_GETENABLETHEMETEXTUREFUNC_DEPRECATED:
    case NPPM_ENCODESCI:
    case NPPM_DECODESCI:
        return 0;

    // ---- RUNCOMMAND_USER ---------------------------------------------------------------
    case NPPM_GETFULLCURRENTPATH:
    case NPPM_GETCURRENTDIRECTORY:
    case NPPM_GETFILENAME:
    case NPPM_GETNAMEPART:
    case NPPM_GETEXTPART:
    case NPPM_GETCURRENTWORD:
    case NPPM_GETNPPDIRECTORY:
    case NPPM_GETNPPFULLFILEPATH:
    case NPPM_GETCURRENTLINESTR: {
        static const std::pair<unsigned, const char*> vars[] = {
            {NPPM_GETFULLCURRENTPATH, "FULL_CURRENT_PATH"}, {NPPM_GETCURRENTDIRECTORY, "CURRENT_DIRECTORY"},
            {NPPM_GETFILENAME, "FILE_NAME"},                {NPPM_GETNAMEPART, "NAME_PART"},
            {NPPM_GETEXTPART, "EXT_PART"},                  {NPPM_GETCURRENTWORD, "CURRENT_WORD"},
            {NPPM_GETNPPDIRECTORY, "NPP_DIRECTORY"},        {NPPM_GETNPPFULLFILEPATH, "NPP_FULL_FILE_PATH"},
            {NPPM_GETCURRENTLINESTR, "CURRENT_LINESTR"}};
        std::string value;
        for (const auto& [m, name] : vars)
            if (m == msg)
                value = expandRunVariables(std::string("$(") + name + ")");
        return copyRunVariable(toWide(value), wParam, lParam);
    }
    case NPPM_GETFILENAMEATCURSOR:
        return copyRunVariable(toWide(view ? fileNameAtCursor(*view) : std::string()), wParam, lParam);
    case NPPM_GETCURRENTLINE:
        return view ? view->lineFromPos(view->currentPos()) : 0;
    case NPPM_GETCURRENTCOLUMN:
        return view ? view->execute(SCI_GETCOLUMN, static_cast<uptr_t>(view->currentPos())) : 0;

    default:
        return 0; // unknown or not supported on Linux
    }
}

} // namespace npp::ui

// ============================================================================
// Plugins Admin (upstream WinControls/PluginsAdmin), for plugins on disk:
// Installed and Deactivated tabs; install from a file, remove, deactivate
// (moved to <plugins>/disabled/, as upstream) and activate. Upstream's
// Available and Updates tabs download Windows DLLs and are left out.
// ============================================================================

namespace npp::ui {

namespace {

std::string homeShortened(const fs::path& p)
{
    const std::string s = p.string(), home = g_get_home_dir();
    return !home.empty() && s.rfind(home + "/", 0) == 0 ? "~" + s.substr(home.size()) : s;
}

enum AdminCol { kAdmName, kAdmVersion, kAdmLocation, kAdmStatus, kAdmIndex, kAdmPath, kAdmCount };

} // namespace

std::string MainWindow::pluginFolderStatus(const core::PluginFolder& f, const std::vector<core::PluginFolder>& all) const
{
    const PluginManager::Plugin* loaded = nullptr;
    if (plugins_)
        for (const auto& p : plugins_->plugins())
            if (p->root == f.root && p->folder == f.name)
                loaded = p.get();
    if (f.misplaced)
        return "Not loaded: a plugin must be in a folder of its own (" + f.name + "/" + f.name + ".so); Install it, or move it there";
    if (f.disabled)
        return loaded ? "Deactivated, unloaded at the next start" : "Deactivated";
    if (loaded)
        return "Loaded";
    const std::string file = f.file.string();
    for (const std::string& e : pluginErrors_)
        if (e.rfind(file + ": ", 0) == 0)
            return "Not loaded: " + e.substr(file.size() + 2);
    if (!plugins_)
        return "Not loaded (-noPlugin)";
    for (const core::PluginFolder& other : all) {
        if (&other == &f)
            break;
        if (!other.disabled && !other.misplaced && other.root != f.root && strcasecmp(other.name.c_str(), f.name.c_str()) == 0)
            return "Not loaded: " + homeShortened(other.root) + " has a plugin of this name";
    }
    return "Loads at the next start";
}

std::vector<fs::path> MainWindow::choosePluginFiles(GtkWindow* parent)
{
    GtkFileChooserNative* dlg = gtk_file_chooser_native_new(core::Translator::plain(tr_.command(IDM_SETTING_IMPORTPLUGIN, "Import plugin(s)")).c_str(),
                                                            parent, GTK_FILE_CHOOSER_ACTION_OPEN, "_Install", "_Cancel");
    gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dlg), TRUE);
    GtkFileFilter* f = gtk_file_filter_new();
    gtk_file_filter_set_name(f, "Notepad++ plugin (*.so, *.zip)");
    gtk_file_filter_add_pattern(f, "*.so");
    gtk_file_filter_add_pattern(f, "*.zip");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), f);
    std::vector<fs::path> files;
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        GSList* list = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(dlg));
        for (GSList* l = list; l; l = l->next) {
            files.emplace_back(static_cast<const char*>(l->data));
            g_free(l->data);
        }
        g_slist_free(list);
    }
    g_object_unref(dlg);
    return files;
}

int MainWindow::installPlugins(const std::vector<fs::path>& files)
{
    const auto dirs = PluginManager::defaultDirs();
    if (dirs.empty() || !core::canModify(dirs.front())) {
        showError("Cannot install plugins", dirs.empty() ? "No plugin directory (NPP_GTK_PLUGINS_DIR is empty)."
                                                         : homeShortened(dirs.front()) + " cannot be written.");
        return 0;
    }
    const fs::path root = dirs.front();
    int installed = 0;
    for (const fs::path& f : files) {
        // The name a .so or a folder installs as; a zip says so only once opened.
        std::string name = f.extension() == ".zip" ? std::string() : f.filename().string();
        if (f.extension() == ".so")
            name = f.stem().string();
        std::error_code ec;
        if (!name.empty() && (fs::exists(root / name, ec) || fs::exists(root / "disabled" / name, ec))
            && ask("Replace the installed plugin \"" + name + "\"?", homeShortened(root / name), false) != Answer::Yes)
            continue;
        std::string err;
        if (core::installPlugin(f, root, &err))
            ++installed;
        else
            showError("Cannot install \"" + f.filename().string() + "\"", err);
    }
    return installed;
}

void MainWindow::cmdImportPlugins()
{
    // Upstream Settings > Import > Import plugin(s): copy, then tell to restart.
    if (installPlugins(choosePluginFiles(GTK_WINDOW(window_))) > 0 && !prompt_) {
        GtkWidget* dlg = gtk_message_dialog_new(GTK_WINDOW(window_), GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "%s",
                                                tr_.message("NeedToRestartToLoadPlugins", "message", "You have to restart Notepad++ to load plugins you installed.").c_str());
        gtk_window_set_title(GTK_WINDOW(dlg), tr_.message("NeedToRestartToLoadPlugins", "title", "Notepad++ needs to be relaunched").c_str());
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
    }
}

void MainWindow::cmdPluginsAdmin()
{
    const char* sec = "PluginsAdminDlg";
    struct Admin {
        MainWindow* w = nullptr;
        GtkWidget* dlg = nullptr;
        GtkWidget* tabs = nullptr;
        GtkWidget* search = nullptr;
        GtkWidget* views[2] = {};
        GtkListStore* stores[2] = {};
        GtkTreeModel* filters[2] = {};
        GtkWidget* remove = nullptr;
        GtkWidget* deactivate = nullptr;
        GtkWidget* activate = nullptr;
        GtkWidget* note = nullptr;
        std::vector<core::PluginFolder> rows;

        int tab() const { return gtk_notebook_get_current_page(GTK_NOTEBOOK(tabs)); }
        const core::PluginFolder* selected() const
        {
            GtkTreeModel* m = nullptr;
            GtkTreeIter it;
            const int t = std::max(tab(), 0);
            if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(views[t])), &m, &it))
                return nullptr;
            int i = -1;
            gtk_tree_model_get(m, &it, kAdmIndex, &i, -1);
            return i >= 0 && i < static_cast<int>(rows.size()) ? &rows[static_cast<std::size_t>(i)] : nullptr;
        }
        void fill()
        {
            rows = core::listPlugins(PluginManager::defaultDirs());
            for (GtkListStore* s : stores)
                gtk_list_store_clear(s);
            for (std::size_t i = 0; i < rows.size(); ++i) {
                const core::PluginFolder& f = rows[i];
                std::string name = f.name;
                if (w->plugins_ && !f.misplaced)
                    for (const auto& p : w->plugins_->plugins())
                        if (p->root == f.root && p->folder == f.name)
                            name = p->displayName;
                const std::string version = f.version;
                gtk_list_store_insert_with_values(stores[f.disabled ? 1 : 0], nullptr, -1, kAdmName, core::Translator::plain(name).c_str(),
                                                  kAdmVersion, version.c_str(), kAdmLocation, homeShortened(f.root).c_str(), kAdmStatus,
                                                  w->pluginFolderStatus(f, rows).c_str(), kAdmIndex, static_cast<int>(i), kAdmPath,
                                                  f.file.c_str(), -1);
            }
            update();
        }
        void update()
        {
            const core::PluginFolder* f = selected();
            const bool editable = f && f->writable;
            gtk_widget_set_sensitive(remove, editable);
            gtk_widget_set_sensitive(deactivate, editable && !f->misplaced);
            gtk_widget_set_sensitive(activate, editable);
            gtk_widget_set_visible(deactivate, tab() != 1);
            gtk_widget_set_visible(activate, tab() == 1);
            const char* why = f && !f->writable ? "A plugin in a system directory: change it with the package manager." : nullptr;
            for (GtkWidget* b : {remove, deactivate, activate})
                gtk_widget_set_tooltip_text(b, why);
        }
        void changed()
        {
            fill();
            gtk_widget_show(note);
        }
    } a;
    a.w = this;

    a.dlg = gtk_dialog_new_with_buttons(tr_.dialogAttr(sec, "title", "Plugins Admin").c_str(), GTK_WINDOW(window_), GTK_DIALOG_MODAL,
                                        core::Translator::toMnemonic(tr_.dialog(sec, 2, "Close")).c_str(), GTK_RESPONSE_CLOSE, nullptr);
    gtk_window_set_default_size(GTK_WINDOW(a.dlg), 760, 440);
    GtkWidget* box = gtk_dialog_get_content_area(GTK_DIALOG(a.dlg));
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    gtk_box_set_spacing(GTK_BOX(box), 6);

    GtkWidget* top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget* searchLabel = gtk_label_new_with_mnemonic(core::Translator::toMnemonic(tr_.dialog(sec, 5501, "&Search:")).c_str());
    a.search = gtk_search_entry_new();
    gtk_label_set_mnemonic_widget(GTK_LABEL(searchLabel), a.search);
    gtk_box_pack_start(GTK_BOX(top), searchLabel, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), a.search, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), top, FALSE, FALSE, 0);

    a.tabs = gtk_notebook_new();
    const std::string tabNames[] = {tr_.dialogAttr(sec, "titleInstalled", "Installed"), tr_.dialogAttr(sec, "titleDisabled", "Deactivated")};
    const std::string cols[] = {tr_.dialogAttr(sec + std::string("/ColumnPlugin"), "name", "Plugin"),
                                tr_.dialogAttr(sec + std::string("/ColumnVersion"), "name", "Version"), "Location", "Status"};
    for (int i = 0; i < 2; ++i) {
        a.stores[i] = gtk_list_store_new(kAdmCount, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT, G_TYPE_STRING);
        a.filters[i] = gtk_tree_model_filter_new(GTK_TREE_MODEL(a.stores[i]), nullptr);
        gtk_tree_model_filter_set_visible_func(GTK_TREE_MODEL_FILTER(a.filters[i]), +[](GtkTreeModel* m, GtkTreeIter* it, gpointer p) -> gboolean {
            gchar* folded = g_utf8_casefold(gtk_entry_get_text(GTK_ENTRY(p)), -1);
            gchar* name = nullptr;
            gtk_tree_model_get(m, it, kAdmName, &name, -1);
            gchar* n = g_utf8_casefold(name ? name : "", -1);
            const bool show = strstr(n, folded) != nullptr;
            g_free(n);
            g_free(name);
            g_free(folded);
            return show;
        }, a.search, nullptr);
        a.views[i] = gtk_tree_view_new_with_model(a.filters[i]);
        gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(a.views[i]), kAdmPath);
        for (int c = 0; c < 4; ++c) {
            GtkCellRenderer* r = gtk_cell_renderer_text_new();
            if (c == kAdmStatus || c == kAdmLocation)
                g_object_set(r, "ellipsize", c == kAdmLocation ? PANGO_ELLIPSIZE_MIDDLE : PANGO_ELLIPSIZE_END, nullptr);
            GtkTreeViewColumn* col = gtk_tree_view_column_new_with_attributes(cols[c].c_str(), r, "text", c, nullptr);
            gtk_tree_view_column_set_resizable(col, TRUE);
            if (c == kAdmLocation) {
                gtk_tree_view_column_set_sizing(col, GTK_TREE_VIEW_COLUMN_FIXED);
                gtk_tree_view_column_set_fixed_width(col, 220);
            }
            gtk_tree_view_column_set_sort_column_id(col, c);
            gtk_tree_view_column_set_expand(col, c == kAdmStatus);
            gtk_tree_view_append_column(GTK_TREE_VIEW(a.views[i]), col);
        }
        g_signal_connect_swapped(gtk_tree_view_get_selection(GTK_TREE_VIEW(a.views[i])), "changed",
                                 G_CALLBACK(+[](Admin* x) { x->update(); }), &a);
        GtkWidget* sw = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_container_add(GTK_CONTAINER(sw), a.views[i]);
        gtk_notebook_append_page(GTK_NOTEBOOK(a.tabs), sw, gtk_label_new(tabNames[i].c_str()));
    }
    gtk_box_pack_start(GTK_BOX(box), a.tabs, TRUE, TRUE, 0);
    g_signal_connect_swapped(a.search, "search-changed", G_CALLBACK(+[](Admin* x) {
                                 for (GtkTreeModel* f : x->filters)
                                     gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(f));
                             }), &a);
    g_signal_connect_after(a.tabs, "switch-page", G_CALLBACK(+[](GtkNotebook*, GtkWidget*, guint, gpointer p) {
                               g_idle_add(+[](gpointer q) -> gboolean { static_cast<Admin*>(q)->update(); return G_SOURCE_REMOVE; }, p);
                           }), &a);

    a.note = gtk_label_new("Restart Notepad++ to apply the changes.");
    gtk_label_set_xalign(GTK_LABEL(a.note), 0);
    gtk_widget_set_no_show_all(a.note, TRUE);
    gtk_box_pack_start(GTK_BOX(box), a.note, FALSE, FALSE, 0);

    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    auto button = [&](int id, const char* fb, void (*fn)(Admin*)) {
        GtkWidget* b = gtk_button_new_with_mnemonic(core::Translator::toMnemonic(tr_.dialog(sec, id, fb)).c_str());
        g_signal_connect_swapped(b, "clicked", G_CALLBACK(fn), &a);
        gtk_box_pack_start(GTK_BOX(row), b, FALSE, FALSE, 0);
        return b;
    };
    button(5503, "&Install", +[](Admin* x) {
        if (x->w->installPlugins(x->w->choosePluginFiles(GTK_WINDOW(x->dlg))) > 0) {
            gtk_notebook_set_current_page(GTK_NOTEBOOK(x->tabs), 0);
            x->changed();
        }
    });
    a.remove = button(5505, "&Remove", +[](Admin* x) {
        const core::PluginFolder* f = x->selected();
        if (!f || x->w->ask("Remove the plugin \"" + f->name + "\"?", homeShortened(f->misplaced ? f->file : f->dir), false) != Answer::Yes)
            return;
        std::string err;
        if (core::removePlugin(*f, &err))
            x->changed();
        else
            x->w->showError("Cannot remove \"" + f->name + "\"", err);
    });
    a.deactivate = button(5512, "D&eactivate", +[](Admin* x) {
        const core::PluginFolder* f = x->selected();
        std::string err;
        if (f && core::deactivatePlugin(*f, &err))
            x->changed();
        else if (f)
            x->w->showError("Cannot deactivate \"" + f->name + "\"", err);
    });
    a.activate = button(5513, "&Activate", +[](Admin* x) {
        const core::PluginFolder* f = x->selected();
        std::string err;
        if (f && core::activatePlugin(*f, &err))
            x->changed();
        else if (f)
            x->w->showError("Cannot activate \"" + f->name + "\"", err);
    });
    GtkWidget* folder = gtk_button_new_with_mnemonic(core::Translator::toMnemonic(core::Translator::plain(tr_.command(IDM_SETTING_OPENPLUGINSDIR, "Open Plugins Folder..."))).c_str());
    g_signal_connect_swapped(folder, "clicked", G_CALLBACK(+[](MainWindow* w) { w->cmds_.run(IDM_SETTING_OPENPLUGINSDIR); }), this);
    gtk_box_pack_end(GTK_BOX(row), folder, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);

    gtk_widget_show_all(box);
    a.fill();
    gtk_dialog_run(GTK_DIALOG(a.dlg));
    while (g_main_context_iteration(nullptr, FALSE)) { // the idle above uses `a`
    }
    gtk_widget_destroy(a.dlg);
    for (int i = 0; i < 2; ++i) {
        g_object_unref(a.filters[i]);
        g_object_unref(a.stores[i]);
    }
}

} // namespace npp::ui
