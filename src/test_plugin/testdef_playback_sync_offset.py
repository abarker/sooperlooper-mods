#! /usr/bin/env python3
"""
Regression tests for PLAYBACK_SYNC_OFFSET_FIX (stock bug).

With playback_sync on, a playing loop re-locks to a sync pulse once a pass.
Stock set its position to the sync point with no input plus output latency,
while normal play runs IL + OL ahead, so from the first re-lock the loop
played IL + OL late.  Its window (half a cycle here) was measured around
the sync point itself, so with IL + OL larger than that it fired at another
pulse (by the code, a cycle away; measured: the same phase as IL + OL late,
modulo the cycle).  With the fix the target is IL + OL past the sync point
and the window is measured around it: a re-lock keeps the phase normal play
has, and a loop follows pulses that move.  With the switch at 0 every test
here fails but the control (playback sync off).

Uses testdef_rel_sync_pos.py's exact tempo (eighth 1001, cycle 8008) and its
ramp-input phase check (playback_offsets(): the phase of playback against
the recording, mod the cycle).  The harness's latencies are normally 0,
which would hide the bug, so these tests set them.  Each test reads the
phase after the take, before playback sync is on, and compares with that.
"""

import unittest
import test_engine
from testdef_rel_sync_pos import CYCLE, QUARTERS
from testdef_sync_count_off_by_one import SyncCountTestCase

SMALL = (1000, 1500)     # IL + OL 2500: inside the window (4004)
LARGE = (3000, 4000)     # IL + OL 7000: outside it
SHIFT = 200              # samples the pulse grid moves earlier
PASS = 2 * CYCLE         # the takes here are two cycles


