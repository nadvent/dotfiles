#include "library_browser.h"
#include "mpd_client.h"
#include "picker_ui.h"
#include "waveform_daemon.h"

#include <errno.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    GtkApplication *app;
    bool use_layer_shell;
    char *sock_path;
    int listen_fd;
    int ready_fd;
    GtkWindow *active_win;
    PickerCaches *caches;
} DaemonState;

typedef struct {
    DaemonState *daemon;
    int client_fd;
    char *pid_str;
    GPtrArray *target_songs;
} ActiveRequest;

static void active_request_free(ActiveRequest *req) {
    if (!req) return;
    if (req->client_fd >= 0) close(req->client_fd);
    g_free(req->pid_str);
    if (req->target_songs) g_ptr_array_unref(req->target_songs);
    g_free(req);
}

static char *default_socket_path(void) {
    const char *runtime = g_getenv("XDG_RUNTIME_DIR");
    if (runtime && *runtime) {
        return g_build_filename(runtime, "rmpc-picker.sock", NULL);
    }
    return g_strdup_printf("/tmp/rmpc-picker-%lu.sock", (unsigned long)getuid());
}

static char *lock_path_for_socket(const char *sock_path) {
    return g_strdup_printf("%s.lock", sock_path);
}

static int parse_env_int(const char *name, int fallback) {
    const char *raw = g_getenv(name);
    if (!raw || !*raw) return fallback;
    char *endptr = NULL;
    long v = strtol(raw, &endptr, 10);
    if (endptr && *endptr == '\0' && v >= 0 && v <= 10000) return (int)v;
    return fallback;
}

static PickerGeometry default_geometry_from_env(void) {
    PickerGeometry g = {
        .width = parse_env_int("RMPC_PICKER_WIDTH", 0),
        .height = parse_env_int("RMPC_PICKER_HEIGHT", 0),
        .left_margin = parse_env_int("RMPC_PICKER_LEFT_MARGIN", -1),
        .center_screen = (g_strcmp0(g_getenv("RMPC_PICKER_CENTER"), "1") == 0),
    };
    return g;
}

