// SpellCheck — Enchant, loaded with dlopen; see Speller.h.

#include "Speller.h"

#include <dlfcn.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <sys/types.h>

#include <algorithm>
#include <cstdio>
#include <set>
#include <type_traits>

namespace spellcheck {

// The part of Enchant 2's C API (enchant.h) used here; stable since 2.0.
struct EnchantApi {
    using DescribeFn = void (*)(const char* tag, const char* providerName, const char* providerDesc, const char* providerFile, void* data);

    void* lib = nullptr;
    void* (*brokerInit)() = nullptr;
    void (*brokerFree)(void*) = nullptr;
    void* (*requestDict)(void*, const char*) = nullptr;
    void* (*requestPwlDict)(void*, const char*) = nullptr;
    void (*freeDict)(void*, void*) = nullptr;
    int (*dictExists)(void*, const char*) = nullptr;
    void (*listDicts)(void*, DescribeFn, void*) = nullptr;
    const char* (*brokerError)(void*) = nullptr;
    int (*check)(void*, const char*, ssize_t) = nullptr;
    char** (*suggest)(void*, const char*, ssize_t, size_t*) = nullptr;
    void (*freeList)(void*, char**) = nullptr;
    void (*add)(void*, const char*, ssize_t) = nullptr;

    bool load(std::string& error)
    {
        for (const char* name : {"libenchant-2.so.2", "libenchant-2.so"}) {
            // Never unloaded: Enchant registers GObject types, which stay.
            lib = dlopen(name, RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE);
            if (lib)
                break;
        }
        if (!lib) {
            error = "Enchant is not installed (package libenchant-2-2).";
            return false;
        }
        bool ok = true;
        auto sym = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name));
            ok = ok && fn;
        };
        sym(brokerInit, "enchant_broker_init");
        sym(brokerFree, "enchant_broker_free");
        sym(requestDict, "enchant_broker_request_dict");
        sym(requestPwlDict, "enchant_broker_request_pwl_dict");
        sym(freeDict, "enchant_broker_free_dict");
        sym(dictExists, "enchant_broker_dict_exists");
        sym(listDicts, "enchant_broker_list_dicts");
        sym(brokerError, "enchant_broker_get_error");
        sym(check, "enchant_dict_check");
        sym(suggest, "enchant_dict_suggest");
        sym(freeList, "enchant_dict_free_string_list");
        sym(add, "enchant_dict_add");
        if (!ok)
            error = "The installed Enchant library is not version 2.";
        return ok;
    }
};

Speller::Speller() = default;

Speller::~Speller()
{
    if (broker_) {
        freeDicts();
        if (personal_)
            api_->freeDict(broker_, personal_);
        api_->brokerFree(broker_);
    }
}

bool Speller::available()
{
    if (!tried_) {
        tried_ = true;
        api_ = std::make_unique<EnchantApi>();
        if (api_->load(error_)) {
            broker_ = api_->brokerInit();
            if (!broker_)
                error_ = "Enchant could not start.";
        }
    }
    return broker_ != nullptr;
}

std::vector<std::string> Speller::dictionaries()
{
    std::set<std::string> tags;
    if (available())
        api_->listDicts(broker_, +[](const char* tag, const char*, const char*, const char*, void* data) {
            if (isLanguageTag(tag))
                static_cast<std::set<std::string>*>(data)->insert(tag);
        }, &tags);
    return {tags.begin(), tags.end()};
}

std::vector<std::string> Speller::userLanguages()
{
    // "de_AT.UTF-8", "de_AT", "de", ..., "C".
    std::vector<std::string> out;
    for (const gchar* const* n = g_get_language_names(); *n; ++n) {
        const std::string name = *n;
        if (name == "C" || name == "POSIX" || name.find('.') != std::string::npos || name.find('@') != std::string::npos)
            continue;
        out.push_back(name);
    }
    return out;
}

std::string Speller::defaultLanguage()
{
    if (!available())
        return {};
    for (const std::string& name : userLanguages())
        if (api_->dictExists(broker_, name.c_str()))
            return name;
    for (const char* fallback : {"en_US", "en_GB", "en"})
        if (api_->dictExists(broker_, fallback))
            return fallback;
    const std::vector<std::string> all = dictionaries();
    return all.empty() ? std::string() : all.front();
}

