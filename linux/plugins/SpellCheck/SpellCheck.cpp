// SpellCheck — spell checking as you type, in the spirit of DSpellCheck:
// misspelled words in the visible part of each view get a red squiggle (in
// source code only in comments and strings), a right-click on one offers
// suggestions, Ignore and Add to Dictionary, and the Plugins menu moves to
// the next or previous misspelling. Dictionaries come from Enchant
// (Speller.h). Bundled with the GTK port; built only on the plugin SDK and
// GTK.

#include <windows.h>
#include "PluginInterface.h"
#include "NppGtk.h"

#include "SpellText.h"
#include "Speller.h"

#include <gtk/gtk.h>
#include <glib/gstdio.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

NPPGTK_PLUGIN_VERSION("1.0")

namespace {

using namespace spellcheck;

NppData npp;

enum Command { kAutoCheck, kNext, kPrevious, kSuggest, kSeparator, kSettings, kAbout, kCommands };
FuncItem funcItems[kCommands];

struct Settings {
    bool enabled = true;
    bool autoDetect = true;             // all installed languages, detected
    std::vector<std::string> languages; // otherwise these (empty: the user's)
    bool commentsOnly = true;           // in source code
    WordOptions words;
};
Settings settings;
std::string settingsPath, personalPath;

Speller speller;
int indicator = -1; // NPPM_ALLOCATEINDICATOR, at NPPN_READY
guint checkTimer = 0;
// The word being typed is not marked until the caret leaves it.
HWND typingView = nullptr;
sptr_t typingEnd = -1;
std::vector<GtkWidget*> hookedWidgets;

// ---- Strings and messages ----------------------------------------------------

std::string narrow(const wchar_t* s)
{
    gchar* u = g_ucs4_to_utf8(reinterpret_cast<const gunichar*>(s), -1, nullptr, nullptr, nullptr);
    std::string out = u ? u : "";
    g_free(u);
    return out;
}

std::wstring widen(const std::string& s)
{
    gunichar* w = g_utf8_to_ucs4(s.c_str(), -1, nullptr, nullptr, nullptr);
    std::wstring out = w ? reinterpret_cast<const wchar_t*>(w) : L"";
    g_free(w);
    return out;
}

LRESULT nppMsg(UINT msg, WPARAM w = 0, LPARAM l = 0)
{
    return ::SendMessage(npp._nppHandle, msg, w, l);
}

LRESULT sci(HWND h, UINT msg, WPARAM w = 0, LPARAM l = 0)
{
    return ::SendMessage(h, msg, w, l);
}

HWND currentView()
{
    int which = 0;
    nppMsg(NPPM_GETCURRENTSCINTILLA, 0, reinterpret_cast<LPARAM>(&which));
    return which == 1 ? npp._scintillaSecondHandle : npp._scintillaMainHandle;
}

int viewIndex(HWND h)
{
    return h == npp._scintillaSecondHandle ? SUB_VIEW : MAIN_VIEW;
}

bool viewShown(HWND h)
{
    return nppMsg(NPPM_GETCURRENTDOCINDEX, 0, viewIndex(h)) >= 0;
}

// The handle of an editor widget: the view it is the current document of.
HWND handleOf(GtkWidget* w)
{
    for (HWND h : {npp._scintillaMainHandle, npp._scintillaSecondHandle})
        if (reinterpret_cast<GtkWidget*>(nppMsg(NPPM_GTKGETSCINTILLAWIDGET, 0, reinterpret_cast<LPARAM>(h))) == w)
            return h;
    return nullptr;
}

std::string bufferPath(UINT_PTR id)
{
    const LRESULT len = nppMsg(NPPM_GETFULLPATHFROMBUFFERID, id, 0);
    if (len <= 0)
        return {};
    std::wstring w(static_cast<std::size_t>(len) + 1, L'\0');
    nppMsg(NPPM_GETFULLPATHFROMBUFFERID, id, reinterpret_cast<LPARAM>(w.data()));
    return narrow(w.c_str());
}

UINT_PTR bufferOf(HWND h)
{
    const LRESULT pos = nppMsg(NPPM_GETCURRENTDOCINDEX, 0, viewIndex(h));
    return pos < 0 ? 0 : static_cast<UINT_PTR>(nppMsg(NPPM_GETBUFFERIDFROMPOS, static_cast<WPARAM>(pos), viewIndex(h)));
}

bool isMarkdown(const std::string& path)
{
    for (const char* ext : {".md", ".markdown", ".mdown", ".mkd"})
        if (g_str_has_suffix(path.c_str(), ext))
            return true;
    return false;
}

// ---- Settings ------------------------------------------------------------------

void loadSettings()
{
    if (!settingsPath.empty())
        return;
    const LRESULT len = nppMsg(NPPM_GETPLUGINSCONFIGDIR, 0, 0);
    std::wstring dir(static_cast<std::size_t>(len > 0 ? len : 0) + 1, L'\0');
    if (len <= 0 || !nppMsg(NPPM_GETPLUGINSCONFIGDIR, dir.size(), reinterpret_cast<LPARAM>(dir.data())))
        dir.assign(1, L'\0');
    std::string configDir = narrow(dir.c_str());
    if (configDir.empty())
        configDir = std::string(g_get_user_config_dir()) + "/notepad-plus-plus-gtk/plugins/config";
    g_mkdir_with_parents(configDir.c_str(), 0700);
    settingsPath = configDir + "/SpellCheck.ini";
    personalPath = configDir + "/SpellCheck.dic";

    GKeyFile* kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, settingsPath.c_str(), G_KEY_FILE_NONE, nullptr)) {
        auto flag = [&](const char* key, bool& value) {
            GError* e = nullptr;
            const gboolean v = g_key_file_get_boolean(kf, "SpellCheck", key, &e);
            if (e)
                g_error_free(e);
            else
                value = v;
        };
        flag("Enabled", settings.enabled);
        flag("AutoDetect", settings.autoDetect);
        flag("CommentsOnly", settings.commentsOnly);
        flag("IgnoreDigits", settings.words.ignoreDigits);
        flag("IgnoreAllCaps", settings.words.ignoreAllCaps);
        flag("IgnoreMixedCase", settings.words.ignoreMixedCase);
        gsize n = 0;
        if (gchar** langs = g_key_file_get_string_list(kf, "SpellCheck", "Languages", &n, nullptr)) {
            for (gsize i = 0; i < n; ++i)
                if (*langs[i])
                    settings.languages.emplace_back(langs[i]);
            g_strfreev(langs);
        }
    }
    g_key_file_free(kf);
}

