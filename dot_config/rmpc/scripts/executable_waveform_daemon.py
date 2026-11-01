#!/usr/bin/env python3
"""
Waveform Daemon for rmpc / MPD
Extracts audio waveform peaks via ffmpeg and dynamically updates the 'waveform'
sticker on the current song in MPD. Dimmed/stippled bars represent played audio,
a playhead marks the current position, and solid bars represent unplayed audio.
"""

import hashlib
import json
import os
import re
import signal
import socket
import subprocess
import sys
import time

WIDTH = int(os.environ.get("WAVEFORM_WIDTH", 38))
CACHE_DIR = os.environ.get("WAVEFORM_CACHE_DIR", "/tmp/rmpc_waveforms")
os.makedirs(CACHE_DIR, exist_ok=True)

# Unplayed (solid/bright block characters U+2581 to U+2588)
# Index 0 is space, 1 is lower 1/8 block, up to 8 full block
SOLID_BARS = [" ", " ", "▂", "▃", "▄", "▅", "▆", "▇", "█"]

# Played (darkened/stippled unicode characters)
DIM_BARS = [" ", "·", "·", "░", "░", "▒", "▒", "▓", "▓"]

PLAYHEAD_CHAR = os.environ.get("WAVEFORM_PLAYHEAD", "█")

running = True


def handle_signal(sig, frame):
    global running
    running = False


signal.signal(signal.SIGINT, handle_signal)
signal.signal(signal.SIGTERM, handle_signal)


class MPDClient:
    def __init__(self, host=None, port=None):
        self.host = host or os.environ.get("MPD_HOST", "127.0.0.1")
        self.port = int(port or os.environ.get("MPD_PORT", 6600))
        self.sock = None

    def connect(self):
        self.close()
        try:
            s = socket.create_connection((self.host, self.port), timeout=3.0)
            banner = s.recv(1024).decode("utf-8", errors="ignore")
            if not banner.startswith("OK MPD"):
                s.close()
                return False
            self.sock = s
            return True
        except Exception:
            self.sock = None
            return False

    def close(self):
        if self.sock:
            try:
                self.sock.close()
            except Exception:
                pass
            self.sock = None

    def _read_response(self):
        buf = []
        while True:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionResetError("MPD connection closed")
            text = chunk.decode("utf-8", errors="ignore")
            buf.append(text)
            if text.endswith("\nOK\n") or text.endswith("OK\n") or text.startswith("ACK "):
                break
        return "".join(buf)

    def get_status_and_song(self):
        """Fetches currentsong and status in a single round-trip."""
        if not self.sock and not self.connect():
            return None, None
        try:
            cmd = "command_list_ok_begin\ncurrentsong\nstatus\ncommand_list_end\n"
            self.sock.sendall(cmd.encode("utf-8"))
            raw = self._read_response()
            if raw.startswith("ACK"):
                return None, None

            song = {}
            status = {}
            target = song
            for line in raw.splitlines():
                if line == "list_OK":
                    target = status
                    continue
                if line == "OK":
                    break
                if ": " in line:
                    k, v = line.split(": ", 1)
                    target[k.lower()] = v

            return song, status
        except Exception:
            self.close()
            return None, None

    def set_sticker(self, file_path, name, value):
        """Sets a song sticker in MPD."""
        if not self.sock and not self.connect():
            return False
        try:
            # Escape backslashes and double quotes in sticker value
            escaped_val = value.replace("\\", "\\\\").replace('"', '\\"')
            escaped_path = file_path.replace("\\", "\\\\").replace('"', '\\"')
            cmd = f'sticker set song "{escaped_path}" "{name}" "{escaped_val}"\n'
            self.sock.sendall(cmd.encode("utf-8"))
            res = self._read_response()
            return "OK" in res
        except Exception:
            self.close()
            return False


def resolve_audio_file(rel_file, music_dir):
    """
    Resolves virtual tracks (e.g. from CUE sheets) or standard audio files
    to an existing absolute audio file on disk.
    """
    # 1. Check if rel_file is a track inside a .cue sheet
    if ".cue/" in rel_file:
        cue_rel, track_part = rel_file.split(".cue/", 1)
        cue_path = os.path.join(music_dir, cue_rel + ".cue")
        if os.path.exists(cue_path):
            cue_dir = os.path.dirname(cue_path)
            try:
                with open(cue_path, "r", errors="ignore") as f:
                    for line in f:
                        if line.strip().upper().startswith("FILE"):
                            m = re.search(r'FILE\s+"([^"]+)"', line, re.IGNORECASE)
                            if m:
                                audio_candidate = os.path.join(cue_dir, m.group(1))
                                if os.path.exists(audio_candidate):
                                    return audio_candidate
            except Exception:
                pass
            # Fallback: search directory for matching flac/wav/mp3/ape/wv
            base = os.path.splitext(cue_path)[0]
            for ext in [".flac", ".wav", ".ape", ".wv", ".mp3", ".m4a", ".ogg"]:
                candidate = base + ext
                if os.path.exists(candidate):
                    return candidate

    # 2. Regular file directly under music_dir
    full_path = os.path.join(music_dir, rel_file)
    if os.path.exists(full_path):
        return full_path

    # 3. Handle possible full absolute path
    if os.path.exists(rel_file):
        return rel_file

    return None


