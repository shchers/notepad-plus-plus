#include "Panel.h"

#include "RemoteUtil.h"

#include <glib/gstdio.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <strings.h>

namespace nppremote {

namespace {

enum Col { kColIcon, kColName, kColPath, kColKind, kColLoaded, kColSize, kColTime, kColCount };
enum Kind { kDir, kFile, kLink, kPlaceholder };

constexpr int kLogLines = 2000;

std::wstring wide(const std::string& s)
{
    gunichar* u = g_utf8_to_ucs4(s.c_str(), -1, nullptr, nullptr, nullptr);
    std::wstring out = u ? reinterpret_cast<const wchar_t*>(u) : L"";
    g_free(u);
    return out;
}

std::string narrow(const wchar_t* s)
{
    gchar* u = g_ucs4_to_utf8(reinterpret_cast<const gunichar*>(s), -1, nullptr, nullptr, nullptr);
    std::string out = u ? u : "";
    g_free(u);
    return out;
}

std::string getPath(GtkTreeModel* m, GtkTreeIter* it)
{
    gchar* p = nullptr;
    gtk_tree_model_get(m, it, kColPath, &p, -1);
    std::string s = p ? p : "";
    g_free(p);
    return s;
}

int getKind(GtkTreeModel* m, GtkTreeIter* it)
{
    int k = kPlaceholder;
    gtk_tree_model_get(m, it, kColKind, &k, -1);
    return k;
}

void addPlaceholder(GtkTreeStore* store, GtkTreeIter* parent, const char* text)
{
    GtkTreeIter ph;
    gtk_tree_store_append(store, &ph, parent);
    gtk_tree_store_set(store, &ph, kColName, text, kColKind, kPlaceholder, kColPath, "", -1);
}

std::string profileUrl(const Profile& p)
{
    std::string url = p.protocol == Protocol::Ftp ? "ftp://" : "sftp://";
    if (!p.user.empty())
        url += p.user + "@";
    url += p.host;
    if (p.port > 0)
        url += ":" + std::to_string(p.port);
    return url;
}

// First layout of the panel: two thirds for the tree, the rest for the log.
void firstLayout(GtkWidget* paned, GdkRectangle* a, gpointer)
{
    if (a->height < 50)
        return;
    gtk_paned_set_position(GTK_PANED(paned), a->height * 2 / 3);
    g_signal_handlers_disconnect_by_func(paned, reinterpret_cast<gpointer>(firstLayout), nullptr);
}

// `icon` may list fallbacks, separated by spaces: the first one the theme has.
GtkWidget* iconButton(const char* icon, const char* tip)
{
    gchar** names = g_strsplit(icon, " ", -1);
    GIcon* gicon = g_themed_icon_new_from_names(names, -1);
    g_strfreev(names);
    GtkWidget* b = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(b), gtk_image_new_from_gicon(gicon, GTK_ICON_SIZE_MENU));
    gtk_button_set_always_show_image(GTK_BUTTON(b), TRUE);
    g_object_unref(gicon);
    gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(b, tip);
    return b;
}

} // namespace

Panel::Panel(const NppData& npp, std::string settingsFile, std::string cacheRoot, int panelCommand)
    : npp_(npp), settingsFile_(std::move(settingsFile)), cacheRoot_(std::move(cacheRoot)), panelCommand_(panelCommand)
{
    loadSettings(settingsFile_, settings_);
}

Panel::~Panel()
{
    session_.reset();
}

GtkWindow* Panel::mainWindow() const
{
    return reinterpret_cast<GtkWindow*>(::SendMessage(npp_._nppHandle, NPPM_GTKGETMAINWINDOW, 0, 0));
}

// ============================================================================
// Widgets
// ============================================================================

