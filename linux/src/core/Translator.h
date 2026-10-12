// nativeLang localization — the port's NativeLangSpeaker. Loads upstream's
// nativeLang/<language>.xml unmodified, over english.xml so that strings a
// translation lacks fall back to English, and to the caller's fallback text
// when even english.xml is missing.
//
// Lookups mirror the file's structure:
//   menu entries   <Menu><Main><Entries><Item menuId="file" .../>
//   submenus       <Menu><Main><SubEntries><Item subMenuId="edit-indent" .../>
//   commands       <Menu><Main><Commands><Item id="41001" .../>  (IDM_* IDs)
//   dialogs        <Dialog><Find><Item id="1620" .../>  path "Find", or
//                  nested sections such as "Preference/Global"
//   panels         <FolderAsWorkspace><Menu><Item id="3511" .../> path
//                  "FolderAsWorkspace/Menu"; <DocList><ColumnName name=.../>
//                  through dialogAttr("DocList/ColumnName", "name")
//   misc strings   <MiscStrings><find-status-count-nb-matches value=.../>
//   message boxes  <MessageBox><DocTooDirtyToMonitor title=... message=.../>
//
// Labels keep upstream's Windows mnemonics ("&File"); toMnemonic() turns them
// into GTK ones ("_File").
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace npp::core {

class Translator {
public:
    struct Language {
        std::string name;     // <Native-Langue name="Français">
        std::string fileName; // "french.xml"
    };

    // `dir` holds english.xml and the translations. `fileName` empty or
    // "english.xml" means English only. Returns false if the requested
    // translation could not be read (English stays loaded when available).
    bool load(const std::filesystem::path& dir, const std::string& fileName, std::string* error = nullptr);
    static std::vector<Language> available(const std::filesystem::path& dir);

    const std::string& languageName() const { return name_; }
    const std::string& fileName() const { return file_; }
    bool rtl() const { return rtl_; }

    std::string menu(std::string_view menuId, std::string_view fallback) const;
    std::string subMenu(std::string_view subMenuId, std::string_view fallback) const;
    std::string command(int id, std::string_view fallback) const;
    std::string dialog(std::string_view path, int id, std::string_view fallback) const;
    std::string dialogAttr(std::string_view path, std::string_view attr, std::string_view fallback) const;
    std::string misc(std::string_view key, std::string_view fallback) const;
    std::string message(std::string_view box, std::string_view attr, std::string_view fallback) const;

    // "&File" -> "_File", "&&" -> "&", "_" -> "__"; drops a "\t…" tail.
    static std::string toMnemonic(std::string_view label);
    // Same without any mnemonic marker (titles, list entries).
    static std::string plain(std::string_view label);
    // Replaces upstream placeholders $INT_REPLACE$ / $STR_REPLACE$ (also
    // numbered: $INT_REPLACE1$ ...).
    static std::string format(std::string text, std::string_view placeholder, std::string_view value);

private:
    bool merge(const std::filesystem::path& file, std::string* error);
    static std::string key(std::string_view a, std::string_view b);
    std::string lookup(const std::string& k, std::string_view fallback) const;

    std::unordered_map<std::string, std::string> strings_;
    std::string name_ = "English";
    std::string file_ = "english.xml";
    bool rtl_ = false;
};

} // namespace npp::core
