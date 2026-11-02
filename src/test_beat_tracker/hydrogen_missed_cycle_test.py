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


def kick_onsets_wall(blocks):
    """Kick onsets in wall-clock JACK frames (frame_time), from blocks of
    (transport frame, rolling, samples, frame_time); a relocation does not
    split or reorder them."""
    ons = []
    for f, rolling, buf, w in blocks:
        if not rolling:
            continue
        for i in np.nonzero(np.abs(buf) > 0.05)[0]:
            if not ons or w + int(i) - ons[-1] > 0.3 * SR:
                ons.append(w + int(i))
    return ons


def pulses_to_wall(pulses, blocks):
    """Map the engine's pulses (transport frames, in time order) to wall
    frames, walking the probe's blocks in order.  pulses may also hold
    ("relocate", frame) items, the engine's relocate events in log order:
    the walk then skips to the block where the transport is at that frame,
    so transport frames that repeat after a relocation backwards map to the
    pass after it, not the one before."""
    out, k = [], 0
    for p, tf in pulses:
        if p == "relocate":
            j = k
            while j < len(blocks) and not (blocks[j][1] and blocks[j][0] == tf):
                j += 1
            if j < len(blocks):
                k = j
            continue
        while k < len(blocks):
            f, rolling, buf, w = blocks[k]
            if rolling and f <= p < f + len(buf):
                out.append((w + (p - f), tf))
                break
            k += 1
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
            blocks.append((pos["frame"], st == jack.ROLLING, inp.get_array().copy(),
                           probe.last_frame_time))

        if args.slow_sync:
            # A slow-sync client: not ready for args.slow_sync cycles each
            # time the transport is starting, so JACK holds it that much
            # longer at every start and every relocation while rolling.
            held = {"n": 0}

            @probe.set_sync_callback
            def sync(state, pos):
                if state == jack.STARTING and held["n"] < args.slow_sync:
                    held["n"] += 1
                    return False
                held["n"] = 0
                return True

        probe.activate()

        def start_engine(log):
            return spawn([args.engine, "-S", SERVER, "-p", str(SL_OSC_PORT), "-l", "1", "-c", "2",
                          "-j", "sl_h2"], dict(env, HOME=sl_home), log)

        def set_engine():
            for msg in (["/set", "sf", "sync_source", "-1"],
                        ["/set", "sf", "eighth_per_cycle", "2"],
                        ["/sl/0/set", "sf", "quantize", "1"]):
                osc(SL_OSC_PORT, *msg)
            if args.auto_realign:
                osc(SL_OSC_PORT, "/set", "sf", "auto_realign", "1")

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

        # Marks are wall-clock JACK frames (frame_time): a relocation moves
        # the transport frame, not these.
        marks = {"start": probe.frame_time}
        plan = [(8.0, "incr"), (8.1, "incr"), (8.2, "incr"), (15.0, "freeze")]
        if args.restart_engine:
            plan += [(19.0, "restart")]
        if args.realign:
            plan += [(28.0 if args.restart_engine else 19.0, "realign")]
        last = max(at for at, _ in plan)
        plan += [(last + 7.0 + (4.0 if args.restart_engine and not args.realign else 0.0), "relocate")]
        plan += [(plan[-1][0] + 6.0, "end")]
        for at, what in plan:
            while time.monotonic() - t0 < at:
                time.sleep(0.002)
            marks.setdefault(what, probe.frame_time)
            if what == "realign":
                # The engine's realign: the transport moves so that the
                # drums land on the engine's beat count.
                osc(SL_OSC_PORT, "/set", "sf", "realign_transport", "1")
            elif what == "incr":
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
                if args.toggle:
                    # Hydrogen's Play/Stop toggle, twice: locate to the
                    # start and stop, then play.
                    osc(H2_OSC_PORT, "/Hydrogen/PLAY_STOP_TOGGLE", "f", "1")
                    time.sleep(1.0)
                    osc(H2_OSC_PORT, "/Hydrogen/PLAY_STOP_TOGGLE", "f", "1")
                    marks["relocate"] = probe.frame_time
                else:
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
    ap.add_argument("--realign", action="store_true",
                    help="send the engine's realign (/set realign_transport) after the "
                         "freeze (and after the restart, with --restart-engine); window E "
                         "after it must be on the beat")
    ap.add_argument("--auto-realign", action="store_true",
                    help="turn on the engine's auto_realign: the tempo change while rolling "
                         "is followed by a realign, so the freeze has no offset to drop and "
                         "B must be on the beat whatever --expect says")
    ap.add_argument("--slow-sync", type=int, default=0, metavar="N",
                    help="the probe is a slow-sync client, not ready for N cycles whenever "
                         "the transport is starting; with --auto-realign the engine must "
                         "then stop auto-realigning and say so")
    ap.add_argument("--toggle", action="store_true",
                    help="end with Hydrogen's Play/Stop toggle pressed twice (locate to the "
                         "start, stop, play) instead of a JACK relocate to frame 0; window C")
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
    pulses = []          # pulses and, in log order, the engine's relocations
    resets = []          # [relocate frame, loops' sync counters reset?] in log order
    for line in open(log):
        d = dict(kv.split("=", 1) for kv in line.split() if "=" in kv)
        if line.startswith("frame="):
            pulses.append((int(d["frame"]) + int(d["diff"]), float(d["tempoframes"])))
        elif line.startswith("event="):
            print("  engine log:", line.strip())
            if d.get("event") == "relocate":
                pulses.append(("relocate", int(d["frame"])))
                resets.append([int(d["frame"]), False])
            elif d.get("event") == "sync_reset" and resets:
                resets[-1][1] = True
    # Everything below in wall-clock JACK frames: relocations (the realign
    # among them) move the transport frame, not these.
    onsets = kick_onsets_wall(blocks)
    pulses_w = pulses_to_wall(pulses, blocks)
    res = kick_vs_pulse(onsets, pulses_w)
    print("freeze %.0f ms; kick onsets %d; engine pulses %d (%d placed in the recording)"
          % (args.freeze_ms, len(onsets), sum(1 for p in pulses if p[0] != "relocate"),
             len(pulses_w)))

    first = [d for o, d in res if marks["start"] + SR <= o < marks["incr"]]
    if not first:
        print("FAIL: no kicks before the tempo change; see", tmp)
        return 1
    base = float(np.median(first))
    print("kick minus pulse before any tempo change: %+.3f beat "
          "(output latency; subtracted below)" % base)

    def window(lo, hi, label):
        sel = sorted(wrap(d - base) for o, d in res if lo <= o < hi)
        if not sel:
            print("  %-36s no kicks" % label)
            return None
        med = sel[len(sel) // 2]
        print("  %-36s n=%3d  median %+.3f beat (range %+.3f .. %+.3f)"
              % (label, len(sel), med, sel[0], sel[-1]))
        return med

    fz = marks["freeze"]
    a0 = window(marks["start"] + 2 * SR, marks["incr"], "A0 before the tempo change")
    a = window(marks["incr"] + 2 * SR, fz, "A  after +3 BPM, before freeze")
    b_end = min(marks.get(k, marks["relocate"]) for k in ("restart", "realign", "relocate"))
    b = window(fz + SR, b_end, "B  after Hydrogen missed cycles")
    d = e = None
    if "restart" in marks:
        d = window(marks["restart"] + 5 * SR, marks.get("realign", marks["relocate"]),
                   "D  after the engine restarted")
    if "realign" in marks:
        e = window(marks["realign"] + SR, marks["relocate"], "E  after the realign")
        # The engine's grid must not move at the realign: pulse spacing in
        # wall frames stays one beat; and no kick may be dropped or doubled.
        lo, hi = marks["realign"] - SR, marks["realign"] + 2 * SR
        sp = [(b2[0] - b1[0], b1[1]) for b1, b2 in zip(pulses_w, pulses_w[1:]) if lo <= b1[0] < hi]
        worst = max((abs(s - t) for s, t in sp), default=None)
        print("  engine pulse spacing around the realign: %d spacings, worst %s frames off a beat"
              % (len(sp), "%.1f" % worst if worst is not None else "-"))
        ko = [o for o in onsets if lo <= o < hi]
        beat = pulses_w[-1][1] if pulses_w else 1.0
        iv = [(o2 - o1) / beat for o1, o2 in zip(ko, ko[1:])]
        odd = [x for x in iv if abs(x - round(x)) > 0.02 or round(x) != 1]
        print("  kick intervals around the realign (beats): %s%s"
              % (" ".join("%.3f" % x for x in iv), "" if not odd else "  <- not all one beat"))
        # The transport around the realign: periods where its frame did not
        # move on by the period (the relocation itself, or a held cycle).
        tj = [(b2[3] - marks["realign"], b2[0] - b1[0], b2[3] - b1[3], "" if b2[1] else " stopped")
              for b1, b2 in zip(blocks, blocks[1:])
              if lo <= b2[3] < hi and (not b2[1] or (b2[0] - b1[0]) != (b2[3] - b1[3]))]
        print("  transport around the realign (wall frames after the request:"
              " transport moved / wall moved): %s"
              % (" ".join("%+d:%d/%d%s" % t for t in tj) or "none"))
    c = window(marks["relocate"] + SR, 10 ** 15,
               "C  after Play/Stop toggle x2" if args.toggle else "C  after relocate to frame 0")
    # A realign must not reset the loops' sync counters, even when it
    # moves the transport backwards; other backwards relocates do (stock).
    # The realign's relocations are those before the final one to frame 0.
    realign_resets = [f for f, r in resets[:-1] if r] if (args.realign or args.auto_realign) else []
    if args.realign or args.auto_realign:
        print("  relocations (frame: loops' sync counters reset?): %s"
              % " ".join("%d:%s" % (f, "reset" if r else "kept") for f, r in resets))

    engine_out = "".join(open(os.path.join(tmp, f)).read() for f in ("engine.out", "engine2.out")
                         if os.path.exists(os.path.join(tmp, f)))
    if os.environ.get("SL_BT_KEEP"):
        print("kept", tmp)
    else:
        shutil.rmtree(tmp, ignore_errors=True)

    if (None in (a0, a, b, c) or (args.restart_engine and d is None)
            or (args.realign and e is None)):
        print("FAIL: a window had no kicks")
        return 1
    on = [abs(x) <= ON_BEAT for x in (a0, a, c)]
    # E (after a realign) must be on the beat whatever Hydrogen did.
    on_e = e is None or abs(e) <= ON_BEAT
    if realign_resets:
        print("  FAIL: the realign reset the loops' sync counters")
        on_e = False
    stopped = "auto_realign stopped" in engine_out
    if stopped:
        print("  engine:", [l for l in engine_out.splitlines() if "auto_realign stopped" in l][0])
    if stopped and not args.slow_sync:
        print("  FAIL: the engine stopped auto_realign (a hold that was not one cycle)")
        on_e = False
    if args.slow_sync and args.auto_realign and not stopped:
        print("  FAIL: a slow-sync client held the transport, and auto_realign did not stop")
        on_e = False
    if args.auto_realign:
        # With the offset cleared after the tempo change, nothing moves at
        # the freeze, whatever Hydrogen does.
        ok = all(on) and abs(b) <= ON_BEAT and (d is None or abs(d) <= ON_BEAT) and on_e
        if not resets[:-1]:
            print("  FAIL: no automatic realign seen after the tempo change")
            ok = False
    elif args.expect == "reset":
        # D is not judged: after a stock Hydrogen's reset and an engine
        # restart both are on the plain grid, by coincidence.
        ok = all(on) and abs(b) > OFF_BEAT and on_e
    else:
        ok = all(on) and abs(b) <= ON_BEAT and (d is None or abs(d) <= ON_BEAT) and on_e
    print("PASS" if ok else "FAIL", "(expect %s)" % args.expect)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
