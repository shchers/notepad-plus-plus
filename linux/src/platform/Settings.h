// Persistent user settings — the port's stand-in for upstream config.xml
// (NppGUI). Stored as an INI file via GKeyFile at
// $XDG_CONFIG_HOME/notepad-plus-plus-gtk/settings.ini. Settings from a
// Windows config.xml can be imported (core/NppConfig).
#pragma once

#include <filesystem>
#include <array>
#include <map>
#include <string>
#include <vector>

namespace npp::platform {

// Upstream BackupFeature (Preferences > Backup > "Backup on save").
enum class SaveBackup { None = 0, Simple = 1, Verbose = 2 };
// Upstream "File Status Auto-Detection" (Preferences > MISC.).
enum class ChangeDetection { Off = 0, Notify = 1, AutoUpdate = 2 };

// Find/Replace dialog state (upstream config.xml <FindHistory>).
struct FindSettings {
    std::vector<std::string> what, with, filters, directories; // most recent first
    int mode = 0;                 // core::SearchMode
    bool matchCase = false;
    bool wholeWord = false;
    bool wrap = true;
    bool backward = false;
    bool dotMatchesNewline = false;
    bool inSelection = false;
    bool recursive = true;
    bool hidden = false;
    bool bookmarkLine = false;
    bool purge = false;
    static constexpr std::size_t kMaxHistory = 10; // upstream nbMaxFindHistoryPath
};

struct Settings {
    std::string theme;          // theme file name in themes/, empty = stylers.model.xml
    std::string localization;   // nativeLang file name, empty = english.xml
    bool wordWrap = false;
    bool showSpaces = false;    // View > Show Symbol (upstream whiteSpaceShow)
    bool showEol = false;       // (eolShow); both on = "Show All Characters"
    bool showIndentGuides = true;
    bool showWrapSymbol = false;
    int zoom = 0;
    // Preferences > General / Editing / Margins (upstream NppGUI, ScintillaViewParams)
    bool hideToolbar = false;
    bool hideStatusBar = false;
    int wrapIndent = 0;            // 0 Default (fixed), 1 Aligned, 2 Indent
    int caretWidth = 1;            // pixels, 0 = block
    int caretBlinkMs = 600;        // 0 = no blinking
    bool scrollBeyondLastLine = true;
    bool virtualSpace = false;
    bool currentLineHighlight = true;
    int foldStyle = 3;             // 0 Simple, 1 Arrow, 2 Circle tree, 3 Box tree, 4 None
    bool showLineNumbers = true;
    bool showBookmarkMargin = true;
    int edgeMode = 0;              // 0 none, 1 line, 2 background
    int edgeColumn = 80;
    // Preferences > New Document
    int newDocEol = 2;             // SC_EOL_LF
    int newDocEncoding = 0;        // 0 UTF-8, 1 UTF-8 BOM, 2 UTF-16 BE BOM, 3 UTF-16 LE BOM
    std::string newDocLanguage;    // langs.model.xml name, empty = Normal text
    int tabSize = 4;
    bool replaceTabsBySpace = false;
    bool autoIndent = true;

    // Session and backup, upstream defaults.
    bool rememberSession = true;
    bool snapshot = true;          // session snapshot + periodic backup of unsaved documents
    int snapshotIntervalMs = 7000;
    SaveBackup saveBackup = SaveBackup::None;
    std::string saveBackupDir;     // empty = next to the file (upstream "useCustumDir" off)
    ChangeDetection changeDetection = ChangeDetection::Notify;

    FindSettings find;

