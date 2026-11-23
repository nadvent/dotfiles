#include "picker_ui.h"

#include <gtk4-layer-shell.h>
#include <stdlib.h>
#include <string.h>

/* Rosé Pine Dawn palette matching themes/scratchpad.ron */
#define RP_BASE      "#faf4ed"
#define RP_SURFACE   "#fffaf3"
#define RP_OVERLAY   "#f2e9e1"
#define RP_BORDER    "#286983"
#define RP_TEXT      "#575279"
#define RP_SUBTLE    "#797593"
#define RP_MUTED     "#9893a5"
#define RP_LOVE      "#b4637a"
#define RP_GOLD      "#ea9d34"
#define RP_PINE      "#286983"

/* Sidebar anchor matching ~/.config/umbriel/cfg/rules.toml (600x1150 at x=8, y=8) */
#define SIDEBAR_LEFT_OFFSET 8
#define SIDEBAR_WIDTH       600
#define PL_WINDOW_WIDTH     500
#define PL_WINDOW_HEIGHT    440
#define SR_WINDOW_WIDTH     560
#define SR_WINDOW_HEIGHT    680

static const char *LAYER_CSS =
    "window.rmpc-layer-window {\n"
    "    background: transparent;\n"
    "}\n"
    "button {\n"
    "    background-image: none;\n"
    "    box-shadow: none;\n"
    "    text-shadow: none;\n"
    "}\n"
    ".picker-card {\n"
    "    background-color: rgba(250, 244, 237, 0.98);\n"
    "    border: 1.5px solid " RP_BORDER ";\n"
    "    border-radius: 12px;\n"
    "    box-shadow: 0 8px 22px rgba(87, 82, 121, 0.24);\n"
    "    margin: 10px;\n"
    "    padding: 12px 14px;\n"
    "    color: " RP_TEXT ";\n"
    "    font-family: \"Liga SFMono Nerd Font\", \"LigaSF Mono NF\", \"JetBrainsMono Nerd Font Mono\", monospace;\n"
    "}\n"
    ".picker-card.toplevel-card {\n"
    "    margin: 0;\n"
    "    border-radius: 10px;\n"
    "    box-shadow: none;\n"
    "}\n"
    ".picker-title {\n"
    "    color: " RP_LOVE ";\n"
    "    font-weight: 700;\n"
    "    font-size: 13px;\n"
    "}\n"
    ".picker-subtitle {\n"
    "    color: " RP_SUBTLE ";\n"
    "    font-size: 12px;\n"
    "}\n"
    ".section-header {\n"
    "    color: " RP_PINE ";\n"
    "    font-weight: 700;\n"
    "    font-size: 11px;\n"
    "    margin-top: 2px;\n"
    "}\n"
    "button.tab-pill {\n"
    "    background-image: none;\n"
    "    background-color: " RP_SURFACE ";\n"
    "    color: " RP_SUBTLE ";\n"
    "    border: 1px solid #cecacd;\n"
    "    border-radius: 7px;\n"
    "    padding: 2px 9px;\n"
    "    font-size: 11px;\n"
    "    font-weight: 600;\n"
    "}\n"
    "button.tab-pill label { color: " RP_SUBTLE "; }\n"
    "button.tab-pill.tab-active {\n"
    "    background-image: none;\n"
    "    background-color: " RP_LOVE ";\n"
    "    color: " RP_BASE ";\n"
    "    border-color: " RP_LOVE ";\n"
    "}\n"
    "button.tab-pill.tab-active label { color: " RP_BASE "; }\n"
    "button.pill-btn {\n"
    "    background-image: none;\n"
    "    background-color: " RP_SURFACE ";\n"
    "    color: " RP_TEXT ";\n"
    "    border: 1px solid #cecacd;\n"
    "    border-radius: 7px;\n"
    "    padding: 2px 8px;\n"
    "    font-size: 11px;\n"
    "    font-weight: 600;\n"
    "}\n"
    "button.pill-btn label { color: " RP_TEXT "; }\n"
    "button.pill-btn:hover {\n"
    "    border-color: " RP_PINE ";\n"
    "    background-color: " RP_OVERLAY ";\n"
    "}\n"
    "button.pill-btn.pill-active {\n"
    "    background-image: none;\n"
    "    background-color: " RP_PINE ";\n"
    "    color: " RP_BASE ";\n"
    "    border-color: " RP_PINE ";\n"
    "}\n"
    "button.pill-btn.pill-active label { color: " RP_BASE "; }\n"
    "button.crumb-btn, button.crumb-active {\n"
    "    background-image: none;\n"
    "    background: transparent;\n"
    "    border: none;\n"
    "    padding: 1px 4px;\n"
    "    font-size: 11px;\n"
    "    border-radius: 4px;\n"
    "}\n"
    "button.crumb-btn, button.crumb-btn label { color: " RP_PINE "; }\n"
    "button.crumb-btn:hover { background-color: " RP_OVERLAY "; }\n"
    "button.crumb-active, button.crumb-active label { color: " RP_TEXT "; font-weight: 700; }\n"
    ".crumb-sep { color: " RP_MUTED "; font-size: 11px; }\n"
    "entry.picker-entry {\n"
    "    background-color: " RP_SURFACE ";\n"
    "    color: " RP_TEXT ";\n"
    "    border: 1.5px solid " RP_PINE ";\n"
    "    border-radius: 8px;\n"
    "    padding: 6px 10px;\n"
    "    min-height: 30px;\n"
    "    caret-color: " RP_GOLD ";\n"
    "}\n"
    "entry.picker-entry.search-inactive {\n"
    "    background-color: #f4ede8;\n"
    "    color: " RP_SUBTLE ";\n"
    "    border: 1px solid #dfdad9;\n"
    "}\n"
    "entry.picker-entry:focus-within {\n"
    "    border-color: " RP_PINE ";\n"
    "    outline: none;\n"
    "}\n"
    "entry.picker-entry.create-error {\n"
    "    border-color: " RP_LOVE ";\n"
    "}\n"
    "list.picker-list {\n"
    "    background: transparent;\n"
    "}\n"
    "list.picker-list row {\n"
    "    border-radius: 7px;\n"
    "    border-left: 2.5px solid transparent;\n"
    "    padding: 5px 8px 5px 6px;\n"
    "    margin: 1px 0;\n"
    "    color: " RP_TEXT ";\n"
    "}\n"
    "list.picker-list row.row-parent {\n"
    "    background-color: rgba(223, 218, 217, 0.35);\n"
    "}\n"
    "list.picker-list row:hover {\n"
    "    background-color: rgba(242, 233, 225, 0.75);\n"
    "}\n"
    "list.picker-list row:selected {\n"
    "    background-color: #ede2d8;\n"
    "    border-left-color: " RP_PINE ";\n"
    "    color: " RP_TEXT ";\n"
    "    outline: none;\n"
    "}\n"
    "flowbox.picker-grid {\n"
    "    background: transparent;\n"
    "}\n"
    "flowbox.picker-grid flowboxchild.grid-card-child {\n"
    "    border-radius: 9px;\n"
    "    padding: 5px;\n"
    "    border: 1.5px solid transparent;\n"
    "    background-color: rgba(255, 250, 243, 0.55);\n"
    "}\n"
    "flowbox.picker-grid flowboxchild.grid-card-child.grid-card-parent {\n"
    "    background-color: rgba(223, 218, 217, 0.52);\n"
    "}\n"
    "flowbox.picker-grid flowboxchild.grid-card-child:hover {\n"
    "    background-color: " RP_OVERLAY ";\n"
    "}\n"
    "flowbox.picker-grid flowboxchild.grid-card-child:selected {\n"
    "    background-color: #ede2d8;\n"
    "    border-color: " RP_PINE ";\n"
    "    outline: none;\n"
    "}\n"
    ".cover-box, .folder-box {\n"
    "    background-color: " RP_OVERLAY ";\n"
    "    border: 1px solid #dfdad9;\n"
    "    border-radius: 8px;\n"
    "}\n"
    ".folder-box-parent {\n"
    "    background-color: #e6dfd9;\n"
    "    border: 1px solid #cecacd;\n"
    "    border-radius: 8px;\n"
    "}\n"
    ".cover-placeholder-lg { color: " RP_MUTED "; font-size: 28px; }\n"
    ".cover-placeholder-sm { color: " RP_MUTED "; font-size: 15px; }\n"
    ".folder-icon-lg { color: " RP_PINE "; font-size: 36px; }\n"
    ".folder-icon-sm { color: " RP_PINE "; font-size: 17px; }\n"
    ".folder-icon-parent-lg { color: " RP_SUBTLE "; font-size: 36px; }\n"
    ".folder-icon-parent-sm { color: " RP_SUBTLE "; font-size: 17px; }\n"
    ".card-title-album { color: " RP_TEXT "; font-weight: 700; font-size: 11px; }\n"
    ".card-title-folder { color: " RP_PINE "; font-weight: 700; font-size: 11px; }\n"
    ".card-title-parent { color: " RP_TEXT "; font-weight: 700; font-size: 11px; }\n"
    ".card-subtitle { color: " RP_SUBTLE "; font-size: 10px; }\n"
    ".card-subtitle-parent { color: " RP_SUBTLE "; font-size: 10px; }\n"
    ".track-fav { color: " RP_LOVE "; font-size: 11px; }\n"
    "checkbutton.picker-check check {\n"
    "    border: 1.5px solid " RP_MUTED ";\n"
    "    border-radius: 4px;\n"
    "    background-color: " RP_SURFACE ";\n"
    "    min-width: 15px;\n"
    "    min-height: 15px;\n"
    "}\n"
    "checkbutton.picker-check:checked check {\n"
    "    background-color: " RP_LOVE ";\n"
    "    border-color: " RP_LOVE ";\n"
    "    color: " RP_BASE ";\n"
    "}\n"
    ".row-name-fav {\n"
    "    color: " RP_LOVE ";\n"
    "    font-weight: 700;\n"
    "}\n"
    ".row-name-pl {\n"
    "    color: " RP_TEXT ";\n"
    "}\n"
    ".row-name-new {\n"
    "    color: " RP_GOLD ";\n"
    "    font-weight: 600;\n"
    "}\n"
    ".row-count {\n"
    "    color: " RP_MUTED ";\n"
    "    font-size: 11px;\n"
    "}\n"
    ".badge-all {\n"
    "    color: " RP_PINE ";\n"
    "    font-weight: 700;\n"
    "    font-size: 11px;\n"
    "    background-color: rgba(40, 105, 131, 0.12);\n"
    "    border-radius: 5px;\n"
    "    padding: 1px 6px;\n"
    "}\n"
    ".badge-some {\n"
    "    color: " RP_GOLD ";\n"
    "    font-weight: 700;\n"
    "    font-size: 11px;\n"
    "    background-color: rgba(234, 157, 52, 0.14);\n"
    "    border-radius: 5px;\n"
    "    padding: 1px 6px;\n"
    "}\n"
    ".badge-none {\n"
    "    color: " RP_MUTED ";\n"
    "    font-size: 11px;\n"
    "}\n"
    ".new-box {\n"
    "    background-color: " RP_OVERLAY ";\n"
    "    border-radius: 8px;\n"
    "    padding: 8px 10px;\n"
    "}\n"
    ".new-label {\n"
    "    color: " RP_GOLD ";\n"
    "    font-weight: 700;\n"
    "    font-size: 12px;\n"
    "}\n"
    ".error-label {\n"
    "    color: " RP_LOVE ";\n"
    "    font-size: 11px;\n"
    "}\n"
    ".footer-hints {\n"
    "    color: " RP_MUTED ";\n"
    "    font-size: 11px;\n"
    "}\n"
    "button.btn-primary {\n"
    "    background-image: none;\n"
    "    background-color: " RP_PINE ";\n"
    "    color: " RP_BASE ";\n"
    "    border-radius: 7px;\n"
    "    padding: 4px 12px;\n"
    "    font-weight: 700;\n"
    "    font-size: 12px;\n"
    "    border: none;\n"
    "}\n"
    "button.btn-primary label { color: " RP_BASE "; }\n"
    "button.btn-primary:hover {\n"
    "    background-color: #1f556b;\n"
    "}\n"
    "button.btn-danger {\n"
    "    background-image: none;\n"
    "    background-color: transparent;\n"
    "    color: " RP_LOVE ";\n"
    "    border: 1px solid rgba(180, 99, 122, 0.45);\n"
    "    border-radius: 7px;\n"
    "    padding: 3px 9px;\n"
    "    font-size: 11px;\n"
    "}\n"
    "button.btn-danger label { color: " RP_LOVE "; }\n"
    "button.btn-danger:hover:not(:disabled) {\n"
    "    background-color: rgba(180, 99, 122, 0.12);\n"
    "}\n"
    "button.btn-danger:disabled, button.btn-danger:disabled label {\n"
    "    color: " RP_MUTED ";\n"
    "    border-color: #dfdad9;\n"
    "    opacity: 0.6;\n"
    "}\n";

