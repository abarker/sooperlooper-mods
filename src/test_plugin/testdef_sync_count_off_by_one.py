#! /usr/bin/env python3
"""
Tests for SYNC_COUNT_OFF_BY_ONE_FIX (plugin.cc): lSamplesSinceSync reads
one less than the distance from the pulse.  Stock took it as the distance
in a synced restart pressed in the slack window (with a tempo set, less
than an eighth after a pulse it acts at once): one sample late under plain
Sync.  And a Rel Sync recording's sync position put its grid one sample
late, so a restart that waited for the pulse came back one sample early.
Now every synced restart plays the loop exactly in phase: plain Sync and
Rel Sync, pressed in the slack window or waiting for the pulse, Trigger
and mute_trigger.

Uses testdef_rel_sync_pos.py's exact tempo (eighth 1001, cycle 8008) and
its ramp-input phase check.
"""

import unittest
import test_engine
from testdef_rel_sync_pos import RelSyncPosTestCase, CYCLE, EIGHTH, QUARTERS

SLACK = 300             # samples after the pulse: inside the slack window
SLACK_SPAN = 400        # a slack press lands SLACK .. SLACK + SLACK_SPAN - 1 after it
WAIT = 3 * EIGHTH       # outside it: the restart waits for the next pulse


class SyncCountTestCase(RelSyncPosTestCase):

    SYNC = 2.0

    def setUp(self):
        super().setUp()
        self.looper.set_port(test_engine.Sync, self.SYNC)
        self.wait_cycles(1)

    def record(self):
        if self.SYNC == 1.0:
            # Plain Sync: wait for the pulse, end on one two cycles on.
            pulse = (self.looper.sync_clock // CYCLE + 2) * CYCLE
            self.press_at("RECORD", pulse - 6 * self.buf)
            self.wait_state("Recording")
            self.press_at("RECORD", pulse + 2 * CYCLE - 6 * self.buf)
            self.wait_state("Playing")
        else:
            self.record_at_phase(*QUARTERS[2])
        self.assertEqual(self.playback_offsets(), {0})

    def press_frame(self, after):
        """A block start about `after` samples after a pulse at least two
        cycles on.  A press acts at the first block starting at or after
        its frame, so "pulse + after" can land up to a block later: past
        the eighth (1001) for a slack press with 1024-sample blocks.  For
        one meant for the slack window (after < EIGHTH), pick a pulse
        whose next block start is less than SLACK_SPAN later."""
        k = self.looper.sync_clock // CYCLE + 2
        while True:
            f = -(-(k * CYCLE + after) // self.buf) * self.buf
            if after >= EIGHTH or f - k * CYCLE < after + SLACK_SPAN:
                return f
            k += 1

    def restart(self, command, after):
        """Mute, then press `command` `after` samples after a pulse (see
        press_frame())."""
        self.engine.request("MUTE")
        self.wait_state("Muted")
        self.press_at(command, self.press_frame(after))
        acted = self.state() == "Playing"
        self.assertEqual(acted, after < EIGHTH,
                         "in the slack window it should act at once, else wait")
        self.wait_state("Playing", units=2)
        self.assertEqual(self.playback_offsets(), {0},
                         f"{command} {after} samples after the pulse: out of phase")


class TestPlainSync(SyncCountTestCase):

    SYNC = 1.0

    def testTrigger_Slack(self):
        self.record()
        self.restart("TRIGGER", SLACK)

    def testTrigger_Waits(self):
        self.record()
        self.restart("TRIGGER", WAIT)

    def testMuteTrigger_Slack(self):
        self.record()
        self.restart("MUTE_TRIGGER", SLACK)

    def testMuteTrigger_Waits(self):
        self.record()
        self.restart("MUTE_TRIGGER", WAIT)

    def testRepeatedSlackRestarts_StayInPhase(self):
        self.record()
        for _ in range(3):
            self.restart("MUTE_TRIGGER", SLACK)


class TestRelSync(SyncCountTestCase):

    SYNC = 2.0

    def testTrigger_Slack(self):
        self.record()
        self.restart("TRIGGER", SLACK)

    def testTrigger_Waits(self):
        self.record()
        self.restart("TRIGGER", WAIT)

    def testMuteTrigger_Slack(self):
        self.record()
        self.restart("MUTE_TRIGGER", SLACK)

    def testMuteTrigger_Waits(self):
        self.record()
        self.restart("MUTE_TRIGGER", WAIT)


if __name__ == "__main__":
    unittest.main()
