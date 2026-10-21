#! /usr/bin/env python3
"""
Regression tests for MULTIPLY_END_FIX (plugin.cc): a multiply end leaves
no silent stretch at the end of the multiplied loop.

The take is a constant level and the input is silent during the multiply,
so the multiplied loop is the take repeated (times the safety feedback,
0.96, on the copies) and plays back at a steady level.  Stock 1.7.9
finished a multiply when the play position reached the end, with the
record position still IL + OL short of it, so the last IL + OL samples of
every pass were never written: the output dropped to silence there.

The take is recorded with sync off; then sync goes on with no pulses but
one, sent to start the multiply.  An end press then waits for a pulse that
never comes, and the multiply finishes where the bug was: when the play
position reaches the loop's end (as on a rig, where it gets there IL + OL
before the beat).  With sync off a Round-off or Overdub end would take
effect at once and miss that path.  The late multiply end (late-hit) is
covered by the dummy-server probes, except for TestLateEndMidCycle below.
"""

import time
import unittest
import test_engine
import engine_wrapper


class MultiplyEndTestCase(unittest.TestCase):

    IL, OL = 300, 500
    LEVEL = 0.5

    def setUp(self):
        self.engine = engine_wrapper.Engine("SL_test_multend")
        self.looper = self.engine.looper
        self.buf = int(self.looper.get_buffer_size())
        self.sr = int(self.engine.get_sample_rate())
        self.looper.sync_inject_value = 0.0      # armed before Sync goes on
        self.looper.sync_inject_enabled = True
        self.looper.set_port(test_engine.Sync, 0.0)
        self.looper.set_port(test_engine.DryLevel, 0.0)
        self.looper.set_port(test_engine.WetLevel, 1.0)
        self.looper.set_port(test_engine.InputLatency, float(self.IL))
        self.looper.set_port(test_engine.OutputLatency, float(self.OL))
        self.looper.set_port(test_engine.FadeSamples, 64.0)
        self.looper.set_port(test_engine.Quantize, 1.0)       # cycle

    def tearDown(self):
        del self.engine

    def wait_buffers(self, n, timeout=5.0):
        target = self.looper.process_count + n
        period = float(self.buf) / float(self.sr)
        deadline = time.time() + max(timeout, 3.0 * n * period)
        while self.looper.process_count < target:
            if time.time() > deadline:
                raise RuntimeError(
                    f"Timed out waiting for {n} JACK callbacks "
                    f"(process_count stuck at {self.looper.process_count})")

    def assertState(self, state):
        actual = self.engine.getState()
        self.assertEqual(actual, engine_wrapper.states[state],
                         engine_wrapper.states[actual] + " != " + state)

    def loop_len(self):
        return int(round(self.looper.get_control_value(test_engine.LoopLength) * self.sr))

    def take_bufs(self):
        """A take several times longer than IL + OL."""
        return max(12, 4 * (self.IL + self.OL) // self.buf + 1)

    def record_take(self):
        self.looper.input_inject_value = self.LEVEL
        self.engine.request("RECORD")
        self.wait_buffers(self.take_bufs())
        self.engine.request("RECORD")
        self.wait_buffers(3)
        self.assertState("Playing")
        self.take = self.loop_len()
        self.looper.input_inject_value = 0.0      # silence from here on
        self.looper.set_port(test_engine.Sync, 1.0)

    def multiply(self, end_with="MULTIPLY"):
        """Multiply for about one and a half takes, then end it."""
        tb = self.take // self.buf + 1
        self.engine.request("MULTIPLY")
        self.wait_buffers(2)
        self.looper.sync_inject_value = 2.0       # the one pulse: start
        self.wait_buffers(2)                      # one whole callback sees it
        self.looper.sync_inject_value = 0.0
        self.wait_buffers(2)
        self.assertState("Multiplying")
        self.wait_buffers(tb + tb // 2)
        self.engine.request(end_with)
        self.wait_buffers(2 * tb)                 # rounding to the cycle end

    def assertSteadyPass(self, what):
        L = self.loop_len()
        self.wait_buffers(3)
        self.looper.reset_output_stats()
        self.wait_buffers(L // self.buf + 3)      # a whole pass of the loop
        lo, hi = self.looper.output_min, self.looper.output_max
        self.assertTrue(0.8 * self.LEVEL <= lo and hi <= 1.1 * self.LEVEL,
                        f"{what}: playback ranges {lo:.3f}..{hi:.3f} over a pass of the "
                        f"{L}-sample loop (take {self.take}); expected a steady "
                        f"{self.LEVEL} (or 0.96 x it).  A low minimum is the stock "
                        "silent stretch at the loop's end")


class TestMultiplyEnd(MultiplyEndTestCase):

    def testRoundOn_NoSilentEnd(self):
        """Round on: the loop rounds up to whole takes, no silent stretch."""
        self.looper.set_port(test_engine.Round, 1.0)
        self.record_take()
        self.multiply()
        self.assertState("Playing")
        self.assertEqual(self.loop_len() % self.take, 0,
                         f"loop {self.loop_len()} is not whole takes of {self.take}")
        self.assertSteadyPass("Round on")

    def testRoundOff_NoSilentEnd(self):
        """Round off: also no silent stretch."""
        self.looper.set_port(test_engine.Round, 0.0)
        self.record_take()
        self.multiply()
        self.assertState("Playing")
        self.assertSteadyPass("Round off")

    def testEndedByOverdub_NoSilentEnd(self):
        """A multiply ended by Overdub goes on into Overdub; stop that and
        the loop still has no silent stretch."""
        self.looper.set_port(test_engine.Round, 1.0)
        self.record_take()
        self.multiply(end_with="OVERDUB")
        self.assertState("Overdubbing")
        self.engine.request("OVERDUB")
        self.wait_buffers(3)
        self.assertState("Playing")
        self.assertSteadyPass("ended by Overdub")

    def testUndoRedo_AfterMultiply(self):
        """Undo after a multiply gives back the take, and Redo the
        multiplied loop, still with no silent stretch."""
        self.looper.set_port(test_engine.Round, 1.0)
        self.record_take()
        self.multiply()
        self.assertState("Playing")
        multiplied = self.loop_len()
        self.engine.request("UNDO")
        self.wait_buffers(3)
        self.assertState("Playing")
        self.assertEqual(self.loop_len(), self.take)
        self.assertSteadyPass("after Undo")
        self.engine.request("REDO")
        self.wait_buffers(3)
        self.assertState("Playing")
        self.assertEqual(self.loop_len(), multiplied)
        self.assertSteadyPass("after Redo")


class TestLateEndMidCycle(MultiplyEndTestCase):
    """A multiply end just after a sync pulse inside a cycle is not a late
    hit: only a pulse at a cycle start added a cycle.  With Eighths/Cycle
    shorter than the loop's cycle, pulses come several times per cycle,
    and the fork used to take a cycle back after any of them."""

    WINDOW_BUFS = 4

    def setUp(self):
        super().setUp()
        test_engine.cvar.g_late_hit_window_ms = (self.WINDOW_BUFS * self.buf / self.sr) * 1000.0

    def tearDown(self):
        test_engine.cvar.g_late_hit_window_ms = 100.0
        super().tearDown()

    def testRoundOn_EndAfterMidCyclePulse_Waits(self):
        """Round on: the end waits for the cycle's end, as with no pulse."""
        self.looper.set_port(test_engine.Round, 1.0)
        self.record_take()
        tb = self.take // self.buf + 1
        self.engine.request("MULTIPLY")
        self.wait_buffers(2)
        self.looper.sync_inject_value = 2.0       # start
        self.wait_buffers(2)
        self.looper.sync_inject_value = 0.0
        self.wait_buffers(2)
        self.assertState("Multiplying")
        self.wait_buffers(tb + tb // 2)           # mid second cycle
        self.looper.sync_inject_value = 2.0       # a pulse inside the cycle
        self.wait_buffers(2)
        self.looper.sync_inject_value = 0.0
        self.wait_buffers(1)                      # well inside the window
        self.engine.request("MULTIPLY")
        self.wait_buffers(3)
        self.assertState("Multiplying")           # waiting, not trimmed to one take
        self.wait_buffers(2 * tb)
        self.assertState("Playing")
        L = self.loop_len()
        self.assertTrue(L % self.take == 0 and L >= 2 * self.take,
                        f"loop {L}, take {self.take}: expected whole takes, at least two")
        self.assertSteadyPass("late end after a mid-cycle pulse")


if __name__ == "__main__":
    unittest.main()
