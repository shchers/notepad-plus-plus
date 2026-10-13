// Notepad++ GTK port — plugin SDK: the small part of <windows.h> that the
// upstream plugin headers (PluginInterface.h, Notepad_plus_msgs.h, Docking.h)
// and a typical plugin's message calls need. It is NOT the Win32 API: there
// are no windows, dialogs or GDI here. A plugin builds its user interface
// with GTK 3 and talks to Notepad++ with SendMessage, as on Windows.
//
// Differences from Windows a plugin author must know (see linux/docs/PLUGINS.md):
//  - wchar_t is 32 bits (UTF-32) on Linux. Strings exchanged with Notepad++
//    are still wchar_t, so L"" literals and the wcs* functions keep working;
//    use %ls (not %s) for wide strings in swprintf formats.
//  - LONG/ULONG are 32 bits as on Windows; LPARAM, WPARAM and LRESULT are
//    pointer-sized.
//  - HWND is an opaque handle. The handles in NppData are understood by
//    SendMessage only; a GTK widget is passed where Notepad++ expects a
//    window of the plugin (DockedWidgetData::hClient). See NppGtk.h.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

// ---- Calling conventions and export -----------------------------------------
#ifndef __cdecl
#define __cdecl
#endif
#ifndef __stdcall
#define __stdcall
#endif
#define WINAPI
#define CALLBACK
#define APIENTRY
// __declspec(dllexport) -> visible from the plugin's shared object.
#define __declspec(x) NPP_DECLSPEC_##x
#define NPP_DECLSPEC_dllexport __attribute__((visibility("default")))
#define NPP_DECLSPEC_dllimport
#define NPP_DECLSPEC_noinline __attribute__((noinline))
#define NPP_DECLSPEC_selectany __attribute__((weak))

// ---- Basic types ---------------------------------------------------------------
typedef int BOOL;
#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE 1
#endif
typedef int INT;
typedef unsigned int UINT;
typedef unsigned char UCHAR;
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned short USHORT;
typedef short SHORT;
typedef int32_t LONG;
typedef uint32_t ULONG;
typedef uint32_t DWORD;
typedef int64_t LONGLONG;
typedef uint64_t ULONGLONG;
typedef intptr_t INT_PTR;
typedef intptr_t LONG_PTR;
typedef uintptr_t UINT_PTR;
typedef uintptr_t ULONG_PTR;
typedef uintptr_t DWORD_PTR;
typedef uintptr_t SIZE_T;
typedef UINT_PTR WPARAM;
typedef LONG_PTR LPARAM;
typedef LONG_PTR LRESULT;
typedef DWORD COLORREF;
typedef void VOID;
typedef void* PVOID;
typedef void* LPVOID;
typedef const void* LPCVOID;

typedef char CHAR;
typedef wchar_t WCHAR;
typedef WCHAR TCHAR;
typedef CHAR* LPSTR;
typedef const CHAR* LPCSTR;
typedef WCHAR* LPWSTR;
typedef const WCHAR* LPCWSTR;
typedef WCHAR* LPTSTR;
typedef const WCHAR* LPCTSTR;
typedef WCHAR* PWSTR;
typedef const WCHAR* PCWSTR;

// Opaque handles: distinct pointer types, as with STRICT on Windows.
#define DECLARE_HANDLE(name) typedef struct name##__* name
DECLARE_HANDLE(HWND);
DECLARE_HANDLE(HINSTANCE);
DECLARE_HANDLE(HICON);
DECLARE_HANDLE(HBITMAP);
DECLARE_HANDLE(HMENU);
DECLARE_HANDLE(HFONT);
DECLARE_HANDLE(HBRUSH);
DECLARE_HANDLE(HDC);
typedef HINSTANCE HMODULE;
typedef void* HANDLE;

typedef struct tagRECT {
    LONG left;
    LONG top;
    LONG right;
    LONG bottom;
} RECT, *PRECT, *LPRECT;

typedef struct tagPOINT {
    LONG x;
    LONG y;
} POINT, *PPOINT, *LPPOINT;

typedef struct tagSIZE {
    LONG cx;
    LONG cy;
} SIZE, *PSIZE, *LPSIZE;

// ---- Macros ---------------------------------------------------------------------
#define MAX_PATH 260
#define WM_USER 0x0400
#define WM_COMMAND 0x0111
#define WM_NOTIFY 0x004E

#define TEXT(s) L##s
#ifndef _T
#define _T(s) L##s
#endif

#define MAKEWORD(a, b) ((WORD)(((BYTE)((DWORD_PTR)(a) & 0xff)) | ((WORD)((BYTE)((DWORD_PTR)(b) & 0xff))) << 8))
#define MAKELONG(a, b) ((LONG)(((WORD)((DWORD_PTR)(a) & 0xffff)) | ((DWORD)((WORD)((DWORD_PTR)(b) & 0xffff))) << 16))
#define LOWORD(l) ((WORD)((DWORD_PTR)(l) & 0xffff))
#define HIWORD(l) ((WORD)((DWORD_PTR)(l) >> 16))
#define LOBYTE(w) ((BYTE)((DWORD_PTR)(w) & 0xff))
#define HIBYTE(w) ((BYTE)((DWORD_PTR)(w) >> 8))
#define MAKEWPARAM(l, h) ((WPARAM)(DWORD)MAKELONG(l, h))
#define MAKELPARAM(l, h) ((LPARAM)(DWORD)MAKELONG(l, h))
#define RGB(r, g, b) ((COLORREF)(((BYTE)(r) | ((WORD)((BYTE)(g)) << 8)) | (((DWORD)(BYTE)(b)) << 16)))
#define GetRValue(rgb) (LOBYTE(rgb))
#define GetGValue(rgb) (LOBYTE(((WORD)(rgb)) >> 8))
#define GetBValue(rgb) (LOBYTE((rgb) >> 16))
#define UNREFERENCED_PARAMETER(p) ((void)(p))

