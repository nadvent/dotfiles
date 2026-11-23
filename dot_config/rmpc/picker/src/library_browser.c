#include "library_browser.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <string.h>

#define COVER_LRU_CAP 256
#define GRID_THUMB_SIZE 132
#define LIST_THUMB_SIZE 36
#define MAX_GRID_ITEMS 60
#define MAX_TRACK_ITEMS 80

typedef struct {
    char *key;
    GdkTexture *tex;
    GList *lru_link;
} CoverEntry;

typedef struct {
    char *key;
    char *song_uri;
    char *thumb_path;
    char *music_dir;
    GBytes *rgba_bytes;
    int width;
    int height;
    int rowstride;
    bool has_alpha;
    PickerCaches *caches;
} ArtDecodeJob;

struct _PickerCaches {
    LibIndex *lib_index;
    char *music_dir;
    char *thumb_dir;
    bool compact_mode;

    GHashTable *tex_map;    /* key -> CoverEntry* (main-thread only) */
    GQueue *lru_queue;      /* element-type: CoverEntry* (head = MRU, tail = LRU) */
    GHashTable *inflight;   /* key -> GPtrArray* of GWeakRef* (main-thread only) */
    GThreadPool *art_pool;
    bool shutting_down;
};

static void cover_entry_free(CoverEntry *e) {
    if (!e) return;
    g_free(e->key);
    if (e->tex) g_object_unref(e->tex);
    g_free(e);
}

static void weak_ref_heap_free(gpointer p) {
    GWeakRef *wr = (GWeakRef *)p;
    if (!wr) return;
    g_weak_ref_clear(wr);
    g_free(wr);
}

static void art_worker_func(gpointer data, gpointer user_data);

PickerCaches *picker_caches_new(void) {
    PickerCaches *c = g_new0(PickerCaches, 1);
    c->music_dir = mpd_resolve_music_dir();
    const char *xdg_cache = g_getenv("XDG_CACHE_HOME");
    if (xdg_cache && *xdg_cache) {
        c->thumb_dir = g_build_filename(xdg_cache, "rmpc", "picker-thumbs", NULL);
    } else {
        c->thumb_dir = g_strdup("/tmp/rmpc/picker-thumbs");
    }
    g_mkdir_with_parents(c->thumb_dir, 0755);

    c->tex_map = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, (GDestroyNotify)cover_entry_free);
    c->lru_queue = g_queue_new();
    c->inflight = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
    c->art_pool = g_thread_pool_new(art_worker_func, NULL, 2, FALSE, NULL);
    return c;
}

void picker_caches_free(PickerCaches *c) {
    if (!c) return;
    c->shutting_down = true;
    if (c->art_pool) {
        g_thread_pool_free(c->art_pool, TRUE, TRUE);
        c->art_pool = NULL;
    }
    if (c->inflight) {
        g_hash_table_destroy(c->inflight);
        c->inflight = NULL;
    }
    if (c->lru_queue) {
        g_queue_free(c->lru_queue);
        c->lru_queue = NULL;
    }
    if (c->tex_map) {
        g_hash_table_destroy(c->tex_map);
        c->tex_map = NULL;
    }
    if (c->lib_index) {
        lib_index_free(c->lib_index);
        c->lib_index = NULL;
    }
    g_free(c->music_dir);
    g_free(c->thumb_dir);
    g_free(c);
}

LibIndex *picker_caches_get_library(PickerCaches *caches, struct mpd_connection *conn, bool force_refresh, GError **err) {
    if (!caches || !conn) return NULL;
    unsigned long cur_db_time = mpd_get_db_update_time(conn);
    if (!force_refresh && caches->lib_index &&
        (cur_db_time == 0 || caches->lib_index->db_update_time == cur_db_time)) {
        mpd_library_index_refresh_favs(conn, caches->lib_index);
        return caches->lib_index;
    }
    LibIndex *fresh = mpd_fetch_library_index(conn, err);
    if (!fresh) return caches->lib_index;
    if (caches->lib_index) lib_index_free(caches->lib_index);
    caches->lib_index = fresh;
    return caches->lib_index;
}

bool picker_caches_get_compact_mode(PickerCaches *caches) {
    return caches ? caches->compact_mode : false;
}

void picker_caches_set_compact_mode(PickerCaches *caches, bool compact) {
    if (caches) caches->compact_mode = compact;
}

static GdkTexture *lru_lookup(PickerCaches *c, const char *key) {
    CoverEntry *e = (CoverEntry *)g_hash_table_lookup(c->tex_map, key);
    if (!e) return NULL;
    g_queue_unlink(c->lru_queue, e->lru_link);
    g_queue_push_head_link(c->lru_queue, e->lru_link);
    return e->tex;
}

static void lru_insert(PickerCaches *c, const char *key, GdkTexture *tex) {
    if (!c || !key || !tex || c->shutting_down) return;
    CoverEntry *existing = (CoverEntry *)g_hash_table_lookup(c->tex_map, key);
    if (existing) {
        g_object_unref(existing->tex);
        existing->tex = g_object_ref(tex);
        g_queue_unlink(c->lru_queue, existing->lru_link);
        g_queue_push_head_link(c->lru_queue, existing->lru_link);
        return;
    }

    while (g_queue_get_length(c->lru_queue) >= COVER_LRU_CAP) {
        GList *tail = g_queue_peek_tail_link(c->lru_queue);
        if (!tail) break;
        CoverEntry *victim = (CoverEntry *)tail->data;
        g_queue_delete_link(c->lru_queue, tail);
        g_hash_table_remove(c->tex_map, victim->key);
    }

    CoverEntry *e = g_new0(CoverEntry, 1);
    e->key = g_strdup(key);
    e->tex = g_object_ref(tex);
    g_queue_push_head(c->lru_queue, e);
    e->lru_link = g_queue_peek_head_link(c->lru_queue);
    g_hash_table_insert(c->tex_map, e->key, e);
}

static void on_loader_size_prepared(GdkPixbufLoader *loader, int width, int height, gpointer user_data) {
    (void)user_data;
    if (width <= 0 || height <= 0) return;
    int max_dim = (width > height) ? width : height;
    if (max_dim > GRID_THUMB_SIZE) {
        double scale = (double)GRID_THUMB_SIZE / (double)max_dim;
        int tw = (int)(width * scale);
        int th = (int)(height * scale);
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;
        gdk_pixbuf_loader_set_size(loader, tw, th);
    }
}

static gboolean finish_art_job_main(gpointer user_data) {
    ArtDecodeJob *job = (ArtDecodeJob *)user_data;
    PickerCaches *c = job->caches;

    if (!c || c->shutting_down) {
        if (job->rgba_bytes) g_bytes_unref(job->rgba_bytes);
        g_free(job->key);
        g_free(job->song_uri);
        g_free(job->thumb_path);
        g_free(job->music_dir);
        g_free(job);
        return G_SOURCE_REMOVE;
    }

    GdkTexture *tex = NULL;
    if (job->rgba_bytes && job->width > 0 && job->height > 0) {
        GdkMemoryFormat fmt = job->has_alpha ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8;
        tex = gdk_memory_texture_new(job->width, job->height, fmt, job->rgba_bytes, (gsize)job->rowstride);
        if (tex && job->thumb_path && !g_file_test(job->thumb_path, G_FILE_TEST_EXISTS)) {
            (void)gdk_texture_save_to_png(tex, job->thumb_path);
        }
    } else if (job->thumb_path && g_file_test(job->thumb_path, G_FILE_TEST_IS_REGULAR)) {
        tex = gdk_texture_new_from_filename(job->thumb_path, NULL);
    }

    if (tex) {
        lru_insert(c, job->key, tex);
    }

    GPtrArray *waiters = (GPtrArray *)g_hash_table_lookup(c->inflight, job->key);
    if (waiters && tex) {
        for (guint i = 0; i < waiters->len; i++) {
            GWeakRef *wr = (GWeakRef *)g_ptr_array_index(waiters, i);
            GtkPicture *pic = (GtkPicture *)g_weak_ref_get(wr);
            if (pic) {
                gtk_picture_set_paintable(pic, GDK_PAINTABLE(tex));
                gtk_widget_set_visible(GTK_WIDGET(pic), TRUE);
                g_object_unref(pic);
            }
        }
    }
    g_hash_table_remove(c->inflight, job->key);

    if (tex) g_object_unref(tex);
    if (job->rgba_bytes) g_bytes_unref(job->rgba_bytes);
    g_free(job->key);
    g_free(job->song_uri);
    g_free(job->thumb_path);
    g_free(job->music_dir);
    g_free(job);
    return G_SOURCE_REMOVE;
}

static void art_worker_func(gpointer data, gpointer user_data) {
    (void)user_data;
    ArtDecodeJob *job = (ArtDecodeJob *)data;
    if (!job->caches || job->caches->shutting_down) {
        g_idle_add(finish_art_job_main, job);
        return;
    }

    if (job->thumb_path && g_file_test(job->thumb_path, G_FILE_TEST_IS_REGULAR)) {
        g_idle_add(finish_art_job_main, job);
        return;
    }

    GBytes *raw = mpd_fetch_cover_bytes(NULL, job->music_dir, job->song_uri);
    if (raw) {
        GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
        g_signal_connect(loader, "size-prepared", G_CALLBACK(on_loader_size_prepared), NULL);
        gsize sz = 0;
        const guchar *ptr = (const guchar *)g_bytes_get_data(raw, &sz);
        if (gdk_pixbuf_loader_write(loader, ptr, sz, NULL) &&
            gdk_pixbuf_loader_close(loader, NULL)) {
            GdkPixbuf *pb = gdk_pixbuf_loader_get_pixbuf(loader);
            if (pb) {
                int w = gdk_pixbuf_get_width(pb);
                int h = gdk_pixbuf_get_height(pb);
                int rs = gdk_pixbuf_get_rowstride(pb);
                int nchan = gdk_pixbuf_get_n_channels(pb);
                bool has_alpha = gdk_pixbuf_get_has_alpha(pb);
                if (w > 0 && h > 0 && (nchan == 3 || nchan == 4)) {
                    const guchar *pixels = gdk_pixbuf_read_pixels(pb);
                    gsize byte_len = (gsize)h * (gsize)rs;
                    job->rgba_bytes = g_bytes_new(pixels, byte_len);
                    job->width = w;
                    job->height = h;
                    job->rowstride = rs;
                    job->has_alpha = has_alpha;
                }
            }
        }
        g_object_unref(loader);
        g_bytes_unref(raw);
    }

    g_idle_add(finish_art_job_main, job);
}

