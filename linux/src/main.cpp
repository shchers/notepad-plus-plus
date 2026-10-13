// Entry point — GTK counterpart of upstream winmain.cpp.
//
// Single instance, as upstream: the first process owns the D-Bus name
// NPP_APP_ID; later invocations forward their command line to it (GApplication
// "command-line") and exit. -multiInst starts a separate process; like
// upstream, a second instance then runs without a session.
//
// Accepts the upstream command line switches that make sense here:
//   -n<line> -c<column> -l<language> -ro -multiInst -nosession -noPlugin
//   -pluginMessage=<text>  and file names
// plus --exit-after=<ms> (CI smoke tests) and --version / --help.

#include "core/LanguageCatalog.h"
#include "platform/Paths.h"
#include "ui/MainWindow.h"

#include <gtk/gtk.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct CommandLine {
    npp::ui::OpenOptions openOpts;
    std::string langArg;
    std::vector<fs::path> files;
    int exitAfterMs = -1;
    bool multiInst = false;
    bool noSession = false;
    bool noPlugin = false;
    std::string pluginMessage; // -pluginMessage="..."
    std::string text;          // the whole command line (NPPM_GETCURRENTCMDLINE)
};

struct App {
    npp::core::LanguageCatalog langs;
    std::unique_ptr<npp::ui::MainWindow> window;
    GApplication* gapp = nullptr;
    bool useSession = true;
};

void printUsage(const char* argv0)
{
    std::printf(
        "Usage: %s [options] [file ...]\n"
        "\n"
        "Notepad++ compatible switches:\n"
        "  -n<line>          go to line\n"
        "  -c<column>        go to column\n"
        "  -l<language>      apply language (e.g. -lpython, -l\"C++\")\n"
        "  -ro               open files read-only\n"
        "  -multiInst        start a separate instance (without session)\n"
        "  -nosession        do not restore or save the session\n"
        "  -noPlugin         do not load plugins\n"
        "  -pluginMessage=\"text\"  pass text to the plugins (NPPN_CMDLINEPLUGINMSG)\n"
        "\n"
        "Other options:\n"
        "  --exit-after=MS   quit MS milliseconds after start (for smoke tests)\n"
        "  --version         print version and exit\n"
        "  --help            show this help\n",
        argv0);
}

enum class Parse { Ok, Exit, Error };

// `cwd` resolves relative file names (a forwarded command line comes from
// another process's directory).
Parse parseArgs(int argc, char** argv, const fs::path& cwd, CommandLine& cl, std::string* message)
{
    bool endOfOptions = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        cl.text += (cl.text.empty() ? "" : " ") + (a.find(' ') != std::string::npos ? "\"" + a + "\"" : a);
        if (endOfOptions || a.empty() || a[0] != '-') {
            const fs::path p(a);
            cl.files.push_back(p.is_absolute() || cwd.empty() ? p : cwd / p);
            continue;
        }
        if (a == "--") {
            endOfOptions = true;
        } else if (a == "--help" || a == "-h" || a == "-?") {
            printUsage(argv[0]);
            return Parse::Exit;
        } else if (a == "--version") {
            std::printf("Notepad++ (GTK port) %s\n", NPP_VERSION);
            return Parse::Exit;
        } else if (a.rfind("--exit-after=", 0) == 0) {
            cl.exitAfterMs = std::atoi(a.c_str() + 13);
        } else if (a == "-ro") {
            cl.openOpts.readOnly = true;
        } else if (a == "-multiInst") {
            cl.multiInst = true;
        } else if (a == "-nosession") {
            cl.noSession = true;
        } else if (a == "-noPlugin") {
            cl.noPlugin = true;
        } else if (a.rfind("-pluginMessage=", 0) == 0) {
            cl.pluginMessage = a.substr(15);
        } else if (a == "-notabbar" || a == "-nosys") {
            // Upstream switches accepted for compatibility; no-ops here.
        } else if (a.size() > 2 && a[1] == 'n') {
            cl.openOpts.line = std::atol(a.c_str() + 2);
        } else if (a.size() > 2 && a[1] == 'c') {
            cl.openOpts.column = std::atol(a.c_str() + 2);
        } else if (a.size() > 2 && a[1] == 'l') {
            cl.langArg = a.substr(2);
        } else {
            *message = std::string(argv[0]) + ": unknown option '" + a + "' (try --help)";
            return Parse::Error;
        }
    }
    return Parse::Ok;
}

