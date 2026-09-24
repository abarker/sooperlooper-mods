#!/usr/bin/env python3
"""Offline report on SooperLooper's ALB_SYNC_PULSE_LOG file.

Reads ~/.sooperlooper/logs/sl_sync_pulse.log (or a given path) and, per
transport session, prints:

* how long it rolled and how many tempo changes it saw;
* pulse gaps outside a tight band around the current cycle length
  (a half-cycle slip shows up as a 0.5x or 1.5x gap);
* the predicted offset between SooperLooper's cycle grid and the
  timebase master's beat.

The offset model assumes a master that keeps its beat position continuous
through a tempo change and realigns on every relocate, which is what
Hydrogen 1.2.x does (see mod_docs/ROADMAP.rst entry 8).  SooperLooper's
grid is framepos mod cycle length, so the two drift apart only when the
tempo changes while rolling.  A session is everything between two
relocates; both grids are taken as aligned at its start.

Pulse lines ("frame=...") are the original format.  Newer builds also
write "event=relocate" and "event=tempo" lines with exact frames; when
they are present the report uses them.  Older logs fall back to splitting
sessions where the frame goes backwards and placing each tempo change
midway between the two pulses whose tempo differs (roughly +/-0.1 beat
over a long ramp).

Usage:
    sync_pulse_log_report.py [LOGFILE] [--timeline N] [--sr 48000]

--timeline N lists every change in predicted offset for session N.
"""
import argparse
import os
import sys


def parse(path):
    """Yield dicts: kind 'pulse', 'relocate' or 'tempo', plus numeric fields."""
    with open(path) as f:
        for line in f:
            fields = dict(kv.split("=", 1) for kv in line.split() if "=" in kv)
            if "event" in fields:
                kind = fields.pop("event")
            elif "frame" in fields:
                kind = "pulse"
            else:
                continue
            rec = {"kind": kind}
            for k, v in fields.items():
                try:
                    rec[k] = float(v)
                except ValueError:
                    rec[k] = v
            yield rec


def sessions(records):
    """Split into sessions at relocates (explicit, or frame going backwards)."""
    cur = {"pulses": [], "tempos": []}
    last_frame = None
    for r in records:
        if r["kind"] == "relocate":
            if cur["pulses"]:
                yield cur
            cur = {"pulses": [], "tempos": []}
            last_frame = None
            continue
        if r["kind"] == "tempo":
            cur["tempos"].append((r["frame"], r["to"]))
            continue
        pulse = r["frame"] + r["diff"]
        if last_frame is not None and r["frame"] < last_frame:
            if cur["pulses"]:
                yield cur
            cur = {"pulses": [], "tempos": []}
        cur["pulses"].append((pulse, r["tempoframes"], r["bpm"]))
        last_frame = r["frame"]
    if cur["pulses"]:
        yield cur


def wrap(x):
    """Wrap a phase difference in cycles to [-0.5, 0.5)."""
    return (x + 0.5) % 1.0 - 0.5


def offsets(sess):
    """Predicted grid offset (cycles) at each pulse after the first.

    Master phase = integral of 1/T over frames; SooperLooper's = frame/T.
    T is the cycle length in frames (tempoframes), which tracks the bpm.
    """
    pulses = sess["pulses"]
    exact = sorted(sess["tempos"])  # (frame, bpm) -- empty for old logs
    first, T0, _ = pulses[0]
    master = first / T0
    out = []
    for (pa, Ta, _), (pb, Tb, bpm_b) in zip(pulses, pulses[1:]):
        if Ta == Tb:
            master += (pb - pa) / Ta
        else:
            # Where did the tempo change?  Exact frame if logged, else midway.
            changes = [f for f, _ in exact if pa < f <= pb]
            fc = changes[-1] if changes else (pa + pb) / 2.0
            master += (fc - pa) / Ta + (pb - fc) / Tb
        out.append((pb, wrap(master - pb / Tb), bpm_b))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("logfile", nargs="?",
                    default=os.path.expanduser(
                        "~/.sooperlooper/logs/sl_sync_pulse.log"))
    ap.add_argument("--timeline", type=int, metavar="N")
    ap.add_argument("--sr", type=float, default=48000.0,
                    help="sample rate (default 48000)")
    ap.add_argument("--band", type=float, default=0.1,
                    help="gap tolerance as a fraction of a cycle (default 0.1)")
    args = ap.parse_args()

    all_sessions = list(sessions(parse(args.logfile)))
    if not all_sessions:
        sys.exit("no pulses found in %s" % args.logfile)

    print("sess  pulses  rolled   tempo-chg  odd-gaps  |offset|>0.1   final")
    for i, s in enumerate(all_sessions):
        p = s["pulses"]
        rolled = (p[-1][0] - p[0][0]) / args.sr / 60.0
        tchg = sum(1 for a, b in zip(p, p[1:]) if a[1] != b[1])
        odd = sum(1 for a, b in zip(p, p[1:])
                  if abs((b[0] - a[0]) / b[1] - 1.0) > args.band)
        offs = offsets(s)
        big = sum(1 for _, o, _ in offs if abs(o) > 0.1)
        pct = 100.0 * big / len(offs) if offs else 0.0
        final = offs[-1][1] if offs else 0.0
        print("%4d  %6d  %5.1fmin  %9d  %8d  %6d (%3.0f%%)  %+5.2f"
              % (i, len(p), rolled, tchg, odd, big, pct, final))
    print("offsets in cycles (1 cycle = 1 beat at 2 eighths per cycle)")

    if args.timeline is not None:
        s = all_sessions[args.timeline]
        print("\nsession %d: predicted offset changes" % args.timeline)
        last = None
        for frame, off, bpm in offsets(s):
            if last is None or abs(off - last[0]) > 0.03 or bpm != last[1]:
                print("  rolled %6.1fmin  bpm=%6.1f  offset=%+.2f cycle"
                      % (frame / args.sr / 60.0, bpm, off))
                last = (off, bpm)


if __name__ == "__main__":
    main()
