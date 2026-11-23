#include "mpd_client.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define RMPC_PICKER_ERROR (g_quark_from_static_string("rmpc-picker-mpd"))
#define MAX_COVER_BYTES (16 * 1024 * 1024)

void playlist_overview_item_free(gpointer p) {
    PlaylistOverviewItem *item = (PlaylistOverviewItem *)p;
    if (!item) return;
    g_free(item->name);
    g_free(item);
}

void playlist_add_result_free(gpointer p) {
    PlaylistAddResult *r = (PlaylistAddResult *)p;
    if (!r) return;
    g_free(r->name);
    g_free(r);
}

void playlist_remove_result_free(gpointer p) {
    PlaylistRemoveResult *r = (PlaylistRemoveResult *)p;
    if (!r) return;
    g_free(r->name);
    g_free(r);
}

static void lib_folder_free(gpointer p) {
    LibFolder *f = (LibFolder *)p;
    if (!f) return;
    if (f->child_dir_indices) g_array_unref(f->child_dir_indices);
    if (f->track_indices) g_array_unref(f->track_indices);
    g_free(f);
}

void lib_index_free(LibIndex *idx) {
    if (!idx) return;
    if (idx->folder_by_path) g_hash_table_destroy(idx->folder_by_path);
    if (idx->track_by_uri) g_hash_table_destroy(idx->track_by_uri);
    if (idx->folders) g_ptr_array_unref(idx->folders);
    if (idx->tracks) g_ptr_array_unref(idx->tracks);
    if (idx->arena) g_string_chunk_free(idx->arena);
    g_free(idx);
}

static bool set_mpd_error(struct mpd_connection *conn, GError **err, const char *context) {
    enum mpd_error code = mpd_connection_get_error(conn);
    if (code == MPD_ERROR_SUCCESS) return false;
    const char *msg = mpd_connection_get_error_message(conn);
    g_set_error(err, RMPC_PICKER_ERROR, (int)code, "%s: %s", context, msg ? msg : "unknown MPD error");
    return true;
}

static char *resolve_config_ron_path(void) {
    const char *xdg = g_getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        return g_build_filename(xdg, "rmpc", "config.ron", NULL);
    }
    const char *home = g_getenv("HOME");
    if (!home || !*home) home = g_get_home_dir();
    return g_build_filename(home, ".config", "rmpc", "config.ron", NULL);
}

static void read_rmpc_config_connection(char **out_host, unsigned *out_port, char **out_password) {
    *out_host = NULL;
    *out_port = 0;
    *out_password = NULL;

    char *path = resolve_config_ron_path();
    gchar *raw = NULL;
    gsize len = 0;
    if (!g_file_get_contents(path, &raw, &len, NULL)) {
        g_free(path);
        return;
    }
    g_free(path);

    /* Strip // line comments before lightweight RON matching */
    GString *stripped = g_string_sized_new(len);
    const char *p = raw;
    while (*p) {
        const char *nl = strchr(p, '\n');
        const char *line_end = nl ? nl : (p + strlen(p));
        const char *comment = strstr(p, "//");
        const char *take_end = (comment && comment < line_end) ? comment : line_end;
        g_string_append_len(stripped, p, (gssize)(take_end - p));
        g_string_append_c(stripped, '\n');
        p = nl ? (nl + 1) : line_end;
    }
    g_free(raw);

    GRegex *re_addr = g_regex_new("\\baddress\\s*:\\s*\"([^\"]+)\"", 0, 0, NULL);
    GRegex *re_pass = g_regex_new("\\bpassword\\s*:\\s*\"([^\"]*)\"", 0, 0, NULL);
    GMatchInfo *mi = NULL;

    if (re_pass && g_regex_match(re_pass, stripped->str, 0, &mi)) {
        *out_password = g_match_info_fetch(mi, 1);
    }
    g_clear_pointer(&mi, g_match_info_free);

    if (re_addr && g_regex_match(re_addr, stripped->str, 0, &mi)) {
        char *addr_raw = g_match_info_fetch(mi, 1);
        g_strstrip(addr_raw);
        if (addr_raw[0] == '/' || addr_raw[0] == '@') {
            *out_host = addr_raw;
        } else {
            char *colon = strrchr(addr_raw, ':');
            if (colon) {
                *colon = '\0';
                char *h = addr_raw;
                size_t hlen = strlen(h);
                if (hlen >= 2 && h[0] == '[' && h[hlen - 1] == ']') {
                    h[hlen - 1] = '\0';
                    h++;
                }
                char *endptr = NULL;
                unsigned long parsed = strtoul(colon + 1, &endptr, 10);
                if (endptr && *endptr == '\0' && parsed > 0 && parsed <= 65535) {
                    *out_host = g_strdup(h);
                    *out_port = (unsigned)parsed;
                    g_free(addr_raw);
                } else {
                    *colon = ':';
                    *out_host = addr_raw;
                }
            } else {
                *out_host = addr_raw;
            }
        }
    }
    g_clear_pointer(&mi, g_match_info_free);
    g_clear_pointer(&re_addr, g_regex_unref);
    g_clear_pointer(&re_pass, g_regex_unref);
    g_string_free(stripped, TRUE);
}

static void resolve_mpd_connection(char **out_host, unsigned *out_port, char **out_password) {
    char *cfg_host = NULL;
    unsigned cfg_port = 0;
    char *cfg_pw = NULL;
    read_rmpc_config_connection(&cfg_host, &cfg_port, &cfg_pw);

    const char *env_host_raw = g_getenv("MPD_HOST");
    char *env_host = NULL;
    char *env_pw = NULL;
    if (env_host_raw && *env_host_raw) {
        char *trimmed = g_strdup(env_host_raw);
        g_strstrip(trimmed);
        if (*trimmed) {
            const char *at = strrchr(trimmed, '@');
            if (at && at != trimmed) {
                env_pw = g_strndup(trimmed, (gsize)(at - trimmed));
                env_host = g_strdup(at + 1);
            } else {
                env_host = g_strdup(trimmed);
            }
        }
        g_free(trimmed);
    }

    const char *final_host = (env_host && *env_host) ? env_host : ((cfg_host && *cfg_host) ? cfg_host : "127.0.0.1");
    *out_host = g_strdup(final_host);

    const char *env_port_raw = g_getenv("MPD_PORT");
    unsigned final_port = 6600;
    if (env_port_raw && *env_port_raw) {
        char *endptr = NULL;
        unsigned long parsed = strtoul(env_port_raw, &endptr, 10);
        if (endptr && *endptr == '\0' && parsed <= 65535) {
            final_port = (unsigned)parsed;
        }
    } else if (cfg_port > 0) {
        final_port = cfg_port;
    }
    *out_port = final_port;

    if (env_pw && *env_pw) {
        *out_password = g_strdup(env_pw);
    } else if (cfg_pw && *cfg_pw) {
        *out_password = g_strdup(cfg_pw);
    } else {
        *out_password = NULL;
    }

    g_free(cfg_host);
    g_free(cfg_pw);
    g_free(env_host);
    g_free(env_pw);
}

struct mpd_connection *mpd_open_configured(GError **err) {
    char *host = NULL;
    unsigned port = 6600;
    char *password = NULL;
    resolve_mpd_connection(&host, &port, &password);

    unsigned effective_port = (host[0] == '/' || host[0] == '@') ? 0 : port;
    struct mpd_connection *conn = mpd_connection_new(host, effective_port, 5000);
    if (!conn) {
        g_set_error(err, RMPC_PICKER_ERROR, MPD_ERROR_OOM, "Out of memory connecting to MPD");
        g_free(host);
        g_free(password);
        return NULL;
    }

    if (mpd_connection_get_error(conn) != MPD_ERROR_SUCCESS) {
        g_set_error(
            err,
            RMPC_PICKER_ERROR,
            (int)mpd_connection_get_error(conn),
            "Unable to connect to MPD at %s:%u (%s)",
            host,
            port,
            mpd_connection_get_error_message(conn)
        );
        mpd_connection_free(conn);
        g_free(host);
        g_free(password);
        return NULL;
    }

    if (password && *password) {
        if (!mpd_run_password(conn, password)) {
            set_mpd_error(conn, err, "MPD authentication failed");
            mpd_connection_free(conn);
            g_free(host);
            g_free(password);
            return NULL;
        }
    }

    g_free(host);
    g_free(password);
    return conn;
}

static char *expand_tilde(const char *path) {
    if (!path || !*path) return NULL;
    if (path[0] == '~' && (path[1] == '/' || path[1] == '\0')) {
        const char *home = g_getenv("HOME");
        if (!home || !*home) home = g_get_home_dir();
        return g_build_filename(home, path + (path[1] == '/' ? 2 : 1), NULL);
    }
    return g_strdup(path);
}