// Runs in the primary instance for its own command line and for every
// forwarded one.
int onCommandLine(GApplication* gapp, GApplicationCommandLine* cmd, App& app)
{
    gint argc = 0;
    gchar** argv = g_application_command_line_get_arguments(cmd, &argc);
    const gchar* cwd = g_application_command_line_get_cwd(cmd);
    CommandLine cl;
    std::string msg;
    const Parse res = parseArgs(argc, argv, cwd ? fs::path(cwd) : fs::path(), cl, &msg);
    g_strfreev(argv);
    if (res != Parse::Ok) // already validated by the sending process
        return res == Parse::Exit ? 0 : 2;

    if (!cl.langArg.empty()) {
        cl.openOpts.language = app.langs.byAnyName(cl.langArg);
        if (!cl.openOpts.language)
            g_application_command_line_printerr(cmd, "notepad-plus-plus-gtk: unknown language '%s', ignored\n", cl.langArg.c_str());
    }

    const bool first = !app.window;
    if (first) {
        app.window = std::make_unique<npp::ui::MainWindow>(GTK_APPLICATION(gapp), app.langs, app.useSession);
        app.window->setCommandLine(cl.text);
        // Upstream order: plugins before the session, NPPN_READY once all is open.
        if (!cl.noPlugin)
            app.window->loadPlugins();
        app.window->sendPluginCommandLine(cl.pluginMessage);
        app.window->restoreLastSession();
        if (app.window->documentCount() == 0)
            app.window->newDocument();
        // Close like the user would, so the exit protocol (session, snapshots) runs.
        if (cl.exitAfterMs >= 0 && !g_application_command_line_get_is_remote(cmd))
            g_timeout_add(static_cast<guint>(cl.exitAfterMs), +[](gpointer p) -> gboolean {
                auto* a = static_cast<App*>(p);
                if (a->window && a->window->requestQuit())
                    gtk_widget_hide(GTK_WIDGET(a->window->window()));
                g_application_quit(a->gapp);
                return G_SOURCE_REMOVE;
            }, &app);
    }
    if (!cl.files.empty())
        app.window->openFiles(cl.files, cl.openOpts);
    app.window->present();
    if (first)
        app.window->pluginsReady();
    else
        app.window->sendPluginCommandLine(cl.pluginMessage);
    return 0;
}

// Whether a primary instance owns NPP_APP_ID on the session bus.
bool instanceRunning()
{
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (!bus)
        return false;
    GVariant* r = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                              "NameHasOwner", g_variant_new("(s)", NPP_APP_ID), G_VARIANT_TYPE("(b)"),
                                              G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr);
    gboolean owned = FALSE;
    if (r) {
        g_variant_get(r, "(b)", &owned);
        g_variant_unref(r);
    }
    g_object_unref(bus);
    return owned;
}

} // namespace

int main(int argc, char** argv)
{
    // Validate locally first, so errors and --help print in this terminal
    // rather than in the running instance.
    CommandLine local;
    std::string msg;
    std::error_code ec;
    switch (parseArgs(argc, argv, fs::current_path(ec), local, &msg)) {
    case Parse::Exit: return 0;
    case Parse::Error: std::fprintf(stderr, "%s\n", msg.c_str()); return 2;
    case Parse::Ok: break;
    }

    App app;
    std::string err;
    const fs::path langsXml = npp::platform::findConfigFile("langs.xml", "langs.model.xml");
    if (langsXml.empty() || !app.langs.load(langsXml, &err)) {
        std::fprintf(stderr, "notepad-plus-plus-gtk: cannot load language definitions from %s %s\n"
                             "Set NPP_GTK_DATA_DIR to the directory containing langs.model.xml.\n",
                     npp::platform::dataDir().c_str(), err.c_str());
        return 1;
    }
    app.useSession = !local.noSession;

    g_set_prgname(NPP_EXE_NAME);
    g_set_application_name("Notepad++");

    GtkApplication* gapp = nullptr;
    if (local.multiInst) {
        // Upstream: a second instance under -multiInst runs without a session.
        if (instanceRunning())
            app.useSession = false;
        gapp = gtk_application_new(NPP_APP_ID, GApplicationFlags(G_APPLICATION_HANDLES_COMMAND_LINE | G_APPLICATION_NON_UNIQUE));
    } else {
        gapp = gtk_application_new(NPP_APP_ID, G_APPLICATION_HANDLES_COMMAND_LINE);
        if (!g_application_register(G_APPLICATION(gapp), nullptr, nullptr)) {
            // No session bus (headless CI, some containers): run on our own.
            g_object_unref(gapp);
            gapp = gtk_application_new(NPP_APP_ID, GApplicationFlags(G_APPLICATION_HANDLES_COMMAND_LINE | G_APPLICATION_NON_UNIQUE));
        }
    }

    app.gapp = G_APPLICATION(gapp);
    g_signal_connect(gapp, "command-line", G_CALLBACK(+[](GApplication* ga, GApplicationCommandLine* cmd, gpointer p) -> int {
                         return onCommandLine(ga, cmd, *static_cast<App*>(p));
                     }),
                     &app);

    // GApplication passes arguments through untouched (no option entries are
    // registered); a remote instance forwards them and returns the exit code.
    const int status = g_application_run(G_APPLICATION(gapp), argc, argv);

    app.window.reset();
    g_object_unref(gapp);
    scintilla_release_resources();
    return status;
}