static int connect_unix_socket(const char *sock_path) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(sock_path) >= sizeof(addr.sun_path)) {
        close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(addr.sun_path, sock_path, strlen(sock_path) + 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

static int bind_unix_socket(const char *sock_path) {
    int probe = connect_unix_socket(sock_path);
    if (probe >= 0) {
        close(probe);
        errno = EADDRINUSE;
        return -1;
    }
    if (errno == ECONNREFUSED || errno == ENOENT) {
        unlink(sock_path);
    }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(sock_path) >= sizeof(addr.sun_path)) {
        close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(addr.sun_path, sock_path, strlen(sock_path) + 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    chmod(sock_path, 0600);
    if (listen(fd, 16) < 0) {
        int saved = errno;
        close(fd);
        unlink(sock_path);
        errno = saved;
        return -1;
    }
    return fd;
}

static bool write_all(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        off += (size_t)n;
    }
    return true;
}

static void reply_and_close(int *fd_ptr, int exit_code) {
    if (!fd_ptr || *fd_ptr < 0) return;
    const char *msg = (exit_code == 0) ? "0\n" : "1\n";
    (void)write_all(*fd_ptr, msg, 2);
    close(*fd_ptr);
    *fd_ptr = -1;
}

static void on_picker_complete(
    PickerAction action,
    GPtrArray *playlists,
    GPtrArray *override_target_songs,
    gpointer user_data
) {
    ActiveRequest *req = (ActiveRequest *)user_data;
    if (req->daemon) {
        req->daemon->active_win = NULL;
    }

    GPtrArray *effective_targets = override_target_songs ? override_target_songs : req->target_songs;

    if (action == PICKER_ACTION_CANCEL || !playlists || playlists->len == 0 ||
        !effective_targets || effective_targets->len == 0) {
        reply_and_close(&req->client_fd, 0);
        active_request_free(req);
        return;
    }

    GError *err = NULL;
    struct mpd_connection *conn = mpd_open_configured(&err);
    if (!conn) {
        char *msg = g_strdup_printf("Playlist picker error: %s", err ? err->message : "MPD connection failed");
        rmpc_notify(req->pid_str, "error", 3500, msg);
        g_printerr("Error: %s\n", msg);
        g_free(msg);
        g_clear_error(&err);
        reply_and_close(&req->client_fd, 1);
        active_request_free(req);
        return;
    }

    int rc = 0;
    if (action == PICKER_ACTION_REMOVE) {
        GPtrArray *res = mpd_remove_songs_from_playlists(conn, effective_targets, playlists, &err);
        if (!res) {
            char *msg = g_strdup_printf("Playlist picker error: %s", err ? err->message : "remove failed");
            rmpc_notify(req->pid_str, "error", 3500, msg);
            g_printerr("Error: %s\n", msg);
            g_free(msg);
            rc = 1;
        } else {
            char *toast = mpd_format_remove_status(res);
            rmpc_notify(req->pid_str, "info", 3500, toast);
            g_free(toast);
            g_ptr_array_unref(res);
        }
    } else {
        GPtrArray *res = mpd_add_songs_to_playlists(conn, effective_targets, playlists, &err);
        if (!res) {
            char *msg = g_strdup_printf("Playlist picker error: %s", err ? err->message : "add failed");
            rmpc_notify(req->pid_str, "error", 3500, msg);
            g_printerr("Error: %s\n", msg);
            g_free(msg);
            rc = 1;
        } else {
            char *toast = mpd_format_add_status(res);
            rmpc_notify(req->pid_str, "info", 3500, toast);
            g_free(toast);
            g_ptr_array_unref(res);
        }
    }

    g_clear_error(&err);
    mpd_connection_free(conn);
    reply_and_close(&req->client_fd, rc);
    active_request_free(req);
}

static void handle_picker_request(
    DaemonState *d,
    int client_fd,
    PickerTabMode tab_mode,
    const PickerGeometry *geom,
    const char *pid_str,
    const char *selected_raw,
    const char *current_song
) {
    if (d->active_win) {
        GtkWindow *old = d->active_win;
        d->active_win = NULL;
        gtk_window_close(old);
    }

    GError *err = NULL;
    struct mpd_connection *conn = mpd_open_configured(&err);
    if (!conn) {
        char *msg = g_strdup_printf("MPD error: %s", err ? err->message : "MPD connection failed");
        rmpc_notify(pid_str, "error", 3500, msg);
        g_printerr("Error: %s\n", msg);
        g_free(msg);
        g_clear_error(&err);
        reply_and_close(&client_fd, 1);
        return;
    }

    GPtrArray *target_songs = NULL;
    GPtrArray *overview = NULL;
    char *summary = NULL;
    char *tooltip = NULL;
    guint target_count = 0;

    if (tab_mode == PICKER_TAB_PLAYLISTS) {
        target_songs = mpd_resolve_target_songs(conn, selected_raw, current_song, &err);
        if (!target_songs) {
            char *msg = g_strdup_printf("Playlist picker error: %s", err ? err->message : "failed to resolve songs");
            rmpc_notify(pid_str, "error", 3500, msg);
            g_printerr("Error: %s\n", msg);
            g_free(msg);
            g_clear_error(&err);
            mpd_connection_free(conn);
            reply_and_close(&client_fd, 1);
            return;
        }
        if (target_songs->len == 0) {
            rmpc_notify(pid_str, "warn", 3500, "No song selected or playing");
            g_ptr_array_unref(target_songs);
            mpd_connection_free(conn);
            reply_and_close(&client_fd, 0);
            return;
        }
        overview = mpd_get_playlists_overview(conn, target_songs, &err);
        if (!overview) {
            char *msg = g_strdup_printf("Playlist picker error: %s", err ? err->message : "failed to list playlists");
            rmpc_notify(pid_str, "error", 3500, msg);
            g_printerr("Error: %s\n", msg);
            g_free(msg);
            g_clear_error(&err);
            g_ptr_array_unref(target_songs);
            mpd_connection_free(conn);
            reply_and_close(&client_fd, 1);
            return;
        }
        summary = mpd_describe_targets(conn, target_songs, &tooltip);
        target_count = target_songs->len;
    } else {
        target_songs = g_ptr_array_new_with_free_func(g_free);
    }

    mpd_connection_free(conn);

    ActiveRequest *req = g_new0(ActiveRequest, 1);
    req->daemon = d;
    req->client_fd = client_fd;
    req->pid_str = g_strdup(pid_str ? pid_str : "");
    req->target_songs = target_songs;

    d->active_win = picker_ui_show(
        d->app,
        tab_mode,
        summary,
        tooltip,
        target_count,
        overview,
        d->caches,
        req->pid_str,
        geom,
        d->use_layer_shell,
        on_picker_complete,
        req
    );

    g_free(summary);
    g_free(tooltip);
    if (overview) g_ptr_array_unref(overview);
}

static gboolean on_socket_readable(gint fd, GIOCondition cond, gpointer user_data) {
    (void)cond;
    DaemonState *d = (DaemonState *)user_data;

    int client_fd = accept(fd, NULL, NULL);
    if (client_fd < 0) {
        return G_SOURCE_CONTINUE;
    }

    struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };
    (void)setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    GString *buf = g_string_new("");
    char tmp[4096];
    bool done = false;
    while (!done && buf->len < 1024 * 1024) {
        ssize_t n = read(client_fd, tmp, sizeof(tmp));
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        g_string_append_len(buf, tmp, (gssize)n);
        if (g_str_has_suffix(buf->str, "\nEND\n") || strcmp(buf->str, "END\n") == 0 ||
            g_str_has_suffix(buf->str, "\nQUIT\n") || strcmp(buf->str, "QUIT\n") == 0) {
            done = true;
        }
    }

    if (g_strcmp0(buf->str, "QUIT\n") == 0 || g_str_has_suffix(buf->str, "\nQUIT\n")) {
        g_string_free(buf, TRUE);
        if (d->active_win) {
            GtkWindow *old = d->active_win;
            d->active_win = NULL;
            gtk_window_close(old);
        }
        reply_and_close(&client_fd, 0);
        if (d->sock_path) unlink(d->sock_path);
        g_application_quit(G_APPLICATION(d->app));
        return G_SOURCE_REMOVE;
    }

    PickerTabMode tab_mode = PICKER_TAB_PLAYLISTS;
    PickerGeometry geom = default_geometry_from_env();
    char *pid_str = NULL;
    char *cur_song = NULL;
    GString *sel = g_string_new("");

    gchar **lines = g_strsplit(buf->str, "\n", -1);
    for (int i = 0; lines && lines[i]; i++) {
        const char *line = lines[i];
        if (g_strcmp0(line, "MODE:SEARCH") == 0) {
            tab_mode = PICKER_TAB_SEARCH;
        } else if (g_strcmp0(line, "MODE:PICK") == 0) {
            tab_mode = PICKER_TAB_PLAYLISTS;
        } else if (g_str_has_prefix(line, "WIDTH:")) {
            geom.width = (int)strtol(line + 6, NULL, 10);
        } else if (g_str_has_prefix(line, "HEIGHT:")) {
            geom.height = (int)strtol(line + 7, NULL, 10);
        } else if (g_str_has_prefix(line, "LEFT:")) {
            geom.left_margin = (int)strtol(line + 5, NULL, 10);
        } else if (g_str_has_prefix(line, "CENTER:")) {
            geom.center_screen = (atoi(line + 7) != 0);
        } else if (g_str_has_prefix(line, "PID:")) {
            g_free(pid_str);
            pid_str = g_strdup(line + 4);
        } else if (g_str_has_prefix(line, "CUR:")) {
            g_free(cur_song);
            cur_song = g_strdup(line + 4);
        } else if (g_str_has_prefix(line, "SEL:")) {
            if (sel->len > 0) g_string_append_c(sel, '\n');
            g_string_append(sel, line + 4);
        }
    }
    g_strfreev(lines);
    g_string_free(buf, TRUE);

    handle_picker_request(d, client_fd, tab_mode, &geom, pid_str, sel->str, cur_song);

    g_free(pid_str);
    g_free(cur_song);
    g_string_free(sel, TRUE);
    return G_SOURCE_CONTINUE;
}

static gboolean on_unix_signal(gpointer user_data) {
    DaemonState *d = (DaemonState *)user_data;
    if (d->active_win) {
        GtkWindow *old = d->active_win;
        d->active_win = NULL;
        gtk_window_close(old);
    }
    if (d->sock_path) unlink(d->sock_path);
    g_application_quit(G_APPLICATION(d->app));
    return G_SOURCE_REMOVE;
}

static void on_daemon_activate(GtkApplication *app, gpointer user_data) {
    DaemonState *d = (DaemonState *)user_data;
    if (d->listen_fd >= 0) return;

    g_application_hold(G_APPLICATION(app));
    picker_ui_install_css(gdk_display_get_default());
    d->caches = picker_caches_new();

    d->listen_fd = bind_unix_socket(d->sock_path);
    if (d->listen_fd < 0) {
        g_printerr("Failed to bind daemon socket %s: %s\n", d->sock_path, strerror(errno));
        if (d->ready_fd >= 0) {
            (void)write_all(d->ready_fd, "0", 1);
            close(d->ready_fd);
            d->ready_fd = -1;
        }
        g_application_release(G_APPLICATION(app));
        g_application_quit(G_APPLICATION(app));
        return;
    }

    g_unix_fd_add(d->listen_fd, G_IO_IN, on_socket_readable, d);
    g_unix_signal_add(SIGINT, on_unix_signal, d);
    g_unix_signal_add(SIGTERM, on_unix_signal, d);

    if (d->ready_fd >= 0) {
        (void)write_all(d->ready_fd, "1", 1);
        close(d->ready_fd);
        d->ready_fd = -1;
    }
}

static int run_daemon(const char *sock_path, bool use_layer_shell, int ready_fd) {
    GtkApplication *app = gtk_application_new("org.rmpc.minipicker", G_APPLICATION_NON_UNIQUE);
    DaemonState d = {
        .app = app,
        .use_layer_shell = use_layer_shell,
        .sock_path = g_strdup(sock_path),
        .listen_fd = -1,
        .ready_fd = ready_fd,
        .active_win = NULL,
        .caches = NULL,
    };

    g_signal_connect(app, "activate", G_CALLBACK(on_daemon_activate), &d);
    int st = g_application_run(G_APPLICATION(app), 0, NULL);

    if (d.active_win) {
        GtkWindow *old = d.active_win;
        d.active_win = NULL;
        gtk_window_close(old);
    }
    if (d.caches) {
        picker_caches_free(d.caches);
        d.caches = NULL;
    }
    if (d.listen_fd >= 0) close(d.listen_fd);
    if (d.sock_path) {
        unlink(d.sock_path);
        g_free(d.sock_path);
    }
    g_object_unref(app);
    return st;
}

typedef struct {
    GtkApplication *app;
    PickerTabMode tab_mode;
    PickerGeometry geom;
    bool use_layer_shell;
    PickerCaches *caches;
    int exit_code;
} OneshotState;

static void on_oneshot_complete(
    PickerAction action,
    GPtrArray *playlists,
    GPtrArray *override_target_songs,
    gpointer user_data
) {
    OneshotState *st = (OneshotState *)user_data;
    const char *pid_str = g_getenv("PID");

    if (action == PICKER_ACTION_CANCEL || !playlists || playlists->len == 0) {
        st->exit_code = 0;
        g_application_quit(G_APPLICATION(st->app));
        return;
    }

    GError *err = NULL;
    struct mpd_connection *conn = mpd_open_configured(&err);
    if (!conn) {
        char *msg = g_strdup_printf("Playlist picker error: %s", err ? err->message : "MPD connection failed");
        rmpc_notify(pid_str, "error", 3500, msg);
        g_printerr("Error: %s\n", msg);
        g_free(msg);
        g_clear_error(&err);
        st->exit_code = 1;
        g_application_quit(G_APPLICATION(st->app));
        return;
    }

    GPtrArray *target_songs = override_target_songs
        ? g_ptr_array_ref(override_target_songs)
        : mpd_resolve_target_songs(conn, g_getenv("SELECTED_SONGS"), g_getenv("CURRENT_SONG"), &err);

    if (!target_songs || target_songs->len == 0) {
        if (err) {
            char *msg = g_strdup_printf("Playlist picker error: %s", err->message);
            rmpc_notify(pid_str, "error", 3500, msg);
            g_free(msg);
            st->exit_code = 1;
        }
        g_clear_error(&err);
        if (target_songs) g_ptr_array_unref(target_songs);
        mpd_connection_free(conn);
        g_application_quit(G_APPLICATION(st->app));
        return;
    }

    if (action == PICKER_ACTION_REMOVE) {
        GPtrArray *res = mpd_remove_songs_from_playlists(conn, target_songs, playlists, &err);
        if (!res) {
            char *msg = g_strdup_printf("Playlist picker error: %s", err ? err->message : "remove failed");
            rmpc_notify(pid_str, "error", 3500, msg);
            g_free(msg);
            st->exit_code = 1;
        } else {
            char *toast = mpd_format_remove_status(res);
            rmpc_notify(pid_str, "info", 3500, toast);
            g_free(toast);
            g_ptr_array_unref(res);
        }
    } else {
        GPtrArray *res = mpd_add_songs_to_playlists(conn, target_songs, playlists, &err);
        if (!res) {
            char *msg = g_strdup_printf("Playlist picker error: %s", err ? err->message : "add failed");
            rmpc_notify(pid_str, "error", 3500, msg);
            g_free(msg);
            st->exit_code = 1;
        } else {
            char *toast = mpd_format_add_status(res);
            rmpc_notify(pid_str, "info", 3500, toast);
            g_free(toast);
            g_ptr_array_unref(res);
        }
    }

    g_clear_error(&err);
    g_ptr_array_unref(target_songs);
    mpd_connection_free(conn);
    g_application_quit(G_APPLICATION(st->app));
}

static void on_oneshot_activate(GtkApplication *app, gpointer user_data) {
    OneshotState *st = (OneshotState *)user_data;
    const char *pid_str = g_getenv("PID");

    GError *err = NULL;
    struct mpd_connection *conn = mpd_open_configured(&err);
    if (!conn) {
        char *msg = g_strdup_printf("MPD error: %s", err ? err->message : "MPD connection failed");
        rmpc_notify(pid_str, "error", 3500, msg);
        g_printerr("Error: %s\n", msg);
        g_free(msg);
        g_clear_error(&err);
        st->exit_code = 1;
        g_application_quit(G_APPLICATION(app));
        return;
    }

    GPtrArray *overview = NULL;
    char *summary = NULL;
    char *tooltip = NULL;
    guint target_count = 0;

    if (st->tab_mode == PICKER_TAB_PLAYLISTS) {
        GPtrArray *target_songs = mpd_resolve_target_songs(
            conn, g_getenv("SELECTED_SONGS"), g_getenv("CURRENT_SONG"), &err
        );
        if (!target_songs) {
            char *msg = g_strdup_printf("Playlist picker error: %s", err ? err->message : "failed to resolve songs");
            rmpc_notify(pid_str, "error", 3500, msg);
            g_free(msg);
            g_clear_error(&err);
            mpd_connection_free(conn);
            st->exit_code = 1;
            g_application_quit(G_APPLICATION(app));
            return;
        }
        if (target_songs->len == 0) {
            rmpc_notify(pid_str, "warn", 3500, "No song selected or playing");
            g_ptr_array_unref(target_songs);
            mpd_connection_free(conn);
            st->exit_code = 0;
            g_application_quit(G_APPLICATION(app));
            return;
        }
        overview = mpd_get_playlists_overview(conn, target_songs, &err);
        if (!overview) {
            char *msg = g_strdup_printf("Playlist picker error: %s", err ? err->message : "failed to list playlists");
            rmpc_notify(pid_str, "error", 3500, msg);
            g_free(msg);
            g_clear_error(&err);
            g_ptr_array_unref(target_songs);
            mpd_connection_free(conn);
            st->exit_code = 1;
            g_application_quit(G_APPLICATION(app));
            return;
        }
        summary = mpd_describe_targets(conn, target_songs, &tooltip);
        target_count = target_songs->len;
        g_ptr_array_unref(target_songs);
    }

    mpd_connection_free(conn);
    st->caches = picker_caches_new();

    g_application_hold(G_APPLICATION(app));
    picker_ui_show(
        app,
        st->tab_mode,
        summary,
        tooltip,
        target_count,
        overview,
        st->caches,
        pid_str,
        &st->geom,
        st->use_layer_shell,
        on_oneshot_complete,
        st
    );
    g_free(summary);
    g_free(tooltip);
    if (overview) g_ptr_array_unref(overview);
    g_application_release(G_APPLICATION(app));
}

static int run_oneshot(PickerTabMode tab_mode, const PickerGeometry *geom, bool use_layer_shell) {
    GtkApplication *app = gtk_application_new("org.rmpc.minipicker", G_APPLICATION_NON_UNIQUE);
    OneshotState st = {
        .app = app,
        .tab_mode = tab_mode,
        .geom = *geom,
        .use_layer_shell = use_layer_shell,
        .caches = NULL,
        .exit_code = 0,
    };
    g_signal_connect(app, "activate", G_CALLBACK(on_oneshot_activate), &st);
    int run_rc = g_application_run(G_APPLICATION(app), 0, NULL);
    if (st.caches) picker_caches_free(st.caches);
    g_object_unref(app);
    return (st.exit_code != 0) ? st.exit_code : run_rc;
}

static int spawn_detached_daemon(const char *sock_path, bool use_layer_shell) {
    int ready_pipe[2];
    if (pipe(ready_pipe) < 0) return -1;

    pid_t pid1 = fork();
    if (pid1 < 0) {
        close(ready_pipe[0]);
        close(ready_pipe[1]);
        return -1;
    }

    if (pid1 == 0) {
        close(ready_pipe[0]);
        setsid();
        pid_t pid2 = fork();
        if (pid2 < 0) _exit(1);
        if (pid2 > 0) _exit(0);

        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }
        int rc = run_daemon(sock_path, use_layer_shell, ready_pipe[1]);
        _exit(rc);
    }

    close(ready_pipe[1]);
    int status = 0;
    while (waitpid(pid1, &status, 0) < 0 && errno == EINTR) {}

    char ch = 0;
    ssize_t n = 0;
    while ((n = read(ready_pipe[0], &ch, 1)) < 0 && errno == EINTR) {}
    close(ready_pipe[0]);
    return (n == 1 && ch == '1') ? 0 : -1;
}

