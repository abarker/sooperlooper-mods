#!/usr/bin/env python3
"""Do new takes line up with Hydrogen's drums: in step, after Hydrogen
drops its tempo offset, and after Hydrogen's Play/Stop toggle?

The real programs on a private dummy JACK server (no hardware, no effect on
a running rig):

- Hydrogen in pattern mode as timebase master, own HOME, OSC on a private
  port, no window, a one-bar pattern with an accented downbeat (beat 1 at
  velocity 1, beats 2-4 at 0.4);
- the engine: 3 loops, JACK sync, one bar per cycle (8 eighths), quantize
  to the cycle; Sync and mute_quantized switched on at a transport start and
  off at a stop, the way the rig's big_tempo does;
- one probe client that records Hydrogen and the engine's output, and plays
  a "player": at a drum downbeat it copies the drums into the engine's
  input and presses record (OSC, a few ms late, like a hand), and 8 beats
  less 0.1 later stops copying and presses record again, just before the
  next downbeat.  A player exactly in time with the drums.

Takes, each in its own loop, the earlier loops muted:
  A  after +3 BPM while rolling (the engine's beat count carries Hydrogen's
     tempo offset): expected complete and in time;
  B  after a Hydrogen freeze (SIGSTOP, 30 ms) made it drop its offset while
     the engine kept its count, so the engine's bar lines are off the
     drums' (tried up to five times: +3 BPM and a freeze); reported only:
     the loop comes out the right length and in time, but its record window
     is shifted by the offset, so part of the phrase is missing;
  C  after Hydrogen's Play/Stop toggle pressed twice (locate to the start
     and stop, then play): expected complete and in time again.

For each take: the loop's length (asked over OSC), its hits per pass (8
expected), their offset from the nearest live drum hit, and the engine's
cycle pulses against the live downbeats.  PASS: A and C each 8.000 beats
long, 8 hits per pass, every hit within 0.02 beat of the offset A shows
(output latency).

The dummy server skips cycles when the machine is busy, and the engine
and Hydrogen then drift apart by whole periods; a run where that happens
during a take is reported INCONCLUSIVE (exit status 2) -- run it again on
a quiet machine.  Other jobs at normal priority starve it when it runs
under nice 19.

Requires what hydrogen_missed_cycle_test.py does: jackd, oscsend (liblo),
python3 with the `jack` module and numpy, an engine built with
ALB_SYNC_PULSE_LOG defined, and Hydrogen (--h2-root, or --appimage).

Usage:
    hydrogen_take_test.py [--engine PATH] [--appimage FILE] [--h2-root DIR]

Set SL_BT_KEEP=1 to keep the run's files (and data.npz, the recording) for
inspection.  About 90 s.
"""
import argparse
import os
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hydrogen_missed_cycle_test as H  # noqa: E402

SR, SERVER = H.SR, H.SERVER
# Longer than the missed-cycle test's: under nice 19 and without realtime
# scheduling, 256-frame cycles left the dummy server skipping some even on
# a quiet machine.  Timing here is judged in fractions of a beat.
PERIOD = 1024
ON_BEAT = 0.02          # beats


def write_song(root, path):
    """The missed-cycle test's song in pattern mode, beats 2-4 quieter."""
    H.write_song(root, path, "pattern")
    s = open(path).read()

    def vel(m):
        v = "1" if int(m.group(1)) == 0 else "0.4"
        return m.group(0).replace("<velocity>1</velocity>", "<velocity>%s</velocity>" % v)
    s = re.sub(r"<position>(\d+)</position>\s*<leadlag>0</leadlag>\s*<velocity>1</velocity>",
               vel, s)
    with open(path, "w") as f:
        f.write(s)


def onsets(blocks, idx):
    """[(wall frame, peak)] of hits in channel idx of blocks of (transport
    frame, rolling, drums, engine out, wall frame)."""
    out = []
    for b in blocks:
        for i in np.nonzero(np.abs(b[idx]) > 0.05)[0]:
            o = b[4] + int(i)
            if not out or o - out[-1][0] > 0.35 * SR:   # past the kick's tail
                out.append([o, 0.0])
    if not out:
        return out
    wall = np.concatenate([np.arange(b[4], b[4] + len(b[idx])) for b in blocks])
    sig = np.concatenate([b[idx] for b in blocks])
    for o in out:
        j = int(np.searchsorted(wall, o[0]))
        o[1] = float(np.max(np.abs(sig[j:j + int(0.01 * SR)])))
    return out


