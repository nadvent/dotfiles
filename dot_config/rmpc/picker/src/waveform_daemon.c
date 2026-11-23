#include "waveform_daemon.h"
#include "mpd_client.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define DEFAULT_WAVE_WIDTH 38
#define MAX_WAVE_WIDTH 128
#define MAX_PCM_BYTES (32u * 1024u * 1024u)

#define STICKER_DONE "waveform_done"
#define STICKER_HEAD "waveform_head"
#define STICKER_TODO "waveform_todo"

static const char *const WAVE_BARS[9] = {
    "▁", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"
};

static int g_wake_pipe[2] = {-1, -1};
static volatile sig_atomic_t g_running = 1;

static void on_term_signal(int sig) {
    (void)sig;
    g_running = 0;
    if (g_wake_pipe[1] >= 0) {
        char b = 1;
        ssize_t ignored = write(g_wake_pipe[1], &b, 1);
        (void)ignored;
    }
}

static int get_wave_width(void) {
    const char *env = g_getenv("WAVEFORM_WIDTH");
    if (!env || !*env) return DEFAULT_WAVE_WIDTH;
    char *endptr = NULL;
    long v = strtol(env, &endptr, 10);
    if (endptr && *endptr == '\0' && v >= 8 && v <= MAX_WAVE_WIDTH) {
        return (int)v;
    }
    return DEFAULT_WAVE_WIDTH;
}

static char *get_cache_dir(void) {
    const char *env = g_getenv("WAVEFORM_CACHE_DIR");
    const char *dir = (env && *env) ? env : "/tmp/rmpc_waveforms";
    g_mkdir_with_parents(dir, 0755);
    return g_strdup(dir);
}

static char *resolve_audio_file(const char *rel_file, const char *music_dir) {
    if (!rel_file || !*rel_file) return NULL;
    if (g_str_has_prefix(rel_file, "http://") || g_str_has_prefix(rel_file, "https://")) {
        return NULL;
    }

    const char *cue_sep = strstr(rel_file, ".cue/");
    if (cue_sep && music_dir && *music_dir) {
        char *cue_rel = g_strndup(rel_file, (gsize)(cue_sep - rel_file));
        char *cue_file_rel = g_strconcat(cue_rel, ".cue", NULL);
        char *cue_path = g_build_filename(music_dir, cue_file_rel, NULL);
        g_free(cue_file_rel);

        if (g_file_test(cue_path, G_FILE_TEST_IS_REGULAR)) {
            char *cue_dir = g_path_get_dirname(cue_path);
            gchar *cue_raw = NULL;
            if (g_file_get_contents(cue_path, &cue_raw, NULL, NULL)) {
                GRegex *re = g_regex_new("^\\s*FILE\\s+\"([^\"]+)\"", G_REGEX_MULTILINE | G_REGEX_CASELESS, 0, NULL);
                GMatchInfo *mi = NULL;
                if (re && g_regex_match(re, cue_raw, 0, &mi)) {
                    char *target = g_match_info_fetch(mi, 1);
                    char *cand = g_build_filename(cue_dir, target, NULL);
                    g_free(target);
                    if (g_file_test(cand, G_FILE_TEST_IS_REGULAR)) {
                        g_match_info_free(mi);
                        g_regex_unref(re);
                        g_free(cue_raw);
                        g_free(cue_dir);
                        g_free(cue_path);
                        g_free(cue_rel);
                        return cand;
                    }
                    g_free(cand);
                }
                g_clear_pointer(&mi, g_match_info_free);
                g_clear_pointer(&re, g_regex_unref);
                g_free(cue_raw);
            }
            g_free(cue_dir);

            static const char *const exts[] = {".flac", ".wav", ".ape", ".wv", ".mp3", ".m4a", ".ogg", NULL};
            char *base_abs = g_build_filename(music_dir, cue_rel, NULL);
            for (int i = 0; exts[i]; i++) {
                char *cand = g_strconcat(base_abs, exts[i], NULL);
                if (g_file_test(cand, G_FILE_TEST_IS_REGULAR)) {
                    g_free(base_abs);
                    g_free(cue_path);
                    g_free(cue_rel);
                    return cand;
                }
                g_free(cand);
            }
            g_free(base_abs);
        }
        g_free(cue_path);
        g_free(cue_rel);
    }

    if (music_dir && *music_dir) {
        char *full = g_build_filename(music_dir, rel_file, NULL);
        if (g_file_test(full, G_FILE_TEST_IS_REGULAR)) {
            return full;
        }
        g_free(full);
    }

    if (g_path_is_absolute(rel_file) && g_file_test(rel_file, G_FILE_TEST_IS_REGULAR)) {
        return g_strdup(rel_file);
    }

    return NULL;
}

