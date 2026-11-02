#!/usr/bin/env python3
"""Does Hydrogen keep its tempo offset when it misses audio cycles?

With ALB_JACK_BEAT_TRACKING the engine counts beats through tempo changes
made while rolling, the way Hydrogen 1.2.x keeps its own beat continuous.
That only works while Hydrogen keeps that offset.  Hydrogen 1.2.4-1.2.7
drop it whenever their frame and the JACK frame disagree at the start of a
cycle (JackAudioDriver::updateTransportPosition -> locateToFrame ->
resetOffsets), and a cycle Hydrogen misses causes exactly that: a failed
engine lock ("missed buffer") returns before the transport position is
advanced.  Hydrogen then plays on the plain tempo grid while the engine
keeps the offset, and the loops are off the drums until the next relocate.

The test runs the real programs on a private dummy JACK server (no
hardware, no effect on a running rig):

- Hydrogen as timebase master, own HOME, OSC on a private port, no window,
  playing a looped one-kick-per-beat song;
- the engine (own HOME, so the rig's pulse log is untouched) syncing to
  JACK with one beat per cycle;
- a probe client that records Hydrogen's audio, stamped with the JACK frame.

It starts at 115 BPM, steps the tempo up 3 BPM while rolling, freezes
Hydrogen (SIGSTOP) long enough to miss several cycles, then relocates to
frame 0.  The freeze stands in for a failed engine lock: from Hydrogen's
side both leave its frame behind JACK's, and both go through the same
reset.  Kick onsets in the audio are compared with the engine's pulses;
Hydrogen's published BBT is no use for this, since its timebase callback
computes it from the JACK frame, ignoring the offset.

Windows reported, as kick-minus-pulse in beats (output latency subtracted):
  A0  before the tempo change            expected 0
  A   after it, before the freeze        expected 0 (beat tracking works)
  B   after the freeze                   0 if Hydrogen kept its offset;
                                         about -0.4 beat if it reset
  C   after the relocate to frame 0      expected 0

--expect reset (default; Hydrogen 1.2.4-1.2.7): pass if A0, A and C are on
the beat and B is off by more than 0.1 beat, i.e. the drift is reproduced.
--expect kept: pass if all four are on the beat -- for a Hydrogen fixed to
keep its offset across missed cycles.  Exit status 0 on pass.

Requires: jackd, oscsend (liblo), python3 with the `jack` module and
numpy, an engine built with ALB_SYNC_PULSE_LOG defined, and Hydrogen:
either an AppImage (--appimage; extracted into --h2-root on first use)
or a Hydrogen tree with usr/bin and
usr/share/hydrogen (--h2-root, e.g. a self-built install prefix's parent).
The AppImage bundles only Qt's X11 platform plugin; the system's
libqoffscreen.so (Qt 5) is copied in so Hydrogen can run without a window.

Usage:
    hydrogen_missed_cycle_test.py [--engine PATH] [--appimage FILE]
        [--h2-root DIR] [--freeze-ms N] [--expect reset|kept]

Set SL_BT_KEEP=1 to keep the run's files for inspection.  About 45 s.
"""
import argparse
import bisect
import glob
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

import numpy as np

SR = 48000
PERIOD = 256
# Fixed: JACK2 has a few server slots and reuses a dead server's only for a
# server of the same name, so a new name per run ran out of slots ("Too many
# servers already active") after about seven runs.
SERVER = "sl_h2_test"
SL_OSC_PORT = 19000 + os.getpid() % 900
H2_OSC_PORT = SL_OSC_PORT + 1000
ON_BEAT = 0.02                 # beats; one 256-frame cycle is 0.01 beat
OFF_BEAT = 0.1


def find_h2_root(args):
    """Return the directory holding usr/bin and usr/share/hydrogen."""
    root = args.h2_root
    if os.path.isdir(os.path.join(root, "usr", "share", "hydrogen")):
        return root
    if not args.appimage:
        sys.exit("no Hydrogen at %s; pass --appimage to extract one" % root)
    print("extracting %s into %s (a few minutes)..." % (args.appimage, root))
    parent = os.path.dirname(os.path.abspath(root))
    os.makedirs(parent, exist_ok=True)
    subprocess.run([os.path.abspath(args.appimage), "--appimage-extract"],
                   cwd=parent, check=True, stdout=subprocess.DEVNULL)
    extracted = os.path.join(parent, "squashfs-root")
    if os.path.abspath(extracted) != os.path.abspath(root):
        os.rename(extracted, root)
    return root