def osc_get(port, path, ctrl):
    """One control value from the engine over OSC (/sl/N/get)."""
    def pad(b):
        b += b"\0"
        return b + b"\0" * ((4 - len(b) % 4) % 4)
    sk = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sk.bind(("127.0.0.1", 0))
    sk.settimeout(2.0)
    url = "osc.udp://127.0.0.1:%d/" % sk.getsockname()[1]
    sk.sendto(pad(path.encode()) + pad(b",sss") + pad(ctrl.encode()) + pad(url.encode())
              + pad(b"/r"), ("127.0.0.1", port))
    data = sk.recvfrom(1024)[0]
    sk.close()
    return struct.unpack(">f", data[-4:])[0]


def read_pulse_log(path):
    """The engine's pulses and, in log order, its relocations."""
    pulses = []
    for line in open(path):
        d = dict(kv.split("=", 1) for kv in line.split() if "=" in kv)
        if line.startswith("frame="):
            pulses.append((int(d["frame"]) + int(d["diff"]), float(d["tempoframes"])))
        elif d.get("event") == "relocate":
            pulses.append(("relocate", int(d["frame"])))
    return pulses


def run(args, tmp, h2_root):
    # The probe's callback and the main thread share Python's interpreter
    # lock; switch often so the callback is never held up for a period.
    sys.setswitchinterval(0.0005)
    env = dict(os.environ, JACK_NO_START_SERVER="1", JACK_DEFAULT_SERVER=SERVER)
    procs = []

    def spawn(cmd, penv, log):
        p = subprocess.Popen(cmd, env=penv, stdout=open(os.path.join(tmp, log), "w"),
                             stderr=subprocess.STDOUT, start_new_session=True)
        procs.append(p)
        return p

    sl_home, h2_home = os.path.join(tmp, "slhome"), os.path.join(tmp, "h2home")
    os.makedirs(sl_home)
    H.write_h2_config(h2_root, h2_home)
    song = os.path.join(tmp, "test.h2song")
    write_song(h2_root, song)
    apprun = os.path.join(h2_root, "AppRun")
    h2_cmd = [apprun] if os.path.exists(apprun) else [os.path.join(h2_root, "usr", "bin", "hydrogen")]

    def slo(*m):
        H.osc(H.SL_OSC_PORT, *m)

    def h2o(*m):
        H.osc(H.H2_OSC_PORT, *m)

    blocks, takes = [], []
    try:
        spawn(["jackd", "-n", SERVER, "-r", "-d", "dummy", "-r", str(SR), "-p", str(PERIOD)],
              env, "jackd.out")
        time.sleep(2)
        import jack
        rec = jack.Client("rec", servername=SERVER, no_start_server=True)
        r_h2 = rec.inports.register("h2")
        r_sl = rec.inports.register("sl")
        p_out = rec.outports.register("play")
        g = {"armed": False, "left": 0, "acc": 1e9, "opened": None, "closed": None}

        @rec.set_process_callback
        def process(n):
            st, pos = rec.transport_query()
            x = r_h2.get_array()
            w = rec.last_frame_time
            blocks.append((pos["frame"], st == jack.ROLLING, x.copy(), r_sl.get_array().copy(), w))
            # The player.
            y = p_out.get_array()
            y[:] = 0.0
            if g["armed"] and np.max(np.abs(x)) > g["acc"]:
                g["armed"] = False
                bpm = pos.get("beats_per_minute") or 118.0
                g["left"] = int((8 - 0.1) * 60.0 / bpm * SR)
                g["opened"] = w
            if g["left"] > 0:
                k = min(n, g["left"])
                y[:k] = x[:k]
                g["left"] -= k
                if g["left"] == 0:
                    g["closed"] = w + k

        rec.activate()
        spawn([args.engine, "-S", SERVER, "-p", str(H.SL_OSC_PORT), "-l", "3", "-c", "2",
               "-j", "sl_h2"], dict(env, HOME=sl_home), "engine.out")
        h2 = spawn(h2_cmd + ["--child", "-n", "-s", song, "-O", str(H.H2_OSC_PORT),
                             "-L", os.path.join(tmp, "h2.log"), "-T", "-V", "Warning"],
                   dict(env, HOME=h2_home, QT_QPA_PLATFORM="offscreen"), "h2.out")
        time.sleep(8)
        for m in (["/set", "sf", "sync_source", "-1"], ["/set", "sf", "eighth_per_cycle", "8"],
                  ["/sl/-1/set", "sf", "quantize", "1"]):
            slo(*m)
        h2l = [p.name for p in rec.get_ports("Hydrogen:", is_audio=True, is_output=True)
               if p.name.endswith("out_L")]
        sl_in = [p.name for p in rec.get_ports("sl_h2:common_in", is_audio=True, is_input=True)]
        sl_out = [p.name for p in rec.get_ports("sl_h2:common_out", is_audio=True, is_output=True)]
        if not h2l or not sl_in or not sl_out:
            print("FAIL: Hydrogen or the engine did not join the test JACK server; see", tmp)
            return None
        rec.connect(h2l[0], "rec:h2")
        rec.connect(sl_out[0], "rec:sl")
        for p in sl_in:
            rec.connect("rec:play", p)

        def big_tempo(rolling):
            v = "1" if rolling else "0"
            slo("/sl/-1/set", "sf", "sync", v)
            slo("/sl/-1/set", "sf", "mute_quantized", v)

        rec.transport_locate(0)
        h2o("/Hydrogen/BPM", "f", "115")
        time.sleep(1)
        blocks.clear()
        rec.transport_start()
        big_tempo(True)
        time.sleep(3)
        peak = max(float(np.max(np.abs(b[2]))) for b in blocks[-400:])
        g["acc"] = 0.7 * peak               # accented downbeat vs 0.4 velocity
        bpm_now = [115.0]

        def tempo_up():
            for _ in range(3):
                h2o("/Hydrogen/BPM_INCR", "f", "1")
                time.sleep(0.1)
            bpm_now[0] += 3

        def take(loop, label):
            g["opened"] = g["closed"] = None
            g["armed"] = True
            while g["opened"] is None:
                time.sleep(0.005)
            slo("/sl/%d/hit" % loop, "s", "record")
            while g["closed"] is None:
                time.sleep(0.005)
            slo("/sl/%d/hit" % loop, "s", "record")
            time.sleep(2.5)                 # the stop waits for the next bar line
            m0 = rec.frame_time
            time.sleep(8.5)                 # two passes of the loop
            m1 = rec.frame_time
            ln = osc_get(H.SL_OSC_PORT, "/sl/%d/get" % loop, "loop_len")
            slo("/sl/%d/hit" % loop, "s", "mute")
            time.sleep(0.5)
            takes.append({"label": label, "open": g["opened"], "close": g["closed"],
                          "m0": m0, "m1": m1, "len": ln})

        def grid_offset():
            """Engine pulses minus the nearest live downbeat over the last
            3 s (median, beats), in transport frames."""
            pl = [p for p, _ in read_pulse_log(log_path) if p != "relocate"]
            bl = blocks[-int(3 * SR / PERIOD):]
            acc = []
            for b in bl:
                if b[1]:
                    i = np.nonzero(np.abs(b[2]) > g["acc"])[0]
                    if len(i) and (not acc or b[0] + i[0] - acc[-1] > SR // 2):
                        acc.append(b[0] + int(i[0]))
            beat = 60.0 / bpm_now[0] * SR
            offs = [(p - min(acc, key=lambda a: abs(a - p))) / beat
                    for p in pl if acc and bl[0][0] <= p <= bl[-1][0]]
            return float(np.median(offs)) if offs else 0.0

        log_path = os.path.join(sl_home, ".sooperlooper", "logs", "sl_sync_pulse.log")
        tempo_up()
        time.sleep(2)
        take(0, "A in step, after +3 BPM while rolling")
        for attempt in range(5):
            tempo_up()
            time.sleep(2.0)
            os.kill(h2.pid, signal.SIGSTOP)
            time.sleep(0.03)
            os.kill(h2.pid, signal.SIGCONT)
            time.sleep(4.0)
            off = grid_offset()
            print("freeze %d at %.0f BPM: engine bar lines minus the drums' %+.3f beat"
                  % (attempt + 1, bpm_now[0], off))
            if abs(off) > 0.1:
                break
        take(1, "B engine bar lines off the drums")
        h2o("/Hydrogen/PLAY_STOP_TOGGLE", "f", "1")
        time.sleep(0.2)
        big_tempo(False)
        time.sleep(1.3)
        h2o("/Hydrogen/PLAY_STOP_TOGGLE", "f", "1")
        time.sleep(0.2)
        big_tempo(True)
        time.sleep(3)
        take(2, "C after Play/Stop toggle x2")
        rec.transport_stop()
        time.sleep(0.5)
        rec.deactivate()
        return blocks, takes, log_path
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
    args = ap.parse_args()

    h2_root = H.find_h2_root(args)
    H.offscreen_plugin(h2_root)
    tmp = tempfile.mkdtemp(prefix="sl_take_test_")
    res = run(args, tmp, h2_root)
    if res is None:
        return 1
    blocks, takes, log_path = res
    if not os.path.exists(log_path):
        print("FAIL: no pulse log -- was the engine built with ALB_SYNC_PULSE_LOG?")
        return 1

    live = onsets(blocks, 2)
    loop = onsets(blocks, 3)
    lpk = max(p for _, p in live)
    downbeats = [o for o, p in live if p > 0.7 * lpk]
    lall = np.array([o for o, _ in live])
    pw = [p for p, _ in H.pulses_to_wall(read_pulse_log(log_path),
                                         [(b[0], b[1], b[2], b[4]) for b in blocks])]
    wall = [b[4] for b in blocks]
    skips = [w2 for w1, w2 in zip(wall, wall[1:]) if w2 - w1 != PERIOD]
    if os.environ.get("SL_BT_KEEP"):
        np.savez(os.path.join(tmp, "data.npz"),
                 wall=np.concatenate([np.arange(b[4], b[4] + len(b[2])) for b in blocks]),
                 drums=np.concatenate([b[2] for b in blocks]),
                 engine=np.concatenate([b[3] for b in blocks]), pulses=np.array(pw))

    res = {}
    base = None
    for t in takes:
        lo_w, hi_w = t["open"] - 2 * SR, t["m1"]
        sp = [b - a for a, b in zip(pw, pw[1:]) if lo_w <= a <= hi_w]
        beat = float(np.median(sp)) / 4.0 if sp else 60.0 / 118.0 * SR
        bar_off = [(p - min(downbeats, key=lambda a: abs(a - p))) / beat
                   for p in pw if lo_w <= p <= hi_w]
        hits = [(o, p) for o, p in loop if t["m0"] <= o < t["m1"]]
        dev = [(o - lall[int(np.argmin(np.abs(lall - o)))]) / beat for o, _ in hits]
        per_pass = len(hits) / ((t["m1"] - t["m0"]) / (8 * beat))
        length = t["len"] * SR / beat
        skipped = sum(1 for w in skips if lo_w <= w <= hi_w)
        print("== take %s" % t["label"])
        print("  engine bar lines minus the drums' (beats): %s"
              % " ".join("%+.3f" % x for x in bar_off[:6]))
        if dev:
            print("  loop %.3f beats long; %.1f hits per pass (8 expected); hits minus the"
                  " nearest drum hit: median %+.3f beat (range %+.3f .. %+.3f)"
                  % (length, per_pass, float(np.median(dev)), min(dev), max(dev)))
        else:
            print("  loop %.3f beats long; plays nothing" % length)
        if skipped:
            print("  the dummy server skipped cycles %d time(s) during this take" % skipped)
        if base is None and dev:
            base = float(np.median(dev))
        res[t["label"][0]] = (length, per_pass, dev, skipped, bar_off)

    if os.environ.get("SL_BT_KEEP"):
        print("kept", tmp)
    else:
        shutil.rmtree(tmp, ignore_errors=True)

    if any(res[k][3] for k in res):
        print("INCONCLUSIVE: the dummy server skipped cycles during a take (machine busy?);"
              " run again")
        return 2
    b_off = res.get("B", (0, 0, [], 0, []))[4]
    if not b_off or abs(float(np.median(b_off))) <= 0.1:
        print("note: take B never got the engine's bar lines off the drums")

    def good(k):
        length, per_pass, dev, _, _ = res.get(k, (0, 0, [], 0, []))
        return (abs(length - 8.0) < 0.01 and abs(per_pass - 8.0) < 0.5 and dev
                and base is not None and max(abs(d - base) for d in dev) <= ON_BEAT)
    ok = good("A") and good("C")
    print("PASS" if ok else "FAIL", "(takes A and C complete and in time)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
