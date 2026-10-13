// File > Print / Print Now — upstream Printer.cpp with GtkPrintOperation in
// place of the Win32 print dialog and DC. Scintilla lays the pages out
// itself (SCI_FORMATRANGEFULL draws into the print context's cairo_t), as
// upstream does with an HDC. Header and footer come from Preferences >
// Print (three parts each, with upstream's $(...) variables).

#include "ui/MainWindow.h"

#include "menuCmdID.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace fs = std::filesystem;

namespace npp::ui {

namespace {

struct PrintJob {
    EditorView* view = nullptr;
    const platform::Settings* st = nullptr;
    std::string fullPath, fileName, directory;
    sptr_t from = 0, to = 0;   // the selection, or the whole document
    std::vector<sptr_t> pageStarts;
    double headerHeight = 0, footerHeight = 0;
    bool header() const { return !(st->headerLeft.empty() && st->headerMiddle.empty() && st->headerRight.empty()); }
    bool footer() const { return !(st->footerLeft.empty() && st->footerMiddle.empty() && st->footerRight.empty()); }
};

std::string nowFormatted(const char* fmt)
{
    GDateTime* now = g_date_time_new_now_local();
    gchar* s = g_date_time_format(now, fmt);
    std::string out = s ? s : "";
    g_free(s);
    g_date_time_unref(now);
    return out;
}

// Upstream Printer::expandVar.
std::string expandVars(std::string text, const PrintJob& job, int page)
{
    const std::pair<const char*, std::string> vars[] = {
        {"$(FULL_CURRENT_PATH)", job.fullPath}, {"$(FILE_NAME)", job.fileName}, {"$(CURRENT_DIRECTORY)", job.directory},
        {"$(CURRENT_PRINTING_PAGE)", std::to_string(page)}, {"$(SHORT_DATE)", nowFormatted("%x")},
        {"$(LONG_DATE)", nowFormatted("%A, %B %e, %Y")}, {"$(TIME)", nowFormatted("%X")}};
    for (const auto& [name, value] : vars)
        for (std::size_t at = text.find(name); at != std::string::npos; at = text.find(name, at + value.size()))
            text.replace(at, std::strlen(name), value);
    return text;
}

constexpr double kGap = 6.0; // points between header/footer and text

Sci_RangeToFormatFull formatRange(cairo_t* cr, const PrintJob& job, double width, double height, sptr_t from)
{
    Sci_RangeToFormatFull fr{};
    fr.hdc = cr;
    fr.hdcTarget = cr;
    fr.rcPage = {0, 0, static_cast<int>(width), static_cast<int>(height)};
    fr.rc = {0, static_cast<int>(job.headerHeight + kGap), static_cast<int>(width), static_cast<int>(height - job.footerHeight - kGap)};
    fr.chrg.cpMin = from;
    fr.chrg.cpMax = job.to;
    return fr;
}

PangoLayout* textLayout(GtkPrintContext* ctx, const std::string& text, double width, bool bold = false, bool italic = false)
{
    PangoLayout* l = gtk_print_context_create_pango_layout(ctx);
    PangoFontDescription* fd = pango_font_description_from_string("Sans 8");
    if (bold)
        pango_font_description_set_weight(fd, PANGO_WEIGHT_BOLD);
    if (italic)
        pango_font_description_set_style(fd, PANGO_STYLE_ITALIC);
    pango_layout_set_font_description(l, fd);
    pango_font_description_free(fd);
    pango_layout_set_width(l, static_cast<int>(width * PANGO_SCALE));
    pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_MIDDLE);
    pango_layout_set_text(l, text.c_str(), -1);
    return l;
}

void beginPrint(GtkPrintOperation* op, GtkPrintContext* ctx, gpointer p)
{
    auto* job = static_cast<PrintJob*>(p);
    cairo_t* cr = gtk_print_context_get_cairo_context(ctx);
    const double w = gtk_print_context_get_width(ctx), h = gtk_print_context_get_height(ctx);
    int lh = 0;
    PangoLayout* l = textLayout(ctx, "Xg", w);
    pango_layout_get_pixel_size(l, nullptr, &lh);
    g_object_unref(l);
    job->headerHeight = job->header() ? lh : -kGap;
    job->footerHeight = job->footer() ? lh : -kGap;
    // Paginate: Scintilla returns where each page ends.
    job->pageStarts.clear();
    sptr_t pos = job->from;
    while (pos < job->to) {
        job->pageStarts.push_back(pos);
        Sci_RangeToFormatFull fr = formatRange(cr, *job, w, h, pos);
        const sptr_t next = job->view->execute(SCI_FORMATRANGEFULL, 0, reinterpret_cast<sptr_t>(&fr));
        if (next <= pos)
            break;
        pos = next;
    }
    if (job->pageStarts.empty())
        job->pageStarts.push_back(job->from);
    gtk_print_operation_set_n_pages(op, static_cast<int>(job->pageStarts.size()));
}

void drawPage(GtkPrintOperation*, GtkPrintContext* ctx, gint page, gpointer p)
{
    auto* job = static_cast<PrintJob*>(p);
    cairo_t* cr = gtk_print_context_get_cairo_context(ctx);
    const double w = gtk_print_context_get_width(ctx), h = gtk_print_context_get_height(ctx);

    const platform::Settings& st = *job->st;
    cairo_set_source_rgb(cr, 0, 0, 0);
    // Left, middle and right parts, as upstream.
    auto parts = [&](const std::string& l, const std::string& m, const std::string& r, double y, bool bold, bool italic) {
        const std::pair<const std::string*, PangoAlignment> all[] = {{&l, PANGO_ALIGN_LEFT}, {&m, PANGO_ALIGN_CENTER}, {&r, PANGO_ALIGN_RIGHT}};
        for (const auto& [text, align] : all) {
            if (text->empty())
                continue;
            PangoLayout* layout = textLayout(ctx, expandVars(*text, *job, page + 1), w, bold, italic);
            pango_layout_set_alignment(layout, align);
            cairo_move_to(cr, 0, y);
            pango_cairo_show_layout(cr, layout);
            g_object_unref(layout);
        }
    };
    cairo_set_line_width(cr, 0.5);
    if (job->header()) {
        parts(st.headerLeft, st.headerMiddle, st.headerRight, 0, st.headerBold, st.headerItalic);
        cairo_move_to(cr, 0, job->headerHeight + kGap / 2);
        cairo_line_to(cr, w, job->headerHeight + kGap / 2);
    }
    if (job->footer()) {
        parts(st.footerLeft, st.footerMiddle, st.footerRight, h - job->footerHeight, st.footerBold, st.footerItalic);
        cairo_move_to(cr, 0, h - job->footerHeight - kGap / 2);
        cairo_line_to(cr, w, h - job->footerHeight - kGap / 2);
    }
    cairo_stroke(cr);

    Sci_RangeToFormatFull fr = formatRange(cr, *job, w, h, job->pageStarts[static_cast<std::size_t>(page)]);
    job->view->execute(SCI_FORMATRANGEFULL, 1, reinterpret_cast<sptr_t>(&fr));
}

} // namespace

