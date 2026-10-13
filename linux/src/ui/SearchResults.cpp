#include "ui/SearchResults.h"

#include <SciLexer.h>

#include <algorithm>
#include <cstdio>
#include <map>

namespace npp::ui {

namespace {

// Upstream commafyInt: "12,345".
std::string commafy(long long n)
{
    std::string s = std::to_string(n);
    for (int i = static_cast<int>(s.size()) - 3; i > (n < 0 ? 1 : 0); i -= 3)
        s.insert(static_cast<std::size_t>(i), ",");
    return s;
}

} // namespace

SearchResults::SearchResults(const core::Translator& tr) : tr_(tr)
{
    box_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    g_object_ref_sink(box_);

    view_.showLineNumbers = false;
    view_.execute(SCI_SETMARGINWIDTHN, 0, 0);
    view_.execute(SCI_SETMARGINWIDTHN, 1, 0);
    view_.execute(SCI_SETREADONLY, 1);
    view_.execute(SCI_SETCARETLINEVISIBLE, 1);
    view_.execute(SCI_SETUNDOCOLLECTION, 0);
    view_.execute(SCI_SETEOLMODE, SC_EOL_LF);
    view_.onDoubleClick = [this](sptr_t pos) { openLine(view_.lineFromPos(pos)); };
    gtk_widget_set_size_request(view_.widget(), -1, 80);
    gtk_box_pack_start(GTK_BOX(box_), view_.widget(), TRUE, TRUE, 0);
    gtk_widget_show_all(box_);
}

SearchResults::~SearchResults()
{
    gtk_widget_destroy(box_);
    g_object_unref(box_);
}

void SearchResults::applyStyles(const core::StyleCatalog& styles)
{
    view_.applyLexer("searchResult", "searchResult", styles);
    view_.execute(SCI_SETMARGINWIDTHN, 0, 0);
    view_.execute(SCI_SETMARGINWIDTHN, 1, 0);
    if (const auto* st = styles.forLanguage("searchResult"))
        for (const core::Style& s : *st)
            if (s.id == 6 && s.bg) // "Current line background colour"
                view_.execute(SCI_SETELEMENTCOLOUR, SC_ELEMENT_CARET_LINE_BACK, static_cast<sptr_t>(*s.bg | 0xFF000000u));
    updateMarkings();
}

std::string SearchResults::hitsString(int n) const
{
    // Upstream uses nativeLang strings only with a translation loaded; its
    // built-in English knows singular from plural, english.xml does not.
    const std::string t = english() ? std::string() : tr_.misc("find-result-hits", "");
    if (!t.empty())
        return core::Translator::format(t, "$INT_REPLACE$", std::to_string(n));
    return n == 1 ? "(1 hit)" : "(" + std::to_string(n) + " hits)";
}

void SearchResults::begin(const std::string& what)
{
    pending_ = {};
    std::string one = what;
    one.erase(std::remove(one.begin(), one.end(), '\r'), one.end());
    one.erase(std::remove(one.begin(), one.end(), '\n'), one.end());
    pending_.text = tr_.misc("find-result-title", "Search") + " \"" + one + "\" ";
    // Line 0 of the block: the search header, completed by finish().
    pending_.marks.emplace_back();
    pending_.infos.emplace_back();
}

void SearchResults::addFile(const std::string& file, const std::vector<core::Hit>& hits)
{
    if (hits.empty())
        return;
    // Upstream default: one entry per found line, every hit on it coloured;
    // otherwise (Preferences > Searching) one entry per hit.
    std::vector<std::pair<sptr_t, std::vector<const core::Hit*>>> byLine;
    for (const core::Hit& h : hits) {
        if (oneEntryPerLine && !byLine.empty() && byLine.back().first == h.line)
            byLine.back().second.push_back(&h);
        else
            byLine.push_back({h.line, {&h}});
    }
    pending_.text += "\n  " + file + " " + hitsString(static_cast<int>(hits.size()));
    pending_.marks.emplace_back();
    pending_.infos.push_back(ResultLocation{file, -1, 0, 0});
    for (const auto& [line, list] : byLine) {
        const std::string prefix = "\tLine " + std::to_string(line + 1) + ": ";
        std::string text = list.front()->lineText;
        std::replace(text.begin(), text.end(), '\r', ' ');
        std::replace(text.begin(), text.end(), '\n', ' ');
        SearchResultMarkingLine m;
        for (const core::Hit* h : list) {
            const sptr_t s = std::min<sptr_t>(h->matchStartInLine, static_cast<sptr_t>(text.size()));
            const sptr_t e = std::min<sptr_t>(h->matchEndInLine, static_cast<sptr_t>(text.size()));
            m._segmentPostions.emplace_back(static_cast<intptr_t>(prefix.size()) + s, static_cast<intptr_t>(prefix.size()) + e);
        }
        pending_.text += "\n" + prefix + text;
        pending_.marks.push_back(std::move(m));
        pending_.infos.push_back(ResultLocation{file, line, list.front()->matchStartInLine, list.front()->matchEndInLine});
    }
    pending_.hits += static_cast<int>(hits.size());
    ++pending_.files;
}

void SearchResults::finish(int searched, bool wholeNotSelection, const core::FindOptions& o)
{
    std::string info = english() ? std::string() : tr_.misc(wholeNotSelection ? "find-result-title-info" : "find-result-title-info-selections", "");
    if (info.empty()) {
        std::string what = wholeNotSelection ? "file" : "selection";
        if (pending_.files != 1)
            what += "s";
        info = "(" + commafy(pending_.hits) + (pending_.hits == 1 ? " hit" : " hits") + " in " + commafy(pending_.files) + " " + what +
               " of " + commafy(searched) + " searched)";
    } else {
        info = core::Translator::format(info, "$INT_REPLACE1$", commafy(pending_.hits));
        info = core::Translator::format(info, "$INT_REPLACE2$", commafy(pending_.files));
        info = core::Translator::format(info, "$INT_REPLACE3$", commafy(searched));
    }
    std::string mode;
    switch (o.mode) {
    case core::SearchMode::Extended: mode = tr_.misc("find-result-title-info-options-searchmode-extended", "Extended"); break;
    case core::SearchMode::Regex:
        mode = tr_.misc("find-result-title-info-options-searchmode-regexp", "RegEx");
        if (o.dotMatchesNewline)
            mode += ".";
        break;
    case core::SearchMode::Normal: mode = tr_.misc("find-result-title-info-options-searchmode-normal", "Normal"); break;
    }
    std::string opts;
    if (o.matchCase)
        opts += tr_.misc("find-result-title-info-options-case", "Case");
    if (o.wholeWord)
        opts += (opts.empty() ? "" : "/") + tr_.misc("find-result-title-info-options-word", "Word");
    if (!opts.empty())
        mode += ": ";
    info += " [" + mode + opts + "]";

    // Insert the header's tail, then the block on top of older results.
    const std::size_t nl = pending_.text.find('\n');
    pending_.text.insert(nl == std::string::npos ? pending_.text.size() : nl, info);
    pending_.text += "\n";
    lastHits_ = pending_.hits;

    // Fold the earlier searches, as upstream.
    for (sptr_t l = 0, n = view_.lineCount(); l < n; ++l)
        if (l < static_cast<sptr_t>(infos_.size()) && !infos_[l] && (view_.execute(SCI_GETFOLDLEVEL, static_cast<uptr_t>(l)) & SC_FOLDLEVELHEADERFLAG)
            && (view_.execute(SCI_GETFOLDLEVEL, static_cast<uptr_t>(l)) & SC_FOLDLEVELNUMBERMASK) == SC_FOLDLEVELBASE)
            view_.execute(SCI_FOLDLINE, static_cast<uptr_t>(l), SC_FOLDACTION_CONTRACT);

    marks_.insert(marks_.begin(), pending_.marks.begin(), pending_.marks.end());
    infos_.insert(infos_.begin(), pending_.infos.begin(), pending_.infos.end());
    view_.execute(SCI_SETREADONLY, 0);
    view_.execute(SCI_INSERTTEXT, 0, reinterpret_cast<sptr_t>(pending_.text.c_str()));
    view_.execute(SCI_SETREADONLY, 1);
    pending_ = {};
    updateMarkings();
    view_.execute(SCI_GOTOPOS, 0);
    view_.execute(SCI_FOLDLINE, 0, SC_FOLDACTION_EXPAND);
}

void SearchResults::clear()
{
    view_.execute(SCI_SETREADONLY, 0);
    view_.execute(SCI_CLEARALL);
    view_.execute(SCI_SETREADONLY, 1);
    marks_.clear();
    infos_.clear();
    updateMarkings();
}

void SearchResults::updateMarkings()
{
    // The lexer reads the markings through a pointer in a property, as upstream.
    markStruct_._length = static_cast<intptr_t>(marks_.size());
    markStruct_._markings = marks_.empty() ? nullptr : marks_.data();
    char addr[32];
    std::snprintf(addr, sizeof addr, "%p", static_cast<void*>(&markStruct_));
    view_.execute(SCI_SETPROPERTY, reinterpret_cast<uptr_t>("@MarkingsStruct"), reinterpret_cast<sptr_t>(addr));
    view_.execute(SCI_COLOURISE, 0, -1);
}

void SearchResults::openLine(sptr_t line)
{
    if (line < 0 || line >= static_cast<sptr_t>(infos_.size()))
        return;
    if (!infos_[line]) { // a search header: fold or unfold it
        view_.execute(SCI_TOGGLEFOLD, static_cast<uptr_t>(line));
        return;
    }
    view_.execute(SCI_GOTOLINE, static_cast<uptr_t>(line));
    if (onOpen)
        onOpen(*infos_[line]);
}

bool SearchResults::next(bool forward)
{
    const sptr_t n = static_cast<sptr_t>(infos_.size());
    if (n == 0)
        return false;
    const sptr_t cur = view_.lineFromPos(view_.currentPos());
    for (sptr_t i = 1; i <= n; ++i) {
        const sptr_t l = ((forward ? cur + i : cur - i) % n + n) % n;
        if (infos_[l] && infos_[l]->line >= 0) {
            view_.execute(SCI_ENSUREVISIBLE, static_cast<uptr_t>(l));
            openLine(l);
            return true;
        }
    }
    return false;
}

void SearchResults::focus()
{
    gtk_widget_grab_focus(view_.widget());
}

} // namespace npp::ui