def offscreen_plugin(root):
    """Let an extracted AppImage run with QT_QPA_PLATFORM=offscreen."""
    plat = os.path.join(root, "usr", "plugins", "platforms")
    if not os.path.isdir(plat) or os.path.exists(os.path.join(plat, "libqoffscreen.so")):
        return
    found = glob.glob("/usr/lib/*/qt5/plugins/platforms/libqoffscreen.so")
    if not found:
        sys.exit("no system libqoffscreen.so to run Hydrogen without a window")
    shutil.copy(found[0], plat)


def write_h2_config(root, home):
    """Hydrogen's default config, set up for this test."""
    src = os.path.join(root, "usr", "share", "hydrogen", "data", "hydrogen.default.conf")
    s = open(src).read()

    def put(tag, val):
        nonlocal s
        s, n = re.subn(r"<%s>[^<]*</%s>" % (tag, tag), "<%s>%s</%s>" % (tag, val, tag), s)
        if n == 0:
            sys.exit("%s has no <%s>" % (src, tag))
    put("audio_driver", "JACK")
    put("jack_transport_mode", "USE_JACK_TRANSPORT")
    put("jack_timebase_enabled", "true")
    put("jack_transport_mode_master", "USE_JACK_TIME_MASTER")
    put("jack_connect_defaults", "false")
    put("oscEnabled", "true")
    put("oscServerPort", str(H2_OSC_PORT))
    os.makedirs(os.path.join(home, ".hydrogen"))
    with open(os.path.join(home, ".hydrogen", "hydrogen.conf"), "w") as f:
        f.write(s)


def write_song(root, path, mode="song"):
    """A demo song cut down to one looped pattern with a kick on every beat,
    in song or pattern mode."""
    src = os.path.join(root, "usr", "share", "hydrogen", "data", "demo_songs",
                       "GM_kit_demo1.h2song")
    s = open(src).read()
    notes = "".join(
        "    <note>\n     <position>%d</position>\n     <leadlag>0</leadlag>\n"
        "     <velocity>1</velocity>\n     <pan>0</pan>\n     <pitch>0</pitch>\n"
        "     <probability>1</probability>\n     <key>C0</key>\n     <length>-1</length>\n"
        "     <instrument>0</instrument>\n     <note_off>false</note_off>\n    </note>\n" % p
        for p in (0, 48, 96, 144))
    pat = (" <patternList>\n  <pattern>\n   <name>beat</name>\n   <category>test</category>\n"
           "   <size>192</size>\n   <denominator>4</denominator>\n   <info></info>\n"
           "   <noteList>\n" + notes + "   </noteList>\n  </pattern>\n </patternList>")
    s = re.sub(r" <patternList>.*?</patternList>", pat, s, flags=re.S)
    s = re.sub(r"<patternSequence>.*?</patternSequence>",
               "<patternSequence>\n  <group>\n   <patternID>beat</patternID>\n"
               "  </group>\n </patternSequence>", s, flags=re.S)
    s = re.sub(r"<loopEnabled>[^<]*</loopEnabled>", "<loopEnabled>true</loopEnabled>", s)
    s = re.sub(r"<mode>[^<]*</mode>", "<mode>%s</mode>" % mode, s)
    for tag in ("humanize_time", "humanize_velocity", "swing_factor"):
        s = re.sub(r"<%s>[^<]*</%s>" % (tag, tag), "<%s>0</%s>" % (tag, tag), s)
    with open(path, "w") as f:
        f.write(s)


def osc(port, *msg):
    subprocess.run(["oscsend", "localhost", str(port)] + list(msg), check=True)


def wrap(x):
    return (x + 0.5) % 1.0 - 0.5


def kick_onsets(blocks):
    """Kick onsets as JACK frames, one list per transport run (split at
    the relocate to 0)."""
    runs, cur, last = [], [], None
    for f, rolling, buf in blocks:
        if not rolling:
            continue
        if last is not None and f < last:
            runs.append(cur)
            cur = []
        cur.append((f, buf))
        last = f
    runs.append(cur)
    out = []
    for run in runs:
        # Onset: first sample over the threshold at least 0.3 s (well over
        # half a beat at these tempos) after the previous onset.
        ons = []
        for f, buf in run:
            for i in np.nonzero(np.abs(buf) > 0.05)[0]:
                if not ons or f + int(i) - ons[-1] > 0.3 * SR:
                    ons.append(f + int(i))
        out.append(ons)
    return out