static int trigger_daemon(
    const char *sock_path,
    PickerTabMode tab_mode,
    const PickerGeometry *geom,
    bool use_layer_shell,
    bool auto_daemon
) {
    int fd = connect_unix_socket(sock_path);
    if (fd < 0 && auto_daemon) {
        char *lock_path = lock_path_for_socket(sock_path);
        int lfd = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (lfd >= 0) {
            while (flock(lfd, LOCK_EX) < 0 && errno == EINTR) {}
        }

        fd = connect_unix_socket(sock_path);
        if (fd < 0) {
            if (spawn_detached_daemon(sock_path, use_layer_shell) == 0) {
                fd = connect_unix_socket(sock_path);
            }
        }

        if (lfd >= 0) {
            flock(lfd, LOCK_UN);
            close(lfd);
        }
        g_free(lock_path);
    }

    if (fd < 0) {
        return run_oneshot(tab_mode, geom, use_layer_shell);
    }

    const char *pid_str = g_getenv("PID");
    const char *cur_song = g_getenv("CURRENT_SONG");
    const char *sel_raw = g_getenv("SELECTED_SONGS");

    GString *payload = g_string_new("");
    g_string_append_printf(payload, "MODE:%s\n", tab_mode == PICKER_TAB_SEARCH ? "SEARCH" : "PICK");
    if (geom->width > 0) g_string_append_printf(payload, "WIDTH:%d\n", geom->width);
    if (geom->height > 0) g_string_append_printf(payload, "HEIGHT:%d\n", geom->height);
    if (geom->left_margin >= 0) g_string_append_printf(payload, "LEFT:%d\n", geom->left_margin);
    if (geom->center_screen) g_string_append(payload, "CENTER:1\n");
    if (pid_str && *pid_str) g_string_append_printf(payload, "PID:%s\n", pid_str);
    if (cur_song && *cur_song) g_string_append_printf(payload, "CUR:%s\n", cur_song);
    if (sel_raw && *sel_raw) {
        gchar **lines = g_strsplit(sel_raw, "\n", -1);
        for (int i = 0; lines && lines[i]; i++) {
            char *item = g_strstrip(lines[i]);
            if (*item) g_string_append_printf(payload, "SEL:%s\n", item);
        }
        g_strfreev(lines);
    }
    g_string_append(payload, "END\n");

    bool sent = write_all(fd, payload->str, payload->len);
    g_string_free(payload, TRUE);
    if (!sent) {
        close(fd);
        return 1;
    }

    char resp[16];
    ssize_t n = 0;
    while ((n = read(fd, resp, sizeof(resp) - 1)) < 0 && errno == EINTR) {}
    close(fd);

    if (n <= 0) return 0;
    return (resp[0] == '0') ? 0 : 1;
}