def extract_peaks(rel_file, range_val=None):
    """
    Decodes audio via ffmpeg into mono 1kHz unsigned 8-bit PCM,
    calculates peak loudness deviations from midpoint (128),
    and normalizes them into 0-8 bar levels across WIDTH columns.
    Results are cached to disk using sha256.
    """
    music_dir = os.environ.get("XDG_MUSIC_DIR", os.path.expanduser("~/Music"))
    cache_key = hashlib.sha256(f"{rel_file}_{range_val}_{WIDTH}".encode("utf-8")).hexdigest()
    cache_file = os.path.join(CACHE_DIR, f"{cache_key}.json")

    if os.path.exists(cache_file):
        try:
            with open(cache_file, "r") as f:
                return json.load(f)
        except Exception:
            pass

    audio_file = resolve_audio_file(rel_file, music_dir)
    if not audio_file:
        return [1] * WIDTH

    cmd = ["ffmpeg", "-v", "error"]

    # If range is specified (e.g. from MPD CUE track: "start-end" or "start-end.end")
    if range_val and "-" in range_val:
        try:
            start_str, end_str = range_val.split("-")
            start_sec = float(start_str)
            end_sec = float(end_str)
            dur_sec = max(0.1, end_sec - start_sec)
            cmd += ["-ss", str(start_sec), "-t", str(dur_sec)]
        except Exception:
            pass

    cmd += [
        "-i",
        audio_file,
        "-ac",
        "1",
        "-filter:a",
        "aresample=1000",
        "-f",
        "u8",
        "-",
    ]

    try:
        proc = subprocess.run(cmd, capture_output=True, timeout=15)
        raw = proc.stdout
        if not raw:
            return [1] * WIDTH

        chunk_size = max(1, len(raw) // WIDTH)
        peaks = []
        for i in range(WIDTH):
            chunk = raw[i * chunk_size : (i + 1) * chunk_size]
            if chunk:
                val = max((abs(b - 128) for b in chunk), default=0)
                peaks.append(val)
            else:
                peaks.append(0)

        max_val = max(peaks) or 1
        normalized = [min(8, int((p / max_val) * 8)) for p in peaks]

        with open(cache_file, "w") as f:
            json.dump(normalized, f)

        return normalized
    except Exception:
        return [1] * WIDTH


def render_waveform(peaks, elapsed, duration):
    if duration <= 0 or not peaks:
        return "".join(SOLID_BARS[p] for p in peaks) if peaks else ""

    progress_ratio = max(0.0, min(1.0, elapsed / float(duration)))
    played_bars_count = int(progress_ratio * WIDTH)

    rendered = []
    for i, p in enumerate(peaks):
        if i < played_bars_count:
            rendered.append(DIM_BARS[p])  # Darkened / played
        elif i == played_bars_count:
            rendered.append(PLAYHEAD_CHAR)  # Playhead marker
        else:
            rendered.append(SOLID_BARS[p])  # Bright / unplayed

    return "".join(rendered)


def main():
    client = MPDClient()
    last_file = ""
    last_rendered = ""
    peaks = []

    while running:
        song, status = client.get_status_and_song()
        if not song or not status:
            time.sleep(1.0)
            continue

        state = status.get("state", "stop")
        song_file = song.get("file", "")

        if not song_file or state == "stop":
            last_file = ""
            last_rendered = ""
            time.sleep(1.0)
            continue

        # Song changed: extract or fetch cached peaks
        if song_file != last_file:
            range_val = song.get("range")
            peaks = extract_peaks(song_file, range_val)
            last_file = song_file
            last_rendered = ""

        # Calculate timing
        try:
            elapsed = float(status.get("elapsed", 0.0))
        except ValueError:
            elapsed = 0.0

        try:
            duration = float(status.get("duration", song.get("duration", 0.0)))
        except ValueError:
            duration = 0.0

        waveform_str = render_waveform(peaks, elapsed, duration)

        # Only update sticker when the rendered visual changes
        if waveform_str and waveform_str != last_rendered:
            if client.set_sticker(song_file, "waveform", waveform_str):
                last_rendered = waveform_str

        # When paused, don't tick fast; when playing, tick every 0.8s
        if state == "pause":
            time.sleep(1.0)
        else:
            time.sleep(0.8)

    client.close()


if __name__ == "__main__":
    main()