    // ---- Preferences pages added later (upstream NppGUI / ScintillaViewParams;
    // defaults are upstream's). Read and written through visit() below.
    // General
    bool hideMenu = false;               // Alt or F10 shows it
    // Toolbar: 0 Fluent small, 1 Fluent large, 2 Filled Fluent small, 3 Filled Fluent large, 4 Standard small
    int toolbarIconSet = 0;
    // Tab Bar
    bool tabReduce = false;
    bool tabLock = false;                // no drag and drop
    bool tabInactiveColor = true;
    bool tabActiveBar = true;
    bool tabButtonsOnInactive = true;
    bool tabCloseButton = true;
    bool tabDoubleClickClose = false;
    bool tabHide = false;
    bool tabVertical = false;
    bool tabExitOnLast = false;
    int tabMaxLabel = 0;                 // characters, 0 = no limit
    // Editing 1
    bool foldToggle = false;             // fold/unfold current level toggles
    bool smoothFont = false;
    bool keepSelOnRightClick = true;
    bool copyLineNoSelection = true;
    bool noSelDragDrop = false;
    bool selectedTextFg = false;         // "Selected text colour" foreground
    int currentLineMode = 1;             // 0 none, 1 background, 2 frame
    int currentLineFrame = 1;            // frame width, pixels
    // Editing 2
    bool multiEditing = true;
    bool columnToMultiEdit = true;
    int eolDisplay = 0;                  // 0 default (rounded box), 1 plain text
    bool eolCustomColor = false;
    bool showNpc = false;                // View > Show Symbol > Show Non-Printing Characters
    bool showCcUniEol = false;           // ... > Show Control Characters & Unicode EOL
    int npcDisplay = 0;                  // non-printing characters: 0 abbreviation, 1 codepoint
    bool npcCustomColor = false;
    bool npcIncludeCcUniEol = false;
    bool preventC0 = true;
    // Dark Mode: 0 light, 1 dark, 2 follow the desktop
    int darkMode = 0;
    std::string lightTheme;              // theme to return to from dark mode
    std::string darkTheme = "DarkModeDefault.xml";
    // Margins/Border/Edge
    bool lineNumberDynamic = true;
    bool changeHistoryMargin = true;
    bool changeHistoryText = false;
    std::string edgeColumns = "80";      // several columns separated by spaces (line mode)
    int paddingLeft = 0;
    int paddingRight = 0;
    // New Document
    bool newDocAtStartup = false;
    bool firstLineAsTabName = false;
    // Default Directory: 0 follow current document, 1 remember last used, 2 this directory
    int defaultDirMode = 0;
    std::string defaultDir;
    bool dropFolderOpensFiles = false;
    // Language
    bool langMenuCompact = true;
    std::vector<std::string> langHidden; // langs.model.xml names left out of the Language menu
    bool sqlBackslashEscape = true;
    // Indentation
    int autoIndentMode = 2;              // 0 none, 1 basic, 2 advanced (autoIndent follows it)
    bool backspaceUnindent = false;
    std::map<std::string, std::string> langIndent; // language -> "size,useSpaces"
    // Highlighting
    bool markMatchCase = false;
    bool markWholeWord = true;
    bool smartHighlight = true;
    bool smartMatchCase = false;
    bool smartWholeWord = true;
    bool smartUseFind = false;
    bool smartOtherView = true;
    bool tagMatch = true;
    bool tagAttributes = true;
    bool tagZones = true;
    // Print
    bool printLineNumbers = true;
    int printColorMode = 0;              // 0 WYSIWYG, 1 invert, 2 black on white, 3 no background
    int printMarginLeft = 0, printMarginTop = 0, printMarginRight = 0, printMarginBottom = 0; // mm
    std::string headerLeft, headerMiddle, headerRight;
    std::string footerLeft, footerMiddle, footerRight;
    bool headerBold = false, headerItalic = false, footerBold = false, footerItalic = false;
    // Searching
    bool findMonospace = false;
    bool findStaysOpen = false;
    bool confirmReplaceAllDocs = true;
    bool replaceNoMove = false;
    bool oneEntryPerLine = true;
    bool fillFindWithSelection = true;
    bool selectWordUnderCaret = true;
    int inSelectionThreshold = 1024;     // characters
    bool fifDirFromDoc = false;
    int fillFindMaxChars = 1024;
    // Recent Files History
    bool recentCheckAtLaunch = true;
    bool recentSubmenu = false;
    int recentDisplay = 1;               // 0 file name, 1 full path, 2 full path cut to recentMaxLen
    int recentMaxLen = 100;
    // Backup
    bool keepAbsentSessionFiles = false;
    // Auto-Completion
    bool acEnable = true;
    int acMode = 2;                      // 0 function, 1 word, 2 function and word
    bool acInsertTab = true;
    bool acInsertEnter = true;
    bool acIgnoreNumbers = true;
    int acFromChar = 1;
    bool acBrief = false;
    bool acParamHint = true;
    bool insertCloseTag = false;
    bool insertParen = false, insertBracket = false, insertBrace = false, insertDQuote = false, insertSQuote = false;
    std::array<std::string, 3> pairOpen, pairClose; // user matched pairs, one character each
    // Multi-Instance & Date
    int multiInstMode = 0;               // 0 mono-instance, 1 session in a new instance, 2 always multi-instance
    bool dateReverse = false;
    std::string dateFormat = "yyyy-MM-dd HH:mm:ss";
    // Panels restored with -nosession / in other instances
    bool noSessionClipboard = false, noSessionDocList = false, noSessionCharPanel = false, noSessionFolder = false;
    bool noSessionProjects = false, noSessionDocMap = false, noSessionFuncList = false, noSessionPlugins = false;
    // Delimiter
    std::string delimOpen = "(", delimClose = ")";
    bool delimMultiLine = false;
    bool extraWordCharsOn = false;
    std::string extraWordChars;
    // Performance
    bool largeFileRestrict = true;
    int largeFileMB = 200;
    bool largeAllowBrace = false, largeAllowAC = false, largeAllowSmart = false, largeNoWrap = true, largeAllowLinks = false;
    // Clickable links
    bool linkEnable = true;
    bool linkNoUnderline = false;
    bool linkFullBox = false;
    std::string linkSchemes = "svn:// cvs:// git:// imap:// irc:// irc6:// ircs:// ldap:// ldaps:// news: telnet:// gopher:// ssh:// sftp:// smb:// skype: snmp:// spotify: steam:// sms: slack:// chrome:// bitcoin:";
    // Search Engine: 0 DuckDuckGo, 1 Google, 2 Bing, 3 Yahoo!, 4 custom
    int searchEngine = 0;
    std::string searchEngineCustom;
    // MISC.
    bool scrollToEndAfterUpdate = false;
    std::string sessionExt, workspaceExt;
    bool docSwitcher = true;
    bool docSwitcherMru = true;
    bool titleFileNameOnly = false;
    bool autodetectEncoding = true;
    bool muteSounds = false;
    bool saveAllConfirm = false;
    bool faWSymlinks = true;

    // Calls v.item(group, key, field[, lo, hi]) for each field above
    // (bool, int with bounds, string, string list, string map).
    template <class V>
    void visit(V& v);

    int maxRecentFiles = 10;
    std::vector<std::string> recentFiles; // most recent first
    std::vector<std::string> runHistory;  // Run... dialog, most recent first

    // Docking panels: DockManager::saveState() ("id" -> "area,shown,...").
    std::map<std::string, std::string> docking;
    std::array<std::string, 3> projectWorkspaces; // Project Panel 1-3 workspace files
    bool functionListSorted = false;

    int width = 1100;
    int height = 750;
    bool maximized = false;
    int splitPosition = -1;        // divider between the two views, -1 = half

    static std::filesystem::path defaultPath();
    // Returns false when the file does not exist yet (first run); defaults stay.
    bool load(const std::filesystem::path& file = defaultPath());
    bool save(const std::filesystem::path& file = defaultPath()) const;
};

} // namespace npp::platform
