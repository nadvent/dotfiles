#!/usr/bin/env bash

# Toggle the "Quick Search" scratchpad in Umbriel.
# Umbriel's built-in scratchpad manager will automatically spawn `qutebrowser-scratchpad`
# (defined in ~/.config/umbriel/config.toml under [[scratchpad]] spawn_when_empty)
# if no instance is running, or toggle its visibility if already active.

if command -v umbriel >/dev/null 2>&1; then
    umbriel msg "scratchpad-toggle:Quick Search"
else
    exec qutebrowser-scratchpad "$@"
fi
