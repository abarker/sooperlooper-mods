#! /usr/bin/env python3
"""
Tests for ONESHOT_ONE_PASS_FIX (plugin.hpp; stock bug, ROADMAP entry 43):
a oneshot plays the loop once, one loop length from its start, and ends
with the loop's pass.  Stock ended it when the position reached the end
of the buffer, so it played lSyncPos samples when that was positive (part
of a pass: a Rel Sync take started late in the cycle), and the fork's
RESTART_POSITION_FIX, which starts it IL + OL further in, ended it on its
first sample when lSyncPos was 1 .. IL + OL.

Rel Sync, the exact tempo of testdef_rel_sync_pos.py (cycle 8008), rig
latencies (IL = OL = 743), takes two cycles long started at different
points in the cycle, so lSyncPos is negative, small positive (under
IL + OL), and large positive.  The output's first and last nonzero
samples give the pass's length exactly (the harness's crossfade is one
sample).  A oneshot that waits for the pulse plays the whole loop; one
pressed in the slack window plays the loop less the samples since the
pulse, as its start skips them.
"""

import unittest
import test_engine
from testdef_rel_sync_pos import RelSyncPosTestCase, CYCLE

ENABLED = bool(getattr(test_engine, "ONESHOT_ONE_PASS_FIX", 0))
IL = OL = 743


@unittest.skipUnless(ENABLED, "ONESHOT_ONE_PASS_FIX is 0")
class OneshotOnePassTestCase(RelSyncPosTestCase):

    def setUp(self):
        super().setUp()
        self.looper.set_port(test_engine.InputLatency, float(IL))
        self.looper.set_port(test_engine.OutputLatency, float(OL))
        self.looper.set_port(test_engine.TriggerLatency, float(IL))
        self.wait_buffers(4)

    def tearDown(self):
        self.looper.set_port(test_engine.InputLatency, 0.0)
        self.looper.set_port(test_engine.OutputLatency, 0.0)
        self.looper.set_port(test_engine.TriggerLatency, 0.0)
        super().tearDown()

    def press_frame(self, lo, hi):
        """A block start lo .. hi - 1 samples after a pulse at least two
        cycles on."""
        k = self.looper.sync_clock // CYCLE + 2
        while True:
            f = -(-(k * CYCLE + lo) // self.buf) * self.buf
            if f - k * CYCLE < hi:
                return f, k * CYCLE
            k += 1

    def oneshot_span(self, phase, lo, hi):
        """Record at `phase` (a (lo, hi) pair), mute, press Oneshot lo .. hi
        samples after a pulse; return (lSyncPos, press - pulse, span),
        span = the number of samples from the first nonzero output sample
        to the last, inclusive."""
        self.record_at_phase(*phase)
        sync_pos = self.looper.get_channel_sync_pos(0)
        self.engine.request("MUTE")
        self.wait_state("Muted")
        self.wait_buffers(4)
        self.looper.reset_output_stats()
        f, pulse = self.press_frame(lo, hi)
        got = self.press_at("ONESHOT", f)
        self.assertEqual(got, f)
        self.wait_buffers(int(2.5 * self.length() / self.buf) + 4)
        self.assertEqual(self.state(), "Muted", "the oneshot did not end")
        a, b = self.looper.output_nonzero_first, self.looper.output_nonzero_last
        self.assertGreaterEqual(a, 0, "the oneshot played nothing")
        return sync_pos, f - pulse, b - a + 1

    def waits(self, phase, sync_pos_check):
        sync_pos, _, span = self.oneshot_span(phase, 3000, 4000)
        self.assertTrue(sync_pos_check(sync_pos), f"lSyncPos {sync_pos} not as intended")
        self.assertEqual(span, self.length(),
                         f"lSyncPos {sync_pos}: played {span} samples, not one loop")

    # lSyncPos from record_at_phase() at IL = OL = 743 (measured over the
    # whole cycle): about the take's phase less 870 up to half a cycle,
    # negative after it.  The phase ranges keep a margin from each sign
    # change, wherever in the cycle the test starts.
    def testWaits_SyncPosNegative(self):
        self.waits((0, 300), lambda p: p < 0)

    def testWaits_SyncPosUnderLatency(self):
        # Stock + RESTART_POSITION_FIX: nothing played.
        self.waits((1100, 1400), lambda p: 0 < p < IL + OL)

    def testWaits_SyncPosLargePositive(self):
        self.waits((3000, 3800), lambda p: p > IL + OL)

    def testWaits_SecondHalf(self):
        self.waits((4400, 7600), lambda p: p < 0)

    def testSlack_SyncPosUnderLatency(self):
        sync_pos, late, span = self.oneshot_span((1100, 1400), 100, 500)
        self.assertTrue(0 < sync_pos < IL + OL, f"lSyncPos {sync_pos} not as intended")
        self.assertEqual(span, self.length() - late,
                         f"pressed {late} after the pulse: played {span} samples")

    def testSlack_SyncPosNegative(self):
        sync_pos, late, span = self.oneshot_span((4400, 7600), 100, 500)
        self.assertLess(sync_pos, 0, f"lSyncPos {sync_pos} not as intended")
        self.assertEqual(span, self.length() - late,
                         f"pressed {late} after the pulse: played {span} samples")


if __name__ == "__main__":
    unittest.main()