void Panel::build()
{
    box_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

    GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    connectButton_ = iconButton("network-server-symbolic", "Connect");
    g_signal_connect(connectButton_, "clicked", G_CALLBACK(+[](GtkWidget* w, gpointer p) { static_cast<Panel*>(p)->showProfileMenu(w); }), this);
    gtk_box_pack_start(GTK_BOX(bar), connectButton_, FALSE, FALSE, 0);

    auto add = [&](const char* icon, const char* tip, void (*fn)(Panel*), bool needsConnection) {
        GtkWidget* b = iconButton(icon, tip);
        g_signal_connect_swapped(b, "clicked", G_CALLBACK(fn), this);
        gtk_box_pack_start(GTK_BOX(bar), b, FALSE, FALSE, 0);
        if (needsConnection)
            needConnection_.push_back(b);
        return b;
    };
    // An action icon: Breeze draws the status icon network-offline half
    // transparent, as if the button were disabled.
    add("network-disconnect-symbolic network-offline-symbolic", "Disconnect", +[](Panel* p) { p->disconnect(); }, true);
    add("go-up-symbolic", "Parent folder", +[](Panel* p) {
        if (!p->root_.empty() && p->root_ != "/")
            p->setRoot(parentPath(p->root_));
    }, true);
    add("view-refresh-symbolic", "Refresh", +[](Panel* p) { p->refreshDir(p->selectedDir()); }, true);
    add("document-send-symbolic", "Upload the current document", +[](Panel* p) { p->uploadCurrent(); }, true);
    add("document-new-symbolic", "New file", +[](Panel* p) { p->newFile(); }, true);
    add("folder-new-symbolic", "New folder", +[](Panel* p) { p->newFolder(); }, true);
    add("edit-delete-symbolic", "Delete", +[](Panel* p) { p->deleteSelected(); }, true);
    stopButton_ = add("process-stop-symbolic", "Stop", +[](Panel* p) {
        if (p->session_) {
            p->session_->cancel();
            p->log(LogKind::Info, "Stopped");
        }
    }, false);
    spinner_ = gtk_spinner_new();
    gtk_box_pack_start(GTK_BOX(bar), spinner_, FALSE, FALSE, 4);

    GtkWidget* profiles = iconButton("emblem-system-symbolic", "Edit profiles");
    g_signal_connect_swapped(profiles, "clicked", G_CALLBACK(+[](Panel* p) { p->editProfiles(); }), this);
    gtk_box_pack_end(GTK_BOX(bar), profiles, FALSE, FALSE, 0);
    GtkWidget* logToggle = gtk_toggle_button_new();
    gtk_button_set_image(GTK_BUTTON(logToggle), gtk_image_new_from_icon_name("utilities-terminal-symbolic", GTK_ICON_SIZE_MENU));
    gtk_button_set_relief(GTK_BUTTON(logToggle), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(logToggle, "Show the log");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(logToggle), settings_.showLog);
    g_signal_connect(logToggle, "toggled", G_CALLBACK(+[](GtkToggleButton* b, gpointer p) {
                         auto* self = static_cast<Panel*>(p);
                         self->settings_.showLog = gtk_toggle_button_get_active(b);
                         gtk_widget_set_visible(self->logBox_, self->settings_.showLog);
                         self->saveSettings();
                     }), this);
    gtk_box_pack_end(GTK_BOX(bar), logToggle, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box_), bar, FALSE, FALSE, 0);

    pathEntry_ = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(pathEntry_), "Remote folder");
    g_signal_connect(pathEntry_, "activate", G_CALLBACK(+[](GtkEntry* e, gpointer p) {
                         auto* self = static_cast<Panel*>(p);
                         std::string path = gtk_entry_get_text(e);
                         if (self->session_ && !path.empty())
                             self->setRoot(resolvePath(path, self->home_, self->root_));
                     }), this);
    needConnection_.push_back(pathEntry_);
    gtk_box_pack_start(GTK_BOX(box_), pathEntry_, FALSE, FALSE, 0);

    // Name (icon + text), Size, Modified.
    store_ = gtk_tree_store_new(kColCount, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT, G_TYPE_BOOLEAN, G_TYPE_STRING,
                                G_TYPE_STRING);
    tree_ = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store_));
    g_object_unref(store_); // the view keeps it
    GtkTreeViewColumn* col = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(col, "Name");
    gtk_tree_view_column_set_expand(col, TRUE);
    gtk_tree_view_column_set_resizable(col, TRUE);
    GtkCellRenderer* icon = gtk_cell_renderer_pixbuf_new();
    GtkCellRenderer* txt = gtk_cell_renderer_text_new();
    g_object_set(txt, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
    gtk_tree_view_column_pack_start(col, icon, FALSE);
    gtk_tree_view_column_pack_start(col, txt, TRUE);
    gtk_tree_view_column_add_attribute(col, icon, "icon-name", kColIcon);
    gtk_tree_view_column_add_attribute(col, txt, "text", kColName);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree_), col);
    GtkCellRenderer* right = gtk_cell_renderer_text_new();
    g_object_set(right, "xalign", 1.0, nullptr);
    GtkTreeViewColumn* sizeCol = gtk_tree_view_column_new_with_attributes("Size", right, "text", kColSize, nullptr);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree_), sizeCol);
    GtkTreeViewColumn* timeCol = gtk_tree_view_column_new_with_attributes("Modified (UTC)", gtk_cell_renderer_text_new(), "text", kColTime, nullptr);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree_), timeCol);
    gtk_tree_view_set_search_column(GTK_TREE_VIEW(tree_), kColName);

    g_signal_connect(tree_, "test-expand-row", G_CALLBACK(+[](GtkTreeView*, GtkTreeIter* it, GtkTreePath*, gpointer p) -> gboolean {
                         auto* self = static_cast<Panel*>(p);
                         GtkTreeModel* m = GTK_TREE_MODEL(self->store_);
                         gboolean loaded = FALSE;
                         gtk_tree_model_get(m, it, kColLoaded, &loaded, -1);
                         if (!loaded)
                             self->loadDir(getPath(m, it));
                         return FALSE;
                     }), this);
    g_signal_connect(tree_, "row-activated", G_CALLBACK(+[](GtkTreeView* tv, GtkTreePath* path, GtkTreeViewColumn*, gpointer p) {
                         auto* self = static_cast<Panel*>(p);
                         GtkTreeModel* m = GTK_TREE_MODEL(self->store_);
                         GtkTreeIter it;
                         if (!gtk_tree_model_get_iter(m, &it, path))
                             return;
                         const int kind = getKind(m, &it);
                         if (kind == kFile)
                             self->openRemote(getPath(m, &it));
                         else if (kind == kDir || kind == kLink) {
                             if (gtk_tree_view_row_expanded(tv, path))
                                 gtk_tree_view_collapse_row(tv, path);
                             else
                                 gtk_tree_view_expand_row(tv, path, FALSE);
                         }
                     }), this);
    g_signal_connect(tree_, "button-press-event", G_CALLBACK(+[](GtkWidget* w, GdkEventButton* ev, gpointer p) -> gboolean {
                         if (ev->type != GDK_BUTTON_PRESS || ev->button != 3)
                             return FALSE;
                         GtkTreePath* path = nullptr;
                         if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(w), static_cast<gint>(ev->x), static_cast<gint>(ev->y), &path, nullptr,
                                                           nullptr, nullptr)) {
                             gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(w)), path);
                             gtk_tree_path_free(path);
                         }
                         static_cast<Panel*>(p)->popupMenu(ev);
                         return TRUE;
                     }), this);
    g_signal_connect(tree_, "key-press-event", G_CALLBACK(+[](GtkWidget*, GdkEventKey* ev, gpointer p) -> gboolean {
                         auto* self = static_cast<Panel*>(p);
                         if (ev->keyval == GDK_KEY_Delete) {
                             self->deleteSelected();
                             return TRUE;
                         }
                         if (ev->keyval == GDK_KEY_F2) {
                             self->renameSelected();
                             return TRUE;
                         }
                         if (ev->keyval == GDK_KEY_F5) {
                             self->refreshDir(self->selectedDir());
                             return TRUE;
                         }
                         return FALSE;
                     }), this);

    GtkWidget* treeScroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(treeScroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(treeScroll), tree_);

    logView_ = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(logView_), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(logView_), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(logView_), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(logView_), GTK_WRAP_WORD_CHAR);
    GtkTextBuffer* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(logView_));
    gtk_text_buffer_create_tag(buf, "command", "foreground", "#3584e4", nullptr);
    gtk_text_buffer_create_tag(buf, "error", "foreground", "#e01b24", nullptr);
    gtk_text_buffer_create_tag(buf, "info", "style", PANGO_STYLE_ITALIC, nullptr);
    logBox_ = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(logBox_), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(logBox_), logView_);

    paned_ = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_paned_pack1(GTK_PANED(paned_), treeScroll, TRUE, FALSE);
    gtk_paned_pack2(GTK_PANED(paned_), logBox_, FALSE, TRUE);
    if (settings_.logPanePosition > 0)
        gtk_paned_set_position(GTK_PANED(paned_), settings_.logPanePosition);
    else
        g_signal_connect(paned_, "size-allocate", G_CALLBACK(firstLayout), nullptr);
    gtk_box_pack_start(GTK_BOX(box_), paned_, TRUE, TRUE, 0);

    gtk_widget_show_all(box_);
    gtk_widget_set_visible(logBox_, settings_.showLog);
    gtk_widget_hide(spinner_);
    updateButtons();
    log(LogKind::Info, "Choose a profile with the Connect button (FTP, or SFTP through the system's OpenSSH client).");
}

void Panel::setShown(bool shown)
{
    if (shutDown_)
        return;
    if (shown && !box_)
        build();
    if (shown && !registered_) {
        dock_ = DockedWidgetData{};
        dock_.hClient = NppGtk_handle(box_);
        dock_.pszName = title_.c_str();
        dock_.dlgID = panelCommand_;
        dock_.uMask = DWS_DF_CONT_RIGHT | DWS_ADDINFO;
        dock_.pszAddInfo = addInfo_;
        dock_.pszModuleName = L"NppRemote.so";
        ::SendMessage(npp_._nppHandle, NPPM_DMMREGASDCKDLG, 0, reinterpret_cast<LPARAM>(&dock_));
        registered_ = true;
        updateTitle();
    }
    if (!registered_)
        return;
    shown_ = shown;
    ::SendMessage(npp_._nppHandle, shown ? NPPM_DMMSHOW : NPPM_DMMHIDE, 0, reinterpret_cast<LPARAM>(box_));
    if (shownChanged)
        shownChanged(shown);
}

void Panel::updateButtons()
{
    if (!box_)
        return;
    for (GtkWidget* w : needConnection_)
        gtk_widget_set_sensitive(w, session_ != nullptr);
    gtk_widget_set_sensitive(stopButton_, busy_);
    if (busy_) {
        gtk_widget_show(spinner_);
        gtk_spinner_start(GTK_SPINNER(spinner_));
    } else {
        gtk_spinner_stop(GTK_SPINNER(spinner_));
        gtk_widget_hide(spinner_);
    }
}