std::vector<std::string> Speller::allLanguages()
{
    std::vector<std::string> preferred = userLanguages();
    const std::string def = defaultLanguage();
    if (!def.empty())
        preferred.insert(preferred.begin(), def);
    return detectionLanguages(dictionaries(), preferred);
}

void Speller::freeDicts()
{
    for (Dict& d : dicts_)
        if (d.handle)
            api_->freeDict(broker_, d.handle);
    dicts_.clear();
}

void Speller::setLanguages(const std::vector<std::string>& tags)
{
    languages_.clear();
    knownCache_.clear();
    if (!available())
        return;
    freeDicts();
    for (const std::string& tag : tags) {
        if (std::find(languages_.begin(), languages_.end(), tag) != languages_.end() || !api_->dictExists(broker_, tag.c_str()))
            continue;
        Dict d;
        d.tag = tag;
        d.script = languageScript(tag);
        dicts_.push_back(std::move(d));
        languages_.push_back(tag);
    }
}

void* Speller::open(Dict& d)
{
    if (!d.opened) {
        d.opened = true;
        d.handle = api_->requestDict(broker_, d.tag.c_str());
        if (!d.handle)
            g_warning("SpellCheck: cannot open the %s dictionary: %s", d.tag.c_str(), api_->brokerError(broker_));
    }
    return d.handle;
}

bool Speller::accepts(Dict& d, const std::string& word)
{
    const auto hit = d.cache.find(word);
    if (hit != d.cache.end())
        return hit->second;
    void* h = open(d);
    // < 0: an error, not a misspelling.
    const bool ok = !h || api_->check(h, word.c_str(), static_cast<ssize_t>(word.size())) <= 0;
    if (d.cache.size() > 100000)
        d.cache.clear();
    d.cache.emplace(word, ok);
    return ok;
}

void Speller::setPersonalDictionary(const std::string& path)
{
    personalPath_ = path;
    knownCache_.clear();
    if (!available() || path.empty())
        return;
    if (personal_) {
        api_->freeDict(broker_, personal_);
        personal_ = nullptr;
    }
    if (!g_file_test(path.c_str(), G_FILE_TEST_EXISTS)) {
        gchar* dir = g_path_get_dirname(path.c_str());
        g_mkdir_with_parents(dir, 0700);
        g_free(dir);
        if (FILE* f = g_fopen(path.c_str(), "a"))
            std::fclose(f);
    }
    personal_ = api_->requestPwlDict(broker_, path.c_str());
}

bool Speller::known(const std::string& word)
{
    const auto hit = knownCache_.find(word);
    if (hit != knownCache_.end())
        return hit->second;
    const bool ok = ignored_.count(word) != 0 || (personal_ && api_->check(personal_, word.c_str(), static_cast<ssize_t>(word.size())) == 0);
    if (knownCache_.size() > 100000)
        knownCache_.clear();
    knownCache_.emplace(word, ok);
    return ok;
}

int Speller::detect(const std::vector<Word>& words, const std::vector<std::size_t>& which, GUnicodeScript script, int preferred)
{
    std::vector<int> candidates;
    for (std::size_t i = 0; i < dicts_.size(); ++i)
        if (dicts_[i].script == script)
            candidates.push_back(static_cast<int>(i));
    if (candidates.empty())
        return -1;
    if (preferred < 0 || dicts_[static_cast<std::size_t>(preferred)].script != script)
        preferred = candidates.front();
    if (candidates.size() == 1)
        return candidates.front();
    // A sample is enough to tell; the user's own words tell nothing.
    constexpr std::size_t kSample = 60;
    std::vector<const std::string*> sample;
    for (std::size_t i : which) {
        if (sample.size() == kSample)
            break;
        if (!known(words[i].text))
            sample.push_back(&words[i].text);
    }
    auto score = [&](int d) {
        int n = 0;
        for (const std::string* w : sample)
            n += accepts(dicts_[static_cast<std::size_t>(d)], *w);
        return n;
    };
    // Mostly the text is in the preferred language: no other is opened.
    const int preferredScore = score(preferred);
    if (preferredScore == static_cast<int>(sample.size()))
        return preferred;
    int best = preferred, bestScore = preferredScore;
    for (int d : candidates) {
        if (d == preferred)
            continue;
        const int s = score(d);
        if (s > bestScore) {
            best = d;
            bestScore = s;
        }
    }
    return best;
}

