test_plugin
===========

Regression tests for SooperLooper's plugin.cc.  The test harness wraps
plugin.cc via SWIG and runs each looper instance as a real JACK client,
taking it through its states from Python.


What the tests check, from the performer's side
-----------------------------------------------

The tests play the looper the way a performer would: they press its
commands at exact moments and feed it input, then read back what it
recorded and what it plays.  The input is usually a ramp or a constant,
so every recorded sample says which moment of the input it came from.
Most checks are therefore exact to the sample, not approximate.

What they check:

- **Where recordings start and end.**  A recording, overdub, Replace,
  Substitute, Multiply, or Insert begins and ends with what was played
  at the presses, with the trigger latency below, equal to, and above
  the input latency.  With Sync on, a recording lands on the beat, and a
  press a little after the beat counts as on it.  The sync pre-roll and
  the free pre-roll start a recording early by the set amount.
- **How long loops are.**  With Sync off, a loop is as long as the time
  between the presses.  With Sync on, it is a whole number of cycles.
  Multiply and Insert add whole cycles.
- **Whether playback is in time.**  What is heard lines up with what was
  played: on the first pass after a recording, and after every restart
  (Trigger, ``mute_trigger``, Once), whether it acts at once, in the
  slack window, or on the beat, including several restarts in a row.
  The beats a loop sends to other loops are checked too.
- **That nothing is lost.**  Every sample of a new layer is the input
  played at that point; the layers underneath are intact; there are no
  silent gaps at a Multiply's end, around an Insert, or where an overdub
  crosses the loop's start; a Trigger does not empty the loop.
- **Which presses count.**  Which presses are corrected as late hits
  and which wait for the next beat, with the window's edge exact, for
  Record, Mute, and the commands that end a recording.
- **States and safety.**  The state each command leads to from each
  state; the cancel, undo, and redo commands; the record failsafe; a
  recording that runs out of memory ends without a crash; the two
  channels of a stereo loop stay together.

What they do not check (see also `Not covered`_):

- **The real delays.**  "On the press" means on the press as the input,
  output, and trigger latency settings describe it.  Whether those
  settings match the hardware is for ``sllatency`` and measurements on
  the rig.
- **How it sounds.**  The crossfade is one sample in most tests, so fade
  shapes and level changes are mostly not checked.  The exceptions, with
  a 64-sample fade: a Record started or stopped late on the beat must
  play back flat; every layer type's edges must be straight fades with
  no click, with the trigger latency below, equal to, and above the
  input latency; and a Record over a playing loop must fade the loop
  out, not cut it.  Levels are otherwise checked only as exact copies or
  as "not silent".
- **The rest of the system.**  Several loops playing together, timing
  jitter from a real sync source (beats are injected exactly), MIDI
  through the remapper, slgui, saving and loading sessions, and
  real-time performance (xruns).
- **Some modes.**  A quantized Insert, and rate changes, Reverse, and
  Scratch beyond the moment they end a recording.

The dummy-server probes outside this directory (``rig_probes/``) cover
part of the second list: they run the whole engine, with JACK MIDI
presses, on a private server.


Dependencies
------------

System packages (Ubuntu/Debian)::

    sudo apt install libjack-jackd2-dev ladspa-sdk

Python packages::

    pip install swig pytest parameterized

Note: ``swig`` can be installed either via pip (as above) or via the
system package manager (``sudo apt install swig``).  Either works.


Build
-----

With JACK running (a dummy backend is sufficient)::

    jackd -d dummy &
    make

