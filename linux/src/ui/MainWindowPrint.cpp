// File > Print / Print Now — upstream Printer.cpp with GtkPrintOperation in
// place of the Win32 print dialog and DC. Scintilla lays the pages out
// itself (SCI_FORMATRANGEFULL draws into the print context's cairo_t), as
// upstream does with an HDC. A header (file name) and footer (page number)
// are printed as with upstream's default print settings.

#include "ui/MainWindow.h"

#include "menuCmdID.h"

#include <vector>

namespace fs = std::filesystem;

namespace npp::ui {

namespace {

struct PrintJob {
    EditorView* view = nullptr;
    std::string title;
    sptr_t from = 0, to = 0;   // the selection, or the whole document
    std::vector<sptr_t> pageStarts;
    double headerHeight = 0, footerHeight = 0;
};

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

PangoLayout* textLayout(GtkPrintContext* ctx, const std::string& text, double width)
{
    PangoLayout* l = gtk_print_context_create_pango_layout(ctx);
    PangoFontDescription* fd = pango_font_description_from_string("Sans 8");
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
    job->headerHeight = job->footerHeight = lh;
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

    PangoLayout* head = textLayout(ctx, job->title, w);
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_move_to(cr, 0, 0);
    pango_cairo_show_layout(cr, head);
    g_object_unref(head);
    PangoLayout* foot = textLayout(ctx, std::to_string(page + 1) + " / " + std::to_string(job->pageStarts.size()), w);
    pango_layout_set_alignment(foot, PANGO_ALIGN_CENTER);
    cairo_move_to(cr, 0, h - job->footerHeight);
    pango_cairo_show_layout(cr, foot);
    g_object_unref(foot);
    cairo_set_line_width(cr, 0.5);
    cairo_move_to(cr, 0, job->headerHeight + kGap / 2);
    cairo_line_to(cr, w, job->headerHeight + kGap / 2);
    cairo_move_to(cr, 0, h - job->footerHeight - kGap / 2);
    cairo_line_to(cr, w, h - job->footerHeight - kGap / 2);
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
    job.title = b->fullName();
    const sptr_t a = v->execute(SCI_GETSELECTIONSTART), e = v->execute(SCI_GETSELECTIONEND);
    // Upstream prints the selection when there is one.
    job.from = a != e ? a : 0;
    job.to = a != e ? e : v->length();
    v->execute(SCI_SETPRINTCOLOURMODE, SC_PRINT_COLOURONWHITE);
    v->execute(SCI_SETPRINTMAGNIFICATION, 0);
    v->execute(SCI_SETPRINTWRAPMODE, SC_WRAP_WORD);

    GtkPrintOperation* op = gtk_print_operation_new();
    gtk_print_operation_set_job_name(op, b->name().c_str());
    gtk_print_operation_set_unit(op, GTK_UNIT_POINTS);
    gtk_print_operation_set_use_full_page(op, FALSE);
    gtk_print_operation_set_embed_page_setup(op, TRUE);
    if (printSettings_)
        gtk_print_operation_set_print_settings(op, printSettings_);
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