std::vector<std::size_t> Speller::misspelled(std::string_view text, std::vector<Word>& words)
{
    std::vector<std::size_t> bad;
    if (dicts_.empty() || words.empty())
        return bad;
    std::vector<GUnicodeScript> scripts(words.size());
    std::vector<GUnicodeScript> present;
    for (std::size_t i = 0; i < words.size(); ++i) {
        scripts[i] = wordScript(words[i].text);
        if (std::find(present.begin(), present.end(), scripts[i]) == present.end())
            present.push_back(scripts[i]);
    }
    // The language of the whole text, per script: what a paragraph too
    // short to tell is taken to be in.
    std::vector<int> textLang(present.size(), -1);
    for (std::size_t s = 0; s < present.size(); ++s) {
        std::vector<std::size_t> all;
        for (std::size_t i = 0; i < words.size(); ++i)
            if (scripts[i] == present[s])
                all.push_back(i);
        textLang[s] = detect(words, all, present[s], -1);
    }
    std::vector<std::size_t> starts = paragraphStarts(text, words);
    starts.push_back(words.size());
    for (std::size_t p = 0; p + 1 < starts.size(); ++p) {
        for (std::size_t s = 0; s < present.size(); ++s) {
            if (textLang[s] < 0)
                continue; // no dictionary for this script
            std::vector<std::size_t> group;
            for (std::size_t i = starts[p]; i < starts[p + 1]; ++i)
                if (scripts[i] == present[s])
                    group.push_back(i);
            if (group.empty())
                continue;
            const int lang = detect(words, group, present[s], textLang[s]);
            Dict& d = dicts_[static_cast<std::size_t>(lang)];
            for (std::size_t i : group) {
                if (known(words[i].text) || accepts(d, words[i].text))
                    continue;
                words[i].lang = d.tag;
                bad.push_back(i);
            }
        }
    }
    std::sort(bad.begin(), bad.end());
    return bad;
}

bool Speller::correct(const std::string& word)
{
    if (dicts_.empty() || known(word))
        return true;
    const GUnicodeScript script = wordScript(word);
    bool any = false;
    for (Dict& d : dicts_) {
        if (d.script != script)
            continue;
        any = true;
        if (accepts(d, word))
            return true;
    }
    return !any;
}

std::vector<std::string> Speller::suggest(const std::string& word, const std::string& lang, std::size_t max)
{
    std::vector<std::string> out;
    Dict* dict = nullptr;
    const GUnicodeScript script = wordScript(word);
    for (Dict& d : dicts_)
        if (lang.empty() ? d.script == script : d.tag == lang) {
            dict = &d;
            break;
        }
    void* h = dict ? open(*dict) : nullptr;
    if (!h)
        return out;
    std::size_t n = 0;
    char** s = api_->suggest(h, word.c_str(), static_cast<ssize_t>(word.size()), &n);
    for (std::size_t i = 0; s && i < n && out.size() < max; ++i)
        if (std::find(out.begin(), out.end(), s[i]) == out.end())
            out.emplace_back(s[i]);
    if (s)
        api_->freeList(h, s);
    return out;
}

void Speller::add(const std::string& word)
{
    if (personal_)
        api_->add(personal_, word.c_str(), static_cast<ssize_t>(word.size()));
    else
        ignored_.insert(word);
    knownCache_.clear();
}

void Speller::ignore(const std::string& word)
{
    ignored_.insert(word);
    knownCache_.clear();
}

void Speller::forget()
{
    knownCache_.clear();
    for (Dict& d : dicts_)
        d.cache.clear();
}

} // namespace spellcheck