void saveSettings()
{
    GKeyFile* kf = g_key_file_new();
    g_key_file_set_boolean(kf, "SpellCheck", "Enabled", settings.enabled);
    g_key_file_set_boolean(kf, "SpellCheck", "AutoDetect", settings.autoDetect);
    g_key_file_set_boolean(kf, "SpellCheck", "CommentsOnly", settings.commentsOnly);
    g_key_file_set_boolean(kf, "SpellCheck", "IgnoreDigits", settings.words.ignoreDigits);
    g_key_file_set_boolean(kf, "SpellCheck", "IgnoreAllCaps", settings.words.ignoreAllCaps);
    g_key_file_set_boolean(kf, "SpellCheck", "IgnoreMixedCase", settings.words.ignoreMixedCase);
    std::vector<const gchar*> langs;
    for (const std::string& l : settings.languages)
        langs.push_back(l.c_str());
    g_key_file_set_string_list(kf, "SpellCheck", "Languages", langs.data(), langs.size());
    GError* e = nullptr;
    if (!g_key_file_save_to_file(kf, settingsPath.c_str(), &e)) {
        g_warning("SpellCheck: cannot save %s: %s", settingsPath.c_str(), e->message);
        g_error_free(e);
    }
    g_key_file_free(kf);
}

// The dictionaries of the settings; false when Enchant is missing.
bool startSpeller()
{
    static bool started = false;
    if (!speller.available())
        return false;
    if (!started) {
        started = true;
        speller.setPersonalDictionary(personalPath);
    }
    std::vector<std::string> langs = settings.autoDetect ? speller.allLanguages() : settings.languages;
    if (langs.empty()) {
        const std::string def = speller.defaultLanguage();
        if (!def.empty())
            langs.push_back(def);
    }
    speller.setLanguages(langs);
    return true;
}

// ---- Checking ------------------------------------------------------------------

std::array<bool, 256> stylesToCheck(HWND h)
{
    StyleQuery q;
    const LRESULT lexLen = sci(h, SCI_GETLEXERLANGUAGE);
    if (lexLen > 0) {
        std::string lexer(static_cast<std::size_t>(lexLen) + 1, '\0');
        sci(h, SCI_GETLEXERLANGUAGE, 0, reinterpret_cast<LPARAM>(lexer.data()));
        q.lexer = lexer.c_str();
    }
    const LRESULT named = std::min<LRESULT>(sci(h, SCI_GETNAMEDSTYLES), 256);
    for (LRESULT s = 0; s < named; ++s) {
        const LRESULT len = sci(h, SCI_TAGSOFSTYLE, static_cast<WPARAM>(s));
        std::string tags(static_cast<std::size_t>(std::max<LRESULT>(len, 0)) + 1, '\0');
        if (len > 0)
            sci(h, SCI_TAGSOFSTYLE, static_cast<WPARAM>(s), reinterpret_cast<LPARAM>(tags.data()));
        q.tags.emplace_back(tags.c_str());
    }
    const UINT_PTR buffer = bufferOf(h);
    const std::string path = bufferPath(buffer);
    q.prose = !settings.commentsOnly || nppMsg(NPPM_GETBUFFERLANGTYPE, buffer) == L_TEXT || isProseFile(path);
    q.markdown = isMarkdown(path);
    return checkableStyles(q);
}