char *mpd_resolve_music_dir(void) {
    const char *xdg_music = g_getenv("XDG_MUSIC_DIR");
    if (xdg_music && *xdg_music) {
        char *expanded = expand_tilde(xdg_music);
        if (expanded && g_file_test(expanded, G_FILE_TEST_IS_DIR)) {
            return expanded;
        }
        g_free(expanded);
    }

    const char *xdg_cfg = g_getenv("XDG_CONFIG_HOME");
    const char *home = g_getenv("HOME");
    if (!home || !*home) home = g_get_home_dir();
    char *mpd_conf = (xdg_cfg && *xdg_cfg)
        ? g_build_filename(xdg_cfg, "mpd", "mpd.conf", NULL)
        : g_build_filename(home, ".config", "mpd", "mpd.conf", NULL);

    gchar *raw = NULL;
    if (g_file_get_contents(mpd_conf, &raw, NULL, NULL)) {
        GRegex *re = g_regex_new("^\\s*music_directory\\s+\"([^\"]+)\"", G_REGEX_MULTILINE, 0, NULL);
        GMatchInfo *mi = NULL;
        if (re && g_regex_match(re, raw, 0, &mi)) {
            char *val = g_match_info_fetch(mi, 1);
            char *expanded = expand_tilde(val);
            g_free(val);
            if (expanded && g_file_test(expanded, G_FILE_TEST_IS_DIR)) {
                g_match_info_free(mi);
                g_regex_unref(re);
                g_free(raw);
                g_free(mpd_conf);
                return expanded;
            }
            g_free(expanded);
        }
        g_clear_pointer(&mi, g_match_info_free);
        g_clear_pointer(&re, g_regex_unref);
        g_free(raw);
    }
    g_free(mpd_conf);

    return g_build_filename(home, "Music", NULL);
}

bool mpd_validate_playlist_name(const char *raw, char **out_cleaned, char **out_err) {
    if (!raw) {
        if (out_err) *out_err = g_strdup("Playlist name cannot be empty");
        return false;
    }
    char *cleaned = g_strdup(raw);
    g_strstrip(cleaned);
    if (*cleaned == '\0') {
        g_free(cleaned);
        if (out_err) *out_err = g_strdup("Playlist name cannot be empty");
        return false;
    }
    for (const char *p = cleaned; *p; p++) {
        if (*p == '/' || *p == '\n' || *p == '\r') {
            g_free(cleaned);
            if (out_err) *out_err = g_strdup("Playlist name cannot contain '/' or control characters");
            return false;
        }
    }
    *out_cleaned = cleaned;
    return true;
}

/* Single-ownership helper: arr owns strings, seen borrows keys from arr */
static void append_unique_borrowed(GPtrArray *arr, GHashTable *seen, const char *uri) {
    if (!uri || !*uri) return;
    if (g_hash_table_contains(seen, uri)) return;
    char *owned = g_strdup(uri);
    g_ptr_array_add(arr, owned);
    g_hash_table_add(seen, owned);
}

GPtrArray *mpd_resolve_target_songs(
    struct mpd_connection *conn,
    const char *selected_songs_raw,
    const char *current_song_env,
    GError **err
) {
    GPtrArray *resolved = g_ptr_array_new_with_free_func(g_free);
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);

    if (selected_songs_raw && *selected_songs_raw) {
        gchar **lines = g_strsplit(selected_songs_raw, "\n", -1);
        for (int i = 0; lines && lines[i]; i++) {
            char *item = g_strstrip(lines[i]);
            if (!*item) continue;
            if (g_str_has_prefix(item, "http://") || g_str_has_prefix(item, "https://")) {
                append_unique_borrowed(resolved, seen, item);
                continue;
            }

            guint before = resolved->len;
            if (mpd_send_list_all(conn, item)) {
                struct mpd_entity *ent = NULL;
                while ((ent = mpd_recv_entity(conn)) != NULL) {
                    if (mpd_entity_get_type(ent) == MPD_ENTITY_TYPE_SONG) {
                        const struct mpd_song *song = mpd_entity_get_song(ent);
                        append_unique_borrowed(resolved, seen, mpd_song_get_uri(song));
                    }
                    mpd_entity_free(ent);
                }
                if (!mpd_response_finish(conn)) {
                    if (mpd_connection_get_error(conn) == MPD_ERROR_SERVER &&
                        mpd_connection_get_server_error(conn) == MPD_SERVER_ERROR_NO_EXIST) {
                        mpd_connection_clear_error(conn);
                    } else if (mpd_connection_get_error(conn) != MPD_ERROR_SUCCESS) {
                        set_mpd_error(conn, err, "listall failed");
                        g_strfreev(lines);
                        g_hash_table_destroy(seen);
                        g_ptr_array_unref(resolved);
                        return NULL;
                    }
                }
            } else {
                if (mpd_connection_get_error(conn) == MPD_ERROR_SERVER) {
                    mpd_connection_clear_error(conn);
                } else {
                    set_mpd_error(conn, err, "listall send failed");
                    g_strfreev(lines);
                    g_hash_table_destroy(seen);
                    g_ptr_array_unref(resolved);
                    return NULL;
                }
            }
            if (resolved->len == before) {
                append_unique_borrowed(resolved, seen, item);
            }
        }
        g_strfreev(lines);
    }

    if (resolved->len > 0) {
        g_hash_table_destroy(seen);
        return resolved;
    }

    struct mpd_song *cur = mpd_run_current_song(conn);
    if (cur) {
        const char *uri = mpd_song_get_uri(cur);
        if (uri && *uri) append_unique_borrowed(resolved, seen, uri);
        mpd_song_free(cur);
    } else if (mpd_connection_get_error(conn) != MPD_ERROR_SUCCESS) {
        if (mpd_connection_get_error(conn) == MPD_ERROR_SERVER) {
            mpd_connection_clear_error(conn);
        } else {
            set_mpd_error(conn, err, "currentsong failed");
            g_hash_table_destroy(seen);
            g_ptr_array_unref(resolved);
            return NULL;
        }
    }

    if (resolved->len == 0 && current_song_env && *current_song_env) {
        char *copy = g_strdup(current_song_env);
        g_strstrip(copy);
        if (*copy) append_unique_borrowed(resolved, seen, copy);
        g_free(copy);
    }

    g_hash_table_destroy(seen);
    return resolved;
}

static char *path_stem(const char *uri) {
    char *base = g_path_get_basename(uri);
    char *dot = strrchr(base, '.');
    if (dot && dot != base) *dot = '\0';
    return base;
}

static char *describe_single_uri(struct mpd_connection *conn, const char *uri) {
    char *title = NULL;
    char *artist = NULL;

    if (conn && mpd_send_list_meta(conn, uri)) {
        struct mpd_entity *ent = NULL;
        while ((ent = mpd_recv_entity(conn)) != NULL) {
            if (mpd_entity_get_type(ent) == MPD_ENTITY_TYPE_SONG && !title && !artist) {
                const struct mpd_song *song = mpd_entity_get_song(ent);
                const char *t = mpd_song_get_tag(song, MPD_TAG_TITLE, 0);
                const char *a = mpd_song_get_tag(song, MPD_TAG_ARTIST, 0);
                if (t && *t) title = g_strdup(t);
                if (a && *a) artist = g_strdup(a);
            }
            mpd_entity_free(ent);
        }
        if (!mpd_response_finish(conn)) {
            mpd_connection_clear_error(conn);
        }
    } else if (conn) {
        mpd_connection_clear_error(conn);
    }

    char *label = NULL;
    if (artist && title) {
        label = g_strdup_printf("%s - %s", artist, title);
    } else if (title) {
        label = g_strdup(title);
    } else {
        char *stem = path_stem(uri);
        if (*stem) {
            label = stem;
        } else {
            g_free(stem);
            label = g_strdup(uri);
        }
    }

    g_free(title);
    g_free(artist);
    return label;
}

char *mpd_describe_targets(
    struct mpd_connection *conn,
    GPtrArray *target_songs,
    char **out_tooltip
) {
    if (out_tooltip) *out_tooltip = NULL;
    if (!target_songs || target_songs->len == 0) {
        return g_strdup("0 songs selected");
    }

    if (target_songs->len == 1) {
        const char *uri = (const char *)g_ptr_array_index(target_songs, 0);
        char *label = describe_single_uri(conn, uri);
        if (out_tooltip) *out_tooltip = g_strdup(label);
        return label;
    }

    if (out_tooltip) {
        GString *tip = g_string_new("");
        guint limit = (target_songs->len < 8) ? target_songs->len : 8;
        for (guint i = 0; i < limit; i++) {
            const char *uri = (const char *)g_ptr_array_index(target_songs, i);
            char *item_lbl = describe_single_uri(conn, uri);
            if (i > 0) g_string_append_c(tip, '\n');
            g_string_append_printf(tip, "• %s", item_lbl);
            g_free(item_lbl);
        }
        if (target_songs->len > limit) {
            g_string_append_printf(tip, "\n… and %u more", target_songs->len - limit);
        }
        *out_tooltip = g_string_free(tip, FALSE);
    }

    return g_strdup_printf("%u songs selected", target_songs->len);
}

static gint compare_names_casefold(gconstpointer a, gconstpointer b) {
    const char *sa = *(const char *const *)a;
    const char *sb = *(const char *const *)b;
    char *fa = g_utf8_casefold(sa, -1);
    char *fb = g_utf8_casefold(sb, -1);
    int cmp = g_strcmp0(fa, fb);
    if (cmp == 0) cmp = g_strcmp0(sa, sb);
    g_free(fa);
    g_free(fb);
    return cmp;
}

