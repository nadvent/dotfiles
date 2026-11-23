#ifndef RMPC_PICKER_UI_H
#define RMPC_PICKER_UI_H

#include "library_browser.h"
#include "mpd_client.h"
#include <gtk/gtk.h>
#include <stdbool.h>

typedef enum {
    PICKER_ACTION_CANCEL = 0,
    PICKER_ACTION_ADD    = 1,
    PICKER_ACTION_REMOVE = 2,
} PickerAction;

typedef enum {
    PICKER_TAB_PLAYLISTS = 0,
    PICKER_TAB_SEARCH    = 1,
} PickerTabMode;

typedef struct {
    int width;        /* 0 = mode default */
    int height;       /* 0 = mode default */
    int left_margin;  /* -1 = centered over 600px sidebar at x=8 */
    bool center_screen;
} PickerGeometry;

typedef void (*PickerCompleteCb)(
    PickerAction action,
    GPtrArray *playlists,             /* element-type: char* */
    GPtrArray *override_target_songs, /* nullable, element-type: char* */
    gpointer user_data
);

void picker_ui_install_css(GdkDisplay *display);

GtkWindow *picker_ui_show(
    GtkApplication *app,
    PickerTabMode initial_tab,
    const char *summary,
    const char *summary_tooltip,
    guint target_count,
    GPtrArray *overview, /* element-type: PlaylistOverviewItem* (nullable in Search mode) */
    PickerCaches *caches,
    const char *pid_str,
    const PickerGeometry *geom,
    bool use_layer_shell,
    PickerCompleteCb on_complete,
    gpointer user_data
);

#endif