The tests run in real time against whatever server they connect to, so
buffer size matters only for how long they take.  At 48 kHz they pass
at every size tried, 64 to 4096 frames, including non-power-of-two
sizes such as 384 (the dummy backend's default is 1024).  Waits are
counted in buffers, with timeouts that scale with the buffer period.
To keep them off a live rig's server, start a separately named one and
point the tests at it::

    jackd -n sltest -d dummy -r 48000 -p 256 &
    JACK_DEFAULT_SERVER=sltest python3 -m pytest

This runs SWIG to generate the Python bindings and compiles the shared
library ``_test_engine.so``.


Run
---

::

    python3 -m pytest

Using ``python3 -m pytest`` rather than the bare ``pytest`` command ensures
all imports resolve against the active virtual environment, avoiding
conflicts when pytest dependencies are split across system and venv
site-packages.  To run a single file::

    python3 -m pytest testdef_simple_state.py


Test files
----------

The docstring at the top of each file gives its background and setup,
and each test's docstring the case it checks.

testdef_simple_state.py
    Basic state transitions: RECORD from STATE_OFF.

testdef_simple_state_after_record.py
    Transitions from STATE_PLAY (after a complete record cycle).

testdef_simple_state_off.py
    All commands from STATE_OFF; parameterized over the full command set.

testdef_simple_state_offmuted.py
    All commands from STATE_OFF_MUTED.

testdef_simple_state_overdub.py
    Transitions from STATE_OVERDUB.

testdef_simple_state_record_undo.py
    State after RECORD → RECORD → UNDO; parameterized over commands.

testdef_simple_state_record_undoall.py
    State after RECORD → RECORD → UNDO_ALL; parameterized over commands.

testdef_cancel_record_mute.py
    Regression tests for CANCEL_RECORD_MUTE ("cancel_record_mute"): cancels
    an in-progress record keeping the prior take, muting only if audio was
    actually captured, and is a no-op in every other state.  Also asserts
    the command-ordinal contract between event.hpp, the positional
    ``commands`` tuple in engine_wrapper.py, and the MULTI_* integer
    literals in plugin.cc.  Covers the unquantized branches, the quantized
    pending states (sync on, no pulse -- WaitStart and WaitStop), and audio
    content via the input-injection / output-readback support in
    TestLooper.  See ../../mod_docs/late_hit_patch_summary.rst.

testdef_cancel_overdub_undo.py
    Tests for CANCEL_OVERDUB_UNDO ("cancel_overdub_undo"), the overdub
    button's long-press companion: undoes the overdub press just before
    it, and only when that was the last command; never removes a loop's
    first take, and redo cannot bring back the discarded layer.  Checks
    the audio layer by layer.

testdef_record_failsafe.py
    Tests for the record failsafe (g_record_failsafe_seconds): a record
    that interrupted an unmuted, playing loop and runs past the limit is
    canceled as by cancel_record_mute; a first record, or one started
    from Muted, runs on; 0 turns it off.

testdef_late_hit.py
    Tests for Record's late-hit correction: a Record start or stop just
    after a sync pulse acts as if on the pulse; none with sync off,
    before any pulse, outside the window or on time, or under Rel Sync;
    the window edge is exact; late takes are whole buffers long and play
    back flat, like on-time ones.

testdef_late_hit_half_unit.py
    Tests for the late-hit window's limits against the Quantize unit (an
    eighth with Quantize 8th, a cycle with Quantize Cycle): a press a little
    before a pulse starts on it even with a window over half the unit (the
    nearer pulse wins), Record and Mute alike; late presses under half the
    unit are still corrected; the fraction setting is a fraction of the unit,
    not of the cycle.  With a window longer than the shortest input history
    (32768 samples), a press beyond it is not corrected and is counted for
    the engine's warning; with g_input_history_seconds (--input-history) set
    to 2 it is corrected.

testdef_mute_late_hit.py
    Tests for the mute/unmute late-hit correction (g_mute_late_hit_ms): a
    quantized Mute pressed just after a pulse mutes or unmutes at once
    instead of waiting for the next pulse; outside the window or with
    the window 0 it waits; Pause and Record are unaffected.

testdef_stale_sync_counter_after_toggle.py
    Tests that the samples-since-pulse counter, frozen while Sync is
    off, does not trigger a late-hit Record or unmute when Sync comes
    back on with no pulse since.

