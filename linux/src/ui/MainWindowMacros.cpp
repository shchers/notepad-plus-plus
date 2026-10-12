// Macro and Run menus — upstream Notepad_plus macro recording/playback
// (NppCommands.cpp, Notepad_plus::macroPlayback), RunMacroDlg and RunDlg.
// Recorded macros and Run commands live in shortcuts.xml in upstream's
// format, so they move between Windows and Linux unchanged.

#include "ui/MainWindow.h"

#include "platform/Paths.h"
#include "ui/ShortcutDialog.h"

#include "menuCmdID.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <system_error>

namespace fs = std::filesystem;

namespace npp::ui {

namespace {

// Escapes '_' so GMenu does not take it for a mnemonic.
std::string menuLabel(std::string_view s)
{
    std::string out;
    for (char c : s) {
        if (c == '_')
            out += '_';
        out += c;
    }
    return out;
}

} // namespace

void MainWindow::registerMacroCommands()
{
    cmds_.add(IDM_MACRO_STARTRECORDINGMACRO, [this] { startRecording(); });
    cmds_.add(IDM_MACRO_STOPRECORDINGMACRO, [this] { stopRecording(); });
    cmds_.add(IDM_MACRO_PLAYBACKRECORDEDMACRO, [this] { playMacro(macro_); });
    cmds_.add(IDM_MACRO_SAVECURRENTMACRO, [this] { cmdSaveMacro(); });
    cmds_.add(IDM_MACRO_RUNMULTIMACRODLG, [this] { cmdRunMultiMacro(); });
    cmds_.add(IDM_EXECUTE, [this] { cmdRun(); });
    cmds_.add(IDM_SETTING_PREFERENCE, [this] { cmdPreferences(); });
    cmds_.add(IDM_LANGSTYLE_CONFIG_DLG, [this] { cmdStyleConfigurator(); });
    cmds_.add(IDM_EDIT_COLUMNMODE, [this] { cmdColumnEditor(); });
    cmds_.add(IDM_FILE_PRINT, [this] { print(true); });
    cmds_.add(IDM_FILE_PRINTNOW, [this] { print(false); });
    cmds_.add(IDM_SETTING_SHORTCUT_MAPPER, [this] { cmdShortcutMapper(0); });
    cmds_.add(IDM_SETTING_SHORTCUT_MAPPER_MACRO, [this] { cmdShortcutMapper(1); });
    cmds_.add(IDM_SETTING_SHORTCUT_MAPPER_RUN, [this] { cmdShortcutMapper(2); });

    // A recorded menu command is replayed by its ID, so the Scintilla
    // messages it sends on the way are not recorded too.
    cmds_.onBeforeRun = [this](int id) { inRecordedCommand_ = recording_ && core::isRecordableCommand(id); };
    cmds_.onRun = [this](int id) {
        inRecordedCommand_ = false;
        if (recording_ && core::isRecordableCommand(id))
            macro_.push_back(core::MacroStep::command(id));
    };

    // Saved macros and Run commands: "win.run-macro::<index>", "win.run-command::<index>".
    auto add = [this](const char* name, GCallback cb) {
        GSimpleAction* a = g_simple_action_new(name, G_VARIANT_TYPE_STRING);
        g_signal_connect(a, "activate", cb, this);
        g_action_map_add_action(G_ACTION_MAP(window_), G_ACTION(a));
        g_object_unref(a);
    };
    add("run-macro", G_CALLBACK(+[](GSimpleAction*, GVariant* p, gpointer self) {
            auto* w = static_cast<MainWindow*>(self);
            const std::size_t i = std::strtoul(g_variant_get_string(p, nullptr), nullptr, 10);
            if (i < w->shortcuts_.macros.size() && !w->recording_)
                w->playMacro(w->shortcuts_.macros[i].steps);
        }));
    add("run-command", G_CALLBACK(+[](GSimpleAction*, GVariant* p, gpointer self) {
            auto* w = static_cast<MainWindow*>(self);
            const std::size_t i = std::strtoul(g_variant_get_string(p, nullptr), nullptr, 10);
            std::string err;
            if (i < w->shortcuts_.userCommands.size() && !w->runUserCommand(w->shortcuts_.userCommands[i].command, &err))
                w->showError("Cannot run the command", err);
        }));
}

std::vector<std::pair<std::string, core::KeyCombo>> MainWindow::extraAccels() const
{
    std::vector<std::pair<std::string, core::KeyCombo>> out;
    for (std::size_t i = 0; i < shortcuts_.macros.size(); ++i)
        if (!shortcuts_.macros[i].key.empty())
            out.emplace_back("win.run-macro::" + std::to_string(i), shortcuts_.macros[i].key);
    for (std::size_t i = 0; i < shortcuts_.userCommands.size(); ++i)
        if (!shortcuts_.userCommands[i].key.empty())
            out.emplace_back("win.run-command::" + std::to_string(i), shortcuts_.userCommands[i].key);
    return out;
}

void MainWindow::refreshMacroMenu()
{
    if (dyn_.macros) {
        g_menu_remove_all(dyn_.macros);
        // Upstream groups macros by their FolderName attribute in submenus.
        std::map<std::string, GMenu*> folders;
        for (std::size_t i = 0; i < shortcuts_.macros.size(); ++i) {
            const core::SavedMacro& m = shortcuts_.macros[i];
            GMenu* target = dyn_.macros;
            if (!m.folder.empty()) {
                auto [it, fresh] = folders.try_emplace(m.folder, nullptr);
                if (fresh)
                    it->second = g_menu_new();
                target = it->second;
            }
            appendItem(target, menuLabel(m.name), "win.run-macro", std::to_string(i).c_str());
        }
        for (auto& [name, menu] : folders) {
            g_menu_append_submenu(dyn_.macros, menuLabel(name).c_str(), G_MENU_MODEL(menu));
            g_object_unref(menu);
        }
    }
    refreshMacroState();
}

void MainWindow::refreshRunMenu()
{
    if (!dyn_.runCommands)
        return;
    g_menu_remove_all(dyn_.runCommands);
    std::map<std::string, GMenu*> folders;
    for (std::size_t i = 0; i < shortcuts_.userCommands.size(); ++i) {
        const core::UserCommand& c = shortcuts_.userCommands[i];
        GMenu* target = dyn_.runCommands;
        if (!c.folder.empty()) {
            auto [it, fresh] = folders.try_emplace(c.folder, nullptr);
            if (fresh)
                it->second = g_menu_new();
            target = it->second;
        }
        appendItem(target, menuLabel(c.name), "win.run-command", std::to_string(i).c_str());
    }
    for (auto& [name, menu] : folders) {
        g_menu_append_submenu(dyn_.runCommands, menuLabel(name).c_str(), G_MENU_MODEL(menu));
        g_object_unref(menu);
    }
}

void MainWindow::refreshMacroState()
{
    // Upstream enables the Macro items by recording state.
    cmds_.setEnabled(IDM_MACRO_STARTRECORDINGMACRO, !recording_ && !playing_);
    cmds_.setEnabled(IDM_MACRO_STOPRECORDINGMACRO, recording_);
    const bool canPlay = !recording_ && !playing_ && !macro_.empty();
    cmds_.setEnabled(IDM_MACRO_PLAYBACKRECORDEDMACRO, canPlay);
    cmds_.setEnabled(IDM_MACRO_SAVECURRENTMACRO, canPlay);
    cmds_.setEnabled(IDM_MACRO_RUNMULTIMACRODLG, !recording_ && !playing_ && (!macro_.empty() || !shortcuts_.macros.empty()));
    if (GAction* a = g_action_map_lookup_action(G_ACTION_MAP(window_), "run-macro"))
        g_simple_action_set_enabled(G_SIMPLE_ACTION(a), !recording_ && !playing_);
}

// ============================================================================
// Recording and playback
// ============================================================================

void MainWindow::startRecording()
{
    if (recording_ || playing_)
        return;
    macro_.clear();
    recording_ = true;
    for (auto& t : tabs_)
        t->view->execute(SCI_STARTRECORD);
    refreshMacroState();
}

void MainWindow::stopRecording()
{
    if (!recording_)
        return;
    for (auto& t : tabs_)
        t->view->execute(SCI_STOPRECORD);
    recording_ = false;
    macro_ = core::normaliseNewlines(macro_);
    refreshMacroState();
}

void MainWindow::recordScintilla(int message, uptr_t wParam, sptr_t lParam)
{
    if (!recording_ || inRecordedCommand_ || playing_)
        return;
    macro_.push_back(core::MacroStep::fromScintilla(message, wParam, lParam));
}

void MainWindow::playMacro(const core::Macro& m, int times)
{
    if (m.empty() || recording_ || playing_)
        return;
    playing_ = true;
    refreshMacroState();
    auto playOnce = [&] {
        for (const core::MacroStep& s : m) {
            EditorView* v = activeView(); // a step may switch documents
            if (!v)
                break;
            switch (s.type) {
            case core::MacroStep::UseLParam:
            case core::MacroStep::UseSParam:
                if (s.isPlayableScintilla())
                    v->execute(static_cast<unsigned>(s.message), static_cast<uptr_t>(s.wParam),
                               s.type == core::MacroStep::UseSParam ? reinterpret_cast<sptr_t>(s.sParam.c_str())
                                                                     : static_cast<sptr_t>(s.lParam));
                break;
            case core::MacroStep::MenuCommand:
                cmds_.run(static_cast<int>(s.wParam));
                break;
            case core::MacroStep::SavedSearch:
                findDlg_->execSaved(s.message, s.lParam, s.sParam);
                break;
            default:
                break;
            }
        }
    };
    EditorView* v = activeView();
    if (v)
        v->execute(SCI_BEGINUNDOACTION);
    if (times >= 0) {
        for (int i = 0; i < times; ++i)
            playOnce();
    } else if (v) {
        // Upstream "Run until the end of file": stop at the last line, or when
        // a run makes no progress (the caret stays and the text is unchanged).
        for (int guard = 0; guard < 1000000; ++guard) {
            const sptr_t pos = v->currentPos(), len = v->length();
            const sptr_t line = v->lineFromPos(pos);
            if (pos >= len && guard > 0)
                break;
            playOnce();
            if (activeView() != v)
                break;
            const sptr_t newPos = v->currentPos();
            if ((newPos == pos && v->length() == len) || v->lineFromPos(newPos) < line)
                break;
            if (v->lineFromPos(newPos) >= v->lineCount() - 1 && newPos >= v->length())
                break;
        }
    }
    // The view may be gone if the macro closed its document.
    if (v && std::any_of(tabs_.begin(), tabs_.end(), [v](const auto& t) { return t->view.get() == v; }))
        v->execute(SCI_ENDUNDOACTION);
    playing_ = false;
    refreshMacroState();
    refreshAll();
}

// ============================================================================
// Saved macros and Run commands
// ============================================================================

void MainWindow::addSavedMacro(const core::SavedMacro& m)
{
    shortcuts_.macros.push_back(m);
    std::string err;
    if (!saveShortcuts(&err))
        showError("Cannot save shortcuts.xml", err);
    refreshMacroMenu();
    applyShortcuts();
}

void MainWindow::addUserCommand(const core::UserCommand& c)
{
    shortcuts_.userCommands.push_back(c);
    std::string err;
    if (!saveShortcuts(&err))
        showError("Cannot save shortcuts.xml", err);
    refreshRunMenu();
    applyShortcuts();
}

void MainWindow::cmdSaveMacro()
{
    if (macro_.empty())
        return;
    ShortcutEdit e;
    e.conflictInfo = [this](const core::KeyCombo& k) {
        const auto c = keys_.conflicts(k);
        return c.empty() ? std::string() : "CONFLICT FOUND! " + k.toString() + " is used by " + core::Translator::plain(tr_.command(c.front().id, "a menu command"));
    };
    if (!runShortcutDialog(GTK_WINDOW(window_), tr_, e) || e.name.empty())
        return;
    addSavedMacro({e.name, e.key, {}, macro_});
}

void MainWindow::cmdRunMultiMacro()
{
    const char* sec = "MultiMacro";
    GtkWidget* dlg = gtk_dialog_new_with_buttons(tr_.dialogAttr(sec, "title", "Run a Macro Multiple Times").c_str(), GTK_WINDOW(window_),
                                                 GTK_DIALOG_MODAL, core::Translator::toMnemonic(tr_.dialog(sec, 2, "&Cancel")).c_str(),
                                                 GTK_RESPONSE_CANCEL, core::Translator::toMnemonic(tr_.dialog(sec, 1, "&Run")).c_str(),
                                                 GTK_RESPONSE_OK, nullptr);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
    GtkWidget* box = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    gtk_box_set_spacing(GTK_BOX(box), 8);
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(row), gtk_label_new(core::Translator::plain(tr_.dialog(sec, 8006, "Macro to run")).c_str()), FALSE, FALSE, 0);
    GtkWidget* combo = gtk_combo_box_text_new();
    if (!macro_.empty())
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "Current recorded macro");
    for (const core::SavedMacro& m : shortcuts_.macros)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), m.name.c_str());
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
    gtk_box_pack_start(GTK_BOX(row), combo, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);

    GtkWidget* timesRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* multi = gtk_radio_button_new_with_label(nullptr, core::Translator::plain(tr_.dialog(sec, 8001, "Run")).c_str());
    GtkWidget* spin = gtk_spin_button_new_with_range(1, 1000000, 1);
    gtk_box_pack_start(GTK_BOX(timesRow), multi, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(timesRow), spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(timesRow), gtk_label_new(core::Translator::plain(tr_.dialog(sec, 8005, "times")).c_str()), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), timesRow, FALSE, FALSE, 0);
    GtkWidget* eof = gtk_radio_button_new_with_mnemonic_from_widget(
        GTK_RADIO_BUTTON(multi), core::Translator::toMnemonic(tr_.dialog(sec, 8002, "Run until the &end of file")).c_str());
    gtk_box_pack_start(GTK_BOX(box), eof, FALSE, FALSE, 0);
    gtk_widget_show_all(box);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
        gtk_spin_button_update(GTK_SPIN_BUTTON(spin));
        const int times = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(eof)) ? -1 : gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin));
        gtk_widget_destroy(dlg);
        if (!macro_.empty()) {
            if (idx == 0) {
                playMacro(macro_, times);
                return;
            }
            --idx;
        }
        if (idx >= 0 && idx < static_cast<int>(shortcuts_.macros.size())) {
            const core::Macro m = shortcuts_.macros[static_cast<std::size_t>(idx)].steps;
            playMacro(m, times);
        }
        return;
    }
    gtk_widget_destroy(dlg);
}

