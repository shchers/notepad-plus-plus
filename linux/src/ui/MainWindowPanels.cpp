// Docking panels in the main window: creating them in the dock, the View /
// Edit / File menu commands that show them, keeping them in step with the
// active document, and their state in settings.ini and session.xml.

#include "ui/MainWindow.h"

#include "platform/Paths.h"

#include "menuCmdID.h"

#include <system_error>

namespace fs = std::filesystem;

namespace npp::ui {

namespace {

constexpr const char* kFunctionList = "functionList";
constexpr const char* kFolder = "folderAsWorkspace";
constexpr const char* kDocMap = "documentMap";
constexpr const char* kDocList = "documentList";
constexpr const char* kCharPanel = "charPanel";
constexpr const char* kClipboard = "clipboardHistory";
const char* const kProjects[] = {"project1", "project2", "project3"};

} // namespace

GtkWidget* MainWindow::buildPanels(GtkWidget* center)
{
    using Area = DockManager::Area;
    funcParsers_.init(platform::userConfigDir() / "functionList", platform::dataDir() / "functionList");
    dock_ = std::make_unique<DockManager>(GTK_WINDOW(window_), center);
    dock_->onShownChanged = [this](const std::string& id, bool shown) { onPanelShown(id, shown); };
    auto panelTitle = [this](const char* section, const char* fallback) { return tr_.dialogAttr(std::string(section) + "/PanelTitle", "name", fallback); };

    // Search results: upstream's Finder, docked at the bottom.
    results_ = std::make_unique<SearchResults>(tr_);
    results_->applyStyles(styles_);
    results_->onOpen = [this](const ResultLocation& loc) { openResult(loc); };
    dock_->add(kSearchResultsPanel, tr_.misc("find-result-caption", "Search results"), results_->widget(), Area::Bottom);

    funcList_ = std::make_unique<FunctionListPanel>(funcParsers_, tr_, [this] {
        FunctionListPanel::Document d;
        Tab* t = activeTab();
        if (!t)
            return d;
        d.view = t->view.get();
        d.name = t->buf->name();
        d.langName = t->buf->language ? t->buf->language->name : "normal";
        d.udlName = t->buf->userLanguage ? t->buf->userLanguage->name : "";
        return d;
    });
    funcList_->setSorted(settings_.functionListSorted);
    dock_->add(kFunctionList, panelTitle("FunctionList", "Function List"), funcList_->widget(), Area::Right);

    FolderPanelHost fh;
    fh.openFile = [this](const fs::path& p) { openFiles({p}); };
    fh.findInFiles = [this](const fs::path& dir) {
        findDlg_->setDirectory(dir.string());
        findDlg_->show(FindDialog::Tab::FindInFiles);
    };
    fh.openTerminal = [this](const fs::path& dir) { openTerminal(dir); };
    fh.openUri = [this](const std::string& uri) { openUri(uri); };
    fh.followSymlinks = [this] { return settings_.faWSymlinks; };
    fh.currentFile = [this] {
        core::Buffer* b = activeBuffer();
        return b && !b->isUntitled() ? b->path : fs::path();
    };
    fh.chooseFolder = [this] {
        GtkFileChooserNative* dlg = gtk_file_chooser_native_new(
            tr_.dialogAttr("FolderAsWorkspace/SelectFolderFromBrowserString", "name", "Select a folder to add in Folder as Workspace panel").c_str(),
            GTK_WINDOW(window_), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "_Add", "_Cancel");
        fs::path out;
        if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT)
            if (gchar* fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg))) {
                out = fn;
                g_free(fn);
            }
        g_object_unref(dlg);
        return out;
    };
    folder_ = std::make_unique<FolderPanel>(tr_, std::move(fh));
    dock_->add(kFolder, panelTitle("FolderAsWorkspace", "Folder as Workspace"), folder_->widget(), Area::Left);

    docMap_ = std::make_unique<DocumentMap>([this](EditorView& map, EditorView& source) {
        for (const auto& t : tabs_)
            if (t->view.get() == &source) {
                applyBufferLanguage(map, *t->buf);
                break;
            }
    });
    dock_->add(kDocMap, panelTitle("DocumentMap", "Document Map"), docMap_->widget(), Area::Right);

    docList_ = std::make_unique<DocListPanel>(
        tr_,
        [this](int view, int index) {
            if (Tab* t = tabAt(view, index))
                activateTab(*t);
        },
        [this](int view, int index) {
            if (Tab* t = tabAt(view, index))
                closeTab(*t);
        });
    dock_->add(kDocList, panelTitle("DocList", "Document List"), docList_->widget(), Area::Left);

    auto insertText = [this](const std::string& text) {
        editOp([&](const core::Sci& s) { s(SCI_REPLACESEL, 0, reinterpret_cast<sptr_t>(text.c_str())); });
        if (EditorView* v = activeView())
            gtk_widget_grab_focus(v->widget());
    };
    charPanel_ = std::make_unique<CharPanel>(tr_, insertText);
    dock_->add(kCharPanel, panelTitle("AsciiInsertion", "ASCII Codes Insertion Panel"), charPanel_->widget(), Area::Right);
    clipHistory_ = std::make_unique<ClipboardHistoryPanel>(insertText);
    dock_->add(kClipboard, panelTitle("ClipboardHistory", "Clipboard History"), clipHistory_->widget(), Area::Right);

    for (int i = 0; i < 3; ++i) {
        ProjectPanelHost ph;
        ph.openFile = [this](const fs::path& p) { openFiles({p}); };
        ph.findInProjects = [this](const std::vector<fs::path>& files) { findInProjects(files); };
        ph.ask = [this](const std::string& primary, const std::string& secondary, bool cancellable) {
            switch (ask(primary, secondary, cancellable)) {
            case Answer::Yes: return 1;
            case Answer::No: return 0;
            case Answer::Cancel: break;
            }
            return -1;
        };
        ph.error = [this](const std::string& primary, const std::string& secondary) { showError(primary, secondary); };
        projects_[static_cast<std::size_t>(i)] = std::make_unique<ProjectPanel>(i + 1, tr_, std::move(ph));
        dock_->add(kProjects[i], projects_[static_cast<std::size_t>(i)]->title(), projects_[static_cast<std::size_t>(i)]->widget(), Area::Left);
    }
    return dock_->widget();
}