void Panel::updateTitle()
{
    std::wstring info;
    if (session_)
        info = wide(session_->profile().name + (connected_ ? "" : busy_ ? " (connecting)" : " (offline)"));
    // The dock reads the text through the pointer it was given.
    wcsncpy(addInfo_, info.c_str(), sizeof addInfo_ / sizeof addInfo_[0] - 1);
    addInfo_[sizeof addInfo_ / sizeof addInfo_[0] - 1] = 0;
    if (registered_)
        ::SendMessage(npp_._nppHandle, NPPM_DMMUPDATEDISPINFO, 0, reinterpret_cast<LPARAM>(box_));
}

void Panel::log(LogKind kind, const std::string& text)
{
    if (!logView_ || shutDown_)
        return;
    GtkTextBuffer* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(logView_));
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buf, &end);
    const char* tag = kind == LogKind::Command ? "command" : kind == LogKind::Error ? "error" : kind == LogKind::Info ? "info" : nullptr;
    std::string line = (kind == LogKind::Command ? "> " : "") + text + "\n";
    if (!g_utf8_validate(line.c_str(), -1, nullptr)) {
        gchar* fixed = g_utf8_make_valid(line.c_str(), -1);
        line = fixed;
        g_free(fixed);
    }
    if (tag)
        gtk_text_buffer_insert_with_tags_by_name(buf, &end, line.c_str(), -1, tag, nullptr);
    else
        gtk_text_buffer_insert(buf, &end, line.c_str(), -1);
    const int extra = gtk_text_buffer_get_line_count(buf) - kLogLines;
    if (extra > 0) {
        GtkTextIter a, b;
        gtk_text_buffer_get_start_iter(buf, &a);
        gtk_text_buffer_get_iter_at_line(buf, &b, extra);
        gtk_text_buffer_delete(buf, &a, &b);
    }
    gtk_text_buffer_get_end_iter(buf, &end);
    GtkTextMark* mark = gtk_text_buffer_get_mark(buf, "end");
    if (!mark)
        mark = gtk_text_buffer_create_mark(buf, "end", &end, FALSE);
    else
        gtk_text_buffer_move_mark(buf, mark, &end);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(logView_), mark);
}

// ============================================================================
// Dialogs
// ============================================================================

void Panel::showError(const std::string& what, const std::string& why) const
{
    GtkWidget* d = gtk_message_dialog_new(mainWindow(), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", what.c_str());
    if (!why.empty())
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", why.c_str());
    gtk_window_set_title(GTK_WINDOW(d), "Remote Files");
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

bool Panel::confirm(const std::string& question, const std::string& yes) const
{
    GtkWidget* d = gtk_message_dialog_new(mainWindow(), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "%s", question.c_str());
    gtk_window_set_title(GTK_WINDOW(d), "Remote Files");
    gtk_dialog_add_buttons(GTK_DIALOG(d), "_Cancel", GTK_RESPONSE_CANCEL, yes.c_str(), GTK_RESPONSE_ACCEPT, nullptr);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL);
    const bool ok = gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT;
    gtk_widget_destroy(d);
    return ok;
}

namespace {

bool entryDialog(GtkWindow* parent, const std::string& title, const std::string& label, std::string& value, bool secret)
{
    GtkWidget* d = gtk_dialog_new_with_buttons(title.c_str(), parent, static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
                                               "_Cancel", GTK_RESPONSE_CANCEL, "_OK", GTK_RESPONSE_ACCEPT, nullptr);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT);
    GtkWidget* area = gtk_dialog_get_content_area(GTK_DIALOG(d));
    gtk_container_set_border_width(GTK_CONTAINER(area), 12);
    gtk_box_set_spacing(GTK_BOX(area), 8);
    GtkWidget* l = gtk_label_new(label.c_str());
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 60);
    GtkWidget* e = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(e), value.c_str());
    gtk_entry_set_activates_default(GTK_ENTRY(e), TRUE);
    gtk_entry_set_width_chars(GTK_ENTRY(e), 40);
    if (secret) {
        gtk_entry_set_visibility(GTK_ENTRY(e), FALSE);
        gtk_entry_set_input_purpose(GTK_ENTRY(e), GTK_INPUT_PURPOSE_PASSWORD);
    }
    gtk_box_pack_start(GTK_BOX(area), l, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(area), e, FALSE, FALSE, 0);
    gtk_widget_show_all(d);
    // A name: select it without the extension, as file managers do.
    if (!secret) {
        const std::size_t dot = value.rfind('.');
        gtk_editable_select_region(GTK_EDITABLE(e), 0, dot != std::string::npos && dot > 0 ? static_cast<gint>(g_utf8_pointer_to_offset(value.c_str(), value.c_str() + dot)) : -1);
    }
    const bool ok = gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT;
    if (ok)
        value = gtk_entry_get_text(GTK_ENTRY(e));
    gtk_widget_destroy(d);
    return ok;
}

} // namespace

bool Panel::askText(const std::string& title, const std::string& label, std::string& value) const
{
    return entryDialog(mainWindow(), title, label, value, false);
}

bool Panel::askSecret(const std::string& prompt, std::string& answer) const
{
    answer.clear();
    const std::string who = session_ ? session_->profile().name : std::string("Remote Files");
    return entryDialog(mainWindow(), who, prompt, answer, true);
}

// ============================================================================
// Profiles
// ============================================================================

void Panel::saveSettings()
{
    if (paned_ && settings_.showLog)
        settings_.logPanePosition = gtk_paned_get_position(GTK_PANED(paned_));
    std::string err;
    if (!nppremote::saveSettings(settingsFile_, settings_, err))
        log(LogKind::Error, "Cannot save the profiles: " + err);
}

void Panel::showProfileMenu(GtkWidget* anchor)
{
    GtkWidget* menu = gtk_menu_new();
    for (const Profile& p : settings_.profiles) {
        const std::string label = p.name + "  (" + profileUrl(p) + ")";
        GtkWidget* item = gtk_menu_item_new_with_label(label.c_str());
        g_object_set_data_full(G_OBJECT(item), "profile", g_strdup(p.name.c_str()), g_free);
        g_signal_connect(item, "activate", G_CALLBACK(+[](GtkMenuItem* i, gpointer self) {
                             static_cast<Panel*>(self)->connectTo(static_cast<const char*>(g_object_get_data(G_OBJECT(i), "profile")));
                         }), this);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }
    if (!settings_.profiles.empty())
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    GtkWidget* edit = gtk_menu_item_new_with_mnemonic("_Edit Profiles…");
    g_signal_connect_swapped(edit, "activate", G_CALLBACK(+[](Panel* p) { p->editProfiles(); }), this);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), edit);
    gtk_widget_show_all(menu);
    g_signal_connect(menu, "deactivate", G_CALLBACK(+[](GtkWidget* m, gpointer) {
                         // Destroyed after the item's "activate" has run.
                         g_idle_add(+[](gpointer w) -> gboolean {
                             gtk_widget_destroy(GTK_WIDGET(w));
                             return G_SOURCE_REMOVE;
                         }, m);
                     }), nullptr);
    gtk_menu_popup_at_widget(GTK_MENU(menu), anchor, GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST, nullptr);
}

