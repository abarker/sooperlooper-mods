#! /usr/bin/env python3
"""
Tests for SIGNED_SYNC_POS_FIX (plugin.cc): with Sync off, a loop makes its
own boundaries (its sync output) where (pos + lSyncPos) is a whole number
of units.  lSyncPos can be negative; stock converted the sum to unsigned
for a Cycle or Loop unit, so one position near the loop's start also
tested as a boundary: where -lSyncPos - pos == 2^64 mod unit.

A Rel Sync early start pressed just after a pulse gives a negative
lSyncPos (the count since the pulse, less the early start).  The cycle is
8008 samples, whose remainder 2^64 mod 8008 is only 16, so any lSyncPos
below -16 shows the extra boundary.  The sync output is read back
(TestLooper.syncout_count / get_syncout_frame).
"""

import unittest
import test_engine
from testdef_rel_sync_late_hit import RelSyncLateHitTestCase

CYCLE = 8008            # 2^64 mod 8008 == 16


class SignedSyncPosTestCase(RelSyncLateHitTestCase):

    TRIG_LATENCY = 500

    def setUp(self):
        super().setUp()
        self.assertEqual(2 ** 64 % CYCLE, 16)
        self.looper.sync_inject_period = 0
        self.cycle = CYCLE
        self.looper.sync_clock = 0
        self.looper.sync_inject_period = CYCLE
        self.wait_cycles(3)

    def press_after_pulse(self, since_min, since_max):
        """Press Record on the next block start that is since_min to
        since_max samples after a pulse; return its frame."""
        f = (self.looper.sync_clock // self.buf + 4) * self.buf
        while not since_min <= f % CYCLE < since_max:
            f += self.buf
        s0 = self.press_at("RECORD", f)
        self.assertEqual(s0 % CYCLE, f % CYCLE, "the press landed in another block")
        self.assertEqual(self.state(), "Recording")
        return s0

    def own_boundary_phases(self, cycles=4):
        """Turn Sync off and return the phases (relative to the pulses)
        of the loop's own boundaries over `cycles` cycles."""
        self.looper.set_port(test_engine.Sync, 0.0)
        self.wait_buffers(2)
        n0 = self.looper.syncout_count
        self.wait_cycles(cycles)
        n1 = self.looper.syncout_count
        delta = self.looper.input_minus_sync
        return [(self.looper.get_syncout_frame(k) - delta) % CYCLE
                for k in range(n0, n1)]


class TestSignedSyncPos(SignedSyncPosTestCase):

    def testNegativeSyncPos_OneBoundaryPerCycle(self):
        # Since the pulse < early start (500): lSyncPos negative, by more
        # than 16.
        s0 = self.press_after_pulse(0, self.early - 100)
        self.press_at("RECORD", s0 + CYCLE - 6 * self.buf)
        self.wait_state("Playing")
        self.assertEqual(self.length(), CYCLE)
        phases = self.own_boundary_phases()
        self.assertGreaterEqual(len(phases), 3)
        self.assertEqual(len(set(phases)), 1,
                         f"one boundary per cycle expected, at one phase: {phases}")
        self.assertLessEqual(len(phases), 5)

    def testPositiveSyncPos_Unchanged(self):
        # Pressed well after the pulse: lSyncPos positive, as before.
        s0 = self.press_after_pulse(self.early + 100, CYCLE // 2)
        self.press_at("RECORD", s0 + CYCLE - 6 * self.buf)
        self.wait_state("Playing")
        phases = self.own_boundary_phases()
        self.assertGreaterEqual(len(phases), 3)
        self.assertEqual(len(set(phases)), 1, f"{phases}")


if __name__ == "__main__":
    unittest.main()
