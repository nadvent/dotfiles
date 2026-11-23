#ifndef RMPC_PICKER_LIBRARY_BROWSER_H
#define RMPC_PICKER_LIBRARY_BROWSER_H

#include "mpd_client.h"
#include <gtk/gtk.h>
#include <stdbool.h>

typedef struct _PickerCaches PickerCaches;

PickerCaches *picker_caches_new(void);
void picker_caches_free(PickerCaches *caches);

/* Ensures LibIndex is up to date vs MPD db_update_time; returns borrowed pointer owned by caches */
LibIndex *picker_caches_get_library(PickerCaches *caches, struct mpd_connection *conn, bool force_refresh, GError **err);
bool picker_caches_get_compact_mode(PickerCaches *caches);
void picker_caches_set_compact_mode(PickerCaches *caches, bool compact);

typedef enum {
    LIB_ACTION_NONE = 0,
    LIB_ACTION_APPEND_AND_PLAY,
    LIB_ACTION_REPLACE_AND_PLAY,
    LIB_ACTION_QUIT,
} LibPlayAction;

typedef void (*LibSendToPlaylistsCb)(GPtrArray *uris, const char *summary, const char *tooltip, gpointer user_data);
typedef void (*LibPlayDoneCb)(LibPlayAction action, gpointer user_data);

typedef struct _LibraryBrowserCtx LibraryBrowserCtx;

GtkWidget *library_browser_build_page(
    PickerCaches *caches,
    const char *pid_str,
    LibSendToPlaylistsCb on_send_playlists,
    LibPlayDoneCb on_play_done,
    gpointer user_data,
    LibraryBrowserCtx **out_ctx
);

void library_browser_on_activate(LibraryBrowserCtx *ctx);
void library_browser_exit_search_mode(LibraryBrowserCtx *ctx);
bool library_browser_is_search_mode(const LibraryBrowserCtx *ctx);
gboolean library_browser_handle_key(
    LibraryBrowserCtx *ctx,
    guint keyval,
    GdkModifierType state
);

#endif