static char *thumb_key_for_uri(const char *song_uri) {
    char *dir = g_path_get_dirname(song_uri);
    if (!dir || g_strcmp0(dir, ".") == 0) {
        g_free(dir);
        return g_strdup(song_uri);
    }
    return dir;
}

static void request_cover_for_picture(PickerCaches *c, GtkPicture *pic, const char *song_uri) {
    if (!c || !pic || !song_uri || !*song_uri) return;

    char *art_key = thumb_key_for_uri(song_uri);
    GdkTexture *cached = lru_lookup(c, art_key);
    if (cached) {
        gtk_picture_set_paintable(pic, GDK_PAINTABLE(cached));
        gtk_widget_set_visible(GTK_WIDGET(pic), TRUE);
        g_free(art_key);
        return;
    }

    GWeakRef *wr = g_new0(GWeakRef, 1);
    g_weak_ref_init(wr, pic);

    GPtrArray *waiters = (GPtrArray *)g_hash_table_lookup(c->inflight, art_key);
    if (waiters) {
        g_ptr_array_add(waiters, wr);
        g_free(art_key);
        return;
    }

    waiters = g_ptr_array_new_with_free_func(weak_ref_heap_free);
    g_ptr_array_add(waiters, wr);
    g_hash_table_insert(c->inflight, g_strdup(art_key), waiters);

    char *sha = g_compute_checksum_for_string(G_CHECKSUM_SHA256, art_key, -1);
    char *fname = g_strdup_printf("%s_%d.png", sha, GRID_THUMB_SIZE);
    char *thumb_path = g_build_filename(c->thumb_dir, fname, NULL);
    g_free(fname);
    g_free(sha);

    ArtDecodeJob *job = g_new0(ArtDecodeJob, 1);
    job->key = art_key;
    job->song_uri = g_strdup(song_uri);
    job->thumb_path = thumb_path;
    job->music_dir = g_strdup(c->music_dir);
    job->caches = c;

    g_thread_pool_push(c->art_pool, job, NULL);
}

typedef enum {
    SCOPE_ALL = 0,
    SCOPE_ALBUMS = 1,
    SCOPE_TRACKS = 2,
} SearchScope;

typedef enum {
    ITEM_FOLDER = 0,
    ITEM_ALBUM  = 1,
    ITEM_TRACK  = 2,
    ITEM_PARENT = 3,
} ResultItemKind;

typedef struct {
    ResultItemKind kind;
    guint index; /* index into LibIndex->folders or LibIndex->tracks */
    GtkWidget *card_widget;
} ResultItem;

struct _LibraryBrowserCtx {
    PickerCaches *caches;
    char *pid_str;
    LibSendToPlaylistsCb on_send_playlists;
    LibPlayDoneCb on_play_done;
    gpointer user_data;

    GtkWidget *root_box;
    GtkEntry *search_entry;
    GtkButton *view_toggle_btn;
    GtkButton *refresh_btn;
    GtkBox *nav_bar;
    GtkBox *scope_bar;
    GtkButton *scope_all_btn;
    GtkButton *scope_alb_btn;
    GtkButton *scope_trk_btn;
    GtkLabel *status_lbl;

    GtkScrolledWindow *scrolled;
    GtkBox *scroll_content;
    GtkBox *album_banner;
    GtkLabel *folders_hdr;
    GtkFlowBox *grid_box;
    GtkListBox *compact_box;
    GtkLabel *tracks_hdr;
    GtkListBox *tracks_box;
    GtkLabel *empty_lbl;

    LibIndex *lib;
    guint current_folder_idx;
    gint detail_album_idx; /* >= 0 when viewing single album tracklist */
    SearchScope scope;
    bool compact_mode;
    bool search_mode;

    GPtrArray *displayed_folders; /* ResultItem* (in grid_box or compact_box) */
    GPtrArray *displayed_tracks;  /* ResultItem* (in tracks_box) */
    int active_zone; /* 0 = folder/album section, 1 = track section */
    int active_folder_cursor;
    int active_track_cursor;
    guint search_debounce_id;
};

static void result_item_free(gpointer p) {
    g_free(p);
}

static void on_browser_destroy(GtkWidget *w, gpointer user_data) {
    (void)w;
    LibraryBrowserCtx *ctx = (LibraryBrowserCtx *)user_data;
    if (!ctx) return;
    if (ctx->search_debounce_id > 0) {
        g_source_remove(ctx->search_debounce_id);
        ctx->search_debounce_id = 0;
    }
    if (ctx->displayed_folders) g_ptr_array_unref(ctx->displayed_folders);
    if (ctx->displayed_tracks) g_ptr_array_unref(ctx->displayed_tracks);
    g_free(ctx->pid_str);
    g_free(ctx);
}

static void enter_search_mode(LibraryBrowserCtx *ctx) {
    if (!ctx) return;
    ctx->search_mode = true;
    gtk_widget_set_can_focus(GTK_WIDGET(ctx->search_entry), TRUE);
    gtk_widget_remove_css_class(GTK_WIDGET(ctx->search_entry), "search-inactive");
    gtk_widget_grab_focus(GTK_WIDGET(ctx->search_entry));
    gtk_editable_set_position(GTK_EDITABLE(ctx->search_entry), -1);
}

void library_browser_exit_search_mode(LibraryBrowserCtx *ctx) {
    if (!ctx) return;
    ctx->search_mode = false;
    gtk_widget_set_can_focus(GTK_WIDGET(ctx->search_entry), FALSE);
    gtk_widget_add_css_class(GTK_WIDGET(ctx->search_entry), "search-inactive");
    gtk_widget_grab_focus(GTK_WIDGET(ctx->scrolled));
}

bool library_browser_is_search_mode(const LibraryBrowserCtx *ctx) {
    return ctx ? ctx->search_mode : false;
}

static void on_search_entry_pressed(
    GtkGestureClick *gesture,
    gint n_press,
    gdouble x,
    gdouble y,
    gpointer user_data
) {
    (void)gesture; (void)n_press; (void)x; (void)y;
    enter_search_mode((LibraryBrowserCtx *)user_data);
}

static void collect_folder_uris_recursive(const LibIndex *idx, const LibFolder *f, GPtrArray *out) {
    if (!idx || !f || !out) return;
    if (f->is_single_album || f->child_dir_indices->len == 0) {
        for (guint i = 0; i < f->track_indices->len; i++) {
            guint tidx = g_array_index(f->track_indices, guint, i);
            const LibTrack *t = (const LibTrack *)g_ptr_array_index(idx->tracks, tidx);
            g_ptr_array_add(out, g_strdup(t->uri));
        }
        return;
    }
    for (guint i = 0; i < f->track_indices->len; i++) {
        guint tidx = g_array_index(f->track_indices, guint, i);
        const LibTrack *t = (const LibTrack *)g_ptr_array_index(idx->tracks, tidx);
        g_ptr_array_add(out, g_strdup(t->uri));
    }
    for (guint i = 0; i < f->child_dir_indices->len; i++) {
        guint cidx = g_array_index(f->child_dir_indices, guint, i);
        const LibFolder *c = (const LibFolder *)g_ptr_array_index(idx->folders, cidx);
        collect_folder_uris_recursive(idx, c, out);
    }
}

static void format_duration(unsigned secs, char *buf, size_t sz) {
    unsigned mins = secs / 60;
    unsigned rem = secs % 60;
    snprintf(buf, sz, "%u:%02u", mins, rem);
}

static const ResultItem *current_selected_item(LibraryBrowserCtx *ctx) {
    if (ctx->active_zone == 0 && ctx->active_folder_cursor >= 0 &&
        (guint)ctx->active_folder_cursor < ctx->displayed_folders->len) {
        return (const ResultItem *)g_ptr_array_index(ctx->displayed_folders, (guint)ctx->active_folder_cursor);
    }
    if (ctx->active_zone == 1 && ctx->active_track_cursor >= 0 &&
        (guint)ctx->active_track_cursor < ctx->displayed_tracks->len) {
        return (const ResultItem *)g_ptr_array_index(ctx->displayed_tracks, (guint)ctx->active_track_cursor);
    }
    if (ctx->displayed_folders->len > 0) {
        return (const ResultItem *)g_ptr_array_index(ctx->displayed_folders, 0);
    }
    if (ctx->displayed_tracks->len > 0) {
        return (const ResultItem *)g_ptr_array_index(ctx->displayed_tracks, 0);
    }
    return NULL;
}

static GPtrArray *uris_for_item(LibraryBrowserCtx *ctx, const ResultItem *item, char **out_label, char **out_tip) {
    if (out_label) *out_label = NULL;
    if (out_tip) *out_tip = NULL;
    if (!ctx || !ctx->lib || !item || item->kind == ITEM_PARENT) return NULL;

    GPtrArray *uris = g_ptr_array_new_with_free_func(g_free);
    if (item->kind == ITEM_TRACK) {
        const LibTrack *t = (const LibTrack *)g_ptr_array_index(ctx->lib->tracks, item->index);
        g_ptr_array_add(uris, g_strdup(t->uri));
        if (out_label) {
            *out_label = (*t->artist && *t->title)
                ? g_strdup_printf("%s - %s", t->artist, t->title)
                : g_strdup(t->title);
        }
        if (out_tip) *out_tip = g_strdup(*out_label);
    } else {
        const LibFolder *f = (const LibFolder *)g_ptr_array_index(ctx->lib->folders, item->index);
        collect_folder_uris_recursive(ctx->lib, f, uris);
        if (out_label) {
            const char *title = (f->is_single_album && f->album_name && *f->album_name) ? f->album_name : f->name;
            *out_label = g_strdup_printf("%s (%u tracks)", title, uris->len);
        }
        if (out_tip) {
            *out_tip = g_strdup(f->path && *f->path ? f->path : f->name);
        }
    }
    return uris;
}