namespace {

// The profile editor: a list on the left, the selected profile's fields on
// the right. It edits a copy, kept on OK.
struct ProfileEditor {
    std::vector<Profile> profiles;
    int current = -1;
    bool loading = false;
    GtkListStore* names = nullptr;
    GtkWidget* list = nullptr;
    GtkWidget* form = nullptr;
    GtkWidget *name, *protocol, *host, *port, *user, *password, *savePassword, *initialDir, *timeout, *activeMode, *identity, *identityBrowse,
        *sftpOptions;

    void store()
    {
        if (current < 0 || current >= static_cast<int>(profiles.size()))
            return;
        Profile& p = profiles[current];
        p.name = gtk_entry_get_text(GTK_ENTRY(name));
        p.protocol = gtk_combo_box_get_active(GTK_COMBO_BOX(protocol)) == 1 ? Protocol::Ftp : Protocol::Sftp;
        p.host = gtk_entry_get_text(GTK_ENTRY(host));
        p.port = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(port));
        p.user = gtk_entry_get_text(GTK_ENTRY(user));
        p.password = gtk_entry_get_text(GTK_ENTRY(password));
        p.savePassword = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(savePassword));
        p.initialDir = gtk_entry_get_text(GTK_ENTRY(initialDir));
        p.timeout = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(timeout));
        p.activeMode = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(activeMode));
        p.identityFile = gtk_entry_get_text(GTK_ENTRY(identity));
        p.sftpOptions = gtk_entry_get_text(GTK_ENTRY(sftpOptions));
        GtkTreeIter it;
        if (gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(names), &it, nullptr, current))
            gtk_list_store_set(names, &it, 0, p.name.c_str(), -1);
    }

    void load(int index)
    {
        current = index;
        const bool any = index >= 0 && index < static_cast<int>(profiles.size());
        gtk_widget_set_sensitive(form, any);
        const Profile p = any ? profiles[index] : Profile{};
        loading = true;
        gtk_entry_set_text(GTK_ENTRY(name), p.name.c_str());
        gtk_combo_box_set_active(GTK_COMBO_BOX(protocol), p.protocol == Protocol::Ftp ? 1 : 0);
        gtk_entry_set_text(GTK_ENTRY(host), p.host.c_str());
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(port), p.port);
        gtk_entry_set_text(GTK_ENTRY(user), p.user.c_str());
        gtk_entry_set_text(GTK_ENTRY(password), p.password.c_str());
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(savePassword), p.savePassword);
        gtk_entry_set_text(GTK_ENTRY(initialDir), p.initialDir.c_str());
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(timeout), p.timeout);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(activeMode), p.activeMode);
        gtk_entry_set_text(GTK_ENTRY(identity), p.identityFile.c_str());
        gtk_entry_set_text(GTK_ENTRY(sftpOptions), p.sftpOptions.c_str());
        loading = false;
        protocolChanged();
    }

    void protocolChanged()
    {
        const bool ftp = gtk_combo_box_get_active(GTK_COMBO_BOX(protocol)) == 1;
        gtk_widget_set_sensitive(activeMode, ftp);
        gtk_widget_set_sensitive(identity, !ftp);
        gtk_widget_set_sensitive(identityBrowse, !ftp);
        gtk_widget_set_sensitive(sftpOptions, !ftp);
    }

    void select(int index)
    {
        GtkTreeIter it;
        GtkTreeSelection* sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(list));
        if (index >= 0 && gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(names), &it, nullptr, index))
            gtk_tree_selection_select_iter(sel, &it);
        else
            load(-1);
    }

    void add(Profile p)
    {
        store();
        GtkTreeIter it;
        gtk_list_store_append(names, &it);
        gtk_list_store_set(names, &it, 0, p.name.c_str(), -1);
        profiles.push_back(std::move(p));
        select(static_cast<int>(profiles.size()) - 1);
    }
};

GtkWidget* formLabel(const char* text)
{
    GtkWidget* l = gtk_label_new_with_mnemonic(text);
    gtk_label_set_xalign(GTK_LABEL(l), 1);
    return l;
}

} // namespace

