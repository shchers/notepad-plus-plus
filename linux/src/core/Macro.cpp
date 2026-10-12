#include "core/Macro.h"

#include "CommandTables.generated.h"
#include "FindReplaceDlg_rc.h"

#include <algorithm>

namespace npp::core {

MacroStep MacroStep::fromScintilla(int message, uptr_t wParam, sptr_t lParam)
{
    MacroStep s{UseLParam, message, static_cast<std::int64_t>(wParam), static_cast<std::int64_t>(lParam), {}};
    if (!lParam)
        return s;
    switch (message) {
    case SCI_SETTEXT:
    case SCI_REPLACESEL:
    case SCI_REPLACETARGET:
    case SCI_REPLACETARGETRE:
    case SCI_SEARCHINTARGET:
    case SCI_ADDTEXT:
    case SCI_ADDSTYLEDTEXT:
    case SCI_INSERTTEXT:
    case SCI_APPENDTEXT:
    case SCI_SETWORDCHARS:
    case SCI_SETWHITESPACECHARS:
    case SCI_SETSTYLINGEX:
    case SCI_TEXTWIDTH:
    case SCI_STYLESETFONT:
    case SCI_SEARCHNEXT:
    case SCI_SEARCHPREV:
        // SCI_ADDTEXT and friends pass a length in wParam; the others are 0-terminated.
        if (message == SCI_ADDTEXT || message == SCI_APPENDTEXT || message == SCI_ADDSTYLEDTEXT)
            s.sParam.assign(reinterpret_cast<const char*>(lParam), static_cast<std::size_t>(wParam));
        else
            s.sParam = reinterpret_cast<const char*>(lParam);
        s.type = UseSParam;
        s.lParam = 0;
        break;
    default:
        break;
    }
    return s;
}

bool MacroStep::isPlayableScintilla() const
{
    switch (message) {
    case SCI_REPLACESEL:
    case SCI_ADDTEXT:
    case SCI_INSERTTEXT:
    case SCI_APPENDTEXT:
    case SCI_SEARCHNEXT:
    case SCI_SEARCHPREV:
        return type == UseSParam;

    case SCI_GOTOLINE: case SCI_GOTOPOS: case SCI_SETSELECTIONMODE: case SCI_CUT: case SCI_COPY: case SCI_PASTE:
    case SCI_CLEAR: case SCI_CLEARALL: case SCI_SELECTALL: case SCI_SEARCHANCHOR: case SCI_LINEDOWN:
    case SCI_LINEDOWNEXTEND: case SCI_PARADOWN: case SCI_PARADOWNEXTEND: case SCI_LINEUP: case SCI_LINEUPEXTEND:
    case SCI_PARAUP: case SCI_PARAUPEXTEND: case SCI_CHARLEFT: case SCI_CHARLEFTEXTEND: case SCI_CHARRIGHT:
    case SCI_CHARRIGHTEXTEND: case SCI_WORDLEFT: case SCI_WORDLEFTEXTEND: case SCI_WORDRIGHT:
    case SCI_WORDRIGHTEXTEND: case SCI_WORDPARTLEFT: case SCI_WORDPARTLEFTEXTEND: case SCI_WORDPARTRIGHT:
    case SCI_WORDPARTRIGHTEXTEND: case SCI_WORDLEFTEND: case SCI_WORDLEFTENDEXTEND: case SCI_WORDRIGHTEND:
    case SCI_WORDRIGHTENDEXTEND: case SCI_HOME: case SCI_HOMEEXTEND: case SCI_LINEEND: case SCI_LINEENDEXTEND:
    case SCI_HOMEWRAP: case SCI_HOMEWRAPEXTEND: case SCI_LINEENDWRAP: case SCI_LINEENDWRAPEXTEND:
    case SCI_DOCUMENTSTART: case SCI_DOCUMENTSTARTEXTEND: case SCI_DOCUMENTEND: case SCI_DOCUMENTENDEXTEND:
    case SCI_STUTTEREDPAGEUP: case SCI_STUTTEREDPAGEUPEXTEND: case SCI_STUTTEREDPAGEDOWN:
    case SCI_STUTTEREDPAGEDOWNEXTEND: case SCI_PAGEUP: case SCI_PAGEUPEXTEND: case SCI_PAGEDOWN:
    case SCI_PAGEDOWNEXTEND: case SCI_EDITTOGGLEOVERTYPE: case SCI_CANCEL: case SCI_DELETEBACK: case SCI_TAB:
    case SCI_BACKTAB: case SCI_FORMFEED: case SCI_VCHOME: case SCI_VCHOMEEXTEND: case SCI_VCHOMEWRAP:
    case SCI_VCHOMEWRAPEXTEND: case SCI_VCHOMEDISPLAY: case SCI_VCHOMEDISPLAYEXTEND: case SCI_DELWORDLEFT:
    case SCI_DELWORDRIGHT: case SCI_DELWORDRIGHTEND: case SCI_DELLINELEFT: case SCI_DELLINERIGHT:
    case SCI_LINECOPY: case SCI_LINECUT: case SCI_LINEDELETE: case SCI_LINETRANSPOSE: case SCI_LINEDUPLICATE:
    case SCI_LOWERCASE: case SCI_UPPERCASE: case SCI_LINESCROLLDOWN: case SCI_LINESCROLLUP:
    case SCI_DELETEBACKNOTLINE: case SCI_HOMEDISPLAY: case SCI_HOMEDISPLAYEXTEND: case SCI_LINEENDDISPLAY:
    case SCI_LINEENDDISPLAYEXTEND: case SCI_LINEDOWNRECTEXTEND: case SCI_LINEUPRECTEXTEND:
    case SCI_CHARLEFTRECTEXTEND: case SCI_CHARRIGHTRECTEXTEND: case SCI_HOMERECTEXTEND: case SCI_VCHOMERECTEXTEND:
    case SCI_LINEENDRECTEXTEND: case SCI_PAGEUPRECTEXTEND: case SCI_PAGEDOWNRECTEXTEND: case SCI_SELECTIONDUPLICATE:
    case SCI_COPYALLOWLINE: case SCI_VERTICALCENTRECARET: case SCI_MOVESELECTEDLINESUP:
    case SCI_MOVESELECTEDLINESDOWN: case SCI_SCROLLTOSTART: case SCI_SCROLLTOEND: case SCI_SETVIRTUALSPACEOPTIONS:
    case SCI_SETCARETLINEBACKALPHA: case SCI_NEWLINE:
        return type == UseLParam;
    default:
        return false;
    }
}

bool isRecordableCommand(int id)
{
    return std::find(std::begin(gen::kMacroRecordable), std::end(gen::kMacroRecordable), id) != std::end(gen::kMacroRecordable);
}

Macro normaliseNewlines(const Macro& in)
{
    Macro out;
    auto prevIsCR = [&] { return !out.empty() && out.back().message == SCI_REPLACESEL && out.back().sParam == "\r"; };
    const MacroStep newline{MacroStep::UseLParam, SCI_NEWLINE, 0, 0, {}};
    for (const MacroStep& s : in) {
        const bool isCR = s.sParam == "\r";
        if (s.message == SCI_REPLACESEL && (isCR || s.sParam == "\r\n" || s.sParam == "\n")) {
            if (prevIsCR()) {
                if (isCR)
                    out.back() = newline;
                else
                    out.pop_back(); // CR then LF: one newline
            }
            out.push_back(isCR ? s : newline); // a lone CR is resolved by the next step
        } else {
            if (prevIsCR())
                out.back() = newline;
            out.push_back(s);
        }
    }
    if (prevIsCR())
        out.back() = newline;
    return out;
}

} // namespace npp::core