static void execute_queue_action(LibraryBrowserCtx *ctx, const ResultItem *item, int mode) {
    char *lbl = NULL;
    GPtrArray *uris = uris_for_item(ctx, item, &lbl, NULL);
    if (!uris || uris->len == 0) {
        if (uris) g_ptr_array_unref(uris);
        g_free(lbl);
        return;
    }

    GError *err = NULL;
    struct mpd_connection *conn = mpd_open_configured(&err);
    if (!conn) {
        char *msg = g_strdup_printf("Search error: %s", err ? err->message : "MPD connection failed");
        rmpc_notify(ctx->pid_str, "error", 3000, msg);
        g_free(msg);
        g_clear_error(&err);
        g_ptr_array_unref(uris);
        g_free(lbl);
        return;
    }

    bool ok = false;
    const char *verb = "Queued";
    if (mode == 0) {
        ok = mpd_append_uris(conn, uris, &err);
        verb = "Queued";
    } else if (mode == 1) {
        ok = mpd_insert_uris_next(conn, uris, &err);
        verb = "Queued next";
    } else if (mode == 2) {
        ok = mpd_append_and_play_uris(conn, uris, &err);
        verb = "Playing";
    } else {
        ok = mpd_replace_and_play_uris(conn, uris, &err);
        verb = "Playing (replaced)";
    }

    if (!ok) {
        char *msg = g_strdup_printf("Queue error: %s", err ? err->message : "failed");
        rmpc_notify(ctx->pid_str, "error", 3000, msg);
        g_free(msg);
    } else {
        char *msg = g_strdup_printf("%s: %s", verb, lbl ? lbl : "");
        rmpc_notify(ctx->pid_str, "info", 2800, msg);
        g_free(msg);
    }

    g_clear_error(&err);
    mpd_connection_free(conn);
    g_ptr_array_unref(uris);
    g_free(lbl);

    if (ok && (mode == 2 || mode == 3) && ctx->on_play_done) {
        ctx->on_play_done(mode == 2 ? LIB_ACTION_APPEND_AND_PLAY : LIB_ACTION_REPLACE_AND_PLAY, ctx->user_data);
    }
}

static void toggle_item_favourite(LibraryBrowserCtx *ctx, const ResultItem *item) {
    GPtrArray *uris = uris_for_item(ctx, item, NULL, NULL);
    if (!uris || uris->len == 0) {
        if (uris) g_ptr_array_unref(uris);
        return;
    }
    GError *err = NULL;
    struct mpd_connection *conn = mpd_open_configured(&err);
    if (!conn) {
        g_clear_error(&err);
        g_ptr_array_unref(uris);
        return;
    }
    char *action = NULL;
    int count = 0;
    if (mpd_toggle_favourites(conn, uris, &action, &count, &err)) {
        if (g_strcmp0(action, "added") == 0) {
            rmpc_notify(ctx->pid_str, "info", 2500, "♥ Added to favourites");
        } else if (g_strcmp0(action, "removed") == 0) {
            rmpc_notify(ctx->pid_str, "info", 2500, "Removed from favourites");
        }
        mpd_library_index_refresh_favs(conn, ctx->lib);
    }
    g_free(action);
    g_clear_error(&err);
    mpd_connection_free(conn);
    g_ptr_array_unref(uris);
}

static void send_item_to_playlists(LibraryBrowserCtx *ctx, const ResultItem *item) {
    if (!ctx->on_send_playlists || !item || item->kind == ITEM_PARENT) return;
    char *lbl = NULL;
    char *tip = NULL;
    GPtrArray *uris = uris_for_item(ctx, item, &lbl, &tip);
    if (!uris || uris->len == 0) {
        if (uris) g_ptr_array_unref(uris);
        g_free(lbl);
        g_free(tip);
        return;
    }
    ctx->on_send_playlists(uris, lbl, tip, ctx->user_data);
    g_ptr_array_unref(uris);
    g_free(lbl);
    g_free(tip);
}

static void rebuild_view(LibraryBrowserCtx *ctx);

static void sync_selection_visuals(LibraryBrowserCtx *ctx, bool ensure_visible) {
    if (ctx->displayed_folders->len > 0) {
        if (ctx->active_folder_cursor < 0) ctx->active_folder_cursor = 0;
        if ((guint)ctx->active_folder_cursor >= ctx->displayed_folders->len) {
            ctx->active_folder_cursor = (int)ctx->displayed_folders->len - 1;
        }
    } else {
        ctx->active_folder_cursor = -1;
    }

    if (ctx->displayed_tracks->len > 0) {
        if (ctx->active_track_cursor < 0) ctx->active_track_cursor = 0;
        if ((guint)ctx->active_track_cursor >= ctx->displayed_tracks->len) {
            ctx->active_track_cursor = (int)ctx->displayed_tracks->len - 1;
        }
    } else {
        ctx->active_track_cursor = -1;
    }

    if (ctx->active_zone == 0 && ctx->active_folder_cursor < 0 && ctx->active_track_cursor >= 0) {
        ctx->active_zone = 1;
    } else if (ctx->active_zone == 1 && ctx->active_track_cursor < 0 && ctx->active_folder_cursor >= 0) {
        ctx->active_zone = 0;
    }

    GtkWidget *target_widget = NULL;

    for (guint i = 0; i < ctx->displayed_folders->len; i++) {
        ResultItem *it = (ResultItem *)g_ptr_array_index(ctx->displayed_folders, i);
        bool sel = (ctx->active_zone == 0 && (int)i == ctx->active_folder_cursor);
        if (ctx->compact_mode) {
            if (sel) {
                gtk_list_box_select_row(ctx->compact_box, GTK_LIST_BOX_ROW(it->card_widget));
                target_widget = it->card_widget;
            }
        } else {
            if (sel) {
                gtk_flow_box_select_child(ctx->grid_box, GTK_FLOW_BOX_CHILD(it->card_widget));
                target_widget = it->card_widget;
            }
        }
    }
    if (ctx->active_zone != 0) {
        if (ctx->compact_mode) gtk_list_box_unselect_all(ctx->compact_box);
        else gtk_flow_box_unselect_all(ctx->grid_box);
    }

    if (ctx->active_zone == 1 && ctx->active_track_cursor >= 0) {
        ResultItem *it = (ResultItem *)g_ptr_array_index(ctx->displayed_tracks, (guint)ctx->active_track_cursor);
        gtk_list_box_select_row(ctx->tracks_box, GTK_LIST_BOX_ROW(it->card_widget));
        target_widget = it->card_widget;
    } else {
        gtk_list_box_unselect_all(ctx->tracks_box);
    }

    if (ensure_visible && target_widget) {
        graphene_rect_t rect;
        if (gtk_widget_compute_bounds(target_widget, GTK_WIDGET(ctx->scroll_content), &rect)) {
            GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(ctx->scrolled);
            double top = rect.origin.y;
            double bottom = top + rect.size.height;
            double val = gtk_adjustment_get_value(vadj);
            double page = gtk_adjustment_get_page_size(vadj);
            if (page > 0.0) {
                if (top < val) gtk_adjustment_set_value(vadj, top);
                else if (bottom > val + page) gtk_adjustment_set_value(vadj, bottom - page);
            }
        }
    }
}

static void activate_result_item(LibraryBrowserCtx *ctx, const ResultItem *item) {
    if (!item) return;
    if (item->kind == ITEM_PARENT || item->kind == ITEM_FOLDER) {
        ctx->current_folder_idx = item->index;
        ctx->detail_album_idx = -1;
        gtk_editable_set_text(GTK_EDITABLE(ctx->search_entry), "");
        rebuild_view(ctx);
    } else if (item->kind == ITEM_ALBUM) {
        ctx->detail_album_idx = (gint)item->index;
        rebuild_view(ctx);
    } else {
        execute_queue_action(ctx, item, 2);
    }
}

typedef struct {
    LibraryBrowserCtx *ctx;
    ResultItem *item;
} CardClickData;

static void card_click_free(gpointer p, GClosure *cl) {
    (void)cl;
    g_free(p);
}

static void on_folder_card_pressed(
    GtkGestureClick *g,
    gint n_press,
    gdouble x,
    gdouble y,
    gpointer user_data
) {
    (void)g; (void)n_press; (void)x; (void)y;
    CardClickData *d = (CardClickData *)user_data;
    LibraryBrowserCtx *ctx = d->ctx;
    for (guint i = 0; i < ctx->displayed_folders->len; i++) {
        if (g_ptr_array_index(ctx->displayed_folders, i) == d->item) {
            ctx->active_zone = 0;
            ctx->active_folder_cursor = (int)i;
            break;
        }
    }
    sync_selection_visuals(ctx, FALSE);
    activate_result_item(ctx, d->item);
}

static void on_track_row_pressed(
    GtkGestureClick *g,
    gint n_press,
    gdouble x,
    gdouble y,
    gpointer user_data
) {
    (void)g; (void)x; (void)y;
    CardClickData *d = (CardClickData *)user_data;
    LibraryBrowserCtx *ctx = d->ctx;
    for (guint i = 0; i < ctx->displayed_tracks->len; i++) {
        if (g_ptr_array_index(ctx->displayed_tracks, i) == d->item) {
            ctx->active_zone = 1;
            ctx->active_track_cursor = (int)i;
            break;
        }
    }
    sync_selection_visuals(ctx, FALSE);
    if (n_press == 2) {
        execute_queue_action(ctx, d->item, 2);
    }
}

static void on_crumb_clicked(GtkButton *btn, gpointer user_data) {
    LibraryBrowserCtx *ctx = (LibraryBrowserCtx *)user_data;
    guint target_idx = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(btn), "folder-idx"));
    ctx->current_folder_idx = target_idx;
    ctx->detail_album_idx = -1;
    rebuild_view(ctx);
}

static void on_back_from_detail(GtkButton *btn, gpointer user_data) {
    (void)btn;
    LibraryBrowserCtx *ctx = (LibraryBrowserCtx *)user_data;
    ctx->detail_album_idx = -1;
    rebuild_view(ctx);
}

static void on_banner_action(GtkButton *btn, gpointer user_data) {
    LibraryBrowserCtx *ctx = (LibraryBrowserCtx *)user_data;
    if (ctx->detail_album_idx < 0 || !ctx->lib) return;
    int action = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "banner-action"));
    ResultItem tmp = { .kind = ITEM_ALBUM, .index = (guint)ctx->detail_album_idx, .card_widget = NULL };
    if (action == 4) {
        send_item_to_playlists(ctx, &tmp);
    } else if (action == 5) {
        toggle_item_favourite(ctx, &tmp);
        rebuild_view(ctx);
    } else {
        execute_queue_action(ctx, &tmp, action);
    }
}