void Panel::editProfiles()
{
    ProfileEditor ed;
    ed.profiles = settings_.profiles;

    GtkWidget* d = gtk_dialog_new_with_buttons("Remote Profiles", mainWindow(), static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
                                               "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, nullptr);
    gtk_window_set_default_size(GTK_WINDOW(d), 640, 400);
    GtkWidget* area = gtk_dialog_get_content_area(GTK_DIALOG(d));
    gtk_container_set_border_width(GTK_CONTAINER(area), 12);
    GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_pack_start(GTK_BOX(area), hbox, TRUE, TRUE, 0);

    // Left: the profiles.
    GtkWidget* left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    ed.names = gtk_list_store_new(1, G_TYPE_STRING);
    for (const Profile& p : ed.profiles) {
        GtkTreeIter it;
        gtk_list_store_append(ed.names, &it);
        gtk_list_store_set(ed.names, &it, 0, p.name.c_str(), -1);
    }
    ed.list = gtk_tree_view_new_with_model(GTK_TREE_MODEL(ed.names));
    g_object_unref(ed.names);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(ed.list), FALSE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(ed.list), gtk_tree_view_column_new_with_attributes("", gtk_cell_renderer_text_new(), "text", 0, nullptr));
    GtkWidget* sw = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
    gtk_widget_set_size_request(sw, 180, -1);
    gtk_container_add(GTK_CONTAINER(sw), ed.list);
    gtk_box_pack_start(GTK_BOX(left), sw, TRUE, TRUE, 0);
    GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget* addB = gtk_button_new_with_mnemonic("_New");
    GtkWidget* copyB = gtk_button_new_with_mnemonic("Co_py");
    GtkWidget* delB = gtk_button_new_with_mnemonic("_Delete");
    gtk_box_pack_start(GTK_BOX(buttons), addB, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), copyB, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), delB, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(left), buttons, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), left, FALSE, TRUE, 0);

    // Right: its fields.
    GtkWidget* grid = gtk_grid_new();
    ed.form = grid;
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    int row = 0;
    auto field = [&](const char* label, GtkWidget* w, GtkWidget* extra = nullptr) {
        GtkWidget* l = formLabel(label);
        gtk_label_set_mnemonic_widget(GTK_LABEL(l), w);
        gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);
        gtk_widget_set_hexpand(w, TRUE);
        gtk_grid_attach(GTK_GRID(grid), w, 1, row, extra ? 1 : 2, 1);
        if (extra)
            gtk_grid_attach(GTK_GRID(grid), extra, 2, row, 1, 1);
        ++row;
    };
    ed.name = gtk_entry_new();
    field("_Name:", ed.name);
    ed.protocol = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ed.protocol), "SFTP (OpenSSH)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ed.protocol), "FTP");
    field("_Protocol:", ed.protocol);
    ed.host = gtk_entry_new();
    field("_Host:", ed.host);
    ed.port = gtk_spin_button_new_with_range(0, 65535, 1);
    gtk_widget_set_tooltip_text(ed.port, "0: the protocol's default (22 for SFTP, 21 for FTP)");
    field("P_ort:", ed.port);
    ed.user = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ed.user), "anonymous (FTP) / from ~/.ssh/config (SFTP)");
    field("_User:", ed.user);
    ed.password = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(ed.password), FALSE);
    gtk_entry_set_input_purpose(GTK_ENTRY(ed.password), GTK_INPUT_PURPOSE_PASSWORD);
    gtk_entry_set_placeholder_text(GTK_ENTRY(ed.password), "asked when needed");
    field("Pass_word:", ed.password);
    ed.savePassword = gtk_check_button_new_with_mnemonic("Sa_ve the password (unencrypted, in a file only you can read)");
    gtk_grid_attach(GTK_GRID(grid), ed.savePassword, 1, row++, 2, 1);
    ed.initialDir = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ed.initialDir), "the login folder");
    gtk_widget_set_tooltip_text(ed.initialDir, "An absolute path, or one relative to the login folder (~ or ~/path).");
    field("_Initial folder:", ed.initialDir);
    ed.timeout = gtk_spin_button_new_with_range(5, 600, 1);
    field("_Timeout (s):", ed.timeout);
    ed.activeMode = gtk_check_button_new_with_mnemonic("FTP _active mode (the server connects back)");
    gtk_grid_attach(GTK_GRID(grid), ed.activeMode, 1, row++, 2, 1);
    ed.identity = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ed.identity), "default keys and ssh-agent");
    ed.identityBrowse = gtk_button_new_with_label("…");
    field("SSH _key:", ed.identity, ed.identityBrowse);
    ed.sftpOptions = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ed.sftpOptions), "e.g. -o ProxyJump=gateway");
    field("sftp optio_ns:", ed.sftpOptions);
    GtkWidget* note = gtk_label_new("SFTP runs the system's sftp command: ~/.ssh/config, keys and the agent apply.");
    gtk_label_set_xalign(GTK_LABEL(note), 0);
    gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(note), "dim-label");
    gtk_grid_attach(GTK_GRID(grid), note, 0, row++, 3, 1);
    gtk_box_pack_start(GTK_BOX(hbox), grid, TRUE, TRUE, 0);

    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(ed.list)), "changed", G_CALLBACK(+[](GtkTreeSelection* sel, gpointer p) {
                         auto* e = static_cast<ProfileEditor*>(p);
                         GtkTreeModel* m;
                         GtkTreeIter it;
                         if (!gtk_tree_selection_get_selected(sel, &m, &it))
                             return;
                         GtkTreePath* path = gtk_tree_model_get_path(m, &it);
                         const int index = gtk_tree_path_get_indices(path)[0];
                         gtk_tree_path_free(path);
                         if (index == e->current)
                             return;
                         e->store();
                         e->load(index);
                     }), &ed);
    g_signal_connect(ed.name, "changed", G_CALLBACK(+[](GtkEditable*, gpointer p) {
                         auto* e = static_cast<ProfileEditor*>(p);
                         if (!e->loading)
                             e->store();
                     }), &ed);
    g_signal_connect(ed.protocol, "changed", G_CALLBACK(+[](GtkComboBox*, gpointer p) { static_cast<ProfileEditor*>(p)->protocolChanged(); }), &ed);
    g_signal_connect(addB, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) {
                         auto* e = static_cast<ProfileEditor*>(p);
                         e->store();
                         Profile np;
                         np.name = uniqueProfileName(e->profiles, "New profile");
                         e->add(np);
                         gtk_widget_grab_focus(e->host);
                     }), &ed);
    g_signal_connect(copyB, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) {
                         auto* e = static_cast<ProfileEditor*>(p);
                         if (e->current < 0)
                             return;
                         e->store();
                         Profile np = e->profiles[e->current];
                         np.name = uniqueProfileName(e->profiles, np.name);
                         e->add(np);
                     }), &ed);
    g_signal_connect(delB, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) {
                         auto* e = static_cast<ProfileEditor*>(p);
                         if (e->current < 0)
                             return;
                         const int i = e->current;
                         GtkTreeIter it;
                         e->current = -1; // nothing to store
                         if (gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(e->names), &it, nullptr, i))
                             gtk_list_store_remove(e->names, &it);
                         e->profiles.erase(e->profiles.begin() + i);
                         e->select(std::min(i, static_cast<int>(e->profiles.size()) - 1));
                     }), &ed);
    g_signal_connect(ed.identityBrowse, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer p) {
                         auto* e = static_cast<ProfileEditor*>(p);
                         GtkFileChooserNative* fc = gtk_file_chooser_native_new("SSH Key", GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(b))),
                                                                                GTK_FILE_CHOOSER_ACTION_OPEN, "_Open", "_Cancel");
                         const std::string ssh = std::string(g_get_home_dir()) + "/.ssh";
                         gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(fc), ssh.c_str());
                         gtk_file_chooser_set_show_hidden(GTK_FILE_CHOOSER(fc), TRUE);
                         if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(fc)) == GTK_RESPONSE_ACCEPT) {
                             gchar* f = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(fc));
                             gtk_entry_set_text(GTK_ENTRY(e->identity), f ? f : "");
                             g_free(f);
                         }
                         g_object_unref(fc);
                     }), &ed);

    gtk_widget_show_all(d);
    if (ed.profiles.empty()) {
        Profile np;
        np.name = uniqueProfileName(ed.profiles, "New profile");
        ed.add(np);
    } else {
        ed.select(0);
    }

    for (;;) {
        if (gtk_dialog_run(GTK_DIALOG(d)) != GTK_RESPONSE_ACCEPT)
            break;
        ed.store();
        // Names identify profiles (and their cache folders): unique, not empty.
        std::string problem;
        for (std::size_t i = 0; i < ed.profiles.size() && problem.empty(); ++i) {
            const Profile& p = ed.profiles[i];
            if (p.name.empty())
                problem = "Every profile needs a name.";
            else if (p.host.empty())
                problem = "The profile \"" + p.name + "\" has no host.";
            for (std::size_t j = 0; j < i && problem.empty(); ++j)
                if (ed.profiles[j].name == p.name)
                    problem = "Two profiles are named \"" + p.name + "\".";
            if (!problem.empty())
                ed.select(static_cast<int>(i));
        }
        if (!problem.empty()) {
            GtkWidget* m = gtk_message_dialog_new(GTK_WINDOW(d), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_CLOSE, "%s", problem.c_str());
            gtk_dialog_run(GTK_DIALOG(m));
            gtk_widget_destroy(m);
            continue;
        }
        settings_.profiles = ed.profiles;
        saveSettings();
        break;
    }
    gtk_widget_destroy(d);
}

// ============================================================================
// Connection
// ============================================================================

bool Panel::ensureSession(const Profile& p)
{
    if (session_ && session_->profile().name == p.name)
        return true;
    session_.reset();
    connected_ = false;
    Session::Hooks hooks;
    hooks.log = [this](LogKind k, const std::string& s) { log(k, s); };
    hooks.askSecret = [this](const std::string& prompt, std::string& answer) { return askSecret(prompt, answer); };
    hooks.confirm = [this](const std::string& q) { return confirm(q, "_Connect"); };
    hooks.connectionChanged = [this](bool c) {
        connected_ = c;
        updateTitle();
        if (!c)
            log(LogKind::Info, "Disconnected");
    };
    hooks.busyChanged = [this](bool b) {
        busy_ = b;
        updateButtons();
        updateTitle();
    };
    session_ = std::make_unique<Session>(p, std::move(hooks));
    updateButtons();
    updateTitle();
    return true;
}

