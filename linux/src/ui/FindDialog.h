// Find / Replace / Find in Files / Mark — upstream FindReplaceDlg. One
// non-modal window with the four tabs over a shared set of fields, as
// upstream; the searching itself is core::SearchEngine and core::FileSearch.
//
// Every operation can also run without the window (F3, macros, tests): the
// do*() functions take the options explicitly. Operations started from the
// window are recorded into a macro in upstream's format (type 3 steps:
// IDC_FRCOMMAND_INIT ... IDC_FRCOMMAND_EXEC) and execSaved() replays them.
#pragma once

#include "core/FileSearch.h"
#include "core/Macro.h"
#include "core/SearchEngine.h"
#include "core/Translator.h"
#include "platform/Settings.h"
#include "ui/EditorView.h"
#include "ui/SearchResults.h"

#include <gtk/gtk.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace npp::ui {

class FindProgress; // the progress window of Find in Files

// What the dialog needs from the main window.
struct FindHost {
    std::function<EditorView*()> activeView;
    // Open documents, one view per buffer: (Buffer::fullName(), view).
    std::function<std::vector<std::pair<std::string, EditorView*>>()> documents;
    // The open document showing a file, or nullptr.
    std::function<EditorView*(const std::filesystem::path&)> viewForFile;
    std::function<std::filesystem::path()> currentDirectory; // of the active document
    std::function<SearchResults*()> showResults;              // shows the panel
    std::function<void(const core::MacroStep&)> record;       // set while a macro is recorded
    // Yes/No question; false = No.
    std::function<bool(const std::string& primary, const std::string& secondary)> confirm;
    std::function<void()> afterEdit; // documents changed (refresh tabs/status)
};

class FindDialog {
public:
    enum class Tab { Find = 0, Replace, FindInFiles, Mark };

    FindDialog(GtkWindow* parent, const core::Translator& tr, platform::FindSettings& settings, FindHost host);
    ~FindDialog();
    FindDialog(const FindDialog&) = delete;
    FindDialog& operator=(const FindDialog&) = delete;

    // Shows the tab, with the selection (or the word at the caret) as the text to find.
    void show(Tab tab);
    void hide();
    bool isVisible() const;
    GtkWidget* window() const { return window_; }
    void retranslate();

    core::FindOptions options() const;  // from the fields
    void setSearchText(const std::string& s);
    void setDirectory(const std::string& dir);
    const std::string& status() const { return status_; }

    // Search > Find Next / Previous (F3 / Shift+F3) with the dialog's options.
    void findNext(bool forward);
    // Ctrl+F3 and friends: the selection (or caret word) becomes the text.
    void selectAndFind(bool forward, bool volatileSearch);

    // ---- Operations (upstream IDs in comments) -----------------------------
    core::FindStatus doFindNext(const core::FindOptions& o);  // IDOK
    core::FindStatus doReplace(const core::FindOptions& o);   // IDREPLACE
    int doReplaceAll(const core::FindOptions& o);             // IDREPLACEALL
    int doReplaceAllInOpened(const core::FindOptions& o);     // IDC_REPLACE_OPENEDFILES
    int doCount(const core::FindOptions& o);                  // IDCCOUNTALL
    int doMarkAll(const core::FindOptions& o);                // IDCMARKALL
    void doClearMarks(const core::FindOptions& o);            // IDC_CLEAR_ALL
    int doFindAllInCurrent(const core::FindOptions& o);       // IDC_FINDALL_CURRENTFILE
    int doFindAllInOpened(const core::FindOptions& o);        // IDC_FINDALL_OPENEDFILES
    int doFindInFiles(const core::FindOptions& o);            // IDD_FINDINFILES_FIND_BUTTON
    int doReplaceInFiles(const core::FindOptions& o, bool confirm = true); // IDD_FINDINFILES_REPLACEINFILES
    // Find in Projects: the given files instead of a folder (IDD_FINDINFILES_FINDINPROJECTS).
    int doFindInFileList(const core::FindOptions& o, const std::vector<std::filesystem::path>& files);

    // Presses a button by its upstream ID (IDOK = 1 is Find Next): takes the
    // fields, remembers them, records the operation into a macro and runs it.
    void click(int buttonId);

    // Macro playback of a type 3 step.
    void execSaved(int what, std::int64_t intValue, const std::string& s);

private:
    struct Row; // a labelled field row
    int searchFiles(const core::FindOptions& o, const std::vector<std::filesystem::path>& files, FindProgress& progress);
    void build();
    void updateTab();
    void setStatus(const std::string& msg, bool error = false);
    void remember(); // pushes the fields into the history
    void loadFields();
    void record(int cmd, const core::FindOptions& o, int kind);
    std::string misc(const char* key, const char* fallback) const { return tr_.misc(key, fallback); }
    std::string scopeInfo(const core::FindOptions& o) const;
    std::string label(int id, const char* fallback) const;
    void copyMarkedText();
    EditorView* view() const { return host_.activeView ? host_.activeView() : nullptr; }

    GtkWindow* parent_;
    const core::Translator& tr_;
    platform::FindSettings& st_;
    FindHost host_;
    std::string status_;
    std::unique_ptr<core::FindOptions> savedEnv_; // macro playback (upstream _env)

    GtkWidget* window_ = nullptr;
    GtkWidget* tabs_ = nullptr;
    GtkWidget* what_ = nullptr;      // GtkComboBoxText with entry
    GtkWidget* with_ = nullptr;
    GtkWidget* filters_ = nullptr;
    GtkWidget* dir_ = nullptr;
    GtkWidget* withRow_[2] = {};
    GtkWidget* filtersRow_[2] = {};
    GtkWidget* dirRow_[4] = {};
    GtkWidget* wholeWord_ = nullptr;
    GtkWidget* matchCase_ = nullptr;
    GtkWidget* wrap_ = nullptr;
    GtkWidget* backward_ = nullptr;
    GtkWidget* inSelection_ = nullptr;
    GtkWidget* bookmark_ = nullptr;
    GtkWidget* purge_ = nullptr;
    GtkWidget* recursive_ = nullptr;
    GtkWidget* hidden_ = nullptr;
    GtkWidget* normal_ = nullptr;
    GtkWidget* extended_ = nullptr;
    GtkWidget* regex_ = nullptr;
    GtkWidget* dotNl_ = nullptr;
    GtkWidget* modeFrame_ = nullptr;
    GtkWidget* statusLabel_ = nullptr;
    struct Button {
        int id;
        GtkWidget* widget;
        unsigned tabs; // bit per Tab
    };
    std::vector<Button> buttons_;
};

} // namespace npp::ui
