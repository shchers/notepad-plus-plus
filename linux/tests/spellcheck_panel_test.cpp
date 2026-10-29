// The bundled Spell Checker plugin (linux/plugins/SpellCheck) in a live
// MainWindow: misspellings get its indicator (in source code only in
// comments), a right-click on one offers suggestions that replace it, Add to
// Dictionary keeps a word, and Next Misspelling selects them in turn.
// Skipped without Enchant and its en_US dictionary.

#include "core/LanguageCatalog.h"
#include "ui/MainWindow.h"

#include <windows.h>
#include "PluginInterface.h"

#include "Speller.h"

#include <gtk/gtk.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
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
        g_usleep(5000);
    }
    return true;
}

void writeFile(const fs::path& p, const std::string& text)
{
    std::ofstream(p, std::ios::binary) << text;
}

std::string readFile(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// Whether a squiggle indicator covers pos.
bool marked(ui::EditorView* v, sptr_t pos)
{
    for (int i = 0; i <= INDICATOR_MAX; ++i)
        if (v->execute(SCI_INDICGETSTYLE, static_cast<uptr_t>(i)) == INDIC_SQUIGGLEPIXMAP && v->execute(SCI_INDICATORVALUEAT, static_cast<uptr_t>(i), pos))
            return true;
    return false;
}

sptr_t find(ui::EditorView* v, const std::string& what)
{
    const sptr_t len = v->execute(SCI_GETLENGTH);
    std::string text(static_cast<std::size_t>(len) + 1, '\0');
    v->execute(SCI_GETTEXT, static_cast<uptr_t>(len + 1), reinterpret_cast<sptr_t>(text.data()));
    const std::size_t at = text.find(what);
    return at == std::string::npos ? -1 : static_cast<sptr_t>(at);
}

std::string selection(ui::EditorView* v)
{
    const sptr_t len = v->execute(SCI_GETSELTEXT);
    std::string s(static_cast<std::size_t>(len) + 1, '\0');
    v->execute(SCI_GETSELTEXT, 0, reinterpret_cast<sptr_t>(s.data()));
    return s.c_str();
}

int commandId(ui::MainWindow& w, const std::string& name)
{
    for (const auto& c : w.plugins()->commands()) {
        std::string plain = c.name;
        plain.erase(std::remove(plain.begin(), plain.end(), '&'), plain.end());
        if (plain == name)
            return c.cmdId;
    }
    return -1;
}

// Right-clicks pos; true when the plugin showed its menu, which `menu` gets.
bool rightClick(ui::EditorView* v, sptr_t pos, GtkMenu** menu)
{
    GtkWidget* w = v->widget();
    GdkEvent* ev = gdk_event_new(GDK_BUTTON_PRESS);
    ev->button.window = GDK_WINDOW(g_object_ref(gtk_widget_get_window(w)));
    ev->button.button = 3;
    ev->button.x = static_cast<double>(v->execute(SCI_POINTXFROMPOSITION, 0, pos) + 2);
    ev->button.y = static_cast<double>(v->execute(SCI_POINTYFROMPOSITION, 0, pos) + 2);
    ev->button.time = GDK_CURRENT_TIME;
    GdkSeat* seat = gdk_display_get_default_seat(gtk_widget_get_display(w));
    gdk_event_set_device(ev, gdk_seat_get_pointer(seat));
    gboolean handled = FALSE;
    g_signal_emit_by_name(w, "button-press-event", ev, &handled);
    gdk_event_free(ev);
    *menu = nullptr;
    for (GList* l = gtk_menu_get_for_attach_widget(w); l; l = l->next)
        *menu = GTK_MENU(l->data);
    return handled;
}

std::vector<GtkMenuItem*> items(GtkMenu* menu)
{
    std::vector<GtkMenuItem*> out;
    GList* children = gtk_container_get_children(GTK_CONTAINER(menu));
    for (GList* c = children; c; c = c->next)
        if (GTK_IS_MENU_ITEM(c->data) && !GTK_IS_SEPARATOR_MENU_ITEM(c->data))
            out.push_back(GTK_MENU_ITEM(c->data));
    g_list_free(children);
    return out;
}

GtkMenuItem* itemLabelled(GtkMenu* menu, const std::string& label)
{
    for (GtkMenuItem* it : items(menu)) {
        GtkWidget* l = gtk_bin_get_child(GTK_BIN(it));
        if (GTK_IS_LABEL(l) && label == gtk_label_get_text(GTK_LABEL(l)))
            return it;
    }
    return nullptr;
}

} // namespace