// The misspelled words between two line starts (or ends), lexed first.
std::vector<Word> misspellings(HWND h, sptr_t start, sptr_t end, const std::array<bool, 256>& styles, bool skipTyping)
{
    std::vector<Word> bad;
    if (end <= start)
        return bad;
    sci(h, SCI_COLOURISE, static_cast<WPARAM>(start), end);
    std::string styled(static_cast<std::size_t>(end - start) * 2 + 2, '\0');
    Sci_TextRangeFull tr{{start, end}, styled.data()};
    sci(h, SCI_GETSTYLEDTEXTFULL, 0, reinterpret_cast<LPARAM>(&tr));
    const std::size_t n = static_cast<std::size_t>(end - start);
    std::string text(n, '\0'), mask(n, '\0');
    for (std::size_t i = 0; i < n; ++i) {
        text[i] = styled[2 * i];
        mask[i] = styles[static_cast<unsigned char>(styled[2 * i + 1])] ? 1 : 0;
    }
    std::vector<Word> words = findWords(text, mask, settings.words);
    const sptr_t caret = skipTyping && h == typingView ? sci(h, SCI_GETCURRENTPOS) : -1;
    if (caret >= 0 && caret == typingEnd)
        words.erase(std::remove_if(words.begin(), words.end(), [&](const Word& w) { return start + static_cast<sptr_t>(w.end) == caret; }),
                    words.end());
    for (std::size_t i : speller.misspelled(text, words)) {
        Word& w = words[i];
        w.start += static_cast<std::size_t>(start);
        w.end += static_cast<std::size_t>(start);
        bad.push_back(std::move(w));
    }
    return bad;
}

void setupIndicator(HWND h)
{
    sci(h, SCI_INDICSETSTYLE, static_cast<WPARAM>(indicator), INDIC_SQUIGGLEPIXMAP);
    sci(h, SCI_INDICSETFORE, static_cast<WPARAM>(indicator), 0x2828E0); // BGR: red
    sci(h, SCI_INDICSETUNDER, static_cast<WPARAM>(indicator), TRUE);
}

void fillIndicator(HWND h, sptr_t start, sptr_t end, const std::vector<Word>& bad)
{
    const LRESULT previous = sci(h, SCI_GETINDICATORCURRENT);
    sci(h, SCI_SETINDICATORCURRENT, static_cast<WPARAM>(indicator));
    sci(h, SCI_INDICATORCLEARRANGE, static_cast<WPARAM>(start), end - start);
    for (const Word& w : bad)
        sci(h, SCI_INDICATORFILLRANGE, w.start, static_cast<LPARAM>(w.end - w.start));
    sci(h, SCI_SETINDICATORCURRENT, static_cast<WPARAM>(previous));
}

// The lines on screen: what is checked as you type.
std::pair<sptr_t, sptr_t> visibleRange(HWND h)
{
    const sptr_t firstVisible = sci(h, SCI_GETFIRSTVISIBLELINE);
    const sptr_t first = sci(h, SCI_DOCLINEFROMVISIBLE, static_cast<WPARAM>(firstVisible));
    const sptr_t last = sci(h, SCI_DOCLINEFROMVISIBLE, static_cast<WPARAM>(firstVisible + sci(h, SCI_LINESONSCREEN) + 1));
    const sptr_t start = sci(h, SCI_POSITIONFROMLINE, static_cast<WPARAM>(first));
    // A huge line is checked up to a point.
    return {start, std::min(sci(h, SCI_GETLINEENDPOSITION, static_cast<WPARAM>(last)), start + (1 << 20))};
}

void checkView(HWND h)
{
    if (indicator < 0)
        return;
    setupIndicator(h);
    const sptr_t length = sci(h, SCI_GETLENGTH);
    if (!settings.enabled || speller.languages().empty()) {
        fillIndicator(h, 0, length, {});
        return;
    }
    const auto [start, end] = visibleRange(h);
    fillIndicator(h, start, end, misspellings(h, start, end, stylesToCheck(h), true));
}

void hookWidgets();

void checkNow()
{
    if (checkTimer) {
        g_source_remove(checkTimer);
        checkTimer = 0;
    }
    hookWidgets();
    for (HWND h : {npp._scintillaMainHandle, npp._scintillaSecondHandle})
        if (viewShown(h))
            checkView(h);
}

void scheduleCheck(guint delay = 200)
{
    if (indicator < 0)
        return;
    if (checkTimer)
        g_source_remove(checkTimer);
    checkTimer = g_timeout_add(delay, +[](gpointer) -> gboolean {
        checkTimer = 0;
        checkNow();
        return G_SOURCE_REMOVE;
    }, nullptr);
}

// The next (or previous) misspelling from the selection, round the end of
// the document; selects it.
bool gotoMisspelling(HWND h, bool forward)
{
    const std::array<bool, 256> styles = stylesToCheck(h);
    const sptr_t length = sci(h, SCI_GETLENGTH);
    constexpr sptr_t kBlock = 500; // lines at a time
    auto line = [&](sptr_t pos) { return sci(h, SCI_LINEFROMPOSITION, static_cast<WPARAM>(pos)); };
    auto lineStart = [&](sptr_t l) { return sci(h, SCI_POSITIONFROMLINE, static_cast<WPARAM>(l)); };
    auto lineEnd = [&](sptr_t l) { return sci(h, SCI_GETLINEENDPOSITION, static_cast<WPARAM>(l)); };
    // The first (last) misspelling within [a, b].
    auto scan = [&](sptr_t a, sptr_t b, Word& found) {
        const sptr_t first = line(a), last = line(b);
        auto inside = [&](const Word& w) { return static_cast<sptr_t>(w.start) >= a && static_cast<sptr_t>(w.end) <= b; };
        if (forward) {
            for (sptr_t l = first; l <= last; l += kBlock)
                for (Word& w : misspellings(h, lineStart(l), lineEnd(std::min(l + kBlock - 1, last)), styles, false))
                    if (inside(w)) {
                        found = std::move(w);
                        return true;
                    }
        } else {
            for (sptr_t l = last; l >= first; l -= kBlock) {
                std::vector<Word> bad = misspellings(h, lineStart(std::max(l - kBlock + 1, first)), lineEnd(l), styles, false);
                for (auto it = bad.rbegin(); it != bad.rend(); ++it)
                    if (inside(*it)) {
                        found = std::move(*it);
                        return true;
                    }
            }
        }
        return false;
    };
    Word w;
    const sptr_t selStart = sci(h, SCI_GETSELECTIONSTART), selEnd = sci(h, SCI_GETSELECTIONEND);
    const bool found = forward ? scan(selEnd, length, w) || scan(0, selEnd, w) : scan(0, selStart, w) || scan(selStart, length, w);
    if (!found)
        return false;
    sci(h, SCI_ENSUREVISIBLEENFORCEPOLICY, static_cast<WPARAM>(line(static_cast<sptr_t>(w.start))));
    sci(h, SCI_SETSEL, w.start, static_cast<LPARAM>(w.end));
    sci(h, SCI_SCROLLCARET);
    return true;
}