static int run_quit_daemon(const char *sock_path) {
    int fd = connect_unix_socket(sock_path);
    if (fd < 0) return 0;
    (void)write_all(fd, "QUIT\n", 5);
    char buf[16];
    (void)read(fd, buf, sizeof(buf));
    close(fd);
    return 0;
}

static int run_toggle_fav(void) {
    const char *pid_str = g_getenv("PID");
    GError *err = NULL;
    struct mpd_connection *conn = mpd_open_configured(&err);
    if (!conn) {
        char *msg = g_strdup_printf("Favourites error: %s", err ? err->message : "MPD connection failed");
        rmpc_notify(pid_str, "error", 3500, msg);
        g_printerr("Error: %s\n", msg);
        g_free(msg);
        g_clear_error(&err);
        return 1;
    }

    GPtrArray *songs = mpd_resolve_target_songs(conn, g_getenv("SELECTED_SONGS"), g_getenv("CURRENT_SONG"), &err);
    if (!songs) {
        char *msg = g_strdup_printf("Favourites error: %s", err ? err->message : "resolve failed");
        rmpc_notify(pid_str, "error", 3500, msg);
        g_printerr("Error: %s\n", msg);
        g_free(msg);
        g_clear_error(&err);
        mpd_connection_free(conn);
        return 1;
    }

    if (songs->len == 0) {
        rmpc_notify(pid_str, "warn", 3500, "No song selected or playing");
        g_ptr_array_unref(songs);
        mpd_connection_free(conn);
        return 0;
    }

    char *action = NULL;
    int count = 0;
    if (!mpd_toggle_favourites(conn, songs, &action, &count, &err)) {
        char *msg = g_strdup_printf("Favourites error: %s", err ? err->message : "toggle failed");
        rmpc_notify(pid_str, "error", 3500, msg);
        g_printerr("Error: %s\n", msg);
        g_free(msg);
        g_free(action);
        g_clear_error(&err);
        g_ptr_array_unref(songs);
        mpd_connection_free(conn);
        return 1;
    }

    if (g_strcmp0(action, "added") == 0) {
        const char *label = (count == 1) ? "song" : "songs";
        char *toast = g_strdup_printf("♥ Added %d %s to favourites", count, label);
        rmpc_notify(pid_str, "info", 3500, toast);
        g_free(toast);
    } else if (g_strcmp0(action, "removed") == 0) {
        const char *label = (songs->len == 1) ? "song" : "songs";
        char *toast = g_strdup_printf("Removed %u %s from favourites", songs->len, label);
        rmpc_notify(pid_str, "info", 3500, toast);
        g_free(toast);
    }

    g_free(action);
    g_ptr_array_unref(songs);
    mpd_connection_free(conn);
    return 0;
}

