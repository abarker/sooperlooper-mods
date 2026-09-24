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


def run_tool(delay, sl_port, sl_client):
    """Run the sllatency command line through the fake hardware; return
    (round trip, loop offset) parsed from its report, and the report."""
    hw = FakeHardware(delay)
    hw.c.activate()
    try:
        out = subprocess.run(
            [sys.executable, os.path.join(HERE, "sllatency.py"), "--yes",
             "--out", "fakehw:in", "--in", "fakehw:out",
             "--sl-port", str(sl_port), "--sl-client", sl_client],
            capture_output=True, text=True, timeout=90).stdout
    finally:
        hw.c.deactivate()
        hw.c.close()
    rt = off = None
    for line in out.splitlines():
        if "Round trip:" in line:
            rt = float(line.split("Round trip:")[1].split()[0])
        if "loop playback is" in line:
            off = float(line.split("loop playback is")[1].split()[0])
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
    fails = 0
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
        fails += sl_tests(args.engine, args.session)
    finally:
        jackd.terminate()
        jackd.wait(timeout=5)
    print("all passed" if not fails else "%d failed" % fails)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
