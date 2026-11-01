#!/usr/bin/env sh
# Saves the next queued song's "Artist - Title" into the current song's "next_song" sticker.
# Requires: mpc (or jq with `rmpc queue`) and sticker_file enabled in mpd.conf.

[ -z "$FILE" ] && exit 0

NEXT_TRACK="$(mpc --format '%artist% - %title%' queued 2>/dev/null)"

if [ -z "$NEXT_TRACK" ] || [ "$NEXT_TRACK" = " - " ]; then
    NEXT_TRACK="End of queue"
fi

rmpc sticker set "$FILE" "next_song" "$NEXT_TRACK" >/dev/null 2>&1