static int run_sync_fav(void) {
    GError *err = NULL;
    struct mpd_connection *conn = mpd_open_configured(&err);
    if (!conn) {
        g_printerr("Error: %s\n", err ? err->message : "MPD connection failed");
        g_clear_error(&err);
        return 1;
    }
    int added = 0;
    int removed = 0;
    if (!mpd_sync_favourites_stickers(conn, &added, &removed, &err)) {
        g_printerr("Error: %s\n", err ? err->message : "sync failed");
        g_clear_error(&err);
        mpd_connection_free(conn);
        return 1;
    }
    g_print("Synced fav stickers: +%d -%d\n", added, removed);
    mpd_connection_free(conn);
    return 0;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);

    bool mode_daemon = false;
    bool mode_oneshot = false;
    bool mode_toggle_fav = false;
    bool mode_sync_fav = false;
    bool mode_quit = false;
    bool mode_waveform = false;
    bool use_layer_shell = false;
    PickerTabMode tab_mode = PICKER_TAB_PLAYLISTS;
    PickerGeometry geom = default_geometry_from_env();
    const char *sock_override = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--daemon") == 0) {
            mode_daemon = true;
        } else if (strcmp(argv[i], "--oneshot") == 0) {
            mode_oneshot = true;
        } else if (strcmp(argv[i], "--search") == 0) {
            tab_mode = PICKER_TAB_SEARCH;
        } else if (strcmp(argv[i], "--waveform-daemon") == 0) {
            mode_waveform = true;
        } else if (strcmp(argv[i], "--layer") == 0) {
            use_layer_shell = true;
        } else if (strcmp(argv[i], "--no-layer") == 0) {
            use_layer_shell = false;
        } else if (strcmp(argv[i], "--center") == 0) {
            geom.center_screen = true;
        } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            geom.width = (int)strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            geom.height = (int)strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--left-margin") == 0 && i + 1 < argc) {
            geom.left_margin = (int)strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--toggle-fav") == 0) {
            mode_toggle_fav = true;
        } else if (strcmp(argv[i], "--sync-fav") == 0) {
            mode_sync_fav = true;
        } else if (strcmp(argv[i], "--quit-daemon") == 0) {
            mode_quit = true;
        } else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            sock_override = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            g_print(
                "Usage: rmpc-picker [OPTIONS]\n"
                "  --daemon           Run resident GTK4 socket daemon\n"
                "  --oneshot          Run single popup without daemonizing\n"
                "  --search           Open directly on Search & Visual Folder/Album tab\n"
                "  --waveform-daemon  Run event-driven MPD waveform & fav-sync daemon\n"
                "  --layer            Use Gtk4LayerShell overlay instead of regular window\n"
                "  --no-layer         Force regular xdg_toplevel window (default)\n"
                "  --center           Center window on screen instead of sidebar anchor\n"
                "  --width PX         Override window width\n"
                "  --height PX        Override window height\n"
                "  --left-margin PX   Override left anchor margin\n"
                "  --socket PATH      Override daemon Unix socket path\n"
                "  --toggle-fav       Fast-path favourites toggle (no GTK)\n"
                "  --sync-fav         Sync MPD 'fav' stickers from favourites playlist\n"
                "  --quit-daemon      Stop running rmpc-picker daemon\n"
            );
            return 0;
        } else {
            g_printerr("Unknown option: %s\n", argv[i]);
            return 2;
        }
    }

    if (mode_waveform) return run_waveform_daemon();
    if (mode_toggle_fav) return run_toggle_fav();
    if (mode_sync_fav) return run_sync_fav();

    /* Default to Cairo GSK renderer so tiny popups skip Mesa/Vulkan GPU startup */
    g_setenv("GSK_RENDERER", "cairo", FALSE);

    const char *gdk_backend = g_getenv("GDK_BACKEND");
    if (g_strcmp0(gdk_backend, "broadway") == 0) {
        use_layer_shell = false;
    }

    char *sock_path = sock_override ? g_strdup(sock_override) : default_socket_path();
    int rc = 0;

    if (mode_quit) {
        rc = run_quit_daemon(sock_path);
    } else if (mode_daemon) {
        rc = run_daemon(sock_path, use_layer_shell, -1);
    } else if (mode_oneshot) {
        rc = run_oneshot(tab_mode, &geom, use_layer_shell);
    } else {
        rc = trigger_daemon(sock_path, tab_mode, &geom, use_layer_shell, true);
    }

    g_free(sock_path);
    return rc;
}