// ============================================================================
// Run
// ============================================================================

std::string MainWindow::expandRunVariables(const std::string& cmd) const
{
    const core::Buffer* b = activeBuffer();
    EditorView* v = activeView();
    const fs::path path = b && !b->isUntitled() ? b->path : fs::path();
    auto value = [&](const std::string& name) -> std::string {
        if (name == "FULL_CURRENT_PATH")
            return b ? b->fullName() : std::string();
        if (name == "CURRENT_DIRECTORY")
            return path.empty() ? std::string() : path.parent_path().string();
        if (name == "FILE_NAME")
            return b ? b->name() : std::string();
        if (name == "NAME_PART")
            return b ? fs::path(b->name()).stem().string() : std::string();
        if (name == "EXT_PART")
            return b ? fs::path(b->name()).extension().string() : std::string();
        if (name == "NPP_DIRECTORY")
            return platform::executableDir().string();
        if (name == "NPP_FULL_FILE_PATH")
            return (platform::executableDir() / NPP_EXE_NAME).string();
        if (name == "NPP_SETTINGS_DIR")
            return platform::userConfigDir().string();
        if (!v)
            return {};
        if (name == "CURRENT_WORD") {
            std::string s = v->selectedText();
            if (s.empty()) {
                const sptr_t p = v->currentPos();
                s = v->sci().range(v->execute(SCI_WORDSTARTPOSITION, static_cast<uptr_t>(p), true),
                                   v->execute(SCI_WORDENDPOSITION, static_cast<uptr_t>(p), true));
            }
            return s;
        }
        if (name == "CURRENT_LINE")
            return std::to_string(v->lineFromPos(v->currentPos()) + 1);
        if (name == "CURRENT_COLUMN")
            return std::to_string(v->execute(SCI_GETCOLUMN, static_cast<uptr_t>(v->currentPos())) + 1);
        if (name == "CURRENT_LINESTR")
            return v->sci().lineText(v->lineFromPos(v->currentPos()));
        if (name.rfind("SYS.", 0) == 0) {
            const char* e = g_getenv(name.c_str() + 4);
            return e ? e : "";
        }
        return "$(" + name + ")"; // unknown: left as written, as upstream
    };
    std::string out;
    for (std::size_t i = 0; i < cmd.size();) {
        if (cmd.compare(i, 2, "$(") == 0) {
            const std::size_t end = cmd.find(')', i + 2);
            if (end != std::string::npos) {
                out += value(cmd.substr(i + 2, end - i - 2));
                i = end + 1;
                continue;
            }
        }
        out += cmd[i++];
    }
    return out;
}