static GPtrArray *fetch_single_playlist_tracks(struct mpd_connection *conn, const char *name, GError **err) {
    GPtrArray *tracks = g_ptr_array_new_with_free_func(g_free);
    if (!mpd_send_list_playlist(conn, name)) {
        if (mpd_connection_get_error(conn) == MPD_ERROR_SERVER &&
            mpd_connection_get_server_error(conn) == MPD_SERVER_ERROR_NO_EXIST) {
            mpd_connection_clear_error(conn);
            return tracks;
        }
        set_mpd_error(conn, err, "listplaylist send failed");
        g_ptr_array_unref(tracks);
        return NULL;
    }

    struct mpd_entity *ent = NULL;
    while ((ent = mpd_recv_entity(conn)) != NULL) {
        if (mpd_entity_get_type(ent) == MPD_ENTITY_TYPE_SONG) {
            const struct mpd_song *song = mpd_entity_get_song(ent);
            const char *uri = mpd_song_get_uri(song);
            if (uri) g_ptr_array_add(tracks, g_strdup(uri));
        }
        mpd_entity_free(ent);
    }

    if (!mpd_response_finish(conn)) {
        if (mpd_connection_get_error(conn) == MPD_ERROR_SERVER &&
            mpd_connection_get_server_error(conn) == MPD_SERVER_ERROR_NO_EXIST) {
            mpd_connection_clear_error(conn);
            return tracks;
        }
        set_mpd_error(conn, err, "listplaylist failed");
        g_ptr_array_unref(tracks);
        return NULL;
    }
    return tracks;
}

static GHashTable *fetch_playlists_tracks_batch(struct mpd_connection *conn, GPtrArray *names, GError **err) {
    GHashTable *map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
    if (!names || names->len == 0) return map;

    bool batch_ok = mpd_command_list_begin(conn, true);
    if (batch_ok) {
        for (guint i = 0; i < names->len; i++) {
            const char *name = (const char *)g_ptr_array_index(names, i);
            if (!mpd_send_list_playlist(conn, name)) {
                batch_ok = false;
                break;
            }
        }
        if (batch_ok && !mpd_command_list_end(conn)) {
            batch_ok = false;
        }
    }

    if (batch_ok) {
        for (guint i = 0; i < names->len; i++) {
            const char *name = (const char *)g_ptr_array_index(names, i);
            GPtrArray *tracks = g_ptr_array_new_with_free_func(g_free);
            struct mpd_pair *pair = NULL;
            while ((pair = mpd_recv_pair_named(conn, "file")) != NULL) {
                g_ptr_array_add(tracks, g_strdup(pair->value));
                mpd_return_pair(conn, pair);
            }
            g_hash_table_insert(map, g_strdup(name), tracks);
            if (i + 1 < names->len) {
                if (!mpd_response_next(conn)) {
                    batch_ok = false;
                    break;
                }
            }
        }
        if (batch_ok && !mpd_response_finish(conn)) {
            batch_ok = false;
        }
    }

    if (batch_ok) return map;

    if (mpd_connection_get_error(conn) == MPD_ERROR_SERVER) {
        mpd_connection_clear_error(conn);
        g_hash_table_remove_all(map);
        for (guint i = 0; i < names->len; i++) {
            const char *name = (const char *)g_ptr_array_index(names, i);
            GPtrArray *tracks = fetch_single_playlist_tracks(conn, name, err);
            if (!tracks) {
                g_hash_table_destroy(map);
                return NULL;
            }
            g_hash_table_insert(map, g_strdup(name), tracks);
        }
        return map;
    }

    set_mpd_error(conn, err, "listplaylist batch failed");
    g_hash_table_destroy(map);
    return NULL;
}

GPtrArray *mpd_get_playlists_overview(
    struct mpd_connection *conn,
    GPtrArray *target_songs,
    GError **err
) {
    if (!mpd_send_list_playlists(conn)) {
        set_mpd_error(conn, err, "listplaylists send failed");
        return NULL;
    }

    GHashTable *mtime_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    GPtrArray *other_names = g_ptr_array_new_with_free_func(g_free);

    struct mpd_playlist *pl = NULL;
    while ((pl = mpd_recv_playlist(conn)) != NULL) {
        const char *name = mpd_playlist_get_path(pl);
        if (name && *name) {
            gint64 *mt = g_new(gint64, 1);
            *mt = (gint64)mpd_playlist_get_last_modified(pl);
            g_hash_table_insert(mtime_map, g_strdup(name), mt);
            if (g_strcmp0(name, FAVOURITES_PLAYLIST) != 0) {
                g_ptr_array_add(other_names, g_strdup(name));
            }
        }
        mpd_playlist_free(pl);
    }

    if (!mpd_response_finish(conn)) {
        set_mpd_error(conn, err, "listplaylists failed");
        g_hash_table_destroy(mtime_map);
        g_ptr_array_unref(other_names);
        return NULL;
    }

    g_ptr_array_sort(other_names, compare_names_casefold);

    GPtrArray *ordered_names = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *existing_names = g_ptr_array_new_with_free_func(g_free);

    g_ptr_array_add(ordered_names, g_strdup(FAVOURITES_PLAYLIST));
    if (g_hash_table_contains(mtime_map, FAVOURITES_PLAYLIST)) {
        g_ptr_array_add(existing_names, g_strdup(FAVOURITES_PLAYLIST));
    }
    for (guint i = 0; i < other_names->len; i++) {
        const char *n = (const char *)g_ptr_array_index(other_names, i);
        g_ptr_array_add(ordered_names, g_strdup(n));
        g_ptr_array_add(existing_names, g_strdup(n));
    }
    g_ptr_array_unref(other_names);

    GHashTable *tracks_by_name = fetch_playlists_tracks_batch(conn, existing_names, err);
    g_ptr_array_unref(existing_names);
    if (!tracks_by_name) {
        g_hash_table_destroy(mtime_map);
        g_ptr_array_unref(ordered_names);
        return NULL;
    }

    GHashTable *target_set = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; target_songs && i < target_songs->len; i++) {
        g_hash_table_add(target_set, g_ptr_array_index(target_songs, i));
    }
    guint total_targets = g_hash_table_size(target_set);

    GPtrArray *overview = g_ptr_array_new_with_free_func(playlist_overview_item_free);
    for (guint i = 0; i < ordered_names->len; i++) {
        const char *name = (const char *)g_ptr_array_index(ordered_names, i);
        GPtrArray *tracks = (GPtrArray *)g_hash_table_lookup(tracks_by_name, name);

        GHashTable *track_set = g_hash_table_new(g_str_hash, g_str_equal);
        int count = 0;
        if (tracks) {
            count = (int)tracks->len;
            for (guint j = 0; j < tracks->len; j++) {
                g_hash_table_add(track_set, g_ptr_array_index(tracks, j));
            }
        }

        int matched = 0;
        GHashTableIter it;
        gpointer key = NULL;
        g_hash_table_iter_init(&it, target_set);
        while (g_hash_table_iter_next(&it, &key, NULL)) {
            if (g_hash_table_contains(track_set, key)) matched++;
        }
        g_hash_table_destroy(track_set);

        PickerState st = PICKER_STATE_NONE;
        if (total_targets > 0 && matched > 0) {
            st = ((guint)matched == total_targets) ? PICKER_STATE_ALL : PICKER_STATE_SOME;
        }

        gint64 *mt_ptr = (gint64 *)g_hash_table_lookup(mtime_map, name);

        PlaylistOverviewItem *item = g_new0(PlaylistOverviewItem, 1);
        item->name = g_strdup(name);
        item->is_favourites = (g_strcmp0(name, FAVOURITES_PLAYLIST) == 0);
        item->exists = (mt_ptr != NULL);
        item->count = count;
        item->matched = matched;
        item->last_modified = mt_ptr ? *mt_ptr : 0;
        item->state = st;
        g_ptr_array_add(overview, item);
    }

    g_hash_table_destroy(target_set);
    g_hash_table_destroy(tracks_by_name);
    g_hash_table_destroy(mtime_map);
    g_ptr_array_unref(ordered_names);
    return overview;
}

static GPtrArray *dedup_and_validate_playlists(GPtrArray *playlist_names, GError **err) {
    GPtrArray *validated = g_ptr_array_new_with_free_func(g_free);
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; playlist_names && i < playlist_names->len; i++) {
        const char *raw = (const char *)g_ptr_array_index(playlist_names, i);
        char *cleaned = NULL;
        char *verr = NULL;
        if (!mpd_validate_playlist_name(raw, &cleaned, &verr)) {
            g_set_error_literal(err, RMPC_PICKER_ERROR, MPD_ERROR_ARGUMENT, verr ? verr : "Invalid playlist name");
            g_free(verr);
            g_hash_table_destroy(seen);
            g_ptr_array_unref(validated);
            return NULL;
        }
        append_unique_borrowed(validated, seen, cleaned);
        g_free(cleaned);
    }
    g_hash_table_destroy(seen);
    return validated;
}

