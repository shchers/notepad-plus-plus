// SpellCheck — the dictionaries, through Enchant (libenchant-2), which is
// loaded when first needed: without it the plugin says what to install
// instead of failing to load. Enchant finds the system's Hunspell, Nuspell
// and Aspell dictionaries (hunspell-de-de, aspell-fr...).
//
// A text is checked in one language per paragraph and script, detected
// among the chosen ones: the one that knows most of its words (ties: the
// one the text around it is in, then the first chosen). So English and
// German paragraphs, or Russian words in an English one, are each checked
// in their own language, and a typo is not excused by some other language
// that happens to have the word.
#pragma once

#include "SpellText.h"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace spellcheck {

struct EnchantApi;

class Speller {
public:
    Speller();
    ~Speller();
    Speller(const Speller&) = delete;
    Speller& operator=(const Speller&) = delete;

    // Enchant is installed; otherwise error() says why not.
    bool available();
    const std::string& error() const { return error_; }

    // The installed dictionaries ("en_US", "de_DE"), sorted, without
    // Aspell's variants ("en-variant_0").
    std::vector<std::string> dictionaries();
    // The user's languages (LANGUAGE, LANG: "de_AT", "de"), best first.
    static std::vector<std::string> userLanguages();
    // The first of the user's languages with a dictionary, else English,
    // else any; empty when there is none.
    std::string defaultLanguage();
    // One dictionary per installed language, the user's first: what
    // detection chooses from in automatic mode.
    std::vector<std::string> allLanguages();
    // Checks in these (detecting which, the first preferred); the ones
    // without a dictionary are left out. A dictionary is opened when a
    // text in its script first needs it.
    void setLanguages(const std::vector<std::string>& tags);
    const std::vector<std::string>& languages() const { return languages_; }
    // The user's own words: a file of one word per line, created if missing.
    void setPersonalDictionary(const std::string& path);
    // Reads it again (edited in Notepad++).
    void reloadPersonalDictionary() { setPersonalDictionary(personalPath_); }

    // The misspelled words of `text` (indexes into words, which findWords
    // found in it), each with its lang set. Words in a script no chosen
    // language is written in are not checked.
    std::vector<std::size_t> misspelled(std::string_view text, std::vector<Word>& words);
    // In any chosen language of its script (no detection).
    bool correct(const std::string& word);
    // In `lang` (empty: the first language of the word's script).
    std::vector<std::string> suggest(const std::string& word, const std::string& lang = {}, std::size_t max = 10);
    // To the personal dictionary.
    void add(const std::string& word);
    // Until Notepad++ quits.
    void ignore(const std::string& word);
    // Something changed what is correct; the next checks ask again.
    void forget();

private:
    struct Dict {
        std::string tag;
        GUnicodeScript script = G_UNICODE_SCRIPT_LATIN;
        void* handle = nullptr;
        bool opened = false;
        std::unordered_map<std::string, bool> cache;
    };
    void freeDicts();
    void* open(Dict& d);
    bool accepts(Dict& d, const std::string& word);
    // The user's own words: personal dictionary, Ignore.
    bool known(const std::string& word);
    // The language (index into dicts_) of these words, all in `script`;
    // -1 when none is written in it.
    int detect(const std::vector<Word>& words, const std::vector<std::size_t>& which, GUnicodeScript script, int preferred);

    std::unique_ptr<EnchantApi> api_;
    bool tried_ = false;
    std::string error_;
    void* broker_ = nullptr;
    std::vector<Dict> dicts_;
    void* personal_ = nullptr;
    std::string personalPath_;
    std::vector<std::string> languages_;
    std::unordered_set<std::string> ignored_;
    std::unordered_map<std::string, bool> knownCache_;
};

} // namespace spellcheck