// ---- The suggestions menu ------------------------------------------------------

struct Target {
    HWND view = nullptr;
    sptr_t start = 0, end = 0;
    std::string raw;  // the document's bytes there
    std::string word; // as checked
    std::string lang; // misspelled in this language; empty: correct
};

std::string textRange(HWND h, sptr_t start, sptr_t end)
{
    std::string s(static_cast<std::size_t>(std::max<sptr_t>(end - start, 0)) + 1, '\0');
    Sci_TextRangeFull tr{{start, end}, s.data()};
    sci(h, SCI_GETTEXTRANGEFULL, 0, reinterpret_cast<LPARAM>(&tr));
    s.pop_back();
    return s;
}

// The word at pos as findWords sees it (no checking, all styles).
bool wordAt(HWND h, sptr_t pos, Target& t)
{
    const sptr_t line = sci(h, SCI_LINEFROMPOSITION, static_cast<WPARAM>(pos));
    const sptr_t start = sci(h, SCI_POSITIONFROMLINE, static_cast<WPARAM>(line));
    const sptr_t end = std::min(sci(h, SCI_GETLINEENDPOSITION, static_cast<WPARAM>(line)), start + (1 << 20));
    const std::string text = textRange(h, start, end);
    WordOptions all;
    all.ignoreDigits = all.ignoreAllCaps = all.ignoreMixedCase = false;
    for (const Word& w : findWords(text, {}, all)) {
        if (pos >= start + static_cast<sptr_t>(w.start) && pos <= start + static_cast<sptr_t>(w.end)) {
            t.view = h;
            t.start = start + static_cast<sptr_t>(w.start);
            t.end = start + static_cast<sptr_t>(w.end);
            t.raw = text.substr(w.start, w.end - w.start);
            t.word = w.text;
            return true;
        }
    }
    return false;
}

// Whether the word t is misspelled, as checking as you type tells (the same
// lines, so the same languages detected), and in which language.
bool misspelledWord(Target& t)
{
    const HWND h = t.view;
    auto [start, end] = visibleRange(h);
    if (t.start < start || t.end > end) {
        // Off screen: its paragraph.
        sptr_t first = sci(h, SCI_LINEFROMPOSITION, static_cast<WPARAM>(t.start)), last = first;
        const sptr_t lines = sci(h, SCI_GETLINECOUNT);
        auto blank = [&](sptr_t l) { return sci(h, SCI_GETLINEENDPOSITION, static_cast<WPARAM>(l)) == sci(h, SCI_GETLINEINDENTPOSITION, static_cast<WPARAM>(l)); };
        while (first > 0 && t.start - sci(h, SCI_POSITIONFROMLINE, static_cast<WPARAM>(first)) < 20000 && !blank(first - 1))
            --first;
        while (last + 1 < lines && sci(h, SCI_POSITIONFROMLINE, static_cast<WPARAM>(last + 1)) - t.end < 20000 && !blank(last + 1))
            ++last;
        start = sci(h, SCI_POSITIONFROMLINE, static_cast<WPARAM>(first));
        end = sci(h, SCI_GETLINEENDPOSITION, static_cast<WPARAM>(last));
    }
    for (const Word& w : misspellings(h, start, end, stylesToCheck(h), false))
        if (static_cast<sptr_t>(w.start) == t.start && static_cast<sptr_t>(w.end) == t.end) {
            t.lang = w.lang;
            return true;
        }
    // Not in a checked style (Suggestions on code): the word by itself.
    return !speller.correct(t.word);
}

Target* targetOf(GtkWidget* item)
{
    return static_cast<Target*>(g_object_get_data(G_OBJECT(gtk_widget_get_parent(item)), "spellcheck-target"));
}

void replaceWord(GtkMenuItem* item, gpointer)
{
    const Target* t = targetOf(GTK_WIDGET(item));
    const char* with = static_cast<const char*>(g_object_get_data(G_OBJECT(item), "spellcheck-word"));
    if (!t || !with || textRange(t->view, t->start, t->end) != t->raw)
        return; // the text changed meanwhile
    sci(t->view, SCI_SETTARGETRANGE, static_cast<WPARAM>(t->start), t->end);
    sci(t->view, SCI_REPLACETARGET, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(with));
    const sptr_t caret = t->start + static_cast<sptr_t>(std::strlen(with));
    sci(t->view, SCI_SETSEL, static_cast<WPARAM>(caret), caret);
}

