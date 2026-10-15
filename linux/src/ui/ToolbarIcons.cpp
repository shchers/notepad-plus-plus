#include "ui/ToolbarIcons.h"

#include "menuCmdID.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace npp::ui {

const ToolbarButton kToolbarButtons[] = {
    {IDM_FILE_NEW, "new", "newFile", "document-new", "New"},
    {IDM_FILE_OPEN, "open", "openFile", "document-open", "Open"},
    {IDM_FILE_SAVE, "save", "saveFile", "document-save", "Save"},
    {IDM_FILE_SAVEALL, "saveall", "saveAll", "document-save-as", "Save All"},
    {IDM_FILE_CLOSE, "close", "closeFile", "window-close", "Close"},
    {IDM_FILE_CLOSEALL, "closeall", "closeAll", "window-close", "Close All"},
    {IDM_FILE_PRINT, "print", "print", "document-print", "Print"},
    {0, nullptr, nullptr, nullptr, nullptr},
    {IDM_EDIT_CUT, "cut", "cut", "edit-cut", "Cut"},
    {IDM_EDIT_COPY, "copy", "copy", "edit-copy", "Copy"},
    {IDM_EDIT_PASTE, "paste", "paste", "edit-paste", "Paste"},
    {0, nullptr, nullptr, nullptr, nullptr},
    {IDM_EDIT_UNDO, "undo", "undo", "edit-undo", "Undo"},
    {IDM_EDIT_REDO, "redo", "redo", "edit-redo", "Redo"},
    {0, nullptr, nullptr, nullptr, nullptr},
    {IDM_SEARCH_FIND, "find", "find", "edit-find", "Find"},
    {IDM_SEARCH_REPLACE, "findrep", "findReplace", "edit-find-replace", "Replace"},
    {0, nullptr, nullptr, nullptr, nullptr},
    {IDM_VIEW_ZOOMIN, "zoomIn", "zoomIn", "zoom-in", "Zoom in"},
    {IDM_VIEW_ZOOMOUT, "zoomOut", "zoomOut", "zoom-out", "Zoom out"},
    {0, nullptr, nullptr, nullptr, nullptr},
    {IDM_VIEW_SYNSCROLLV, "syncV", "syncV", "object-flip-vertical", "Synchronize Vertical Scrolling"},
    {IDM_VIEW_SYNSCROLLH, "syncH", "syncH", "object-flip-horizontal", "Synchronize Horizontal Scrolling"},
    {0, nullptr, nullptr, nullptr, nullptr},
    {IDM_VIEW_WRAP, "wrap", "wrap", "format-justify-left", "Word wrap"},
    {IDM_VIEW_ALL_CHARACTERS, "allChars", "allChars", "format-text-direction-ltr", "Show All Characters"},
    {IDM_VIEW_INDENT_GUIDE, "indentGuide", "indentGuide", "format-indent-more", "Show Indent Guide"},
    {0, nullptr, nullptr, nullptr, nullptr},
    {IDM_LANG_USER_DLG, "udl", "udl", "preferences-desktop-font", "Define your language..."},
    {IDM_VIEW_DOC_MAP, "docMap", "docMap", "view-paged", "Document Map"},
    {IDM_VIEW_DOCLIST, "docList", "docList", "view-list", "Document List"},
    {IDM_VIEW_FUNC_LIST, "funcList", "funcList", "view-list-tree", "Function List"},
    {IDM_VIEW_FILEBROWSER, "fileBrowser", "fileBrowser", "folder", "Folder as Workspace"},
    {0, nullptr, nullptr, nullptr, nullptr},
    {IDM_VIEW_MONITORING, "monitoring", "monitoring", "view-refresh", "Monitoring (tail -f)"},
    {0, nullptr, nullptr, nullptr, nullptr},
    {IDM_MACRO_STARTRECORDINGMACRO, "startrecord", "startRecord", "media-record", "Start Recording"},
    {IDM_MACRO_STOPRECORDINGMACRO, "stoprecord", "stopRecord", "media-playback-stop", "Stop Recording"},
    {IDM_MACRO_PLAYBACKRECORDEDMACRO, "playrecord", "playRecord", "media-playback-start", "Playback"},
    {IDM_MACRO_RUNMULTIMACRODLG, "playrecord_m", "playRecord_m", "media-seek-forward", "Run a Macro Multiple Times..."},
    {IDM_MACRO_SAVECURRENTMACRO, "saverecord", "saveRecord", "document-save", "Save Current Recorded Macro..."},
};
const int kToolbarButtonCount = static_cast<int>(std::size(kToolbarButtons));

