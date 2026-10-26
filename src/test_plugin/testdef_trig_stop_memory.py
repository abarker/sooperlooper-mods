#! /usr/bin/env python3
"""
Regression tests for TRIG_STOP_MEMORY_FIX (plugin.cc): a recording whose
stop is still pending when the loop memory runs out.

Background
----------
STATE_TRIG_STOP keeps recording while it waits for its stop, asking
ensureLoopSpace() for room each buffer.  Stock 1.7.9 doesn't check the
result: at the end of memory it gets NULL and dereferences it, and the
engine crashes.  With the fix it ends as STATE_RECORD does at the end of
memory: the take is kept and plays.

Each scenario runs in a child process (a crash would take pytest down
with it), with SL_SAMPLE_TIME=3, which the plugin rounds up to 2^18
samples (5.46 s at 48 kHz), so the memory runs out in seconds.  The child
prints one JSON line with what it saw; a crash shows as a negative return
code.
"""

import json
import os
import subprocess
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SAMPLE_TIME = "3"


# ----------------------------------------------------------------------
# Child side

def _scenario(name, chans):
    import time
    import test_engine
    import engine_wrapper

    e = engine_wrapper.Engine("SL_test_trigstopmem", chans)
    L = e.looper
    buf = int(L.get_buffer_size())
    sr = int(e.get_sample_rate())

    def wait(n):
        t = L.process_count + n
        deadline = time.time() + 10 + 3.0 * n * buf / sr
        while L.process_count < t:
            if time.time() > deadline:
                raise RuntimeError("JACK callbacks stopped")

    def secs(s):
        wait(int(s * sr / buf) + 1)

    def st():
        return engine_wrapper.states[e.getState()]

    def chan_states():
        return [engine_wrapper.states[L.get_channel_state(i)] for i in range(chans)]

    test_engine.cvar.g_late_hit_window_ms = 0.0
    L.sync_inject_enabled = True
    L.set_port(test_engine.Quantize, 1.0)          # Cycle

    if name in ("stuck_stop", "queued_overdub"):
        # Record free, then turn Sync on with no pulses and end the take:
        # the stop waits for a pulse that never comes.
        L.sync_inject_value = 0.0
        L.set_port(test_engine.Sync, 0.0)
        wait(2)
        e.request("RECORD")
        secs(0.5)
        L.set_port(test_engine.Sync, 1.0)
        wait(2)
        e.request("OVERDUB" if name == "queued_overdub" else "RECORD")
        wait(2)
    elif name == "long_take":
        # Plain Sync, 4 s cycles: the take starts on a pulse and the stop
        # is pressed 4.3 s in, after the first boundary, so it waits for
        # the next at 8 s -- past the 5.46 s of memory.
        L.sync_inject_period = 4 * sr
        L.sync_clock = 1                          # first pulse in ~4 s
        L.set_port(test_engine.Sync, 1.0)
        wait(2)
        e.request("RECORD")
        while st() != "Recording":
            wait(1)
        secs(4.3)
        e.request("RECORD")
        wait(2)
    else:
        raise ValueError(name)

    pending = st()
    # Past the end of memory (5.46 s from the take's start) with room to
    # spare, whichever scenario.
    secs(6.5)
    out = {
        "pending": pending,
        "state": st(),
        "chan_states": chan_states(),
        "chan_lengths": [L.get_channel_loop_length(i) for i in range(chans)],
        "buffer_secs": L.get_control_value(test_engine.LoopFreeMemory),
        "sr": sr,
    }
    p0 = L.get_control_value(test_engine.LoopPosition)
    secs(0.3)
    p1 = L.get_control_value(test_engine.LoopPosition)
    out["position_moved"] = p1 != p0
    e.request("UNDO")
    wait(4)
    out["after_undo"] = st()
    print(json.dumps(out), flush=True)


# ----------------------------------------------------------------------
# Parent side

def run_scenario(name, chans=1):
    env = dict(os.environ, SL_SAMPLE_TIME=SAMPLE_TIME,
               PYTHONPATH=HERE + os.pathsep + os.environ.get("PYTHONPATH", ""))
    p = subprocess.run(
        [sys.executable, os.path.abspath(__file__), "--scenario", name,
         str(chans)],
        cwd=HERE, env=env, capture_output=True, text=True, timeout=120)
    return p


class TestTrigStopMemory(unittest.TestCase):

    def result(self, name, chans=1):
        p = run_scenario(name, chans)
        self.assertGreaterEqual(
            p.returncode, 0,
            f"engine crashed (signal {-p.returncode}) with a stop pending at "
            f"the end of loop memory\n{p.stdout}{p.stderr[-2000:]}")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr[-2000:])
        lines = [l for l in p.stdout.splitlines() if l.startswith("{")]
        self.assertTrue(lines, p.stdout + p.stderr[-2000:])
        return json.loads(lines[-1])

    def check_ended_at_memory(self, r):
        self.assertEqual(r["pending"], "WaitStop",
                         "setup: the stop was not pending")
        self.assertEqual(r["state"], "Playing")
        for s in r["chan_states"]:
            self.assertEqual(s, "Playing")
        mem = int(round(r["buffer_secs"] * r["sr"]))
        for n in r["chan_lengths"]:
            # Ends within a buffer or so of the end of memory.
            self.assertLessEqual(n, mem)
            self.assertGreater(n, mem - 4096)
        self.assertEqual(len(set(r["chan_lengths"])), 1,
                         "channels ended at different lengths")
        self.assertTrue(r["position_moved"], "the kept take is not playing")
        self.assertEqual(r["after_undo"], "Off",
                         "undo did not remove the take")

    def testStopThatNeverComes(self):
        self.check_ended_at_memory(self.result("stuck_stop"))

    def testLongTakeWithStopPastMemory(self):
        self.check_ended_at_memory(self.result("long_take"))

    def testQueuedOverdubIsDropped(self):
        self.check_ended_at_memory(self.result("queued_overdub"))

    def testStereo_StopThatNeverComes(self):
        self.check_ended_at_memory(self.result("stuck_stop", 2))


if __name__ == "__main__":
    if len(sys.argv) >= 4 and sys.argv[1] == "--scenario":
        _scenario(sys.argv[2], int(sys.argv[3]))
    else:
        unittest.main()