void ignoreWord(GtkMenuItem* item, gpointer)
{
    if (const Target* t = targetOf(GTK_WIDGET(item))) {
        speller.ignore(t->word);
        checkNow();
    }
}

void addWord(GtkMenuItem* item, gpointer)
{
    if (const Target* t = targetOf(GTK_WIDGET(item))) {
        speller.add(t->word);
        checkNow();
    }
}

GtkWidget* addItem(GtkWidget* menu, const std::string& label, GCallback fn, bool sensitive = true)
{
    GtkWidget* it = gtk_menu_item_new_with_label(label.c_str());
    gtk_widget_set_sensitive(it, sensitive);
    if (fn)
        g_signal_connect(it, "activate", fn, nullptr);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
    return it;
}

void addSeparator(GtkWidget* menu)
{
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
}

// Scintilla's own context menu, which the suggestions replace.
void addEditItems(GtkWidget* menu, HWND h)
{
    struct Edit {
        const char* label;
        UINT msg;
        bool sensitive;
    };
    const bool selection = !sci(h, SCI_GETSELECTIONEMPTY);
    const bool writable = !sci(h, SCI_GETREADONLY);
    const Edit edits[] = {
        {"Undo", SCI_UNDO, writable && sci(h, SCI_CANUNDO) != 0},
        {"Redo", SCI_REDO, writable && sci(h, SCI_CANREDO) != 0},
        {nullptr, 0, false},
        {"Cut", SCI_CUT, writable && selection},
        {"Copy", SCI_COPY, selection},
        {"Paste", SCI_PASTE, writable && sci(h, SCI_CANPASTE) != 0},
        {"Delete", SCI_CLEAR, writable && selection},
        {nullptr, 0, false},
        {"Select All", SCI_SELECTALL, true},
    };
    addSeparator(menu);
    for (const Edit& e : edits) {
        if (!e.label) {
            addSeparator(menu);
            continue;
        }
        GtkWidget* it = addItem(menu, e.label, G_CALLBACK(+[](GtkMenuItem* item, gpointer) {
                                    if (const Target* t = targetOf(GTK_WIDGET(item)))
                                        sci(t->view, GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(item), "spellcheck-msg")));
                                }), e.sensitive);
        g_object_set_data(G_OBJECT(it), "spellcheck-msg", GUINT_TO_POINTER(e.msg));
    }
}

GtkWidget* suggestionsMenu(const Target& target, bool misspelled, bool editItems)
{
    GtkWidget* menu = gtk_menu_new();
    g_object_set_data_full(G_OBJECT(menu), "spellcheck-target", new Target(target), +[](gpointer p) { delete static_cast<Target*>(p); });
    if (misspelled) {
        const std::vector<std::string> suggestions = speller.suggest(target.word, target.lang);
        for (const std::string& s : suggestions) {
            GtkWidget* it = addItem(menu, s, G_CALLBACK(replaceWord));
            g_object_set_data_full(G_OBJECT(it), "spellcheck-word", g_strdup(s.c_str()), g_free);
            if (GtkWidget* label = gtk_bin_get_child(GTK_BIN(it))) {
                gchar* bold = g_markup_printf_escaped("<b>%s</b>", s.c_str());
                gtk_label_set_markup(GTK_LABEL(label), bold);
                g_free(bold);
            }
        }
        if (suggestions.empty())
            addItem(menu, "(No Suggestions)", nullptr, false);
        if (!target.lang.empty() && speller.languages().size() > 1)
            addItem(menu, "Language: " + target.lang, nullptr, false);
        addSeparator(menu);
        addItem(menu, "Ignore “" + target.word + "”", G_CALLBACK(ignoreWord));
        addItem(menu, "Add “" + target.word + "” to Dictionary", G_CALLBACK(addWord));
    } else {
        addItem(menu, "“" + target.word + "” is spelled correctly", nullptr, false);
    }
    if (editItems)
        addEditItems(menu, target.view);
    gtk_widget_show_all(menu);
    g_signal_connect(menu, "deactivate", G_CALLBACK(+[](GtkMenuShell* sh, gpointer) {
                         g_idle_add(+[](gpointer w) -> gboolean { gtk_widget_destroy(GTK_WIDGET(w)); return G_SOURCE_REMOVE; }, sh);
                     }), nullptr);
    return menu;
}

bool misspelledAt(HWND h, sptr_t pos)
{
    return indicator >= 0 && sci(h, SCI_INDICATORVALUEAT, static_cast<WPARAM>(indicator), pos) != 0;
}

gboolean onButtonPress(GtkWidget* w, GdkEventButton* ev, gpointer)
{
    if (ev->type != GDK_BUTTON_PRESS || ev->button != 3 || !settings.enabled || indicator < 0)
        return FALSE;
    const HWND h = handleOf(w);
    if (!h)
        return FALSE;
    const sptr_t pos = sci(h, SCI_CHARPOSITIONFROMPOINTCLOSE, static_cast<WPARAM>(ev->x), static_cast<LPARAM>(ev->y));
    if (pos < 0 || !misspelledAt(h, pos))
        return FALSE;
    Target t;
    if (!wordAt(h, pos, t))
        return FALSE;
    misspelledWord(t);
    GtkWidget* menu = suggestionsMenu(t, true, true);
    gtk_menu_attach_to_widget(GTK_MENU(menu), w, nullptr);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), reinterpret_cast<GdkEvent*>(ev));
    return TRUE; // not Scintilla's menu
}

