#!/usr/bin/env python3
"""Known-answer tests for sllatency's round-trip measurement.

Starts a private dummy-backend JACK server (no hardware, no effect on a
running rig) and a helper client that stands in for the audio hardware:
it delays the click by a known number of samples and can blur it (a
simple low-pass, like a mic and headphones) and add noise.  The click
loops from sllatency's output, through the helper, back to its input, so
JACK adds one buffer for the loop; the expected round trip is the delay
plus one buffer.

Usage: test_sllatency.py            (about 30 s)
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


def main():
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
    finally:
        jackd.terminate()
        jackd.wait(timeout=5)
    print("all passed" if not fails else "%d failed" % fails)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