bool mpd_sync_favourites_stickers(
    struct mpd_connection *conn,
    int *out_added,
    int *out_removed,
    GError **err
) {
    if (out_added) *out_added = 0;
    if (out_removed) *out_removed = 0;

    GPtrArray *fav_tracks = fetch_single_playlist_tracks(conn, FAVOURITES_PLAYLIST, err);
    if (!fav_tracks) return false;

    GHashTable *fav_set = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < fav_tracks->len; i++) {
        g_hash_table_add(fav_set, g_ptr_array_index(fav_tracks, i));
    }

    GHashTable *existing_stickers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    if (!mpd_send_sticker_find(conn, "song", "", FAV_STICKER)) {
        set_mpd_error(conn, err, "sticker find failed");
        g_hash_table_destroy(existing_stickers);
        g_hash_table_destroy(fav_set);
        g_ptr_array_unref(fav_tracks);
        return false;
    }

    char *cur_file = NULL;
    struct mpd_pair *pair = NULL;
    while ((pair = mpd_recv_pair(conn)) != NULL) {
        if (g_strcmp0(pair->name, "file") == 0) {
            g_free(cur_file);
            cur_file = g_strdup(pair->value);
        } else if (g_strcmp0(pair->name, "sticker") == 0 && cur_file) {
            size_t name_len = 0;
            const char *val = mpd_parse_sticker(pair->value, &name_len);
            if (val && name_len == strlen(FAV_STICKER) &&
                strncmp(pair->value, FAV_STICKER, name_len) == 0) {
                g_hash_table_insert(existing_stickers, g_strdup(cur_file), g_strdup(val));
            }
        }
        mpd_return_pair(conn, pair);
    }
    g_free(cur_file);

    if (!mpd_response_finish(conn)) {
        if (mpd_connection_get_error(conn) == MPD_ERROR_SERVER &&
            mpd_connection_get_server_error(conn) == MPD_SERVER_ERROR_NO_EXIST) {
            mpd_connection_clear_error(conn);
        } else {
            set_mpd_error(conn, err, "sticker find response failed");
            g_hash_table_destroy(existing_stickers);
            g_hash_table_destroy(fav_set);
            g_ptr_array_unref(fav_tracks);
            return false;
        }
    }

    GPtrArray *to_set = g_ptr_array_new();
    GPtrArray *to_del = g_ptr_array_new();

    GHashTableIter it;
    gpointer key = NULL;
    gpointer val = NULL;

    g_hash_table_iter_init(&it, fav_set);
    while (g_hash_table_iter_next(&it, &key, NULL)) {
        const char *existing = (const char *)g_hash_table_lookup(existing_stickers, key);
        if (g_strcmp0(existing, "1") != 0) {
            g_ptr_array_add(to_set, key);
        }
    }

    g_hash_table_iter_init(&it, existing_stickers);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        if (!g_hash_table_contains(fav_set, key)) {
            g_ptr_array_add(to_del, key);
        }
    }

    if (to_set->len > 0 || to_del->len > 0) {
        if (!mpd_command_list_begin(conn, false)) {
            set_mpd_error(conn, err, "sticker command_list_begin failed");
            g_ptr_array_unref(to_set);
            g_ptr_array_unref(to_del);
            g_hash_table_destroy(existing_stickers);
            g_hash_table_destroy(fav_set);
            g_ptr_array_unref(fav_tracks);
            return false;
        }
        for (guint i = 0; i < to_set->len; i++) {
            const char *uri = (const char *)g_ptr_array_index(to_set, i);
            mpd_send_sticker_set(conn, "song", uri, FAV_STICKER, "1");
        }
        for (guint i = 0; i < to_del->len; i++) {
            const char *uri = (const char *)g_ptr_array_index(to_del, i);
            mpd_send_sticker_delete(conn, "song", uri, FAV_STICKER);
        }
        if (!mpd_command_list_end(conn) || !mpd_response_finish(conn)) {
            set_mpd_error(conn, err, "sticker sync failed");
            g_ptr_array_unref(to_set);
            g_ptr_array_unref(to_del);
            g_hash_table_destroy(existing_stickers);
            g_hash_table_destroy(fav_set);
            g_ptr_array_unref(fav_tracks);
            return false;
        }
    }

    if (out_added) *out_added = (int)to_set->len;
    if (out_removed) *out_removed = (int)to_del->len;

    g_ptr_array_unref(to_set);
    g_ptr_array_unref(to_del);
    g_hash_table_destroy(existing_stickers);
    g_hash_table_destroy(fav_set);
    g_ptr_array_unref(fav_tracks);
    return true;
}

GPtrArray *mpd_add_songs_to_playlists(
    struct mpd_connection *conn,
    GPtrArray *target_songs,
    GPtrArray *playlist_names,
    GError **err
) {
    GPtrArray *results = g_ptr_array_new_with_free_func(playlist_add_result_free);
    if (!target_songs || target_songs->len == 0 || !playlist_names || playlist_names->len == 0) {
        return results;
    }

    GPtrArray *validated = dedup_and_validate_playlists(playlist_names, err);
    if (!validated) {
        g_ptr_array_unref(results);
        return NULL;
    }

    GHashTable *existing_by_pl = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
    for (guint i = 0; i < validated->len; i++) {
        const char *pl = (const char *)g_ptr_array_index(validated, i);
        GPtrArray *tracks = fetch_single_playlist_tracks(conn, pl, err);
        if (!tracks) {
            g_hash_table_destroy(existing_by_pl);
            g_ptr_array_unref(validated);
            g_ptr_array_unref(results);
            return NULL;
        }
        g_hash_table_insert(existing_by_pl, g_strdup(pl), tracks);
    }

    bool has_cmds = false;
    bool fav_touched = false;

    typedef struct {
        const char *pl;
        const char *uri;
    } AddOp;
    GPtrArray *ops = g_ptr_array_new_with_free_func(g_free);

    for (guint i = 0; i < validated->len; i++) {
        const char *pl = (const char *)g_ptr_array_index(validated, i);
        if (g_strcmp0(pl, FAVOURITES_PLAYLIST) == 0) fav_touched = true;

        GPtrArray *tracks = (GPtrArray *)g_hash_table_lookup(existing_by_pl, pl);
        GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
        for (guint j = 0; tracks && j < tracks->len; j++) {
            g_hash_table_add(seen, g_ptr_array_index(tracks, j));
        }

        int added = 0;
        int skipped = 0;
        for (guint j = 0; j < target_songs->len; j++) {
            gpointer uri_ptr = g_ptr_array_index(target_songs, j);
            const char *uri = (const char *)uri_ptr;
            if (g_hash_table_contains(seen, uri)) {
                skipped++;
                continue;
            }
            g_hash_table_add(seen, uri_ptr);
            AddOp *op = g_new(AddOp, 1);
            op->pl = pl;
            op->uri = uri;
            g_ptr_array_add(ops, op);
            added++;
            has_cmds = true;
        }
        g_hash_table_destroy(seen);

        PlaylistAddResult *res = g_new0(PlaylistAddResult, 1);
        res->name = g_strdup(pl);
        res->added = added;
        res->skipped = skipped;
        g_ptr_array_add(results, res);
    }

    bool ok = true;
    if (has_cmds) {
        ok = mpd_command_list_begin(conn, false);
        for (guint i = 0; ok && i < ops->len; i++) {
            AddOp *op = (AddOp *)g_ptr_array_index(ops, i);
            ok = mpd_send_playlist_add(conn, op->pl, op->uri);
        }
        if (ok) ok = mpd_command_list_end(conn);
        if (ok) ok = mpd_response_finish(conn);
        if (!ok) {
            set_mpd_error(conn, err, "playlistadd failed");
        }
    }

    g_ptr_array_unref(ops);
    g_hash_table_destroy(existing_by_pl);
    g_ptr_array_unref(validated);

    if (!ok) {
        g_ptr_array_unref(results);
        return NULL;
    }

    if (fav_touched && !mpd_sync_favourites_stickers(conn, NULL, NULL, err)) {
        g_ptr_array_unref(results);
        return NULL;
    }

    return results;
}

