#! /usr/bin/env python3
"""
Regression tests for RESTART_POSITION_FIX (stock bug).

A trigger into play restarts the loop at the top of its buffer.  Playback
after a record starts input + output latency (IL + OL) into the buffer, so
that what is heard lines up with what was played; stock added nothing on
a restart, so a restarted loop played IL + OL behind.  With the fix, every
trigger into play starts at IL + OL -- immediate, in the relative-sync
slack window, or after waiting for a sync pulse -- except from Record and
TrigStop, where MULTI_TRIGGER already adds it (must not be doubled).  A
oneshot had the same bug and gets the same offset, immediate or synced,
and must still end Muted after one pass.

Positions are read from the LoopPosition control (dCurrPos / sample rate,
updated every buffer).  A command acts at the start of the next buffer, so
"right after" a trigger means within a few buffers: IL + OL is set far
larger than that, so the check cannot pass by accident.

Sync buffer injection values:
  0.0  -- no sync signal; a synced trigger waits indefinitely
  2.0  -- sync pulse (> 1.5 threshold)
"""

import time
import unittest
import test_engine
import engine_wrapper

IL, OL = 3000, 4000      # unequal, and far larger than a few buffers
ALL_EDGES = bool(getattr(test_engine, "TRIGGER_LATENCY_ALL_EDGES", 0))


