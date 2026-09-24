#!/usr/bin/env python3
"""Measure where the drum machine's hits fall relative to SooperLooper's
sync pulses, from an Ardour recording of the drums plus the sync-pulse log.

The pulse log gives SooperLooper's pulses in JACK frames from a session's
frame 0 (a session runs from one relocate to the next; see
sync_pulse_log_report.py).  A recording of the drums gives their hits in
recording time.  Anchor the two with T0, the recording time of the
session's frame 0, and every hit becomes a JACK frame that can be
compared with the nearest pulse.

Anchoring: when the drums start from a pedal stop/start (Hydrogen's
PLAY/STOP_TOGGLE relocates to frame 0), the session's first drum hit is
frame 0 -- use --starts to list drum starts after silence.  A session
that began in an earlier take can be anchored across takes from the
files' timestamps (mtime minus duration), then refined against a stretch
whose offset is known.

Per segment it prints the circular-mean phase of the hits relative to the
nearest pulse, in cycles and ms: 0 means the drums sit on SooperLooper's
grid; +/-0.5 cycle at 2 eighths per cycle is one eighth note off.

Needs ffmpeg and numpy.  Decodes at 8 kHz (plenty for this; 0.125 ms).

Usage:
    measure_drum_offsets.py --starts DRUM.flac [DRUM2.flac ...]
    measure_drum_offsets.py SESSION T0 SEGMENTS DRUM.flac [DRUM2.flac ...]

SEGMENTS is a comma-separated list of rolled-time windows in seconds from
the session's frame 0, e.g. "0-480,500-660".
"""
import os
import subprocess
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sync_pulse_log_report as rep  # noqa: E402

JACK_SR = 48000.0
DEC_SR = 8000.0
LOG = os.path.expanduser("~/.sooperlooper/logs/sl_sync_pulse.log")


def decode(path):
    raw = subprocess.run(
        ["ffmpeg", "-loglevel", "error", "-i", path, "-ac", "1",
         "-ar", str(int(DEC_SR)), "-f", "f32le", "-acodec", "pcm_f32le", "-"],
        check=True, stdout=subprocess.PIPE).stdout
    return np.abs(np.frombuffer(raw, dtype=np.float32))


def hits(paths, thr=0.05, refractory=0.06, merge=0.02):
    """Hit times (s) across all drum tracks, one per hit."""
    times = []
    for p in paths:
        x = decode(p)
        last = -1e9
        for i in np.nonzero(x > thr)[0]:
            if i - last > refractory * DEC_SR:
                times.append(i / DEC_SR)
            last = i
    times.sort()
    out = [times[0]]
    for t in times[1:]:
        if t - out[-1] > merge:
            out.append(t)
    return np.array(out)


def main():
    args = sys.argv[1:]
    if args and args[0] == "--starts":
        h = hits(args[1:])
        starts = [h[0]] + [b for a, b in zip(h, h[1:]) if b - a > 1.5]
        print("drum starts after >1.5 s of silence (recording seconds):")
        for s in starts:
            print("  %.4f" % s)
        return
    if len(args) < 4:
        sys.exit(__doc__)
    si, t0, segspec, paths = int(args[0]), float(args[1]), args[2], args[3:]

    sess = list(rep.sessions(rep.parse(LOG)))[si]
    pulses = np.array([p[0] for p in sess["pulses"]])
    cyc = np.array([p[1] for p in sess["pulses"]])

    frames = (hits(paths) - t0) * JACK_SR
    rows = []
    for fr in frames:
        if fr < pulses[0] - 5000 or fr > pulses[-1] + 5000:
            continue
        j = np.argmin(np.abs(pulses - fr))
        rows.append((fr / JACK_SR, rep.wrap((fr - pulses[j]) / cyc[j]), cyc[j]))
    rows = np.array(rows)

    for spec in segspec.split(","):
        a, b = map(float, spec.split("-"))
        sel = rows[(rows[:, 0] >= a) & (rows[:, 0] < b)] if len(rows) else rows
        if not len(sel):
            print("%7.1f-%7.1fs: no hits" % (a, b))
            continue
        ph = np.angle(np.mean(np.exp(2j * np.pi * sel[:, 1]))) / (2 * np.pi)
        spread = np.median(np.abs([rep.wrap(p - ph) for p in sel[:, 1]]))
        ms = ph * np.median(sel[:, 2]) / JACK_SR * 1000.0
        print("%7.1f-%7.1fs: %5d hits, drums vs pulses %+.3f cycle (%+.0f ms), "
              "spread %.3f" % (a, b, len(sel), ph, ms, spread))


if __name__ == "__main__":
    main()