GPtrArray *mpd_remove_songs_from_playlists(
    struct mpd_connection *conn,
    GPtrArray *target_songs,
    GPtrArray *playlist_names,
    GError **err
) {
    GPtrArray *results = g_ptr_array_new_with_free_func(playlist_remove_result_free);
    if (!target_songs || target_songs->len == 0 || !playlist_names || playlist_names->len == 0) {
        return results;
    }

    GPtrArray *validated = dedup_and_validate_playlists(playlist_names, err);
    if (!validated) {
        g_ptr_array_unref(results);
        return NULL;
    }

    GHashTable *existing_by_pl = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
    for (guint i = 0; i < validated->len; i++) {
        const char *pl = (const char *)g_ptr_array_index(validated, i);
        GPtrArray *tracks = fetch_single_playlist_tracks(conn, pl, err);
        if (!tracks) {
            g_hash_table_destroy(existing_by_pl);
            g_ptr_array_unref(validated);
            g_ptr_array_unref(results);
            return NULL;
        }
        g_hash_table_insert(existing_by_pl, g_strdup(pl), tracks);
    }

    GHashTable *target_set = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < target_songs->len; i++) {
        g_hash_table_add(target_set, g_ptr_array_index(target_songs, i));
    }

    typedef struct {
        const char *pl;
        unsigned pos;
    } DelOp;
    GPtrArray *ops = g_ptr_array_new_with_free_func(g_free);
    bool fav_touched = false;

    for (guint i = 0; i < validated->len; i++) {
        const char *pl = (const char *)g_ptr_array_index(validated, i);
        if (g_strcmp0(pl, FAVOURITES_PLAYLIST) == 0) fav_touched = true;

        GPtrArray *tracks = (GPtrArray *)g_hash_table_lookup(existing_by_pl, pl);
        int count = 0;
        /* Delete in descending position order so earlier indices remain stable */
        if (tracks && tracks->len > 0) {
            for (guint j = tracks->len; j > 0; j--) {
                guint idx = j - 1;
                const char *uri = (const char *)g_ptr_array_index(tracks, idx);
                if (g_hash_table_contains(target_set, uri)) {
                    DelOp *op = g_new(DelOp, 1);
                    op->pl = pl;
                    op->pos = idx;
                    g_ptr_array_add(ops, op);
                    count++;
                }
            }
        }

        PlaylistRemoveResult *res = g_new0(PlaylistRemoveResult, 1);
        res->name = g_strdup(pl);
        res->removed = count;
        g_ptr_array_add(results, res);
    }

    bool ok = true;
    if (ops->len > 0) {
        ok = mpd_command_list_begin(conn, false);
        for (guint i = 0; ok && i < ops->len; i++) {
            DelOp *op = (DelOp *)g_ptr_array_index(ops, i);
            ok = mpd_send_playlist_delete(conn, op->pl, op->pos);
        }
        if (ok) ok = mpd_command_list_end(conn);
        if (ok) ok = mpd_response_finish(conn);
        if (!ok) {
            set_mpd_error(conn, err, "playlistdelete failed");
        }
    }

    g_ptr_array_unref(ops);
    g_hash_table_destroy(target_set);
    g_hash_table_destroy(existing_by_pl);
    g_ptr_array_unref(validated);

    if (!ok) {
        g_ptr_array_unref(results);
        return NULL;
    }

    if (fav_touched && !mpd_sync_favourites_stickers(conn, NULL, NULL, err)) {
        g_ptr_array_unref(results);
        return NULL;
    }

    return results;
}

bool mpd_toggle_favourites(
    struct mpd_connection *conn,
    GPtrArray *target_songs,
    char **out_action,
    int *out_count,
    GError **err
) {
    if (out_action) *out_action = g_strdup("none");
    if (out_count) *out_count = 0;
    if (!target_songs || target_songs->len == 0) return true;

    GPtrArray *existing_tracks = fetch_single_playlist_tracks(conn, FAVOURITES_PLAYLIST, err);
    if (!existing_tracks) return false;

    GHashTable *existing_set = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < existing_tracks->len; i++) {
        g_hash_table_add(existing_set, g_ptr_array_index(existing_tracks, i));
    }

    bool all_present = true;
    for (guint i = 0; i < target_songs->len; i++) {
        if (!g_hash_table_contains(existing_set, g_ptr_array_index(target_songs, i))) {
            all_present = false;
            break;
        }
    }
    g_hash_table_destroy(existing_set);
    g_ptr_array_unref(existing_tracks);

    GPtrArray *pls = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(pls, g_strdup(FAVOURITES_PLAYLIST));

    if (all_present) {
        GPtrArray *res = mpd_remove_songs_from_playlists(conn, target_songs, pls, err);
        g_ptr_array_unref(pls);
        if (!res) return false;
        int removed = 0;
        if (res->len > 0) {
            PlaylistRemoveResult *r = (PlaylistRemoveResult *)g_ptr_array_index(res, 0);
            removed = r->removed;
        }
        g_ptr_array_unref(res);
        if (out_action) {
            g_free(*out_action);
            *out_action = g_strdup("removed");
        }
        if (out_count) *out_count = removed;
        return true;
    }

    GPtrArray *res = mpd_add_songs_to_playlists(conn, target_songs, pls, err);
    g_ptr_array_unref(pls);
    if (!res) return false;
    int added = 0;
    if (res->len > 0) {
        PlaylistAddResult *r = (PlaylistAddResult *)g_ptr_array_index(res, 0);
        added = r->added;
    }
    g_ptr_array_unref(res);
    if (out_action) {
        g_free(*out_action);
        *out_action = g_strdup("added");
    }
    if (out_count) *out_count = added;
    return true;
}

bool mpd_append_uris(struct mpd_connection *conn, GPtrArray *uris, GError **err) {
    if (!uris || uris->len == 0) return true;
    if (!mpd_command_list_begin(conn, false)) {
        set_mpd_error(conn, err, "queue add begin failed");
        return false;
    }
    for (guint i = 0; i < uris->len; i++) {
        const char *uri = (const char *)g_ptr_array_index(uris, i);
        if (!mpd_send_add(conn, uri)) {
            set_mpd_error(conn, err, "queue add failed");
            return false;
        }
    }
    if (!mpd_command_list_end(conn) || !mpd_response_finish(conn)) {
        set_mpd_error(conn, err, "queue add finish failed");
        return false;
    }
    return true;
}

bool mpd_append_and_play_uris(struct mpd_connection *conn, GPtrArray *uris, GError **err) {
    if (!uris || uris->len == 0) return true;
    struct mpd_status *st = mpd_run_status(conn);
    unsigned start_pos = 0;
    if (st) {
        start_pos = mpd_status_get_queue_length(st);
        mpd_status_free(st);
    } else if (mpd_connection_get_error(conn) != MPD_ERROR_SUCCESS) {
        mpd_connection_clear_error(conn);
    }

    if (!mpd_append_uris(conn, uris, err)) return false;
    if (!mpd_run_play_pos(conn, start_pos)) {
        set_mpd_error(conn, err, "play appended failed");
        return false;
    }
    return true;
}

bool mpd_replace_and_play_uris(struct mpd_connection *conn, GPtrArray *uris, GError **err) {
    if (!uris || uris->len == 0) return true;
    if (!mpd_command_list_begin(conn, false)) {
        set_mpd_error(conn, err, "replace queue begin failed");
        return false;
    }
    if (!mpd_send_clear(conn)) {
        set_mpd_error(conn, err, "queue clear failed");
        return false;
    }
    for (guint i = 0; i < uris->len; i++) {
        const char *uri = (const char *)g_ptr_array_index(uris, i);
        if (!mpd_send_add(conn, uri)) {
            set_mpd_error(conn, err, "queue add failed");
            return false;
        }
    }
    if (!mpd_send_play_pos(conn, 0)) {
        set_mpd_error(conn, err, "play pos 0 failed");
        return false;
    }
    if (!mpd_command_list_end(conn) || !mpd_response_finish(conn)) {
        set_mpd_error(conn, err, "replace & play failed");
        return false;
    }
    return true;
}

bool mpd_insert_uris_next(struct mpd_connection *conn, GPtrArray *uris, GError **err) {
    if (!uris || uris->len == 0) return true;
    struct mpd_status *st = mpd_run_status(conn);
    int cur_pos = -1;
    if (st) {
        enum mpd_state state = mpd_status_get_state(st);
        if (state == MPD_STATE_PLAY || state == MPD_STATE_PAUSE) {
            cur_pos = mpd_status_get_song_pos(st);
        }
        mpd_status_free(st);
    } else if (mpd_connection_get_error(conn) != MPD_ERROR_SUCCESS) {
        mpd_connection_clear_error(conn);
    }

    if (cur_pos < 0) {
        return mpd_append_uris(conn, uris, err);
    }

    if (!mpd_command_list_begin(conn, false)) {
        set_mpd_error(conn, err, "insert next begin failed");
        return false;
    }
    unsigned target_pos = (unsigned)cur_pos + 1;
    for (guint i = 0; i < uris->len; i++) {
        const char *uri = (const char *)g_ptr_array_index(uris, i);
        if (!mpd_send_add_id_to(conn, uri, target_pos + i)) {
            set_mpd_error(conn, err, "insert next send failed");
            return false;
        }
    }
    if (!mpd_command_list_end(conn) || !mpd_response_finish(conn)) {
        set_mpd_error(conn, err, "insert next finish failed");
        return false;
    }
    return true;
}

unsigned long mpd_get_db_update_time(struct mpd_connection *conn) {
    if (!conn) return 0;
    struct mpd_stats *st = mpd_run_stats(conn);
    if (!st) {
        mpd_connection_clear_error(conn);
        return 0;
    }
    unsigned long t = mpd_stats_get_db_update_time(st);
    mpd_stats_free(st);
    return t;
}

static unsigned parse_leading_uint(const char *s) {
    if (!s || !*s) return 0;
    unsigned long val = strtoul(s, NULL, 10);
    return (unsigned)val;
}

static const char *arena_str(GStringChunk *arena, const char *s) {
    return g_string_chunk_insert_const(arena, (s && *s) ? s : "");
}

