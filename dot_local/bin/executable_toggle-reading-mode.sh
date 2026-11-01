#!/usr/bin/env bash
# Toggle reading mode screen shader in Umbriel

if umbriel effects | grep -q 'screen[[:space:]]\+reading_mode'; then
    umbriel msg effect-screen-reset
    command -v notify-send >/dev/null && notify-send "Reading Mode" "Disabled" -i display -t 2000 2>/dev/null || true
else
    umbriel msg effect-screen-set:reading_mode
    command -v notify-send >/dev/null && notify-send "Reading Mode" "Enabled (e-ink paper shader)" -i display -t 2000 2>/dev/null || true
fi
