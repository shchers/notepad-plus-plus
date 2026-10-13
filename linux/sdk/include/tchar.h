// Notepad++ GTK port — plugin SDK: <tchar.h> for a Unicode build, which is
// the only kind Notepad++ supports. TCHAR is wchar_t (see windows.h).
#pragma once

#include "windows.h"

#ifndef _T
#define _T(s) L##s
#endif
#define _TEXT(s) L##s
#define _tcslen wcslen
#define _tcscpy wcscpy
#define _tcscpy_s wcscpy_s
#define _tcsncpy wcsncpy
#define _tcsncpy_s wcsncpy_s
#define _tcscat wcscat
#define _tcscat_s wcscat_s
#define _tcscmp wcscmp
#define _tcsicmp wcscasecmp
#define _tcsnicmp wcsncasecmp
#define _tcschr wcschr
#define _tcsrchr wcsrchr
#define _tcsstr wcsstr
#define _tcstol wcstol
#define _ttoi _wtoi
#define _stprintf swprintf
#define _sntprintf swprintf
