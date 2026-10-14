#include "ui/TerminalPanel.h"

#include "platform/Sandbox.h"

#include <Scintilla.h>

#include <dlfcn.h>
#include <pwd.h>
#include <unistd.h>

#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace npp::ui {

namespace {

// The part of VTE's API the panel uses (vte/vteterminal.h, vte-2.91).
using SpawnCallback = void (*)(GtkWidget* term, GPid pid, GError* error, gpointer data);
struct Vte {
    GtkWidget* (*terminalNew)();
    void (*spawnAsync)(GtkWidget*, int ptyFlags, const char* wd, char** argv, char** envv, GSpawnFlags, GSpawnChildSetupFunc, gpointer,
                       GDestroyNotify, int timeout, GCancellable*, SpawnCallback, gpointer);
    void (*feed)(GtkWidget*, const char*, gssize);
    void (*feedChild)(GtkWidget*, const char*, gssize);
    void (*copyClipboardFormat)(GtkWidget*, int format);
    void (*pasteClipboard)(GtkWidget*);
    void (*selectAll)(GtkWidget*);
    gboolean (*getHasSelection)(GtkWidget*);
    void (*setColors)(GtkWidget*, const GdkRGBA* fg, const GdkRGBA* bg, const GdkRGBA* palette, gsize paletteSize);
    void (*setFont)(GtkWidget*, const PangoFontDescription*);
    void (*setScrollbackLines)(GtkWidget*, glong);
};
constexpr int kPtyDefault = 0;
constexpr int kFormatText = 1;

const char* g_library = "libvte-2.91.so.0"; // the GTK 3 one; vte-2.91-gtk4 is another library
bool g_tried = false;

const Vte* vte()
{
    static const Vte* loaded = nullptr;
    if (g_tried)
        return loaded;
    g_tried = true;
    loaded = [] () -> const Vte* {
        void* h = dlopen(g_library, RTLD_NOW | RTLD_LOCAL);
        if (!h)
            return nullptr;
        static Vte v;
        bool ok = true;
        auto sym = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(h, name));
            ok = ok && fn;
        };
        sym(v.terminalNew, "vte_terminal_new");
        sym(v.spawnAsync, "vte_terminal_spawn_async");
        sym(v.feed, "vte_terminal_feed");
        sym(v.feedChild, "vte_terminal_feed_child");
        sym(v.copyClipboardFormat, "vte_terminal_copy_clipboard_format");
        sym(v.pasteClipboard, "vte_terminal_paste_clipboard");
        sym(v.selectAll, "vte_terminal_select_all");
        sym(v.getHasSelection, "vte_terminal_get_has_selection");
        sym(v.setColors, "vte_terminal_set_colors");
        sym(v.setFont, "vte_terminal_set_font");
        sym(v.setScrollbackLines, "vte_terminal_set_scrollback_lines");
        return ok ? &v : nullptr; // too old a VTE (spawn_async is 0.48)
    }();
    return loaded;
}

// The user's login shell: $SHELL, else the passwd entry, else sh.
std::string userShell()
{
    if (const char* s = g_getenv("SHELL"); s && *s && access(s, X_OK) == 0)
        return s;
    if (const passwd* pw = getpwuid(getuid()); pw && pw->pw_shell && *pw->pw_shell && access(pw->pw_shell, X_OK) == 0)
        return pw->pw_shell;
    return "/bin/sh";
}

GdkRGBA rgba(std::uint32_t bgr)
{
    return {(bgr & 0xFF) / 255.0, ((bgr >> 8) & 0xFF) / 255.0, ((bgr >> 16) & 0xFF) / 255.0, 1.0};
}

// Single quotes for the shell.
std::string quoted(const std::string& s)
{
    std::string out = "'";
    for (char c : s)
        out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}

} // namespace

bool TerminalPanel::available()
{
    return vte() != nullptr;
}

void TerminalPanel::setLibraryForTests(const char* soname)
{
    g_library = soname;
    g_tried = false;
}

TerminalPanel::TerminalPanel(std::function<void()> exited, std::function<fs::path()> currentFolder)
    : exited_(std::move(exited)), currentFolder_(std::move(currentFolder)), alive_(std::make_shared<TerminalPanel*>(this))
{
    box_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    g_object_ref_sink(box_);
}

TerminalPanel::~TerminalPanel()
{
    if (term_)
        g_object_remove_weak_pointer(G_OBJECT(term_), reinterpret_cast<gpointer*>(&term_));
    g_object_unref(box_);
}

