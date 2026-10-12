// Stand-in for <windows.h>, used ONLY when compiling
// lexilla/lexers/LexUser.cxx (Notepad++ User Defined Language lexer).
//
// That file uses no Win32 API at all; it relies on <windows.h> merely to
// drag in <string.h>, plus the MSVC CRT function _itoa. Providing those
// here lets the upstream source build unchanged.
//
// Do not add this directory to any other target's include path: a general
// Win32 shim is deliberately NOT the porting strategy (see docs/PORTING.md).
#pragma once

#include <cstdio>
#include <cstring>

// MSVC CRT _itoa; LexUser only uses radix 10 on small style numbers.
static inline char* _itoa(int value, char* buffer, int radix)
{
    if (radix == 10)
        std::sprintf(buffer, "%d", value);
    else if (radix == 16)
        std::sprintf(buffer, "%x", static_cast<unsigned>(value));
    else
        buffer[0] = '\0';
    return buffer;
}