bool MainWindow::print(bool showDialog, const fs::path& pdf, std::string* error, int* pages)
{
    EditorView* v = activeView();
    core::Buffer* b = activeBuffer();
    if (!v || !b)
        return false;
    PrintJob job;
    job.view = v;
    job.st = &settings_;
    job.fullPath = b->fullName();
    job.fileName = b->name();
    job.directory = b->isUntitled() ? std::string() : b->path.parent_path().string();
    const sptr_t a = v->execute(SCI_GETSELECTIONSTART), e = v->execute(SCI_GETSELECTIONEND);
    // Upstream prints the selection when there is one.
    job.from = a != e ? a : 0;
    job.to = a != e ? e : v->length();
    // Preferences > Print: colours, line numbers, margins.
    static const int kColour[] = {SC_PRINT_NORMAL, SC_PRINT_INVERTLIGHT, SC_PRINT_BLACKONWHITE, SC_PRINT_COLOURONWHITE};
    v->execute(SCI_SETPRINTCOLOURMODE, static_cast<uptr_t>(kColour[std::clamp(settings_.printColorMode, 0, 3)]));
    const sptr_t lineNumberWidth = v->execute(SCI_GETMARGINWIDTHN, 0);
    if (!settings_.printLineNumbers)
        v->execute(SCI_SETMARGINWIDTHN, 0, 0);
    v->execute(SCI_SETPRINTMAGNIFICATION, 0);
    v->execute(SCI_SETPRINTWRAPMODE, SC_WRAP_WORD);

    GtkPrintOperation* op = gtk_print_operation_new();
    gtk_print_operation_set_job_name(op, b->name().c_str());
    gtk_print_operation_set_unit(op, GTK_UNIT_POINTS);
    gtk_print_operation_set_use_full_page(op, FALSE);
    gtk_print_operation_set_embed_page_setup(op, TRUE);
    if (printSettings_)
        gtk_print_operation_set_print_settings(op, printSettings_);
    GtkPageSetup* margins = nullptr;
    if (settings_.printMarginLeft || settings_.printMarginTop || settings_.printMarginRight || settings_.printMarginBottom) {
        margins = gtk_page_setup_new();
        gtk_page_setup_set_left_margin(margins, settings_.printMarginLeft, GTK_UNIT_MM);
        gtk_page_setup_set_top_margin(margins, settings_.printMarginTop, GTK_UNIT_MM);
        gtk_page_setup_set_right_margin(margins, settings_.printMarginRight, GTK_UNIT_MM);
        gtk_page_setup_set_bottom_margin(margins, settings_.printMarginBottom, GTK_UNIT_MM);
        gtk_print_operation_set_default_page_setup(op, margins);
    }
    g_signal_connect(op, "begin-print", G_CALLBACK(beginPrint), &job);
    g_signal_connect(op, "draw-page", G_CALLBACK(drawPage), &job);
    GtkPrintOperationAction action = showDialog ? GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG : GTK_PRINT_OPERATION_ACTION_PRINT;
    if (!pdf.empty()) {
        gtk_print_operation_set_export_filename(op, pdf.c_str());
        action = GTK_PRINT_OPERATION_ACTION_EXPORT;
    }
    GError* err = nullptr;
    const GtkPrintOperationResult r = gtk_print_operation_run(op, action, GTK_WINDOW(window_), &err);
    if (r == GTK_PRINT_OPERATION_RESULT_APPLY && pdf.empty()) {
        if (printSettings_)
            g_object_unref(printSettings_);
        printSettings_ = GTK_PRINT_SETTINGS(g_object_ref(gtk_print_operation_get_print_settings(op)));
    }
    const bool ok = r != GTK_PRINT_OPERATION_RESULT_ERROR;
    v->execute(SCI_SETMARGINWIDTHN, 0, lineNumberWidth);
    if (margins)
        g_object_unref(margins);
    if (pages)
        *pages = static_cast<int>(job.pageStarts.size());
    if (!ok) {
        const std::string msg = err ? err->message : "printing failed";
        if (error)
            *error = msg;
        else
            showError("Cannot print \"" + b->name() + "\"", msg);
    }
    if (err)
        g_error_free(err);
    g_object_unref(op);
    return ok;
}

} // namespace npp::ui