static guint ensure_folder(LibIndex *idx, const char *dir_path) {
    gpointer found = NULL;
    if (g_hash_table_lookup_extended(idx->folder_by_path, dir_path, NULL, &found)) {
        return GPOINTER_TO_UINT(found);
    }

    guint parent_idx = 0;
    const char *parent_path = "";
    const char *name = "";
    if (*dir_path != '\0') {
        const char *slash = strrchr(dir_path, '/');
        if (slash) {
            char *p_tmp = g_strndup(dir_path, (gsize)(slash - dir_path));
            parent_idx = ensure_folder(idx, p_tmp);
            parent_path = ((LibFolder *)g_ptr_array_index(idx->folders, parent_idx))->path;
            g_free(p_tmp);
            name = slash + 1;
        } else {
            parent_idx = 0;
            parent_path = "";
            name = dir_path;
        }
    }

    LibFolder *f = g_new0(LibFolder, 1);
    f->path = arena_str(idx->arena, dir_path);
    f->name = arena_str(idx->arena, *dir_path ? name : "Library");
    f->parent_path = arena_str(idx->arena, parent_path);
    f->child_dir_indices = g_array_new(FALSE, FALSE, sizeof(guint));
    f->track_indices = g_array_new(FALSE, FALSE, sizeof(guint));

    guint my_idx = idx->folders->len;
    g_ptr_array_add(idx->folders, f);
    g_hash_table_insert(idx->folder_by_path, (gpointer)f->path, GUINT_TO_POINTER(my_idx));

    if (*dir_path != '\0') {
        LibFolder *parent = (LibFolder *)g_ptr_array_index(idx->folders, parent_idx);
        g_array_append_val(parent->child_dir_indices, my_idx);
    }
    return my_idx;
}

static int path_depth(const char *p) {
    if (!p || !*p) return 0;
    int d = 1;
    for (const char *c = p; *c; c++) {
        if (*c == '/') d++;
    }
    return d;
}

static bool is_disc_dir_name(const char *name) {
    if (!name || !*name) return false;
    char *low = g_utf8_casefold(name, -1);
    const char *p = low;
    if (g_str_has_prefix(p, "disc")) {
        p += 4;
    } else if (g_str_has_prefix(p, "disk")) {
        p += 4;
    } else if (g_str_has_prefix(p, "cd")) {
        p += 2;
    } else {
        g_free(low);
        return false;
    }
    while (*p == ' ' || *p == '_' || *p == '-' || *p == '.') p++;
    if (!isdigit((unsigned char)*p)) {
        g_free(low);
        return false;
    }
    while (isdigit((unsigned char)*p)) p++;
    while (*p == ' ') p++;
    bool ok = (*p == '\0');
    g_free(low);
    return ok;
}

static LibIndex *g_sort_idx_ctx = NULL;

static gint cmp_folder_idx_by_name(gconstpointer a, gconstpointer b) {
    guint ia = *(const guint *)a;
    guint ib = *(const guint *)b;
    const LibFolder *fa = (const LibFolder *)g_ptr_array_index(g_sort_idx_ctx->folders, ia);
    const LibFolder *fb = (const LibFolder *)g_ptr_array_index(g_sort_idx_ctx->folders, ib);
    char *ca = g_utf8_casefold(fa->name, -1);
    char *cb = g_utf8_casefold(fb->name, -1);
    int cmp = g_strcmp0(ca, cb);
    if (cmp == 0) cmp = g_strcmp0(fa->name, fb->name);
    g_free(ca);
    g_free(cb);
    return cmp;
}

static gint cmp_track_idx_by_order(gconstpointer a, gconstpointer b) {
    guint ia = *(const guint *)a;
    guint ib = *(const guint *)b;
    const LibTrack *ta = (const LibTrack *)g_ptr_array_index(g_sort_idx_ctx->tracks, ia);
    const LibTrack *tb = (const LibTrack *)g_ptr_array_index(g_sort_idx_ctx->tracks, ib);
    if (ta->disc_num != tb->disc_num) return (ta->disc_num < tb->disc_num) ? -1 : 1;
    if (ta->track_num != tb->track_num) return (ta->track_num < tb->track_num) ? -1 : 1;
    int cmp = g_strcmp0(ta->uri, tb->uri);
    return cmp;
}

static gint cmp_folder_depth_desc(gconstpointer a, gconstpointer b) {
    guint ia = *(const guint *)a;
    guint ib = *(const guint *)b;
    const LibFolder *fa = (const LibFolder *)g_ptr_array_index(g_sort_idx_ctx->folders, ia);
    const LibFolder *fb = (const LibFolder *)g_ptr_array_index(g_sort_idx_ctx->folders, ib);
    int da = path_depth(fa->path);
    int db = path_depth(fb->path);
    if (da != db) return (db > da) ? 1 : -1;
    return g_strcmp0(fa->path, fb->path);
}

static void classify_folder_node(LibIndex *idx, LibFolder *f) {
    if (g_strcmp0(f->path, "") == 0) {
        /* Root is always browsable container; roll up counts */
        guint total = f->track_indices->len;
        guint albums = 0;
        bool fav = false;
        for (guint i = 0; i < f->track_indices->len; i++) {
            const LibTrack *t = (const LibTrack *)g_ptr_array_index(
                idx->tracks, g_array_index(f->track_indices, guint, i));
            if (t->is_fav) fav = true;
        }
        for (guint i = 0; i < f->child_dir_indices->len; i++) {
            const LibFolder *c = (const LibFolder *)g_ptr_array_index(
                idx->folders, g_array_index(f->child_dir_indices, guint, i));
            total += c->recursive_track_count;
            albums += c->is_single_album ? 1 : c->album_count;
            if (c->has_fav) fav = true;
        }
        f->recursive_track_count = total;
        f->album_count = albums;
        f->has_fav = fav;
        f->search_blob = arena_str(idx->arena, "library");
        return;
    }

    /* Case 1: direct tracks only, no subdirectories -> check single album */
    if (f->track_indices->len > 0 && f->child_dir_indices->len == 0) {
        const char *common_album = NULL;
        char *common_album_cf = NULL;
        bool same_album = true;

        const char *common_artist = NULL;
        bool same_artist = true;
        const char *common_date = "";
        bool fav = false;

        for (guint i = 0; i < f->track_indices->len; i++) {
            const LibTrack *t = (const LibTrack *)g_ptr_array_index(
                idx->tracks, g_array_index(f->track_indices, guint, i));
            if (t->is_fav) fav = true;
            if (*t->date && !*common_date) common_date = t->date;

            const char *alb = (*t->album) ? t->album : NULL;
            if (!alb) {
                same_album = false;
            } else if (!common_album) {
                common_album = alb;
                common_album_cf = g_utf8_casefold(alb, -1);
            } else {
                char *cf = g_utf8_casefold(alb, -1);
                if (g_strcmp0(cf, common_album_cf) != 0) same_album = false;
                g_free(cf);
            }

            const char *art = (*t->album_artist) ? t->album_artist : ((*t->artist) ? t->artist : NULL);
            if (art) {
                if (!common_artist) {
                    common_artist = art;
                } else if (g_ascii_strcasecmp(common_artist, art) != 0) {
                    same_artist = false;
                }
            }
        }
        g_free(common_album_cf);

        const LibTrack *first_t = (const LibTrack *)g_ptr_array_index(
            idx->tracks, g_array_index(f->track_indices, guint, 0));

        f->recursive_track_count = f->track_indices->len;
        f->has_fav = fav;

        if (same_album && common_album) {
            f->is_single_album = true;
            f->album_name = arena_str(idx->arena, common_album);
            f->artist_name = arena_str(idx->arena, same_artist && common_artist ? common_artist : "Various Artists");
            f->date = arena_str(idx->arena, common_date);
            f->art_uri = first_t->uri;
            f->album_count = 1;
        } else {
            f->artist_name = arena_str(idx->arena, same_artist && common_artist ? common_artist : "");
            f->date = arena_str(idx->arena, common_date);
        }
    } else if (f->track_indices->len == 0 && f->child_dir_indices->len > 0) {
        /* Case 2: parent of only disc subfolders sharing one Album tag */
        bool all_disc_albums = true;
        const char *common_album = NULL;
        char *common_album_cf = NULL;
        const char *common_artist = NULL;
        bool same_artist = true;
        const char *common_date = "";
        const char *first_art_uri = NULL;
        guint total_tracks = 0;
        guint total_albums = 0;
        bool fav = false;

        for (guint i = 0; i < f->child_dir_indices->len; i++) {
            const LibFolder *c = (const LibFolder *)g_ptr_array_index(
                idx->folders, g_array_index(f->child_dir_indices, guint, i));
            total_tracks += c->recursive_track_count;
            total_albums += c->is_single_album ? 1 : c->album_count;
            if (c->has_fav) fav = true;

            if (!c->is_single_album || !is_disc_dir_name(c->name)) {
                all_disc_albums = false;
                continue;
            }
            if (!first_art_uri && c->art_uri) first_art_uri = c->art_uri;
            if (c->date && *c->date && !*common_date) common_date = c->date;

            if (!common_album) {
                common_album = c->album_name;
                common_album_cf = g_utf8_casefold(c->album_name, -1);
            } else {
                char *cf = g_utf8_casefold(c->album_name, -1);
                if (g_strcmp0(cf, common_album_cf) != 0) all_disc_albums = false;
                g_free(cf);
            }
            if (c->artist_name && *c->artist_name) {
                if (!common_artist) {
                    common_artist = c->artist_name;
                } else if (g_ascii_strcasecmp(common_artist, c->artist_name) != 0) {
                    same_artist = false;
                }
            }
        }
        g_free(common_album_cf);

        f->recursive_track_count = total_tracks;
        f->has_fav = fav;

        if (all_disc_albums && common_album && total_tracks > 0) {
            f->is_single_album = true;
            f->is_multidisc = true;
            f->album_name = arena_str(idx->arena, common_album);
            f->artist_name = arena_str(idx->arena, same_artist && common_artist ? common_artist : "Various Artists");
            f->date = arena_str(idx->arena, common_date);
            f->art_uri = first_art_uri;
            f->album_count = 1;

            /* Flatten disc tracks in disc/track order onto the parent multi-disc album */
            for (guint i = 0; i < f->child_dir_indices->len; i++) {
                const LibFolder *c = (const LibFolder *)g_ptr_array_index(
                    idx->folders, g_array_index(f->child_dir_indices, guint, i));
                for (guint j = 0; j < c->track_indices->len; j++) {
                    guint tidx = g_array_index(c->track_indices, guint, j);
                    g_array_append_val(f->track_indices, tidx);
                }
            }
            g_sort_idx_ctx = idx;
            g_array_sort(f->track_indices, cmp_track_idx_by_order);
        } else {
            f->album_count = total_albums;
        }
    } else {
        /* Case 3: mixed direct tracks + subdirectories */
        guint total = f->track_indices->len;
        guint albums = 0;
        bool fav = false;
        for (guint i = 0; i < f->track_indices->len; i++) {
            const LibTrack *t = (const LibTrack *)g_ptr_array_index(
                idx->tracks, g_array_index(f->track_indices, guint, i));
            if (t->is_fav) fav = true;
        }
        for (guint i = 0; i < f->child_dir_indices->len; i++) {
            const LibFolder *c = (const LibFolder *)g_ptr_array_index(
                idx->folders, g_array_index(f->child_dir_indices, guint, i));
            total += c->recursive_track_count;
            albums += c->is_single_album ? 1 : c->album_count;
            if (c->has_fav) fav = true;
        }
        f->recursive_track_count = total;
        f->album_count = albums;
        f->has_fav = fav;
    }

    char *raw_blob = g_strdup_printf(
        "%s %s %s %s %s",
        f->name ? f->name : "",
        f->album_name ? f->album_name : "",
        f->artist_name ? f->artist_name : "",
        f->date ? f->date : "",
        f->path ? f->path : ""
    );
    char *cf_blob = g_utf8_casefold(raw_blob, -1);
    f->search_blob = arena_str(idx->arena, cf_blob);
    g_free(cf_blob);
    g_free(raw_blob);
}

