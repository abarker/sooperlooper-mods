#! /usr/bin/env python3
"""
Tests for REL_SYNC_POS_FIX (plugin.cc): a Rel Sync recording sets lSyncPos
(where the sync grid falls in the loop) from the count since the last
boundary at its start.  With a tempo set, stock took it half a Quantize
unit off for a start in the unit's second half, and wrapped it to about
2^32 for one in the second quarter; a synced restart then played about
half a cycle out of phase.  Fixed: from the nearer boundary, signed.

The tempo makes an eighth exactly 1001 samples and the cycle 8 eighths,
8008 samples, the pulse spacing, so the tempo-derived unit is exact.
Presses go in at exact frames; the input is a ramp (as in
testdef_rel_sync_late_hit.py), so playback phase is checked exactly.
"""

import unittest
import test_engine
from testdef_rel_sync_late_hit import RelSyncLateHitTestCase, frame_of, RAMP

CYCLE = 8008
EIGHTH = CYCLE // 8


class RelSyncPosTestCase(RelSyncLateHitTestCase):

    TEMPO = True

    def setUp(self):
        super().setUp()
        if self.TEMPO:
            self.looper.set_port(test_engine.EighthPerCycleLoop, 8.0)
            self.looper.set_port(test_engine.TempoInput, self.sr * 30.0 / (EIGHTH + 0.5))
        self.looper.set_port(test_engine.MuteQuantized, 1.0)
        self.looper.sync_inject_period = 0
        self.cycle = CYCLE
        self.looper.sync_clock = 0
        self.looper.sync_inject_period = CYCLE
        self.wait_cycles(3)

    def record_at_phase(self, lo, hi, cycles=2):
        """Record from the next block start lo to hi samples after a
        pulse, `cycles` cycles long.  Returns the press's phase."""
        f = (self.looper.sync_clock // self.buf + 4) * self.buf
        while not lo <= f % CYCLE < hi:
            f += self.buf
        s0 = self.press_at("RECORD", f)
        self.assertEqual(self.state(), "Recording")
        self.press_at("RECORD", s0 + cycles * CYCLE - 6 * self.buf)
        self.wait_state("Playing")
        self.assertEqual(self.length(), cycles * CYCLE)
        return s0 % CYCLE

    def playback_offsets(self):
        """Phase of playback against the recording, mod the cycle: 0 when
        each frame plays the input from whole cycles earlier."""
        L = self.length()
        start = (self.loop_frame(64) - 64) % RAMP
        offs = set()
        for _ in range(30):
            self.wait_buffers(1)
            fr = self.looper.out_first_frame
            v = self.looper.out_first
            if self.looper.out_first_frame != fr:
                continue                  # raced the callback
            if (fr - start) % L < 64:
                continue                  # the seam
            offs.add(((fr - frame_of(v)) % RAMP) % CYCLE)
        self.assertGreater(len(offs), 0)
        return offs

    def assertSyncPos(self, phase, ch=0):
        """lSyncPos is the count since the boundary (which reads one less
        than the press's phase) from the nearer boundary."""
        since = phase - 1
        expected = since if since < CYCLE // 2 else since - CYCLE
        self.assertEqual(self.looper.get_channel_sync_pos(ch), expected)


# Each quarter of the cycle: stock was right only in the first.
QUARTERS = [(0, CYCLE // 4), (CYCLE // 4, CYCLE // 2),
            (CYCLE // 2, 3 * CYCLE // 4), (3 * CYCLE // 4, CYCLE)]


class TestRelSyncPos(RelSyncPosTestCase):

    def restartInPhase(self, quarter, command):
        phase = self.record_at_phase(*QUARTERS[quarter])
        self.assertSyncPos(phase)
        self.assertEqual(self.playback_offsets(), {0})
        self.engine.request("MUTE")
        self.wait_state("Muted")
        self.wait_cycles(1.3)
        self.engine.request(command)
        self.wait_state("Playing", units=2)
        # Within the Rel Sync grid's one sample (stock, the count reads
        # one less than the distance).
        offs = self.playback_offsets()
        self.assertTrue(offs <= {0, CYCLE - 1},
                        f"restart out of phase by {sorted(offs)} samples")

    def testQuarter1_MuteTrigger(self):
        self.restartInPhase(0, "MUTE_TRIGGER")

    def testQuarter2_MuteTrigger(self):
        self.restartInPhase(1, "MUTE_TRIGGER")

    def testQuarter3_MuteTrigger(self):
        self.restartInPhase(2, "MUTE_TRIGGER")

    def testQuarter4_MuteTrigger(self):
        self.restartInPhase(3, "MUTE_TRIGGER")

    def testQuarter2_Trigger(self):
        self.restartInPhase(1, "TRIGGER")

    def testQuarter3_Trigger(self):
        self.restartInPhase(2, "TRIGGER")

    def ownBoundaries(self, quarter):
        phase = self.record_at_phase(*QUARTERS[quarter], cycles=1)
        self.looper.set_port(test_engine.Sync, 0.0)
        self.wait_buffers(2)
        n0 = self.looper.syncout_count
        self.wait_cycles(3)
        n1 = self.looper.syncout_count
        delta = self.looper.input_minus_sync
        return {(self.looper.get_syncout_frame(k) - delta) % CYCLE for k in range(n0, n1)}

    def testOwnBoundariesOnBarLines_EachQuarter(self):
        for q in range(4):
            with self.subTest(quarter=q):
                self.assertEqual(self.ownBoundaries(q), {1},
                                 "with Sync off the loop's boundaries should stay on the"
                                 " bar lines (the Rel Sync grid's one sample late)")
                self.engine.request("UNDO_ALL")
                self.looper.set_port(test_engine.Sync, 2.0)
                self.wait_cycles(2)


class TestRelSyncPosNoTempo(RelSyncPosTestCase):
    """No tempo: stock was already right (always the count since the
    boundary); unchanged."""

    TEMPO = False

    def testSyncPosIsSince_EachQuarter(self):
        for q in range(4):
            with self.subTest(quarter=q):
                phase = self.record_at_phase(*QUARTERS[q], cycles=1)
                self.assertEqual(self.looper.get_channel_sync_pos(0), phase - 1)
                self.engine.request("UNDO_ALL")
                self.wait_cycles(1)


class TestRelSyncPosStereo(RelSyncPosTestCase):

    CHANNELS = 2

    def testChannelsAlike(self):
        phase = self.record_at_phase(*QUARTERS[2])
        self.assertSyncPos(phase, ch=0)
        self.assertSyncPos(phase, ch=1)


if __name__ == "__main__":
    unittest.main()