fs::path toolbarIconFile(const fs::path& iconsDir, const ToolbarButton& b, int set, bool dark, bool disabled)
{
    if (set == 4) // Standard icons: one look, no disabled or dark variants
        return disabled ? fs::path() : iconsDir / "standard" / "toolbar" / (std::string(b.bmp) + ".bmp");
    const fs::path dir = iconsDir / (dark ? "dark" : "light") / "toolbar" / (set >= 2 ? "filled" : "regular");
    if (!disabled)
        return dir / (std::string(b.stem) + "_off.ico");
    std::error_code ec;
    const fs::path dis = dir / (std::string(b.stem) + "_dis.ico");
    return fs::is_regular_file(dis, ec) ? dis : fs::path();
}

namespace {

std::uint16_t u16(const unsigned char* p) { return static_cast<std::uint16_t>(p[0] | p[1] << 8); }
std::uint32_t u32(const unsigned char* p) { return u16(p) | static_cast<std::uint32_t>(u16(p + 2)) << 16; }

GdkPixbuf* fromBytes(const unsigned char* data, std::size_t size)
{
    GdkPixbufLoader* loader = gdk_pixbuf_loader_new();
    GdkPixbuf* pb = nullptr;
    if (gdk_pixbuf_loader_write(loader, data, size, nullptr) && gdk_pixbuf_loader_close(loader, nullptr)) {
        pb = gdk_pixbuf_loader_get_pixbuf(loader);
        if (pb)
            g_object_ref(pb);
    } else {
        gdk_pixbuf_loader_close(loader, nullptr);
    }
    g_object_unref(loader);
    return pb;
}

GdkPixbuf* sized(GdkPixbuf* pb, int px, GdkInterpType interp)
{
    if (!pb || (gdk_pixbuf_get_width(pb) == px && gdk_pixbuf_get_height(pb) == px))
        return pb;
    GdkPixbuf* s = gdk_pixbuf_scale_simple(pb, px, px, interp);
    g_object_unref(pb);
    return s;
}

// The .ico's entry nearest `px` (exact, else the smallest larger, else the
// largest), on its own: GdkPixbuf's ICO loader would pick one by itself.
GdkPixbuf* loadIco(const std::vector<unsigned char>& d, int px)
{
    if (d.size() < 6 || u16(d.data() + 2) != 1)
        return nullptr;
    const int n = u16(d.data() + 4);
    if (d.size() < 6 + 16u * static_cast<std::size_t>(n))
        return nullptr;
    int best = -1, bestSize = 0;
    for (int i = 0; i < n; ++i) {
        const unsigned char* e = d.data() + 6 + 16 * i;
        const int size = e[0] ? e[0] : 256;
        const bool better = best < 0 || (bestSize < px ? size > bestSize : size >= px && size < bestSize);
        if (better) {
            best = i;
            bestSize = size;
        }
    }
    if (best < 0)
        return nullptr;
    const unsigned char* e = d.data() + 6 + 16 * best;
    const std::uint32_t len = u32(e + 8), off = u32(e + 12);
    if (off > d.size() || len > d.size() - off)
        return nullptr;
    // A one-entry .ico around that image.
    std::vector<unsigned char> one(d.begin(), d.begin() + 6);
    one[4] = 1;
    one[5] = 0;
    one.insert(one.end(), e, e + 12);
    const std::uint32_t newOff = 6 + 16;
    for (int i = 0; i < 4; ++i)
        one.push_back(static_cast<unsigned char>(newOff >> (8 * i)));
    one.insert(one.end(), d.begin() + off, d.begin() + off + len);
    return sized(fromBytes(one.data(), one.size()), px, GDK_INTERP_HYPER);
}

GdkPixbuf* loadBmp(const std::vector<unsigned char>& d, int px)
{
    GdkPixbuf* pb = fromBytes(d.data(), d.size());
    if (!pb)
        return nullptr;
    GdkPixbuf* a = gdk_pixbuf_add_alpha(pb, TRUE, 192, 192, 192);
    g_object_unref(pb);
    return sized(a, px, GDK_INTERP_NEAREST); // pixel art: no blur
}

} // namespace

GdkPixbuf* loadToolbarIcon(const fs::path& file, int px)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return nullptr;
    const std::vector<unsigned char> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (d.size() >= 2 && d[0] == 'B' && d[1] == 'M')
        return loadBmp(d, px);
    return loadIco(d, px);
}

} // namespace npp::ui
