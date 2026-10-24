#!/usr/bin/env bash

# Define your two audio sink names (or wpctl IDs)
# Recommended: Use persistent node names from `pactl list short sinks`
DEV1="alsa_output.pci-0000_04_00.6.HiFi__Speaker__sink"
DEV2="alsa_output.usb-BEHRINGER_UMC204HD_192k-00.pro-output-0"

# Get current default sink
CURRENT=$(pactl get-default-sink)

if [ "$CURRENT" = "$DEV1" ]; then
    pactl set-default-sink "$DEV2"
    notify-send "Audio Output" "Switched to Audio Interface" -i audio-card
else
    pactl set-default-sink "$DEV1"
    notify-send "Audio Output" "Switched to Speakers" -i audio-speakers
fi