static GtkWidget *make_cover_box(
    LibraryBrowserCtx *ctx,
    const char *art_uri,
    int size_px,
    bool is_album,
    bool is_parent
) {
    GtkWidget *frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(frame, size_px, size_px);
    gtk_widget_set_halign(frame, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(frame, GTK_ALIGN_CENTER);
    gtk_widget_set_overflow(frame, GTK_OVERFLOW_HIDDEN);
    gtk_widget_add_css_class(frame, is_parent ? "folder-box-parent" : (is_album ? "cover-box" : "folder-box"));

    if (is_album && art_uri && *art_uri) {
        GtkWidget *overlay = gtk_overlay_new();
        gtk_widget_set_size_request(overlay, size_px, size_px);

        GtkWidget *ph = gtk_label_new("󰎇");
        gtk_widget_add_css_class(ph, size_px >= 80 ? "cover-placeholder-lg" : "cover-placeholder-sm");
        gtk_overlay_set_child(GTK_OVERLAY(overlay), ph);

        GtkWidget *pic = gtk_picture_new();
        gtk_picture_set_content_fit(GTK_PICTURE(pic), GTK_CONTENT_FIT_COVER);
        gtk_widget_set_size_request(pic, size_px, size_px);
        gtk_widget_set_visible(pic, FALSE);
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay), pic);

        gtk_box_append(GTK_BOX(frame), overlay);
        request_cover_for_picture(ctx->caches, GTK_PICTURE(pic), art_uri);
    } else {
        const char *glyph = is_parent ? "󰉖" : (is_album ? "󰀥" : "󰉋");
        GtkWidget *icon = gtk_label_new(glyph);
        gtk_widget_set_vexpand(icon, TRUE);
        gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
        if (is_parent) {
            gtk_widget_add_css_class(icon, size_px >= 80 ? "folder-icon-parent-lg" : "folder-icon-parent-sm");
        } else {
            gtk_widget_add_css_class(icon, size_px >= 80 ? "folder-icon-lg" : "folder-icon-sm");
        }
        gtk_box_append(GTK_BOX(frame), icon);
    }

    return frame;
}

static void append_parent_grid_card(LibraryBrowserCtx *ctx, guint parent_idx) {
    const LibFolder *p = (const LibFolder *)g_ptr_array_index(ctx->lib->folders, parent_idx);

    GtkWidget *child = gtk_flow_box_child_new();
    gtk_widget_add_css_class(child, "grid-card-child");
    gtk_widget_add_css_class(child, "grid-card-parent");

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(vbox, "grid-card");
    gtk_flow_box_child_set_child(GTK_FLOW_BOX_CHILD(child), vbox);

    GtkWidget *cover = make_cover_box(ctx, NULL, GRID_THUMB_SIZE, false, true);
    gtk_box_append(GTK_BOX(vbox), cover);

    GtkWidget *title = gtk_label_new(p->name);
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(title), 18);
    gtk_widget_add_css_class(title, "card-title-parent");
    gtk_box_append(GTK_BOX(vbox), title);

    GtkWidget *sub = gtk_label_new("↑ parent folder");
    gtk_label_set_xalign(GTK_LABEL(sub), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(sub), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(sub, "card-subtitle-parent");
    gtk_box_append(GTK_BOX(vbox), sub);

    char *tip = g_strdup_printf("Go up to %s", p->name);
    gtk_widget_set_tooltip_text(child, tip);
    g_free(tip);

    ResultItem *ri = g_new0(ResultItem, 1);
    ri->kind = ITEM_PARENT;
    ri->index = parent_idx;
    ri->card_widget = child;
    g_ptr_array_add(ctx->displayed_folders, ri);

    GtkGesture *click = gtk_gesture_click_new();
    CardClickData *cd = g_new(CardClickData, 1);
    cd->ctx = ctx;
    cd->item = ri;
    g_signal_connect_data(click, "pressed", G_CALLBACK(on_folder_card_pressed), cd, card_click_free, 0);
    gtk_widget_add_controller(child, GTK_EVENT_CONTROLLER(click));

    gtk_flow_box_append(ctx->grid_box, child);
}

static void append_parent_compact_row(LibraryBrowserCtx *ctx, guint parent_idx) {
    const LibFolder *p = (const LibFolder *)g_ptr_array_index(ctx->lib->folders, parent_idx);

    GtkWidget *row = gtk_list_box_row_new();
    gtk_widget_set_focus_on_click(row, FALSE);
    gtk_widget_add_css_class(row, "row-parent");
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), hbox);

    GtkWidget *thumb = make_cover_box(ctx, NULL, LIST_THUMB_SIZE, false, true);
    gtk_box_append(GTK_BOX(hbox), thumb);

    GtkWidget *meta = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_widget_set_hexpand(meta, TRUE);
    GtkWidget *title = gtk_label_new(p->name);
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(title, "card-title-parent");

    GtkWidget *sub = gtk_label_new("↑ parent folder");
    gtk_label_set_xalign(GTK_LABEL(sub), 0.0f);
    gtk_widget_add_css_class(sub, "card-subtitle-parent");

    gtk_box_append(GTK_BOX(meta), title);
    gtk_box_append(GTK_BOX(meta), sub);
    gtk_box_append(GTK_BOX(hbox), meta);

    GtkWidget *up_lbl = gtk_label_new("..");
    gtk_widget_add_css_class(up_lbl, "row-count");
    gtk_box_append(GTK_BOX(hbox), up_lbl);

    ResultItem *ri = g_new0(ResultItem, 1);
    ri->kind = ITEM_PARENT;
    ri->index = parent_idx;
    ri->card_widget = row;
    g_ptr_array_add(ctx->displayed_folders, ri);

    GtkGesture *click = gtk_gesture_click_new();
    CardClickData *cd = g_new(CardClickData, 1);
    cd->ctx = ctx;
    cd->item = ri;
    g_signal_connect_data(click, "pressed", G_CALLBACK(on_folder_card_pressed), cd, card_click_free, 0);
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(click));

    gtk_list_box_append(ctx->compact_box, row);
}

static void append_folder_grid_card(LibraryBrowserCtx *ctx, guint folder_idx) {
    const LibFolder *f = (const LibFolder *)g_ptr_array_index(ctx->lib->folders, folder_idx);

    GtkWidget *child = gtk_flow_box_child_new();
    gtk_widget_add_css_class(child, "grid-card-child");

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(vbox, "grid-card");
    gtk_flow_box_child_set_child(GTK_FLOW_BOX_CHILD(child), vbox);

    GtkWidget *cover = make_cover_box(ctx, f->art_uri, GRID_THUMB_SIZE, f->is_single_album, false);
    gtk_box_append(GTK_BOX(vbox), cover);

    const char *title_txt = (f->is_single_album && f->album_name && *f->album_name) ? f->album_name : f->name;
    GtkWidget *title = gtk_label_new(title_txt);
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(title), 18);
    gtk_widget_add_css_class(title, f->is_single_album ? "card-title-album" : "card-title-folder");
    gtk_box_append(GTK_BOX(vbox), title);

    char *sub_txt = NULL;
    if (f->is_single_album) {
        const char *art = (f->artist_name && *f->artist_name) ? f->artist_name : "Album";
        if (f->date && *f->date) {
            sub_txt = g_strdup_printf("%s · %s", art, f->date);
        } else {
            sub_txt = g_strdup_printf("%s · %u", art, f->recursive_track_count);
        }
    } else if (f->album_count > 0) {
        sub_txt = g_strdup_printf("%u albums · %u", f->album_count, f->recursive_track_count);
    } else {
        sub_txt = g_strdup_printf("%u tracks", f->recursive_track_count);
    }
    GtkWidget *sub = gtk_label_new(sub_txt);
    g_free(sub_txt);
    gtk_label_set_xalign(GTK_LABEL(sub), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(sub), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(sub), 20);
    gtk_widget_add_css_class(sub, "card-subtitle");
    gtk_box_append(GTK_BOX(vbox), sub);

    char *tip = g_strdup_printf("%s\n%s (%u tracks)", title_txt, f->path, f->recursive_track_count);
    gtk_widget_set_tooltip_text(child, tip);
    g_free(tip);

    ResultItem *ri = g_new0(ResultItem, 1);
    ri->kind = f->is_single_album ? ITEM_ALBUM : ITEM_FOLDER;
    ri->index = folder_idx;
    ri->card_widget = child;
    g_ptr_array_add(ctx->displayed_folders, ri);

    GtkGesture *click = gtk_gesture_click_new();
    CardClickData *cd = g_new(CardClickData, 1);
    cd->ctx = ctx;
    cd->item = ri;
    g_signal_connect_data(click, "pressed", G_CALLBACK(on_folder_card_pressed), cd, card_click_free, 0);
    gtk_widget_add_controller(child, GTK_EVENT_CONTROLLER(click));

    gtk_flow_box_append(ctx->grid_box, child);
}

static void append_folder_compact_row(LibraryBrowserCtx *ctx, guint folder_idx) {
    const LibFolder *f = (const LibFolder *)g_ptr_array_index(ctx->lib->folders, folder_idx);

    GtkWidget *row = gtk_list_box_row_new();
    gtk_widget_set_focus_on_click(row, FALSE);
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), hbox);

    GtkWidget *thumb = make_cover_box(ctx, f->art_uri, LIST_THUMB_SIZE, f->is_single_album, false);
    gtk_box_append(GTK_BOX(hbox), thumb);

    GtkWidget *meta = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_widget_set_hexpand(meta, TRUE);
    const char *title_txt = (f->is_single_album && f->album_name && *f->album_name) ? f->album_name : f->name;
    GtkWidget *title = gtk_label_new(title_txt);
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(title, f->is_single_album ? "row-name-pl" : "card-title-folder");

    char *sub_txt = f->is_single_album
        ? g_strdup_printf("%s%s%s", (f->artist_name && *f->artist_name) ? f->artist_name : "",
                          (f->date && *f->date) ? " · " : "", (f->date && *f->date) ? f->date : "")
        : g_strdup(f->path);
    GtkWidget *sub = gtk_label_new(sub_txt);
    g_free(sub_txt);
    gtk_label_set_xalign(GTK_LABEL(sub), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(sub), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(sub, "card-subtitle");

    gtk_box_append(GTK_BOX(meta), title);
    gtk_box_append(GTK_BOX(meta), sub);
    gtk_box_append(GTK_BOX(hbox), meta);

    if (f->has_fav) {
        GtkWidget *fav = gtk_label_new("♥");
        gtk_widget_add_css_class(fav, "track-fav");
        gtk_box_append(GTK_BOX(hbox), fav);
    }

    char *cnt = f->is_single_album
        ? g_strdup_printf("%u trk", f->recursive_track_count)
        : g_strdup_printf("%u/%u", f->album_count, f->recursive_track_count);
    GtkWidget *cnt_lbl = gtk_label_new(cnt);
    g_free(cnt);
    gtk_widget_add_css_class(cnt_lbl, "row-count");
    gtk_box_append(GTK_BOX(hbox), cnt_lbl);

    ResultItem *ri = g_new0(ResultItem, 1);
    ri->kind = f->is_single_album ? ITEM_ALBUM : ITEM_FOLDER;
    ri->index = folder_idx;
    ri->card_widget = row;
    g_ptr_array_add(ctx->displayed_folders, ri);

    GtkGesture *click = gtk_gesture_click_new();
    CardClickData *cd = g_new(CardClickData, 1);
    cd->ctx = ctx;
    cd->item = ri;
    g_signal_connect_data(click, "pressed", G_CALLBACK(on_folder_card_pressed), cd, card_click_free, 0);
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(click));

    gtk_list_box_append(ctx->compact_box, row);
}