void Panel::connectTo(const std::string& profileName)
{
    const Profile* found = nullptr;
    for (const Profile& p : settings_.profiles)
        if (p.name == profileName)
            found = &p;
    if (!found)
        return;
    if (!shown_)
        setShown(true);
    const Profile p = *found;
    if (session_ && session_->profile().name != p.name)
        disconnect();
    ensureSession(p);
    if (store_)
        gtk_tree_store_clear(store_);
    auto home = std::make_shared<std::string>();
    session_->post([home](RemoteClient& c) {
                       c.home(*home);
                       return true;
                   },
                   [this, home, p](bool ok, const std::string& error) {
                       if (!ok) {
                           const bool cancelled = error == "cancelled";
                           session_.reset();
                           connected_ = false;
                           updateButtons();
                           updateTitle();
                           if (!cancelled)
                               showError("Cannot connect to " + p.name, error);
                           return;
                       }
                       home_ = *home;
                       setRoot(p.initialDir.empty() ? home_ : resolvePath(p.initialDir, home_, home_));
                   });
}

void Panel::disconnect()
{
    if (!session_)
        return;
    const bool was = connected_;
    session_.reset(); // logs out
    connected_ = false;
    busy_ = false;
    root_.clear();
    home_.clear();
    if (store_)
        gtk_tree_store_clear(store_);
    if (pathEntry_)
        gtk_entry_set_text(GTK_ENTRY(pathEntry_), "");
    updateButtons();
    updateTitle();
    if (was)
        log(LogKind::Info, "Disconnected");
}

void Panel::shutdown()
{
    if (shutDown_)
        return;
    if (box_)
        saveSettings();
    session_.reset();
    shutDown_ = true;
    // The widgets go with the main window.
    box_ = logView_ = tree_ = nullptr;
    store_ = nullptr;
}

// ============================================================================
// Tree
// ============================================================================

void Panel::setRoot(const std::string& path)
{
    if (!store_)
        return;
    root_ = path;
    gtk_entry_set_text(GTK_ENTRY(pathEntry_), path.c_str());
    gtk_tree_store_clear(store_);
    GtkTreeIter it;
    gtk_tree_store_append(store_, &it, nullptr);
    gtk_tree_store_set(store_, &it, kColIcon, "folder", kColName, path.c_str(), kColPath, path.c_str(), kColKind, kDir, kColLoaded, FALSE, -1);
    addPlaceholder(store_, &it, "Loading…");
    GtkTreePath* tp = gtk_tree_model_get_path(GTK_TREE_MODEL(store_), &it);
    gtk_tree_view_expand_row(GTK_TREE_VIEW(tree_), tp, FALSE);
    gtk_tree_path_free(tp);
}

bool Panel::findRow(const std::string& path, GtkTreeIter& out)
{
    if (!store_)
        return false;
    struct Search {
        const std::string& path;
        GtkTreeIter* out;
        bool found;
    } s{path, &out, false};
    gtk_tree_model_foreach(GTK_TREE_MODEL(store_), +[](GtkTreeModel* m, GtkTreePath*, GtkTreeIter* it, gpointer p) -> gboolean {
        auto* s = static_cast<Search*>(p);
        if (getKind(m, it) == kPlaceholder || getPath(m, it) != s->path)
            return FALSE;
        *s->out = *it;
        s->found = true;
        return TRUE;
    }, &s);
    return s.found;
}

void Panel::loadDir(const std::string& path)
{
    if (!session_ || path.empty())
        return;
    auto entries = std::make_shared<std::vector<Entry>>();
    session_->post([path, entries](RemoteClient& c) { return c.list(path, *entries); },
                   [this, path, entries](bool ok, const std::string& error) {
                       GtkTreeIter it;
                       if (!findRow(path, it))
                           return;
                       const int kind = getKind(GTK_TREE_MODEL(store_), &it);
                       if (ok) {
                           fill(&it, std::move(*entries));
                           gtk_tree_store_set(store_, &it, kColLoaded, TRUE, -1);
                           GtkTreePath* tp = gtk_tree_model_get_path(GTK_TREE_MODEL(store_), &it);
                           gtk_tree_view_expand_row(GTK_TREE_VIEW(tree_), tp, FALSE);
                           gtk_tree_path_free(tp);
                           return;
                       }
                       // A link to a file is not a folder after all.
                       GtkTreeIter child;
                       while (gtk_tree_model_iter_children(GTK_TREE_MODEL(store_), &child, &it))
                           gtk_tree_store_remove(store_, &child);
                       if (kind == kLink && error != "cancelled") {
                           gtk_tree_store_set(store_, &it, kColKind, kFile, kColIcon, "emblem-symbolic-link", kColLoaded, TRUE, -1);
                           return;
                       }
                       addPlaceholder(store_, &it, error == "cancelled" ? "(stopped)" : "(cannot list)");
                       if (error != "cancelled")
                           log(LogKind::Error, "Cannot list " + path + ": " + error);
                   });
}

void Panel::fill(GtkTreeIter* parent, std::vector<Entry> entries)
{
    GtkTreeIter child;
    while (gtk_tree_model_iter_children(GTK_TREE_MODEL(store_), &child, parent))
        gtk_tree_store_remove(store_, &child);
    const std::string dir = getPath(GTK_TREE_MODEL(store_), parent);
    // Folders first, then files, by name.
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.isDir != b.isDir)
            return a.isDir;
        const int c = strcasecmp(a.name.c_str(), b.name.c_str());
        return c ? c < 0 : a.name < b.name;
    });
    for (const Entry& e : entries) {
        std::string name = e.name;
        if (!g_utf8_validate(name.c_str(), -1, nullptr)) {
            gchar* fixed = g_utf8_make_valid(name.c_str(), -1);
            name = fixed;
            g_free(fixed);
        }
        const int kind = e.isDir ? kDir : e.isLink ? kLink : kFile;
        const char* icon = e.isDir ? "folder" : e.isLink ? "emblem-symbolic-link" : "text-x-generic";
        const std::string path = joinPath(dir, e.name);
        GtkTreeIter it;
        gtk_tree_store_append(store_, &it, parent);
        gtk_tree_store_set(store_, &it, kColIcon, icon, kColName, name.c_str(), kColPath, path.c_str(), kColKind, kind, kColLoaded, FALSE,
                           kColSize, e.isDir ? "" : formatSize(e.size).c_str(), kColTime, formatTime(e.mtime).c_str(), -1);
        // Folders, and links that may be folders, can be opened.
        if (kind != kFile)
            addPlaceholder(store_, &it, "Loading…");
    }
    if (entries.empty())
        addPlaceholder(store_, parent, "(empty)");
}

bool Panel::selected(std::string& path, int& kind)
{
    if (!tree_)
        return false;
    GtkTreeModel* m;
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree_)), &m, &it))
        return false;
    kind = getKind(m, &it);
    if (kind == kPlaceholder) {
        GtkTreeIter parent;
        if (!gtk_tree_model_iter_parent(m, &parent, &it))
            return false;
        it = parent;
        kind = getKind(m, &it);
    }
    path = getPath(m, &it);
    return !path.empty();
}

std::string Panel::selectedDir()
{
    std::string path;
    int kind;
    if (!selected(path, kind))
        return root_;
    return kind == kFile ? parentPath(path) : path;
}