static bool load_cached_peaks(const char *cache_file, int *peaks, int width) {
    gchar *raw = NULL;
    gsize len = 0;
    if (!g_file_get_contents(cache_file, &raw, &len, NULL)) {
        return false;
    }

    int count = 0;
    const char *p = raw;
    while (*p && count < width) {
        while (*p && !isdigit((unsigned char)*p)) p++;
        if (!*p) break;
        char *endptr = NULL;
        long val = strtol(p, &endptr, 10);
        if (endptr == p) break;
        if (val < 0) val = 0;
        if (val > 8) val = 8;
        peaks[count++] = (int)val;
        p = endptr;
    }
    while (*p && !isdigit((unsigned char)*p)) p++;
    bool extra = (*p != '\0');
    g_free(raw);

    return (count == width && !extra);
}

static void save_cached_peaks(const char *cache_file, const int *peaks, int width) {
    GString *s = g_string_new("[");
    for (int i = 0; i < width; i++) {
        if (i > 0) g_string_append(s, ", ");
        g_string_append_printf(s, "%d", peaks[i]);
    }
    g_string_append_c(s, ']');
    (void)g_file_set_contents(cache_file, s->str, (gssize)s->len, NULL);
    g_string_free(s, TRUE);
}

static bool create_cloexec_pipe(int fds[2], bool nonblock) {
    if (pipe(fds) < 0) return false;
    for (int i = 0; i < 2; i++) {
        int fl = fcntl(fds[i], F_GETFD);
        if (fl >= 0) (void)fcntl(fds[i], F_SETFD, fl | FD_CLOEXEC);
        if (nonblock) {
            int sfl = fcntl(fds[i], F_GETFL);
            if (sfl >= 0) (void)fcntl(fds[i], F_SETFL, sfl | O_NONBLOCK);
        }
    }
    return true;
}