testdef_rel_sync_stale_phase.py
    Regression tests for REL_SYNC_STALE_PHASE_FIX: a Rel Sync record
    pressed right after Sync comes back on, or later, gives whole cycles.
    Uses TestLooper's own pulse generator (``sync_inject_period``,
    ``sync_clock``) so pulses land on exact samples, and a two-channel
    engine (``Engine(name, 2)``) to check that the second channel, which
    takes its sync from channel 0's output, starts with the first.

testdef_rel_sync_known_phase.py
    Tests for REL_SYNC_STALE_PHASE_FIX's tracked count: Rel Sync coming on
    while pulses flow starts a record at the press; a Quantize change,
    pulses that stop and resume, and a loop that has never seen a pulse
    make it wait for the next pulse; stereo channels start together.
    (A sync source change needs the real engine: the harness does not
    link looper.cpp.)

testdef_rel_sync_late_hit.py
    Tests for REL_SYNC_LATE_HIT: a Rel Sync record started at the press
    starts trigger_latency + rel_sync_preroll_ms earlier, from the input
    history, ends as much earlier (whole cycles) and plays in phase; one
    that waits for a pulse does not start early; a command ending the
    take (Record, Overdub, quantized Mute or mute_trigger) within the
    late-hit window after the end point ends it there at once, outside
    the window or just after the start it waits; Cycle and 8th, stereo.  Presses go in at exact frames
    (request_at_clock) and the input is a ramp (input_ramp), so lengths,
    contents and playback phase are checked exactly.

testdef_late_stop_any_command.py
    Tests for LATE_STOP_ANY_COMMAND: under plain Sync, a take ended just
    after a pulse by mute_trigger, a quantized Mute or Overdub ends on the
    pulse at once (Muted / Overdubbing), plays in phase when unmuted;
    outside the window or with the window 0 it waits another cycle;
    early, it waits for the pulse; Record's own late stop unchanged;
    stereo channels alike.