static GtkCssProvider *g_css_provider = NULL;
static bool g_css_installed = false;

void picker_ui_install_css(GdkDisplay *display) {
    if (g_css_installed || !display) return;
    g_css_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(g_css_provider, LAYER_CSS);
    gtk_style_context_add_provider_for_display(
        display,
        GTK_STYLE_PROVIDER(g_css_provider),
        GTK_STYLE_PROVIDER_PRIORITY_USER
    );
    g_css_installed = true;
}

typedef enum {
    ROW_KIND_FAV = 0,
    ROW_KIND_PL  = 1,
    ROW_KIND_NEW = 2,
} RowKind;

typedef struct {
    RowKind kind;
    char *name;
    bool is_favourites;
    int count;
    int matched;
    PickerState state;
    char *create_candidate;
    GtkListBoxRow *row_widget;
    GtkCheckButton *chk_widget;
} VisibleRow;

typedef struct {
    GtkWindow *win;
    GtkWidget *card;
    GtkStack *stack;
    GtkButton *tab_pl_btn;
    GtkButton *tab_sr_btn;
    GtkLabel *target_lbl;
    PickerTabMode active_tab;
    PickerGeometry geom;
    bool layer_active;

    /* Playlists page */
    GtkEntry *search_entry;
    GtkButton *sort_btn;
    GtkScrolledWindow *scrolled;
    GtkListBox *listbox;
    GtkBox *new_box;
    GtkEntry *new_entry;
    GtkLabel *new_err;
    GtkButton *add_btn;
    GtkButton *del_btn;

    guint target_count;
    GPtrArray *overview;              /* PlaylistOverviewItem* */
    GPtrArray *override_target_songs; /* nullable char* */
    GPtrArray *selected_order;        /* char* preserving selection order */
    GHashTable *selected_set;         /* char* */
    GPtrArray *visible_rows;          /* VisibleRow* */
    bool sort_recent;
    bool pl_search_mode;
    bool prompt_active;
    bool completed;

    /* Search page */
    PickerCaches *caches;
    char *pid_str;
    LibraryBrowserCtx *browser_ctx;
    bool browser_initialized;

    PickerCompleteCb on_complete;
    gpointer user_data;
} PickerCtx;

static void visible_row_free(gpointer p) {
    VisibleRow *r = (VisibleRow *)p;
    if (!r) return;
    g_free(r->name);
    g_free(r->create_candidate);
    g_free(r);
}

static void picker_ctx_free(gpointer p) {
    PickerCtx *ctx = (PickerCtx *)p;
    if (!ctx) return;
    if (ctx->overview) g_ptr_array_unref(ctx->overview);
    if (ctx->override_target_songs) g_ptr_array_unref(ctx->override_target_songs);
    if (ctx->selected_order) g_ptr_array_unref(ctx->selected_order);
    if (ctx->selected_set) g_hash_table_destroy(ctx->selected_set);
    if (ctx->visible_rows) g_ptr_array_unref(ctx->visible_rows);
    g_free(ctx->pid_str);
    g_free(ctx);
}

static GPtrArray *clone_overview(GPtrArray *src) {
    GPtrArray *dst = g_ptr_array_new_with_free_func(playlist_overview_item_free);
    for (guint i = 0; src && i < src->len; i++) {
        PlaylistOverviewItem *s = (PlaylistOverviewItem *)g_ptr_array_index(src, i);
        PlaylistOverviewItem *c = g_new0(PlaylistOverviewItem, 1);
        c->name = g_strdup(s->name);
        c->is_favourites = s->is_favourites;
        c->exists = s->exists;
        c->count = s->count;
        c->matched = s->matched;
        c->last_modified = s->last_modified;
        c->state = s->state;
        g_ptr_array_add(dst, c);
    }
    return dst;
}