class TriggerLatencyTestCase(unittest.TestCase):

    PULSE_BUFS = 3

    def setUp(self):
        self.engine = engine_wrapper.Engine("SL_test_triglat")
        self.looper = self.engine.looper
        self.buf    = int(self.looper.get_buffer_size())
        self.sr     = int(self.engine.get_sample_rate())
        self.looper.sync_inject_value   = 0.0
        self.looper.sync_inject_enabled = True
        self.looper.set_port(test_engine.Sync, 0.0)
        self.looper.set_port(test_engine.MuteQuantized, 0.0)
        self.looper.set_port(test_engine.DryLevel, 0.0)
        self.looper.set_port(test_engine.WetLevel, 1.0)
        self.set_latency(IL, OL)

    def tearDown(self):
        del self.engine

    # ------------------------------------------------------------------
    # helpers

    def set_latency(self, il, ol):
        self.il, self.ol = il, ol
        self.looper.set_port(test_engine.InputLatency, float(il))
        self.looper.set_port(test_engine.OutputLatency, float(ol))

    def wait_buffers(self, n, timeout=5.0):
        """Busy-wait until exactly n more JACK callbacks have completed."""
        target = self.looper.process_count + n
        period = float(self.buf) / float(self.sr)
        deadline = time.time() + max(timeout, 3.0 * n * period)
        while self.looper.process_count < target:
            if time.time() > deadline:
                raise RuntimeError(
                    f"Timed out waiting for {n} JACK callbacks "
                    f"(process_count stuck at {self.looper.process_count})")

    def assertState(self, state, msg=None):
        actual = self.engine.getState()
        self.assertEqual(actual, engine_wrapper.states[state],
                         (engine_wrapper.states[actual] + " != " + state)
                         if msg is None else msg)

    def unsynced_start(self):
        """Where an unsynced restart that ends no take starts: IL + OL;
        trigger_latency + OL with TRIGGER_LATENCY_ALL_EDGES (increment (d):
        it happened at the press)."""
        if ALL_EDGES:
            return int(self.looper.get_control_value(test_engine.TriggerLatency)) + OL
        return IL + OL

    def position(self):
        """Loop position in samples."""
        return self.looper.get_control_value(test_engine.LoopPosition) * self.sr

    def record_bufs(self):
        """A loop several times longer than IL + OL."""
        return max(15, 4 * (IL + OL) // self.buf + 1)

    def record_a_loop(self, level=0.5):
        """Record with sync off, leaving it Playing."""
        self.looper.input_inject_value = level
        self.engine.request("RECORD")
        self.wait_buffers(self.record_bufs())
        self.engine.request("RECORD")
        # With TRIGGER_LATENCY_ALL_EDGES the end waits IL (trigger_latency
        # is 0 here): 5 buffers were enough only at 1024-sample blocks.
        self.wait_buffers(5 + (IL if ALL_EDGES else 0) // self.buf)
        self.assertState("Playing", "record_a_loop: could not enter Playing")

    def mute_it(self):
        self.record_a_loop()
        self.engine.request("MUTE_ON")
        self.wait_buffers(5)
        self.assertState("Muted", "mute_it: could not enter Muted")

    def prime_pulse(self):
        self.looper.sync_inject_value = 2.0
        self.wait_buffers(self.PULSE_BUFS)
        self.looper.sync_inject_value = 0.0

    def wait_for_state(self, state, max_bufs):
        """One buffer at a time until `state`, so it was entered within
        the last buffer.  With TRIGGER_LATENCY_ALL_EDGES on, a take's end
        waits IL - trigger_latency (here IL: trigger_latency is 0)."""
        for _ in range(max_bufs):
            self.wait_buffers(1)
            if self.engine.getState() == engine_wrapper.states[state]:
                return
        self.assertState(state)

    def assertStartsAt(self, start, slack_bufs, what):
        """Position is start + (a few buffers of elapsed time)."""
        pos = self.position()
        elapsed = pos - start
        self.assertTrue(
            0 <= elapsed <= slack_bufs * self.buf,
            f"{what}: position {pos:.0f}, expected {start} + 0..{slack_bufs * self.buf} "
            f"(IL={self.il}, OL={self.ol}, buffer={self.buf})")


class TestTriggerLatency(TriggerLatencyTestCase):

    def testTriggerFromMute_StartsAtLatency(self):
        """Immediate trigger (sync off) from Mute starts at IL + OL."""
        self.mute_it()
        self.engine.request("TRIGGER")
        self.wait_buffers(1)
        self.assertState("Playing")
        self.assertStartsAt(self.unsynced_start(), 3, "trigger from Mute")

    def testMuteTriggerFromMute_StartsAtLatency(self):
        """mute_trigger from Mute (the rig's restart) starts at IL + OL."""
        self.mute_it()
        self.engine.request("MUTE_TRIGGER")
        self.wait_buffers(1)
        self.assertState("Playing")
        self.assertStartsAt(self.unsynced_start(), 3, "mute_trigger from Mute")

    def testTriggerWhilePlaying_StartsAtLatency(self):
        """A restart while playing starts at IL + OL too."""
        self.record_a_loop()
        self.wait_buffers(self.record_bufs() // 2)
        self.engine.request("TRIGGER")
        self.wait_buffers(1)
        self.assertStartsAt(self.unsynced_start(), 3, "trigger while playing")

    def testSyncedTrigger_StartsAtLatencyOnPulse(self):
        """With sync on (tempo 0, so no slack window), a trigger waits for
        the pulse and then starts at IL + OL."""
        self.mute_it()
        self.looper.set_port(test_engine.Sync, 1.0)
        self.wait_buffers(2)
        self.engine.request("TRIGGER")
        self.wait_buffers(4)
        self.assertState("Muted", "synced trigger should wait for the pulse")
        self.prime_pulse()
        self.assertState("Playing")
        self.assertStartsAt(IL + OL, self.PULSE_BUFS + 2, "synced trigger at the pulse")

    def testTriggerInSlackWindow_StartsAtLatencyPlusSinceSync(self):
        """With a tempo, a trigger just after a pulse acts at once and
        starts at IL + OL plus the samples since the pulse."""
        self.mute_it()
        self.looper.set_port(test_engine.TempoInput, 60.0)  # eighth = 0.5 s
        self.looper.set_port(test_engine.Sync, 1.0)
        self.wait_buffers(2)
        self.prime_pulse()
        self.engine.request("TRIGGER")
        self.wait_buffers(1)
        self.assertState("Playing", "trigger in the slack window should act at once")
        self.assertStartsAt(IL + OL, self.PULSE_BUFS + 4, "trigger in the slack window")

    def testTriggerEndingRecord_NotDoubled(self):
        """A trigger that ends a record keeps the stock offset (IL + OL
        once, added by MULTI_TRIGGER), not twice."""
        self.looper.input_inject_value = 0.5
        self.engine.request("RECORD")
        self.wait_buffers(self.record_bufs())
        self.engine.request("TRIGGER")
        self.wait_for_state("Playing", IL // self.buf + 4)
        self.assertStartsAt(IL + OL, 3, "trigger ending a record")

    def testOneShotFromMute_StartsAtLatency(self):
        """Immediate oneshot (sync off) from Mute starts at IL + OL."""
        self.mute_it()
        self.engine.request("ONESHOT")
        self.wait_buffers(1)
        self.assertState("OneShot")
        self.assertStartsAt(self.unsynced_start(), 3, "oneshot from Mute")

    def testSyncedOneShot_StartsAtLatencyOnPulse(self):
        """A synced oneshot waits for the pulse, then starts at IL + OL."""
        self.mute_it()
        self.looper.set_port(test_engine.Sync, 1.0)
        self.wait_buffers(2)
        self.engine.request("ONESHOT")
        self.wait_buffers(4)
        self.assertState("Muted", "synced oneshot should wait for the pulse")
        self.prime_pulse()
        self.assertState("OneShot")
        self.assertStartsAt(IL + OL, self.PULSE_BUFS + 2, "synced oneshot at the pulse")

    def testOneShot_StillEndsMuted(self):
        """Starting IL + OL in, a oneshot still plays to the loop's end and
        then mutes."""
        self.mute_it()
        self.engine.request("ONESHOT")
        # One pass is the loop's length (ONESHOT_ONE_PASS_FIX), which the
        # take end's IL wait makes longer than record_bufs() at small
        # blocks.  Allow up to two: this only checks that it ends Muted;
        # testdef_oneshot_one_pass.py checks the pass is exactly one.
        length = self.looper.get_control_value(test_engine.LoopLength) * self.sr
        self.wait_for_state("Muted", 2 * int(length) // self.buf + 10)
        self.assertState("Muted", "oneshot should end Muted after one pass")

    def testOneShotEndingRecord_NotDoubled(self):
        """A oneshot that ends a record keeps the stock offset (IL + OL
        once), not twice."""
        self.looper.input_inject_value = 0.5
        self.engine.request("RECORD")
        self.wait_buffers(self.record_bufs())
        self.engine.request("ONESHOT")
        self.wait_for_state("OneShot", IL // self.buf + 4)
        self.assertStartsAt(IL + OL, 3, "oneshot ending a record")

    def testZeroLatency_StartsAtTop(self):
        """With IL = OL = 0 a restart starts at the top, as stock."""
        self.set_latency(0, 0)
        self.mute_it()
        self.engine.request("TRIGGER")
        self.wait_buffers(1)
        self.assertStartsAt(0, 3, "trigger with zero latency")


if __name__ == "__main__":
    unittest.main()