void MainWindow::togglePanel(const char* id)
{
    dock_->toggle(id);
}

void MainWindow::registerPanelCommands()
{
    // View / Edit menu toggles, checked while the panel is shown.
    const std::pair<int, const char*> toggles[] = {
        {IDM_VIEW_FUNC_LIST, kFunctionList}, {IDM_VIEW_FILEBROWSER, kFolder},        {IDM_VIEW_DOC_MAP, kDocMap},
        {IDM_VIEW_DOCLIST, kDocList},        {IDM_EDIT_CHAR_PANEL, kCharPanel},     {IDM_EDIT_CLIPBOARDHISTORY_PANEL, kClipboard},
        {IDM_VIEW_PROJECT_PANEL_1, kProjects[0]}, {IDM_VIEW_PROJECT_PANEL_2, kProjects[1]}, {IDM_VIEW_PROJECT_PANEL_3, kProjects[2]},
    };
    for (const auto& [id, panel] : toggles)
        cmds_.add(id, [this, panel = panel] { togglePanel(panel); }, [this, panel = panel] { return dock_ && dock_->isShown(panel); });
    // Switch to ...: show and focus (upstream Ctrl+Alt+Shift+1 ...).
    const std::pair<int, const char*> focus[] = {
        {IDM_VIEW_SWITCHTO_FUNC_LIST, kFunctionList}, {IDM_VIEW_SWITCHTO_FILEBROWSER, kFolder}, {IDM_VIEW_SWITCHTO_DOCLIST, kDocList},
        {IDM_VIEW_SWITCHTO_PROJECT_PANEL_1, kProjects[0]}, {IDM_VIEW_SWITCHTO_PROJECT_PANEL_2, kProjects[1]},
        {IDM_VIEW_SWITCHTO_PROJECT_PANEL_3, kProjects[2]},
    };
    for (const auto& [id, panel] : focus)
        cmds_.add(id, [this, panel = panel] { dock_->show(panel, true); });

    cmds_.add(IDM_FILE_OPENFOLDERASWORKSPACE, [this] {
        GtkFileChooserNative* dlg = gtk_file_chooser_native_new(
            tr_.dialogAttr("FolderAsWorkspace/SelectFolderFromBrowserString", "name", "Select a folder to add in Folder as Workspace panel").c_str(),
            GTK_WINDOW(window_), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "_Open", "_Cancel");
        if (!lastDir_.empty())
            gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), lastDir_.c_str());
        if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT)
            if (gchar* fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg))) {
                folder_->addRoot(fn);
                dock_->show(kFolder, true);
                g_free(fn);
            }
        g_object_unref(dlg);
    });
    cmds_.add(IDM_FILE_CONTAININGFOLDERASWORKSPACE, [this] {
        core::Buffer* b = activeBuffer();
        if (!b || b->isUntitled())
            return;
        folder_->addRoot(b->path.parent_path());
        dock_->show(kFolder, true);
        folder_->locate(b->path);
    });
}

void MainWindow::onPanelShown(const std::string& id, bool shown)
{
    cmds_.syncStates();
    if (id.rfind("plugin:", 0) == 0) {
        onPluginPanelShown(id, shown);
        return;
    }
    if (!shown || closing_)
        return;
    if (id == kFunctionList)
        funcList_->reload();
    else if (id == kDocMap)
    {
        EditorView* v = activeView();
        panelSource_ = v;
        docMap_->attach(v);
    }
    else if (id == kDocList)
        refreshDocList();
    else if (id == kCharPanel)
        if (core::Buffer* b = activeBuffer())
            charPanel_->setCharset(b->encoding.bom == platform::Bom::None ? b->encoding.charset : "UTF-8");
}

