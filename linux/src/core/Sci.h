// Calls into one Scintilla instance without GTK — the same direct-function
// seam as EditorView::execute() / upstream ScintillaEditView::execute(), so
// core algorithms (search, line operations, macros) run on any Scintilla:
// a visible editor or the hidden one used for Find in Files.
#pragma once

#include <Scintilla.h>

#include <string>

namespace npp::core {

struct Sci {
    SciFnDirect fn = nullptr;
    sptr_t ptr = 0;

    explicit operator bool() const { return fn != nullptr; }
    sptr_t operator()(unsigned int msg, uptr_t w = 0, sptr_t l = 0) const { return fn(ptr, msg, w, l); }

    sptr_t length() const { return (*this)(SCI_GETLENGTH); }
    sptr_t lineCount() const { return (*this)(SCI_GETLINECOUNT); }
    sptr_t lineOf(sptr_t pos) const { return (*this)(SCI_LINEFROMPOSITION, static_cast<uptr_t>(pos)); }
    sptr_t lineStart(sptr_t line) const { return (*this)(SCI_POSITIONFROMLINE, static_cast<uptr_t>(line)); }
    sptr_t lineEnd(sptr_t line) const { return (*this)(SCI_GETLINEENDPOSITION, static_cast<uptr_t>(line)); }

    std::string range(sptr_t from, sptr_t to) const
    {
        if (to <= from)
            return {};
        const char* p = reinterpret_cast<const char*>((*this)(SCI_GETRANGEPOINTER, static_cast<uptr_t>(from), to - from));
        return p ? std::string(p, static_cast<std::size_t>(to - from)) : std::string();
    }
    std::string text() const { return range(0, length()); }
    std::string lineText(sptr_t line) const { return range(lineStart(line), lineEnd(line)); }
    void setTarget(sptr_t from, sptr_t to) const { (*this)(SCI_SETTARGETRANGE, static_cast<uptr_t>(from), to); }
    sptr_t replaceTarget(const std::string& s) const
    {
        return (*this)(SCI_REPLACETARGET, s.size(), reinterpret_cast<sptr_t>(s.data()));
    }
};

} // namespace npp::core
