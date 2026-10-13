// Editing aids that upstream implements on ScintillaEditView, here on
// core::Sci so they work on any Scintilla and run in tests:
//   smart highlighting       SmartHighlighter.cpp
//   matching tags            xmlMatchedTagsHighlighter.cpp (simplified)
//   clickable links          Notepad_plus::addHotSpot / isUrl
//   delimiter selection      NppNotification.cpp, Ctrl + double click
//   auto-indent              Notepad_plus::maintainIndentation
//   auto-insert              AutoCompletion::insertMatchedChars / getCloseTag
//   auto-completion          AutoCompletion.cpp, FunctionCallTip.cpp with
//                            upstream's autoCompletion/<language>.xml files
#pragma once

#include "core/Sci.h"

#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace npp::core {

// Indicator numbers, as upstream (ScintillaEditView.h, stylers.xml styleIDs).
inline constexpr int kUrlIndicator = 8;
inline constexpr int kTagAttrIndicator = 26;
inline constexpr int kTagMatchIndicator = 27;
inline constexpr int kSmartIndicator = 29;

void clearIndicator(const Sci& sci, int indicator);

// ---- Smart highlighting ----------------------------------------------------
// The selected word (or text, when not `wholeWord`) in the visible lines of
// `view`, and of `other` (another view) when given.
void smartHighlight(const Sci& view, const Sci* other, bool wholeWord, bool matchCase);

// ---- Matching tags ---------------------------------------------------------
// HTML/XML: the tag around the caret and its partner, the attributes of the
// tag (`attributes`), and the delimiters of a comment, <?php ?> or <% %> zone
// around the caret (`zones`). `ignoreCase` for HTML.
void highlightMatchingTags(const Sci& sci, bool attributes, bool zones, bool ignoreCase);

// ---- Clickable links ---------------------------------------------------------
// URL ranges (byte offsets) in UTF-8 text: upstream isUrl() with the schemes
// ftp:// http:// https:// mailto: file:// plus `extraSchemes` (space separated).
std::vector<std::pair<std::size_t, std::size_t>> findUrls(std::string_view text, std::string_view extraSchemes);
// Marks the links in the visible lines (or clears them when !enable).
void markLinks(const Sci& sci, bool enable, std::string_view extraSchemes);
// The link under `pos`, "" if none.
std::string linkAt(const Sci& sci, sptr_t pos);

// ---- Delimiter selection -----------------------------------------------------
// Selects between `open` and `close` around `pos` (the innermost pair for
// different characters); on the line only unless `multiLine`.
bool selectBetweenDelimiters(const Sci& sci, sptr_t pos, char open, char close, bool multiLine);

// ---- Auto-indent --------------------------------------------------------------
enum class IndentStyle { Plain, CLike, CLikeNoBraceless, Python };
// After typing `ch`: 1 basic (previous line's indentation), 2 advanced (also
// braces / Python colons). `pythonOperatorStyle` is the lexer's operator
// style for the colon check (SCE_P_OPERATOR).
void maintainIndentation(const Sci& sci, int ch, int mode, IndentStyle style);

// ---- Auto-insert ---------------------------------------------------------------
struct MatchedPairs {
    bool parentheses = false, brackets = false, braces = false, doubleQuotes = false, quotes = false;
    bool htmlXmlTag = false;
    std::vector<std::pair<char, char>> user; // user defined pairs
};
// Per editor: remembers the opening characters it completed, so typing the
// closing one steps over the inserted one (upstream InsertedMatchedChars).
class AutoInserter {
public:
    // After `ch` was typed. `markup`: 1 HTML, 2 XML, 0 neither.
    void onChar(const Sci& sci, int ch, const MatchedPairs& conf, int markup);

private:
    struct Inserted {
        char c;
        sptr_t pos;
    };
    void removeInvalid(const Sci& sci, char c, sptr_t pos);
    sptr_t search(const Sci& sci, char startChar, char endChar, sptr_t posToDetect);
    std::vector<Inserted> inserted_;
};
// "</tag>" for the tag just closed by '>', "" when none applies.
std::string closeTagFor(const Sci& sci, sptr_t caretPos, bool html);

// ---- Auto-completion ------------------------------------------------------------
// An upstream autoCompletion/<language>.xml.
struct ApiOverload {
    std::string retVal;
    std::string descr;
    std::vector<std::string> params;
};
struct ApiKeyword {
    std::string name;
    bool func = false;
    std::vector<ApiOverload> overloads;
};
struct ApiFile {
    std::vector<ApiKeyword> keywords; // sorted as upstream (case-insensitively unless ignoreCase="no")
    bool ignoreCase = true;
    char start = '(', stop = ')', param = ',', terminal = ';';
    std::string additionalWordChars;
    std::size_t maxLen = 0;
    bool load(const std::filesystem::path& file);
    const ApiKeyword* find(std::string_view name) const;
};
// Loads and caches the API file of a language: <dir>/<name>.xml (upstream's
// file names: "cpp", "javascript", a UDL's name).
class ApiCatalog {
public:
    explicit ApiCatalog(std::filesystem::path dir) : dir_(std::move(dir)) {}
    const ApiFile* forLanguage(const std::string& name);

private:
    std::filesystem::path dir_;
    std::map<std::string, std::unique_ptr<ApiFile>> cache_;
};

struct AutoCompleteOptions {
    bool enable = true;
    int mode = 2;            // 0 function, 1 word, 2 function and word
    int fromChar = 1;
    bool ignoreNumbers = true;
    bool brief = false;
    bool paramHint = true;
};

// Per editor: completion lists and the function call tip.
class AutoCompleter {
public:
    // After a character was typed (SCN_CHARADDED).
    void onChar(const Sci& sci, int ch, const AutoCompleteOptions& o, const ApiFile* api);
    // Edit > Auto-Completion commands.
    bool showFunctionList(const Sci& sci, const ApiFile* api);          // Ctrl+Space
    bool showWordList(const Sci& sci, const ApiFile* api, bool autoInsert, bool ignoreNumbers); // Ctrl+Enter
    bool showCallTip(const Sci& sci, const ApiFile* api);              // Ctrl+Shift+Space
    // SCN_CALLTIPCLICK: 1 previous overload, 2 next.
    void callTipClick(const Sci& sci, int direction);
    bool callTipShown() const { return tipShown_; }
    void closeCallTip(const Sci& sci);

private:
    bool showList(const Sci& sci, int type, bool autoInsert, const ApiFile* api, bool ignoreNumbers);
    bool updateCallTip(const Sci& sci, int ch, bool needShown, const ApiFile* api);
    bool findFunction(const Sci& sci, const ApiFile& api);
    void drawCallTip(const Sci& sci);

    const ApiFile* tipApi_ = nullptr;
    const ApiKeyword* tipFunc_ = nullptr;
    std::size_t overload_ = 0;
    std::size_t param_ = 0;
    sptr_t tipPos_ = 0;
    bool tipShown_ = false;
    bool imagesRegistered_ = false;
};

} // namespace npp::core