void TerminalPanel::build()
{
    built_ = true;
    const Vte* v = vte();
    if (!v) {
        GtkWidget* label = gtk_label_new("The terminal needs the VTE library for GTK 3: "
                                         "libvte-2.91-0 on Debian and Ubuntu, vte291 on Fedora, vte3 on Arch.");
        gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
        gtk_label_set_selectable(GTK_LABEL(label), TRUE);
        gtk_widget_set_margin_start(label, 12);
        gtk_widget_set_margin_end(label, 12);
        gtk_box_pack_start(GTK_BOX(box_), label, TRUE, TRUE, 0);
        gtk_widget_show_all(box_);
        return;
    }
    term_ = v->terminalNew();
    g_object_add_weak_pointer(G_OBJECT(term_), reinterpret_cast<gpointer*>(&term_));
    v->setScrollbackLines(term_, 10000);
    gtk_widget_set_hexpand(term_, TRUE);
    gtk_widget_set_vexpand(term_, TRUE);
    GtkWidget* bar = gtk_scrollbar_new(GTK_ORIENTATION_VERTICAL, gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(term_)));
    gtk_box_pack_start(GTK_BOX(box_), term_, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box_), bar, FALSE, FALSE, 0);
    gtk_widget_show_all(box_);
    applyStyle();

    // Keys must not reach it before it is realized: focus() waits for it to be mapped.
    g_signal_connect(term_, "map", G_CALLBACK(+[](GtkWidget* t, gpointer p) {
                         auto* self = static_cast<TerminalPanel*>(p);
                         if (std::exchange(self->focusOnMap_, false))
                             gtk_widget_grab_focus(t);
                     }), this);
    g_signal_connect(term_, "child-exited", G_CALLBACK(+[](GtkWidget* t, gint, gpointer p) {
                         auto* self = static_cast<TerminalPanel*>(p);
                         self->pid_ = 0;
                         if (!gtk_widget_in_destruction(t) && self->exited_)
                             self->exited_();
                     }), this);
    // Terminal keys: Ctrl+Shift+C / Ctrl+Shift+V, as GNOME Terminal; the rest goes to the shell.
    g_signal_connect(term_, "key-press-event", G_CALLBACK(+[](GtkWidget*, GdkEventKey* ev, gpointer p) -> gboolean {
                         auto* self = static_cast<TerminalPanel*>(p);
                         const guint mods = ev->state & gtk_accelerator_get_default_mod_mask();
                         if (mods != (GDK_CONTROL_MASK | GDK_SHIFT_MASK))
                             return FALSE;
                         switch (gdk_keyval_to_lower(ev->keyval)) {
                         case GDK_KEY_c: self->copy(); return TRUE;
                         case GDK_KEY_v: self->paste(); return TRUE;
                         default: return FALSE;
                         }
                     }), this);
    g_signal_connect(term_, "button-press-event", G_CALLBACK(+[](GtkWidget*, GdkEventButton* ev, gpointer p) -> gboolean {
                         if (ev->type != GDK_BUTTON_PRESS || ev->button != 3)
                             return FALSE;
                         static_cast<TerminalPanel*>(p)->showMenu(reinterpret_cast<GdkEvent*>(ev));
                         return TRUE;
                     }), this);
}

void TerminalPanel::start(const fs::path& dir)
{
    if (!built_)
        build();
    const Vte* v = vte();
    if (!v || !term_ || pid_ != 0)
        return;
    std::error_code ec;
    const fs::path wd = !dir.empty() && fs::is_directory(dir, ec) ? dir : fs::path(g_get_home_dir());
    std::vector<std::string> args;
    if (platform::currentSandbox() == platform::Sandbox::Flatpak) // the host's shell
        args = platform::hostCommand({"sh", "-c", "exec \"${SHELL:-/bin/sh}\""}, wd, platform::Sandbox::Flatpak);
    else
        args = {userShell()};
    std::vector<char*> argv;
    for (std::string& a : args)
        argv.push_back(a.data());
    argv.push_back(nullptr);
    pid_ = -1;
    v->spawnAsync(term_, kPtyDefault, wd.c_str(), argv.data(), nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr, -1, nullptr,
                  +[](GtkWidget* t, GPid pid, GError* error, gpointer data) {
                      std::unique_ptr<std::weak_ptr<TerminalPanel*>> weak(static_cast<std::weak_ptr<TerminalPanel*>*>(data));
                      auto alive = weak->lock();
                      if (!alive)
                          return;
                      TerminalPanel* self = *alive;
                      self->pid_ = error ? 0 : pid;
                      if (error && self->term_ == t) {
                          const std::string msg = std::string("Cannot start the shell: ") + error->message + "\r\n";
                          vte()->feed(t, msg.c_str(), static_cast<gssize>(msg.size()));
                      }
                  },
                  new std::weak_ptr<TerminalPanel*>(alive_));
}