def kick_vs_pulse(onsets, pulses):
    """[(onset frame, kick minus nearest pulse in beats)]."""
    pf = [p for p, _ in pulses]
    out = []
    for o in onsets:
        i = bisect.bisect_left(pf, o)
        cands = [k for k in (i - 1, i) if 0 <= k < len(pf)]
        if cands:
            k = min(cands, key=lambda k: abs(pf[k] - o))
            out.append((o, wrap((o - pf[k]) / pulses[k][1])))
    return out


def run(args, tmp, h2_root):
    env = dict(os.environ, JACK_NO_START_SERVER="1", JACK_DEFAULT_SERVER=SERVER)
    procs = []

    def spawn(cmd, penv, log):
        p = subprocess.Popen(cmd, env=penv, stdout=open(os.path.join(tmp, log), "w"),
                             stderr=subprocess.STDOUT, start_new_session=True)
        procs.append(p)
        return p

    sl_home = os.path.join(tmp, "slhome")
    h2_home = os.path.join(tmp, "h2home")
    os.makedirs(sl_home)
    write_h2_config(h2_root, h2_home)
    song = os.path.join(tmp, "test.h2song")
    write_song(h2_root, song, "pattern" if args.pattern_mode else "song")
    apprun = os.path.join(h2_root, "AppRun")
    h2_cmd = [apprun] if os.path.exists(apprun) else [os.path.join(h2_root, "usr", "bin", "hydrogen")]

    try:
        spawn(["jackd", "-n", SERVER, "-r", "-d", "dummy", "-r", str(SR), "-p", str(PERIOD)],
              env, "jackd.out")
        time.sleep(2)

        import jack
        probe = jack.Client("h2probe", servername=SERVER, no_start_server=True)
        inp = probe.inports.register("in")
        blocks = []                   # (transport frame, rolling, samples)

        @probe.set_process_callback
        def process(nframes):
            st, pos = probe.transport_query()
            blocks.append((pos["frame"], st == jack.ROLLING, inp.get_array().copy()))

        probe.activate()

        def start_engine(log):
            return spawn([args.engine, "-S", SERVER, "-p", str(SL_OSC_PORT), "-l", "1", "-c", "2",
                          "-j", "sl_h2"], dict(env, HOME=sl_home), log)

        def set_engine():
            for msg in (["/set", "sf", "sync_source", "-1"],
                        ["/set", "sf", "eighth_per_cycle", "2"],
                        ["/sl/0/set", "sf", "quantize", "1"]):
                osc(SL_OSC_PORT, *msg)

        engine = start_engine("engine.out")
        h2 = spawn(h2_cmd + ["--child", "-n", "-s", song, "-O", str(H2_OSC_PORT),
                             "-L", os.path.join(tmp, "h2.log"), "-T", "-V", "Warning"],
                   dict(env, HOME=h2_home, QT_QPA_PLATFORM="offscreen"), "h2.out")
        time.sleep(8)
        set_engine()
        outs = [p.name for p in probe.get_ports("Hydrogen:", is_audio=True, is_output=True)]
        main_l = [n for n in outs if n.endswith("out_L")] or outs[:1]
        if not main_l:
            print("FAIL: Hydrogen did not join the test JACK server; see", tmp)
            return None
        probe.connect(main_l[0], "h2probe:in")

        probe.transport_locate(0)
        osc(H2_OSC_PORT, "/Hydrogen/BPM", "f", "115")
        time.sleep(1)
        blocks.clear()
        probe.transport_start()
        t0 = time.monotonic()

        marks = {}
        plan = [(8.0, "incr"), (8.1, "incr"), (8.2, "incr"), (15.0, "freeze")]
        if args.restart_engine:
            plan += [(19.0, "restart"), (30.0, "relocate"), (36.0, "end")]
        else:
            plan += [(23.0, "relocate"), (29.0, "end")]
        for at, what in plan:
            while time.monotonic() - t0 < at:
                time.sleep(0.002)
            marks.setdefault(what, probe.transport_query()[1]["frame"])
            if what == "incr":
                osc(H2_OSC_PORT, "/Hydrogen/BPM_INCR", "f", "1")
            elif what == "freeze":
                os.kill(h2.pid, signal.SIGSTOP)
                time.sleep(args.freeze_ms / 1000.0)
                os.kill(h2.pid, signal.SIGCONT)
            elif what == "restart":
                # The engine restarted while rolling, as after a crash on the
                # rig: it counts from frame x tempo again unless it follows
                # the master's position.
                os.killpg(engine.pid, signal.SIGTERM)
                engine.wait(timeout=5)
                engine = start_engine("engine2.out")
                time.sleep(3)
                set_engine()
            elif what == "relocate":
                probe.transport_locate(0)
        probe.transport_stop()
        time.sleep(0.5)
        probe.deactivate()
        return blocks, marks
    finally:
        for p in reversed(procs):
            try:
                os.killpg(p.pid, signal.SIGCONT)
                os.killpg(p.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        time.sleep(1.5)
        for p in reversed(procs):
            try:
                os.killpg(p.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--engine", default=os.path.join(here, "..", "sooperlooper"))
    ap.add_argument("--appimage", help="Hydrogen AppImage, extracted into --h2-root if needed")
    ap.add_argument("--h2-root", default=os.path.join(
        os.environ.get("XDG_CACHE_HOME", os.path.expanduser("~/.cache")),
        "sl_hydrogen_test", "squashfs-root"))
    ap.add_argument("--freeze-ms", type=float, default=30.0)
    ap.add_argument("--expect", choices=["reset", "kept"], default="reset")
    ap.add_argument("--pattern-mode", action="store_true",
                    help="Hydrogen in pattern mode (as on the rig) instead of song mode")
    ap.add_argument("--restart-engine", action="store_true",
                    help="also restart the engine while rolling, after the freeze; "
                         "window D after it (on the beat with --expect kept)")
    args = ap.parse_args()

    h2_root = find_h2_root(args)
    offscreen_plugin(h2_root)
    tmp = tempfile.mkdtemp(prefix="sl_h2_test_")
    res = run(args, tmp, h2_root)
    if res is None:
        return 1
    blocks, marks = res

    log = os.path.join(tmp, "slhome", ".sooperlooper", "logs", "sl_sync_pulse.log")
    if not os.path.exists(log):
        print("FAIL: no pulse log -- was the engine built with ALB_SYNC_PULSE_LOG?")
        return 1
    pulses = []
    for line in open(log):
        d = dict(kv.split("=", 1) for kv in line.split() if "=" in kv)
        if line.startswith("frame="):
            pulses.append((int(d["frame"]) + int(d["diff"]), float(d["tempoframes"])))
        elif line.startswith("event="):
            print("  engine log:", line.strip())
    onset_runs = kick_onsets(blocks)
    cut = next((k for k in range(1, len(pulses)) if pulses[k][0] < pulses[k - 1][0]),
               len(pulses))
    res = [kick_vs_pulse(o, p) for o, p in zip(onset_runs, [pulses[:cut], pulses[cut:]])]
    print("freeze %.0f ms; kick onsets per run %s; engine pulses %d"
          % (args.freeze_ms, [len(o) for o in onset_runs], len(pulses)))

    first = [d for o, d in res[0] if o < marks["incr"]]
    if not first:
        print("FAIL: no kicks before the tempo change; see", tmp)
        return 1
    base = float(np.median(first))
    print("kick minus pulse before any tempo change: %+.3f beat "
          "(output latency; subtracted below)" % base)

    def window(r, lo, hi, label):
        sel = sorted(wrap(d - base) for o, d in r if lo <= o < hi)
        if not sel:
            print("  %-36s no kicks" % label)
            return None
        med = sel[len(sel) // 2]
        print("  %-36s n=%3d  median %+.3f beat (range %+.3f .. %+.3f)"
              % (label, len(sel), med, sel[0], sel[-1]))
        return med

    fz = marks["freeze"]
    a0 = window(res[0], 2 * SR, marks["incr"], "A0 before the tempo change")
    a = window(res[0], marks["incr"] + 2 * SR, fz, "A  after +3 BPM, before freeze")
    b_end = marks.get("restart", marks["relocate"])
    b = window(res[0], fz + SR, b_end, "B  after Hydrogen missed cycles")
    d = None
    if "restart" in marks:
        d = window(res[0], marks["restart"] + 5 * SR, marks["relocate"],
                   "D  after the engine restarted")
    c = window(res[1], SR, 10 ** 12, "C  after relocate to frame 0") if len(res) > 1 else None

    if os.environ.get("SL_BT_KEEP"):
        print("kept", tmp)
    else:
        shutil.rmtree(tmp, ignore_errors=True)

    if None in (a0, a, b, c) or (args.restart_engine and d is None):
        print("FAIL: a window had no kicks")
        return 1
    on = [abs(x) <= ON_BEAT for x in (a0, a, c)]
    if args.expect == "reset":
        # D is not judged: after a stock Hydrogen's reset and an engine
        # restart both are on the plain grid, by coincidence.
        ok = all(on) and abs(b) > OFF_BEAT
    else:
        ok = all(on) and abs(b) <= ON_BEAT and (d is None or abs(d) <= ON_BEAT)
    print("PASS" if ok else "FAIL", "(expect %s)" % args.expect)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