static void resolve_geom_for_tab(PickerCtx *ctx, PickerTabMode tab, int *out_w, int *out_h, int *out_left) {
    int def_w = (tab == PICKER_TAB_SEARCH) ? SR_WINDOW_WIDTH : PL_WINDOW_WIDTH;
    int def_h = (tab == PICKER_TAB_SEARCH) ? SR_WINDOW_HEIGHT : PL_WINDOW_HEIGHT;
    int w = (ctx->geom.width > 0) ? ctx->geom.width : def_w;
    int h = (ctx->geom.height > 0) ? ctx->geom.height : def_h;
    int left = (ctx->geom.left_margin >= 0)
        ? ctx->geom.left_margin
        : (SIDEBAR_LEFT_OFFSET + (SIDEBAR_WIDTH - w) / 2);
    if (left < 0) left = 0;
    *out_w = w;
    *out_h = h;
    *out_left = left;
}

static void apply_tab_geometry(PickerCtx *ctx, PickerTabMode tab) {
    int w = 0, h = 0, left = 0;
    resolve_geom_for_tab(ctx, tab, &w, &h, &left);
    gtk_window_set_default_size(ctx->win, w, h);
    if (ctx->layer_active && !ctx->geom.center_screen) {
        gtk_layer_set_margin(ctx->win, GTK_LAYER_SHELL_EDGE_LEFT, left);
    }
}

static VisibleRow *selected_visible_row(PickerCtx *ctx, int *out_idx) {
    GtkListBoxRow *row = gtk_list_box_get_selected_row(ctx->listbox);
    if (!row) {
        if (out_idx) *out_idx = -1;
        return NULL;
    }
    int idx = gtk_list_box_row_get_index(row);
    if (idx >= 0 && (guint)idx < ctx->visible_rows->len) {
        if (out_idx) *out_idx = idx;
        return (VisibleRow *)g_ptr_array_index(ctx->visible_rows, (guint)idx);
    }
    if (out_idx) *out_idx = -1;
    return NULL;
}

static const PlaylistOverviewItem *find_overview_item(const PickerCtx *ctx, const char *name) {
    for (guint i = 0; ctx->overview && i < ctx->overview->len; i++) {
        const PlaylistOverviewItem *it = (const PlaylistOverviewItem *)g_ptr_array_index(ctx->overview, i);
        if (g_strcmp0(it->name, name) == 0) return it;
    }
    return NULL;
}

static bool has_removable_selection(PickerCtx *ctx) {
    if (ctx->selected_order->len > 0) {
        for (guint i = 0; i < ctx->selected_order->len; i++) {
            const char *name = (const char *)g_ptr_array_index(ctx->selected_order, i);
            const PlaylistOverviewItem *it = find_overview_item(ctx, name);
            if (it && it->state != PICKER_STATE_NONE) return true;
        }
        return false;
    }
    VisibleRow *vr = selected_visible_row(ctx, NULL);
    if (!vr || vr->kind == ROW_KIND_NEW) return false;
    return vr->state != PICKER_STATE_NONE;
}

static void update_action_state(PickerCtx *ctx) {
    guint n = ctx->selected_order->len;
    if (n > 0) {
        char *add_lbl = g_strdup_printf("Add (%u)", n);
        char *del_lbl = g_strdup_printf("Remove (%u)", n);
        gtk_button_set_label(ctx->add_btn, add_lbl);
        gtk_button_set_label(ctx->del_btn, del_lbl);
        g_free(add_lbl);
        g_free(del_lbl);
    } else {
        gtk_button_set_label(ctx->add_btn, "Add");
        gtk_button_set_label(ctx->del_btn, "Remove");
    }

    bool can_remove = has_removable_selection(ctx);
    gtk_widget_set_sensitive(GTK_WIDGET(ctx->del_btn), can_remove);
    gtk_widget_set_tooltip_text(
        GTK_WIDGET(ctx->del_btn),
        can_remove
            ? "Remove target song(s) from selected playlist(s) (Ctrl+D)"
            : "Target song(s) are not in the selected playlist(s)"
    );
    gtk_widget_set_tooltip_text(
        GTK_WIDGET(ctx->add_btn),
        "Add target song(s) to selected playlist(s) (Enter)"
    );
}

static void set_selected(PickerCtx *ctx, const char *name, bool active) {
    if (!name || !*name) return;
    bool present = g_hash_table_contains(ctx->selected_set, name);
    if (active && !present) {
        g_hash_table_add(ctx->selected_set, g_strdup(name));
        g_ptr_array_add(ctx->selected_order, g_strdup(name));
    } else if (!active && present) {
        g_hash_table_remove(ctx->selected_set, name);
        for (guint i = 0; i < ctx->selected_order->len; i++) {
            const char *cur = (const char *)g_ptr_array_index(ctx->selected_order, i);
            if (g_strcmp0(cur, name) == 0) {
                g_ptr_array_remove_index(ctx->selected_order, i);
                break;
            }
        }
    }
}

static void finish_picker(PickerCtx *ctx, PickerAction action, GPtrArray *playlists) {
    if (ctx->completed) {
        if (playlists) g_ptr_array_unref(playlists);
        return;
    }
    ctx->completed = true;

    PickerCompleteCb cb = ctx->on_complete;
    gpointer ud = ctx->user_data;
    GPtrArray *override_targets = ctx->override_target_songs ? g_ptr_array_ref(ctx->override_target_songs) : NULL;
    ctx->on_complete = NULL;

    GPtrArray *dedup = g_ptr_array_new_with_free_func(g_free);
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; playlists && i < playlists->len; i++) {
        char *s = (char *)g_ptr_array_index(playlists, i);
        if (s && *s && !g_hash_table_contains(seen, s)) {
            g_hash_table_add(seen, s);
            g_ptr_array_add(dedup, g_strdup(s));
        }
    }
    g_hash_table_destroy(seen);
    if (playlists) g_ptr_array_unref(playlists);

    gtk_window_destroy(ctx->win);

    if (cb) {
        cb(action, dedup, override_targets, ud);
    }
    if (override_targets) g_ptr_array_unref(override_targets);
    g_ptr_array_unref(dedup);
}

static void enter_pl_search(PickerCtx *ctx) {
    if (!ctx) return;
    ctx->pl_search_mode = true;
    gtk_widget_set_can_focus(GTK_WIDGET(ctx->search_entry), TRUE);
    gtk_widget_remove_css_class(GTK_WIDGET(ctx->search_entry), "search-inactive");
    gtk_widget_grab_focus(GTK_WIDGET(ctx->search_entry));
    gtk_editable_set_position(GTK_EDITABLE(ctx->search_entry), -1);
}

static void exit_pl_search(PickerCtx *ctx) {
    if (!ctx) return;
    ctx->pl_search_mode = false;
    gtk_widget_set_can_focus(GTK_WIDGET(ctx->search_entry), FALSE);
    gtk_widget_add_css_class(GTK_WIDGET(ctx->search_entry), "search-inactive");
    VisibleRow *vr = selected_visible_row(ctx, NULL);
    if (vr && vr->row_widget) {
        gtk_widget_grab_focus(GTK_WIDGET(vr->row_widget));
    } else {
        gtk_widget_grab_focus(GTK_WIDGET(ctx->listbox));
    }
}

static void on_pl_search_pressed(
    GtkGestureClick *gesture,
    gint n_press,
    gdouble x,
    gdouble y,
    gpointer user_data
) {
    (void)gesture; (void)n_press; (void)x; (void)y;
    enter_pl_search((PickerCtx *)user_data);
}

