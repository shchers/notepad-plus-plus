#include "ui/ShortcutDialog.h"

#include "ui/KeyMap.h"

namespace npp::ui {

namespace {

struct Ctx {
    ShortcutEdit* edit;
    GtkWidget* ctrl;
    GtkWidget* alt;
    GtkWidget* shift;
    GtkWidget* keys;
    GtkWidget* info;
    GtkWidget* ok;
    GtkWidget* name;

    core::KeyCombo combo() const
    {
        const int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(keys));
        const int vk = idx <= 0 ? 0 : core::assignableKeys()[static_cast<std::size_t>(idx - 1)];
        return {gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ctrl)) != 0, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(alt)) != 0,
                gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(shift)) != 0, vk};
    }
    void set(const core::KeyCombo& k)
    {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ctrl), k.ctrl);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(alt), k.alt);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(shift), k.shift);
        const auto& all = core::assignableKeys();
        int idx = 0;
        for (std::size_t i = 0; i < all.size(); ++i)
            if (all[i] == k.vk)
                idx = static_cast<int>(i) + 1;
        gtk_combo_box_set_active(GTK_COMBO_BOX(keys), idx);
    }
    void update()
    {
        const core::KeyCombo k = combo();
        // Upstream: a key needs Ctrl or Alt unless it is a function key and the like.
        std::string msg = edit->conflictInfo && !k.empty() ? edit->conflictInfo(k) : std::string();
        gtk_label_set_text(GTK_LABEL(info), msg.c_str());
        const bool nameOk = !edit->nameEditable || gtk_entry_get_text_length(GTK_ENTRY(name)) > 0;
        gtk_widget_set_sensitive(ok, nameOk);
    }
};

} // namespace

bool runShortcutDialog(GtkWindow* parent, const core::Translator& tr, ShortcutEdit& edit)
{
    const char* sec = "ShortcutMapperSubDialg";
    GtkWidget* dlg = gtk_dialog_new_with_buttons(tr.dialogAttr(sec, "title", "Shortcut").c_str(), parent, GTK_DIALOG_MODAL,
                                                 core::Translator::toMnemonic(tr.dialog(sec, 2, "Cancel")).c_str(), GTK_RESPONSE_CANCEL,
                                                 core::Translator::toMnemonic(tr.dialog(sec, 1, "OK")).c_str(), GTK_RESPONSE_OK, nullptr);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
    GtkWidget* box = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_box_pack_start(GTK_BOX(box), grid, TRUE, TRUE, 0);

    Ctx c{&edit, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    GtkWidget* nl = gtk_label_new(core::Translator::plain(tr.dialog(sec, 5006, "Name")).c_str());
    gtk_label_set_xalign(GTK_LABEL(nl), 1.0f);
    c.name = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(c.name), edit.name.c_str());
    gtk_entry_set_activates_default(GTK_ENTRY(c.name), TRUE);
    gtk_widget_set_sensitive(c.name, edit.nameEditable);
    gtk_widget_set_hexpand(c.name, TRUE);
    gtk_grid_attach(GTK_GRID(grid), nl, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), c.name, 1, 0, 4, 1);

    c.ctrl = gtk_check_button_new_with_label("CTRL");
    c.alt = gtk_check_button_new_with_label("ALT");
    c.shift = gtk_check_button_new_with_label("SHIFT");
    c.keys = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(c.keys), "None");
    for (int vk : core::assignableKeys())
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(c.keys), core::vkName(vk).c_str());
    gtk_grid_attach(GTK_GRID(grid), c.ctrl, 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), c.alt, 2, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), c.shift, 3, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("+"), 4, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), c.keys, 5, 1, 1, 1);
    c.info = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(c.info), 0);
    gtk_label_set_line_wrap(GTK_LABEL(c.info), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(c.info), 50);
    gtk_grid_attach(GTK_GRID(grid), c.info, 0, 2, 6, 1);
    GtkWidget* hint = gtk_label_new("Tip: press the key combination to fill these fields.");
    gtk_style_context_add_class(gtk_widget_get_style_context(hint), "dim-label");
    gtk_label_set_xalign(GTK_LABEL(hint), 0);
    gtk_grid_attach(GTK_GRID(grid), hint, 0, 3, 6, 1);
    c.ok = gtk_dialog_get_widget_for_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);

    c.set(edit.key);
    for (GtkWidget* w : {c.ctrl, c.alt, c.shift})
        g_signal_connect_swapped(w, "toggled", G_CALLBACK(+[](Ctx* x) { x->update(); }), &c);
    g_signal_connect_swapped(c.keys, "changed", G_CALLBACK(+[](Ctx* x) { x->update(); }), &c);
    g_signal_connect_swapped(c.name, "changed", G_CALLBACK(+[](Ctx* x) { x->update(); }), &c);
    // A key combination pressed anywhere but the name field fills the fields.
    g_signal_connect(dlg, "key-press-event", G_CALLBACK(+[](GtkWidget* w, GdkEventKey* ev, gpointer p) -> gboolean {
                         auto* x = static_cast<Ctx*>(p);
                         if (gtk_window_get_focus(GTK_WINDOW(w)) == x->name)
                             return FALSE;
                         const core::KeyCombo k = comboFromEvent(ev);
                         if (k.empty() || ((ev->keyval == GDK_KEY_Return || ev->keyval == GDK_KEY_Escape || ev->keyval == GDK_KEY_Tab) && !k.ctrl && !k.alt))
                             return FALSE;
                         x->set(k);
                         return TRUE;
                     }), &c);
    c.update();
    gtk_widget_show_all(box);

    const bool ok = gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK;
    if (ok) {
        edit.name = gtk_entry_get_text(GTK_ENTRY(c.name));
        edit.key = c.combo();
    }
    gtk_widget_destroy(dlg);
    return ok;
}

} // namespace npp::ui
