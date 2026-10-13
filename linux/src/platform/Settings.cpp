#include "platform/Settings.h"

#include "platform/Paths.h"

#include <glib.h>

#include <algorithm>
#include <memory>

namespace npp::platform {

namespace {
constexpr const char* kGroupView = "View";
constexpr const char* kGroupEditing = "Editing";
constexpr const char* kGroupBackup = "Backup";
constexpr const char* kGroupHistory = "History";
constexpr const char* kGroupWindow = "Window";
constexpr const char* kGroupFind = "Find";
constexpr const char* kGroupDocking = "Docking";
constexpr const char* kGroupPanels = "Panels";
using KeyFilePtr = std::unique_ptr<GKeyFile, decltype(&g_key_file_unref)>;
} // namespace

template <class V>
void Settings::visit(V& v)
{
    const char* g = "Preferences";
    v.item(g, "hideMenu", hideMenu);
    v.item(g, "toolbarIconSet", toolbarIconSet, 0, 4);
    v.item("TabBar", "reduce", tabReduce);
    v.item("TabBar", "lock", tabLock);
    v.item("TabBar", "inactiveColor", tabInactiveColor);
    v.item("TabBar", "activeBar", tabActiveBar);
    v.item("TabBar", "buttonsOnInactive", tabButtonsOnInactive);
    v.item("TabBar", "closeButton", tabCloseButton);
    v.item("TabBar", "doubleClickClose", tabDoubleClickClose);
    v.item("TabBar", "hide", tabHide);
    v.item("TabBar", "vertical", tabVertical);
    v.item("TabBar", "exitOnLast", tabExitOnLast);
    v.item("TabBar", "maxLabel", tabMaxLabel, 0, 256);
    v.item(kGroupView, "foldToggle", foldToggle);
    v.item(kGroupView, "smoothFont", smoothFont);
    v.item(kGroupView, "keepSelOnRightClick", keepSelOnRightClick);
    v.item(kGroupView, "copyLineNoSelection", copyLineNoSelection);
    v.item(kGroupView, "noSelDragDrop", noSelDragDrop);
    v.item(kGroupView, "selectedTextFg", selectedTextFg);
    v.item(kGroupView, "currentLineMode", currentLineMode, 0, 2);
    v.item(kGroupView, "currentLineFrame", currentLineFrame, 1, 6);
    v.item(kGroupView, "multiEditing", multiEditing);
    v.item(kGroupView, "columnToMultiEdit", columnToMultiEdit);
    v.item(kGroupView, "eolDisplay", eolDisplay, 0, 1);
    v.item(kGroupView, "eolCustomColor", eolCustomColor);
    v.item(kGroupView, "showNpc", showNpc);
    v.item(kGroupView, "showCcUniEol", showCcUniEol);
    v.item(kGroupView, "npcDisplay", npcDisplay, 0, 1);
    v.item(kGroupView, "npcCustomColor", npcCustomColor);
    v.item(kGroupView, "npcIncludeCcUniEol", npcIncludeCcUniEol);
    v.item(kGroupView, "preventC0", preventC0);
    v.item(kGroupView, "darkMode", darkMode, 0, 2);
    v.item(kGroupView, "lightTheme", lightTheme);
    v.item(kGroupView, "darkTheme", darkTheme);
    v.item(kGroupView, "lineNumberDynamic", lineNumberDynamic);
    v.item(kGroupView, "changeHistoryMargin", changeHistoryMargin);
    v.item(kGroupView, "changeHistoryText", changeHistoryText);
    v.item(kGroupView, "edgeColumns", edgeColumns);
    v.item(kGroupView, "paddingLeft", paddingLeft, 0, 30);
    v.item(kGroupView, "paddingRight", paddingRight, 0, 30);
    v.item(kGroupEditing, "newDocAtStartup", newDocAtStartup);
    v.item(kGroupEditing, "firstLineAsTabName", firstLineAsTabName);
    v.item(kGroupEditing, "defaultDirMode", defaultDirMode, 0, 2);
    v.item(kGroupEditing, "defaultDir", defaultDir);
    v.item(kGroupEditing, "dropFolderOpensFiles", dropFolderOpensFiles);
    v.item(g, "langMenuCompact", langMenuCompact);
    v.item(g, "langHidden", langHidden);
    v.item(g, "sqlBackslashEscape", sqlBackslashEscape);
    v.item(kGroupEditing, "autoIndentMode", autoIndentMode, 0, 2);
    v.item(kGroupEditing, "backspaceUnindent", backspaceUnindent);
    v.item("LanguageIndent", langIndent);
    v.item("Highlighting", "markMatchCase", markMatchCase);
    v.item("Highlighting", "markWholeWord", markWholeWord);
    v.item("Highlighting", "smart", smartHighlight);
    v.item("Highlighting", "smartMatchCase", smartMatchCase);
    v.item("Highlighting", "smartWholeWord", smartWholeWord);
    v.item("Highlighting", "smartUseFind", smartUseFind);
    v.item("Highlighting", "smartOtherView", smartOtherView);
    v.item("Highlighting", "tagMatch", tagMatch);
    v.item("Highlighting", "tagAttributes", tagAttributes);
    v.item("Highlighting", "tagZones", tagZones);
    v.item("Print", "lineNumbers", printLineNumbers);
    v.item("Print", "colorMode", printColorMode, 0, 3);
    v.item("Print", "marginLeft", printMarginLeft, 0, 100);
    v.item("Print", "marginTop", printMarginTop, 0, 100);
    v.item("Print", "marginRight", printMarginRight, 0, 100);
    v.item("Print", "marginBottom", printMarginBottom, 0, 100);
    v.item("Print", "headerLeft", headerLeft);
    v.item("Print", "headerMiddle", headerMiddle);
    v.item("Print", "headerRight", headerRight);
    v.item("Print", "footerLeft", footerLeft);
    v.item("Print", "footerMiddle", footerMiddle);
    v.item("Print", "footerRight", footerRight);
    v.item("Print", "headerBold", headerBold);
    v.item("Print", "headerItalic", headerItalic);
    v.item("Print", "footerBold", footerBold);
    v.item("Print", "footerItalic", footerItalic);
    v.item("Searching", "monospace", findMonospace);
    v.item("Searching", "staysOpen", findStaysOpen);
    v.item("Searching", "confirmReplaceAllDocs", confirmReplaceAllDocs);
    v.item("Searching", "replaceNoMove", replaceNoMove);
    v.item("Searching", "oneEntryPerLine", oneEntryPerLine);
    v.item("Searching", "fillWithSelection", fillFindWithSelection);
    v.item("Searching", "selectWordUnderCaret", selectWordUnderCaret);
    v.item("Searching", "inSelectionThreshold", inSelectionThreshold, 0, 1000000);
    v.item("Searching", "fifDirFromDoc", fifDirFromDoc);
    v.item("Searching", "fillMaxChars", fillFindMaxChars, 1, 16383);
    v.item(kGroupHistory, "checkAtLaunch", recentCheckAtLaunch);
    v.item(kGroupHistory, "submenu", recentSubmenu);
    v.item(kGroupHistory, "display", recentDisplay, 0, 2);
    v.item(kGroupHistory, "maxLength", recentMaxLen, 10, 1000);
    v.item(kGroupBackup, "keepAbsentSessionFiles", keepAbsentSessionFiles);
    v.item("AutoCompletion", "enable", acEnable);
    v.item("AutoCompletion", "mode", acMode, 0, 2);
    v.item("AutoCompletion", "insertTab", acInsertTab);
    v.item("AutoCompletion", "insertEnter", acInsertEnter);
    v.item("AutoCompletion", "ignoreNumbers", acIgnoreNumbers);
    v.item("AutoCompletion", "fromChar", acFromChar, 1, 9);
    v.item("AutoCompletion", "brief", acBrief);
    v.item("AutoCompletion", "paramHint", acParamHint);
    v.item("AutoCompletion", "closeTag", insertCloseTag);
    v.item("AutoCompletion", "parentheses", insertParen);
    v.item("AutoCompletion", "brackets", insertBracket);
    v.item("AutoCompletion", "braces", insertBrace);
    v.item("AutoCompletion", "doubleQuotes", insertDQuote);
    v.item("AutoCompletion", "quotes", insertSQuote);
    for (int i = 0; i < 3; ++i) {
        v.item("AutoCompletion", ("pairOpen" + std::to_string(i + 1)).c_str(), pairOpen[static_cast<std::size_t>(i)]);
        v.item("AutoCompletion", ("pairClose" + std::to_string(i + 1)).c_str(), pairClose[static_cast<std::size_t>(i)]);
    }
    v.item(g, "multiInstMode", multiInstMode, 0, 2);
    v.item(g, "dateReverse", dateReverse);
    v.item(g, "dateFormat", dateFormat);
    v.item(kGroupPanels, "noSessionClipboard", noSessionClipboard);
    v.item(kGroupPanels, "noSessionDocList", noSessionDocList);
    v.item(kGroupPanels, "noSessionCharPanel", noSessionCharPanel);
    v.item(kGroupPanels, "noSessionFolder", noSessionFolder);
    v.item(kGroupPanels, "noSessionProjects", noSessionProjects);
    v.item(kGroupPanels, "noSessionDocMap", noSessionDocMap);
    v.item(kGroupPanels, "noSessionFuncList", noSessionFuncList);
    v.item(kGroupPanels, "noSessionPlugins", noSessionPlugins);
    v.item(kGroupEditing, "delimOpen", delimOpen);
    v.item(kGroupEditing, "delimClose", delimClose);
    v.item(kGroupEditing, "delimMultiLine", delimMultiLine);
    v.item(kGroupEditing, "extraWordCharsOn", extraWordCharsOn);
    v.item(kGroupEditing, "extraWordChars", extraWordChars);
    v.item("Performance", "largeFileRestrict", largeFileRestrict);
    v.item("Performance", "largeFileMB", largeFileMB, 1, 2046);
    v.item("Performance", "allowBraceMatch", largeAllowBrace);
    v.item("Performance", "allowAutoCompletion", largeAllowAC);
    v.item("Performance", "allowSmartHighlight", largeAllowSmart);
    v.item("Performance", "noWordWrap", largeNoWrap);
    v.item("Performance", "allowClickableLinks", largeAllowLinks);
    v.item("Links", "enable", linkEnable);
    v.item("Links", "noUnderline", linkNoUnderline);
    v.item("Links", "fullBox", linkFullBox);
    v.item("Links", "schemes", linkSchemes);
    v.item(g, "searchEngine", searchEngine, 0, 4);
    v.item(g, "searchEngineCustom", searchEngineCustom);
    v.item(g, "scrollToEndAfterUpdate", scrollToEndAfterUpdate);
    v.item(g, "sessionExt", sessionExt);
    v.item(g, "workspaceExt", workspaceExt);
    v.item(g, "docSwitcher", docSwitcher);
    v.item(g, "docSwitcherMru", docSwitcherMru);
    v.item(g, "titleFileNameOnly", titleFileNameOnly);
    v.item(g, "autodetectEncoding", autodetectEncoding);
    v.item(g, "muteSounds", muteSounds);
    v.item(g, "saveAllConfirm", saveAllConfirm);
    v.item(g, "folderSymlinks", faWSymlinks);
}

namespace {

struct Reader {
    GKeyFile* kf;
    template <class T>
    bool get(const char* group, const char* key, T (*fn)(GKeyFile*, const gchar*, const gchar*, GError**), T& out)
    {
        GError* e = nullptr;
        const T v = fn(kf, group, key, &e);
        if (e) {
            g_error_free(e);
            return false;
        }
        out = v;
        return true;
    }
    void item(const char* group, const char* key, bool& out)
    {
        gboolean v = FALSE;
        if (get<gboolean>(group, key, &g_key_file_get_boolean, v))
            out = v;
    }
    void item(const char* group, const char* key, int& out, int lo, int hi)
    {
        gint v = 0;
        if (get<gint>(group, key, &g_key_file_get_integer, v))
            out = std::clamp(v, lo, hi);
    }
    void item(const char* group, const char* key, std::string& out)
    {
        if (gchar* t = g_key_file_get_string(kf, group, key, nullptr)) {
            out = t;
            g_free(t);
        }
    }
    void item(const char* group, const char* key, std::vector<std::string>& out)
    {
        gsize n = 0;
        if (gchar** list = g_key_file_get_string_list(kf, group, key, &n, nullptr)) {
            out.assign(list, list + n);
            g_strfreev(list);
        }
    }
    void item(const char* group, std::map<std::string, std::string>& out)
    {
        if (gchar** keys = g_key_file_get_keys(kf, group, nullptr, nullptr)) {
            for (gchar** k = keys; *k; ++k)
                item(group, *k, out[*k]);
            g_strfreev(keys);
        }
    }
};

struct Writer {
    GKeyFile* kf;
    void item(const char* group, const char* key, bool& v) { g_key_file_set_boolean(kf, group, key, v); }
    void item(const char* group, const char* key, int& v, int, int) { g_key_file_set_integer(kf, group, key, v); }
    void item(const char* group, const char* key, std::string& v) { g_key_file_set_string(kf, group, key, v.c_str()); }
    void item(const char* group, const char* key, std::vector<std::string>& v)
    {
        std::vector<const gchar*> list;
        for (const std::string& s : v)
            list.push_back(s.c_str());
        g_key_file_set_string_list(kf, group, key, list.data(), list.size());
    }
    void item(const char* group, std::map<std::string, std::string>& m)
    {
        for (auto& [k, val] : m)
            g_key_file_set_string(kf, group, k.c_str(), val.c_str());
    }
};

} // namespace

std::filesystem::path Settings::defaultPath()
{
    return userConfigDir() / "settings.ini";
}

bool Settings::load(const std::filesystem::path& file)
{
    KeyFilePtr kf(g_key_file_new(), &g_key_file_unref);
    if (!g_key_file_load_from_file(kf.get(), file.c_str(), G_KEY_FILE_NONE, nullptr))
        return false; // first run: keep defaults

    auto getBool = [&](const char* group, const char* key, bool& out) {
        GError* e = nullptr;
        const gboolean v = g_key_file_get_boolean(kf.get(), group, key, &e);
        if (e)
            g_error_free(e);
        else
            out = v;
    };
    auto getInt = [&](const char* group, const char* key, int& out, int lo, int hi) {
        GError* e = nullptr;
        const gint v = g_key_file_get_integer(kf.get(), group, key, &e);
        if (e)
            g_error_free(e);
        else
            out = std::clamp(v, lo, hi);
    };
    auto getString = [&](const char* group, const char* key, std::string& out) {
        if (gchar* t = g_key_file_get_string(kf.get(), group, key, nullptr)) {
            out = t;
            g_free(t);
        }
    };
    getString(kGroupView, "theme", theme);
    getBool(kGroupView, "wordWrap", wordWrap);
    getString(kGroupView, "localization", localization);
    bool all = false;
    getBool(kGroupView, "showAllChars", all); // before the split into two settings
    showSpaces = showEol = all;
    getBool(kGroupView, "showSpaces", showSpaces);
    getBool(kGroupView, "showEol", showEol);
    getBool(kGroupView, "showWrapSymbol", showWrapSymbol);
    getBool(kGroupView, "showIndentGuides", showIndentGuides);
    getInt(kGroupView, "zoom", zoom, -10, 20);

    getBool(kGroupView, "hideToolbar", hideToolbar);
    getBool(kGroupView, "hideStatusBar", hideStatusBar);
    getInt(kGroupView, "wrapIndent", wrapIndent, 0, 2);
    getInt(kGroupView, "caretWidth", caretWidth, 0, 3);
    getInt(kGroupView, "caretBlinkMs", caretBlinkMs, 0, 2500);
    getBool(kGroupView, "scrollBeyondLastLine", scrollBeyondLastLine);
    getBool(kGroupView, "virtualSpace", virtualSpace);
    getBool(kGroupView, "currentLineHighlight", currentLineHighlight);
    getInt(kGroupView, "foldStyle", foldStyle, 0, 4);
    getBool(kGroupView, "showLineNumbers", showLineNumbers);
    getBool(kGroupView, "showBookmarkMargin", showBookmarkMargin);
    getInt(kGroupView, "edgeMode", edgeMode, 0, 2);
    getInt(kGroupView, "edgeColumn", edgeColumn, 1, 1000);
    getInt(kGroupEditing, "newDocEol", newDocEol, 0, 2);
    getInt(kGroupEditing, "newDocEncoding", newDocEncoding, 0, 3);
    getString(kGroupEditing, "newDocLanguage", newDocLanguage);
    getInt(kGroupEditing, "tabSize", tabSize, 1, 64);
    getBool(kGroupEditing, "replaceTabsBySpace", replaceTabsBySpace);
    getBool(kGroupEditing, "autoIndent", autoIndent);
    int detection = static_cast<int>(changeDetection);
    getInt(kGroupEditing, "changeDetection", detection, 0, 2);
    changeDetection = static_cast<ChangeDetection>(detection);

    getBool(kGroupBackup, "rememberSession", rememberSession);
    getBool(kGroupBackup, "snapshot", snapshot);
    getInt(kGroupBackup, "snapshotIntervalMs", snapshotIntervalMs, 1000, 3600 * 1000);
    int backup = static_cast<int>(saveBackup);
    getInt(kGroupBackup, "saveBackup", backup, 0, 2);
    saveBackup = static_cast<SaveBackup>(backup);
    getString(kGroupBackup, "saveBackupDir", saveBackupDir);

    getInt(kGroupHistory, "maxRecentFiles", maxRecentFiles, 0, 30); // upstream NB_MAX_LRF_FILE
    gsize n = 0;
    if (gchar** list = g_key_file_get_string_list(kf.get(), kGroupHistory, "recentFiles", &n, nullptr)) {
        recentFiles.assign(list, list + n);
        g_strfreev(list);
    }
    if (gchar** list = g_key_file_get_string_list(kf.get(), kGroupHistory, "run", &n, nullptr)) {
        runHistory.assign(list, list + std::min<gsize>(n, 10));
        g_strfreev(list);
    }

    auto getList = [&](const char* key, std::vector<std::string>& out) {
        gsize n = 0;
        if (gchar** list = g_key_file_get_string_list(kf.get(), kGroupFind, key, &n, nullptr)) {
            out.assign(list, list + std::min<gsize>(n, FindSettings::kMaxHistory));
            g_strfreev(list);
        }
    };
    getList("what", find.what);
    getList("with", find.with);
    getList("filters", find.filters);
    getList("directories", find.directories);
    getInt(kGroupFind, "mode", find.mode, 0, 2);
    getBool(kGroupFind, "matchCase", find.matchCase);
    getBool(kGroupFind, "wholeWord", find.wholeWord);
    getBool(kGroupFind, "wrap", find.wrap);
    getBool(kGroupFind, "backward", find.backward);
    getBool(kGroupFind, "dotMatchesNewline", find.dotMatchesNewline);
    getBool(kGroupFind, "inSelection", find.inSelection);
    getBool(kGroupFind, "recursive", find.recursive);
    getBool(kGroupFind, "hidden", find.hidden);
    getBool(kGroupFind, "bookmarkLine", find.bookmarkLine);
    getBool(kGroupFind, "purge", find.purge);

    if (gchar** keys = g_key_file_get_keys(kf.get(), kGroupDocking, nullptr, nullptr)) {
        for (gchar** k = keys; *k; ++k)
            getString(kGroupDocking, *k, docking[*k]);
        g_strfreev(keys);
    }
    for (std::size_t i = 0; i < projectWorkspaces.size(); ++i)
        getString(kGroupPanels, ("projectWorkspace" + std::to_string(i + 1)).c_str(), projectWorkspaces[i]);
    getBool(kGroupPanels, "functionListSorted", functionListSorted);

    // Settings that became multiple choices; their new keys win when present.
    autoIndentMode = autoIndent ? 2 : 0;
    currentLineMode = currentLineHighlight ? 1 : 0;
    edgeColumns = std::to_string(edgeColumn);
    Reader reader{kf.get()};
    visit(reader);
    autoIndent = autoIndentMode != 0;
    currentLineHighlight = currentLineMode != 0;

    getInt(kGroupWindow, "width", width, 200, 16384);
    getInt(kGroupWindow, "height", height, 150, 16384);
    getBool(kGroupWindow, "maximized", maximized);
    getInt(kGroupWindow, "splitPosition", splitPosition, -1, 16384);
    return true;
}

bool Settings::save(const std::filesystem::path& file) const
{
    KeyFilePtr kf(g_key_file_new(), &g_key_file_unref);
    g_key_file_set_string(kf.get(), kGroupView, "theme", theme.c_str());
    g_key_file_set_boolean(kf.get(), kGroupView, "wordWrap", wordWrap);
    g_key_file_set_string(kf.get(), kGroupView, "localization", localization.c_str());
    g_key_file_set_boolean(kf.get(), kGroupView, "showSpaces", showSpaces);
    g_key_file_set_boolean(kf.get(), kGroupView, "showEol", showEol);
    g_key_file_set_boolean(kf.get(), kGroupView, "showWrapSymbol", showWrapSymbol);
    g_key_file_set_boolean(kf.get(), kGroupView, "showIndentGuides", showIndentGuides);
    g_key_file_set_integer(kf.get(), kGroupView, "zoom", zoom);

    g_key_file_set_boolean(kf.get(), kGroupView, "hideToolbar", hideToolbar);
    g_key_file_set_boolean(kf.get(), kGroupView, "hideStatusBar", hideStatusBar);
    g_key_file_set_integer(kf.get(), kGroupView, "wrapIndent", wrapIndent);
    g_key_file_set_integer(kf.get(), kGroupView, "caretWidth", caretWidth);
    g_key_file_set_integer(kf.get(), kGroupView, "caretBlinkMs", caretBlinkMs);
    g_key_file_set_boolean(kf.get(), kGroupView, "scrollBeyondLastLine", scrollBeyondLastLine);
    g_key_file_set_boolean(kf.get(), kGroupView, "virtualSpace", virtualSpace);
    g_key_file_set_boolean(kf.get(), kGroupView, "currentLineHighlight", currentLineHighlight);
    g_key_file_set_integer(kf.get(), kGroupView, "foldStyle", foldStyle);
    g_key_file_set_boolean(kf.get(), kGroupView, "showLineNumbers", showLineNumbers);
    g_key_file_set_boolean(kf.get(), kGroupView, "showBookmarkMargin", showBookmarkMargin);
    g_key_file_set_integer(kf.get(), kGroupView, "edgeMode", edgeMode);
    g_key_file_set_integer(kf.get(), kGroupView, "edgeColumn", edgeColumn);
    g_key_file_set_integer(kf.get(), kGroupEditing, "newDocEol", newDocEol);
    g_key_file_set_integer(kf.get(), kGroupEditing, "newDocEncoding", newDocEncoding);
    g_key_file_set_string(kf.get(), kGroupEditing, "newDocLanguage", newDocLanguage.c_str());
    g_key_file_set_integer(kf.get(), kGroupEditing, "tabSize", tabSize);
    g_key_file_set_boolean(kf.get(), kGroupEditing, "replaceTabsBySpace", replaceTabsBySpace);
    g_key_file_set_boolean(kf.get(), kGroupEditing, "autoIndent", autoIndent);
    g_key_file_set_integer(kf.get(), kGroupEditing, "changeDetection", static_cast<int>(changeDetection));

    g_key_file_set_boolean(kf.get(), kGroupBackup, "rememberSession", rememberSession);
    g_key_file_set_boolean(kf.get(), kGroupBackup, "snapshot", snapshot);
    g_key_file_set_integer(kf.get(), kGroupBackup, "snapshotIntervalMs", snapshotIntervalMs);
    g_key_file_set_integer(kf.get(), kGroupBackup, "saveBackup", static_cast<int>(saveBackup));
    g_key_file_set_string(kf.get(), kGroupBackup, "saveBackupDir", saveBackupDir.c_str());

    g_key_file_set_integer(kf.get(), kGroupHistory, "maxRecentFiles", maxRecentFiles);
    std::vector<const gchar*> list;
    for (const std::string& f : recentFiles)
        list.push_back(f.c_str());
    g_key_file_set_string_list(kf.get(), kGroupHistory, "recentFiles", list.data(), list.size());
    list.clear();
    for (const std::string& r : runHistory)
        list.push_back(r.c_str());
    g_key_file_set_string_list(kf.get(), kGroupHistory, "run", list.data(), list.size());

    auto setList = [&](const char* key, const std::vector<std::string>& in) {
        std::vector<const gchar*> v;
        for (const std::string& s : in)
            v.push_back(s.c_str());
        g_key_file_set_string_list(kf.get(), kGroupFind, key, v.data(), v.size());
    };
    setList("what", find.what);
    setList("with", find.with);
    setList("filters", find.filters);
    setList("directories", find.directories);
    g_key_file_set_integer(kf.get(), kGroupFind, "mode", find.mode);
    g_key_file_set_boolean(kf.get(), kGroupFind, "matchCase", find.matchCase);
    g_key_file_set_boolean(kf.get(), kGroupFind, "wholeWord", find.wholeWord);
    g_key_file_set_boolean(kf.get(), kGroupFind, "wrap", find.wrap);
    g_key_file_set_boolean(kf.get(), kGroupFind, "backward", find.backward);
    g_key_file_set_boolean(kf.get(), kGroupFind, "dotMatchesNewline", find.dotMatchesNewline);
    g_key_file_set_boolean(kf.get(), kGroupFind, "inSelection", find.inSelection);
    g_key_file_set_boolean(kf.get(), kGroupFind, "recursive", find.recursive);
    g_key_file_set_boolean(kf.get(), kGroupFind, "hidden", find.hidden);
    g_key_file_set_boolean(kf.get(), kGroupFind, "bookmarkLine", find.bookmarkLine);
    g_key_file_set_boolean(kf.get(), kGroupFind, "purge", find.purge);

    for (const auto& [k, v] : docking)
        g_key_file_set_string(kf.get(), kGroupDocking, k.c_str(), v.c_str());
    for (std::size_t i = 0; i < projectWorkspaces.size(); ++i)
        g_key_file_set_string(kf.get(), kGroupPanels, ("projectWorkspace" + std::to_string(i + 1)).c_str(), projectWorkspaces[i].c_str());
    g_key_file_set_boolean(kf.get(), kGroupPanels, "functionListSorted", functionListSorted);

    Writer writer{kf.get()};
    const_cast<Settings*>(this)->visit(writer);

    g_key_file_set_integer(kf.get(), kGroupWindow, "width", width);
    g_key_file_set_integer(kf.get(), kGroupWindow, "height", height);
    g_key_file_set_boolean(kf.get(), kGroupWindow, "maximized", maximized);
    g_key_file_set_integer(kf.get(), kGroupWindow, "splitPosition", splitPosition);
    return g_key_file_save_to_file(kf.get(), file.c_str(), nullptr);
}

} // namespace npp::platform
