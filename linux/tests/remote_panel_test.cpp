// The bundled Remote Files plugin (linux/plugins/NppRemote) in a live
// MainWindow, against the test FTP server: saving a file of its cache
// connects and uploads it, the panel lists the server, a double-click opens
// a remote file in a tab, and saving that sends it back.

#include "core/LanguageCatalog.h"
#include "platform/FileIO.h"
#include "ui/MainWindow.h"

#include <windows.h>
#include "PluginInterface.h"

#include "FakeFtpServer.h"

#include <gtk/gtk.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

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
bool waitFor(F cond, int ms = 15000)
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

GtkTreeView* findTree(GtkWidget* w)
{
    if (GTK_IS_TREE_VIEW(w))
        return GTK_TREE_VIEW(w);
    GtkTreeView* found = nullptr;
    if (GTK_IS_CONTAINER(w)) {
        GList* children = gtk_container_get_children(GTK_CONTAINER(w));
        for (GList* c = children; c && !found; c = c->next)
            found = findTree(GTK_WIDGET(c->data));
        g_list_free(children);
    }
    return found;
}

// The row named `name` (column 1, the panel's name column).
GtkTreePath* findRow(GtkTreeView* tv, const std::string& name)
{
    struct Search {
        std::string name;
        GtkTreePath* path = nullptr;
    } s{name};
    gtk_tree_model_foreach(gtk_tree_view_get_model(tv), +[](GtkTreeModel* m, GtkTreePath* p, GtkTreeIter* it, gpointer data) -> gboolean {
        auto* s = static_cast<Search*>(data);
        gchar* n = nullptr;
        gtk_tree_model_get(m, it, 1, &n, -1);
        const bool hit = n && s->name == n;
        g_free(n);
        if (hit)
            s->path = gtk_tree_path_copy(p);
        return hit;
    }, &s);
    return s.path;
}

std::string activePath(ui::MainWindow& w)
{
    return w.activeBuffer() ? w.activeBuffer()->fullName() : std::string();
}

} // namespace

int main(int argc, char** argv)
{
    char tmpl[] = "/tmp/nppgtk-remote-panel-XXXXXX";
    const char* rootDir = g_mkdtemp(tmpl);
    if (!rootDir)
        return 2;
    const fs::path tmp = rootDir;
    // Before GLib reads them.
    g_setenv("XDG_CONFIG_HOME", (tmp / "config").c_str(), TRUE);
    g_setenv("XDG_CACHE_HOME", (tmp / "cache").c_str(), TRUE);
    g_setenv("NPP_GTK_DATA_DIR", NPP_TEST_DATA_DIR, TRUE);

    const fs::path server = tmp / "server";
    fs::create_directories(server / "docs");
    fake_ftp::writeAll(server / "hello.txt", "remote hello\n");
    fake_ftp::writeAll(server / "docs" / "readme.md", "# Read me\n");
    FakeFtpServer ftp(server);

    // A profile with its password: no question to answer.
    const fs::path config = tmp / "config" / "notepad-plus-plus-gtk" / "plugins" / "config";
    fs::create_directories(config);
    fake_ftp::writeAll(config / "NppRemote.ini", "[Profile 1]\nName=Test Server\nProtocol=ftp\nHost=127.0.0.1\nPort=" + std::to_string(ftp.port()) +
                                                     "\nUser=alice\nPassword=secret\nTimeout=5\n");

    gtk_init(&argc, &argv);
    core::LanguageCatalog langs;
    std::string err;
    CHECK(langs.load(fs::path(NPP_TEST_DATA_DIR) / "langs.model.xml", &err));
    GtkApplication* app = gtk_application_new("org.notepad_plus_plus.GtkPortRemotePanelTests", G_APPLICATION_NON_UNIQUE);
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
        CHECK(w->plugins() && w->plugins()->commands().size() >= 5);

        std::printf("saving a cached file uploads it...\n");
        const fs::path cache = tmp / "cache" / "notepad-plus-plus-gtk" / "NppRemote" / "Test Server";
        fs::create_directories(cache);
        fake_ftp::writeAll(cache / "hello.txt", "local hello\n");
        CHECK(w->openFiles({cache / "hello.txt"}) == 1);
        w->activeView()->execute(SCI_APPENDTEXT, 5, reinterpret_cast<sptr_t>("more\n"));
        CHECK(w->pluginMessage(NPPM_SAVECURRENTFILE, 0, 0) == TRUE);
        CHECK(waitFor([&] { return fake_ftp::readAll(server / "hello.txt") == "local hello\nmore\n"; }));
        CHECK(w->dock().isShown("plugin:NppRemote.so:0"));

        std::printf("the panel lists the server, a double-click opens a file...\n");
        auto* panel = reinterpret_cast<GtkWidget*>(w->pluginMessage(NPPM_DMMGETPLUGINHWNDBYNAME, 0, reinterpret_cast<sptr_t>(L"NppRemote.so")));
        CHECK(panel != nullptr);
        GtkTreeView* tv = panel ? findTree(panel) : nullptr;
        CHECK(tv != nullptr);
        if (tv) {
            GtkTreePath* docs = nullptr;
            CHECK(waitFor([&] { return (docs = findRow(tv, "docs")) != nullptr; }));
            if (docs) {
                gtk_tree_view_expand_row(tv, docs, FALSE);
                gtk_tree_path_free(docs);
            }
            GtkTreePath* readme = nullptr;
            CHECK(waitFor([&] { return (readme = findRow(tv, "readme.md")) != nullptr; }));
            if (readme) {
                gtk_tree_view_row_activated(tv, readme, gtk_tree_view_get_column(tv, 0));
                gtk_tree_path_free(readme);
            }
            const fs::path local = cache / "docs" / "readme.md";
            CHECK(waitFor([&] { return activePath(*w) == local.string(); }));
            CHECK(fake_ftp::readAll(local) == "# Read me\n");
            CHECK(w->activeView()->execute(SCI_GETLENGTH, 0, 0) == 10);

            std::printf("saving it sends it back...\n");
            w->activeView()->execute(SCI_APPENDTEXT, 6, reinterpret_cast<sptr_t>("Edit.\n"));
            CHECK(w->pluginMessage(NPPM_SAVECURRENTFILE, 0, 0) == TRUE);
            CHECK(waitFor([&] { return fake_ftp::readAll(server / "docs" / "readme.md") == "# Read me\nEdit.\n"; }));
        }
        // One connection did it all.
        CHECK(ftp.logins == 1);
        CHECK(w->requestQuit());
    }
    g_object_unref(app);
    std::error_code ec;
    fs::remove_all(tmp, ec);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
