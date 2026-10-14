// Terminal — a docked shell, the port's addition (upstream has none; plugins
// such as NppExec fill the gap on Windows). The terminal is VTE's widget,
// the one GNOME Terminal uses, loaded when the panel first opens: VTE is not
// a build dependency, and without it the panel says what to install.
#pragma once

#include "core/StyleCatalog.h"

#include <gtk/gtk.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace npp::ui {

class TerminalPanel {
public:
    // `exited`: the shell ended (typed `exit`, killed); the next start() runs a new one.
    TerminalPanel(std::function<void()> exited, std::function<std::filesystem::path()> currentFolder);
    ~TerminalPanel();
    TerminalPanel(const TerminalPanel&) = delete;
    TerminalPanel& operator=(const TerminalPanel&) = delete;

    GtkWidget* widget() const { return box_; }

    // Whether VTE could be loaded (tried once). Tests may name another
    // library, tried again by panels built after that.
    static bool available();
    static void setLibraryForTests(const char* soname);

    // Starts the user's shell in `dir` unless one is running. The first
    // call loads VTE and builds the terminal (or the message without it).
    void start(const std::filesystem::path& dir);
    bool hasTerminal() const { return term_ != nullptr; }
    bool running() const { return pid_ > 0; }
    // Types `text` into the shell, as from the keyboard.
    void type(const std::string& text);
    // The shell changes to `dir` (context menu "Go to Current Document's Folder").
    void changeDirectory(const std::filesystem::path& dir);
    void focus();
    bool hasFocus() const;
    void copy();
    void paste();

    // The editor's default style: colours and font.
    void applyStyles(const core::StyleCatalog& styles);

private:
    void build();
    void applyStyle();
    void showMenu(GdkEvent* ev);

    std::function<void()> exited_;
    std::function<std::filesystem::path()> currentFolder_;
    GtkWidget* box_ = nullptr;
    GtkWidget* term_ = nullptr; // VteTerminal, or nullptr without VTE
    bool built_ = false;
    bool focusOnMap_ = false;   // focus() before the terminal was on screen
    int pid_ = 0;               // the shell; 0 when none, -1 while starting
    std::uint32_t fg_ = 0x000000, bg_ = 0xFFFFFF; // Scintilla BGR
    std::string font_ = "Monospace 10";
    // Spawn callbacks and child exits arrive asynchronously, maybe after the panel is gone.
    std::shared_ptr<TerminalPanel*> alive_;
};

} // namespace npp::ui