void TerminalPanel::type(const std::string& text)
{
    if (term_ && pid_ > 0)
        vte()->feedChild(term_, text.data(), static_cast<gssize>(text.size()));
}

void TerminalPanel::changeDirectory(const fs::path& dir)
{
    // Ctrl+E Ctrl+U clears what was typed on the line first (readline, zle).
    if (!dir.empty())
        type("\x05\x15" "cd -- " + quoted(dir.string()) + "\r");
}

void TerminalPanel::focus()
{
    if (!term_)
        return;
    if (gtk_widget_get_mapped(term_))
        gtk_widget_grab_focus(term_);
    else
        focusOnMap_ = true;
}

bool TerminalPanel::hasFocus() const
{
    return term_ && gtk_widget_is_focus(term_); // the window's focus widget, active or not
}

void TerminalPanel::copy()
{
    if (term_ && vte()->getHasSelection(term_))
        vte()->copyClipboardFormat(term_, kFormatText);
}

void TerminalPanel::paste()
{
    if (term_)
        vte()->pasteClipboard(term_);
}

void TerminalPanel::showMenu(GdkEvent* ev)
{
    GtkWidget* menu = gtk_menu_new();
    enum Action { Copy, Paste, SelectAll, GoToFolder };
    auto item = [&](const char* text, bool enabled, Action a) {
        GtkWidget* mi = gtk_menu_item_new_with_mnemonic(text);
        gtk_widget_set_sensitive(mi, enabled);
        g_object_set_data(G_OBJECT(mi), "action", GINT_TO_POINTER(a));
        g_signal_connect(mi, "activate", G_CALLBACK(+[](GtkMenuItem* m, gpointer p) {
                             auto* self = static_cast<TerminalPanel*>(p);
                             switch (static_cast<Action>(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(m), "action")))) {
                             case Copy: self->copy(); break;
                             case Paste: self->paste(); break;
                             case SelectAll: vte()->selectAll(self->term_); break;
                             case GoToFolder: self->changeDirectory(self->currentFolder_()); break;
                             }
                         }), this);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    };
    item("_Copy", vte()->getHasSelection(term_), Copy);
    item("_Paste", true, Paste);
    item("Select _All", true, SelectAll);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    const bool hasFolder = currentFolder_ && !currentFolder_().empty();
    item("_Go to Current Document's Folder", hasFolder && pid_ > 0, GoToFolder);
    gtk_widget_show_all(menu);
    gtk_menu_attach_to_widget(GTK_MENU(menu), term_, nullptr);
    g_signal_connect(menu, "deactivate", G_CALLBACK(+[](GtkMenuShell* m, gpointer) {
                         // Destroyed after the activated item ran.
                         g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, +[](gpointer w) -> gboolean {
                             gtk_widget_destroy(GTK_WIDGET(w));
                             return G_SOURCE_REMOVE;
                         }, m, nullptr);
                     }), nullptr);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), ev);
}

void TerminalPanel::applyStyles(const core::StyleCatalog& styles)
{
    if (const core::Style* def = styles.globalById(STYLE_DEFAULT)) {
        fg_ = def->fg.value_or(0x000000);
        bg_ = def->bg.value_or(0xFFFFFF);
        font_ = (def->fontName.empty() ? std::string("Monospace") : def->fontName) + " " + std::to_string(def->fontSize.value_or(10));
    }
    applyStyle();
}

void TerminalPanel::applyStyle()
{
    if (!term_)
        return;
    const GdkRGBA fg = rgba(fg_), bg = rgba(bg_);
    vte()->setColors(term_, &fg, &bg, nullptr, 0); // VTE's own 16-colour palette
    PangoFontDescription* desc = pango_font_description_from_string(font_.c_str());
    vte()->setFont(term_, desc);
    pango_font_description_free(desc);
}

} // namespace npp::ui