// The data of the plugin's handlers on the editors, to find them again.
int hookTag;

void widgetGone(gpointer, GObject* gone)
{
    hookedWidgets.erase(std::remove(hookedWidgets.begin(), hookedWidgets.end(), reinterpret_cast<GtkWidget*>(gone)), hookedWidgets.end());
}

void hookWidgets()
{
    for (HWND h : {npp._scintillaMainHandle, npp._scintillaSecondHandle}) {
        auto* w = reinterpret_cast<GtkWidget*>(nppMsg(NPPM_GTKGETSCINTILLAWIDGET, 0, reinterpret_cast<LPARAM>(h)));
        if (!w || std::find(hookedWidgets.begin(), hookedWidgets.end(), w) != hookedWidgets.end())
            continue;
        g_signal_connect(w, "button-press-event", G_CALLBACK(onButtonPress), &hookTag);
        // More lines on screen.
        g_signal_connect_after(w, "size-allocate", G_CALLBACK(+[](GtkWidget*, GdkRectangle*, gpointer) { scheduleCheck(); }), &hookTag);
        g_object_weak_ref(G_OBJECT(w), widgetGone, nullptr);
        hookedWidgets.push_back(w);
    }
}

void unhookWidgets()
{
    for (GtkWidget* w : hookedWidgets) {
        g_signal_handlers_disconnect_by_data(w, &hookTag);
        g_object_weak_unref(G_OBJECT(w), widgetGone, nullptr);
    }
    hookedWidgets.clear();
}

// ---- Commands ------------------------------------------------------------------

bool ensureDictionaries()
{
    if (!speller.available()) {
        ::MessageBox(npp._nppHandle,
                     widen(speller.error() + "\n\nInstall Enchant and a dictionary for your language, e.g.:\n"
                                             "sudo apt install libenchant-2-2 hunspell-en-us")
                         .c_str(),
                     L"Spell Checker", MB_OK | MB_ICONWARNING);
        return false;
    }
    if (speller.languages().empty()) {
        ::MessageBox(npp._nppHandle,
                     L"No dictionary is installed for the chosen languages.\n\n"
                     L"Install one (e.g. hunspell-en-us, hunspell-de-de) and choose it in Spell Checker Settings.",
                     L"Spell Checker", MB_OK | MB_ICONWARNING);
        return false;
    }
    return true;
}

void toggleAutoCheck()
{
    settings.enabled = !settings.enabled;
    nppMsg(NPPM_SETMENUITEMCHECK, static_cast<WPARAM>(funcItems[kAutoCheck]._cmdID), settings.enabled);
    saveSettings();
    if (settings.enabled)
        ensureDictionaries();
    checkNow();
}

void gotoNext(bool forward)
{
    if (!ensureDictionaries())
        return;
    if (!gotoMisspelling(currentView(), forward))
        ::MessageBox(npp._nppHandle, L"No misspelled words found.", L"Spell Checker", MB_OK | MB_ICONINFORMATION);
}

void nextMisspelling()
{
    gotoNext(true);
}

void previousMisspelling()
{
    gotoNext(false);
}

// The suggestions for the word at the caret, below it.
void suggestAtCaret()
{
    if (!ensureDictionaries())
        return;
    const HWND h = currentView();
    const sptr_t caret = sci(h, SCI_GETCURRENTPOS);
    Target t;
    if (!wordAt(h, caret, t)) {
        ::MessageBox(npp._nppHandle, L"The caret is not on a word.", L"Spell Checker", MB_OK | MB_ICONINFORMATION);
        return;
    }
    auto* w = reinterpret_cast<GtkWidget*>(nppMsg(NPPM_GTKGETSCINTILLAWIDGET, 0, reinterpret_cast<LPARAM>(h)));
    if (!w || !gtk_widget_get_window(w))
        return;
    const bool misspelled = misspelledWord(t);
    GtkWidget* menu = suggestionsMenu(t, misspelled, false);
    gtk_menu_attach_to_widget(GTK_MENU(menu), w, nullptr);
    const int x = static_cast<int>(sci(h, SCI_POINTXFROMPOSITION, 0, t.start));
    const int y = static_cast<int>(sci(h, SCI_POINTYFROMPOSITION, 0, t.start));
    const int height = static_cast<int>(sci(h, SCI_TEXTHEIGHT, static_cast<WPARAM>(sci(h, SCI_LINEFROMPOSITION, static_cast<WPARAM>(t.start)))));
    const GdkRectangle rect{x, y, 1, height};
    gtk_menu_popup_at_rect(GTK_MENU(menu), gtk_widget_get_window(w), &rect, GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST, nullptr);
}

