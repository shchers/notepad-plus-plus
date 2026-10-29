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
            static_cast<std::set<std::string>*>(data)->insert(tag);
        }, &tags);
    return {tags.begin(), tags.end()};
}

std::string Speller::defaultLanguage()
{
    if (!available())
        return {};
    // "de_DE.UTF-8", "de_DE", "de", ..., "C".
    for (const gchar* const* n = g_get_language_names(); *n; ++n) {
        const std::string name = *n;
        if (name == "C" || name == "POSIX" || name.find('.') != std::string::npos || name.find('@') != std::string::npos)
            continue;
        if (api_->dictExists(broker_, name.c_str()))
            return name;
    }
    for (const char* fallback : {"en_US", "en_GB", "en"})
        if (api_->dictExists(broker_, fallback))
            return fallback;
    const std::vector<std::string> all = dictionaries();
    return all.empty() ? std::string() : all.front();
}

void Speller::freeDicts()
{
    for (void* d : dicts_)
        api_->freeDict(broker_, d);
    dicts_.clear();
}

void Speller::setLanguages(const std::vector<std::string>& tags)
{
    languages_.clear();
    cache_.clear();
    if (!available())
        return;
    freeDicts();
    for (const std::string& tag : tags) {
        if (std::find(languages_.begin(), languages_.end(), tag) != languages_.end())
            continue;
        if (void* d = api_->requestDict(broker_, tag.c_str())) {
            dicts_.push_back(d);
            languages_.push_back(tag);
        }
    }
}

void Speller::setPersonalDictionary(const std::string& path)
{
    personalPath_ = path;
    cache_.clear();
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

bool Speller::correct(const std::string& word)
{
    if (dicts_.empty())
        return true;
    const auto hit = cache_.find(word);
    if (hit != cache_.end())
        return hit->second;
    const auto len = static_cast<ssize_t>(word.size());
    bool ok = ignored_.count(word) != 0 || (personal_ && api_->check(personal_, word.c_str(), len) == 0);
    for (std::size_t i = 0; !ok && i < dicts_.size(); ++i)
        ok = api_->check(dicts_[i], word.c_str(), len) <= 0; // < 0: an error, not a misspelling
    if (cache_.size() > 100000)
        cache_.clear();
    cache_.emplace(word, ok);
    return ok;
}

std::vector<std::string> Speller::suggest(const std::string& word, std::size_t max)
{
    std::vector<std::string> out;
    // The dictionaries' in turn, best first, without repeats.
    std::vector<std::vector<std::string>> lists;
    for (void* d : dicts_) {
        std::size_t n = 0;
        char** s = api_->suggest(d, word.c_str(), static_cast<ssize_t>(word.size()), &n);
        lists.emplace_back(s, s + (s ? n : 0));
        if (s)
            api_->freeList(d, s);
    }
    for (std::size_t i = 0; out.size() < max; ++i) {
        bool any = false;
        for (const auto& l : lists) {
            if (i < l.size()) {
                any = true;
                if (out.size() < max && std::find(out.begin(), out.end(), l[i]) == out.end())
                    out.push_back(l[i]);
            }
        }
        if (!any)
            break;
    }
    return out;
}

void Speller::add(const std::string& word)
{
    if (personal_)
        api_->add(personal_, word.c_str(), static_cast<ssize_t>(word.size()));
    else
        ignored_.insert(word);
    cache_.clear();
}

void Speller::ignore(const std::string& word)
{
    ignored_.insert(word);
    cache_.clear();
}

} // namespace spellcheck