static void ensure_row_visible(PickerCtx *ctx, GtkListBoxRow *row) {
    graphene_rect_t rect;
    if (!gtk_widget_compute_bounds(GTK_WIDGET(row), GTK_WIDGET(ctx->listbox), &rect)) {
        return;
    }
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(ctx->scrolled);
    double top = rect.origin.y;
    double bottom = top + rect.size.height;
    double val = gtk_adjustment_get_value(vadj);
    double page = gtk_adjustment_get_page_size(vadj);
    if (page <= 0.0) return;
    if (top < val) {
        gtk_adjustment_set_value(vadj, top);
    } else if (bottom > val + page) {
        gtk_adjustment_set_value(vadj, bottom - page);
    }
}

static void move_cursor(PickerCtx *ctx, int delta) {
    if (ctx->visible_rows->len == 0) return;
    int idx = -1;
    selected_visible_row(ctx, &idx);
    int nxt = (idx < 0) ? 0 : (idx + delta);
    if (nxt < 0) nxt = 0;
    if ((guint)nxt >= ctx->visible_rows->len) nxt = (int)ctx->visible_rows->len - 1;
    VisibleRow *vr = (VisibleRow *)g_ptr_array_index(ctx->visible_rows, (guint)nxt);
    gtk_list_box_select_row(ctx->listbox, vr->row_widget);
    ensure_row_visible(ctx, vr->row_widget);
    update_action_state(ctx);
    if (!ctx->pl_search_mode) {
        gtk_widget_grab_focus(GTK_WIDGET(vr->row_widget));
    }
}

static void jump_cursor(PickerCtx *ctx, int target_idx) {
    if (ctx->visible_rows->len == 0) return;
    if (target_idx < 0) target_idx = 0;
    if ((guint)target_idx >= ctx->visible_rows->len) target_idx = (int)ctx->visible_rows->len - 1;
    VisibleRow *vr = (VisibleRow *)g_ptr_array_index(ctx->visible_rows, (guint)target_idx);
    gtk_list_box_select_row(ctx->listbox, vr->row_widget);
    ensure_row_visible(ctx, vr->row_widget);
    update_action_state(ctx);
    if (!ctx->pl_search_mode) {
        gtk_widget_grab_focus(GTK_WIDGET(vr->row_widget));
    }
}

static void open_new_prompt(PickerCtx *ctx, const char *initial_text) {
    ctx->prompt_active = true;
    gtk_widget_set_visible(GTK_WIDGET(ctx->new_err), FALSE);
    gtk_widget_remove_css_class(GTK_WIDGET(ctx->new_entry), "create-error");
    gtk_widget_set_visible(GTK_WIDGET(ctx->new_box), TRUE);

    char *trimmed = g_strdup(initial_text ? initial_text : "");
    g_strstrip(trimmed);
    gtk_editable_set_text(GTK_EDITABLE(ctx->new_entry), trimmed);
    gtk_editable_set_position(GTK_EDITABLE(ctx->new_entry), -1);
    g_free(trimmed);

    gtk_widget_grab_focus(GTK_WIDGET(ctx->new_entry));
}

static void close_new_prompt(PickerCtx *ctx) {
    ctx->prompt_active = false;
    gtk_widget_set_visible(GTK_WIDGET(ctx->new_box), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(ctx->new_err), FALSE);
    gtk_widget_remove_css_class(GTK_WIDGET(ctx->new_entry), "create-error");
    if (ctx->pl_search_mode) enter_pl_search(ctx);
    else exit_pl_search(ctx);
}

static void submit_new_prompt(PickerCtx *ctx) {
    const char *raw = gtk_editable_get_text(GTK_EDITABLE(ctx->new_entry));
    char *validated = NULL;
    char *err = NULL;
    if (!mpd_validate_playlist_name(raw, &validated, &err)) {
        gtk_label_set_text(ctx->new_err, err ? err : "Invalid playlist name");
        gtk_widget_set_visible(GTK_WIDGET(ctx->new_err), TRUE);
        gtk_widget_add_css_class(GTK_WIDGET(ctx->new_entry), "create-error");
        g_free(err);
        return;
    }

    GPtrArray *chosen = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < ctx->selected_order->len; i++) {
        g_ptr_array_add(chosen, g_strdup((const char *)g_ptr_array_index(ctx->selected_order, i)));
    }
    g_ptr_array_add(chosen, validated);
    finish_picker(ctx, PICKER_ACTION_ADD, chosen);
}

static void submit_action(PickerCtx *ctx, PickerAction action) {
    if (ctx->prompt_active && action == PICKER_ACTION_ADD) {
        submit_new_prompt(ctx);
        return;
    }

    if (action == PICKER_ACTION_REMOVE && !has_removable_selection(ctx)) {
        return;
    }

    GPtrArray *chosen = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < ctx->selected_order->len; i++) {
        g_ptr_array_add(chosen, g_strdup((const char *)g_ptr_array_index(ctx->selected_order, i)));
    }

    VisibleRow *vr = selected_visible_row(ctx, NULL);
    if (action == PICKER_ACTION_ADD && vr && vr->kind == ROW_KIND_NEW) {
        if (vr->create_candidate && *vr->create_candidate) {
            g_ptr_array_add(chosen, g_strdup(vr->create_candidate));
            finish_picker(ctx, PICKER_ACTION_ADD, chosen);
        } else {
            g_ptr_array_unref(chosen);
            open_new_prompt(ctx, gtk_editable_get_text(GTK_EDITABLE(ctx->search_entry)));
        }
        return;
    }

    if (chosen->len == 0) {
        if (!vr || vr->kind == ROW_KIND_NEW) {
            g_ptr_array_unref(chosen);
            return;
        }
        g_ptr_array_add(chosen, g_strdup(vr->name));
    }

    finish_picker(ctx, action, chosen);
}

static void toggle_row_check(PickerCtx *ctx, VisibleRow *vr, int step) {
    if (vr->kind == ROW_KIND_NEW) {
        const char *init = (vr->create_candidate && *vr->create_candidate)
            ? vr->create_candidate
            : gtk_editable_get_text(GTK_EDITABLE(ctx->search_entry));
        open_new_prompt(ctx, init);
        return;
    }

    if (vr->chk_widget) {
        bool cur = gtk_check_button_get_active(vr->chk_widget);
        gtk_check_button_set_active(vr->chk_widget, !cur);
    }
    if (step != 0) {
        move_cursor(ctx, step);
    }
}

typedef struct {
    PickerCtx *ctx;
    VisibleRow *vr;
} RowClickData;

static void row_click_data_free(gpointer p, GClosure *closure) {
    (void)closure;
    g_free(p);
}

static void on_chk_toggled(GtkCheckButton *btn, gpointer user_data) {
    RowClickData *d = (RowClickData *)user_data;
    set_selected(d->ctx, d->vr->name, gtk_check_button_get_active(btn));
    update_action_state(d->ctx);
}

static void on_row_pressed(
    GtkGestureClick *gesture,
    gint n_press,
    gdouble x,
    gdouble y,
    gpointer user_data
) {
    (void)gesture;
    (void)x;
    (void)y;
    RowClickData *d = (RowClickData *)user_data;
    PickerCtx *ctx = d->ctx;
    VisibleRow *vr = d->vr;

    gtk_list_box_select_row(ctx->listbox, vr->row_widget);
    update_action_state(ctx);

    if (vr->kind == ROW_KIND_NEW) {
        if (vr->create_candidate && *vr->create_candidate) {
            GPtrArray *chosen = g_ptr_array_new_with_free_func(g_free);
            for (guint i = 0; i < ctx->selected_order->len; i++) {
                g_ptr_array_add(chosen, g_strdup((const char *)g_ptr_array_index(ctx->selected_order, i)));
            }
            g_ptr_array_add(chosen, g_strdup(vr->create_candidate));
            finish_picker(ctx, PICKER_ACTION_ADD, chosen);
        } else {
            open_new_prompt(ctx, gtk_editable_get_text(GTK_EDITABLE(ctx->search_entry)));
        }
        return;
    }

    if (n_press == 2) {
        set_selected(ctx, vr->name, true);
        if (vr->chk_widget && !gtk_check_button_get_active(vr->chk_widget)) {
            gtk_check_button_set_active(vr->chk_widget, TRUE);
        }
        GPtrArray *chosen = g_ptr_array_new_with_free_func(g_free);
        for (guint i = 0; i < ctx->selected_order->len; i++) {
            g_ptr_array_add(chosen, g_strdup((const char *)g_ptr_array_index(ctx->selected_order, i)));
        }
        finish_picker(ctx, PICKER_ACTION_ADD, chosen);
    } else if (vr->chk_widget) {
        gtk_check_button_set_active(
            vr->chk_widget,
            !gtk_check_button_get_active(vr->chk_widget)
        );
    }
}