static void append_track_row(LibraryBrowserCtx *ctx, guint track_idx, bool show_thumb) {
    const LibTrack *t = (const LibTrack *)g_ptr_array_index(ctx->lib->tracks, track_idx);

    GtkWidget *row = gtk_list_box_row_new();
    gtk_widget_set_focus_on_click(row, FALSE);
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), hbox);

    if (show_thumb) {
        GtkWidget *thumb = make_cover_box(ctx, t->uri, LIST_THUMB_SIZE, true, false);
        gtk_box_append(GTK_BOX(hbox), thumb);
    } else {
        char num_buf[16];
        if (t->track_num > 0) snprintf(num_buf, sizeof(num_buf), "%02u.", t->track_num);
        else snprintf(num_buf, sizeof(num_buf), "•");
        GtkWidget *num_lbl = gtk_label_new(num_buf);
        gtk_label_set_width_chars(GTK_LABEL(num_lbl), 3);
        gtk_label_set_xalign(GTK_LABEL(num_lbl), 1.0f);
        gtk_widget_add_css_class(num_lbl, "row-count");
        gtk_box_append(GTK_BOX(hbox), num_lbl);
    }

    GtkWidget *meta = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_widget_set_hexpand(meta, TRUE);
    GtkWidget *title = gtk_label_new(t->title);
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(title, "row-name-pl");
    gtk_box_append(GTK_BOX(meta), title);

    if (show_thumb) {
        char *sub_txt = g_strdup_printf(
            "%s%s%s",
            (*t->artist) ? t->artist : "Unknown Artist",
            (*t->album) ? " · " : "",
            (*t->album) ? t->album : ""
        );
        GtkWidget *sub = gtk_label_new(sub_txt);
        g_free(sub_txt);
        gtk_label_set_xalign(GTK_LABEL(sub), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(sub), PANGO_ELLIPSIZE_END);
        gtk_widget_add_css_class(sub, "card-subtitle");
        gtk_box_append(GTK_BOX(meta), sub);
    }
    gtk_box_append(GTK_BOX(hbox), meta);

    if (t->is_fav) {
        GtkWidget *fav = gtk_label_new("♥");
        gtk_widget_add_css_class(fav, "track-fav");
        gtk_box_append(GTK_BOX(hbox), fav);
    }

    char dur_buf[16];
    if (t->duration_s > 0) format_duration(t->duration_s, dur_buf, sizeof(dur_buf));
    else snprintf(dur_buf, sizeof(dur_buf), "--:--");
    GtkWidget *dur_lbl = gtk_label_new(dur_buf);
    gtk_widget_add_css_class(dur_lbl, "row-count");
    gtk_box_append(GTK_BOX(hbox), dur_lbl);

    ResultItem *ri = g_new0(ResultItem, 1);
    ri->kind = ITEM_TRACK;
    ri->index = track_idx;
    ri->card_widget = row;
    g_ptr_array_add(ctx->displayed_tracks, ri);

    GtkGesture *click = gtk_gesture_click_new();
    CardClickData *cd = g_new(CardClickData, 1);
    cd->ctx = ctx;
    cd->item = ri;
    g_signal_connect_data(click, "pressed", G_CALLBACK(on_track_row_pressed), cd, card_click_free, 0);
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(click));

    gtk_list_box_append(ctx->tracks_box, row);
}

static bool token_matches_track(const LibTrack *t, const char *tok_cf) {
    if (g_strcmp0(tok_cf, "fav:") == 0 || g_strcmp0(tok_cf, "is:fav") == 0 || g_strcmp0(tok_cf, ":fav") == 0) {
        return t->is_fav;
    }
    const char *colon = strchr(tok_cf, ':');
    if (colon && colon > tok_cf && colon[1] != '\0') {
        const char *needle = colon + 1;
        size_t plen = (size_t)(colon - tok_cf);
        const char *hay = NULL;
        if ((plen == 1 && tok_cf[0] == 'a') || strncmp(tok_cf, "artist", plen) == 0) {
            char *fa = g_utf8_casefold(t->artist, -1);
            char *faa = g_utf8_casefold(t->album_artist, -1);
            bool ok = (strstr(fa, needle) != NULL) || (strstr(faa, needle) != NULL);
            g_free(fa);
            g_free(faa);
            return ok;
        }
        if ((plen == 1 && tok_cf[0] == 'l') || strncmp(tok_cf, "album", plen) == 0) hay = t->album;
        else if ((plen == 1 && tok_cf[0] == 't') || strncmp(tok_cf, "title", plen) == 0) hay = t->title;
        else if ((plen == 1 && tok_cf[0] == 'g') || strncmp(tok_cf, "genre", plen) == 0) hay = t->genre;
        else if ((plen == 1 && tok_cf[0] == 'y') || strncmp(tok_cf, "date", plen) == 0) hay = t->date;
        else if ((plen == 1 && tok_cf[0] == 'f') || strncmp(tok_cf, "path", plen) == 0) hay = t->dir;
        if (hay) {
            char *cf = g_utf8_casefold(hay, -1);
            bool ok = (strstr(cf, needle) != NULL);
            g_free(cf);
            return ok;
        }
    }
    return strstr(t->search_blob, tok_cf) != NULL;
}

static bool token_matches_folder(const LibFolder *f, const char *tok_cf) {
    if (g_strcmp0(tok_cf, "fav:") == 0 || g_strcmp0(tok_cf, "is:fav") == 0 || g_strcmp0(tok_cf, ":fav") == 0) {
        return f->has_fav;
    }
    const char *colon = strchr(tok_cf, ':');
    if (colon && colon > tok_cf && colon[1] != '\0') {
        const char *needle = colon + 1;
        size_t plen = (size_t)(colon - tok_cf);
        const char *hay = NULL;
        if ((plen == 1 && tok_cf[0] == 'a') || strncmp(tok_cf, "artist", plen) == 0) hay = f->artist_name;
        else if ((plen == 1 && tok_cf[0] == 'l') || strncmp(tok_cf, "album", plen) == 0) hay = f->album_name ? f->album_name : f->name;
        else if ((plen == 1 && tok_cf[0] == 'y') || strncmp(tok_cf, "date", plen) == 0) hay = f->date;
        else if ((plen == 1 && tok_cf[0] == 'f') || strncmp(tok_cf, "path", plen) == 0) hay = f->path;
        else if ((plen == 1 && tok_cf[0] == 't') || (plen == 1 && tok_cf[0] == 'g')) return false;
        if (hay) {
            char *cf = g_utf8_casefold(hay, -1);
            bool ok = (strstr(cf, needle) != NULL);
            g_free(cf);
            return ok;
        }
    }
    return strstr(f->search_blob, tok_cf) != NULL;
}

static void clear_box_children(GtkWidget *box) {
    GtkWidget *child = gtk_widget_get_first_child(box);
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_box_remove(GTK_BOX(box), child);
        child = next;
    }
}

static void rebuild_breadcrumbs(LibraryBrowserCtx *ctx) {
    clear_box_children(GTK_WIDGET(ctx->nav_bar));
    if (!ctx->lib || ctx->current_folder_idx >= ctx->lib->folders->len) return;

    GArray *chain = g_array_new(FALSE, FALSE, sizeof(guint));
    guint walk = ctx->current_folder_idx;
    while (true) {
        g_array_prepend_val(chain, walk);
        const LibFolder *wf = (const LibFolder *)g_ptr_array_index(ctx->lib->folders, walk);
        if (!wf->path || *wf->path == '\0') break;
        gpointer p = NULL;
        if (!g_hash_table_lookup_extended(ctx->lib->folder_by_path, wf->parent_path, NULL, &p)) break;
        walk = GPOINTER_TO_UINT(p);
    }

    for (guint i = 0; i < chain->len; i++) {
        if (i > 0) {
            GtkWidget *sep = gtk_label_new("›");
            gtk_widget_add_css_class(sep, "crumb-sep");
            gtk_box_append(ctx->nav_bar, sep);
        }
        guint fidx = g_array_index(chain, guint, i);
        const LibFolder *cf = (const LibFolder *)g_ptr_array_index(ctx->lib->folders, fidx);
        GtkWidget *btn = gtk_button_new_with_label(cf->name);
        gtk_widget_add_css_class(btn, (i + 1 == chain->len) ? "crumb-active" : "crumb-btn");
        g_object_set_data(G_OBJECT(btn), "folder-idx", GUINT_TO_POINTER(fidx));
        g_signal_connect(btn, "clicked", G_CALLBACK(on_crumb_clicked), ctx);
        gtk_box_append(ctx->nav_bar, btn);
    }
    g_array_unref(chain);
}

static void update_scope_buttons(LibraryBrowserCtx *ctx) {
    GtkButton *btns[3] = { ctx->scope_all_btn, ctx->scope_alb_btn, ctx->scope_trk_btn };
    for (int i = 0; i < 3; i++) {
        if ((int)ctx->scope == i) gtk_widget_add_css_class(GTK_WIDGET(btns[i]), "pill-active");
        else gtk_widget_remove_css_class(GTK_WIDGET(btns[i]), "pill-active");
    }
}

