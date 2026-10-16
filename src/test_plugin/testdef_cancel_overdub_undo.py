"""Regression tests for CANCEL_OVERDUB_UNDO ("cancel_overdub_undo").

The overdub button's long/double-press companion: undo, as if the eager
overdub press just before it had not happened.  Acts only when the command
immediately before it was an overdub press, never removes a loop's first
take, and never lets redo restore a layer the eager press started.  The
cases are in mod_docs/late_hit_patch_summary.rst,
"cancel_overdub_undo Implementation".

Levels: a take at TAKE, real overdubs adding OD on top (run for longer
than one loop, so the layered loop plays between TAKE + OD and TAKE + 2*OD;
tests compare output ranges against a snapshot taken after each layer is
built), and NOISE on the input while the eager press's overdub runs -- a
discarded eager layer must leave no trace of it.
"""
import unittest

import testdef_simple_state
import engine_wrapper
import test_engine
from testdef_cancel_record_mute import _AudioHelpers


CMD = "CANCEL_OVERDUB_UNDO"

TAKE  =  0.5
OD    =  0.2
NOISE = -0.9


class commandRegistered(unittest.TestCase):

    def testCommandIsRegistered(self):
        self.assertIn(CMD, engine_wrapper.command_ordinals)


class cancelOverdubUndo(_AudioHelpers, testdef_simple_state.simpleStateTest):

    def setUp(self):
        testdef_simple_state.simpleStateTest.setUp(self)
        self.initAudio()

    def tearDown(self):
        self.looper.set_port(test_engine.OverdubQuantized, 0.0)
        testdef_simple_state.simpleStateTest.tearDown(self)

    # -- helpers ----------------------------------------------------------

    def snap(self):
        return self.outputRange()

    def assertLevel(self, want, msg=""):
        """want: a (min, max) snapshot, or a single steady level."""
        if not isinstance(want, tuple):
            want = (want, want)
        mn, mx = self.outputRange()
        ok = abs(mn - want[0]) < 0.005 and abs(mx - want[1]) < 0.005
        self.assertTrue(ok, "loop plays %.4f..%.4f, expected %.4f..%.4f. %s"
                        % (mn, mx, want[0], want[1], msg))

    def waiting(self):
        return self.looper.get_control_value(test_engine.Waiting) > 0.5

    def overdubLayer(self, value=OD):
        """A real overdub on top of what is playing, then stopped."""
        self.looper.input_inject_value = value
        self.engine.request("OVERDUB")
        self.waitBuffers(60)
        self.engine.request("OVERDUB")
        self.waitBuffers(10)
        self.assertState("Playing")
        self.looper.input_inject_value = 0.0

    def syncOnQuantizedOverdub(self):
        """Overdub starts and stops wait for a sync pulse; none is sent, so
        they stay queued until a test sends one."""
        self.looper.set_port(test_engine.OverdubQuantized, 1.0)
        self.looper.sync_inject_value   = 0.0
        self.looper.sync_inject_enabled = True
        self.looper.set_port(test_engine.Sync, 1.0)
        self.waitBuffers(3)

    def pulse(self, n=3):
        self.looper.sync_inject_value = 2.0
        self.waitBuffers(n)
        self.looper.sync_inject_value = 0.0

    def cancel(self, settle=20):
        self.engine.request(CMD)
        self.waitBuffers(settle)

    # -- does nothing unless an overdub press came just before -------------

    def testNoOpWithoutOverdubPress(self):
        self.recordTake(TAKE)
        self.cancel()
        self.assertState("Playing")
        self.assertLevel(TAKE, "nothing should have been undone")

    def testNoOpWhenAnotherCommandCameBetween(self):
        self.recordTake(TAKE)
        self.overdubLayer()                     # last command: overdub stop
        layered = self.snap()
        self.engine.request("UNDO")
        self.waitBuffers(10)
        self.engine.request("REDO")             # back to the layered loop
        self.waitBuffers(10)
        self.assertLevel(layered)
        self.cancel()
        self.assertLevel(layered, "a command came between: must not undo")

    # -- the eager press started an overdub --------------------------------

    def testStartedFromPlayDiscardsItAndUndoesTheLayerBelow(self):
        self.recordTake(TAKE)
        self.overdubLayer()
        layered = self.snap()
        self.looper.input_inject_value = NOISE
        self.engine.request("OVERDUB")          # the eager press
        self.waitBuffers(10)
        self.assertState("Overdubbing")
        self.cancel()
        self.assertState("Playing")
        self.assertLevel(TAKE, "eager layer and the overdub below it should both be gone")
        self.engine.request("REDO")
        self.waitBuffers(20)
        self.assertLevel(layered, "redo should restore the real overdub")
        self.engine.request("REDO")
        self.waitBuffers(20)
        self.assertLevel(layered, "a second redo must not restore the eager layer")

    def testStartedOnFirstTakeKeepsTheTake(self):
        self.recordTake(TAKE)
        self.looper.input_inject_value = NOISE
        self.engine.request("OVERDUB")
        self.waitBuffers(10)
        self.cancel()
        self.assertState("Playing")
        self.assertLevel(TAKE, "the first take must never be removed")
        self.engine.request("REDO")
        self.waitBuffers(20)
        self.assertLevel(TAKE, "redo must not restore the eager layer")

    def testStartedFromMuteReturnsToSilentMute(self):
        self.recordTake(TAKE)
        self.overdubLayer()
        layered = self.snap()
        self.engine.request("MUTE_ON")
        self.waitBuffers(10)
        self.assertState("Muted")
        self.looper.input_inject_value = NOISE
        self.engine.request("OVERDUB")          # from mute: audible again
        self.waitBuffers(10)
        self.assertState("Overdubbing")
        self.cancel()
        self.assertState("Muted")
        mn, mx = self.outputRange(40)
        self.assertAlmostEqual(mn, 0.0, places=3, msg="muted but not silent: %.4f" % mn)
        self.assertAlmostEqual(mx, 0.0, places=3, msg="muted but not silent: %.4f" % mx)
        self.looper.input_inject_value = 0.0
        self.engine.request("MUTE_OFF")
        self.waitBuffers(10)
        self.assertLevel(TAKE, "the overdub below the eager layer should be undone")
        self.engine.request("REDO")
        self.waitBuffers(20)
        self.assertLevel(layered, "redo should restore the real overdub")

    def testQueuedStartIsDroppedAndLastLayerUndone(self):
        self.recordTake(TAKE)
        self.overdubLayer()
        self.syncOnQuantizedOverdub()
        self.looper.input_inject_value = NOISE
        self.engine.request("OVERDUB")          # queued until a pulse
        self.waitBuffers(5)
        self.assertTrue(self.waiting(), "not queued")
        self.cancel()
        self.assertFalse(self.waiting(), "queued start not dropped")
        self.pulse()
        self.waitBuffers(10)
        self.assertState("Playing", "the dropped start must not begin at the pulse")
        self.assertLevel(TAKE, "the last overdub should be undone")

    # -- the eager press stopped an overdub ---------------------------------

    def testImmediateStopUndoesThatOverdub(self):
        self.recordTake(TAKE)
        self.looper.input_inject_value = OD
        self.engine.request("OVERDUB")          # overdubbing for a while
        self.waitBuffers(60)
        self.engine.request("OVERDUB")          # the eager press: stops at once
        self.waitBuffers(10)
        self.assertState("Playing")
        self.looper.input_inject_value = 0.0
        layered = self.snap()
        self.cancel()
        self.assertLevel(TAKE, "the overdub just stopped should be undone")
        self.engine.request("REDO")
        self.waitBuffers(20)
        self.assertLevel(layered, "a real overdub stays redo-able")

    def testQueuedStopDiscardsTheRunningOverdub(self):
        self.recordTake(TAKE)
        self.looper.input_inject_value = OD
        self.engine.request("OVERDUB")          # started unquantized
        self.waitBuffers(60)
        self.syncOnQuantizedOverdub()
        self.engine.request("OVERDUB")          # the eager press: stop queued
        self.waitBuffers(5)
        self.assertState("Overdubbing")
        self.assertTrue(self.waiting(), "not queued")
        self.cancel()
        self.assertState("Playing")
        self.looper.input_inject_value = 0.0
        self.pulse()
        self.waitBuffers(10)
        self.assertState("Playing")
        self.assertLevel(TAKE, "the running overdub should be discarded")

    # -- the eager press ended a record into an overdub ----------------------

    def testRecordIntoOverdubKeepsTheTake(self):
        self.looper.input_inject_value = TAKE
        self.engine.request("RECORD")
        self.waitBuffers(30)
        self.looper.input_inject_value = NOISE
        self.engine.request("OVERDUB")          # ends the record into an overdub
        self.waitBuffers(10)
        self.assertState("Overdubbing")
        self.cancel()
        self.assertState("Playing")
        self.looper.input_inject_value = 0.0
        self.assertLevel(TAKE, "the take should be kept, the overdub dropped")
        self.engine.request("REDO")
        self.waitBuffers(20)
        self.assertLevel(TAKE, "redo must not restore the eager layer")