static gint cmp_overview_alpha(gconstpointer a, gconstpointer b) {
    const PlaylistOverviewItem *ia = *(const PlaylistOverviewItem *const *)a;
    const PlaylistOverviewItem *ib = *(const PlaylistOverviewItem *const *)b;
    if (ia->is_favourites != ib->is_favourites) return ia->is_favourites ? -1 : 1;
    char *fa = g_utf8_casefold(ia->name, -1);
    char *fb = g_utf8_casefold(ib->name, -1);
    int cmp = g_strcmp0(fa, fb);
    if (cmp == 0) cmp = g_strcmp0(ia->name, ib->name);
    g_free(fa);
    g_free(fb);
    return cmp;
}

static gint cmp_overview_recent(gconstpointer a, gconstpointer b) {
    const PlaylistOverviewItem *ia = *(const PlaylistOverviewItem *const *)a;
    const PlaylistOverviewItem *ib = *(const PlaylistOverviewItem *const *)b;
    if (ia->is_favourites != ib->is_favourites) return ia->is_favourites ? -1 : 1;

    int rank_a = (int)ia->state;
    int rank_b = (int)ib->state;
    if (rank_a != rank_b) return (rank_b > rank_a) ? 1 : -1;

    if (ia->last_modified != ib->last_modified) {
        return (ib->last_modified > ia->last_modified) ? 1 : -1;
    }
    return cmp_overview_alpha(a, b);
}

static void rebuild_rows(PickerCtx *ctx, const char *query) {
    gtk_list_box_remove_all(ctx->listbox);
    g_ptr_array_set_size(ctx->visible_rows, 0);

    if (ctx->overview && ctx->overview->len > 1) {
        g_ptr_array_sort(ctx->overview, ctx->sort_recent ? cmp_overview_recent : cmp_overview_alpha);
    }
    gtk_button_set_label(ctx->sort_btn, ctx->sort_recent ? "Recent ⇅" : "A–Z ⇅");

    char *cleaned = g_strdup(query ? query : "");
    g_strstrip(cleaned);
    char *needle = g_utf8_casefold(cleaned, -1);
    bool exact_match = false;

    for (guint i = 0; ctx->overview && i < ctx->overview->len; i++) {
        PlaylistOverviewItem *item = (PlaylistOverviewItem *)g_ptr_array_index(ctx->overview, i);
        char *cf = g_utf8_casefold(item->name, -1);
        if (*needle && g_strcmp0(cf, needle) == 0) {
            exact_match = true;
        }
        if (*needle && strstr(cf, needle) == NULL) {
            g_free(cf);
            continue;
        }
        g_free(cf);

        VisibleRow *vr = g_new0(VisibleRow, 1);
        vr->kind = item->is_favourites ? ROW_KIND_FAV : ROW_KIND_PL;
        vr->name = g_strdup(item->name);
        vr->is_favourites = item->is_favourites;
        vr->count = item->count;
        vr->matched = item->matched;
        vr->state = item->state;
        g_ptr_array_add(ctx->visible_rows, vr);
    }

    char *candidate = NULL;
    if (*cleaned && !exact_match) {
        if (!mpd_validate_playlist_name(cleaned, &candidate, NULL)) {
            candidate = NULL;
        }
    }
    VisibleRow *new_vr = g_new0(VisibleRow, 1);
    new_vr->kind = ROW_KIND_NEW;
    new_vr->name = g_strdup("");
    new_vr->create_candidate = candidate ? candidate : g_strdup("");
    g_ptr_array_add(ctx->visible_rows, new_vr);

    g_free(needle);
    g_free(cleaned);

    for (guint i = 0; i < ctx->visible_rows->len; i++) {
        VisibleRow *vr = (VisibleRow *)g_ptr_array_index(ctx->visible_rows, i);
        GtkWidget *row_w = gtk_list_box_row_new();
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row_w), TRUE);
        gtk_widget_set_focus_on_click(row_w, FALSE);

        GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row_w), hbox);
        vr->row_widget = GTK_LIST_BOX_ROW(row_w);

        if (vr->kind != ROW_KIND_NEW) {
            GtkWidget *chk_w = gtk_check_button_new();
            gtk_widget_add_css_class(chk_w, "picker-check");
            gtk_widget_set_can_focus(chk_w, FALSE);
            gtk_widget_set_can_target(chk_w, FALSE);
            gtk_check_button_set_active(
                GTK_CHECK_BUTTON(chk_w),
                g_hash_table_contains(ctx->selected_set, vr->name)
            );
            gtk_box_append(GTK_BOX(hbox), chk_w);
            vr->chk_widget = GTK_CHECK_BUTTON(chk_w);

            RowClickData *chk_data = g_new(RowClickData, 1);
            chk_data->ctx = ctx;
            chk_data->vr = vr;
            g_signal_connect_data(chk_w, "toggled", G_CALLBACK(on_chk_toggled), chk_data, row_click_data_free, 0);

            char *title_text = g_strdup_printf("%s %s", vr->is_favourites ? "★" : "♫", vr->name);
            GtkWidget *name_lbl = gtk_label_new(title_text);
            g_free(title_text);
            gtk_label_set_xalign(GTK_LABEL(name_lbl), 0.0f);
            gtk_widget_add_css_class(name_lbl, vr->is_favourites ? "row-name-fav" : "row-name-pl");
            gtk_widget_set_hexpand(name_lbl, TRUE);
            gtk_label_set_ellipsize(GTK_LABEL(name_lbl), PANGO_ELLIPSIZE_END);
            gtk_label_set_max_width_chars(GTK_LABEL(name_lbl), 28);
            gtk_box_append(GTK_BOX(hbox), name_lbl);

            char badge_buf[32];
            const char *badge_css = "badge-none";
            if (vr->state == PICKER_STATE_ALL) {
                snprintf(badge_buf, sizeof(badge_buf), "✓ in");
                badge_css = "badge-all";
            } else if (vr->state == PICKER_STATE_SOME) {
                if (ctx->target_count > 1) {
                    snprintf(badge_buf, sizeof(badge_buf), "~ %d/%u", vr->matched, ctx->target_count);
                } else {
                    snprintf(badge_buf, sizeof(badge_buf), "~ part");
                }
                badge_css = "badge-some";
            } else {
                snprintf(badge_buf, sizeof(badge_buf), "—");
            }
            GtkWidget *badge_lbl = gtk_label_new(badge_buf);
            gtk_widget_add_css_class(badge_lbl, badge_css);
            gtk_box_append(GTK_BOX(hbox), badge_lbl);

            char *cnt_txt = g_strdup_printf("(%d)", vr->count);
            GtkWidget *cnt_lbl = gtk_label_new(cnt_txt);
            g_free(cnt_txt);
            gtk_widget_add_css_class(cnt_lbl, "row-count");
            gtk_box_append(GTK_BOX(hbox), cnt_lbl);

            if (ctx->target_count > 0) {
                char *tip = g_strdup_printf(
                    "%s: %d of %u target song%s already in playlist (%d total)",
                    vr->name, vr->matched, ctx->target_count,
                    ctx->target_count == 1 ? "" : "s", vr->count
                );
                gtk_widget_set_tooltip_text(row_w, tip);
                g_free(tip);
            }
        } else {
            char *txt = (vr->create_candidate && *vr->create_candidate)
                ? g_strdup_printf("+ Create \"%s\"", vr->create_candidate)
                : g_strdup("+ Create new playlist…");
            GtkWidget *new_row_lbl = gtk_label_new(txt);
            g_free(txt);
            gtk_label_set_xalign(GTK_LABEL(new_row_lbl), 0.0f);
            gtk_widget_add_css_class(new_row_lbl, "row-name-new");
            gtk_widget_set_hexpand(new_row_lbl, TRUE);
            gtk_box_append(GTK_BOX(hbox), new_row_lbl);
        }

        GtkGesture *click = gtk_gesture_click_new();
        RowClickData *click_data = g_new(RowClickData, 1);
        click_data->ctx = ctx;
        click_data->vr = vr;
        g_signal_connect_data(click, "pressed", G_CALLBACK(on_row_pressed), click_data, row_click_data_free, 0);
        gtk_widget_add_controller(row_w, GTK_EVENT_CONTROLLER(click));

        gtk_list_box_append(ctx->listbox, row_w);
    }

    if (ctx->visible_rows->len > 0) {
        VisibleRow *first = (VisibleRow *)g_ptr_array_index(ctx->visible_rows, 0);
        gtk_list_box_select_row(ctx->listbox, first->row_widget);
    }
    update_action_state(ctx);
}