static bool run_ffmpeg_pcm(
    const char *audio_file,
    unsigned start_ms,
    unsigned end_ms,
    GByteArray *out_pcm
) {
    int pipefd[2];
    if (!create_cloexec_pipe(pipefd, false)) return false;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }

        char ss_buf[32];
        char dur_buf[32];
        const char *argv[20];
        int argc = 0;
        argv[argc++] = "ffmpeg";
        argv[argc++] = "-v";
        argv[argc++] = "error";
        if (end_ms > start_ms) {
            double start_s = (double)start_ms / 1000.0;
            double dur_s = (double)(end_ms - start_ms) / 1000.0;
            if (dur_s < 0.1) dur_s = 0.1;
            snprintf(ss_buf, sizeof(ss_buf), "%.3f", start_s);
            snprintf(dur_buf, sizeof(dur_buf), "%.3f", dur_s);
            argv[argc++] = "-ss";
            argv[argc++] = ss_buf;
            argv[argc++] = "-t";
            argv[argc++] = dur_buf;
        }
        argv[argc++] = "-i";
        argv[argc++] = audio_file;
        argv[argc++] = "-ac";
        argv[argc++] = "1";
        argv[argc++] = "-filter:a";
        argv[argc++] = "aresample=1000";
        argv[argc++] = "-f";
        argv[argc++] = "u8";
        argv[argc++] = "-";
        argv[argc] = NULL;

        execvp("ffmpeg", (char *const *)argv);
        _exit(127);
    }

    close(pipefd[1]);
    guint8 buf[16384];
    while (g_running && out_pcm->len < MAX_PCM_BYTES) {
        ssize_t n = read(pipefd[0], buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        guint room = MAX_PCM_BYTES - out_pcm->len;
        guint take = ((guint)n < room) ? (guint)n : room;
        g_byte_array_append(out_pcm, buf, take);
    }
    close(pipefd[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 && out_pcm->len > 0;
}

static void extract_peaks(
    const char *rel_file,
    unsigned start_ms,
    unsigned end_ms,
    const char *music_dir,
    const char *cache_dir,
    int *peaks,
    int width
) {
    char *key_in = g_strdup_printf("%s_%u_%u_%d", rel_file, start_ms, end_ms, width);
    char *sha = g_compute_checksum_for_string(G_CHECKSUM_SHA256, key_in, -1);
    g_free(key_in);

    char *cache_name = g_strdup_printf("%s.json", sha);
    char *cache_file = g_build_filename(cache_dir, cache_name, NULL);
    g_free(cache_name);
    g_free(sha);

    if (load_cached_peaks(cache_file, peaks, width)) {
        g_free(cache_file);
        return;
    }

    for (int i = 0; i < width; i++) peaks[i] = 1;

    char *audio_file = resolve_audio_file(rel_file, music_dir);
    if (!audio_file) {
        g_free(cache_file);
        return;
    }

    GByteArray *pcm = g_byte_array_new();
    if (run_ffmpeg_pcm(audio_file, start_ms, end_ms, pcm) && pcm->len > 0) {
        gsize chunk_size = pcm->len / (gsize)width;
        if (chunk_size == 0) chunk_size = 1;

        int raw_peaks[MAX_WAVE_WIDTH];
        int max_val = 1;
        for (int i = 0; i < width; i++) {
            gsize off = (gsize)i * chunk_size;
            gsize end = off + chunk_size;
            if (end > pcm->len) end = pcm->len;
            int pmax = 0;
            for (gsize j = off; j < end; j++) {
                int diff = (int)pcm->data[j] - 128;
                if (diff < 0) diff = -diff;
                if (diff > pmax) pmax = diff;
            }
            raw_peaks[i] = pmax;
            if (pmax > max_val) max_val = pmax;
        }
        for (int i = 0; i < width; i++) {
            int norm = (raw_peaks[i] * 8) / max_val;
            if (norm < 1) norm = 1;
            if (norm > 8) norm = 8;
            peaks[i] = norm;
        }
        save_cached_peaks(cache_file, peaks, width);
    }

    g_byte_array_unref(pcm);
    g_free(audio_file);
    g_free(cache_file);
}

static void clear_song_waveform_stickers(struct mpd_connection *conn, const char *uri) {
    if (!conn || !uri || !*uri) return;
    if (!mpd_command_list_begin(conn, false)) {
        mpd_connection_clear_error(conn);
        return;
    }
    mpd_send_sticker_delete(conn, "song", uri, STICKER_DONE);
    mpd_send_sticker_delete(conn, "song", uri, STICKER_HEAD);
    mpd_send_sticker_delete(conn, "song", uri, STICKER_TODO);
    if (!mpd_command_list_end(conn) || !mpd_response_finish(conn)) {
        mpd_connection_clear_error(conn);
    }
}

static bool apply_waveform_stickers(
    struct mpd_connection *conn,
    const char *uri,
    const int *peaks,
    int width,
    int playhead
) {
    if (!conn || !uri || !*uri) return false;

    GString *done = g_string_new("");
    GString *head = g_string_new("");
    GString *todo = g_string_new("");

    for (int i = 0; i < width; i++) {
        int p = peaks[i];
        if (p < 0) p = 0;
        if (p > 8) p = 8;
        const char *glyph = WAVE_BARS[p];
        if (playhead < 0) {
            g_string_append(todo, glyph);
        } else if (i < playhead) {
            g_string_append(done, glyph);
        } else if (i == playhead) {
            g_string_append(head, glyph);
        } else {
            g_string_append(todo, glyph);
        }
    }

    bool ok = mpd_command_list_begin(conn, false);
    if (ok) {
        if (done->len > 0) {
            ok = mpd_send_sticker_set(conn, "song", uri, STICKER_DONE, done->str);
        } else {
            mpd_send_sticker_delete(conn, "song", uri, STICKER_DONE);
        }
    }
    if (ok) {
        if (head->len > 0) {
            ok = mpd_send_sticker_set(conn, "song", uri, STICKER_HEAD, head->str);
        } else {
            mpd_send_sticker_delete(conn, "song", uri, STICKER_HEAD);
        }
    }
    if (ok) {
        if (todo->len > 0) {
            ok = mpd_send_sticker_set(conn, "song", uri, STICKER_TODO, todo->str);
        } else {
            mpd_send_sticker_delete(conn, "song", uri, STICKER_TODO);
        }
    }
    if (ok) ok = mpd_command_list_end(conn);
    if (ok) ok = mpd_response_finish(conn);

    if (!ok) {
        /* Fallback individually so missing sticker delete doesn't fail whole batch */
        mpd_connection_clear_error(conn);
        if (done->len > 0) {
            mpd_run_sticker_set(conn, "song", uri, STICKER_DONE, done->str);
        } else {
            if (!mpd_run_sticker_delete(conn, "song", uri, STICKER_DONE)) mpd_connection_clear_error(conn);
        }
        if (head->len > 0) {
            mpd_run_sticker_set(conn, "song", uri, STICKER_HEAD, head->str);
        } else {
            if (!mpd_run_sticker_delete(conn, "song", uri, STICKER_HEAD)) mpd_connection_clear_error(conn);
        }
        if (todo->len > 0) {
            mpd_run_sticker_set(conn, "song", uri, STICKER_TODO, todo->str);
        } else {
            if (!mpd_run_sticker_delete(conn, "song", uri, STICKER_TODO)) mpd_connection_clear_error(conn);
        }
        ok = (mpd_connection_get_error(conn) == MPD_ERROR_SUCCESS);
    }

    g_string_free(done, TRUE);
    g_string_free(head, TRUE);
    g_string_free(todo, TRUE);
    return ok;
}

static bool query_state(
    struct mpd_connection *conn,
    enum mpd_state *out_state,
    unsigned *out_elapsed_ms,
    unsigned *out_duration_ms,
    char **out_uri,
    unsigned *out_start_ms,
    unsigned *out_end_ms
) {
    *out_state = MPD_STATE_STOP;
    *out_elapsed_ms = 0;
    *out_duration_ms = 0;
    *out_uri = NULL;
    *out_start_ms = 0;
    *out_end_ms = 0;

    if (!mpd_command_list_begin(conn, true) ||
        !mpd_send_status(conn) ||
        !mpd_send_current_song(conn) ||
        !mpd_command_list_end(conn)) {
        return false;
    }

    struct mpd_status *st = mpd_recv_status(conn);
    if (st) {
        *out_state = mpd_status_get_state(st);
        *out_elapsed_ms = mpd_status_get_elapsed_ms(st);
        *out_duration_ms = mpd_status_get_total_time(st) * 1000u;
        mpd_status_free(st);
    }
    if (!mpd_response_next(conn)) {
        return false;
    }

    struct mpd_song *song = mpd_recv_song(conn);
    if (song) {
        const char *uri = mpd_song_get_uri(song);
        if (uri && *uri) *out_uri = g_strdup(uri);
        unsigned dur_ms = mpd_song_get_duration_ms(song);
        if (dur_ms == 0) dur_ms = mpd_song_get_duration(song) * 1000u;
        if (*out_duration_ms == 0 && dur_ms > 0) *out_duration_ms = dur_ms;
        *out_start_ms = mpd_song_get_start_ms(song);
        *out_end_ms = mpd_song_get_end_ms(song);
        if (*out_start_ms == 0 && *out_end_ms == 0) {
            *out_start_ms = mpd_song_get_start(song) * 1000u;
            *out_end_ms = mpd_song_get_end(song) * 1000u;
        }
        mpd_song_free(song);
    }

    if (!mpd_response_finish(conn)) {
        g_free(*out_uri);
        *out_uri = NULL;
        return false;
    }
    return true;
}

int run_waveform_daemon(void) {
    if (!create_cloexec_pipe(g_wake_pipe, true)) {
        g_printerr("waveform-daemon: pipe failed\n");
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_term_signal;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int width = get_wave_width();
    char *music_dir = mpd_resolve_music_dir();
    char *cache_dir = get_cache_dir();

    struct mpd_connection *cmd_conn = NULL;
    struct mpd_connection *idle_conn = NULL;
    bool idle_active = false;

    char *last_uri = NULL;
    unsigned last_start_ms = 0;
    unsigned last_end_ms = 0;
    int last_playhead = -2;
    int peaks[MAX_WAVE_WIDTH];
    for (int i = 0; i < width; i++) peaks[i] = 1;

    bool need_sync_fav = true;

    while (g_running) {
        if (!cmd_conn) {
            cmd_conn = mpd_open_configured(NULL);
            if (cmd_conn) {
                mpd_connection_set_keepalive(cmd_conn, true);
                need_sync_fav = true;
            }
        }
        if (!idle_conn) {
            idle_conn = mpd_open_configured(NULL);
            if (idle_conn) {
                mpd_connection_set_keepalive(idle_conn, true);
                idle_active = false;
            }
        }

        if (!cmd_conn || !idle_conn) {
            if (cmd_conn) { mpd_connection_free(cmd_conn); cmd_conn = NULL; }
            if (idle_conn) { mpd_connection_free(idle_conn); idle_conn = NULL; }
            idle_active = false;
            struct pollfd pfd = { .fd = g_wake_pipe[0], .events = POLLIN };
            poll(&pfd, 1, 1500);
            continue;
        }

        if (need_sync_fav) {
            if (!mpd_sync_favourites_stickers(cmd_conn, NULL, NULL, NULL)) {
                mpd_connection_free(cmd_conn);
                cmd_conn = NULL;
                continue;
            }
            need_sync_fav = false;
        }

        if (!idle_active) {
            if (!mpd_send_idle_mask(idle_conn, (enum mpd_idle)(MPD_IDLE_PLAYER | MPD_IDLE_PLAYLIST))) {
                mpd_connection_free(idle_conn);
                idle_conn = NULL;
                continue;
            }
            idle_active = true;
        }

        enum mpd_state state = MPD_STATE_STOP;
        unsigned elapsed_ms = 0;
        unsigned duration_ms = 0;
        char *cur_uri = NULL;
        unsigned start_ms = 0;
        unsigned end_ms = 0;

        if (!query_state(cmd_conn, &state, &elapsed_ms, &duration_ms, &cur_uri, &start_ms, &end_ms)) {
            g_free(cur_uri);
            mpd_connection_free(cmd_conn);
            cmd_conn = NULL;
            continue;
        }

        if (!cur_uri || state == MPD_STATE_STOP) {
            if (last_uri) {
                clear_song_waveform_stickers(cmd_conn, last_uri);
                g_clear_pointer(&last_uri, g_free);
                last_playhead = -2;
            }
            g_free(cur_uri);
        } else {
            bool song_changed = (g_strcmp0(cur_uri, last_uri) != 0 ||
                                 start_ms != last_start_ms ||
                                 end_ms != last_end_ms);
            if (song_changed) {
                if (last_uri && g_strcmp0(cur_uri, last_uri) != 0) {
                    clear_song_waveform_stickers(cmd_conn, last_uri);
                }
                extract_peaks(cur_uri, start_ms, end_ms, music_dir, cache_dir, peaks, width);
                g_free(last_uri);
                last_uri = g_strdup(cur_uri);
                last_start_ms = start_ms;
                last_end_ms = end_ms;
                last_playhead = -2;
            }

            int playhead = -1;
            if (duration_ms > 0) {
                double ratio = (double)elapsed_ms / (double)duration_ms;
                if (ratio < 0.0) ratio = 0.0;
                if (ratio > 1.0) ratio = 1.0;
                playhead = (int)(ratio * (double)width);
                if (playhead >= width) playhead = width - 1;
            }

            if (playhead != last_playhead) {
                if (apply_waveform_stickers(cmd_conn, cur_uri, peaks, width, playhead)) {
                    last_playhead = playhead;
                } else {
                    mpd_connection_free(cmd_conn);
                    cmd_conn = NULL;
                }
            }
            g_free(cur_uri);
        }

        if (!g_running || !cmd_conn || !idle_conn) continue;

        /* Compute exact sleep until next bar transition when playing; infinite when paused/stopped */
        int timeout_ms = -1;
        if (state == MPD_STATE_PLAY && duration_ms > 0) {
            double ms_per_bar = (double)duration_ms / (double)width;
            int cur_idx = (last_playhead >= 0) ? last_playhead : 0;
            if (cur_idx < width - 1) {
                double next_ms = (double)(cur_idx + 1) * ms_per_bar;
                double delta = next_ms - (double)elapsed_ms + 20.0;
                if (delta < 60.0) delta = 60.0;
                if (delta > 5000.0) delta = 5000.0;
                timeout_ms = (int)delta;
            } else {
                timeout_ms = 2000;
            }
        }

        struct pollfd fds[2];
        fds[0].fd = g_wake_pipe[0];
        fds[0].events = POLLIN;
        fds[1].fd = mpd_connection_get_fd(idle_conn);
        fds[1].events = POLLIN;

        int pr = poll(fds, 2, timeout_ms);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (fds[0].revents & POLLIN) {
            break;
        }
        if (fds[1].revents & (POLLIN | POLLERR | POLLHUP)) {
            enum mpd_idle ev = mpd_recv_idle(idle_conn, false);
            idle_active = false;
            if (ev == 0 && mpd_connection_get_error(idle_conn) != MPD_ERROR_SUCCESS) {
                mpd_connection_free(idle_conn);
                idle_conn = NULL;
                continue;
            }
            if (ev & MPD_IDLE_PLAYLIST) {
                need_sync_fav = true;
            }
        }
    }

    if (cmd_conn && last_uri) {
        clear_song_waveform_stickers(cmd_conn, last_uri);
    }
    g_free(last_uri);
    if (idle_conn) mpd_connection_free(idle_conn);
    if (cmd_conn) mpd_connection_free(cmd_conn);
    g_free(music_dir);
    g_free(cache_dir);
    if (g_wake_pipe[0] >= 0) close(g_wake_pipe[0]);
    if (g_wake_pipe[1] >= 0) close(g_wake_pipe[1]);
    return 0;
}
