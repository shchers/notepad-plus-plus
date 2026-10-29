// SpellCheck — the dictionaries, through Enchant (libenchant-2), which is
// loaded when first needed: without it the plugin says what to install
// instead of failing to load. Enchant finds the system's Hunspell, Nuspell
// and Aspell dictionaries (hunspell-de-de, aspell-fr...).
#pragma once

#include <memory>
#include <string>
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

    // The installed dictionaries ("en_US", "de_DE"), sorted.
    std::vector<std::string> dictionaries();
    // The first of the user's languages (LANGUAGE, LANG) with a dictionary,
    // else English, else any; empty when there is none.
    std::string defaultLanguage();
    // Checks against these; the ones without a dictionary are left out.
    void setLanguages(const std::vector<std::string>& tags);
    const std::vector<std::string>& languages() const { return languages_; }
    // The user's own words: a file of one word per line, created if missing.
    void setPersonalDictionary(const std::string& path);
    // Reads it again (edited in Notepad++).
    void reloadPersonalDictionary() { setPersonalDictionary(personalPath_); }

    // True also when nothing can check (no dictionary): nothing is marked.
    bool correct(const std::string& word);
    std::vector<std::string> suggest(const std::string& word, std::size_t max = 10);
    // To the personal dictionary.
    void add(const std::string& word);
    // Until Notepad++ quits.
    void ignore(const std::string& word);
    // Something changed what is correct; the next checks ask again.
    void forget() { cache_.clear(); }

private:
    void freeDicts();

    std::unique_ptr<EnchantApi> api_;
    bool tried_ = false;
    std::string error_;
    void* broker_ = nullptr;
    std::vector<void*> dicts_;
    void* personal_ = nullptr;
    std::string personalPath_;
    std::vector<std::string> languages_;
    std::unordered_set<std::string> ignored_;
    std::unordered_map<std::string, bool> cache_;
};

} // namespace spellcheck