void showSettings()
{
    auto* parent = reinterpret_cast<GtkWindow*>(nppMsg(NPPM_GTKGETMAINWINDOW));
    GtkWidget* dlg = gtk_dialog_new_with_buttons("Spell Checker Settings", parent, static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
                                                 "_Cancel", GTK_RESPONSE_CANCEL, "_OK", GTK_RESPONSE_OK, nullptr);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dlg))), box, TRUE, TRUE, 0);

    GtkWidget* title = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(title), "<b>Languages</b>");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);

    std::vector<std::pair<std::string, GtkWidget*>> langChecks;
    GtkWidget* autoDetect = nullptr;
    const std::vector<std::string> dicts = speller.available() ? speller.dictionaries() : std::vector<std::string>();
    if (dicts.empty()) {
        GtkWidget* none = gtk_label_new(speller.available()
                                            ? "No dictionary is installed. Install one for each language you write in,\n"
                                              "e.g. hunspell-en-us, hunspell-de-de or aspell-fr, then open this again."
                                            : (speller.error() + "\nInstall Enchant (libenchant-2-2) and a dictionary, e.g. hunspell-en-us.").c_str());
        gtk_label_set_xalign(GTK_LABEL(none), 0);
        gtk_box_pack_start(GTK_BOX(box), none, FALSE, FALSE, 0);
    } else {
        autoDetect = gtk_radio_button_new_with_mnemonic(nullptr, "Detect the language _automatically, among all installed");
        GtkWidget* chosen = gtk_radio_button_new_with_mnemonic_from_widget(GTK_RADIO_BUTTON(autoDetect), "Only _these languages:");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(settings.autoDetect ? autoDetect : chosen), TRUE);
        gtk_box_pack_start(GTK_BOX(box), autoDetect, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), chosen, FALSE, FALSE, 0);
        GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
        gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), std::min<int>(static_cast<int>(dicts.size()), 8) * 26 + 4);
        GtkWidget* list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_container_set_border_width(GTK_CONTAINER(list), 4);
        // In automatic mode the list keeps the user's own choice.
        std::vector<std::string> active = settings.autoDetect ? settings.languages : speller.languages();
        if (active.empty())
            active.push_back(speller.defaultLanguage());
        for (const std::string& d : dicts) {
            GtkWidget* c = gtk_check_button_new_with_label(d.c_str());
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c), std::find(active.begin(), active.end(), d) != active.end());
            gtk_box_pack_start(GTK_BOX(list), c, FALSE, FALSE, 0);
            langChecks.emplace_back(d, c);
        }
        gtk_container_add(GTK_CONTAINER(scroll), list);
        gtk_widget_set_margin_start(scroll, 24);
        gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);
        gtk_widget_set_sensitive(scroll, !settings.autoDetect);
        g_signal_connect(chosen, "toggled", G_CALLBACK(+[](GtkToggleButton* b, gpointer list) {
                             gtk_widget_set_sensitive(GTK_WIDGET(list), gtk_toggle_button_get_active(b));
                         }), scroll);
        GtkWidget* hint = gtk_label_new("Each paragraph is checked in the language that knows most of its words,\n"
                                        "and words in another alphabet in a language written in it.\n"
                                        "More languages: install their dictionaries, e.g.\n"
                                        "sudo apt install hunspell-fr hunspell-es hunspell-uk");
        gtk_label_set_xalign(GTK_LABEL(hint), 0);
        gtk_label_set_selectable(GTK_LABEL(hint), TRUE);
        gtk_style_context_add_class(gtk_widget_get_style_context(hint), "dim-label");
        gtk_box_pack_start(GTK_BOX(box), hint, FALSE, FALSE, 0);
    }

    GtkWidget* title2 = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(title2), "<b>Words</b>");
    gtk_widget_set_halign(title2, GTK_ALIGN_START);
    gtk_widget_set_margin_top(title2, 8);
    gtk_box_pack_start(GTK_BOX(box), title2, FALSE, FALSE, 0);
    auto option = [&](const char* label, bool value) {
        GtkWidget* c = gtk_check_button_new_with_mnemonic(label);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c), value);
        gtk_box_pack_start(GTK_BOX(box), c, FALSE, FALSE, 0);
        return c;
    };
    GtkWidget* commentsOnly = option("In source code, check only _comments and strings", settings.commentsOnly);
    GtkWidget* digits = option("Ignore words with _digits (utf8, r2d2)", settings.words.ignoreDigits);
    GtkWidget* caps = option("Ignore words in _CAPITALS", settings.words.ignoreAllCaps);
    GtkWidget* mixed = option("Ignore words with capitals _inside (camelCase)", settings.words.ignoreMixedCase);

    GtkWidget* personal = gtk_button_new_with_mnemonic("Edit _Personal Dictionary");
    gtk_widget_set_tooltip_text(personal, personalPath.c_str());
    gtk_widget_set_halign(personal, GTK_ALIGN_START);
    gtk_widget_set_margin_top(personal, 8);
    gtk_widget_set_sensitive(personal, speller.available());
    g_signal_connect(personal, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) { gtk_dialog_response(GTK_DIALOG(d), 1); }), dlg);
    gtk_box_pack_start(GTK_BOX(box), personal, FALSE, FALSE, 0);

    gtk_widget_show_all(dlg);
    const int response = gtk_dialog_run(GTK_DIALOG(dlg));
    if (response == GTK_RESPONSE_OK) {
        if (autoDetect)
            settings.autoDetect = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(autoDetect));
        if (!langChecks.empty()) {
            settings.languages.clear();
            for (const auto& [tag, check] : langChecks)
                if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(check)))
                    settings.languages.push_back(tag);
        }
        settings.commentsOnly = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(commentsOnly));
        settings.words.ignoreDigits = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(digits));
        settings.words.ignoreAllCaps = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(caps));
        settings.words.ignoreMixedCase = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(mixed));
        saveSettings();
        startSpeller();
        checkNow();
    }
    gtk_widget_destroy(dlg);
    if (response == 1) {
        // One word per line; saving it here reloads it (NPPN_FILESAVED).
        speller.setPersonalDictionary(personalPath);
        nppMsg(NPPM_DOOPEN, 0, reinterpret_cast<LPARAM>(widen(personalPath).c_str()));
    }
}