bool MainWindow::runUserCommand(const std::string& cmd, std::string* error)
{
    const std::string expanded = expandRunVariables(cmd);
    gchar** argv = nullptr;
    GError* err = nullptr;
    if (!g_shell_parse_argv(expanded.c_str(), nullptr, &argv, &err)) {
        if (error)
            *error = err ? err->message : expanded;
        if (err)
            g_error_free(err);
        return false;
    }
    // Upstream ShellExecute also opens documents and URLs: do the same when
    // the "program" is not something that runs.
    const std::string prog = argv[0];
    gchar* found = g_find_program_in_path(prog.c_str());
    bool ok = true;
    if (!found && (prog.find("://") != std::string::npos || fs::exists(prog))) {
        g_strfreev(argv);
        gchar* uri = prog.find("://") != std::string::npos ? g_strdup(prog.c_str()) : g_filename_to_uri(fs::absolute(prog).c_str(), nullptr, nullptr);
        ok = uri && gtk_show_uri_on_window(GTK_WINDOW(window_), uri, GDK_CURRENT_TIME, &err);
        g_free(uri);
    } else {
        const core::Buffer* b = activeBuffer();
        const std::string cwd = b && !b->isUntitled() ? b->path.parent_path().string() : g_get_home_dir();
        ok = g_spawn_async(cwd.c_str(), argv, nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr, &err);
        g_strfreev(argv);
    }
    g_free(found);
    if (!ok && error)
        *error = err ? err->message : expanded;
    if (err)
        g_error_free(err);
    return ok;
}