void Panel::refreshDir(const std::string& dir)
{
    if (dir.empty())
        return;
    GtkTreeIter it;
    if (!findRow(dir, it))
        return;
    gtk_tree_store_set(store_, &it, kColLoaded, FALSE, -1);
    loadDir(dir);
}

void Panel::popupMenu(GdkEventButton* ev)
{
    if (!session_)
        return;
    std::string path;
    int kind = kPlaceholder;
    const bool any = selected(path, kind);
    GtkWidget* menu = gtk_menu_new();
    auto item = [&](const char* label, void (*fn)(Panel*), bool enabled = true) {
        GtkWidget* i = gtk_menu_item_new_with_mnemonic(label);
        gtk_widget_set_sensitive(i, enabled);
        g_signal_connect_swapped(i, "activate", G_CALLBACK(fn), this);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), i);
    };
    auto separator = [&] { gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new()); };
    const bool file = any && kind == kFile;
    const bool isRoot = path == root_;
    item("_Open", +[](Panel* p) {
        std::string path;
        int kind;
        if (p->selected(path, kind) && kind == kFile)
            p->openRemote(path);
    }, file);
    item("Download _Again", +[](Panel* p) {
        std::string path;
        int kind;
        if (p->selected(path, kind) && kind == kFile)
            p->refreshLocalCopy(path);
    }, file);
    item("_Save a Copy As…", +[](Panel* p) {
        std::string path;
        int kind;
        if (p->selected(path, kind) && kind == kFile)
            p->downloadTo(path);
    }, file);
    separator();
    item("_Upload Current Document Here", +[](Panel* p) {
        const std::string local = p->currentFilePath();
        if (local.empty() || !g_path_is_absolute(local.c_str())) {
            p->showError("Save the document first.", "");
            return;
        }
        gchar* base = g_path_get_basename(local.c_str());
        p->uploadFile(local, joinPath(p->selectedDir(), base), true);
        g_free(base);
    });
    item("Upload _File Here…", +[](Panel* p) {
        GtkFileChooserNative* fc = gtk_file_chooser_native_new("Upload", p->mainWindow(), GTK_FILE_CHOOSER_ACTION_OPEN, "_Upload", "_Cancel");
        gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(fc), TRUE);
        if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(fc)) == GTK_RESPONSE_ACCEPT) {
            GSList* files = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(fc));
            const std::string dir = p->selectedDir();
            for (GSList* f = files; f; f = f->next) {
                gchar* base = g_path_get_basename(static_cast<const char*>(f->data));
                p->uploadFile(static_cast<const char*>(f->data), joinPath(dir, base), !f->next);
                g_free(base);
            }
            g_slist_free_full(files, g_free);
        }
        g_object_unref(fc);
    });
    separator();
    item("New Fi_le…", +[](Panel* p) { p->newFile(); });
    item("New Fol_der…", +[](Panel* p) { p->newFolder(); });
    item("_Rename…", +[](Panel* p) { p->renameSelected(); }, any && !isRoot);
    item("De_lete", +[](Panel* p) { p->deleteSelected(); }, any && !isRoot);
    separator();
    item("Re_fresh", +[](Panel* p) { p->refreshDir(p->selectedDir()); });
    item("Copy _Path", +[](Panel* p) {
        std::string path;
        int kind;
        if (p->selected(path, kind))
            gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), path.c_str(), -1);
    }, any);
    item("Make It the _Top Folder", +[](Panel* p) { p->setRoot(p->selectedDir()); }, any && kind != kFile && !isRoot);
    gtk_widget_show_all(menu);
    g_signal_connect(menu, "deactivate", G_CALLBACK(+[](GtkWidget* m, gpointer) {
                         g_idle_add(+[](gpointer w) -> gboolean {
                             gtk_widget_destroy(GTK_WIDGET(w));
                             return G_SOURCE_REMOVE;
                         }, m);
                     }), nullptr);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), reinterpret_cast<GdkEvent*>(ev));
}

// ============================================================================
// Files
// ============================================================================

std::string Panel::cacheFile(const std::string& remote) const
{
    return cachePath(cacheRoot_, session_ ? session_->profile().name : "_", remote);
}

const Profile* Panel::profileByFolder(const std::string& folder) const
{
    for (const Profile& p : settings_.profiles)
        if (profileFolder(p.name) == folder)
            return &p;
    return nullptr;
}

std::string Panel::currentFilePath() const
{
    const LRESULT id = ::SendMessage(npp_._nppHandle, NPPM_GETCURRENTBUFFERID, 0, 0);
    const LRESULT len = ::SendMessage(npp_._nppHandle, NPPM_GETFULLPATHFROMBUFFERID, id, 0);
    if (len <= 0)
        return {};
    std::wstring w(static_cast<std::size_t>(len) + 1, L'\0');
    ::SendMessage(npp_._nppHandle, NPPM_GETFULLPATHFROMBUFFERID, id, reinterpret_cast<LPARAM>(w.data()));
    return narrow(w.c_str());
}

void Panel::openRemote(const std::string& remote)
{
    if (!session_)
        return;
    const std::string local = cacheFile(remote);
    // Open already: show it, with whatever changes it has.
    if (::SendMessage(npp_._nppHandle, NPPM_SWITCHTOFILE, 0, reinterpret_cast<LPARAM>(wide(local).c_str())))
        return;
    gchar* dir = g_path_get_dirname(local.c_str());
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    session_->post([remote, local](RemoteClient& c) { return c.download(remote, local); },
                   [this, remote, local](bool ok, const std::string& error) {
                       if (!ok) {
                           if (error != "cancelled")
                               showError("Cannot download " + remote, error);
                           return;
                       }
                       log(LogKind::Info, "Downloaded " + remote);
                       ::SendMessage(npp_._nppHandle, NPPM_DOOPEN, 0, reinterpret_cast<LPARAM>(wide(local).c_str()));
                   });
}

void Panel::refreshLocalCopy(const std::string& remote)
{
    if (!session_)
        return;
    const std::string local = cacheFile(remote);
    const std::wstring wlocal = wide(local);
    const bool open = ::SendMessage(npp_._nppHandle, NPPM_SWITCHTOFILE, 0, reinterpret_cast<LPARAM>(wlocal.c_str()));
    if (open) {
        int view = 0;
        ::SendMessage(npp_._nppHandle, NPPM_GETCURRENTSCINTILLA, 0, reinterpret_cast<LPARAM>(&view));
        const HWND sci = view == 0 ? npp_._scintillaMainHandle : npp_._scintillaSecondHandle;
        if (::SendMessage(sci, SCI_GETMODIFY, 0, 0) && !confirm(remote + " has unsaved changes. Download it again and lose them?", "_Download"))
            return;
    }
    gchar* dir = g_path_get_dirname(local.c_str());
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    session_->post([remote, local](RemoteClient& c) { return c.download(remote, local); },
                   [this, remote, wlocal, open](bool ok, const std::string& error) {
                       if (!ok) {
                           if (error != "cancelled")
                               showError("Cannot download " + remote, error);
                           return;
                       }
                       log(LogKind::Info, "Downloaded " + remote);
                       if (open)
                           ::SendMessage(npp_._nppHandle, NPPM_RELOADFILE, FALSE, reinterpret_cast<LPARAM>(wlocal.c_str()));
                       else
                           ::SendMessage(npp_._nppHandle, NPPM_DOOPEN, 0, reinterpret_cast<LPARAM>(wlocal.c_str()));
                   });
}