static void rebuild_view(LibraryBrowserCtx *ctx) {
    gtk_flow_box_remove_all(ctx->grid_box);
    gtk_list_box_remove_all(ctx->compact_box);
    gtk_list_box_remove_all(ctx->tracks_box);
    clear_box_children(GTK_WIDGET(ctx->album_banner));
    g_ptr_array_set_size(ctx->displayed_folders, 0);
    g_ptr_array_set_size(ctx->displayed_tracks, 0);

    if (!ctx->lib) {
        gtk_widget_set_visible(GTK_WIDGET(ctx->empty_lbl), TRUE);
        gtk_label_set_text(ctx->empty_lbl, "MPD library unavailable");
        return;
    }

    const char *raw_q = gtk_editable_get_text(GTK_EDITABLE(ctx->search_entry));
    char *q_clean = g_strdup(raw_q ? raw_q : "");
    g_strstrip(q_clean);

    gtk_button_set_label(ctx->view_toggle_btn, ctx->compact_mode ? "⊞ Grid" : "☰ Compact");
    update_scope_buttons(ctx);

    /* Case 1: Album Detail View */
    if (ctx->detail_album_idx >= 0 && (guint)ctx->detail_album_idx < ctx->lib->folders->len) {
        const LibFolder *alb = (const LibFolder *)g_ptr_array_index(ctx->lib->folders, (guint)ctx->detail_album_idx);
        gtk_widget_set_visible(GTK_WIDGET(ctx->nav_bar), FALSE);
        gtk_widget_set_visible(GTK_WIDGET(ctx->scope_bar), FALSE);
        gtk_widget_set_visible(GTK_WIDGET(ctx->album_banner), TRUE);
        gtk_widget_set_visible(GTK_WIDGET(ctx->folders_hdr), FALSE);
        gtk_widget_set_visible(GTK_WIDGET(ctx->grid_box), FALSE);
        gtk_widget_set_visible(GTK_WIDGET(ctx->compact_box), FALSE);
        gtk_widget_set_visible(GTK_WIDGET(ctx->tracks_hdr), FALSE);
        gtk_widget_set_visible(GTK_WIDGET(ctx->tracks_box), TRUE);
        gtk_widget_set_visible(GTK_WIDGET(ctx->empty_lbl), FALSE);

        GtkWidget *top_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget *cover = make_cover_box(ctx, alb->art_uri, 84, true, false);
        gtk_box_append(GTK_BOX(top_row), cover);

        GtkWidget *info = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_widget_set_hexpand(info, TRUE);

        GtkWidget *hdr_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *back_btn = gtk_button_new_with_label("← Back");
        gtk_widget_add_css_class(back_btn, "pill-btn");
        g_signal_connect(back_btn, "clicked", G_CALLBACK(on_back_from_detail), ctx);
        gtk_box_append(GTK_BOX(hdr_row), back_btn);

        const char *title_txt = (alb->album_name && *alb->album_name) ? alb->album_name : alb->name;
        GtkWidget *title = gtk_label_new(title_txt);
        gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
        gtk_widget_add_css_class(title, "picker-title");
        gtk_box_append(GTK_BOX(hdr_row), title);
        gtk_box_append(GTK_BOX(info), hdr_row);

        char *sub_txt = g_strdup_printf(
            "%s%s%s · %u tracks",
            (alb->artist_name && *alb->artist_name) ? alb->artist_name : "Unknown Artist",
            (alb->date && *alb->date) ? " · " : "",
            (alb->date && *alb->date) ? alb->date : "",
            alb->track_indices->len
        );
        GtkWidget *sub = gtk_label_new(sub_txt);
        g_free(sub_txt);
        gtk_label_set_xalign(GTK_LABEL(sub), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(sub), PANGO_ELLIPSIZE_END);
        gtk_widget_add_css_class(sub, "picker-subtitle");
        gtk_box_append(GTK_BOX(info), sub);

        GtkWidget *acts = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
        struct { const char *lbl; int code; const char *css; } defs[] = {
            { "▶ Play", 2, "btn-primary" },
            { "+ Queue", 0, "pill-btn" },
            { "⏭ Next", 1, "pill-btn" },
            { "♫ Playlists", 4, "pill-btn" },
            { alb->has_fav ? "♥" : "♡", 5, "pill-btn" },
        };
        for (int i = 0; i < 5; i++) {
            GtkWidget *b = gtk_button_new_with_label(defs[i].lbl);
            gtk_widget_add_css_class(b, defs[i].css);
            g_object_set_data(G_OBJECT(b), "banner-action", GINT_TO_POINTER(defs[i].code));
            g_signal_connect(b, "clicked", G_CALLBACK(on_banner_action), ctx);
            gtk_box_append(GTK_BOX(acts), b);
        }
        gtk_box_append(GTK_BOX(info), acts);
        gtk_box_append(GTK_BOX(top_row), info);
        gtk_box_append(ctx->album_banner, top_row);

        for (guint i = 0; i < alb->track_indices->len; i++) {
            guint tidx = g_array_index(alb->track_indices, guint, i);
            append_track_row(ctx, tidx, false);
        }
        ctx->active_zone = 1;
        ctx->active_track_cursor = 0;
        gtk_label_set_text(ctx->status_lbl, alb->path);
        g_free(q_clean);
        sync_selection_visuals(ctx, FALSE);
        return;
    }

    gtk_widget_set_visible(GTK_WIDGET(ctx->album_banner), FALSE);

    /* Case 2: Empty query -> hierarchical folder browsing */
    if (*q_clean == '\0') {
        gtk_widget_set_visible(GTK_WIDGET(ctx->nav_bar), TRUE);
        gtk_widget_set_visible(GTK_WIDGET(ctx->scope_bar), FALSE);
        rebuild_breadcrumbs(ctx);

        const LibFolder *cur = (const LibFolder *)g_ptr_array_index(ctx->lib->folders, ctx->current_folder_idx);
        guint shown_dirs = 0;

        /* Prepend parent folder item with muted grey styling if we can go up */
        if (ctx->current_folder_idx != 0) {
            guint parent_idx = 0;
            gpointer pp = NULL;
            if (cur->parent_path &&
                g_hash_table_lookup_extended(ctx->lib->folder_by_path, cur->parent_path, NULL, &pp)) {
                parent_idx = GPOINTER_TO_UINT(pp);
            }
            if (ctx->compact_mode) append_parent_compact_row(ctx, parent_idx);
            else append_parent_grid_card(ctx, parent_idx);
            shown_dirs++;
        }

        for (guint i = 0; i < cur->child_dir_indices->len; i++) {
            guint cidx = g_array_index(cur->child_dir_indices, guint, i);
            if (ctx->compact_mode) append_folder_compact_row(ctx, cidx);
            else append_folder_grid_card(ctx, cidx);
            shown_dirs++;
        }

        guint shown_tracks = 0;
        for (guint i = 0; i < cur->track_indices->len && shown_tracks < MAX_TRACK_ITEMS; i++) {
            guint tidx = g_array_index(cur->track_indices, guint, i);
            append_track_row(ctx, tidx, true);
            shown_tracks++;
        }

        gtk_widget_set_visible(GTK_WIDGET(ctx->folders_hdr), FALSE);
        gtk_widget_set_visible(GTK_WIDGET(ctx->grid_box), !ctx->compact_mode && shown_dirs > 0);
        gtk_widget_set_visible(GTK_WIDGET(ctx->compact_box), ctx->compact_mode && shown_dirs > 0);
        gtk_widget_set_visible(GTK_WIDGET(ctx->tracks_hdr), shown_dirs > 0 && shown_tracks > 0);
        gtk_widget_set_visible(GTK_WIDGET(ctx->tracks_box), shown_tracks > 0);
        gtk_widget_set_visible(GTK_WIDGET(ctx->empty_lbl), shown_dirs == 0 && shown_tracks == 0);
        if (shown_dirs == 0 && shown_tracks == 0) {
            gtk_label_set_text(ctx->empty_lbl, "Empty folder");
        }

        char *st = g_strdup_printf("%u folders · %u direct tracks", cur->child_dir_indices->len, cur->track_indices->len);
        gtk_label_set_text(ctx->status_lbl, st);
        g_free(st);

        ctx->active_zone = (shown_dirs > 0) ? 0 : 1;
        ctx->active_folder_cursor = (shown_dirs > 0) ? 0 : -1;
        ctx->active_track_cursor = (shown_tracks > 0) ? 0 : -1;
        g_free(q_clean);
        sync_selection_visuals(ctx, FALSE);
        return;
    }

    /* Case 3: Global search across folders/albums + tracks */
    gtk_widget_set_visible(GTK_WIDGET(ctx->nav_bar), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(ctx->scope_bar), TRUE);

    char *q_cf = g_utf8_casefold(q_clean, -1);
    gchar **raw_tokens = g_strsplit_set(q_cf, " \t", -1);
    GPtrArray *tokens = g_ptr_array_new();
    for (int i = 0; raw_tokens && raw_tokens[i]; i++) {
        if (*raw_tokens[i]) g_ptr_array_add(tokens, raw_tokens[i]);
    }

    guint folder_matches = 0;
    guint folder_shown = 0;
    if (ctx->scope == SCOPE_ALL || ctx->scope == SCOPE_ALBUMS) {
        for (int pass = 0; pass < 2; pass++) {
            for (guint i = 1; i < ctx->lib->folders->len; i++) {
                const LibFolder *f = (const LibFolder *)g_ptr_array_index(ctx->lib->folders, i);
                bool want_album = (pass == 0);
                if (f->is_single_album != want_album) continue;
                if (ctx->scope == SCOPE_ALBUMS && !f->is_single_album) continue;

                bool ok = true;
                for (guint k = 0; k < tokens->len; k++) {
                    if (!token_matches_folder(f, (const char *)g_ptr_array_index(tokens, k))) {
                        ok = false;
                        break;
                    }
                }
                if (!ok) continue;
                folder_matches++;
                if (folder_shown < MAX_GRID_ITEMS) {
                    if (ctx->compact_mode) append_folder_compact_row(ctx, i);
                    else append_folder_grid_card(ctx, i);
                    folder_shown++;
                }
            }
        }
    }

    guint track_matches = 0;
    guint track_shown = 0;
    if (ctx->scope == SCOPE_ALL || ctx->scope == SCOPE_TRACKS) {
        for (guint i = 0; i < ctx->lib->tracks->len; i++) {
            const LibTrack *t = (const LibTrack *)g_ptr_array_index(ctx->lib->tracks, i);
            bool ok = true;
            for (guint k = 0; k < tokens->len; k++) {
                if (!token_matches_track(t, (const char *)g_ptr_array_index(tokens, k))) {
                    ok = false;
                    break;
                }
            }
            if (!ok) continue;
            track_matches++;
            if (track_shown < MAX_TRACK_ITEMS) {
                append_track_row(ctx, i, true);
                track_shown++;
            }
        }
    }

    g_ptr_array_unref(tokens);
    g_strfreev(raw_tokens);
    g_free(q_cf);
    g_free(q_clean);

    gtk_widget_set_visible(GTK_WIDGET(ctx->folders_hdr), folder_shown > 0);
    if (folder_shown > 0) {
        char *fh = (folder_matches > folder_shown)
            ? g_strdup_printf("Albums & Folders (top %u of %u)", folder_shown, folder_matches)
            : g_strdup_printf("Albums & Folders (%u)", folder_matches);
        gtk_label_set_text(ctx->folders_hdr, fh);
        g_free(fh);
    }

    gtk_widget_set_visible(GTK_WIDGET(ctx->grid_box), !ctx->compact_mode && folder_shown > 0);
    gtk_widget_set_visible(GTK_WIDGET(ctx->compact_box), ctx->compact_mode && folder_shown > 0);

    gtk_widget_set_visible(GTK_WIDGET(ctx->tracks_hdr), track_shown > 0);
    if (track_shown > 0) {
        char *th = (track_matches > track_shown)
            ? g_strdup_printf("Tracks (top %u of %u)", track_shown, track_matches)
            : g_strdup_printf("Tracks (%u)", track_matches);
        gtk_label_set_text(ctx->tracks_hdr, th);
        g_free(th);
    }
    gtk_widget_set_visible(GTK_WIDGET(ctx->tracks_box), track_shown > 0);

    bool empty = (folder_shown == 0 && track_shown == 0);
    gtk_widget_set_visible(GTK_WIDGET(ctx->empty_lbl), empty);
    if (empty) gtk_label_set_text(ctx->empty_lbl, "No matching albums, folders, or tracks");

    char *st = g_strdup_printf("%u albums/folders · %u tracks", folder_matches, track_matches);
    gtk_label_set_text(ctx->status_lbl, st);
    g_free(st);

    ctx->active_zone = (folder_shown > 0) ? 0 : 1;
    ctx->active_folder_cursor = (folder_shown > 0) ? 0 : -1;
    ctx->active_track_cursor = (track_shown > 0) ? 0 : -1;
    sync_selection_visuals(ctx, FALSE);
}