static void on_search_changed(GtkEditable *editable, gpointer user_data) {
    PickerCtx *ctx = (PickerCtx *)user_data;
    rebuild_rows(ctx, gtk_editable_get_text(editable));
}

static void on_sort_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    PickerCtx *ctx = (PickerCtx *)user_data;
    ctx->sort_recent = !ctx->sort_recent;
    rebuild_rows(ctx, gtk_editable_get_text(GTK_EDITABLE(ctx->search_entry)));
}

static void on_new_entry_activate(GtkEntry *entry, gpointer user_data) {
    (void)entry;
    submit_new_prompt((PickerCtx *)user_data);
}

static void on_add_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    submit_action((PickerCtx *)user_data, PICKER_ACTION_ADD);
}

static void on_del_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    submit_action((PickerCtx *)user_data, PICKER_ACTION_REMOVE);
}

static void clear_all_marks(PickerCtx *ctx) {
    g_hash_table_remove_all(ctx->selected_set);
    g_ptr_array_set_size(ctx->selected_order, 0);
    for (guint i = 0; i < ctx->visible_rows->len; i++) {
        VisibleRow *vr = (VisibleRow *)g_ptr_array_index(ctx->visible_rows, i);
        if (vr->chk_widget && gtk_check_button_get_active(vr->chk_widget)) {
            gtk_check_button_set_active(vr->chk_widget, FALSE);
        }
    }
    update_action_state(ctx);
}

static void quick_toggle_favourites(PickerCtx *ctx) {
    for (guint i = 0; i < ctx->visible_rows->len; i++) {
        VisibleRow *vr = (VisibleRow *)g_ptr_array_index(ctx->visible_rows, i);
        if (vr->is_favourites && vr->chk_widget) {
            gtk_check_button_set_active(
                vr->chk_widget,
                !gtk_check_button_get_active(vr->chk_widget)
            );
            return;
        }
    }
    bool present = g_hash_table_contains(ctx->selected_set, FAVOURITES_PLAYLIST);
    set_selected(ctx, FAVOURITES_PLAYLIST, !present);
    update_action_state(ctx);
}

static void switch_tab(PickerCtx *ctx, PickerTabMode tab);

static void on_send_to_playlists(GPtrArray *uris, const char *summary, const char *tooltip, gpointer user_data) {
    PickerCtx *ctx = (PickerCtx *)user_data;
    if (!uris || uris->len == 0) return;

    GError *err = NULL;
    struct mpd_connection *conn = mpd_open_configured(&err);
    if (!conn) {
        g_clear_error(&err);
        return;
    }

    GPtrArray *overview = mpd_get_playlists_overview(conn, uris, &err);
    g_clear_error(&err);
    mpd_connection_free(conn);
    if (!overview) return;

    if (ctx->override_target_songs) g_ptr_array_unref(ctx->override_target_songs);
    ctx->override_target_songs = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < uris->len; i++) {
        g_ptr_array_add(ctx->override_target_songs, g_strdup((const char *)g_ptr_array_index(uris, i)));
    }

    if (ctx->overview) g_ptr_array_unref(ctx->overview);
    ctx->overview = overview;
    ctx->target_count = uris->len;
    clear_all_marks(ctx);

    gtk_widget_set_sensitive(GTK_WIDGET(ctx->tab_pl_btn), TRUE);
    char *sub_txt = g_strdup_printf("♪ %s", summary ? summary : "");
    gtk_label_set_text(ctx->target_lbl, sub_txt);
    g_free(sub_txt);
    gtk_widget_set_tooltip_text(GTK_WIDGET(ctx->target_lbl), tooltip ? tooltip : summary);

    rebuild_rows(ctx, gtk_editable_get_text(GTK_EDITABLE(ctx->search_entry)));
    switch_tab(ctx, PICKER_TAB_PLAYLISTS);
}

static void on_search_play_done(LibPlayAction action, gpointer user_data) {
    (void)action;
    PickerCtx *ctx = (PickerCtx *)user_data;
    finish_picker(ctx, PICKER_ACTION_CANCEL, NULL);
}

static void ensure_browser_initialized(PickerCtx *ctx) {
    if (ctx->browser_initialized) return;
    GtkWidget *page = library_browser_build_page(
        ctx->caches,
        ctx->pid_str,
        on_send_to_playlists,
        on_search_play_done,
        ctx,
        &ctx->browser_ctx
    );
    gtk_stack_add_named(ctx->stack, page, "search");
    ctx->browser_initialized = true;
}

static void switch_tab(PickerCtx *ctx, PickerTabMode tab) {
    ctx->active_tab = tab;
    if (tab == PICKER_TAB_SEARCH) {
        ensure_browser_initialized(ctx);
        gtk_window_set_title(ctx->win, "rmpc-minipicker: Search");
        gtk_widget_add_css_class(GTK_WIDGET(ctx->tab_sr_btn), "tab-active");
        gtk_widget_remove_css_class(GTK_WIDGET(ctx->tab_pl_btn), "tab-active");
        gtk_widget_set_visible(GTK_WIDGET(ctx->target_lbl), FALSE);
        apply_tab_geometry(ctx, PICKER_TAB_SEARCH);
        gtk_stack_set_visible_child_name(ctx->stack, "search");
        library_browser_on_activate(ctx->browser_ctx);
    } else {
        gtk_window_set_title(ctx->win, "rmpc-minipicker: Playlists");
        gtk_widget_add_css_class(GTK_WIDGET(ctx->tab_pl_btn), "tab-active");
        gtk_widget_remove_css_class(GTK_WIDGET(ctx->tab_sr_btn), "tab-active");
        gtk_widget_set_visible(GTK_WIDGET(ctx->target_lbl), TRUE);
        apply_tab_geometry(ctx, PICKER_TAB_PLAYLISTS);
        gtk_stack_set_visible_child_name(ctx->stack, "playlists");
        exit_pl_search(ctx);
    }
}

static void on_tab_pl_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    switch_tab((PickerCtx *)user_data, PICKER_TAB_PLAYLISTS);
}

static void on_tab_sr_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    switch_tab((PickerCtx *)user_data, PICKER_TAB_SEARCH);
}