testdef_sync_preroll.py
    Tests for SYNC_PREROLL: with sync_preroll_ms set, a plain Sync record
    starting on a pulse (waiting for it, or a late-hit start) starts that
    much before it, ends as much before its closing pulse (on the pulse,
    Record's late stop, mute_trigger and Overdub late stops), stays whole
    cycles and plays in phase; with a tempo set too; Trigger, Mute and
    Sync off afterwards; the Multiply late end; stereo channels alike;
    none for Quantize Off, a pre-roll too long for the input history, or
    0.  Sync output boundaries are read back (syncout_count,
    get_syncout_frame) to check the loop's own boundaries with Sync off.

testdef_rel_sync_pos.py
    Tests for REL_SYNC_POS_FIX: with a tempo set, a Rel Sync recording
    started in any quarter of the cycle gets its sync position from the
    nearer boundary (read back with get_channel_sync_pos); a synced
    Trigger or mute_trigger restart plays it in phase; with Sync off its
    own boundaries stay on the bar lines; no tempo unchanged; stereo
    channels alike.

testdef_sync_count_off_by_one.py
    Tests for SYNC_COUNT_OFF_BY_ONE_FIX: with a tempo set, every synced
    Trigger or mute_trigger restart plays the loop exactly in phase,
    plain Sync and Rel Sync, pressed in the slack window (acting at once)
    or waiting for the pulse, and repeated restarts stay in phase.

testdef_oneshot_slack.py
    Tests for ONESHOT_SLACK_FIX: with a tempo set, a Once pressed in the
    slack window (acting at once) or waiting for the pulse plays its pass
    exactly in phase, plain Sync and Rel Sync, and still ends in Mute.

testdef_oneshot_one_pass.py
    Tests for ONESHOT_ONE_PASS_FIX: with Rel Sync and IL = OL = 743, a
    Once from Mute plays exactly one loop length whatever the loop's sync
    position (negative, positive under IL + OL, larger), and one pressed
    in the slack window plays the loop less the samples since the pulse.
    The pass is measured from the first to the last nonzero output sample
    (output_nonzero_first / output_nonzero_last).  Skipped when the
    switch is 0.

testdef_trigger_feed_fade.py
    Tests for TRIGGER_FEED_FADE_FIX: a Replace or Substitute ended by
    Trigger (at once, or at the pulse with Sync on), or by a Once that
    waited for the pulse, leaves the loop intact and playing three loop
    lengths later.  Overdub ended by Trigger, and Replace ended by an
    immediate Once, are controls (right in stock too).

testdef_layer_placement.py
    Tests for TL_OVER_IL_LAYER_FIX and OVERDUB_WRAP_FILL_FIX: a take of a
    constant under a ramp-input Overdub, Replace or Substitute, so every
    loop sample tells old layer from new and which input frame it holds.
    Each layer sample sits where its input frame was heard; the layer
    runs from the input played at the first press to that played at the
    second, none lost; the old layer is intact.  At TL > IL (skipped when
    TL_OVER_IL_LAYER_FIX is 0), TL = IL and TL < IL; started mid-loop,
    (OL + IL) mod the block size into the loop, and exactly OL + IL in;
    and a layer shorter than OL + IL + k.  The loop is read a whole loop
    after the layer ends (the new chunk is filled as the play position
    passes).

testdef_multiply_placement.py
    Tests for TL_OVER_IL_LAYER_FIX's Multiply half, as above: every
    multiplied sample where its input frame was heard, the first one
    played at the press; unrounded, the last at the second press and the
    old loop intact after it (the end fills the k samples its leftover
    fill does not); rounded, recorded to the loop's end, within one cycle
    and over two (ROUNDED_MULTIPLY_FILL_FIX).  Run at IL = OL and, for
    MULTIPLY_FRONTFILL_FIX, at IL > OL and OL > IL (no gap or overlap
    before the multiply).

testdef_insert_placement.py
    Tests for TL_OVER_IL_LAYER_FIX's Insert half: the inserted stretch
    starts where the press was heard, every sample in place, the first
    and (unrounded) last ones played at the presses, the old loop intact
    before it.  And INSERT_GAP_FILL_FIX: the old loop whole after the
    inserted cycles (OL + IL + k samples lost without it), and a rounded
    insert's cycles holding the input to their end.  Run at IL = OL, IL >
    OL, and OL > IL.

testdef_layer_fades.py
    Tests for the fades at a layer's edges (TL_OVER_IL_LAYER_FIX's
    fade-out, TL_UNDER_IL_LAYER_FIX) and at a Record over a playing loop
    (RECORD_OVER_PLAY_FADE), with a 64-sample fade.  A constant take
    under a constant layer, so a right loop holds only levels and
    straight ramps between them: no two neighbouring samples may differ
    by more than a ramp's step (a cut, a fade-out away from the layer's
    end, a step at a Replace's start, or silence before a Multiply or
    Insert all do).  Overdub, Replace, Substitute, a short Overdub,
    Multiply and Insert rounded and not, at TL > IL (skipped when
    TL_OVER_IL_LAYER_FIX is 0), TL = IL and TL < IL.  An Insert's start
    is allowed one step (the old loop stops there with no fade, stock),
    but no silence before the inserted input.  The Record cases read the
    output back sample by sample (TestLooper.get_output_at): the loop
    plays until the take starts, then fades out within the fade length.

testdef_signed_sync_pos.py
    Tests for SIGNED_SYNC_POS_FIX: with Sync off, a Rel Sync loop whose
    sync position is negative (an early start pressed just after a pulse)
    makes one boundary per cycle, not an extra one near its start; a
    positive one is unchanged.  The cycle (8008) is chosen so the stock
    unsigned wrap shows.  Reads channel 0's sync output back
    (syncout_count, get_syncout_frame).