static GHashTable *fetch_fav_uri_set(struct mpd_connection *conn) {
    GHashTable *favs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    if (!mpd_send_sticker_find(conn, "song", "", FAV_STICKER)) {
        mpd_connection_clear_error(conn);
        return favs;
    }
    char *cur_file = NULL;
    struct mpd_pair *pair = NULL;
    while ((pair = mpd_recv_pair(conn)) != NULL) {
        if (g_strcmp0(pair->name, "file") == 0) {
            g_free(cur_file);
            cur_file = g_strdup(pair->value);
        } else if (g_strcmp0(pair->name, "sticker") == 0 && cur_file) {
            size_t name_len = 0;
            const char *val = mpd_parse_sticker(pair->value, &name_len);
            if (val && name_len == strlen(FAV_STICKER) &&
                strncmp(pair->value, FAV_STICKER, name_len) == 0 &&
                g_strcmp0(val, "1") == 0) {
                g_hash_table_add(favs, g_strdup(cur_file));
            }
        }
        mpd_return_pair(conn, pair);
    }
    g_free(cur_file);
    if (!mpd_response_finish(conn)) {
        mpd_connection_clear_error(conn);
    }
    return favs;
}

LibIndex *mpd_fetch_library_index(struct mpd_connection *conn, GError **err) {
    if (!conn) return NULL;

    unsigned long db_time = mpd_get_db_update_time(conn);
    GHashTable *fav_set = fetch_fav_uri_set(conn);

    if (!mpd_send_list_all_meta(conn, "")) {
        set_mpd_error(conn, err, "listallinfo send failed");
        g_hash_table_destroy(fav_set);
        return NULL;
    }

    LibIndex *idx = g_new0(LibIndex, 1);
    idx->db_update_time = db_time;
    idx->arena = g_string_chunk_new(16384);
    idx->tracks = g_ptr_array_new_with_free_func(g_free);
    idx->folders = g_ptr_array_new_with_free_func(lib_folder_free);
    idx->folder_by_path = g_hash_table_new(g_str_hash, g_str_equal);
    idx->track_by_uri = g_hash_table_new(g_str_hash, g_str_equal);

    ensure_folder(idx, "");

    struct mpd_entity *ent = NULL;
    while ((ent = mpd_recv_entity(conn)) != NULL) {
        enum mpd_entity_type etype = mpd_entity_get_type(ent);
        if (etype == MPD_ENTITY_TYPE_DIRECTORY) {
            const struct mpd_directory *d = mpd_entity_get_directory(ent);
            const char *dpath = mpd_directory_get_path(d);
            if (dpath && *dpath) ensure_folder(idx, dpath);
        } else if (etype == MPD_ENTITY_TYPE_SONG) {
            const struct mpd_song *s = mpd_entity_get_song(ent);
            const char *uri = mpd_song_get_uri(s);
            if (uri && *uri) {
                const char *slash = strrchr(uri, '/');
                char *dir_tmp = slash ? g_strndup(uri, (gsize)(slash - uri)) : g_strdup("");
                guint f_idx = ensure_folder(idx, dir_tmp);
                g_free(dir_tmp);

                LibFolder *folder = (LibFolder *)g_ptr_array_index(idx->folders, f_idx);

                const char *raw_title = mpd_song_get_tag(s, MPD_TAG_TITLE, 0);
                char *stem = (!raw_title || !*raw_title) ? path_stem(uri) : NULL;
                const char *title_in = (raw_title && *raw_title) ? raw_title : stem;

                const char *artist_in = mpd_song_get_tag(s, MPD_TAG_ARTIST, 0);
                const char *aa_in = mpd_song_get_tag(s, MPD_TAG_ALBUM_ARTIST, 0);
                const char *album_in = mpd_song_get_tag(s, MPD_TAG_ALBUM, 0);
                const char *genre_in = mpd_song_get_tag(s, MPD_TAG_GENRE, 0);
                const char *date_in = mpd_song_get_tag(s, MPD_TAG_DATE, 0);

                LibTrack *t = g_new0(LibTrack, 1);
                t->uri = arena_str(idx->arena, uri);
                t->dir = folder->path;
                t->title = arena_str(idx->arena, title_in);
                t->artist = arena_str(idx->arena, artist_in);
                t->album_artist = arena_str(idx->arena, aa_in);
                t->album = arena_str(idx->arena, album_in);
                t->genre = arena_str(idx->arena, genre_in);
                t->date = arena_str(idx->arena, date_in);
                t->track_num = parse_leading_uint(mpd_song_get_tag(s, MPD_TAG_TRACK, 0));
                t->disc_num = parse_leading_uint(mpd_song_get_tag(s, MPD_TAG_DISC, 0));
                t->duration_s = mpd_song_get_duration(s);
                t->is_fav = g_hash_table_contains(fav_set, t->uri);

                char *raw_blob = g_strdup_printf(
                    "%s %s %s %s %s %s %s",
                    t->title, t->artist, t->album_artist, t->album, t->genre, t->date, t->dir
                );
                char *cf_blob = g_utf8_casefold(raw_blob, -1);
                t->search_blob = arena_str(idx->arena, cf_blob);
                g_free(cf_blob);
                g_free(raw_blob);
                g_free(stem);

                guint tidx = idx->tracks->len;
                g_ptr_array_add(idx->tracks, t);
                g_hash_table_insert(idx->track_by_uri, (gpointer)t->uri, t);
                g_array_append_val(folder->track_indices, tidx);
            }
        }
        mpd_entity_free(ent);
    }

    bool ok = mpd_response_finish(conn);
    g_hash_table_destroy(fav_set);

    if (!ok) {
        set_mpd_error(conn, err, "listallinfo failed");
        lib_index_free(idx);
        return NULL;
    }

    /* Sort children & direct tracks per folder */
    g_sort_idx_ctx = idx;
    GArray *order = g_array_sized_new(FALSE, FALSE, sizeof(guint), idx->folders->len);
    for (guint i = 0; i < idx->folders->len; i++) {
        LibFolder *f = (LibFolder *)g_ptr_array_index(idx->folders, i);
        if (f->child_dir_indices->len > 1) {
            g_array_sort(f->child_dir_indices, cmp_folder_idx_by_name);
        }
        if (f->track_indices->len > 1) {
            g_array_sort(f->track_indices, cmp_track_idx_by_order);
        }
        g_array_append_val(order, i);
    }

    /* Classify bottom-up (deepest folder first) so multi-disc albums fold cleanly */
    g_array_sort(order, cmp_folder_depth_desc);
    for (guint i = 0; i < order->len; i++) {
        guint f_idx = g_array_index(order, guint, i);
        LibFolder *f = (LibFolder *)g_ptr_array_index(idx->folders, f_idx);
        classify_folder_node(idx, f);
    }
    g_array_unref(order);
    g_sort_idx_ctx = NULL;

    return idx;
}

