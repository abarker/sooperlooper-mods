#!/usr/bin/env python3
"""Known-answer tests for sllatency's round-trip measurement.

Starts a private dummy-backend JACK server (no hardware, no effect on a
running rig) and a helper client that stands in for the audio hardware:
it delays the click by a known number of samples and can blur it (a
simple low-pass, like a mic and headphones) and add noise.  The click
loops from sllatency's output, through the helper, back to its input, so
JACK adds one buffer for the loop; the expected round trip is the delay
plus one buffer.

The second part runs the whole tool, including SooperLooper: a real
sooperlooper engine on the same private server, with its home directory
pointed at a temporary folder.  If --session is given, a *copy* of that
session file is loaded into it (so the test uses real settings without
touching the file); otherwise loop 0 gets manual 743/743 latencies over
OSC.  The loop's measured offset must equal (delay + one buffer) minus
(input_latency + output_latency).

Usage: test_sllatency.py [--engine PATH] [--session FILE]   (about 90 s)
"""
import os
import subprocess
import sys
import threading
import time

import numpy as np
import jack

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import sllatency  # noqa: E402

SERVER = "sllat_test_%d" % os.getpid()
PERIOD = 256
SR = 48000


class FakeHardware:
    """Delay (samples), optional one-pole low-pass, optional noise."""

    def __init__(self, delay, lowpass_hz=None, noise=0.0):
        self.c = jack.Client("fakehw", no_start_server=True)
        self.i = self.c.inports.register("in")
        self.o = self.c.outports.register("out")
        self.buf = np.zeros(delay + 8192, dtype=np.float32)
        self.delay = delay
        self.w = 0
        self.a = None if not lowpass_hz else np.exp(-2 * np.pi * lowpass_hz / SR)
        self.y = 0.0
        self.noise = noise
        self.rng = np.random.default_rng(1)
        self.c.set_process_callback(self.process)

    def process(self, frames):
        x = self.i.get_array().astype(np.float64)
        if self.a is not None:
            y = np.empty_like(x)
            prev = self.y
            for k in range(len(x)):
                prev = (1 - self.a) * x[k] + self.a * prev
                y[k] = prev
            self.y = prev
            x = y
        n = len(self.buf)
        idx = (self.w + np.arange(frames)) % n
        self.buf[idx] = x
        ridx = (self.w - self.delay + np.arange(frames)) % n
        out = self.buf[ridx].copy()
        if self.noise:
            out += self.noise * self.rng.standard_normal(frames).astype(np.float32)
        self.o.get_array()[:] = out
        self.w = (self.w + frames) % n


def run_case(delay, lowpass_hz=None, noise=0.0):
    hw = FakeHardware(delay, lowpass_hz, noise)
    hw.c.activate()
    try:
        rec = sllatency.ClickRecorder("sllatency", ["fakehw:in"], ["fakehw:out"])
        click = sllatency.make_click(SR, 0.3)
        sig, starts = sllatency.build_output(SR, 120.0, 4.0, click)
        (r,) = rec.run(sig)
        lags = sllatency.measure_round_trip(r, starts, click, SR, 250.0)
        return lags, rec.xruns
    finally:
        hw.c.deactivate()
        hw.c.close()


def run_tool(delay, sl_port, sl_client, extra=(), answers=None, home=None, wire=()):
    """Run the sllatency command line through the fake hardware; return
    (round trip, loop offset) parsed from its report, and the report.
    With `answers`, run interactively and feed them as typed input.  HOME
    is a temporary directory so the tool's remembered settings never
    touch the real one."""
    hw = FakeHardware(delay)
    hw.c.activate()
    for src, dst in wire:           # e.g. SooperLooper's main ports as "hardware"
        hw.c.connect(src, dst)
    import tempfile
    home = home or tempfile.mkdtemp(prefix="sllat_home_")
    cmd = [sys.executable, os.path.join(HERE, "sllatency.py"),
           "--out", "fakehw:in", "--in", "fakehw:out",
           "--sl-port", str(sl_port), "--sl-client", sl_client,
           "--as-hardware"] + list(extra)
    if answers is None:
        cmd.append("--yes")
    try:
        out = subprocess.run(cmd, input=answers, capture_output=True, text=True,
                             timeout=90, env=dict(os.environ, HOME=home)).stdout
    finally:
        hw.c.deactivate()
        hw.c.close()
    rt = off = None
    for line in out.splitlines():
        if "Round trip:" in line:
            rt = float(line.split("Round trip:")[1].split()[0])
        if "At SooperLooper's output the loop is" in line:
            off = float(line.split("the loop is")[1].split()[0])
    return rt, off, out