static gboolean on_key_pressed(
    GtkEventControllerKey *ctl,
    guint keyval,
    guint keycode,
    GdkModifierType state,
    gpointer user_data
) {
    (void)ctl;
    (void)keycode;
    PickerCtx *ctx = (PickerCtx *)user_data;
    bool ctrl = (state & GDK_CONTROL_MASK) != 0;
    bool shift = (state & GDK_SHIFT_MASK) != 0;
    bool alt = (state & GDK_ALT_MASK) != 0;

    if (alt && keyval == GDK_KEY_1) {
        if (ctx->overview) switch_tab(ctx, PICKER_TAB_PLAYLISTS);
        return TRUE;
    }
    if (alt && keyval == GDK_KEY_2) {
        switch_tab(ctx, PICKER_TAB_SEARCH);
        return TRUE;
    }

    if (ctx->active_tab == PICKER_TAB_SEARCH && ctx->browser_ctx) {
        if (library_browser_handle_key(ctx->browser_ctx, keyval, state)) {
            return TRUE;
        }
        if (keyval == GDK_KEY_Escape) {
            finish_picker(ctx, PICKER_ACTION_CANCEL, NULL);
            return TRUE;
        }
        return FALSE;
    }

    /* Playlists Tab */
    if (ctx->prompt_active) {
        if (keyval == GDK_KEY_Escape) {
            close_new_prompt(ctx);
            return TRUE;
        }
        if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
            submit_new_prompt(ctx);
            return TRUE;
        }
        if (ctrl && (keyval == GDK_KEY_u || keyval == GDK_KEY_U)) {
            gtk_editable_set_text(GTK_EDITABLE(ctx->new_entry), "");
            return TRUE;
        }
        return FALSE;
    }

    /* Ctrl+F toggles search mode */
    if (ctrl && (keyval == GDK_KEY_f || keyval == GDK_KEY_F)) {
        if (ctx->pl_search_mode) exit_pl_search(ctx);
        else enter_pl_search(ctx);
        return TRUE;
    }

    if (ctx->pl_search_mode) {
        if (keyval == GDK_KEY_Escape) {
            exit_pl_search(ctx);
            return TRUE;
        }
        if (ctrl && (keyval == GDK_KEY_u || keyval == GDK_KEY_U)) {
            gtk_editable_set_text(GTK_EDITABLE(ctx->search_entry), "");
            return TRUE;
        }
        if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
            submit_action(ctx, PICKER_ACTION_ADD);
            return TRUE;
        }
        return FALSE;
    }

    /* Playlists Normal (Vim) Mode */
    if (keyval == GDK_KEY_slash) {
        enter_pl_search(ctx);
        return TRUE;
    }

    if (keyval == GDK_KEY_q || keyval == GDK_KEY_Escape) {
        finish_picker(ctx, PICKER_ACTION_CANCEL, NULL);
        return TRUE;
    }

    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
        submit_action(ctx, PICKER_ACTION_ADD);
        return TRUE;
    }

    if (keyval == GDK_KEY_n || (ctrl && (keyval == GDK_KEY_n || keyval == GDK_KEY_N))) {
        open_new_prompt(ctx, gtk_editable_get_text(GTK_EDITABLE(ctx->search_entry)));
        return TRUE;
    }

    if (ctrl && (keyval == GDK_KEY_d || keyval == GDK_KEY_D)) {
        submit_action(ctx, PICKER_ACTION_REMOVE);
        return TRUE;
    }

    if (keyval == GDK_KEY_s || (ctrl && (keyval == GDK_KEY_s || keyval == GDK_KEY_S))) {
        on_sort_clicked(ctx->sort_btn, ctx);
        return TRUE;
    }

    if (ctrl && (keyval == GDK_KEY_x || keyval == GDK_KEY_X)) {
        clear_all_marks(ctx);
        return TRUE;
    }

    if (keyval == GDK_KEY_f || keyval == GDK_KEY_F) {
        quick_toggle_favourites(ctx);
        return TRUE;
    }

    if (keyval == GDK_KEY_k || keyval == GDK_KEY_Up ||
        (ctrl && (keyval == GDK_KEY_k || keyval == GDK_KEY_K || keyval == GDK_KEY_p || keyval == GDK_KEY_P))) {
        move_cursor(ctx, -1);
        return TRUE;
    }

    if (keyval == GDK_KEY_j || keyval == GDK_KEY_Down ||
        (ctrl && (keyval == GDK_KEY_j || keyval == GDK_KEY_J))) {
        move_cursor(ctx, 1);
        return TRUE;
    }

    if (keyval == GDK_KEY_g) {
        jump_cursor(ctx, 0);
        return TRUE;
    }

    if (keyval == GDK_KEY_G) {
        jump_cursor(ctx, (int)ctx->visible_rows->len - 1);
        return TRUE;
    }

    if (keyval == GDK_KEY_Page_Up || (ctrl && (keyval == GDK_KEY_u || keyval == GDK_KEY_U))) {
        move_cursor(ctx, -6);
        return TRUE;
    }

    if (keyval == GDK_KEY_Page_Down) {
        move_cursor(ctx, 6);
        return TRUE;
    }

    if (ctrl && keyval == GDK_KEY_Home) {
        jump_cursor(ctx, 0);
        return TRUE;
    }

    if (ctrl && keyval == GDK_KEY_End) {
        jump_cursor(ctx, (int)ctx->visible_rows->len - 1);
        return TRUE;
    }

    if (keyval == GDK_KEY_ISO_Left_Tab || (keyval == GDK_KEY_Tab && shift)) {
        VisibleRow *vr = selected_visible_row(ctx, NULL);
        if (vr) toggle_row_check(ctx, vr, -1);
        return TRUE;
    }

    if (keyval == GDK_KEY_Tab) {
        VisibleRow *vr = selected_visible_row(ctx, NULL);
        if (vr) toggle_row_check(ctx, vr, 1);
        return TRUE;
    }

    if (keyval == GDK_KEY_space || keyval == GDK_KEY_KP_Space) {
        VisibleRow *vr = selected_visible_row(ctx, NULL);
        if (vr) toggle_row_check(ctx, vr, 0);
        return TRUE;
    }

    return FALSE;
}

static gboolean on_close_request(GtkWindow *win, gpointer user_data) {
    (void)win;
    PickerCtx *ctx = (PickerCtx *)user_data;
    if (!ctx->completed) {
        ctx->completed = true;
        PickerCompleteCb cb = ctx->on_complete;
        gpointer ud = ctx->user_data;
        ctx->on_complete = NULL;
        if (cb) {
            GPtrArray *empty = g_ptr_array_new_with_free_func(g_free);
            cb(PICKER_ACTION_CANCEL, empty, NULL, ud);
            g_ptr_array_unref(empty);
        }
    }
    return FALSE;
}

static void on_win_show(GtkWidget *w, gpointer user_data) {
    (void)w;
    PickerCtx *ctx = (PickerCtx *)user_data;
    if (ctx->active_tab == PICKER_TAB_SEARCH && ctx->browser_ctx) {
        library_browser_exit_search_mode(ctx->browser_ctx);
    } else {
        exit_pl_search(ctx);
    }
}

