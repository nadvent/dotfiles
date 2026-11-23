#ifndef RMPC_PICKER_MPD_CLIENT_H
#define RMPC_PICKER_MPD_CLIENT_H

#include <glib.h>
#include <mpd/client.h>
#include <stdbool.h>

#define FAVOURITES_PLAYLIST "favourites"
#define FAV_STICKER "fav"

typedef enum {
    PICKER_STATE_NONE = 0,
    PICKER_STATE_SOME = 1,
    PICKER_STATE_ALL  = 2,
} PickerState;

typedef struct {
    char *name;
    bool is_favourites;
    bool exists;
    int count;
    int matched;
    gint64 last_modified;
    PickerState state;
} PlaylistOverviewItem;

typedef struct {
    char *name;
    int added;
    int skipped;
} PlaylistAddResult;

typedef struct {
    char *name;
    int removed;
} PlaylistRemoveResult;

typedef struct {
    const char *uri;          /* arena-owned */
    const char *dir;          /* arena-owned */
    const char *title;        /* arena-owned */
    const char *artist;       /* arena-owned */
    const char *album_artist; /* arena-owned */
    const char *album;        /* arena-owned */
    const char *genre;        /* arena-owned */
    const char *date;         /* arena-owned */
    const char *search_blob;  /* casefolded arena-owned */
    unsigned track_num;
    unsigned disc_num;
    unsigned duration_s;
    bool is_fav;
} LibTrack;

typedef struct {
    const char *path;         /* arena-owned: "" for root */
    const char *name;         /* arena-owned */
    const char *parent_path;  /* arena-owned */
    const char *album_name;   /* arena-owned (set if is_single_album) */
    const char *artist_name;  /* arena-owned */
    const char *date;         /* arena-owned */
    const char *art_uri;      /* arena-owned representative song URI */
    const char *search_blob;  /* casefolded arena-owned */
    bool is_single_album;
    bool is_multidisc;
    bool has_fav;
    GArray *child_dir_indices;  /* element-type: guint */
    GArray *track_indices;      /* element-type: guint */
    guint recursive_track_count;
    guint album_count;
} LibFolder;

typedef struct {
    unsigned long db_update_time;
    GStringChunk *arena;
    GPtrArray *tracks;          /* element-type: LibTrack* */
    GPtrArray *folders;         /* element-type: LibFolder* (0 is root) */
    GHashTable *folder_by_path; /* borrowed const char* -> GUINT_TO_POINTER(idx) */
    GHashTable *track_by_uri;   /* borrowed const char* -> LibTrack* */
} LibIndex;

void playlist_overview_item_free(gpointer p);
void playlist_add_result_free(gpointer p);
void playlist_remove_result_free(gpointer p);
void lib_index_free(LibIndex *idx);

struct mpd_connection *mpd_open_configured(GError **err);
char *mpd_resolve_music_dir(void);

bool mpd_validate_playlist_name(const char *raw, char **out_cleaned, char **out_err);

GPtrArray *mpd_resolve_target_songs(
    struct mpd_connection *conn,
    const char *selected_songs_raw,
    const char *current_song_env,
    GError **err
);

char *mpd_describe_targets(
    struct mpd_connection *conn,
    GPtrArray *target_songs,
    char **out_tooltip
);

GPtrArray *mpd_get_playlists_overview(
    struct mpd_connection *conn,
    GPtrArray *target_songs,
    GError **err
);

GPtrArray *mpd_add_songs_to_playlists(
    struct mpd_connection *conn,
    GPtrArray *target_songs,
    GPtrArray *playlist_names,
    GError **err
);

GPtrArray *mpd_remove_songs_from_playlists(
    struct mpd_connection *conn,
    GPtrArray *target_songs,
    GPtrArray *playlist_names,
    GError **err
);

bool mpd_sync_favourites_stickers(
    struct mpd_connection *conn,
    int *out_added,
    int *out_removed,
    GError **err
);

bool mpd_toggle_favourites(
    struct mpd_connection *conn,
    GPtrArray *target_songs,
    char **out_action,
    int *out_count,
    GError **err
);

bool mpd_append_uris(struct mpd_connection *conn, GPtrArray *uris, GError **err);
bool mpd_append_and_play_uris(struct mpd_connection *conn, GPtrArray *uris, GError **err);
bool mpd_replace_and_play_uris(struct mpd_connection *conn, GPtrArray *uris, GError **err);
bool mpd_insert_uris_next(struct mpd_connection *conn, GPtrArray *uris, GError **err);

unsigned long mpd_get_db_update_time(struct mpd_connection *conn);
LibIndex *mpd_fetch_library_index(struct mpd_connection *conn, GError **err);
void mpd_library_index_refresh_favs(struct mpd_connection *conn, LibIndex *idx);

bool mpd_dir_has_local_cover(const char *music_dir, const char *dir_rel);
GBytes *mpd_fetch_cover_bytes(
    struct mpd_connection *conn_opt,
    const char *music_dir,
    const char *song_uri
);

char *mpd_format_add_status(GPtrArray *results);
char *mpd_format_remove_status(GPtrArray *results);

void rmpc_notify(const char *pid_str, const char *level, int timeout_ms, const char *message);

#endif