def sl_tests(engine, session):
    import shutil, tempfile
    sys.path.insert(0, HERE)
    tmp = tempfile.mkdtemp(prefix="sllat_sl_")
    port = 19500 + os.getpid() % 400
    client = "sl_test"
    eng = subprocess.Popen([engine, "-S", SERVER, "-p", str(port), "-l", "1",
                            "-c", "2", "-j", client],
                           env=dict(os.environ, HOME=tmp),
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    fails = 0
    try:
        time.sleep(3.0)
        sl = sllatency.SooperLooper("127.0.0.1", port)
        if session:
            copy = os.path.join(tmp, "session_copy.slsess")
            shutil.copy(session, copy)
            sl.send("/load_session", copy, sl.url, "/err")
            time.sleep(2.0)
        else:
            sl.set("autoset_latency", 0.0, loop=0)
            sl.set("input_latency", 743.0, loop=0)
            sl.set("output_latency", 743.0, loop=0)
            sl.set("trigger_latency", 0.0, loop=0)
        il = sl.get("input_latency", loop=0); ol = sl.get("output_latency", loop=0)
        auto = sl.get("autoset_latency", loop=0)
        before = {g: sl.get(g) for g in ("sync_source", "tempo", "eighth_per_cycle")}
        before["loop0 quantize"] = sl.get("quantize", loop=0)
        print("SooperLooper loop 0: input_latency %s output_latency %s autoset %s%s"
              % (il, ol, auto, " (from a copy of %s)" % session if session else ""))
        for delay in (1230, 1486, 1000):
            rt, off, report = run_tool(delay, port, client)
            exp_rt = delay + PERIOD
            exp_off = exp_rt - (il + ol)
            # SooperLooper's own record/playback arithmetic leaves a fixed
            # offset of about 3 samples (0.06 ms); allow 5.
            ok = (rt is not None and off is not None and abs(rt - exp_rt) <= 0.5
                  and abs(off - exp_off) <= 5.0)
            fails += not ok
            print("%s delay %5d: round trip %s (expected %d), loop offset %s "
                  "(expected %+g)" % ("PASS" if ok else "FAIL", delay, rt, exp_rt,
                                      off, exp_off))
            if not ok:
                print(report)
        fails += autoset_row_tests(sl, port, client)
        # A rig using SooperLooper's main (common) ports: the mic feeds the
        # main input and the main output goes to the speakers.  The
        # temporary loop must not play out through them (the mic would hear
        # it); the result must match the direct case above.
        rt, off, report = run_tool(1230, port, client, wire=(
            ("fakehw:out", "%s:common_in_1" % client),
            ("%s:common_out_1" % client, "fakehw:in")))
        exp_rt, exp_off = 1230 + PERIOD, 1230 + PERIOD - (il + ol)
        ok = (rt is not None and off is not None and abs(rt - exp_rt) <= 0.5
              and abs(off - exp_off) <= 5.0)
        fails += not ok
        print("%s main ports wired as the rig's I/O: round trip %s (expected %d), "
              "loop offset %s (expected %+g)" % ("PASS" if ok else "FAIL", rt, exp_rt,
                                                off, exp_off))
        if not ok:
            print(report)
        fails += apply_save_tests(sl, port, client, tmp, session, il, ol)
        n = sl.ping()
        after = {g: sl.get(g) for g in ("sync_source", "tempo", "eighth_per_cycle")}
        after["loop0 quantize"] = sl.get("quantize", loop=0)
        ok = n == 1 and after == before
        fails += not ok
        print("%s temporary loop removed and settings restored (loop count %s; "
              "before %s, after %s)" % ("PASS" if ok else "FAIL", n, before, after))
    finally:
        eng.terminate()
        try:
            eng.wait(timeout=5)
        except subprocess.TimeoutExpired:
            eng.kill()
        shutil.rmtree(tmp, ignore_errors=True)
    return fails


def chain_test(delay=32):
    """Through a non-hardware JACK program and back, like a DAW bus: the tool
    must report the program's own delay (the round trip minus the one period
    JACK adds to break the loop) and give no QJackctl advice."""
    hw = FakeHardware(delay)
    hw.c.activate()
    try:
        out = subprocess.run(
            [sys.executable, os.path.join(HERE, "sllatency.py"), "--no-sl", "--yes",
             "--out", "fakehw:in", "--in", "fakehw:out"],
            capture_output=True, text=True, timeout=90).stdout
    finally:
        hw.c.deactivate()
        hw.c.close()
    got = None
    flat = " ".join(out.split())
    if "program itself is" in flat:
        got = float(flat.split("program itself is")[1].split()[0])
    ok = got is not None and abs(got - delay) <= 0.5 and "QJackctl, change" not in flat
    print("%s chain through a JACK program: %s samples (expected %d), no QJackctl advice"
          % ("PASS" if ok else "FAIL", got, delay))
    if not ok:
        print(out)
    return 0 if ok else 1


def autoset_row_tests(sl, port, client):
    """The "autoset would use" row must follow SooperLooper's own rule: with
    use_common_ins/outs on it reads the common ports, which the rig leaves
    unconnected (JACK reports 0), not loop 0's own ports."""
    fails = 0
    before = {k: sl.get(k, loop=0) for k in ("use_common_ins", "use_common_outs")}
    wires = (("system:capture_1", "%s:loop0_in_1" % client),
             ("%s:loop0_out_1" % client, "system:playback_1"))
    c = jack.Client("autoset_row_test", no_start_server=True)
    c.activate()
    try:
        for src, dst in wires:
            c.connect(src, dst)
        cap = sllatency.port_latency(c.get_port_by_name("system:capture_1"), False)[1]
        pb = sllatency.port_latency(c.get_port_by_name("system:playback_1"), True)[1]
        for common, expect, short in ((1.0, (0, 0), True), (0.0, (cap, pb), False)):
            sl.set("use_common_ins", common, loop=0)
            sl.set("use_common_outs", common, loop=0)
            time.sleep(0.3)
            _, _, report = run_tool(1230, port, client)
            row = [l for l in report.splitlines() if "autoset would use" in l]
            got = tuple(int(v) for v in row[0].split("autoset would use")[1].split()[:2]) \
                if row else None
            said_short = "Autoset would come out short" in report
            ok = got == expect and said_short == short
            fails += not ok
            print("%s autoset row with use_common %d: %s (expected %s), short note %s"
                  % ("PASS" if ok else "FAIL", common, got, expect, said_short))
            if not ok:
                print(report)
    finally:
        for src, dst in wires:
            try:
                c.disconnect(src, dst)
            except jack.JackError:
                pass
        c.deactivate()
        c.close()
        for k, v in before.items():
            sl.set(k, v, loop=0)
    return fails


def _session_file(sl, tmp, session, name):
    """A session file to save into: a copy of the given one, or one written
    by SooperLooper itself."""
    import shutil
    path = os.path.join(tmp, name)
    if session:
        shutil.copy(session, path)
    else:
        sl.send("/save_session", path, sl.url, "/err")
        time.sleep(1.0)
    return path


def _check_saved(path, orig_text, new_in, new_out):
    """Only the latency values and auto_latency may differ; backup exists."""
    import glob, re
    now = open(path).read()
    lines_a, lines_b = orig_text.splitlines(), now.splitlines()
    changed = [(x, y) for x, y in zip(lines_a, lines_b) if x != y]
    ok = len(lines_a) == len(lines_b) and all(
        ("input_latency" in x or "output_latency" in x or "auto_latency" in x)
        for x, _ in changed)
    ok = ok and ('name="input_latency" value="%d"' % new_in) in now
    ok = ok and ('name="output_latency" value="%d"' % new_out) in now
    ok = ok and bool(glob.glob(path + ".bak-*"))
    return ok, len(changed)


def apply_save_tests(sl, port, client, tmp, session, il, ol):
    """Step 3: scripted --apply --save, then the interactive questions."""
    fails = 0
    delay = 1486                          # loops heard 256 - 3 samples late
    # scripted
    path = _session_file(sl, tmp, session, "scripted.slsess")
    orig = open(path).read()
    rt, off, report = run_tool(delay, port, client,
                               ["--apply", "--save", "--session-file", path])
    exp_total = int(il + ol) + int(round(off))
    got = (sl.get("input_latency", loop=0), sl.get("output_latency", loop=0))
    saved_ok, nchanged = _check_saved(path, orig, int(got[0]), int(got[1]))
    ok = (got[0] is not None and int(got[0] + got[1]) == exp_total
          and sl.get("autoset_latency", loop=0) == 0.0 and saved_ok)
    fails += not ok
    print("%s scripted --apply --save: live %s, total %s (expected %d), file "
          "changed only in %d latency line(s), backup kept"
          % ("PASS" if ok else "FAIL", got, None if got[0] is None else got[0] + got[1],
             exp_total, nchanged))
    if not ok:
        print(report)
    # put loop 0 back for the interactive run
    for k, v in (("input_latency", il), ("output_latency", ol)):
        sl.set(k, v, loop=0)
    time.sleep(0.3)
    # interactive: Enter to start, y to apply, y to save, then the file path
    path2 = _session_file(sl, tmp, session, "interactive.slsess")
    orig2 = open(path2).read()
    rt, off, report = run_tool(delay, port, client,
                               answers="\ny\ny\n%s\n" % path2)
    got2 = (sl.get("input_latency", loop=0), sl.get("output_latency", loop=0))
    saved_ok2, n2 = _check_saved(path2, orig2, int(got2[0]), int(got2[1]))
    ok2 = got2 == got and saved_ok2 and "Save them in the session file?" in report
    fails += not ok2
    print("%s interactive apply and save (asked for the file path): live %s, "
          "file changed in %d line(s)" % ("PASS" if ok2 else "FAIL", got2, n2))
    if not ok2:
        print(report)
    for k, v in (("input_latency", il), ("output_latency", ol)):
        sl.set(k, v, loop=0)
    return fails


def suggest_tests():
    """Output suggestion, from JACK's routing (no server needed)."""
    rig = {  # this rig's routing, 2026-09-24
        "system:playback_1": ["PipeWire:playback_FL", "ardour:Master/audio_out 1",
                              "ardour:voc_mon/audio_out 1"],
        "system:playback_2": ["PipeWire:playback_FR", "ardour:Master/audio_out 2",
                              "ardour:voc_mon/audio_out 2"],
        "system:playback_3": [],
        "system:playback_4": ["ardour:SL_vol_mon/audio_out 1", "ardour:SL_vol_mon/audio_out 2"],
    }
    no_master = {"system:playback_1": [], "system:playback_2": ["hydrogen:out_L"],
                 "system:playback_3": ["hydrogen:out_R", "x:y"], "system:playback_4": []}
    nothing = {"system:playback_1": [], "system:playback_2": [], "system:playback_3": []}
    cases = [(rig, ["system:playback_1", "system:playback_2"]),
             (no_master, ["system:playback_2", "system:playback_3"]),
             (nothing, ["system:playback_1", "system:playback_2"])]
    fails = 0
    for feeds, want in cases:
        got = sllatency.suggest_outputs(feeds)
        ok = got == want
        fails += not ok
        print("%s output suggestion: %s" % ("PASS" if ok else "FAIL", got))
    return fails


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--engine", default=os.path.join(HERE, "..", "src", "sooperlooper"))
    ap.add_argument("--session", help="SooperLooper session file to load a copy of")
    args = ap.parse_args()
    os.environ["JACK_DEFAULT_SERVER"] = SERVER
    os.environ["JACK_NO_START_SERVER"] = "1"
    jackd = subprocess.Popen(["jackd", "-n", SERVER, "-d", "dummy", "-r", str(SR),
                              "-p", str(PERIOD)],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    fails = suggest_tests()
    try:
        time.sleep(2.0)
        cases = [
            (0, None, 0.0, 0.5),
            (100, None, 0.0, 0.5),
            (1486, None, 0.0, 0.5),         # about the rig's 743 + 743
            (1486, 4000.0, 0.0, 1.5),       # blurred like a mic (the filter itself adds ~1 sample)
            (1486, 4000.0, 0.003, 1.5),     # blurred and noisy
        ]
        for delay, lp, noise, tol in cases:
            lags, xruns = run_case(delay, lp, noise)
            expect = delay + PERIOD
            med = float(np.median(lags)) if len(lags) else float("nan")
            ok = len(lags) >= 6 and abs(med - expect) <= tol and xruns == 0
            fails += not ok
            print("%s delay %5d lowpass %-6s noise %.3f: %2d clicks, median %8.2f, "
                  "expected %d (+/-%.1f), xruns %d"
                  % ("PASS" if ok else "FAIL", delay, lp, noise, len(lags), med,
                     expect, tol, xruns))
        fails += chain_test()
        fails += sl_tests(args.engine, args.session)
    finally:
        jackd.terminate()
        jackd.wait(timeout=5)
    print("all passed" if not fails else "%d failed" % fails)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