class PlaybackSyncTestCase(SyncCountTestCase):

    SYNC = 1.0
    LATENCY = SMALL
    PREROLL_MS = 0.0

    def setUp(self):
        test_engine.cvar.g_sync_preroll_ms = self.PREROLL_MS
        super().setUp()
        self.looper.set_port(test_engine.InputLatency, float(self.LATENCY[0]))
        self.looper.set_port(test_engine.OutputLatency, float(self.LATENCY[1]))
        self.looper.set_port(test_engine.PlaybackSync, 0.0)
        self.wait_cycles(1)

    def tearDown(self):
        test_engine.cvar.g_sync_preroll_ms = 0.0
        super().tearDown()

    def take(self):
        """A two-cycle take (plain Sync: on the pulses; Rel Sync: from the
        press), then its playback phase, which every check compares with."""
        if self.SYNC == 1.0:
            pulse = (self.looper.sync_clock // CYCLE + 2) * CYCLE
            self.press_at("RECORD", pulse - 6 * self.buf)
            self.wait_state("Recording")
            self.press_at("RECORD", pulse + 2 * CYCLE - 6 * self.buf)
            self.wait_state("Playing")
        else:
            self.record_at_phase(*QUARTERS[2])
        self.assertEqual(self.length(), PASS)
        self.wait_cycles(1)
        base = self.playback_offsets()
        self.assertEqual(len(base), 1, f"playback not one steady phase: {sorted(base)}")
        return base.pop()

    def passes(self, n):
        self.wait_cycles(n * PASS / CYCLE)

    def shift_grid(self, d):
        """Move the injected pulses about d samples earlier (the input
        clock, and so the recorded phase, does not move); return how far
        they really moved.  The callback advances sync_clock too, so a
        write from here can race it and land a block off: the real shift
        is read back from input_minus_sync (input_clock - sync_clock)."""
        self.wait_buffers(1)
        old = self.looper.input_minus_sync
        self.looper.sync_clock += d
        self.wait_buffers(2)
        moved = old - self.looper.input_minus_sync
        self.assertNotEqual(moved % CYCLE, 0, "the sync grid did not move")
        return moved

    def assertPhase(self, expected, msg):
        offs = self.playback_offsets()
        self.assertEqual(offs, {expected % CYCLE},
                         f"{msg}: playback phase {sorted(offs)}, expected {expected % CYCLE}")


class TestPlaybackSyncOffset(PlaybackSyncTestCase):

    def testRelock_KeepsThePhase(self):
        base = self.take()
        self.looper.set_port(test_engine.PlaybackSync, 1.0)
        self.passes(3)
        self.assertPhase(base, "after re-locks")

    def testRelock_FollowsMovedPulses(self):
        base = self.take()
        self.looper.set_port(test_engine.PlaybackSync, 1.0)
        self.passes(2)
        moved = self.shift_grid(SHIFT)
        self.passes(3)
        self.assertPhase(base - moved, "pulses moved earlier, loop should follow")

    def testNoPlaybackSync_DoesNotFollow(self):
        # Control: without playback sync the loop keeps its own phase.
        base = self.take()
        moved = self.shift_grid(SHIFT)
        self.passes(3)
        self.assertPhase(base, "playback sync off")

    def testMuted_Relocks_UnmutesInTime(self):
        base = self.take()
        self.looper.set_port(test_engine.PlaybackSync, 1.0)
        self.engine.request("MUTE")
        self.wait_state("Muted", units=2)
        moved = self.shift_grid(SHIFT)
        self.passes(3)
        self.engine.request("MUTE")
        self.wait_state("Playing", units=2)
        self.assertPhase(base - moved, "re-locked while muted")

    def testRestart_ThenRelock_NoJump(self):
        # A restart (RESTART_POSITION_FIX: IL + OL in) and the re-lock
        # agree, so the first re-lock after a restart moves nothing.
        base = self.take()
        self.looper.set_port(test_engine.PlaybackSync, 1.0)
        self.passes(1)
        self.engine.request("MUTE")
        self.wait_state("Muted")
        self.press_at("MUTE_TRIGGER", self.press_frame(3 * 1001))    # waits for the pulse
        self.wait_state("Playing", units=2)
        self.assertPhase(base, "right after the restart")
        self.passes(3)
        self.assertPhase(base, "re-locks after the restart")


class TestPlaybackSyncOffsetLargeLatency(PlaybackSyncTestCase):
    """IL + OL larger than the sync window (stock re-locked at another
    pulse)."""

    LATENCY = LARGE

    def testRelock_KeepsThePhase(self):
        base = self.take()
        self.looper.set_port(test_engine.PlaybackSync, 1.0)
        self.passes(3)
        self.assertPhase(base, "after re-locks")

    def testRelock_FollowsMovedPulses(self):
        base = self.take()
        self.looper.set_port(test_engine.PlaybackSync, 1.0)
        self.passes(2)
        moved = self.shift_grid(SHIFT)
        self.passes(3)
        self.assertPhase(base - moved, "pulses moved earlier, loop should follow")


class TestPlaybackSyncOffsetPreroll(PlaybackSyncTestCase):
    """With the sync pre-roll (its sync point is the pre-roll before the
    loop's top: lSyncPos = -P)."""

    PREROLL_MS = 10.0

    def testRelock_KeepsThePhase(self):
        base = self.take()
        self.looper.set_port(test_engine.PlaybackSync, 1.0)
        self.passes(3)
        self.assertPhase(base, "after re-locks, pre-roll on")


class TestPlaybackSyncOffsetRelSync(PlaybackSyncTestCase):

    SYNC = 2.0

    def testRelock_KeepsThePhase(self):
        base = self.take()
        self.looper.set_port(test_engine.PlaybackSync, 1.0)
        self.passes(3)
        self.assertPhase(base, "Rel Sync loop after re-locks")

    def testRelock_FollowsMovedPulses(self):
        base = self.take()
        self.looper.set_port(test_engine.PlaybackSync, 1.0)
        self.passes(2)
        moved = self.shift_grid(SHIFT)
        self.passes(3)
        self.assertPhase(base - moved, "pulses moved earlier, loop should follow")


if __name__ == '__main__':
    unittest.main()