static gboolean on_search_debounced(gpointer user_data) {
    LibraryBrowserCtx *ctx = (LibraryBrowserCtx *)user_data;
    ctx->search_debounce_id = 0;
    ctx->detail_album_idx = -1;
    rebuild_view(ctx);
    return G_SOURCE_REMOVE;
}

static void on_search_changed(GtkEditable *editable, gpointer user_data) {
    (void)editable;
    LibraryBrowserCtx *ctx = (LibraryBrowserCtx *)user_data;
    if (ctx->search_debounce_id > 0) {
        g_source_remove(ctx->search_debounce_id);
    }
    ctx->search_debounce_id = g_timeout_add(60, on_search_debounced, ctx);
}

static void on_view_toggle_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    LibraryBrowserCtx *ctx = (LibraryBrowserCtx *)user_data;
    ctx->compact_mode = !ctx->compact_mode;
    picker_caches_set_compact_mode(ctx->caches, ctx->compact_mode);
    rebuild_view(ctx);
}

static void refresh_library(LibraryBrowserCtx *ctx, bool force) {
    GError *err = NULL;
    struct mpd_connection *conn = mpd_open_configured(&err);
    if (!conn) {
        g_clear_error(&err);
        return;
    }
    ctx->lib = picker_caches_get_library(ctx->caches, conn, force, &err);
    g_clear_error(&err);
    mpd_connection_free(conn);
    if (ctx->lib && ctx->current_folder_idx >= ctx->lib->folders->len) {
        ctx->current_folder_idx = 0;
    }
    rebuild_view(ctx);
}

static void on_refresh_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    refresh_library((LibraryBrowserCtx *)user_data, true);
}

static void on_scope_clicked(GtkButton *btn, gpointer user_data) {
    LibraryBrowserCtx *ctx = (LibraryBrowserCtx *)user_data;
    ctx->scope = (SearchScope)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "scope-val"));
    rebuild_view(ctx);
}

void library_browser_on_activate(LibraryBrowserCtx *ctx) {
    if (!ctx) return;
    ctx->compact_mode = picker_caches_get_compact_mode(ctx->caches);
    library_browser_exit_search_mode(ctx);
    refresh_library(ctx, false);
}

static void step_up_directory(LibraryBrowserCtx *ctx) {
    if (ctx->detail_album_idx >= 0) {
        ctx->detail_album_idx = -1;
        rebuild_view(ctx);
        return;
    }
    if (!ctx->lib || ctx->current_folder_idx == 0 || ctx->current_folder_idx >= ctx->lib->folders->len) {
        return;
    }
    const LibFolder *cur = (const LibFolder *)g_ptr_array_index(ctx->lib->folders, ctx->current_folder_idx);
    gpointer p = NULL;
    if (g_hash_table_lookup_extended(ctx->lib->folder_by_path, cur->parent_path, NULL, &p)) {
        ctx->current_folder_idx = GPOINTER_TO_UINT(p);
    } else {
        ctx->current_folder_idx = 0;
    }
    rebuild_view(ctx);
}

static void move_browser_cursor(LibraryBrowserCtx *ctx, int dx, int dy) {
    if (ctx->active_zone == 0 && ctx->displayed_folders->len > 0) {
        int cols = ctx->compact_mode ? 1 : 3;
        int cur = ctx->active_folder_cursor;
        if (cur < 0) cur = 0;
        int nxt = cur + dx + (dy * cols);
        if (nxt < 0) nxt = 0;
        if ((guint)nxt >= ctx->displayed_folders->len) {
            if (dy > 0 && ctx->displayed_tracks->len > 0) {
                ctx->active_zone = 1;
                ctx->active_track_cursor = 0;
                sync_selection_visuals(ctx, TRUE);
                return;
            }
            nxt = (int)ctx->displayed_folders->len - 1;
        }
        ctx->active_folder_cursor = nxt;
        sync_selection_visuals(ctx, TRUE);
    } else if (ctx->displayed_tracks->len > 0) {
        int cur = ctx->active_track_cursor;
        if (cur < 0) cur = 0;
        int nxt = cur + dy;
        if (nxt < 0) {
            if (dy < 0 && ctx->displayed_folders->len > 0) {
                ctx->active_zone = 0;
                ctx->active_folder_cursor = (int)ctx->displayed_folders->len - 1;
                sync_selection_visuals(ctx, TRUE);
                return;
            }
            nxt = 0;
        }
        if ((guint)nxt >= ctx->displayed_tracks->len) {
            nxt = (int)ctx->displayed_tracks->len - 1;
        }
        ctx->active_track_cursor = nxt;
        sync_selection_visuals(ctx, TRUE);
    }
}

static void jump_browser_cursor(LibraryBrowserCtx *ctx, bool to_end) {
    if (to_end) {
        if (ctx->displayed_tracks->len > 0) {
            ctx->active_zone = 1;
            ctx->active_track_cursor = (int)ctx->displayed_tracks->len - 1;
        } else if (ctx->displayed_folders->len > 0) {
            ctx->active_zone = 0;
            ctx->active_folder_cursor = (int)ctx->displayed_folders->len - 1;
        }
    } else {
        if (ctx->displayed_folders->len > 0) {
            ctx->active_zone = 0;
            ctx->active_folder_cursor = 0;
        } else if (ctx->displayed_tracks->len > 0) {
            ctx->active_zone = 1;
            ctx->active_track_cursor = 0;
        }
    }
    sync_selection_visuals(ctx, TRUE);
}

gboolean library_browser_handle_key(
    LibraryBrowserCtx *ctx,
    guint keyval,
    GdkModifierType state
) {
    if (!ctx) return FALSE;
    bool ctrl = (state & GDK_CONTROL_MASK) != 0;
    bool alt = (state & GDK_ALT_MASK) != 0;

    /* Ctrl+F toggles search mode from anywhere */
    if (ctrl && (keyval == GDK_KEY_f || keyval == GDK_KEY_F)) {
        if (ctx->search_mode) library_browser_exit_search_mode(ctx);
        else enter_search_mode(ctx);
        return TRUE;
    }

    if (ctx->search_mode) {
        if (keyval == GDK_KEY_Escape) {
            library_browser_exit_search_mode(ctx);
            return TRUE;
        }
        if (ctrl && (keyval == GDK_KEY_u || keyval == GDK_KEY_U)) {
            gtk_editable_set_text(GTK_EDITABLE(ctx->search_entry), "");
            return TRUE;
        }
        if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
            const ResultItem *it = current_selected_item(ctx);
            if (it) activate_result_item(ctx, it);
            library_browser_exit_search_mode(ctx);
            return TRUE;
        }
        /* Let GtkEntry handle all typed text in search mode */
        return FALSE;
    }

    /* Normal (Vim) Mode */
    if (keyval == GDK_KEY_slash) {
        enter_search_mode(ctx);
        return TRUE;
    }

    if (keyval == GDK_KEY_q) {
        if (ctx->on_play_done) ctx->on_play_done(LIB_ACTION_QUIT, ctx->user_data);
        return TRUE;
    }

    if (keyval == GDK_KEY_Escape) {
        if (ctx->detail_album_idx >= 0) {
            ctx->detail_album_idx = -1;
            rebuild_view(ctx);
            return TRUE;
        }
        const char *txt = gtk_editable_get_text(GTK_EDITABLE(ctx->search_entry));
        if (txt && *txt) {
            gtk_editable_set_text(GTK_EDITABLE(ctx->search_entry), "");
            return TRUE;
        }
        if (ctx->on_play_done) ctx->on_play_done(LIB_ACTION_QUIT, ctx->user_data);
        return TRUE;
    }

    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
        const ResultItem *it = current_selected_item(ctx);
        if (it) {
            if (ctrl && it->kind != ITEM_PARENT) execute_queue_action(ctx, it, 3);
            else activate_result_item(ctx, it);
        }
        return TRUE;
    }

    if (keyval == GDK_KEY_j || keyval == GDK_KEY_Down) {
        move_browser_cursor(ctx, 0, 1);
        return TRUE;
    }
    if (keyval == GDK_KEY_k || keyval == GDK_KEY_Up) {
        move_browser_cursor(ctx, 0, -1);
        return TRUE;
    }
    if (keyval == GDK_KEY_l || keyval == GDK_KEY_Right) {
        if (!ctx->compact_mode && ctx->detail_album_idx < 0) {
            move_browser_cursor(ctx, 1, 0);
        } else {
            const ResultItem *it = current_selected_item(ctx);
            if (it && (it->kind == ITEM_FOLDER || it->kind == ITEM_ALBUM || it->kind == ITEM_PARENT)) {
                activate_result_item(ctx, it);
            }
        }
        return TRUE;
    }
    if (keyval == GDK_KEY_h || keyval == GDK_KEY_Left) {
        if (ctx->detail_album_idx >= 0) {
            step_up_directory(ctx);
        } else if (!ctx->compact_mode) {
            move_browser_cursor(ctx, -1, 0);
        } else {
            step_up_directory(ctx);
        }
        return TRUE;
    }
    if (keyval == GDK_KEY_g) {
        jump_browser_cursor(ctx, false);
        return TRUE;
    }
    if (keyval == GDK_KEY_G) {
        jump_browser_cursor(ctx, true);
        return TRUE;
    }
    if (keyval == GDK_KEY_Page_Down || (ctrl && (keyval == GDK_KEY_d || keyval == GDK_KEY_D))) {
        move_browser_cursor(ctx, 0, 3);
        return TRUE;
    }
    if (keyval == GDK_KEY_Page_Up || (ctrl && (keyval == GDK_KEY_u || keyval == GDK_KEY_U))) {
        move_browser_cursor(ctx, 0, -3);
        return TRUE;
    }
    if (keyval == GDK_KEY_BackSpace || (alt && (keyval == GDK_KEY_Left || keyval == GDK_KEY_Up))) {
        step_up_directory(ctx);
        return TRUE;
    }

    if (keyval == GDK_KEY_v || keyval == GDK_KEY_V) {
        on_view_toggle_clicked(ctx->view_toggle_btn, ctx);
        return TRUE;
    }
    if (keyval == GDK_KEY_r || (ctrl && (keyval == GDK_KEY_r || keyval == GDK_KEY_R))) {
        refresh_library(ctx, true);
        return TRUE;
    }

    if (alt && (keyval == GDK_KEY_a || keyval == GDK_KEY_A)) {
        if (gtk_widget_get_visible(GTK_WIDGET(ctx->scope_bar))) {
            ctx->scope = SCOPE_ALL;
            rebuild_view(ctx);
            return TRUE;
        }
    }
    if (alt && (keyval == GDK_KEY_l || keyval == GDK_KEY_L)) {
        if (gtk_widget_get_visible(GTK_WIDGET(ctx->scope_bar))) {
            ctx->scope = SCOPE_ALBUMS;
            rebuild_view(ctx);
            return TRUE;
        }
    }
    if (alt && (keyval == GDK_KEY_t || keyval == GDK_KEY_T)) {
        if (gtk_widget_get_visible(GTK_WIDGET(ctx->scope_bar))) {
            ctx->scope = SCOPE_TRACKS;
            rebuild_view(ctx);
            return TRUE;
        }
    }

    const ResultItem *cur_item = current_selected_item(ctx);
    if (!cur_item || cur_item->kind == ITEM_PARENT) return FALSE;

    if (keyval == GDK_KEY_a) {
        execute_queue_action(ctx, cur_item, 0);
        return TRUE;
    }
    if (keyval == GDK_KEY_i) {
        execute_queue_action(ctx, cur_item, 1);
        return TRUE;
    }
    if (keyval == GDK_KEY_m || (ctrl && (keyval == GDK_KEY_p || keyval == GDK_KEY_P))) {
        send_item_to_playlists(ctx, cur_item);
        return TRUE;
    }
    if (keyval == GDK_KEY_f || keyval == GDK_KEY_F) {
        toggle_item_favourite(ctx, cur_item);
        rebuild_view(ctx);
        return TRUE;
    }

    return FALSE;
}

