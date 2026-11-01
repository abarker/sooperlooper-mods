#! /usr/bin/env python3
"""
Tests for SYNC_PREROLL (plugin.cc): with sync_preroll_ms P set, a plain
Sync recording that starts on a pulse starts P before it, from the input
history, and every end comes P before its pulse, so the loop is still
whole cycles and plays in phase with the pulses.  Both starts (waiting for
the pulse, and the late-hit start just after it) and every end (on the
pulse, Record's late stop, the late stop for other commands) are covered;
a start not on a pulse (Quantize Off) and a P the input history cannot
hold get no pre-roll.

Same exact-frame presses and ramp input as testdef_rel_sync_late_hit.py.
"""

import unittest
import test_engine
from testdef_rel_sync_late_hit import RAMP, MARGIN
from testdef_late_stop_any_command import LateStopAnyCommandTestCase

PREROLL_MS = 10.0       # 480 samples at 48 kHz


class SyncPrerollTestCase(LateStopAnyCommandTestCase):

    PREROLL_MS = PREROLL_MS

    def setUp(self):
        super().setUp()
        test_engine.cvar.g_sync_preroll_ms = self.PREROLL_MS
        self.P = int(self.PREROLL_MS * 0.001 * self.sr)

    def tearDown(self):
        test_engine.cvar.g_sync_preroll_ms = 0.0
        super().tearDown()

    def next_pulse(self):
        return (self.looper.sync_clock // self.cycle + 2) * self.cycle

    def start_waiting(self):
        """Press Record before a pulse, so it waits and starts on the
        pulse.  Returns the pulse's frame."""
        pulse = self.next_pulse()
        self.press_at("RECORD", pulse - 6 * self.buf)
        self.assertEqual(self.state(), "WaitStart")
        self.wait_state("Recording")
        self.wait_buffers(2)
        return pulse

    def start_late(self):
        """Press Record just after a pulse (a late-hit start).  Returns
        the pulse's frame."""
        pulse = self.next_pulse()
        self.press_at("RECORD", pulse + MARGIN)
        self.assertEqual(self.state(), "Recording")
        return pulse

    def end_early(self, pulse, cycles=2, command="RECORD", state="Playing"):
        """Press `command` before the pulse `cycles` after `pulse`, so the
        take waits and ends there."""
        self.press_at(command, pulse + cycles * self.cycle - 6 * self.buf)
        self.assertEqual(self.state(), "WaitStop")
        self.wait_state(state)

    def end_late(self, pulse, cycles=2, command="RECORD", state="Playing"):
        """Press `command` just after the pulse `cycles` after `pulse`:
        the late stop ends the take there at once."""
        self.press_at(command, pulse + cycles * self.cycle + MARGIN)
        self.assertEqual(self.state(), state, f"a late {command} should end the take at once")

    def loop_start(self, ch=0):
        """The frame (sync clock) of the input at loop position 0."""
        return (self.loop_frame(64, ch) - 64 - self.looper.input_minus_sync) % RAMP

    def assertPreRolled(self, pulse, length, P=None, ch=0):
        P = self.P if P is None else P
        self.assertEqual(self.length(), length)
        self.assertEqual(self.loop_start(ch), (pulse - P) % RAMP,
                         f"the loop starts {(pulse - self.loop_start(ch)) % RAMP}"
                         f" samples before the pulse, not {P}")
        self.assertContiguous(length, ch)


class TestSyncPreroll(SyncPrerollTestCase):

    def testWaitingStart_EndOnPulse(self):
        pulse = self.start_waiting()
        self.end_early(pulse)
        self.assertPreRolled(pulse, 2 * self.cycle)

    def testWaitingStart_PlaysInPhase(self):
        pulse = self.start_waiting()
        self.end_early(pulse)
        self.assertPlaysInPhase(2 * self.cycle)

    def testLateHitStart_EndOnPulse(self):
        pulse = self.start_late()
        self.end_early(pulse)
        self.assertPreRolled(pulse, 2 * self.cycle)
        self.assertPlaysInPhase(2 * self.cycle)

    def testRecordLateStop(self):
        pulse = self.start_waiting()
        self.end_late(pulse)
        self.assertPreRolled(pulse, 2 * self.cycle)
        self.assertPlaysInPhase(2 * self.cycle)

    def testLateHitStart_RecordLateStop(self):
        pulse = self.start_late()
        self.end_late(pulse)
        self.assertPreRolled(pulse, 2 * self.cycle)
        self.assertPlaysInPhase(2 * self.cycle)

    def testMuteTriggerLateStop(self):
        pulse = self.start_waiting()
        self.end_late(pulse, command="MUTE_TRIGGER", state="Muted")
        self.assertPreRolled(pulse, 2 * self.cycle)

    def testMuteTriggerOnPulse_ThenUnmute_PlaysInPhase(self):
        pulse = self.start_waiting()
        self.end_early(pulse, command="MUTE_TRIGGER", state="Muted")
        self.assertPreRolled(pulse, 2 * self.cycle)
        self.wait_cycles(1.3)
        self.engine.request("MUTE")
        self.wait_state("Playing")
        self.assertPlaysInPhase(2 * self.cycle)

    def testOverdubLateStop(self):
        pulse = self.start_waiting()
        self.end_late(pulse, command="OVERDUB", state="Overdubbing")
        self.assertEqual(self.length(), 2 * self.cycle)
        # (no content check: the overdub is adding to the loop)

    def testOneCycle(self):
        pulse = self.start_waiting()
        self.end_early(pulse, cycles=1)
        self.assertPreRolled(pulse, self.cycle)
        self.assertPlaysInPhase(self.cycle)

    def testTriggerAfterMute_PlaysOnBarGrid(self):
        # A synced Trigger restarts the loop from the top at the next
        # pulse, which may be either bar of a 2-cycle loop: the phase holds
        # modulo the cycle, not the loop.  Exact to the sample with a
        # tempo set too (SYNC_COUNT_OFF_BY_ONE_FIX).
        pulse = self.start_waiting()
        self.end_early(pulse)
        self.engine.request("MUTE")
        self.wait_state("Muted")
        self.wait_cycles(0.7)
        self.engine.request("TRIGGER")
        self.wait_state("Playing")
        self.assertPlaysOnGrid(self.cycle)

    def assertPlaysOnGrid(self, unit, slack=0):
        """Each sample played at frame F was recorded a whole number of
        `unit`s (plus at most `slack` samples) before F."""
        start = (self.loop_frame(64) - 64) % RAMP
        checked = 0
        for _ in range(40):
            self.wait_buffers(1)
            fr = self.looper.out_first_frame
            val = self.looper.out_first
            if self.looper.out_first_frame != fr:
                continue                  # raced the callback
            if (fr - start) % unit < 64:
                continue                  # the seam
            diff = (fr - int(round(val * RAMP))) % RAMP
            self.assertLessEqual(diff % unit, slack,
                                 f"frame {fr} plays input from {diff} frames earlier,"
                                 f" not a whole number of cycles ({unit})")
            checked += 1
        self.assertGreater(checked, 10)

    def testSyncOffAfterwards_PlaysOn(self):
        # The rig turns Sync off at transport stop: the loop plays on.
        pulse = self.start_waiting()
        self.end_early(pulse)
        self.looper.set_port(test_engine.Sync, 0.0)
        self.wait_cycles(1)
        self.assertEqual(self.state(), "Playing")
        self.assertPlaysInPhase(2 * self.cycle)


class TestSyncPrerollWithTempo(TestSyncPreroll):
    """The same with a tempo set, as on a rig (JACK transport or MIDI
    clock): the loop's cycle comes from the tempo, 4 eighths of 2123."""

    def setUp(self):
        super().setUp()
        self.assertEqual(self.cycle % 4, 0)
        self.looper.set_port(test_engine.EighthPerCycleLoop, 4.0)
        self.looper.set_port(test_engine.TempoInput,
                             self.sr * 30.0 / (self.cycle / 4 + 0.25))
        self.wait_cycles(1)


class TestSyncPrerollMultiply(SyncPrerollTestCase):
    """The fork's Multiply late end checks that the pulse it trims to is
    where a cycle began; a pre-rolled loop's cycles begin P before the
    pulses.  P here is more than half the late-hit window, the check's
    tolerance, so the check must allow for it."""

    PREROLL_MS = 25.0       # 1200 samples; half the window is 960

    def multiply(self, end):
        pulse = self.start_waiting()
        self.end_early(pulse, cycles=1)
        self.assertPreRolled(pulse, self.cycle)
        p2 = self.next_pulse()
        self.press_at("MULTIPLY", p2 - 6 * self.buf)
        self.wait_state("Multiplying")
        return p2 + 2 * self.cycle

    def testLateEnd_EndsAtOnce(self):
        end = self.multiply("late")
        self.press_at("MULTIPLY", end + MARGIN)
        self.assertEqual(self.state(), "Playing", "a late multiply end should end at once")
        self.assertEqual(self.length(), 2 * self.cycle)

    def testEarlyEnd_WaitsForPulse(self):
        end = self.multiply("early")
        # Early for the loop's own end, P before the pulse, by more than
        # press_at()'s 2-buffer wait.
        self.press_at("MULTIPLY", end - max(6 * self.buf, self.P + 4 * self.buf + MARGIN))
        self.assertEqual(self.state(), "Multiplying")
        self.wait_state("Playing")
        self.assertEqual(self.length(), 2 * self.cycle)


class TestSyncPrerollOwnBoundaries(SyncPrerollTestCase):
    """With Sync off afterwards (the rig turns it off at transport stop),
    the loop makes its own cycle boundaries (its sync output, used by
    quantized commands and by loops synced to it).  They must stay on the
    bar lines: loop position P."""

    def ownBoundaries(self):
        pulse = self.start_waiting()
        self.end_early(pulse, cycles=1)
        self.looper.set_port(test_engine.Sync, 0.0)
        self.wait_buffers(2)
        n0 = self.looper.syncout_count
        self.wait_cycles(4)
        n1 = self.looper.syncout_count
        delta = self.looper.input_minus_sync
        phases = [(self.looper.get_syncout_frame(k) - delta - pulse) % self.cycle
                  for k in range(n0, n1)]
        self.assertGreaterEqual(len(phases), 3)
        self.assertEqual(phases, [0] * len(phases),
                         "the loop's own boundaries should be on the bar lines only")

    def testOnBarLines(self):
        self.ownBoundaries()

    def testOnBarLines_PrerollPastWrapRemainder(self):
        # A negative sync position wrapped in stock's unsigned modulo of
        # the boundary checks: an extra boundary in [0, P) when
        # 2^64 mod cycle <= P (1-2% of bar lengths at a 30 ms pre-roll).
        # The harness cycle's remainder is 7804, so P = 165 ms (7920)
        # shows it (SIGNED_SYNC_POS_FIX).
        test_engine.cvar.g_sync_preroll_ms = 165.0
        self.P = int(165.0 * 0.001 * self.sr)
        self.assertLessEqual(2 ** 64 % self.cycle, self.P)
        self.ownBoundaries()


class TestSyncPrerollNone(SyncPrerollTestCase):

    def testQuantizeOff_NoPreroll(self):
        # Sync on, Quantize Off: the engine's sync input is 1.0 on every
        # sample, so the record starts at the press, on no pulse: no
        # pre-roll.  With no pulse period the sync clock stops, so check
        # against the input clock: a press acts at a block start, and a
        # pre-roll (480 samples) would put the start off the block grid.
        self.looper.set_port(test_engine.Quantize, 0.0)
        self.looper.sync_inject_period = 0
        self.looper.sync_inject_value = 1.0
        self.wait_buffers(2)
        ic0 = self.looper.input_clock
        self.engine.request("RECORD")
        self.wait_buffers(3)
        self.assertEqual(self.state(), "Recording")
        start = (self.loop_frame(64) - 64) % RAMP
        self.assertLess((start - ic0) % RAMP, 4 * self.buf,
                        "the record should start at the press")
        self.assertEqual((start - ic0) % self.buf, 0,
                         "a Quantize Off record should start at the press, with no pre-roll")

    def testTooLongForHistory_NoPreroll(self):
        test_engine.cvar.g_sync_preroll_ms = 10000.0      # > the input history (5.5 s)
        pulse = self.start_waiting()
        self.end_early(pulse)
        self.assertPreRolled(pulse, 2 * self.cycle, P=0)

    def testZero_NoPreroll(self):
        test_engine.cvar.g_sync_preroll_ms = 0.0
        pulse = self.start_late()
        self.end_late(pulse)
        self.assertPreRolled(pulse, 2 * self.cycle, P=0)


class TestSyncPrerollStereo(SyncPrerollTestCase):

    CHANNELS = 2

    def channelsAlike(self, pulse):
        self.assertEqual(self.looper.get_channel_state(1),
                         self.looper.get_channel_state(0))
        self.assertEqual(self.looper.get_channel_loop_length(0), 2 * self.cycle)
        self.assertEqual(self.looper.get_channel_loop_length(1), 2 * self.cycle)
        self.assertPreRolled(pulse, 2 * self.cycle, ch=0)
        self.assertPreRolled(pulse, 2 * self.cycle, ch=1)

    def testWaitingStart_ChannelsAlike(self):
        pulse = self.start_waiting()
        self.end_early(pulse)
        self.channelsAlike(pulse)

    def testLateHitStart_LateStop_ChannelsAlike(self):
        pulse = self.start_late()
        self.end_late(pulse)
        self.channelsAlike(pulse)


if __name__ == "__main__":
    unittest.main()