void MainWindow::cmdRun()
{
    const char* sec = "Run";
    GtkWidget* dlg = gtk_dialog_new_with_buttons(tr_.dialogAttr(sec, "title", "Run...").c_str(), GTK_WINDOW(window_), GTK_DIALOG_MODAL,
                                                 core::Translator::toMnemonic(tr_.dialog(sec, 1904, "Save...")).c_str(), 1,
                                                 core::Translator::toMnemonic(tr_.dialog(sec, 2, "Cancel")).c_str(), GTK_RESPONSE_CANCEL,
                                                 core::Translator::toMnemonic(tr_.dialog(sec, 1, "Run")).c_str(), GTK_RESPONSE_OK, nullptr);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
    GtkWidget* box = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    gtk_box_set_spacing(GTK_BOX(box), 6);
    GtkWidget* l = gtk_label_new(core::Translator::plain(tr_.dialog(sec, 1903, "The Program to Run")).c_str());
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget* combo = gtk_combo_box_text_new_with_entry();
    for (const std::string& h : settings_.runHistory)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), h.c_str());
    GtkWidget* entry = gtk_bin_get_child(GTK_BIN(combo));
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_entry_set_width_chars(GTK_ENTRY(entry), 50);
    if (!settings_.runHistory.empty())
        gtk_entry_set_text(GTK_ENTRY(entry), settings_.runHistory.front().c_str());
    gtk_box_pack_start(GTK_BOX(row), combo, TRUE, TRUE, 0);
    GtkWidget* browse = gtk_button_new_with_label("...");
    g_signal_connect(browse, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer e) {
                         GtkFileChooserNative* fc = gtk_file_chooser_native_new("Program", GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(b))),
                                                                                GTK_FILE_CHOOSER_ACTION_OPEN, "_Select", "_Cancel");
                         if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(fc)) == GTK_RESPONSE_ACCEPT)
                             if (gchar* fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(fc))) {
                                 gchar* q = g_shell_quote(fn);
                                 gtk_entry_set_text(GTK_ENTRY(e), q);
                                 g_free(q);
                                 g_free(fn);
                             }
                         g_object_unref(fc);
                     }), entry);
    gtk_box_pack_start(GTK_BOX(row), browse, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    gtk_widget_show_all(box);

    for (;;) {
        const gint r = gtk_dialog_run(GTK_DIALOG(dlg));
        const std::string cmd = gtk_entry_get_text(GTK_ENTRY(entry));
        if (r == 1) { // Save...: a name and a shortcut, into the Run menu
            if (cmd.empty())
                continue;
            ShortcutEdit e;
            e.conflictInfo = [this](const core::KeyCombo& k) {
                const auto c = keys_.conflicts(k);
                return c.empty() ? std::string() : "CONFLICT FOUND! " + k.toString();
            };
            if (runShortcutDialog(GTK_WINDOW(dlg), tr_, e) && !e.name.empty())
                addUserCommand({e.name, e.key, {}, cmd});
            continue;
        }
        if (r == GTK_RESPONSE_OK && !cmd.empty()) {
            settings_.runHistory.erase(std::remove(settings_.runHistory.begin(), settings_.runHistory.end(), cmd), settings_.runHistory.end());
            settings_.runHistory.insert(settings_.runHistory.begin(), cmd);
            if (settings_.runHistory.size() > 10)
                settings_.runHistory.resize(10);
            std::string err;
            if (!runUserCommand(cmd, &err)) {
                showError("Cannot run the command", err);
                continue;
            }
        }
        break;
    }
    gtk_widget_destroy(dlg);
}

} // namespace npp::ui