void mpd_library_index_refresh_favs(struct mpd_connection *conn, LibIndex *idx) {
    if (!conn || !idx) return;
    GHashTable *fav_set = fetch_fav_uri_set(conn);
    for (guint i = 0; i < idx->tracks->len; i++) {
        LibTrack *t = (LibTrack *)g_ptr_array_index(idx->tracks, i);
        t->is_fav = g_hash_table_contains(fav_set, t->uri);
    }
    for (guint i = 0; i < idx->folders->len; i++) {
        LibFolder *f = (LibFolder *)g_ptr_array_index(idx->folders, i);
        bool fav = false;
        for (guint j = 0; j < f->track_indices->len; j++) {
            const LibTrack *t = (const LibTrack *)g_ptr_array_index(
                idx->tracks, g_array_index(f->track_indices, guint, j));
            if (t->is_fav) {
                fav = true;
                break;
            }
        }
        f->has_fav = fav;
    }
    g_hash_table_destroy(fav_set);
}

static const char *const LOCAL_COVER_NAMES[] = {
    "cover.jpg", "cover.jpeg", "cover.png", "cover.webp",
    "folder.jpg", "folder.jpeg", "folder.png", "folder.webp",
    "front.jpg", "front.jpeg", "front.png", "front.webp",
    "album.jpg", "album.png",
    "Cover.jpg", "Cover.png", "Folder.jpg", "Folder.png", "Front.jpg", "Front.png",
    NULL
};

static char *find_local_cover_in_dir(const char *abs_dir) {
    if (!abs_dir || !*abs_dir) return NULL;
    for (int i = 0; LOCAL_COVER_NAMES[i] != NULL; i++) {
        char *cand = g_build_filename(abs_dir, LOCAL_COVER_NAMES[i], NULL);
        if (g_file_test(cand, G_FILE_TEST_IS_REGULAR)) {
            return cand;
        }
        g_free(cand);
    }
    return NULL;
}

bool mpd_dir_has_local_cover(const char *music_dir, const char *dir_rel) {
    if (!music_dir || !dir_rel) return false;
    char *abs_dir = g_build_filename(music_dir, dir_rel, NULL);
    char *cover = find_local_cover_in_dir(abs_dir);
    g_free(abs_dir);
    if (!cover) return false;
    g_free(cover);
    return true;
}

static GBytes *read_file_bounded(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0 || st.st_size > MAX_COVER_BYTES) {
        return NULL;
    }
    gchar *data = NULL;
    gsize len = 0;
    if (!g_file_get_contents(path, &data, &len, NULL) || len == 0) {
        g_free(data);
        return NULL;
    }
    return g_bytes_new_take(data, len);
}

static GBytes *fetch_mpd_art_chunks(struct mpd_connection *conn, const char *uri, bool use_albumart) {
    if (!conn || !uri || !*uri) return NULL;
    GByteArray *acc = g_byte_array_new();
    unsigned char buf[65536];
    unsigned offset = 0;

    while (acc->len < MAX_COVER_BYTES) {
        int n = use_albumart
            ? mpd_run_albumart(conn, uri, offset, buf, sizeof(buf))
            : mpd_run_readpicture(conn, uri, offset, buf, sizeof(buf));
        if (n < 0) {
            mpd_connection_clear_error(conn);
            if (acc->len == 0) {
                g_byte_array_unref(acc);
                return NULL;
            }
            break;
        }
        if (n == 0) break;
        g_byte_array_append(acc, buf, (guint)n);
        offset += (unsigned)n;
    }

    if (acc->len == 0) {
        g_byte_array_unref(acc);
        return NULL;
    }
    return g_byte_array_free_to_bytes(acc);
}

GBytes *mpd_fetch_cover_bytes(
    struct mpd_connection *conn_opt,
    const char *music_dir,
    const char *song_uri
) {
    if (!song_uri || !*song_uri) return NULL;

    /* 1. Fast local filesystem lookup (also checks parent folder for disc subfolders) */
    if (music_dir && *music_dir && !g_str_has_prefix(song_uri, "http://") && !g_str_has_prefix(song_uri, "https://")) {
        char *rel_dir = g_path_get_dirname(song_uri);
        if (rel_dir && g_strcmp0(rel_dir, ".") != 0) {
            char *abs_dir = g_build_filename(music_dir, rel_dir, NULL);
            char *local_cover = find_local_cover_in_dir(abs_dir);
            if (!local_cover) {
                const char *base_dir = strrchr(rel_dir, '/');
                const char *leaf = base_dir ? (base_dir + 1) : rel_dir;
                if (base_dir && is_disc_dir_name(leaf)) {
                    char *parent_rel = g_strndup(rel_dir, (gsize)(base_dir - rel_dir));
                    char *parent_abs = g_build_filename(music_dir, parent_rel, NULL);
                    local_cover = find_local_cover_in_dir(parent_abs);
                    g_free(parent_abs);
                    g_free(parent_rel);
                }
            }
            g_free(abs_dir);
            if (local_cover) {
                GBytes *bytes = read_file_bounded(local_cover);
                g_free(local_cover);
                g_free(rel_dir);
                if (bytes) return bytes;
            }
        }
        g_free(rel_dir);
    }

    /* 2. MPD readpicture (embedded) then albumart (folder fallback) */
    struct mpd_connection *owned = NULL;
    struct mpd_connection *conn = conn_opt;
    if (!conn) {
        owned = mpd_open_configured(NULL);
        conn = owned;
    }
    if (!conn) return NULL;

    GBytes *bytes = fetch_mpd_art_chunks(conn, song_uri, false);
    if (!bytes) {
        bytes = fetch_mpd_art_chunks(conn, song_uri, true);
    }

    if (owned) mpd_connection_free(owned);
    return bytes;
}

char *mpd_format_add_status(GPtrArray *results) {
    int total_added = 0;
    int total_skipped = 0;
    GString *names = g_string_new("");

    for (guint i = 0; results && i < results->len; i++) {
        PlaylistAddResult *r = (PlaylistAddResult *)g_ptr_array_index(results, i);
        total_added += r->added;
        total_skipped += r->skipped;
        if (i < 3) {
            if (i > 0) g_string_append(names, ", ");
            if (g_strcmp0(r->name, FAVOURITES_PLAYLIST) == 0) {
                g_string_append(names, "★ favourites");
            } else {
                g_string_append(names, r->name);
            }
        }
    }
    if (results && results->len > 3) {
        g_string_append(names, "…");
    }

    char *msg = NULL;
    if (total_added == 0 && total_skipped > 0) {
        msg = g_strdup_printf("Already in %s", names->str);
    } else if (total_skipped > 0) {
        msg = g_strdup_printf("Added %d (+%d dupes skipped) → %s", total_added, total_skipped, names->str);
    } else {
        msg = g_strdup_printf("Added %d to %s", total_added, names->str);
    }
    g_string_free(names, TRUE);
    return msg;
}

char *mpd_format_remove_status(GPtrArray *results) {
    int total_removed = 0;
    GString *names = g_string_new("");
    for (guint i = 0; results && i < results->len; i++) {
        PlaylistRemoveResult *r = (PlaylistRemoveResult *)g_ptr_array_index(results, i);
        total_removed += r->removed;
        if (i > 0) g_string_append(names, ", ");
        g_string_append(names, r->name);
    }
    char *msg = g_strdup_printf("Removed %d entries from %s", total_removed, names->str);
    g_string_free(names, TRUE);
    return msg;
}

static bool is_digits(const char *s) {
    if (!s || !*s) return false;
    for (const char *p = s; *p; p++) {
        if (!isdigit((unsigned char)*p)) return false;
    }
    return true;
}

void rmpc_notify(const char *pid_str, const char *level, int timeout_ms, const char *message) {
    if (!message) return;
    char *timeout_str = g_strdup_printf("%d", timeout_ms);
    const char *lvl = (level && *level) ? level : "info";

    const char *argv[10];
    int idx = 0;
    argv[idx++] = "rmpc";
    argv[idx++] = "remote";
    if (is_digits(pid_str)) {
        argv[idx++] = "--pid";
        argv[idx++] = pid_str;
    }
    argv[idx++] = "status";
    argv[idx++] = "-l";
    argv[idx++] = lvl;
    argv[idx++] = "-t";
    argv[idx++] = timeout_str;
    argv[idx++] = message;
    argv[idx] = NULL;

    g_spawn_async(
        NULL,
        (gchar **)argv,
        NULL,
        G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
        NULL,
        NULL,
        NULL,
        NULL
    );
    g_free(timeout_str);
}