GtkWindow *picker_ui_show(
    GtkApplication *app,
    PickerTabMode initial_tab,
    const char *summary,
    const char *summary_tooltip,
    guint target_count,
    GPtrArray *overview,
    PickerCaches *caches,
    const char *pid_str,
    const PickerGeometry *geom,
    bool use_layer_shell,
    PickerCompleteCb on_complete,
    gpointer user_data
) {
    picker_ui_install_css(gdk_display_get_default());

    PickerCtx *ctx = g_new0(PickerCtx, 1);
    if (geom) {
        ctx->geom = *geom;
    } else {
        ctx->geom.left_margin = -1;
    }

    GtkWidget *win_w = gtk_application_window_new(app);
    GtkWindow *win = GTK_WINDOW(win_w);
    gtk_window_set_title(win, initial_tab == PICKER_TAB_SEARCH ? "rmpc-minipicker: Search" : "rmpc-minipicker: Playlists");
    gtk_widget_add_css_class(win_w, "rmpc-layer-window");
    gtk_window_set_decorated(win, FALSE);
    gtk_window_set_resizable(win, TRUE);

    if (use_layer_shell && gtk_layer_is_supported()) {
        ctx->layer_active = true;
        gtk_layer_init_for_window(win);
        gtk_layer_set_layer(win, GTK_LAYER_SHELL_LAYER_OVERLAY);
        gtk_layer_set_namespace(win, "rmpc-minipicker");
        gtk_layer_set_keyboard_mode(win, GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
        gtk_layer_set_exclusive_zone(win, 0);
        gtk_layer_set_respect_close(win, TRUE);
        if (!ctx->geom.center_screen) {
            gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
        }
    }

    ctx->win = win;
    ctx->active_tab = initial_tab;
    ctx->target_count = target_count;
    ctx->overview = overview ? clone_overview(overview) : NULL;
    ctx->selected_order = g_ptr_array_new_with_free_func(g_free);
    ctx->selected_set = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    ctx->visible_rows = g_ptr_array_new_with_free_func(visible_row_free);
    ctx->sort_recent = true;
    ctx->pl_search_mode = false;
    ctx->caches = caches;
    ctx->pid_str = g_strdup(pid_str ? pid_str : "");
    ctx->on_complete = on_complete;
    ctx->user_data = user_data;

    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(card, "picker-card");
    if (!ctx->layer_active) {
        gtk_widget_add_css_class(card, "toplevel-card");
    }
    ctx->card = card;
    gtk_window_set_child(win, card);

    apply_tab_geometry(ctx, initial_tab);

    /* Top tab bar + target summary */
    GtkWidget *header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *tab_pl = gtk_button_new_with_label("★ Playlists");
    gtk_widget_add_css_class(tab_pl, "tab-pill");
    gtk_widget_set_can_focus(tab_pl, FALSE);
    gtk_widget_set_tooltip_text(tab_pl, "Playlist Minipicker (Alt+1)");
    ctx->tab_pl_btn = GTK_BUTTON(tab_pl);

    GtkWidget *tab_sr = gtk_button_new_with_label("󰍉 Search");
    gtk_widget_add_css_class(tab_sr, "tab-pill");
    gtk_widget_set_can_focus(tab_sr, FALSE);
    gtk_widget_set_tooltip_text(tab_sr, "Search & Visual Folder Browser (Alt+2)");
    ctx->tab_sr_btn = GTK_BUTTON(tab_sr);

    if (!ctx->overview) {
        gtk_widget_set_sensitive(tab_pl, FALSE);
        gtk_widget_set_tooltip_text(tab_pl, "Select an album/track in Search (m) or open from rmpc (m)");
    }

    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);

    char *sub_txt = g_strdup_printf("♪ %s", summary ? summary : "");
    GtkWidget *target_lbl = gtk_label_new(sub_txt);
    g_free(sub_txt);
    gtk_label_set_xalign(GTK_LABEL(target_lbl), 1.0f);
    gtk_widget_add_css_class(target_lbl, "picker-subtitle");
    gtk_label_set_ellipsize(GTK_LABEL(target_lbl), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(target_lbl), 30);
    gtk_widget_set_tooltip_text(target_lbl, summary_tooltip ? summary_tooltip : (summary ? summary : ""));
    ctx->target_lbl = GTK_LABEL(target_lbl);

    gtk_box_append(GTK_BOX(header_box), tab_pl);
    gtk_box_append(GTK_BOX(header_box), tab_sr);
    gtk_box_append(GTK_BOX(header_box), spacer);
    gtk_box_append(GTK_BOX(header_box), target_lbl);
    gtk_box_append(GTK_BOX(card), header_box);

    /* Stack switching between Playlists and Search */
    GtkWidget *stack_w = gtk_stack_new();
    gtk_widget_set_vexpand(stack_w, TRUE);
    gtk_stack_set_transition_type(GTK_STACK(stack_w), GTK_STACK_TRANSITION_TYPE_NONE);
    ctx->stack = GTK_STACK(stack_w);
    gtk_box_append(GTK_BOX(card), stack_w);

    /* Playlists Page */
    GtkWidget *pl_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);

    GtkWidget *search_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *search_w = gtk_entry_new();
    gtk_widget_add_css_class(search_w, "picker-entry");
    gtk_widget_add_css_class(search_w, "search-inactive");
    gtk_widget_set_can_focus(search_w, FALSE);
    gtk_widget_set_hexpand(search_w, TRUE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(search_w), "Press / or Ctrl+F to filter playlists…");
    ctx->search_entry = GTK_ENTRY(search_w);

    GtkGesture *pl_search_click = gtk_gesture_click_new();
    g_signal_connect(pl_search_click, "pressed", G_CALLBACK(on_pl_search_pressed), ctx);
    gtk_widget_add_controller(search_w, GTK_EVENT_CONTROLLER(pl_search_click));

    GtkWidget *sort_w = gtk_button_new_with_label("Recent ⇅");
    gtk_widget_add_css_class(sort_w, "pill-btn");
    gtk_widget_set_can_focus(sort_w, FALSE);
    gtk_widget_set_tooltip_text(sort_w, "Toggle Recent / Alphabetical sort (s)");
    ctx->sort_btn = GTK_BUTTON(sort_w);

    gtk_box_append(GTK_BOX(search_row), search_w);
    gtk_box_append(GTK_BOX(search_row), sort_w);
    gtk_box_append(GTK_BOX(pl_page), search_row);

    GtkWidget *scrolled_w = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scrolled_w, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_w), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkWidget *listbox_w = gtk_list_box_new();
    gtk_widget_add_css_class(listbox_w, "picker-list");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(listbox_w), GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(listbox_w), FALSE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_w), listbox_w);
    gtk_box_append(GTK_BOX(pl_page), scrolled_w);
    ctx->scrolled = GTK_SCROLLED_WINDOW(scrolled_w);
    ctx->listbox = GTK_LIST_BOX(listbox_w);

    GtkWidget *new_box_w = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(new_box_w, "new-box");
    GtkWidget *new_lbl = gtk_label_new("New playlist name:");
    gtk_label_set_xalign(GTK_LABEL(new_lbl), 0.0f);
    gtk_widget_add_css_class(new_lbl, "new-label");
    GtkWidget *new_entry_w = gtk_entry_new();
    gtk_widget_add_css_class(new_entry_w, "picker-entry");
    gtk_entry_set_placeholder_text(GTK_ENTRY(new_entry_w), "Enter playlist name… (Enter: add, Esc: back)");
    GtkWidget *new_err_w = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(new_err_w), 0.0f);
    gtk_widget_add_css_class(new_err_w, "error-label");
    gtk_widget_set_visible(new_err_w, FALSE);
    gtk_box_append(GTK_BOX(new_box_w), new_lbl);
    gtk_box_append(GTK_BOX(new_box_w), new_entry_w);
    gtk_box_append(GTK_BOX(new_box_w), new_err_w);
    gtk_widget_set_visible(new_box_w, FALSE);
    gtk_box_append(GTK_BOX(pl_page), new_box_w);
    ctx->new_box = GTK_BOX(new_box_w);
    ctx->new_entry = GTK_ENTRY(new_entry_w);
    ctx->new_err = GTK_LABEL(new_err_w);

    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *hints = gtk_label_new("jk:nav  Space:mark  /:search  Enter:add  n:new  ^D:rem  q:quit");
    gtk_label_set_xalign(GTK_LABEL(hints), 0.0f);
    gtk_widget_add_css_class(hints, "footer-hints");
    gtk_widget_set_hexpand(hints, TRUE);
    GtkWidget *del_w = gtk_button_new_with_label("Remove");
    gtk_widget_add_css_class(del_w, "btn-danger");
    gtk_widget_set_can_focus(del_w, FALSE);
    GtkWidget *add_w = gtk_button_new_with_label("Add");
    gtk_widget_add_css_class(add_w, "btn-primary");
    gtk_widget_set_can_focus(add_w, FALSE);
    gtk_box_append(GTK_BOX(footer), hints);
    gtk_box_append(GTK_BOX(footer), del_w);
    gtk_box_append(GTK_BOX(footer), add_w);
    gtk_box_append(GTK_BOX(pl_page), footer);
    ctx->del_btn = GTK_BUTTON(del_w);
    ctx->add_btn = GTK_BUTTON(add_w);

    gtk_stack_add_named(ctx->stack, pl_page, "playlists");

    if (ctx->overview) {
        rebuild_rows(ctx, "");
    }

    g_signal_connect(tab_pl, "clicked", G_CALLBACK(on_tab_pl_clicked), ctx);
    g_signal_connect(tab_sr, "clicked", G_CALLBACK(on_tab_sr_clicked), ctx);
    g_signal_connect(search_w, "changed", G_CALLBACK(on_search_changed), ctx);
    g_signal_connect(sort_w, "clicked", G_CALLBACK(on_sort_clicked), ctx);
    g_signal_connect(new_entry_w, "activate", G_CALLBACK(on_new_entry_activate), ctx);
    g_signal_connect(add_w, "clicked", G_CALLBACK(on_add_clicked), ctx);
    g_signal_connect(del_w, "clicked", G_CALLBACK(on_del_clicked), ctx);

    GtkEventController *key_ctl = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(key_ctl, GTK_PHASE_CAPTURE);
    g_signal_connect(key_ctl, "key-pressed", G_CALLBACK(on_key_pressed), ctx);
    gtk_widget_add_controller(win_w, key_ctl);

    g_signal_connect(win_w, "close-request", G_CALLBACK(on_close_request), ctx);
    g_signal_connect(win_w, "show", G_CALLBACK(on_win_show), ctx);
    g_object_set_data_full(G_OBJECT(win_w), "picker-ctx", ctx, picker_ctx_free);

    switch_tab(ctx, initial_tab);
    gtk_window_present(win);
    return win;
}