void MainWindow::refreshPanels()
{
    if (closing_ || !dock_)
        return;
    Tab* t = activeTab();
    EditorView* v = t ? t->view.get() : nullptr;
    // What the Function List shows depends on the document and its language.
    const std::string key = t ? std::to_string(reinterpret_cast<std::uintptr_t>(v)) + "|" + (t->buf->language ? t->buf->language->name : "") + "|" +
                                    (t->buf->userLanguage ? t->buf->userLanguage->name : "")
                              : std::string();
    if (v != panelSource_ || key != panelKey_) {
        const bool docChanged = v != panelSource_;
        panelSource_ = v;
        panelKey_ = key;
        if (dock_->isShown(kFunctionList))
            funcList_->reload();
        if (docChanged)
            docMap_->attach(dock_->isShown(kDocMap) ? v : nullptr);
    }
    if (dock_->isShown(kCharPanel) && t)
        charPanel_->setCharset(t->buf->encoding.bom == platform::Bom::None ? t->buf->encoding.charset : "UTF-8");
    refreshDocList();
}

void MainWindow::refreshDocList()
{
    if (!docList_ || !dock_->isShown(kDocList))
        return;
    std::vector<DocListPanel::Entry> entries;
    const Tab* active = activeTab();
    for (int vi : {kMainView, kSubView}) {
        int i = 0;
        for (Tab* t : tabsInOrder(vi))
            entries.push_back({vi, i++, t->buf->name(), t->buf->fullName(), isModified(*t->buf), t == active});
    }
    docList_->update(entries);
}

void MainWindow::findInProjects(const std::vector<fs::path>& files)
{
    // Upstream opens the Find dialog's Find in Projects tab; the port asks for
    // the text and searches with the dialog's options.
    core::FindOptions o = findDlg_->options();
    GtkWidget* dlg = gtk_dialog_new_with_buttons(tr_.dialogAttr("Find", "titleFindInProjects", "Find in Projects").c_str(), GTK_WINDOW(window_),
                                                 GTK_DIALOG_MODAL, "_Cancel", GTK_RESPONSE_CANCEL, "_Find All", GTK_RESPONSE_OK, nullptr);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
    GtkWidget* box = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    GtkWidget* entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), o.what.c_str());
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_entry_set_width_chars(GTK_ENTRY(entry), 40);
    gtk_box_pack_start(GTK_BOX(box), gtk_label_new(core::Translator::plain(tr_.dialog("Find", 1620, "&Find what:")).c_str()), FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 4);
    gtk_widget_show_all(box);
    const bool ok = gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK;
    o.what = gtk_entry_get_text(GTK_ENTRY(entry));
    gtk_widget_destroy(dlg);
    if (ok && !o.what.empty())
        findDlg_->doFindInFileList(o, files);
}

void MainWindow::restorePanels()
{
    for (std::size_t i = 0; i < projects_.size(); ++i) {
        std::error_code ec;
        if (!settings_.projectWorkspaces[i].empty() && fs::is_regular_file(settings_.projectWorkspaces[i], ec))
            projects_[i]->openWorkspace(settings_.projectWorkspaces[i]);
    }
    // Upstream: with -nosession (or in another instance) only the panels
    // ticked in Preferences > Multi-Instance & Date open again.
    auto state = settings_.docking;
    if (!useSession_) {
        const platform::Settings& st = settings_;
        auto allowed = [&st](const std::string& id) {
            if (id == kClipboard) return st.noSessionClipboard;
            if (id == kDocList) return st.noSessionDocList;
            if (id == kCharPanel) return st.noSessionCharPanel;
            if (id == kFolder) return st.noSessionFolder;
            if (id == kDocMap) return st.noSessionDocMap;
            if (id == kFunctionList) return st.noSessionFuncList;
            if (id.rfind("project", 0) == 0) return st.noSessionProjects;
            if (id.rfind("plugin:", 0) == 0) return st.noSessionPlugins;
            return false;
        };
        for (auto& [id, v] : state) {
            const std::size_t comma = v.find(',');
            if (id != "sizes" && comma != std::string::npos && v.size() > comma + 1 && !allowed(id))
                v[comma + 1] = '0'; // "area,shown,..."
        }
    }
    dock_->restoreState(state);
}

void MainWindow::savePanels()
{
    if (!dock_ || !dock_->alive())
        return;
    settings_.docking = dock_->saveState();
    for (std::size_t i = 0; i < projects_.size(); ++i)
        settings_.projectWorkspaces[i] = projects_[i]->workspaceFile().string();
    settings_.functionListSorted = funcList_->sorted();
}

} // namespace npp::ui
