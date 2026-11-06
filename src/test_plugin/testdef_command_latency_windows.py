#! /usr/bin/env python3
"""
Tests for TRIGGER_LATENCY_ALL_EDGES increment (e) (plugin.hpp): window
decisions are taken from the press, trigger_latency before the command's
arrival, not from the arrival.  Every window, counted from the boundary
to the arrival, is trigger_latency longer: the late-hit windows
(lateHitWindowWanted(): Record's late start and stops, Multiply's late
end, and the separate mute window) and the Trigger / Oneshot slack window
(inSlackWindow()).  Placement still counts to the arrival, so a restart
accepted late in the slack window plays in phase.

The exact tempo and pulses of testdef_rel_sync_pos.py (eighth 1001,
cycle 8008), plain Sync, presses at exact block starts, late-hit window
40 ms (1920 samples), trigger_latency 600.  Late start: Record pressed
past the window from its arrival.  Slack window: a Trigger from Mute,
the take recorded at trigger_latency 0, then the latency set.
Skipped when the switch is off.
"""

import unittest
import test_engine
from testdef_sync_count_off_by_one import SyncCountTestCase, CYCLE, EIGHTH

ENABLED = bool(getattr(test_engine, "TRIGGER_LATENCY_ALL_EDGES", 0))


class WindowsTestCase(SyncCountTestCase):

    SYNC = 1.0
    TL = 600

    def tearDown(self):
        self.looper.set_port(test_engine.TriggerLatency, 0.0)
        super().tearDown()

    def block_after_pulse(self, lo, hi):
        """A block start lo .. hi - 1 samples after a pulse at least two
        cycles on."""
        k = self.looper.sync_clock // CYCLE + 2
        while True:
            f = -(-(k * CYCLE + lo) // self.buf) * self.buf
            if f - k * CYCLE < hi:
                return f
            k += 1

    def record_at(self, lo, hi):
        """Record (plain Sync) pressed lo .. hi after a pulse; True if it
        started at once (late start), False if it waits."""
        self.looper.set_port(test_engine.TriggerLatency, float(self.TL))
        self.wait_buffers(2)
        self.press_at("RECORD", self.block_after_pulse(lo, hi))
        st = self.state()
        self.assertIn(st, ("Recording", "WaitStart"))
        return st == "Recording"

    def restart_at(self, lo, hi):
        self.record()                          # at trigger_latency 0
        self.looper.set_port(test_engine.TriggerLatency, float(self.TL))
        self.engine.request("MUTE")
        self.wait_state("Muted")
        self.press_at("TRIGGER", self.block_after_pulse(lo, hi))
        return self.state() == "Playing"


class WindowsTests:
    """The cases."""

    def testLateStart_PastTheWindow_InsideFromThePress(self):
        # Arrives window + 180 .. + 580 after the pulse: the press was
        # within the window (trigger_latency 600 earlier).
        w = self.window              # late-hit window, samples (base class)
        self.assertTrue(self.record_at(w + 180, w + 580),
                        "a late Record inside the window (from the press) should start at once")

    def testLateStart_PastTheWindowFromThePressToo(self):
        w = self.window              # late-hit window, samples (base class)
        self.assertFalse(self.record_at(w + self.TL + 100, w + self.TL + 500),
                         "a Record outside the window (from the press) should wait")

    def testPastTheEighth_InsideFromThePress(self):
        # Arrives 1001 .. 1600 after the pulse: the press was less than an
        # eighth after it.  Acts at once, in phase.
        self.assertTrue(self.restart_at(EIGHTH, EIGHTH + self.TL),
                        "a Trigger inside the slack window (from the press) should act at once")
        self.assertEqual(self.playback_offsets(), {0}, "out of phase")

    def testRestartPastTheWindowFromThePressToo(self):
        # Arrives at least an eighth plus trigger_latency after the pulse:
        # waits for the next one, then plays in phase.
        self.assertFalse(self.restart_at(EIGHTH + self.TL + 1, EIGHTH + self.TL + 600),
                         "a Trigger outside the window should wait for the pulse")
        self.wait_state("Playing", units=2)
        self.assertEqual(self.playback_offsets(), {0}, "out of phase")


@unittest.skipUnless(ENABLED, "TRIGGER_LATENCY_ALL_EDGES is 0")
class TestWindowsFromThePress(WindowsTests, WindowsTestCase):
    pass


if __name__ == "__main__":
    unittest.main()