void about()
{
    ::MessageBox(npp._nppHandle,
                 L"Spell Checker 1.0\n\n"
                 L"Checks spelling as you type, in the spirit of DSpellCheck: misspelled words are underlined, "
                 L"in source code only in comments and strings. Right-click one for suggestions, or use "
                 L"Plugins > Spell Checker > Suggestions (give it a shortcut in the Shortcut Mapper).\n\n"
                 L"Dictionaries come from Enchant: install hunspell-xx (or aspell-xx) for your languages. "
                 L"Each paragraph is checked in the installed language that knows most of its words "
                 L"(or only in the languages chosen in the settings). Words you add are kept in SpellCheck.dic in the plugins' "
                 L"configuration folder.",
                 L"Spell Checker", MB_OK | MB_ICONINFORMATION);
}

void setCommand(int i, const wchar_t* name, PFUNCPLUGINCMD fn, bool checked = false)
{
    wcscpy_s(funcItems[i]._itemName, name);
    funcItems[i]._pFunc = fn;
    funcItems[i]._init2Check = checked;
    funcItems[i]._pShKey = nullptr;
}

bool isWordInsert(const SCNotification* n)
{
    if (!n->text || n->length <= 0 || n->length > 4)
        return false;
    const gunichar c = g_utf8_get_char_validated(n->text, n->length);
    return c != static_cast<gunichar>(-1) && c != static_cast<gunichar>(-2) && (g_unichar_isalnum(c) || c == '\'');
}

} // namespace

extern "C" __declspec(dllexport) void setInfo(NppData data)
{
    npp = data;
}

extern "C" __declspec(dllexport) const wchar_t* getName()
{
    return L"Spell Checker";
}

extern "C" __declspec(dllexport) FuncItem* getFuncsArray(int* count)
{
    loadSettings();
    setCommand(kAutoCheck, L"&Check Spelling as You Type", toggleAutoCheck, settings.enabled);
    setCommand(kNext, L"&Next Misspelling", nextMisspelling);
    setCommand(kPrevious, L"&Previous Misspelling", previousMisspelling);
    setCommand(kSuggest, L"&Suggestions…", suggestAtCaret);
    setCommand(kSeparator, L"", nullptr);
    setCommand(kSettings, L"Se&ttings…", showSettings);
    setCommand(kAbout, L"&About", about);
    *count = kCommands;
    return funcItems;
}

extern "C" __declspec(dllexport) void beNotified(SCNotification* n)
{
    switch (n->nmhdr.code) {
    case NPPN_READY: {
        loadSettings();
        int first = -1;
        if (nppMsg(NPPM_ALLOCATEINDICATOR, 1, reinterpret_cast<LPARAM>(&first)) && first >= 0)
            indicator = first;
        else
            indicator = 19; // what DSpellCheck used before allocation existed
        startSpeller();
        scheduleCheck(0);
        break;
    }
    case NPPN_BUFFERACTIVATED:
    case NPPN_LANGCHANGED:
    case NPPN_WORDSTYLESUPDATED:
        typingView = nullptr;
        scheduleCheck(50);
        break;
    case NPPN_FILESAVED:
        // The personal dictionary, edited in Notepad++.
        if (!personalPath.empty() && bufferPath(n->nmhdr.idFrom) == personalPath) {
            speller.reloadPersonalDictionary();
            scheduleCheck();
        }
        break;
    case SCN_MODIFIED:
        if (n->modificationType & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT)) {
            const auto h = static_cast<HWND>(n->nmhdr.hwndFrom);
            if ((n->modificationType & SC_PERFORMED_USER) && (n->modificationType & SC_MOD_INSERTTEXT) && isWordInsert(n)) {
                typingView = h;
                typingEnd = n->position + n->length;
            } else if ((n->modificationType & SC_PERFORMED_USER) && (n->modificationType & SC_MOD_DELETETEXT) && typingView == h &&
                       n->position + n->length == typingEnd) {
                typingEnd = n->position; // Backspace in the word being typed
            } else {
                typingView = nullptr;
            }
            scheduleCheck();
        }
        break;
    case SCN_UPDATEUI:
        if (n->updated & SC_UPDATE_V_SCROLL)
            scheduleCheck();
        if ((n->updated & SC_UPDATE_SELECTION) && typingView == static_cast<HWND>(n->nmhdr.hwndFrom) &&
            sci(typingView, SCI_GETCURRENTPOS) != typingEnd) {
            typingView = nullptr; // left the word: mark it now
            scheduleCheck(0);
        }
        break;
    case NPPN_SHUTDOWN:
        if (checkTimer) {
            g_source_remove(checkTimer);
            checkTimer = 0;
        }
        unhookWidgets();
        indicator = -1;
        break;
    default:
        break;
    }
}

extern "C" __declspec(dllexport) LRESULT messageProc(UINT, WPARAM, LPARAM)
{
    return TRUE;
}

extern "C" __declspec(dllexport) BOOL isUnicode()
{
    return TRUE;
}