GtkWidget *library_browser_build_page(
    PickerCaches *caches,
    const char *pid_str,
    LibSendToPlaylistsCb on_send_playlists,
    LibPlayDoneCb on_play_done,
    gpointer user_data,
    LibraryBrowserCtx **out_ctx
) {
    LibraryBrowserCtx *ctx = g_new0(LibraryBrowserCtx, 1);
    ctx->caches = caches;
    ctx->pid_str = g_strdup(pid_str ? pid_str : "");
    ctx->on_send_playlists = on_send_playlists;
    ctx->on_play_done = on_play_done;
    ctx->user_data = user_data;
    ctx->detail_album_idx = -1;
    ctx->compact_mode = picker_caches_get_compact_mode(caches);
    ctx->search_mode = false;
    ctx->displayed_folders = g_ptr_array_new_with_free_func(result_item_free);
    ctx->displayed_tracks = g_ptr_array_new_with_free_func(result_item_free);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    ctx->root_box = root;

    /* Search + view toggle bar */
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *search = gtk_entry_new();
    gtk_widget_add_css_class(search, "picker-entry");
    gtk_widget_add_css_class(search, "search-inactive");
    gtk_widget_set_can_focus(search, FALSE);
    gtk_widget_set_hexpand(search, TRUE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(search), "Press / or Ctrl+F to search… (a: l: t: g: fav:)");
    ctx->search_entry = GTK_ENTRY(search);

    GtkGesture *search_click = gtk_gesture_click_new();
    g_signal_connect(search_click, "pressed", G_CALLBACK(on_search_entry_pressed), ctx);
    gtk_widget_add_controller(search, GTK_EVENT_CONTROLLER(search_click));

    GtkWidget *view_btn = gtk_button_new_with_label(ctx->compact_mode ? "⊞ Grid" : "☰ Compact");
    gtk_widget_add_css_class(view_btn, "pill-btn");
    gtk_widget_set_can_focus(view_btn, FALSE);
    gtk_widget_set_tooltip_text(view_btn, "Toggle Grid / Compact mode (v)");
    ctx->view_toggle_btn = GTK_BUTTON(view_btn);

    GtkWidget *ref_btn = gtk_button_new_with_label("↻");
    gtk_widget_add_css_class(ref_btn, "pill-btn");
    gtk_widget_set_can_focus(ref_btn, FALSE);
    gtk_widget_set_tooltip_text(ref_btn, "Resync library index from MPD (r)");
    ctx->refresh_btn = GTK_BUTTON(ref_btn);

    gtk_box_append(GTK_BOX(top), search);
    gtk_box_append(GTK_BOX(top), view_btn);
    gtk_box_append(GTK_BOX(top), ref_btn);
    gtk_box_append(GTK_BOX(root), top);

    /* Breadcrumb nav bar */
    GtkWidget *nav = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    ctx->nav_bar = GTK_BOX(nav);
    gtk_box_append(GTK_BOX(root), nav);

    /* Scope filter pills */
    GtkWidget *sbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    ctx->scope_bar = GTK_BOX(sbar);
    struct { const char *lbl; SearchScope sc; GtkButton **dst; } scopes[] = {
        { "All", SCOPE_ALL, &ctx->scope_all_btn },
        { "Albums", SCOPE_ALBUMS, &ctx->scope_alb_btn },
        { "Tracks", SCOPE_TRACKS, &ctx->scope_trk_btn },
    };
    for (int i = 0; i < 3; i++) {
        GtkWidget *b = gtk_button_new_with_label(scopes[i].lbl);
        gtk_widget_add_css_class(b, "pill-btn");
        gtk_widget_set_can_focus(b, FALSE);
        g_object_set_data(G_OBJECT(b), "scope-val", GINT_TO_POINTER((int)scopes[i].sc));
        g_signal_connect(b, "clicked", G_CALLBACK(on_scope_clicked), ctx);
        gtk_box_append(GTK_BOX(sbar), b);
        *scopes[i].dst = GTK_BUTTON(b);
    }
    GtkWidget *st_lbl = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(st_lbl), 1.0f);
    gtk_widget_set_hexpand(st_lbl, TRUE);
    gtk_widget_add_css_class(st_lbl, "row-count");
    ctx->status_lbl = GTK_LABEL(st_lbl);
    gtk_box_append(GTK_BOX(sbar), st_lbl);
    gtk_widget_set_visible(sbar, FALSE);
    gtk_box_append(GTK_BOX(root), sbar);

    /* Scrollable content */
    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_widget_set_can_focus(scrolled, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    ctx->scrolled = GTK_SCROLLED_WINDOW(scrolled);

    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    ctx->scroll_content = GTK_BOX(content);

    GtkWidget *banner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(banner, "new-box");
    gtk_widget_set_visible(banner, FALSE);
    ctx->album_banner = GTK_BOX(banner);
    gtk_box_append(GTK_BOX(content), banner);

    GtkWidget *fhdr = gtk_label_new("Albums & Folders");
    gtk_label_set_xalign(GTK_LABEL(fhdr), 0.0f);
    gtk_widget_add_css_class(fhdr, "section-header");
    ctx->folders_hdr = GTK_LABEL(fhdr);
    gtk_box_append(GTK_BOX(content), fhdr);

    GtkWidget *grid = gtk_flow_box_new();
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(grid), TRUE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(grid), 3);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(grid), 3);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(grid), 8);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(grid), 8);
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(grid), GTK_SELECTION_SINGLE);
    gtk_widget_add_css_class(grid, "picker-grid");
    ctx->grid_box = GTK_FLOW_BOX(grid);
    gtk_box_append(GTK_BOX(content), grid);

    GtkWidget *compact = gtk_list_box_new();
    gtk_widget_add_css_class(compact, "picker-list");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(compact), GTK_SELECTION_SINGLE);
    ctx->compact_box = GTK_LIST_BOX(compact);
    gtk_box_append(GTK_BOX(content), compact);

    GtkWidget *thdr = gtk_label_new("Tracks");
    gtk_label_set_xalign(GTK_LABEL(thdr), 0.0f);
    gtk_widget_add_css_class(thdr, "section-header");
    ctx->tracks_hdr = GTK_LABEL(thdr);
    gtk_box_append(GTK_BOX(content), thdr);

    GtkWidget *tracks = gtk_list_box_new();
    gtk_widget_add_css_class(tracks, "picker-list");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(tracks), GTK_SELECTION_SINGLE);
    ctx->tracks_box = GTK_LIST_BOX(tracks);
    gtk_box_append(GTK_BOX(content), tracks);

    GtkWidget *empty = gtk_label_new("");
    gtk_widget_add_css_class(empty, "picker-subtitle");
    gtk_widget_set_vexpand(empty, TRUE);
    gtk_widget_set_valign(empty, GTK_ALIGN_CENTER);
    ctx->empty_lbl = GTK_LABEL(empty);
    gtk_box_append(GTK_BOX(content), empty);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), content);
    gtk_box_append(GTK_BOX(root), scrolled);

    GtkWidget *footer = gtk_label_new("hjkl:nav  Enter:open/play  /:search  a:add  i:next  m:pl  v:view  q:quit");
    gtk_label_set_xalign(GTK_LABEL(footer), 0.0f);
    gtk_widget_add_css_class(footer, "footer-hints");
    gtk_box_append(GTK_BOX(root), footer);

    g_signal_connect(search, "changed", G_CALLBACK(on_search_changed), ctx);
    g_signal_connect(view_btn, "clicked", G_CALLBACK(on_view_toggle_clicked), ctx);
    g_signal_connect(ref_btn, "clicked", G_CALLBACK(on_refresh_clicked), ctx);
    g_signal_connect(root, "destroy", G_CALLBACK(on_browser_destroy), ctx);

    if (out_ctx) *out_ctx = ctx;
    return root;
}