int main(int argc, char** argv)
{
    char tmpl[] = "/tmp/nppgtk-spell-editor-XXXXXX";
    const char* rootDir = g_mkdtemp(tmpl);
    if (!rootDir)
        return 2;
    const fs::path tmp = rootDir;
    // Dictionaries the developer keeps in their own configuration
    // (~/.config/enchant/hunspell) stay visible.
    // (Not g_get_user_config_dir(), which would keep the old value.)
    const char* ownConfig = g_getenv("XDG_CONFIG_HOME");
    const fs::path ownDicts = (ownConfig && *ownConfig ? fs::path(ownConfig) : fs::path(g_get_home_dir()) / ".config") / "enchant";
    fs::create_directories(tmp / "config");
    if (fs::is_directory(ownDicts))
        fs::copy(ownDicts, tmp / "config" / "enchant", fs::copy_options::recursive);
    g_setenv("XDG_CONFIG_HOME", (tmp / "config").c_str(), TRUE);
    g_setenv("NPP_GTK_DATA_DIR", NPP_TEST_DATA_DIR, TRUE);

    bool german = false;
    {
        spellcheck::Speller probe;
        const std::vector<std::string> dicts = probe.available() ? probe.dictionaries() : std::vector<std::string>();
        german = std::find(dicts.begin(), dicts.end(), "de_DE") != dicts.end();
        if (std::find(dicts.begin(), dicts.end(), "en_US") == dicts.end()) {
            std::printf("SKIP: %s\n", probe.available() ? "no en_US dictionary (hunspell-en-us)" : probe.error().c_str());
            fs::remove_all(tmp);
            return 0;
        }
    }

    const fs::path config = tmp / "config" / "notepad-plus-plus-gtk" / "plugins" / "config";
    fs::create_directories(config);
    writeFile(config / "SpellCheck.ini", "[SpellCheck]\nEnabled=true\nLanguages=en_US;\n");
    // Detected per paragraph: the German one is checked in German.
    writeFile(tmp / "notes.txt", "This sentense has a mistake.\nThe second line is fine.\nAnother eror here.\n\n"
                                 "Das ist ein deutscher Absatz mit einem Fehlr darin.\n");
    writeFile(tmp / "code.cpp", "int mispeled = 0; // a commnet\nconst char* s = \"a strng\";\n");

    gtk_init(&argc, &argv);
    core::LanguageCatalog langs;
    std::string err;
    CHECK(langs.load(fs::path(NPP_TEST_DATA_DIR) / "langs.model.xml", &err));
    GtkApplication* app = gtk_application_new("org.notepad_plus_plus.GtkPortSpellTests", G_APPLICATION_NON_UNIQUE);
    CHECK(g_application_register(G_APPLICATION(app), nullptr, nullptr));
    {
        auto w = std::make_unique<ui::MainWindow>(app, langs, false);
        w->setPromptHandler([](const std::string&, const std::string&, bool) { return ui::MainWindow::Answer::No; });
        w->loadPlugins({fs::path(NPP_BUNDLED_PLUGINS_DIR)});
        w->newDocument();
        w->present();
        pump();
        w->pluginsReady();
        pump();
        CHECK(commandId(*w, "Next Misspelling") > 0);

        std::printf("plain text: misspellings are marked...\n");
        CHECK(w->openFiles({tmp / "notes.txt"}) == 1);
        ui::EditorView* v = w->activeView();
        const sptr_t sentense = find(v, "sentense");
        CHECK(waitFor([&] { return marked(v, sentense + 1); }));
        CHECK(marked(v, find(v, "eror") + 1));
        CHECK(!marked(v, find(v, "This") + 1));
        CHECK(!marked(v, find(v, "second") + 1));
        if (german) {
            std::printf("a German paragraph is checked in German...\n");
            CHECK(marked(v, find(v, "Fehlr") + 1));
            CHECK(!marked(v, find(v, "deutscher") + 1));
            CHECK(!marked(v, find(v, "Absatz") + 1));
            GtkMenu* de = nullptr;
            CHECK(rightClick(v, find(v, "Fehlr") + 1, &de) && de);
            if (de) {
                CHECK(itemLabelled(de, "Fehler") != nullptr);
                CHECK(itemLabelled(de, "Language: de_DE") != nullptr);
                gtk_menu_shell_deactivate(GTK_MENU_SHELL(de));
                pump();
            }
        } else {
            std::printf("SKIP German paragraph: no de_DE dictionary\n");
        }

        std::printf("a right-click offers suggestions...\n");
        GtkMenu* menu = nullptr;
        CHECK(rightClick(v, sentense + 2, &menu));
        CHECK(menu != nullptr);
        if (menu) {
            CHECK(itemLabelled(menu, "Add “sentense” to Dictionary") != nullptr);
            CHECK(itemLabelled(menu, "Copy") != nullptr);
            GtkMenuItem* fix = itemLabelled(menu, "sentence");
            CHECK(fix != nullptr);
            if (fix)
                gtk_menu_item_activate(fix);
            gtk_menu_shell_deactivate(GTK_MENU_SHELL(menu));
            pump();
        }
        CHECK(find(v, "This sentence has") == 0);
        CHECK(waitFor([&] { return !marked(v, 6); }));
        // A correct word: the click goes on to Scintilla (its own menu),
        // which this handler, run after the plugin's, stands in for.
        {
            bool passedOn = false;
            const gulong id = g_signal_connect(v->widget(), "button-press-event", G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer p) -> gboolean {
                                                   *static_cast<bool*>(p) = true;
                                                   return TRUE;
                                               }), &passedOn);
            rightClick(v, find(v, "second") + 1, &menu);
            CHECK(passedOn);
            g_signal_handler_disconnect(v->widget(), id);
        }

        std::printf("Add to Dictionary keeps the word...\n");
        CHECK(rightClick(v, find(v, "eror") + 1, &menu) && menu);
        if (menu) {
            if (GtkMenuItem* add = itemLabelled(menu, "Add “eror” to Dictionary"))
                gtk_menu_item_activate(add);
            else
                CHECK(!"no Add item");
            gtk_menu_shell_deactivate(GTK_MENU_SHELL(menu));
            pump();
        }
        CHECK(waitFor([&] { return !marked(v, find(v, "eror") + 1); }));
        CHECK(readFile(config / "SpellCheck.dic").find("eror") != std::string::npos);

        std::printf("source code: comments and strings only...\n");
        CHECK(w->openFiles({tmp / "code.cpp"}) == 1);
        v = w->activeView();
        CHECK(waitFor([&] { return marked(v, find(v, "commnet") + 1); }));
        CHECK(marked(v, find(v, "strng") + 1));
        CHECK(!marked(v, find(v, "mispeled") + 1));

        std::printf("typing: the word is marked when the caret leaves it...\n");
        v->execute(SCI_DOCUMENTEND);
        v->execute(SCI_NEWLINE);
        v->execute(SCI_ADDTEXT, 3, reinterpret_cast<sptr_t>("// "));
        for (const char* ch : {"t", "y", "p", "o", "o"}) // keystrokes, as the user types them
            v->execute(SCI_REPLACESEL, 0, reinterpret_cast<sptr_t>(ch));
        const sptr_t typo = find(v, "typoo");
        pump();
        g_usleep(400 * 1000);
        pump();
        CHECK(!marked(v, typo + 1));
        v->execute(SCI_REPLACESEL, 0, reinterpret_cast<sptr_t>(" "));
        v->execute(SCI_HOME);
        CHECK(waitFor([&] { return marked(v, typo + 1); }));

        std::printf("Next and Previous Misspelling...\n");
        v->execute(SCI_GOTOPOS, 0);
        const int next = commandId(*w, "Next Misspelling");
        const int previous = commandId(*w, "Previous Misspelling");
        CHECK(w->runCommand(next) && selection(v) == "commnet");
        CHECK(w->runCommand(next) && selection(v) == "strng");
        CHECK(w->runCommand(next) && selection(v) == "typoo");
        CHECK(w->runCommand(next) && selection(v) == "commnet"); // round the end
        CHECK(w->runCommand(previous) && selection(v) == "typoo");

        std::printf("Check Spelling as You Type off clears the marks...\n");
        CHECK(w->runCommand(commandId(*w, "Check Spelling as You Type")));
        CHECK(waitFor([&] { return !marked(v, find(v, "commnet") + 1); }));
        CHECK(readFile(config / "SpellCheck.ini").find("Enabled=false") != std::string::npos);
        CHECK(w->runCommand(commandId(*w, "Check Spelling as You Type")));
        CHECK(waitFor([&] { return marked(v, find(v, "commnet") + 1); }));

        CHECK(w->requestQuit());
    }
    g_object_unref(app);
    fs::remove_all(tmp);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
