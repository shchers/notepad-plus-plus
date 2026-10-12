// Entry point — GTK counterpart of upstream winmain.cpp.
//
// Accepts the upstream command line switches that make sense here:
//   -n<line> -c<column> -l<language> -ro  and file names
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

struct App {
    npp::core::LanguageCatalog langs;
    std::unique_ptr<npp::ui::MainWindow> window;
    npp::ui::OpenOptions openOpts;
    std::string langArg;
    std::vector<fs::path> files;
    int exitAfterMs = -1;
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
        "\n"
        "Other options:\n"
        "  --exit-after=MS   quit MS milliseconds after start (for smoke tests)\n"
        "  --version         print version and exit\n"
        "  --help            show this help\n",
        argv0);
}

// Returns false if the process should exit with *exitCode.
bool parseArgs(App& app, int argc, char** argv, int* exitCode)
{
    bool endOfOptions = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (endOfOptions || a.empty() || a[0] != '-') {
            app.files.emplace_back(a);
            continue;
        }
        if (a == "--") {
            endOfOptions = true;
        } else if (a == "--help" || a == "-h" || a == "-?") {
            printUsage(argv[0]);
            *exitCode = 0;
            return false;
        } else if (a == "--version") {
            std::printf("Notepad++ (GTK port) %s\n", NPP_VERSION);
            *exitCode = 0;
            return false;
        } else if (a.rfind("--exit-after=", 0) == 0) {
            app.exitAfterMs = std::atoi(a.c_str() + 13);
        } else if (a == "-ro") {
            app.openOpts.readOnly = true;
        } else if (a.size() > 2 && a[1] == 'n') {
            app.openOpts.line = std::atol(a.c_str() + 2);
        } else if (a.size() > 2 && a[1] == 'c') {
            app.openOpts.column = std::atol(a.c_str() + 2);
        } else if (a.size() > 2 && a[1] == 'l') {
            app.langArg = a.substr(2);
        } else if (a == "-multiInst" || a == "-nosession" || a == "-notabbar" || a == "-nosys" || a == "-noPlugin") {
            // Upstream switches accepted for compatibility; no-ops in the skeleton.
        } else {
            std::fprintf(stderr, "%s: unknown option '%s' (try --help)\n", argv[0], a.c_str());
            *exitCode = 2;
            return false;
        }
    }
    return true;
}

void ensureWindow(GApplication* gapp, App& app)
{
    if (app.window)
        return;
    app.window = std::make_unique<npp::ui::MainWindow>(GTK_APPLICATION(gapp), app.langs);
    if (app.exitAfterMs >= 0)
        g_timeout_add(static_cast<guint>(app.exitAfterMs), +[](gpointer p) -> gboolean {
            g_application_quit(G_APPLICATION(p));
            return G_SOURCE_REMOVE;
        }, gapp);
}

} // namespace

int main(int argc, char** argv)
{
    App app;
    int exitCode = 0;
    if (!parseArgs(app, argc, argv, &exitCode))
        return exitCode;

    std::string err;
    const fs::path langsXml = npp::platform::findConfigFile("langs.xml", "langs.model.xml");
    if (langsXml.empty() || !app.langs.load(langsXml, &err)) {
        std::fprintf(stderr, "notepad-plus-plus-gtk: cannot load language definitions from %s %s\n"
                             "Set NPP_GTK_DATA_DIR to the directory containing langs.model.xml.\n",
                     npp::platform::dataDir().c_str(), err.c_str());
        return 1;
    }
    if (!app.langArg.empty()) {
        app.openOpts.language = app.langs.byAnyName(app.langArg);
        if (!app.openOpts.language)
            std::fprintf(stderr, "notepad-plus-plus-gtk: unknown language '%s', ignored\n", app.langArg.c_str());
    }

    g_set_prgname(NPP_EXE_NAME);
    g_set_application_name("Notepad++");
    // NON_UNIQUE: one process per invocation for now. Upstream's
    // single-instance behaviour (open in running window) is a planned item.
    GtkApplication* gapp = gtk_application_new(NPP_APP_ID, G_APPLICATION_NON_UNIQUE);

    g_signal_connect(gapp, "activate", G_CALLBACK(+[](GApplication* ga, gpointer p) {
                         auto& a = *static_cast<App*>(p);
                         ensureWindow(ga, a);
                         a.window->newDocument();
                         if (!a.files.empty())
                             a.window->openFiles(a.files, a.openOpts);
                         a.window->present();
                     }),
                     &app);

    // Files were parsed above, so GApplication only sees the program name.
    char* gargv[] = {argv[0], nullptr};
    const int status = g_application_run(G_APPLICATION(gapp), 1, gargv);

    app.window.reset();
    g_object_unref(gapp);
    scintilla_release_resources();
    return status;
}