testdef_trig_stop_memory.py
    Regression tests for TRIG_STOP_MEMORY_FIX: a recording whose stop is
    still pending when the loop memory runs out ends there and plays,
    instead of crashing the engine.  Each scenario runs in a child process
    with ``SL_SAMPLE_TIME=3`` (5.46 s of memory), so a crash fails the
    test rather than pytest.


testdef_multiply_end.py
    Tests for MULTIPLY_END_FIX: a multiply end leaves no silent stretch
    at the end of the multiplied loop (Round on and off, ended by
    Overdub, Undo and Redo afterwards); a multiply end just after a
    pulse inside a cycle is not taken as a late hit.  Presses and single
    pulses go in at exact frames (request_at_clock, sync_pulse_at).

testdef_multiply_after_late_stop.py
    A multiply after a Record late stop keeps the loop's content: the
    output stays non-silent over two passes of the multiplied loop, for a
    waiting or late-hit start, a waiting or late stop, and a late multiply
    end, with and without a pre-roll.  The waiting stops are controls.

testdef_restart_position.py
    Tests for RESTART_POSITION_FIX: every trigger into play (Trigger,
    mute_trigger, Once; immediate, in the slack window or on the pulse)
    starts input + output latency into the loop, as playback after a
    record does, except from Record, where it is not doubled.

testdef_trigger_latency_all_edges.py
    Tests for TRIGGER_LATENCY_ALL_EDGES (skipped when it is off): every
    take and layer edge that acts when its command arrives lands on the
    press, at trigger latency below, equal to, and above the input
    latency.  A take keeps the press interval as its length and its
    playback phase whatever command ends it; a Pause, Reverse or Scratch
    ending a take resumes or plays where the immediate end's would;
    unsynced restarts play from the press; restarts in the slack window
    keep the loop's phase.  The Preroll classes (FREE_PREROLL, skipped
    when it is off) run the take cases again with a free pre-roll below,
    equal to, and above the wait, and at trigger latency equal to and
    above the input latency: the take starts the pre-roll earlier, keeps
    its length and phase, and restarts play its position P at the press.

testdef_command_latency_windows.py
    Tests for TRIGGER_LATENCY_ALL_EDGES's windows (increment (e); skipped
    when it is off): a late Record start, and a Trigger from Mute in the
    slack window, are decided from the press, trigger latency before the
    command's arrival, and the restart still plays in phase.

testdef_pause_from_mute.py
    Tests for PAUSE_FROM_MUTE_FIX: a quantized Pause pressed while Muted
    resolves to Paused, not Playing, with the position frozen, and an
    Undo from there resumes Muted; an unquantized Pause is unchanged.

testdef_eighth_cycle_runaway.py
    Tests for the clamp on eighths-per-cycle in Engine::set_tempo()
    (engine.cpp) when the tempo goes below 60 or above 240 BPM.  Needs
    neither JACK nor _test_engine.so: it checks the shape of the source
    text and simulates the rule over tempo ramps.


Not covered
-----------

Besides what `What the tests check, from the performer's side`_ lists:
the crossfade (``FadeSamples``) is left at 0, which plugin.cc treats as
1 sample, in every test but testdef_late_hit.py's late-take cases (64
and longer; a constant take must play back flat, so a misplaced fade
dips or overlays), testdef_layer_fades.py (64; no step larger than a
ramp's), and testdef_multiply_end.py (64; checks for silence only).  Elsewhere a fade that is missing, misplaced, or the wrong length
passes; only a fade level left at 0 or 1 shows, as lost or doubled
content.

The harness builds plugin.cc but not looper.cpp, so two late-hit details
that looper.cpp handles are untested here:

-  In internal sync, channels above 0 skip the late-hit correction
   (``bLateHitBypassed``, set by ``Looper::run()``; TestLooper never sets
   it).
-  A sync source change resets the late-hit state
   (``Looper::reset_late_hit_state()``, called from
   ``Engine::update_sync_source()``).
