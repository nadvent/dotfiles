#!/bin/bash
SHM_FILE="/dev/shm/looking-glass"

while true; do
  # Check if the shared memory file exists and is populated
  if [ -e "$SHM_FILE" ]; then
    # Launch Looking Glass client if it's not already running
    if ! pgrep -x "looking-glass-clie" >/dev/null; then
      looking-glass-client -F
    fi
  fi
  sleep 2
done