void Panel::downloadTo(const std::string& remote)
{
    if (!session_)
        return;
    GtkFileChooserNative* fc = gtk_file_chooser_native_new("Save a Copy", mainWindow(), GTK_FILE_CHOOSER_ACTION_SAVE, "_Save", "_Cancel");
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(fc), baseName(remote).c_str());
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(fc), TRUE);
    std::string local;
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(fc)) == GTK_RESPONSE_ACCEPT) {
        gchar* f = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(fc));
        local = f ? f : "";
        g_free(f);
    }
    g_object_unref(fc);
    if (local.empty())
        return;
    session_->post([remote, local](RemoteClient& c) { return c.download(remote, local); },
                   [this, remote, local](bool ok, const std::string& error) {
                       if (ok)
                           log(LogKind::Info, "Saved " + remote + " as " + local);
                       else if (error != "cancelled")
                           showError("Cannot download " + remote, error);
                   });
}

void Panel::uploadFile(const std::string& local, const std::string& remote, bool refreshAfter)
{
    if (!session_)
        return;
    session_->post([local, remote](RemoteClient& c) { return c.upload(local, remote); },
                   [this, remote, refreshAfter](bool ok, const std::string& error) {
                       if (!ok) {
                           if (error != "cancelled")
                               showError("Cannot upload " + remote, error);
                           return;
                       }
                       log(LogKind::Info, "Uploaded " + remote);
                       if (refreshAfter)
                           refreshDir(parentPath(remote));
                   });
}

void Panel::uploadCurrent()
{
    const std::string local = currentFilePath();
    if (local.empty() || !g_path_is_absolute(local.c_str())) {
        showError("Save the document first.", "A new document has no file to upload.");
        return;
    }
    std::string folder, remote;
    if (fromCachePath(cacheRoot_, local, folder, remote)) {
        fileSaved(local); // back where it came from
        return;
    }
    if (!session_) {
        showError("Not connected.", "Connect to a server and select the folder to upload to.");
        return;
    }
    gchar* base = g_path_get_basename(local.c_str());
    const std::string target = joinPath(selectedDir(), base);
    g_free(base);
    GtkTreeIter it;
    if (findRow(target, it) && !confirm(target + " exists. Replace it?", "_Replace"))
        return;
    uploadFile(local, target, true);
}

void Panel::fileSaved(const std::string& path)
{
    if (shutDown_)
        return;
    std::string folder, remote;
    if (!fromCachePath(cacheRoot_, path, folder, remote))
        return;
    const Profile* p = profileByFolder(folder);
    if (!p) {
        log(LogKind::Error, "Not uploaded: no profile for " + path);
        return;
    }
    if (session_ && session_->profile().name != p->name) {
        if (!confirm(remote + " belongs to \"" + p->name + "\", not to \"" + session_->profile().name +
                         "\". Disconnect, connect to \"" + p->name + "\" and upload it?",
                     "_Upload"))
            return;
        connectTo(p->name);
    } else if (!session_) {
        connectTo(p->name);
    }
    if (!session_)
        return;
    log(LogKind::Info, "Uploading " + remote);
    uploadFile(path, remote, true);
}

void Panel::newFile()
{
    if (!session_)
        return;
    const std::string dir = selectedDir();
    std::string name = "new.txt";
    if (!askText("New File", "Name of the new file in " + dir + ":", name))
        return;
    if (!validName(name)) {
        showError("\"" + name + "\" is not a file name.", "");
        return;
    }
    const std::string remote = joinPath(dir, name);
    GtkTreeIter it;
    if (findRow(remote, it)) {
        showError(remote + " exists.", "");
        return;
    }
    const std::string local = cacheFile(remote);
    gchar* d = g_path_get_dirname(local.c_str());
    g_mkdir_with_parents(d, 0700);
    g_free(d);
    const int fd = ::open(local.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        showError("Cannot create " + local, std::strerror(errno));
        return;
    }
    ::close(fd);
    session_->post([local, remote](RemoteClient& c) { return c.upload(local, remote); },
                   [this, remote, local, dir](bool ok, const std::string& error) {
                       if (!ok) {
                           if (error != "cancelled")
                               showError("Cannot create " + remote, error);
                           return;
                       }
                       log(LogKind::Info, "Created " + remote);
                       refreshDir(dir);
                       ::SendMessage(npp_._nppHandle, NPPM_DOOPEN, 0, reinterpret_cast<LPARAM>(wide(local).c_str()));
                   });
}

void Panel::newFolder()
{
    if (!session_)
        return;
    const std::string dir = selectedDir();
    std::string name = "New Folder";
    if (!askText("New Folder", "Name of the new folder in " + dir + ":", name))
        return;
    if (!validName(name)) {
        showError("\"" + name + "\" is not a folder name.", "");
        return;
    }
    const std::string path = joinPath(dir, name);
    session_->post([path](RemoteClient& c) { return c.makeDir(path); }, [this, path, dir](bool ok, const std::string& error) {
        if (!ok) {
            if (error != "cancelled")
                showError("Cannot create " + path, error);
            return;
        }
        log(LogKind::Info, "Created " + path);
        refreshDir(dir);
    });
}

void Panel::renameSelected()
{
    std::string path;
    int kind;
    if (!session_ || !selected(path, kind) || path == root_)
        return;
    std::string name = baseName(path);
    if (!askText("Rename", "New name of " + path + ":", name) || name == baseName(path))
        return;
    if (!validName(name)) {
        showError("\"" + name + "\" is not a valid name.", "");
        return;
    }
    const std::string dir = parentPath(path);
    const std::string to = joinPath(dir, name);
    session_->post([path, to](RemoteClient& c) { return c.rename(path, to); }, [this, path, to, dir](bool ok, const std::string& error) {
        if (!ok) {
            if (error != "cancelled")
                showError("Cannot rename " + path, error);
            return;
        }
        log(LogKind::Info, "Renamed " + path + " to " + to);
        refreshDir(dir);
    });
}

namespace {

// A folder and everything in it, depth first.
bool removeTree(RemoteClient& c, const std::string& dir)
{
    std::vector<Entry> entries;
    if (!c.list(dir, entries))
        return false;
    for (const Entry& e : entries) {
        const std::string p = joinPath(dir, e.name);
        if (!(e.isDir && !e.isLink ? removeTree(c, p) : c.removeFile(p)))
            return false;
    }
    return c.removeDir(dir);
}

} // namespace

void Panel::deleteSelected()
{
    std::string path;
    int kind;
    if (!session_ || !selected(path, kind) || path == root_)
        return;
    const bool dir = kind == kDir;
    if (!confirm(dir ? "Delete the folder " + path + " and everything in it?" : "Delete " + path + "?", "_Delete"))
        return;
    const std::string parent = parentPath(path);
    session_->post(
        [path, dir](RemoteClient& c) { return dir ? removeTree(c, path) : c.removeFile(path); },
        [this, path, parent](bool ok, const std::string& error) {
            if (!ok) {
                if (error != "cancelled")
                    showError("Cannot delete " + path, error);
                refreshDir(parent);
                return;
            }
            log(LogKind::Info, "Deleted " + path);
            refreshDir(parent);
        });
}

} // namespace nppremote