// MessageBox
#define MB_OK 0x00000000L
#define MB_OKCANCEL 0x00000001L
#define MB_ABORTRETRYIGNORE 0x00000002L
#define MB_YESNOCANCEL 0x00000003L
#define MB_YESNO 0x00000004L
#define MB_RETRYCANCEL 0x00000005L
#define MB_ICONHAND 0x00000010L
#define MB_ICONQUESTION 0x00000020L
#define MB_ICONEXCLAMATION 0x00000030L
#define MB_ICONASTERISK 0x00000040L
#define MB_ICONERROR MB_ICONHAND
#define MB_ICONSTOP MB_ICONHAND
#define MB_ICONWARNING MB_ICONEXCLAMATION
#define MB_ICONINFORMATION MB_ICONASTERISK
#define MB_DEFBUTTON1 0x00000000L
#define MB_DEFBUTTON2 0x00000100L
#define MB_DEFBUTTON3 0x00000200L
#define MB_APPLMODAL 0x00000000L
#define MB_TASKMODAL 0x00002000L
#define MB_TOPMOST 0x00040000L
#define IDOK 1
#define IDCANCEL 2
#define IDABORT 3
#define IDRETRY 4
#define IDIGNORE 5
#define IDYES 6
#define IDNO 7

// ---- Functions Notepad++ provides to plugins ----------------------------------------
// Implemented by the Notepad++ executable; a plugin links against them
// without an import library (undefined symbols resolved at load time).
#ifdef __cplusplus
extern "C" {
#endif
__attribute__((visibility("default"))) LRESULT SendMessageW(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
// Queued: runs from the GTK main loop after the current event. FALSE if hWnd is unknown.
__attribute__((visibility("default"))) BOOL PostMessageW(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
// A modal GTK message dialog. Returns IDOK, IDYES ... as on Windows.
__attribute__((visibility("default"))) int MessageBoxW(HWND hWnd, LPCWSTR lpText, LPCWSTR lpCaption, UINT uType);
#ifdef __cplusplus
}
#endif
#define SendMessage SendMessageW
#define PostMessage PostMessageW
#define MessageBox MessageBoxW

// ---- String helpers with Windows names ------------------------------------------
static inline int lstrlenW(LPCWSTR s) { return s ? (int)wcslen(s) : 0; }
static inline LPWSTR lstrcpyW(LPWSTR d, LPCWSTR s) { return wcscpy(d, s); }
static inline LPWSTR lstrcatW(LPWSTR d, LPCWSTR s) { return wcscat(d, s); }
static inline int lstrcmpW(LPCWSTR a, LPCWSTR b) { return wcscmp(a, b); }
static inline int lstrcmpiW(LPCWSTR a, LPCWSTR b) { return wcscasecmp(a, b); }
// Copies at most n - 1 characters and always terminates (n > 0).
static inline LPWSTR lstrcpynW(LPWSTR d, LPCWSTR s, int n)
{
    if (n <= 0)
        return d;
    wcsncpy(d, s, (size_t)n - 1);
    d[n - 1] = L'\0';
    return d;
}
#define lstrlen lstrlenW
#define lstrcpy lstrcpyW
#define lstrcat lstrcatW
#define lstrcmp lstrcmpW
#define lstrcmpi lstrcmpiW
#define lstrcpyn lstrcpynW
#define _wcsicmp wcscasecmp
#define _wcsnicmp wcsncasecmp
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define _wtoi(s) ((int)wcstol((s), NULL, 10))
#define _wtol(s) wcstol((s), NULL, 10)

// The bounds-checked copies upstream code uses (truncate instead of failing
// the process, and return 0 on success as MSVC does).
static inline int wcscpy_s(wchar_t* d, size_t n, const wchar_t* s)
{
    if (!d || n == 0)
        return 22; // EINVAL
    if (!s) {
        d[0] = L'\0';
        return 22;
    }
    const size_t len = wcslen(s);
    if (len >= n) {
        d[0] = L'\0';
        return 34; // ERANGE
    }
    wmemcpy(d, s, len + 1);
    return 0;
}
static inline int wcscat_s(wchar_t* d, size_t n, const wchar_t* s)
{
    if (!d || !s || n == 0)
        return 22;
    const size_t used = wcsnlen(d, n);
    if (used == n)
        return 22;
    return wcscpy_s(d + used, n - used, s);
}
static inline int wcsncpy_s(wchar_t* d, size_t n, const wchar_t* s, size_t count)
{
    if (!d || n == 0)
        return 22;
    if (!s) {
        d[0] = L'\0';
        return 22;
    }
    size_t len = wcsnlen(s, count);
    if (len >= n) {
        d[0] = L'\0';
        return 34;
    }
    wmemcpy(d, s, len);
    d[len] = L'\0';
    return 0;
}
#ifdef __cplusplus
template <size_t N>
inline int wcscpy_s(wchar_t (&d)[N], const wchar_t* s) { return wcscpy_s(d, N, s); }
template <size_t N>
inline int wcscat_s(wchar_t (&d)[N], const wchar_t* s) { return wcscat_s(d, N, s); }
template <size_t N>
inline int wcsncpy_s(wchar_t (&d)[N], const wchar_t* s, size_t count) { return wcsncpy_s(d, N, s, count); }
#endif
#ifndef _TRUNCATE
#define _TRUNCATE ((size_t)-1)
#endif
