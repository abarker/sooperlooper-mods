#!/usr/bin/env python3
"""JackSync integration test for ALB_JACK_BEAT_TRACKING.

Runs the real sooperlooper engine against a scripted JACK timebase master
that behaves like Hydrogen 1.2.x: its beat stays continuous through tempo
changes made while rolling, and it re-derives its position from the frame
on a relocate.  The master records the tempo it reported for every audio
cycle, from which this script computes where the master's beats are,
independently of the engine's code.  Every sync pulse the engine logged
must fall on one of them.

Covers: tempo ramps while rolling, a relocate while rolling, stop/start
without a locate, and a stop / locate 0 / start (a pedal stop/start).

Everything is private: a uniquely named dummy-backend JACK server (no
hardware, no effect on a running rig), and HOME pointed at a temporary
directory so the engine's pulse log lands there.

Requires: jackd, oscsend (liblo), python3 with the `jack` module, and an
engine built with ALB_SYNC_PULSE_LOG defined (the test reads pulses from
that log).

Usage:
    jack_integration_test.py [--engine PATH] [--expect stock|tracking]

--expect tracking (default): pass if every pulse is within 2 samples of
the master's beat.  --expect stock: pass if the pulses do NOT all line
up, i.e. the bug is reproduced -- for checking an ALB_JACK_BEAT_TRACKING-off
build.  Exit status 0 on pass.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time

import jack

SR = 48000
PERIOD = 256
# Fixed: JACK2 has a few server slots and reuses a dead server's only for a
# server of the same name, so a new name per run eventually ran out of them.
SERVER = "sl_bt_test"
OSC_PORT = 19000 + os.getpid() % 900


def run_master(env):
    """Drive the transport like Hydrogen; return [(frame, bpm)] per rolling cycle."""
    os.environ.update(env)
    bpm = [120.0]
    cycles = []
    client = jack.Client("fake_h2", no_start_server=True)

    def timebase(state, blocksize, pos, new_pos):
        pos.valid = pos.valid | jack._lib.JackPositionBBT
        pos.bar, pos.beat, pos.tick = 1, 1, 0          # Hydrogen pattern mode
        pos.bar_start_tick = 0.0
        pos.beats_per_bar, pos.beat_type = 2.0, 4.0
        pos.ticks_per_beat = 48.0
        pos.beats_per_minute = bpm[0]
        if state == jack.ROLLING:
            cycles.append((pos.frame, bpm[0]))

    client.set_timebase_callback(timebase, conditional=False)
    client.activate()

    def ramp(to, step, dt):
        while abs(bpm[0] - to) > 1e-9:
            bpm[0] = min(to, bpm[0] + step) if to > bpm[0] else max(to, bpm[0] - step)
            time.sleep(dt)

    client.transport_locate(0); time.sleep(0.3)
    client.transport_start(); time.sleep(3.0)
    ramp(150.0, 2.0, 0.1); time.sleep(3.0)             # repeat-mode ramp up
    ramp(97.0, 3.0, 0.08); time.sleep(3.0)             # and down
    client.transport_locate(client.transport_frame + 300000)  # jump while rolling
    time.sleep(2.0)
    ramp(131.0, 1.0, 0.05); time.sleep(2.0)
    client.transport_stop(); time.sleep(0.5)           # stop/start, no locate
    client.transport_start(); time.sleep(2.0)
    ramp(125.0, 1.0, 0.1); time.sleep(1.0)
    client.transport_stop(); time.sleep(0.5)           # pedal: stop, locate 0, start
    client.transport_locate(0); time.sleep(0.3)
    client.transport_start(); time.sleep(2.0)
    ramp(140.0, 3.0, 0.1); time.sleep(2.0)
    client.transport_stop(); time.sleep(0.5)
    client.deactivate(); client.close()
    return cycles


def master_sessions(cycles):
    """Split the master's cycles at relocates; for each session return a
    function mapping frame -> (master beat position, beats per frame),
    with Hydrogen-style continuity through tempo changes."""
    sessions = []
    segs = []                      # (frame_start, beats_at_start, rate)
    prev_end = None
    beats = 0.0
    for f, b in cycles:
        rate = b / (60.0 * SR)
        if prev_end is None or f != prev_end:
            if segs:
                sessions.append(segs)
            segs = []
            beats = f * rate       # relocate (or start): position from the frame
        segs.append((f, beats, rate))
        beats += PERIOD * rate
        prev_end = f + PERIOD
    if segs:
        sessions.append(segs)

    def make_at(segs):
        def at(frame):
            lo, hi, best = 0, len(segs) - 1, None
            while lo <= hi:
                mid = (lo + hi) // 2
                if segs[mid][0] <= frame:
                    best = mid; lo = mid + 1
                else:
                    hi = mid - 1
            if best is None or frame - segs[best][0] >= PERIOD:
                return None
            f0, b0, r = segs[best]
            return b0 + (frame - f0) * r, r
        return at
    return [make_at(sg) for sg in sessions]


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--engine", default=os.path.join(here, "..", "sooperlooper"))
    ap.add_argument("--expect", choices=["tracking", "stock"], default="tracking")
    args = ap.parse_args()

    tmp = tempfile.mkdtemp(prefix="sl_bt_test_")
    env = {"JACK_DEFAULT_SERVER": SERVER, "JACK_NO_START_SERVER": "1"}
    procs = []
    try:
        jackd = subprocess.Popen(["jackd", "-n", SERVER, "-d", "dummy",
                                  "-r", str(SR), "-p", str(PERIOD)],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        procs.append(jackd)
        time.sleep(2.0)
        eng_env = dict(os.environ, HOME=tmp, **env)
        eng = subprocess.Popen([args.engine, "-S", SERVER, "-p", str(OSC_PORT),
                                "-l", "1", "-c", "2", "-j", "sl_bt"],
                               env=eng_env, stdout=subprocess.DEVNULL,
                               stderr=open(os.path.join(tmp, "engine.err"), "w"))
        procs.append(eng)
        time.sleep(3.0)
        for msg in (["/set", "sf", "sync_source", "-1"],
                    ["/set", "sf", "eighth_per_cycle", "2"],
                    ["/sl/0/set", "sf", "quantize", "1"]):
            subprocess.run(["oscsend", "localhost", str(OSC_PORT)] + msg, check=True)
        time.sleep(0.5)
        cycles = run_master(env)
        time.sleep(1.5)
    finally:
        for p in reversed(procs):
            p.terminate()
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()

    log = os.path.join(tmp, ".sooperlooper", "logs", "sl_sync_pulse.log")
    if not os.path.exists(log):
        print("FAIL: no pulse log -- was the engine built with ALB_SYNC_PULSE_LOG?")
        print("engine stderr in", tmp)
        return 1
    with open(os.path.join(tmp, "engine.err")) as f:
        banner = [l.strip() for l in f if "jack_beat_tracking" in l]
    # Pulses per session, split at the engine's own relocate lines, matched
    # in order to the master's sessions.
    log_sessions = [[]]
    for line in open(log):
        if line.startswith("event=relocate"):
            log_sessions.append([])
        elif line.startswith("frame="):
            d = dict(kv.split("=", 1) for kv in line.split())
            log_sessions[-1].append(int(d["frame"]) + int(d["diff"]))
    masters = master_sessions(cycles)
    if len(masters) != len(log_sessions):
        print("FAIL: master had %d transport sessions, engine log %d"
              % (len(masters), len(log_sessions)))
        return 1
    errs = []
    for at, pulses in zip(masters, log_sessions):
        for p in pulses:
            r = at(p)
            if r is None:
                continue
            beats, rate = r
            errs.append(abs(beats - round(beats)) / rate)  # frames off a beat
    errs.sort()
    if os.environ.get("SL_BT_KEEP"):
        with open(os.path.join(tmp, "cycles.txt"), "w") as f:
            for c in cycles:
                f.write("%d %g\n" % c)
        print("kept", tmp)
    else:
        shutil.rmtree(tmp, ignore_errors=True)

    if banner:
        print(banner[0])
    if not errs:
        print("FAIL: no pulses matched to master cycles (%d pulses, %d cycles)"
              % (len(pulses), len(cycles)))
        return 1
    bad = [e for e in errs if e > 2.0]
    print("%d pulses checked against the master's beat: median %.2f, max %.1f "
          "frames off; %d more than 2 frames off"
          % (len(errs), errs[len(errs) // 2], errs[-1], len(bad)))
    if args.expect == "tracking":
        ok = not bad
    else:
        ok = len(bad) > len(errs) // 4
    print("PASS" if ok else "FAIL", "(expect %s)" % args.expect)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
