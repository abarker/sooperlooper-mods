SooperLooper Late-Hit Recording Modification Summary
====================================================

| **Base version:** SooperLooper 1.7.9
| **Modified files:** ``src/plugin.cc``, ``src/plugin.hpp``, ``src/engine.cpp``,
| ``src/engine.hpp``, ``src/looper.cpp``, ``src/looper.hpp``, ``src/audio_driver.hpp``,
| ``src/jack_audio_driver.hpp``, ``src/jack_audio_driver.cpp``, ``src/event.hpp``,
| ``src/command_map.cpp``, and some test files in ``src/test_plugin``.
| **New files** (not modifications, so not listed above) are documented in
| their own sections: ``src/big_state_display/`` in `New Tool: slbigstate`_,
| new test files in `Test Suite`_.
| **All new code sections** are bracketed with ``albBegin`` / ``albEnd`` markers.
| In ``plugin.cc`` and ``plugin.hpp`` the markers are ``//====== albBegin`` /
| ``//====== albEnd``; in all other files they are ``//------ albBegin`` /
| ``//------ albEnd``. Search for ``albBegin`` to locate every changed block.

.. contents:: Table of Contents
   :local:
   :depth: 3

.. tip:: **Compiling on Ubuntu 24.04**

   The patched source is in the ``late-hit-and-jack-midi`` branch of
   https://github.com/abarker/sooperlooper-mods.  Run
   ``git switch late-hit-and-jack-midi`` to go to the branch.  A convenience
   script ``ubuntu_compile.bash`` at the top level of the repository automates
   the build.

   For a first build, set all four flags near the top of the script to
   ``true`` and run it::

       bash ubuntu_compile.bash

   On subsequent recompiles only ``compile_modified_fork=true`` is needed;
   the other three flags (``install_deps``, ``run_autogen``, ``run_config``)
   can remain ``false``.  The script prompts before running
   ``sudo make install``.

.. tip:: **Quick Start**

   The late-hit correction is active immediately after compiling — no session file
   changes are required. The compile-time defaults (100 ms cap; fraction cap disabled at 100.0) are
   used whenever the session file does not contain the late-hit keys.  Setting
   ``late_hit_max_quant_unit_fraction`` to a value like 0.5 caps the window at
   half the current quantize cycle (useful at faster tempos where the ms cap
   would otherwise allow a press that is more than half a cycle late).

   To change the values:

   1. **Run SooperLooper** with your usual setup and save a session file via
      *File → Save Session*, then exit.

   2. **Edit the session file** in a text editor. Find the ``<Globals>`` element
      and add the two late-hit keys at the end of its attribute list, before the
      closing ``/>``::

          late_hit_max_quant_unit_fraction="0.5" late_hit_window_ms="100"

      A complete ``<Globals>`` element looks like this, taken from a real
      session file (your other values will differ). This one also carries
      ``record_failsafe_seconds`` and ``mute_late_hit_ms`` -- the two other
      globals covered later in this document -- which this two-key walkthrough
      does not require you to add yourself::

          <Globals tempo="170" eighth_per_cycle="2" common_dry="0" common_wet="1"
           input_gain="1" sync_source="-1" auto_disable_latency="1"
           jack_timebase_master="0" output_midi_clock="0" use_midi_start="0"
           use_midi_stop="0" send_midi_start_on_trigger="0" smart_eighths="0"
           late_hit_window_ms="200" late_hit_max_quant_unit_fraction="0.5"
           record_failsafe_seconds="120.0" mute_late_hit_ms="100"/>

   3. **Restart SooperLooper** with the session file::

          sooperlooper --load-session yourfile.slsess

   These values (and two related ones, ``record_failsafe_seconds`` and
   ``mute_late_hit_ms``) can also be read and changed live over OSC instead
   of editing the session file -- see `OSC Control`_.


What This Patch Adds at a Glance
---------------------------------

A concise inventory of all significant capabilities added to SooperLooper
1.7.9, grouped by area.

.. rubric:: Late-Hit Timing Corrections

- **Late-hit Record start correction.** A Record press arriving within the
  configurable window after a sync boundary immediately creates the loop buffer,
  backfills *T* samples from the input ring buffer, and enters ``STATE_RECORD``
  directly — as if the button had been pressed on time.

- **Late-hit Record stop correction.** A Stop press within the window trims *T*
  samples from the loop length, advances ``dCurrPos`` to compensate for the
  late entry into ``STATE_PLAY``, and enters ``STATE_PLAY`` directly.

- **Late-hit Multiply end correction.** The same window logic applied to Multiply
  end; guarded by the ``LATE_HIT_MULTIPLY_CORRECTION`` compile flag (disable with
  value 0 if regressions appear).

- **Late-hit mute/unmute correction.** A ``MUTE``/``MUTE_ON``/``MUTE_OFF``
  press arriving within a separate, plain-ms window after a sync boundary
  is treated as on-time in either direction (entering mute or unmuting),
  instead of waiting for the next boundary. On by default (100 ms,
  matching the Record late-hit window).

- **Pause-from-Mute correction.** With mute-quantize on, a Pause press
  while Muted now resolves to Paused once its wait-for-sync boundary
  arrives -- matching the immediate (non-quantized) behavior already used
  elsewhere in the engine. Stock SooperLooper 1.7.9 resolves this to
  Playing instead. Guarded by the ``PAUSE_FROM_MUTE_FIX`` compile flag
  (default on).  See `Quantized Pause from Muted Now Resolves to Paused`_
  in the appendix.

- **Configurable two-cap window.** The correction fires only within
  ``min(g_late_hit_window_ms, g_late_hit_max_quant_unit_fraction × cycle_samples)``,
  hard-clamped to the input ring buffer depth.  Both values are saved in and loaded
  from the session file ``<Globals>`` node.

- **Late-hit correction robustness.** A slave channel in internal sync mode
  always uses the stock timing path (``bLateHitBypassed``), preventing L/R
  divergence from independently computed *T* values. Changing the sync
  source resets the correction's tracked state
  (``Looper::reset_late_hit_state()`` clears ``bSyncPulseReceived``,
  ``lGlobalSamplesSinceSync``, and ``lLastSyncPeriodSamples``) so stale
  period data from the old source can't affect the first presses on the
  new one.

.. rubric:: New Commands and Safety Features

- **New command** ``cancel_record_mute``.  Cancels a record that is in progress
  or still pending on a sync boundary, keeps the previous take, and is a no-op
  in every other state.  Lets one button, firing eagerly on the first press,
  carry ``record``, play, and then end play (go to mute) in sequence --
  without a latency penalty at any step.  See
  `New Command: cancel_record_mute`_.

- **Record failsafe.**  If a record that interrupted an actively playing,
  unmuted loop is left running in ``STATE_RECORD`` for longer than
  ``record_failsafe_seconds`` (default **0**, i.e. off), the engine
  automatically fires the same ``cancel_record_mute`` branch, as if its
  second trigger had arrived on its own.  Covers "nobody pressed anything
  at all" for that case, which ``cancel_record_mute`` cannot, since it still
  requires a press.  Does **not** apply to a first/fresh recording of an
  empty loop, or one started from mute or pause.  See
  `New Feature: record failsafe`_.

- **Four globals, one persistence path.** ``late_hit_window_ms``,
  ``late_hit_max_quant_unit_fraction``, ``record_failsafe_seconds``, and
  ``mute_late_hit_ms`` are all written and read in the ``<Globals>`` node of
  the ``.slsess`` session file, and all readable/settable live over OSC
  ``/get``/``/set``, in addition to the session file.  See `Record Late-Hit
  Window Size`_ and `OSC Control`_ below.

.. rubric:: JACK MIDI I/O

- **Direct JACK MIDI input port** (``sooperlooper:midi_in``).  Receives MIDI
  commands with sub-buffer sample-accurate timestamps, bypassing the ALSA /
  ``a2jmidid`` path.

- **Sub-buffer TriggerLatency correction for JACK MIDI events.**  The sample
  offset of each inbound JACK MIDI event (``jack_ev.time``) is stored as
  ``_pending_event_offset``; the following ``run()`` call zeros ``TriggerLatency``
  for that segment so no additional latency compensation is applied to an event
  that is already placed at its exact physical sample.

- **Direct JACK MIDI output port** (``sooperlooper:midi_out``).  Generates
  24-ppqn MIDI clock via a phase accumulator for sample-accurate tick placement,
  and sends MIDI Start / Stop on JACK Transport transitions.  Replaces the need
  for ``a2jmidid`` when driving external hardware from SooperLooper.

- **BBT-based JackSync pulse placement** (``ALB_BBT_SYNC``).
  The sync-pulse generator uses JACK BBT position fields supplied by the
  timebase master (``bar_start_tick``, ``beat``, ``bpm``, ``ticks_per_beat``)
  to anchor sync pulses to the master's musical grid.  Correctly handles
  locates to non-zero transport positions and mid-transport Eighths/Cycle
  changes.  Falls back automatically to the stock ``fmod(framepos,
  _tempo_frames)`` arithmetic when the timebase master does not supply BBT
  data, or when the compile flag is disabled.

  **Bug fixed (integer tick truncation):** the initial implementation used
  ``info.tick`` (the integer-truncated tick-within-beat field) to compute the
  phase of the current buffer relative to the cycle grid.  Because
  ``jack_position_t.tick`` is the floor of the true fractional tick, the
  accumulated integer tick count periodically lands on an exact multiple of
  ``ticks_per_cycle`` (960 for QUANT_8TH) even when the true fractional
  position is just past the boundary.  This caused a spurious second sync
  pulse — 4 samples after the legitimate boundary-crossing pulse — which
  produced double pulses that split loop recordings short.  The symptom was
  loop lengths differing from the expected value by multiples of the JACK
  buffer size (256 samples), with the magnitude and sign varying per recording.

  The fix replaces the tick-based phase formula with a frame-based one that
  uses only ``bar_start_tick`` (an exact integer) to compute
  ``bar_start_frame``, then derives a ``phase_shift`` that aligns
  ``fmod(frame_in_cycle, _tempo_frames)`` to the timebase master's bar grid.
  The ``tick`` field is never used.  This is algebraically equivalent to the
  stock ``fmod(framepos, _tempo_frames)`` formula for normal operation and
  correctly re-anchors to the master's grid after a locate.

.. rubric:: RT-Safety Fixes

- **RT-safety: suppressed** ``pthread_cond_signal`` **from RT callback**
  (``ALB_RT_SAFE_COND_SIGNAL``).  Three calls in ``generate_sync()`` that
  can cause priority inversion and xruns are suppressed; the main event loop's
  existing ``timedwait`` handles wakeup instead.

- **RT-safety: gated tempo update** (``ALB_TEMPO_ONLY_WHEN_ROLLING``).
  The JACK Transport tempo path in ``generate_sync()`` is skipped unless
  transport is actually rolling, preventing spurious non-RT-safe ``set_tempo()``
  calls during locate transitions.

.. rubric:: Tools

- ``slbigstate``: **a standalone at-a-distance state display.**  A
  separate program, installed alongside ``sooperlooper``/``slgui``, that
  shows one loop's current state in large, colored text over OSC --
  readable from across a room, unlike ``slgui``'s own small indicators.
  See `New Tool: slbigstate`_.

The Late-Hit Sync Problem
---------------------------

With sync mode enabled, SooperLooper corrects *early* button presses by waiting
for the next sync boundary before acting.  It does nothing for *late* presses —
a Record or Stop hit that arrives slightly after a sync boundary causes the
loop to start one full quantization unit late — the press is treated as
belonging to the *next* boundary, not the one just passed — and the loop length
is inflated by that same unit. This is a persistent source of timing errors in
live performance.

What the Patch Does
-------------------

When the per-loop **Sync checkbox is on** and a **sync source is selected and producing
pulses**, a Record or Stop press that arrives within a configurable window after a sync
boundary is corrected back to that boundary instead of waiting for the next one. Quantize
mode is not required. The default window is 100 ms; see `Record Late-Hit Window Size`_ below.

"Sync active" means both conditions together: the Sync checkbox controls whether
operations wait for boundaries at all (``fSyncMode > 0``); the sync source selection
determines what signal arrives on the sync input and therefore when
``lGlobalSamplesSinceSync`` resets. With no source, T grows without bound and the window
never fires regardless of the checkbox.

Late Record Start
~~~~~~~~~~~~~~~~~

The press would normally go to ``STATE_TRIG_START`` and wait for the next boundary,
producing a loop that starts one full cycle late. Instead:

1. The loop buffer is created immediately via ``pushNewLoopChunk()``.
2. The *T* samples of audio that elapsed since the boundary are copied ("backfilled")
   from the continuous input ring buffer into the start of the loop buffer at positions
   ``[0 .. T)``.
3. The record position ``dCurrPos`` is set to *T*, so live recording continues from
   position *T* onward, seamlessly after the backfilled audio.
4. The engine enters ``STATE_RECORD`` directly rather than ``STATE_TRIG_START``.

The result: the loop content starts at exactly the sync boundary as if the button had been
pressed on time.  Late starts are perfectly corrected.

Late Record Stop
~~~~~~~~~~~~~~~~

The press would normally go to ``STATE_TRIG_STOP`` and wait for the next boundary,
producing a loop that is *T* samples too long and whose tail drifts on every repeat.
Instead:

1. The loop length is trimmed by *T* samples: ``lLoopLength = dCurrPos − T``.
2. The play position ``dCurrPos`` is set to ``lLoopLength + lOutputLatency +
   lInputLatency + T`` — the same formula as the normal ``STATE_TRIG_STOP`` path, plus
   *T* to compensate for having entered ``STATE_PLAY`` *T* samples late in real time.
3. The engine enters ``STATE_PLAY`` directly.

Note that with the late-hit corrections, stop-hit accuracy is more important
than start-hit accuracy.  This is because a late stop results in an audible
delay in the start of the first loop (which is corrected on later loop
repetitions).  In a DAW, a performance with a late-stop error can be repaired
by carefully copying one of the correct loop plays to the first loop play.

After the first natural wrap, ``dCurrPos`` lands at ``lOutputLatency + lInputLatency`` —
identical to a normal on-time loop. No rewind or special first-play handling is needed.

Both Late (start and stop)
~~~~~~~~~~~~~~~~~~~~~~~~~~

The two corrections are applied independently in sequence and compose correctly. An on-
time stop after a late start, or a late stop after an on-time start, each works the same
way.

On-Time Presses and Disabled Modes
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

All new code paths are gated by ``withinWindow`` / ``stopWithinWindow`` /
``multStopInWindow``. These flags are false when the press is on time, when sync is off
(``fSyncMode == 0``), or when no sync pulse has arrived recently (T exceeds the window).
The engine behaves identically to the unpatched version in all these cases. Quantize mode
is no longer a condition.

New Command: ``cancel_record_mute``
------------------------------------

Cancels a record that is in progress or still pending, keeping the previous
take.  Mutes only if audio was actually captured.  **No-op in every other
state**, which is what makes it safe to overload onto a footswitch that also
fires ``record``.

Motivation: plain ``record`` already carries a loop from recording into
play on its own -- that part needs nothing new.  What ``cancel_record_mute``
adds is a clean way to stop that play again -- the same job ``mute_trigger``
would do -- without needing a second button.  An optional second
trigger on the same switch does the stopping: a long hold or a quick double
tap, always following the eager ``record`` that started it.  It always
fires immediately and *eagerly* on the first press, with no
wait-and-see delay: delaying to find out whether a second trigger will
follow would cost the very timing accuracy the whole late-hit correction
system exists to protect.  A long hold on the same switch is one natural
choice for that second, optional trigger (the pedal is already down); a
quick double tap works just as well -- the first tap firing the eager
``record`` as always, a second tap within a short window mapped to
``cancel_record_mute`` instead of a second ``record``.  The command itself
is indifferent to which gesture triggers it: all it needs is that an eager
``record`` has already landed.  A separate button bound to ``mute_trigger``
would do the same job, but costs a second pedal and is one more control to
consciously reach for under performance pressure, instead of an extension
of the gesture already in progress.  Internally the command works by
discarding the take just captured and restoring the previous loop muted --
that is implementation, not intent: the performer is not undoing a
mistake, they are choosing to stop play in favor of silence, on the same
pedal, in one motion.  Doing the retraction externally -- in whatever
software or hardware maps the pedal's MIDI -- is unreliable because that
layer cannot see SooperLooper's state: it cannot tell a committed record
from one still pending on a boundary, and cannot tell whether the loop was
already muted.  An in-engine command has ``state``, ``waitingForSync``,
``wasMuted`` and ``headLoopChunk``, and acts in a single realtime dispatch.

Behaviour by state at the moment the command arrives:

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - State
     - Action
   * - ``STATE_RECORD``
     - Discard the take, restore the previous loop, end **muted**.
   * - ``STATE_TRIG_START``
     - Cancel the pending record; the loop **keeps playing** (nothing was
       captured).  Cancelled in place -- see the note below.
   * - ``STATE_TRIG_STOP``
     - **No-op.**  A quantized record *end* is waiting on its boundary; the
       take is being kept.
   * - everything else
     - **No-op.**

.. important::

   The two pending states must be distinguished, and the guard must key on
   ``STATE_TRIG_START`` rather than ``waitingForSync`` -- that flag is also
   set for deferred *mute* and *play* transitions.  A ``state ==
   STATE_RECORD`` guard alone silently does nothing in the quantized case
   the feature exists for; a loose "anything record-ish" guard destroys a
   take in the case it is supposed to tolerate.

Implementation details -- the ``lMultiCtrl`` remap, the files touched, and
the enum-ordering hazard -- are in `cancel_record_mute Implementation`_
in the appendix.

New Feature: record failsafe
-----------------------------------

If a record is left running in ``STATE_RECORD`` for longer than
``record_failsafe_seconds``, the engine treats it exactly as if
``cancel_record_mute``'s second trigger had just arrived: discard the take,
restore the previous loop, end muted.  **Not a new command** -- it is a
second, automatic way to reach the same ``CANCEL_RECORD_MUTE`` branch
described above, so it inherits that command's guard table and
fade-residue fix unchanged.

Motivation: ``cancel_record_mute`` still requires a second command trigger,
after the eager ``record`` -- a long hold or a double tap, whichever gesture
the pedal is set up to send it as.  It does nothing if a record is simply
left running because attention went elsewhere entirely, with no second
trigger at all, not even a fumbled one.  The record state itself has no
automatic exit.

**Scope: only fires for a record that interrupted an
actively playing, unmuted loop.** It does *not* fire for a first/fresh
recording of an empty loop, and does not fire for a record started from
``STATE_MUTE`` or ``STATE_PAUSED``. Precisely: ``bRecordEnteredFromActivePlay``
(a field on ``SooperLooperI``) is set once, at the moment ``RECORD`` is
pressed, to ``(state == STATE_PLAY || state == STATE_ONESHOT)`` -- true only
when an actively playing, audible loop existed and was unmuted at that
instant. Rationale: the case actually worth an automatic safety net is the
eager-single/second-trigger ambiguity ``cancel_record_mute`` itself is built
around -- a tap fires a real ``record`` immediately, and only a second
trigger (long hold or double tap) retracts it, so a performer who meant to
send that second trigger (or who gets distracted before resolving it
either way) can silently leave an
*existing, audible* take replaced by whatever's being recorded over it. A
first/fresh recording from an empty loop has no equivalent "meant to mute
instead" story and no prior take at stake, so it is left to run
indefinitely, same as before this feature existed.

**Known trade-off:** if the performer genuinely did mean to start a brand
new take right after ending a loop's playback -- not an accidental eager
single -- that new take's available record time is now bounded by
``record_failsafe_seconds``, the same constraint the feature applies to
every other take entering from an active, unmuted loop.  The engine cannot
tell "meant to record" from "meant to trigger mute instead" apart at the
moment the press lands, only from what happens afterward.

Implementation: a sample counter, ``lRecordFailsafeSamples`` (a field on
``SooperLooperI``), accumulates ``SampleCount`` once per buffer while
``state == STATE_RECORD`` **and** ``bRecordEnteredFromActivePlay`` is true,
and resets to 0 the instant state is anything else -- so it measures
unbroken time in that one state, only for a take that qualifies, and never
leaks across separate takes. A flag, ``bRecordFailsafeFired``, is true when
that counter exceeds ``g_record_failsafe_seconds * fSampleRate`` and is ORed into
the existing ``lMultiCtrl == MULTI_CANCEL_RECORD_MUTE`` branch condition in
``plugin.cc`` -- zero new state-transition logic. Deliberately **not**
gated on ``STATE_TRIG_STOP``: that state is a brief, self-resolving wait
for the next quantize boundary (at most one loop cycle), unlike
``STATE_RECORD``, which has no automatic exit at all.

**Default is off** (``RECORD_FAILSAFE_SECONDS_DEFAULT`` = ``0.0f``), unlike
the late-hit window's default-on convention above. Reason: this feature can
discard an entire take with *no press at all*, so it ships opt-in.
Any value at or below zero disables it, matching the late-hit window's own
"0 disables" convention. To enable it, set ``record_failsafe_seconds`` to a positive
value (seconds) either in the session file's ``<Globals>`` node or live via
OSC -- see `OSC Control`_ below, which now applies to this value too.

Same non-LADSPA-port mechanism as ``g_late_hit_window_ms``, for the same
reasons (see `Why Not a LADSPA Port`_ below): a plain ``extern`` global,
saved/loaded as the ``record_failsafe_seconds`` property in the
``<Globals>`` node by ``engine.cpp``'s ``save_session``/``load_session``.

Files: ``plugin.hpp`` (``lRecordFailsafeSamples`` and
``bRecordEnteredFromActivePlay`` fields), ``plugin.cc``
(``RECORD_FAILSAFE_SECONDS_DEFAULT`` / ``g_record_failsafe_seconds``
globals, the ``bRecordEnteredFromActivePlay`` set statement at the top of
``MULTI_RECORD``, the counter-update statements, the
``|| bRecordFailsafeFired`` addition), ``engine.cpp`` (``extern``
declaration, XML load/save, OSC get/set, startup banner). No new
``Event::command_t``, no ``command_map.cpp`` entry, and no pedal/MIDI
mapping -- purely internal to the engine, invisible to any external MIDI
mapping and the ``.slb`` bindings.

New Feature: mute/unmute late-hit correction
-----------------------------------------------

Extends the late-hit idea to ``MUTE``/``MUTE_ON``/``MUTE_OFF``: with
mute-quantize on and sync/quantize active, a press arriving just after a
sync boundary is treated as if it arrived exactly at that boundary,
instead of waiting for the *next* one (up to a full quantize cycle
later) -- in **both** directions, entering mute and leaving it (unmute).

Unlike Record's correction, neither direction reconstructs buffer
content: ``dCurrPos`` keeps advancing every buffer while muted (the
loop's content is already sitting at the correct phase throughout), so
this is a pure branch-selection decision -- immediate vs. wait -- not a
backfill/trim operation.

**Governed by its own window** — ``mute_late_hit_ms`` (milliseconds,
default ``100.0`` -- **on**, matching ``late_hit_window_ms``'s own
default exactly). Deliberately a plain millisecond value with
no cycle-fraction cap, unlike ``late_hit_window_ms`` above: the
fraction cap exists there to keep a late *Record* press unambiguously
attributed to one boundary before backfilling/trimming real audio --
mute/unmute never touches buffer content, so that concern does not
apply, and an over-generous value only ever degrades gracefully
(mute-quantize effectively bypassed at very fast tempos, never
mis-corrected). A value of zero disables the feature entirely, matching the
"0 disables" convention used throughout this patch. Independent of
``late_hit_window_ms`` and ``record_failsafe_seconds`` -- can be
turned on or off, or tuned, without affecting either. Defaults on
because, unlike record failsafe (which can discard an entire take
with no press at all), this feature can only ever turn a "wait for the
next boundary" into "act now" -- never lose content -- the same
default-on reasoning ``late_hit_window_ms`` itself already uses.

To disable, or to change the window, set ``mute_late_hit_ms`` (``0`` for
off, any other value for the window in milliseconds) either in the
session file's ``<Globals>`` node or live via OSC -- see `OSC Control`_
below, which now covers this value too.

Same non-LADSPA-port mechanism as the other three globals (see `Why Not
a LADSPA Port`_ below): a plain ``extern`` global, saved/loaded as the
``mute_late_hit_ms`` property in the ``<Globals>`` node by
``engine.cpp``'s ``save_session``/``load_session``.

Files: ``plugin.cc`` (``MUTE_LATE_HIT_MS_DEFAULT``/``g_mute_late_hit_ms``
globals, the shared window check at the top of the ``MULTI_MUTE``/
``MULTI_PAUSE`` case, one added disjunct in the unmute branch and one in
the entering-mute branch), ``engine.cpp`` (``extern`` declaration, XML
load/save, OSC get/set). No new ``Event::command_t``, no
``command_map.cpp`` entry, and no pedal/MIDI remapping.

Record Late-Hit Window Size
----------------------------

Covers the Record late-hit window (``late_hit_window_ms``/
``late_hit_max_quant_unit_fraction``) specifically. The mute/unmute
window, ``mute_late_hit_ms`` (above), is a separate, single plain-ms
value with no fraction cap -- see its own section for why.

The window size has two independent limits; the smaller of the two applies.

**Millisecond cap** — ``late_hit_window_ms`` (default 100 ms). This is an absolute
ceiling on how late a press can be and still qualify for correction. Its value is saved in
and loaded from the session file (``.slsess``) under the key ``late_hit_window_ms`` in
the ``<Globals>`` node — the same node that holds ``tempo``, ``eighth_per_cycle``, and
other engine-level settings. To change it, edit the session file in a text editor and
restart the engine. If the key is absent (older session files), the compile-time default
``LATE_HIT_WINDOW_MS`` (100 ms) is used.

**Fraction cap** — ``late_hit_max_quant_unit_fraction`` (default 100.0,
which effectively disables the cap so the ms limit always governs).
When set to a value less than 1.0, this limits the window to that fraction of
one full quantize cycle at the current tempo and Eighths/Cycle setting, so the
correction window scales automatically rather than being a fixed duration.
For example, 0.25 caps the window at 25 % of one cycle regardless of tempo.
At slow tempos the ms cap is usually the binding limit; the fraction cap
becomes the effective limit at faster tempos or larger cycle sizes where the
cycle period falls below ``late_hit_window_ms``. Its value is stored in the
session file under the key ``late_hit_max_quant_unit_fraction`` in the
``<Globals>`` node; if absent, the compile-time default
``LATE_HIT_MAX_QUANT_UNIT_FRACTION`` (100.0) is used.

Both limits are evaluated each buffer by ``effectiveLateHitWindow()`` in
``plugin.cc``, which returns the smaller value in samples. The ms cap is additionally clamped to
``lInputBufSize − 1`` (32767 samples, approximately 682 ms at 48 kHz), ensuring the
window never exceeds the depth of the input ring buffer regardless of what
``late_hit_window_ms`` is set to. Setting the ms cap above this ceiling is silently
clamped to it; one practical use is to set ``late_hit_window_ms`` to a large value
so that the fraction cap alone governs the effective window at all normal playing
tempos (set ``late_hit_max_quant_unit_fraction`` to the desired cycle fraction and
leave the ms cap at something like 2000).

OSC Control
~~~~~~~~~~~

All four globals -- ``late_hit_window_ms``, ``late_hit_max_quant_unit_fraction``,
``record_failsafe_seconds`` (see `New Feature: record failsafe`_ above),
and ``mute_late_hit_ms`` (see `New Feature: mute/unmute late-hit
correction`_ above) -- can all be read and changed live over OSC, in
addition to editing the session file. They are reachable through the
engine's existing generic global parameter mechanism -- the same
``/set "sf"`` / ``/get "sss"`` methods already used for ``tempo``,
``eighth_per_cycle``, ``smart_eighths``, and other ``<Globals>`` values:

.. code-block::

    /set  s:param  f:value                     -- no reply
    /get  s:param  s:return_url  s:return_path  -- replies to return_path
                                                    with  i:-2  s:param  f:value

For example, to read and then set the record failsafe over OSC from a
client at ``osc.udp://127.0.0.1:9999/`` listening on ``/reply``::

    /get "record_failsafe_seconds" "osc.udp://127.0.0.1:9999/" "/reply"
    /set "record_failsafe_seconds" 45.0

A negative value sent to ``/set`` for any of the four is silently ignored
(the value is left unchanged) -- the same ``value >= 0.0f`` guard
``load_session`` already applies to the session-file keys, needed because
a negative float would otherwise reach an ``(unsigned long)`` cast in
``plugin.cc``'s threshold checks.

This is a smaller step than full OSC/GUI exposure: no ``command_map.cpp``
``add_global_control`` registration and no LADSPA port were added for any
of the four (see `Why Not a LADSPA Port`_ below), so none of them appear
in ``slgui``, and OSC's ``/register_auto_update`` push-on-change mechanism
-- which keys a listener's registration off ``CommandMap::to_control_t()``
-- will not target them by name. Plain ``/set``/``/get`` are unaffected by
that limitation.

JACK MIDI Output and Improved Timing
--------------------------------------

Direct JACK MIDI Port
~~~~~~~~~~~~~~~~~~~~~

A new direct JACK MIDI output port (``midi_out``) is registered in ``engine.cpp``. It
generates 24-ppqn MIDI clock continuously when a tempo is known, and sends MIDI Start
(``0xFA``) and MIDI Stop (``0xFC``) on JACK Transport transitions. This replaces the need
for an ALSA-to-JACK MIDI bridge (``a2jmidid``) for driving external hardware from
SooperLooper.

A corresponding JACK MIDI input port (``midi_in``) receives commands directly from
JACK MIDI controllers, with a key advantage over ALSA MIDI described below.

Sub-Buffer Timestamping and TriggerLatency
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

JACK MIDI events carry an exact sample offset within the current buffer
(``jack_ev.time``). The engine uses this offset to split the processing buffer at the
event boundary, running audio up to the event position before applying the command. This
is the standard JACK buffer-splitting technique for sample-accurate event handling.

For the late-hit patch specifically, a JACK MIDI Record press arriving at sample *N*
within a buffer is processed with ``_pending_event_offset = N``. The ``run()``
call that follows sees this non-zero offset and zeros ``TriggerLatency`` for that segment,
because the event is already placed at the exact physical sample — no additional latency
correction is needed. ALSA MIDI events, which carry no sub-buffer position, always have
``_pending_event_offset = 0`` and use the full ``TriggerLatency`` correction unchanged.
The result is that JACK MIDI controller presses are handled with higher timing accuracy
than ALSA MIDI.


BBT-Based JackSync Pulse Placement
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

When ``ALB_BBT_SYNC`` is defined (the default), ``Engine::generate_sync()``
uses JACK BBT position fields from the timebase master to determine where sync
pulses fall within each buffer, rather than computing position as
``fmod(framepos, _tempo_frames)`` anchored to JACK frame 0.

The stock formula breaks in two real scenarios.  First, after a transport
locate to a non-zero position: ``fmod(new_pos, _tempo_frames)`` gives a
non-zero phase offset unless the locate target happens to be an exact multiple
of ``_tempo_frames`` from frame 0, which it generally is not.  Second, after a
mid-transport Eighths/Cycle change: ``_tempo_frames`` updates immediately, but
``fmod(framepos, new_tempo_frames)`` then anchors to a different grid from what
the timebase master uses.  Both problems are avoided in practice by always
locating to position 0 before recording, but the BBT path makes the code
correct unconditionally.

Five additional fields are now populated in ``TransportInfo`` by
``JackAudioDriver::get_transport_info()`` when the timebase master has set
the ``JackPositionBBT`` valid bit: ``bar``, ``beat``, ``bar_start_tick``,
``ticks_per_beat``, and the ``bbt_valid`` flag.  (``tick`` is also copied
but is not used by the formula; see the bug-fix note in
`What This Patch Adds at a Glance`_.)
If ``bbt_valid`` is true, ``generate_sync()`` computes:

.. code-block::

    bar_start_frame = bar_start_tick × (60 × SR / bpm) / ticks_per_beat
    phase_shift     = bar_start_frame − fmod(bar_start_frame, _tempo_frames)
    frame_in_cycle  = (framepos + offset) − phase_shift
    thisval         = lrint(fmod(frame_in_cycle, _tempo_frames))

Because ``bar_start_tick`` is an exact integer supplied by the timebase
master, ``bar_start_frame`` is computed without rounding through an
integer tick accumulator.  The ``phase_shift`` value is the frame of the
most-recent cycle boundary on or before ``bar_start_frame``, which aligns
the ``fmod`` grid to the
master's bar grid.  The result is numerically equivalent to the stock
``fmod(framepos, _tempo_frames)`` formula when starting from the transport
origin, and correctly re-anchors after any locate.

Because ``_tempo_frames`` already encodes the correct cycle length for the
current quantize mode (QUANT_8TH or QUANT_CYCLE), the formula is correct
for both modes without any quantize-mode branching.

The ``beat_type == 4`` guard limits the BBT path to quarter-note beat signatures
(the universal case for Hydrogen and essentially all JACK timebase masters).
Any other denominator falls through to the stock ``fmod`` path rather than
computing wrongly.  PipeWire's JACK compatibility layer implements the full BBT
API; no special handling is needed.

Commenting out ``#define ALB_BBT_SYNC`` in ``audio_driver.hpp`` removes the BBT
fields from ``TransportInfo`` entirely and restores verbatim stock
``generate_sync()`` behaviour.

New Tool: ``slbigstate``
--------------------------

A standalone, at-a-distance display of one or more loops' current state
(Record / Playing / Overdub / Mute / ...) in large, high-contrast text
with a per-state background color -- for visibility from across a room
during a performance, where the normal ``slgui`` panel's small state
indicators are too small to read. A separate program from ``slgui``, not
a window inside it.

Built and installed the same way as ``sooperlooper``/``slgui`` -- it is
part of the same ``configure``/``make``/``make install`` this patch
already uses, landing in the same ``bindir`` (e.g. ``/usr/local/bin``)
once installed. Source lives in ``src/big_state_display/``.

Usage::

    slbigstate [options]

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Option
     - Meaning
   * - ``-H``, ``--host``
     - Engine host to connect to (default ``127.0.0.1``).
   * - ``-P``, ``--port``
     - Engine OSC port to connect to (default ``9951``).
   * - ``-l``, ``--loop``
     - Show exactly one loop, the original single-window mode. Mutually
       exclusive with ``-L``/``--loops``.
   * - ``-L``, ``--loops``
     - Comma/range list of loops to stack in one window, e.g.
       ``0,2-4``, in the order given. If neither ``-l`` nor ``-L`` is
       given, all of the engine's loops are shown, auto-detected --
       see *Multiple loops* below.
   * - ``-X``, ``--screen-location``
     - Window position, format ``<xpos>:<ypos>``.
   * - ``-Z``, ``--size``
     - Per-row window size, format ``<width>x<height>`` -- the size one
       loop's row takes; the whole window's height is this times the
       number of loops shown.
   * - ``-T``, ``--stay-on-top``
     - Keep the window above other applications.
   * - ``-M``, ``--margin``
     - Pixels reserved on every side of the label when fitting the
       font (default 20). Turn this down for text that sits closer to
       each row's edges, or up for more breathing room.

The window title includes which loop(s) are shown -- "SooperLooper
State - Loop 0" for a single loop, "SooperLooper State - Loops 0,2,3"
for a stack -- since more than one instance can be running at once.

**Text size adjusts to fit.** The label font grows or shrinks to fill
whatever labels are actually showing right now (BigStateWindow's own
current one; every row's current one for a stack), recalculated
whenever the window is resized or any row's state changes. Sizing is
still one shared computation across every row of a stack at a given
moment, so rows never disagree with each other on size -- it is just
no longer padded out for hypothetical labels nothing on screen is
currently showing (the longest possible one is "Substitute," which
would otherwise leave a short label like "Off" using only a small
fraction of the available space). The trade-off: the font can now
change size when a row's own state changes to a longer or shorter
word, where an earlier version held it perfectly still across every
state change.

**Multiple loops: one window, stacked, by default.** Running
``slbigstate`` with no ``-l``/``-L`` at all shows *every* loop the
engine currently has, one row per loop, stacked top to bottom in loop
order, each row exactly the size a single-loop window would be (same
``--size``/default) -- a single-loop rig therefore gets a window
pixel-identical to the original single-loop-only display, with no
flags needed. A thin black divider line separates adjacent rows, so
two loops in the same state (both Playing, say) don't read as one
undivided block. To show a specific subset, in a specific order, use
``-L``, e.g. ``slbigstate -L 2,0-1`` shows loop 2, then loops 0 and 1,
top to bottom in that order. To force the original single-loop-only
window for exactly one loop (skipping the auto-detect), use
``-l N`` instead -- both remain available; ``-l`` and ``-L`` cannot be
combined.

**How it talks to the engine:** plain OSC, the same generic mechanism
``tempo``/``smart_eighths``/etc. already use elsewhere in this patch.
For each loop shown, it sends one ``/sl/<loop>/register_auto_update``
request for that loop's ``state`` control; the engine then pushes an
update whenever that value actually changes (not on a fixed schedule),
and each loop's registration is refreshed defensively every 20 seconds
so the display recovers on its own if the engine restarts *with the
same loop count* -- N loops means N independent registrations sharing
one process and one window, not N separate processes.

**If the loop count actually changes** -- a loop added or removed live
from within a running engine (SooperLooper's own ``/loop_add``/
``/loop_del``, e.g. from ``slgui`` itself), *or* the engine coming back
from a restart with a different count already set from its own
startup -- the *auto-detect* stack window (no ``-l``/``-L`` given, the
default) notices and rebuilds itself within a few seconds, tearing
down and rebuilding its rows to match, in either direction, without
needing to be restarted itself. Covers both cases because it uses two
mechanisms together: the engine's own config-changed push (immediate,
for a live add/remove on an engine it is already watching) plus a
periodic active re-check (catches a *new* engine instance that already
had a different count from the moment it started, which a passive push
alone cannot -- nothing "changes" for it to push if the count was
already different before anyone was watching). An explicit ``-l``/
``-L`` window deliberately does not do any of this -- you asked for a
specific loop or set, so a count change elsewhere leaves it alone; if a
loop *you explicitly chose* is itself removed, that row just freezes at
its last known state, same as a plain engine restart with the same
loop count but different content.
When no ``-l``/``-L`` is given, one extra,
one-shot exchange happens first, before any window opens: a plain
``/ping`` to ask the engine how many loops it currently has (the same
reply -- host, version, loop count -- ``slgui`` itself already reads
for its own reconnect handling), so the display knows how many rows to
build; a 2-second timeout with no reply falls back to showing loop 0
alone rather than failing to start. No connection to ``libslcore.a`` or
any of this patch's engine-side code either way -- it is an
independent client of the engine's existing OSC interface, and works
identically against a GUI-managed engine (started by ``slgui``) or a
standalone headless ``sooperlooper``.

**Remembering its window position:** its own preferences file,
``~/.sooperlooper/slbigstate_config.xml`` -- separate from ``slgui``'s
own ``~/.sooperlooper/gui_config.xml`` (same directory, different file:
only the *file* needs to be exclusive to avoid the two independent
processes' writes colliding, not the whole directory). A stack of two
or more loops uses a second file,
``~/.sooperlooper/slbigstate_stack_config.xml``, so its remembered
position doesn't collide with a single-loop instance's -- but showing
exactly one loop, whether via ``-l`` or a ``-L``/auto-detect result
that resolves to just one, always uses the original single-loop file,
so a one-loop rig's remembered window position carries over exactly as
it was before this feature existed.


Compile-Time Flags
------------------

All flags are collected here for reference.  Each is also documented at its
definition site.  Flags in ``plugin.cc`` are purely local to that translation
unit.  Two flags are defined in headers rather than the ``.cpp``/``.cc``
files they primarily affect: ``ALB_BBT_SYNC`` in ``audio_driver.hpp``,
because it controls struct layout visible to multiple translation units,
and ``ALB_SYNC_PULSE_LOG`` in ``engine.hpp``, for the same reason (it adds
a member to ``Engine``). ``ALB_TEMPO_ONLY_WHEN_ROLLING`` and
``ALB_RT_SAFE_COND_SIGNAL`` are local to ``engine.cpp``.

.. list-table::
   :header-rows: 1
   :widths: 35 20 15 30

   * - Flag
     - File
     - Default
     - Effect
   * - ``LATE_HIT_WINDOW_MS``
     - ``plugin.cc``
     - ``100.0f``
     - Compile-time initial value for ``g_late_hit_window_ms`` (ms cap).
       Overridden at runtime by the session file or OSC ``/set``.  Do not
       use directly in logic; read ``g_late_hit_window_ms`` instead.
   * - ``LATE_HIT_MAX_QUANT_UNIT_FRACTION``
     - ``plugin.cc``
     - ``100.0f``
     - Compile-time initial value for ``g_late_hit_max_quant_unit_fraction``
       (fraction cap).  100.0 effectively disables the fraction cap so the
       ms cap always governs.  Overridden at runtime by the session file or
       OSC ``/set``.
   * - ``RECORD_FAILSAFE_SECONDS_DEFAULT``
     - ``plugin.cc``
     - ``0.0f``
     - Compile-time initial value for ``g_record_failsafe_seconds``.  ``0``
       means the failsafe ships **off**; a record left running in
       ``STATE_RECORD`` past this many seconds auto-fires
       ``cancel_record_mute``'s branch (see `New Feature: record failsafe`_
       above).  Overridden at runtime by the session file or OSC ``/set``.
   * - ``LATE_HIT_MULTIPLY_CORRECTION``
     - ``plugin.cc``
     - ``1``
     - When 1, the late-hit correction is applied to Multiply end
       (``multStopInWindow`` path).  Set to 0 to disable Multiply-end
       correction only, leaving Record start/stop correction active.
   * - ``MUTE_LATE_HIT_MS_DEFAULT``
     - ``plugin.cc``
     - ``100.0f``
     - Compile-time initial value for ``g_mute_late_hit_ms``.  On by
       default, matching ``LATE_HIT_WINDOW_MS`` exactly (see `New
       Feature: mute/unmute late-hit correction`_ above for why this
       defaults on rather than off, unlike record failsafe).  ``0``
       disables the correction entirely.  Overridden at runtime by the
       session file or OSC ``/set``.
   * - ``PAUSE_FROM_MUTE_FIX``
     - ``plugin.cc``
     - ``1``
     - When 1 (default), a stock 1.7.9 bug fix is active: quantized
       Pause from Muted correctly resolves to Paused instead of
       Playing (see `Quantized Pause from Muted Now Resolves to
       Paused`_ in the appendix, below).  Set to 0 to revert to exact stock (buggy)
       behaviour.  Unlike the other flags/globals here, this is a bug
       fix with an escape hatch, not a tunable parameter -- no
       session-file or OSC equivalent.
   * - ``SYNCWDEBUG``
     - ``plugin.cc``
     - *(commented out)*
     - Master switch for ALL debug output.  When defined: a sentinel
       (``sl_loaded.txt``) is written at binary-load time; a debug log
       (``sl_debug.log``) is created and ``SL_LOG`` startup banners are
       written to it; ``SYNCWDBG()`` / ``SL_LOG_RT()`` per-press messages are
       written with an ``fflush()`` after every call.  Both files land in
       ``$HOME/.sooperlooper/logs/`` (created if it does not already exist --
       SooperLooper's own per-user directory, same one ``ALB_SYNC_PULSE_LOG``
       below uses), falling back to ``/tmp`` if ``$HOME`` is unset or that
       directory can't be created; the debug log falls back further to
       ``stderr`` as a last resort.  When **not** defined (default): no files
       are created at all; ``SL_LOG`` and ``SL_LOG_RT`` expand to nothing;
       ``SYNCWDBG(...)`` expands to nothing; the only unconditional output is
       the ``fprintf(stderr, ...)`` banner in ``engine.cpp`` after session load.
       Must be commented out in any production / performance build.  When
       enabled, the ``fflush()`` in ``SL_LOG_RT`` causes an immediate kernel
       ``write()`` on every button press, which is not RT-safe and will cause
       xruns.
   * - ``ALB_RT_SAFE_COND_SIGNAL``
     - ``engine.cpp``
     - defined
     - **When defined (default):** three ``pthread_cond_signal()`` calls in
       ``generate_sync()`` are suppressed; the main event loop wakes via its
       existing ``timedwait`` timeout instead.  Reduces xruns on transport
       locate events.  **When not defined:** stock behaviour — signals fire
       from the RT callback.
   * - ``ALB_TEMPO_ONLY_WHEN_ROLLING``
     - ``engine.cpp``
     - defined
     - **When defined (default):** tempo is updated from JACK Transport only
       while rolling; during locate transitions the existing ``_tempo`` is
       preserved until rolling resumes, preventing non-RT-safe ``set_tempo()``
       calls from firing on every buffer during a locate.  **When not defined:**
       stock behaviour — tempo updated on every cycle regardless of state.
   * - ``ALB_BBT_SYNC``
     - ``audio_driver.hpp``
     - defined
     - **When defined (default):** adds BBT fields to ``TransportInfo``;
       ``generate_sync()`` uses them to anchor JackSync pulses to the timebase
       master's musical grid, with automatic runtime fallback to
       ``fmod(framepos)`` when BBT data is unavailable.  **When not defined:**
       BBT fields are absent from ``TransportInfo`` and ``generate_sync()``
       uses the stock ``fmod(framepos, _tempo_frames)`` arithmetic in all
       cases — identical to stock SooperLooper 1.7.9 JackSync behaviour.
   * - ``ALB_SYNC_PULSE_LOG``
     - ``engine.hpp``
     - *(commented out)*
     - Diagnostic for the open eighth-note off-beat sync investigation
       (ROADMAP entry 8), in the same spirit as ``SYNCWDEBUG`` above --
       a maintainer's debugging aid, not something a general build should
       enable by default.  **When defined:** logs one line per actual
       JackSync cycle-boundary pulse (frame, beat/tick/bpm, which formula
       path fired) to ``$HOME/.sooperlooper/logs/sl_sync_pulse.log``
       (created if needed, falling back to ``/tmp`` only if ``$HOME`` is
       unset).  RT side only pushes a POD through a lock-free
       ``RingBuffer``, same pattern as ``_sync_queue``; the actual file
       write happens on the non-RT ``mainloop()`` thread, never the RT
       callback -- so unlike ``SYNCWDEBUG`` this carries no real RT-safety
       cost even when enabled, but it still defaults off, since the
       reason to keep it off is who it's for, not what it costs.
       **When not defined (default):** none of this struct/queue/file
       exists; nothing is compiled in.

Appendix: Minor Fixes and Implementation Details
--------------------------------------------------

Smaller corrections, design notes, and implementation-level detail that
don't carry the same weight as the main narrative above -- collected here
so they don't crowd the sections a first-time reader actually needs.

``cancel_record_mute`` Implementation
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A state-based remap in ``plugin.cc`` (the same idiom as ``MULTI_MUTE_ON``),
rewriting ``lMultiCtrl`` rather than adding a dispatch case:

- ``STATE_RECORD`` -> set ``wasMuted``, set the four mute fade deltas, remap
  to ``MULTI_UNDO``.  The existing undo body then lands in ``STATE_MUTE``.
- ``STATE_TRIG_START`` -> cancel in place (clear ``waitingForSync`` /
  ``nextState``, restore state from ``headLoopChunk``) and set
  ``lMultiCtrl = -1``.  It must **not** remap to ``MULTI_UNDO``: no chunk is
  pushed on the pending path (``pushNewLoopChunk`` lives in the late-hit
  branch only), so the undo would pop the existing take.
- anything else -> ``lMultiCtrl = -1``.

The fade deltas mirror ``transitionToNext``'s ``STATE_MUTE`` case, including
the ``*Src*`` pair.  Setting only the non-Src pair leaves stale source-loop
gains that corrupt a later ``MULTIPLY`` (``beginMultiply`` sets no fade
deltas of its own; ``beginOverdub`` does, which is why overdub is immune).

Files: ``event.hpp`` (enum appended before ``LAST_COMMAND``),
``command_map.cpp`` (one line -- OSC and ``.slb`` bindings follow
automatically), ``plugin.cc`` (``#define``, drift ``static_assert``\ s, the
remap branch).

.. warning::

   The enum **must** be appended at the end.  Elsewhere, ``plugin.cc``
   hardcodes ``MULTI_UNDO``..``MULTI_PAUSE_OFF`` as integer literals 0-23 mirroring the
   first 24 ``Event::command_t`` values; inserting inside that range shifts
   the enum but not the literals, silently remapping commands **with no
   compile error**.  Six ``static_assert``\ s now guard this.


Quantized Pause from Muted Now Resolves to Paused
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**A stock SooperLooper 1.7.9 bug, not introduced by this fork** --
confirmed present verbatim in the untouched upstream reference copy
kept in this tree. Found by accident while testing the mute/unmute
late-hit feature earlier in this document, then fixed on its own merits since it is
unrelated to late-hit correction.

**The bug:** with mute-quantize on and sync active, pressing Pause while
a loop is Muted has to wait for the next sync boundary before acting --
the same "wait for sync" mechanism mute-quantize always uses. Once that
boundary arrived, the loop ended up **Playing**, not **Paused**. The
performer pressed Pause and got audible playback instead.

**Why this was confidently a bug, not intended behaviour:** SooperLooper
already handles this exact transition correctly when it acts
*immediately* (mute-quantize off, or sync/quantize off) -- pressing
Pause while Muted goes straight to Paused, explicitly, and the reverse
(pressing Mute while Paused) goes straight back to Muted, also
explicitly. Both directions, named in the code, deliberate. The
*deferred* "wait for sync" version of the same logic only implemented
one of those two symmetric directions; the other silently fell through
to the same generic branch used for an ordinary unmute. Quantization is
only supposed to delay *when* an action happens, never change *what*
the action resolves to -- a deferred press and an immediate press
pressing the same button in the same state should always end up in the
same place, just possibly later. Implementing only one of two mirrored
cases the code already implements immediately above is the signature of
an omission made when that deferred branch was originally written, not
a deliberate design choice.

**The fix has two parts.** The obvious one -- adding the missing case so
a deferred Mute->Pause resolves to Paused -- turned out to have a
non-obvious second half: the engine also tracks whether a loop *was*
muted before it changed state, so that a later Undo can correctly put it
back in Mute rather than Play. That tracking needed fixing too, or the
primary fix alone would have landed in the right state but with an Undo
fired later resuming to the wrong place. Both parts sit inside the same
generic function the engine already uses for every other deferred state
transition, so the fix is small and self-contained -- about four lines
total, not counting the safety switch below.

**Guarded by a compile-time flag, off if wanted.** ``PAUSE_FROM_MUTE_FIX``
(``plugin.cc``, default ``1``) wraps both parts, matching
``LATE_HIT_MULTIPLY_CORRECTION``'s own established role in this file: a
correction with its own independent escape hatch, in case it ever needs
pulling without touching anything else. Set it to ``0`` and rebuild to
revert to exact stock (buggy) behaviour. This is compile-time, not a
session-file/OSC value like the four late-hit globals -- there is nothing to
tune, only a bug to have fixed or not, so a rebuild-time switch fits
better than a runtime one.

Why Not a LADSPA Port
~~~~~~~~~~~~~~~~~~~~~

Every other engine parameter travels through the LADSPA port system: ``looper.cpp``
allocates a float, calls ``connect_port()``, and ``plugin.cc`` reads it via a pointer on
``SooperLooperI``. That is the correct mechanism for a value that needs full GUI exposure
(a slider in ``slgui``) or push-on-change OSC updates.

The four globals themselves -- ``g_late_hit_window_ms``,
``g_late_hit_max_quant_unit_fraction``, ``g_record_failsafe_seconds``, and
``g_mute_late_hit_ms`` -- are intentionally **not** LADSPA ports. Each is a plain
file-scope global in ``plugin.cc``, declared ``extern`` in ``engine.cpp``, written during
session load from the ``<Globals>`` node, and also reachable through the engine's generic
``/set``/``/get`` dispatch (see `OSC Control`_ earlier in this document). This still avoids
the LADSPA port descriptor table and ``slgui`` wiring. The trade-off is that it only works
in the standalone build where both translation units share an address space. When
``plugin.cc`` is loaded as a true LADSPA plugin inside a DAW, ``engine.cpp`` is absent and
the value stays at the compile-time default for the session, which is acceptable
behaviour. If any of the four is ever promoted to a GUI control, the correct migration
path is to add a proper LADSPA port and remove the global.

Live-Performance Audit
~~~~~~~~~~~~~~~~~~~~~~~

A companion document, ``live_performance_review.rst``, is a
live-performance-suitability code audit of the modifications, graded by
severity, live-performance impact, and confidence, with every item's
resolution status tracked explicitly.  It was written before most of its
own findings were addressed, so it now reads as a closed-out historical
record rather than a live list of concerns -- every CRITICAL item is
either implemented or deliberately deferred with stated rationale.  It is
kept, and referenced here, because two source-code comments (in
``looper.cpp`` and ``plugin.cc``) cite specific findings by issue number,
e.g. ``C1``.


Glossaries
----------

Some terminology and code variables related to the patch.

General Terminology
~~~~~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1

   * - Term
     - Description
   * - sync boundary
     - The moment when the looper's sync source crosses a beat or cycle boundary.
       Sync boundaries arrive as a short pulse on the looper's internal sync bus
       (``pfSyncInput[n] > 1.5``).
   * - sync mode (per channel)
     - Controls how a channel responds to a button press. Mode 0 (sync checkbox
       off) means "act immediately on the press." Mode 1 (sync checkbox on) means
       "wait for the next sync boundary before acting." The late-hit correction only
       activates in mode 1.
   * - sync source
     - The signal that generates sync boundaries. Can be JACK Transport, MIDI
       clock, an internal tempo generator, or another loop channel. With no source
       selected, no sync pulses arrive and the late-hit correction never fires.
   * - late-hit window
     - The maximum elapsed time between a sync boundary and a button press for the
       press to qualify as "late" rather than simply off-beat. Bounded by both an
       absolute millisecond cap and a fraction of one full quantize cycle at the
       current tempo and Eighths/Cycle setting; whichever is smaller applies.
   * - T
     - The number of audio samples that elapsed between the most recent sync
       boundary and the moment the button press was detected. The correction undoes
       this displacement: T samples are backfilled on a late start, or T samples
       are trimmed on a late stop.
   * - backfill
     - The process of copying the most recent T samples from the continuous input
       ring buffer into the beginning of the newly created loop buffer, so that the
       loop contains audio from the sync boundary rather than from the press time.
   * - internal sync mode
     - A configuration where the looper uses its own loop playback as the sync
       source rather than an external clock. In SooperLooper's GUI this corresponds
       to selecting Loop 1 as the sync source with the sync checkbox on. Late-hit
       correction is disabled on slave channels in this mode to prevent stereo
       channel divergence; both channels use the stock timing path instead.

Constants and Globals (``plugin.cc``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Variables marked **[NEW]** were added by this patch. All others are pre-existing
SooperLooper variables whose roles are clarified here because the patch depends on them.

.. list-table::
   :header-rows: 1

   * - Name
     - Status
     - Type
     - Description
   * - ``LATE_HIT_WINDOW_MS``
     - **[NEW]**
     - ``#define float``
     - Compile-time default for the late-hit window in milliseconds. Remains defined so it
       can initialise ``g_late_hit_window_ms`` and appear in startup banners. Do not use
       directly in runtime logic — use ``g_late_hit_window_ms``.
   * - ``LATE_HIT_MAX_QUANT_UNIT_FRACTION``
     - **[NEW]**
     - ``#define float``
     - Compile-time default for the quantize-unit fraction cap (default 100.0,
       which effectively disables the fraction cap so the ms cap always governs).
       Initialises ``g_late_hit_max_quant_unit_fraction``.
   * - ``g_late_hit_window_ms``
     - **[NEW]**
     - ``float`` (file-scope global)
     - Runtime window size in milliseconds. Initialised from ``LATE_HIT_WINDOW_MS``.
       Overwritten by ``engine.cpp`` during session load if ``late_hit_window_ms`` is
       present in the ``<Globals>`` node of the session file. Read at buffer-processing
       time via ``effectiveLateHitWindow()``. See the comment block in ``plugin.cc`` for
       the full rationale on why this is a global rather than a LADSPA port.
   * - ``g_late_hit_max_quant_unit_fraction``
     - **[NEW]**
     - ``float`` (file-scope global)
     - Fraction of one full quantize cycle that defines the upper limit of the late-hit
       window. ``effectiveLateHitWindow()`` multiplies this directly by
       ``lLastSyncPeriodSamples`` (the full cycle period in samples). At 0.25 the
       window is capped at 25 % of the current cycle length at any tempo or
       Eighths/Cycle setting. The compile-time default is 100.0, making the ms cap
       always the binding limit until a lower value is set in the session file.
   * - ``effectiveLateHitWindow()``
     - **[NEW]**
     - ``static inline`` function
     - Computes the window in samples as ``min(ms_cap, fraction_cap)``, where
       ``ms_cap = g_late_hit_window_ms × 0.001 × fSampleRate`` (clamped to
       ``lInputBufSize − 1``) and ``fraction_cap = g_late_hit_max_quant_unit_fraction
       × lLastSyncPeriodSamples``. Returns the ms cap alone if
       ``lLastSyncPeriodSamples`` is at its uninitialised sentinel (``ULONG_MAX``).
   * - ``sl_dbg_file``
     - **[NEW]**
     - ``static FILE*``
     - Handle for the debug log file (``$HOME/.sooperlooper/logs/sl_debug.log``,
       falling back to ``/tmp/sl_debug.log``, then ``stderr``). Opened lazily
       by ``sl_dbg_open()``.  Only declared and used when ``SYNCWDEBUG`` is
       defined; absent entirely in production builds.
   * - ``SL_LOG(...)``
     - **[NEW]**
     - macro
     - When ``SYNCWDEBUG`` is defined: calls ``sl_dbg_open()`` if needed, then writes to
       ``sl_dbg_file`` with an immediate ``fflush``.  Use for non-RT messages (startup
       banners, session load).  When ``SYNCWDEBUG`` is not defined: expands to nothing —
       no file is created and no output is produced.
   * - ``SL_LOG_RT(...)``
     - **[NEW]**
     - macro
     - When ``SYNCWDEBUG`` is defined: writes to ``sl_dbg_file`` (if open) and calls
       ``fflush()`` after every write.  Used inside ``SYNCWDBG()`` in the RT callback.
       Not RT-safe: ``fprintf`` acquires a stdio lock and ``fflush`` issues a kernel
       ``write()``; causes xruns when enabled.  The ``fflush`` ensures log data survives
       a crash but is the primary xrun risk.  When ``SYNCWDEBUG`` is not defined:
       expands to nothing.
   * - ``SYNCWDEBUG``
     - **[NEW]**
     - ``#define`` (flag)
     - Master switch for ALL debug output in ``plugin.cc``.  When defined, enables
       the ``sl_loaded.txt`` sentinel, ``sl_debug.log``, startup banners, and
       per-press diagnostics, all under ``$HOME/.sooperlooper/logs/`` (falling
       back to ``/tmp``).  When not defined (default): all ``SL_LOG`` /
       ``SL_LOG_RT`` / ``SYNCWDBG`` calls expand to nothing; no files are
       written.  Commented out by default; must remain disabled in any
       performance build.
   * - ``SYNCWDBG(x)``
     - **[NEW]**
     - macro
     - Expands to ``x`` when ``SYNCWDEBUG`` is defined; expands to nothing otherwise.
       Wraps ``SL_LOG_RT()`` calls in the hot path that are only wanted during debugging.
   * - ``sl_load_probe``
     - **[NEW]**
     - ``static struct SL_LoadProbe``
     - C++ static initialiser that fires when the binary is loaded (before ``main``).
       When ``SYNCWDEBUG`` is defined, writes a ``sl_loaded.txt`` sentinel
       (under ``$HOME/.sooperlooper/logs/``, falling back to ``/tmp``)
       confirming the experimental build is running.  When ``SYNCWDEBUG`` is
       not defined the constructor body is empty and no files are written.


Fields on ``SooperLooperI`` (``plugin.hpp``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``SooperLooperI`` is the per-instance state struct for the LADSPA plugin. One instance
exists per loop channel.

.. list-table::
   :header-rows: 1

   * - Name
     - Status
     - Type
     - Description
   * - ``lGlobalSamplesSinceSync``
     - **[NEW]**
     - ``unsigned long long``
     - Continuously-running sample counter that increments every sample in **all** engine
       states and resets to zero on each sync pulse (``pfSyncInput[n] > 1.5``). Valid at
       button-press time regardless of which state the engine is in. Contrast with the
       pre-existing ``lSamplesSinceSync``, which only increments during ``STATE_RECORD``
       and ``STATE_PLAY`` and is therefore useless at button-press time from
       ``STATE_OFF``. Widened to ``unsigned long long`` (from ``unsigned int``) to
       eliminate overflow at slow tempos or long gaps between syncs.
   * - ``bSyncPulseReceived``
     - **[NEW]**
     - ``bool``
     - Set to ``true`` when the first sync pulse arrives after instantiation or after a
       sync-source change. Guards against the startup case where
       ``lGlobalSamplesSinceSync`` is counting "time since engine start" rather than
       "time since last pulse." All three window conditions include
       ``&& pLS->bSyncPulseReceived``. Reset to ``false`` by ``reset_late_hit_state()``
       on sync-source change.
   * - ``lLastSyncPeriodSamples``
     - **[NEW]**
     - ``unsigned long``
     - Most recently measured full-cycle sync period in samples. Seeded from
       ``fTempo`` every buffer (BLOCK 7, immune to loop-coordination pulse noise) and
       updated from pulse measurement when a pulse passes the sanity floor (BLOCK 8).
       Used by ``effectiveLateHitWindow()`` to compute the fraction cap. Initialised to
       ``ULONG_MAX`` (sentinel meaning "no period known yet"); reset to ``ULONG_MAX``
       by ``reset_late_hit_state()`` on sync-source change.
   * - ``bLateHitBypassed``
     - **[NEW]**
     - ``bool``
     - Set ``true`` by ``Looper::run()`` on slave channels (index > 0) when in internal
       loop-sync mode (one looper driving another). Prevents late-hit correction on those
       channels to avoid L/R divergence; both channels then behave identically using the
       stock timing path. Set unconditionally each buffer by ``looper.cpp`` BLOCK 5 so it
       self-corrects within one buffer period if sync mode changes.
   * - ``bLateStartPending``
     - removed
     - ``bool``
     - Was present in an earlier design where the late-stop correction applied a different
       formula when preceded by a late start. That distinction was eliminated; the late-
       stop formula is now the same regardless of whether the start was late. Removed.
   * - ``lSamplesSinceSync``
     - pre-existing
     - ``unsigned long``
     - Per-instance sync phase counter. Increments only during ``STATE_RECORD`` and
       ``STATE_PLAY``. **Not used** by the late-hit code for the reason above.
   * - ``lInputBufWritePos``
     - pre-existing
     - ``unsigned long``
     - Write cursor into ``pfInputLatencyBuf``. Points to the **start** of the current
       LADSPA buffer block and is not advanced until the end of ``runSooperLooper()``. The
       backfill calculation relies on this: ``lInputBufWritePos − T`` is the read position
       for the T samples that elapsed since the last sync boundary.
   * - ``lInputBufSize``
     - pre-existing
     - ``unsigned long``
     - Length of ``pfInputLatencyBuf`` in samples (32768, hardcoded). Used as the modulus
       for the wrap-around read in the backfill, and as the hard upper bound on the
       effective window in ``effectiveLateHitWindow()``.
   * - ``lInputBufMask``
     - pre-existing
     - ``unsigned long``
     - ``lInputBufSize − 1``. Used with bitwise AND to wrap indices into
       ``pfInputLatencyBuf``.
   * - ``lBufferSizeMask``
     - pre-existing
     - ``unsigned long``
     - ``lBufferSize − 1`` (power-of-two). Used with bitwise AND to wrap write indices
       into ``pSampleBuf`` during the backfill.


Fields on ``LoopChunk`` (``plugin.hpp``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``LoopChunk`` represents one recorded loop. Multiple ``LoopChunk``\\ s can be stacked on a
single channel for overdub layers.

.. list-table::
   :header-rows: 1

   * - Name
     - Status
     - Type
     - Description
   * - [STRIKEOUT:``lFirstPlayOffset``]
     - removed
     - ``unsigned long``
     - Was present from an earlier design where the late-stop correction used a per-play
       rewind. That design was abandoned; the field was always 0 and never read. Removed.
   * - [STRIKEOUT:``hasFirstPlayOffset``]
     - removed
     - ``bool``
     - Companion flag to ``lFirstPlayOffset``. Also always ``false``, never read. Removed.
   * - ``dCurrPos``
     - pre-existing
     - ``double``
     - Current read/write position within the loop buffer, in samples. In the late-stop
       path this is advanced by *T* beyond the normal ``STATE_TRIG_STOP`` value to
       compensate for entering ``STATE_PLAY`` *T* samples late in real time.
   * - ``lLoopLength``
     - pre-existing
     - ``unsigned long``
     - Length of the recorded loop in samples. In the late-stop path this is set to
       ``dCurrPos − T`` (trimming the overrun audio) before entering ``STATE_PLAY``.
   * - ``lLoopStart``
     - pre-existing
     - ``unsigned long``
     - Absolute start offset of this loop in ``pSampleBuf``. The backfill writes to
       ``pSampleBuf[(lLoopStart + i) & lBufferSizeMask]`` for ``i`` in ``[0 .. T)``.


Buffer Variables (local to ``runSooperLooper()``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1

   * - Name
     - Status
     - Type
     - Description
   * - ``lGlobalSyncAtButtonPress``
     - **[NEW]**
     - ``unsigned long long`` (local)
     - Snapshot of ``pLS->lGlobalSamplesSinceSync`` taken **before** scanning the current
       buffer for sync pulses. This is the value used for all window tests. The snapshot
       is necessary because a sync pulse arriving mid-buffer should not retroactively
       change the phase reading for a button press that logically arrived at the buffer
       start.
   * - ``shiftSamples``
     - **[NEW]**
     - ``unsigned long`` (local)
     - Result of ``effectiveLateHitWindow(pLS, eighthPerCycle)``: the effective window in
       samples, bounded by both the ms cap and the fraction cap. Computed fresh at each
       button-press test.
   * - ``T``
     - **[NEW]**
     - ``unsigned long`` (local)
     - Alias for ``lGlobalSyncAtButtonPress`` within each late-hit block. Represents the
       number of samples that have elapsed since the last sync boundary. Reused as the
       trim/backfill size: *T* samples are backfilled on a late start, or *T* samples are
       trimmed on a late stop.
   * - ``withinWindow``
     - **[NEW]**
     - ``bool`` (local)
     - Late-start gate: true when ``T > 0 && T ≤ shiftSamples && fSyncMode > 0 &&
       bSyncPulseReceived && !bLateHitBypassed``. Quantize mode is not a condition; the
       correction fires for any external sync source.
   * - ``stopWithinWindow``
     - **[NEW]**
     - ``bool`` (local)
     - Late-stop gate: ``T > 0 && T ≤ shiftSamples && fSyncMode > 0 &&
       bSyncPulseReceived && loop ≠ NULL && !bLateHitBypassed``.
   * - ``trimmedLength``
     - **[NEW]**
     - ``unsigned long`` (local)
     - Computed loop length after trim: ``(unsigned long)dCurrPos − T``. Protected by a
       guard requiring at least ``xfadeSamples`` of remaining content; if the guard fails,
       the code falls back to ``STATE_TRIG_STOP``.
   * - ``readPos``
     - **[NEW]**
     - ``unsigned long`` (local)
     - Start offset into ``pfInputLatencyBuf`` for the backfill read:
       ``lInputBufWritePos − T``. The ``lInputLatency`` term that appears in the normal
       recording path is absent here: ``pfInputLatencyBuf`` stores samples written at
       physical time, and matching the sample that normal recording would have captured
       from the sync boundary works out to ``lInputBufWritePos − T`` with no additional
       offset. Wrap-around handled for all valid values of T up to ``lInputBufSize − 1``.
   * - ``multStopInWindow``
     - **[NEW]**
     - ``bool`` (local)
     - Multiply-end gate: ``T > 0 && T ≤ shiftSamples && fSyncMode > 0 &&
       bSyncPulseReceived && lCycles ≥ 2 && !bLateHitBypassed``.
       The ``lCycles ≥ 2`` guard prevents decrementing to a zero-length loop. Only active
       when ``LATE_HIT_MULTIPLY_CORRECTION`` is 1.
   * - ``pfInputLatencyBuf``
     - pre-existing
     - ``LADSPA_Data*``
     - The continuous 32768-sample input ring buffer written every block from ``pfInput``.
       The source for the backfill audio.
   * - ``pfSyncInput``
     - pre-existing
     - ``LADSPA_Data*``
     - Per-sample sync signal buffer. A value ``> 1.5`` in any sample position is a sync
       pulse. Scanned every block to maintain ``lGlobalSamplesSinceSync``.
   * - ``xfadeSamples``
     - pre-existing
     - ``unsigned long`` (local)
     - Minimum safe loop length for crossfade purposes (128 samples). Used as the lower
       bound in the trim guard.



Fields added to ``TransportInfo`` (``audio_driver.hpp``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

These fields exist only when ``ALB_BBT_SYNC`` is defined.  They are populated
by ``JackAudioDriver::get_transport_info()`` from the ``jack_position_t``
returned by ``jack_transport_query()`` when the timebase master has set the
``JackPositionBBT`` valid bit.  ``Engine::generate_sync()`` reads ``bbt_valid``
before accessing any other field; when ``bbt_valid`` is false the BBT fields
are not touched and the stock ``fmod`` path is used instead.

.. list-table::
   :header-rows: 1

   * - Name
     - Type
     - Description
   * - ``bar``
     - ``int32_t``
     - 1-indexed current bar from the timebase master.
   * - ``beat``
     - ``int32_t``
     - 1-indexed beat within the current bar.
   * - ``tick``
     - ``int32_t``
     - Tick within the current beat (0 to ``ticks_per_beat − 1``).
   * - ``bar_start_tick``
     - ``double``
     - Cumulative ticks from the transport origin (frame 0) to the start of
       the current bar.  Used with ``beat`` and ``tick`` to compute the
       absolute cumulative tick count at the start of each buffer.
   * - ``ticks_per_beat``
     - ``double``
     - Tick resolution set by the timebase master (Hydrogen uses 1920.0).
   * - ``bbt_valid``
     - ``bool``
     - ``true`` when all of the above fields were successfully populated from
       ``JackPositionBBT`` this buffer.  Always ``false`` when no timebase
       master is active, when ``ALB_BBT_SYNC`` is not defined, or when the
       master does not set ``JackPositionBBT``.

Session File Key (``engine.cpp``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1

   * - Key
     - Status
     - Description
   * - ``late_hit_window_ms``
     - **[NEW]**
     - Floating-point XML property in the ``<Globals>`` node of the ``.slsess`` session
       file — the same node as ``tempo`` and ``eighth_per_cycle``. Written by
       ``Engine::save_session()`` and read by ``Engine::load_session()``. If absent (older
       session files), the compile-time default is used. Edit with a text editor; a
       restart of the engine is required for the change to take effect.
   * - ``late_hit_max_quant_unit_fraction``
     - **[NEW]**
     - Floating-point XML property in the ``<Globals>`` node of the ``.slsess`` session
       file. Written by ``Engine::save_session()`` and read by
       ``Engine::load_session()``. If absent, the compile-time default
       ``LATE_HIT_MAX_QUANT_UNIT_FRACTION`` (100.0) is used. Edit with a text editor;
       a restart is required. Note: the older key ``late_hit_max_8th_fraction`` had
       different units (fraction of one eighth note rather than one full cycle); session
       files containing the old key will use the compile-time default on load.

Architecture Notes
------------------

-  **Why** ``lGlobalSamplesSinceSync`` **instead of** ``lSamplesSinceSync``:
   ``lSamplesSinceSync`` only increments during ``STATE_RECORD`` and ``STATE_PLAY``.
   Pressing Record from ``STATE_OFF`` gives a value of 0, which makes it impossible to
   distinguish an on-time press from a late one. The new counter runs in all states.

-  **Why a pre-buffer snapshot:** LADSPA processes audio in blocks. A sync pulse mid-
   block should not alter the phase measurement for a button press that arrived at the
   block boundary. The snapshot captures the phase as of the start of the current block.

-  **Why backfill rather than a playback offset for late start:** The input ring buffer
   already contains the audio from the boundary onward. Copying it into the loop buffer
   directly is simpler and more robust than skipping the first *T* samples on every play
   pass with a per-play offset.

-  **Why** ``dCurrPos += T`` **rather than** ``lFirstPlayOffset`` **for late stop:** Detecting
   the "first wrap" reliably to apply a rewind proved fragile (the wrap check fires
   inside a ``recenter`` guard at the wrong moment). Advancing ``dCurrPos`` by *T* at
   entry to ``STATE_PLAY`` achieves the same correction with no wrap detection needed.

-  **Why a global for** ``g_late_hit_window_ms`` **rather than a LADSPA port:** See
   `Why Not a LADSPA Port`_ above and the comment block in ``plugin.cc``. The short
   version: it is a setup-time preference, not a real-time control, and avoiding the port
   system keeps the change entirely within ``plugin.cc`` and ``engine.cpp``.

-  **Why the backfill read position is** ``lInputBufWritePos − T`` **(no lInputLatency):**
   ``pfInputLatencyBuf`` is a verbatim copy of ``pfInput`` with its write pointer at the
   current physical time. Normal recording from the sync boundary would have captured
   physical audio at ``T_sync − lInputLatency + i`` for sample *i*. Working backwards
   from ``lInputBufWritePos`` (which points to physical time ``T_now − lInputLatency``)
   this maps to offset ``lInputBufWritePos − T + i``, so ``readPos = lInputBufWritePos −
   T``. The ``lInputLatency`` term cancels exactly.

-  **Why** ``lLastSyncPeriodSamples`` **is seeded from tempo rather than measured pulses:**
   The sync bus carries two kinds of pulses above the 1.5 threshold: the true beat
   boundaries and lower-amplitude loop-coordination signals. Seeding from ``fTempo``
   (BLOCK 7) gives a period that is immune to these sub-threshold pulses, while BLOCK 8
   provides a measured update gated by a sanity floor to catch cases where no tempo is
   available.

Known Issues and Future Work
----------------------------

-  **Only tested on Ubuntu 24.04:** no other environments yet tested.

-  **Only tested in basic looping setups:** only basic looping and overdubs have
   currently been tested.  Multi-loop setups not yet tested (but should revert
   to stock behavior for syncing to a loop).  Tests with syncing assume the sync
   source is Jack.

-  **Internal sync mode:** when SooperLooper drives itself (one loop as the sync source
   for others), late-hit correction is **disabled on all slave channels**. Both channels
   of a stereo pair then use the stock timing path and stay in phase with each other.
   Enabling correction on all channels in this mode would require a broader audit of the
   state machine; deferred. See ``bLateHitBypassed`` in the Glossary.

-  **Window size and sample rate:** the effective maximum window is
   ``(lInputBufSize − 1) / fSampleRate`` seconds — approximately 682 ms at 48 kHz,
   743 ms at 44.1 kHz, 341 ms at 96 kHz. Setting ``late_hit_window_ms`` above this
   value is silently clamped; it effectively makes the fraction cap dominant for all
   musically relevant tempos. To raise the ceiling, increase ``lInputBufSize`` in
   ``instantiate()`` (inexpensive: 128 KB per channel at 48 kHz).

-  **Spurious trigger at startup:** ``bSyncPulseReceived`` (see Glossary) prevents the
   window condition from evaluating until at least one real sync pulse has arrived, so a
   press shortly after startup cannot trigger a spurious correction.

-  **Multiply end:** correction is implemented and pending test confirmation (disable
   with ``LATE_HIT_MULTIPLY_CORRECTION 0`` if regressions appear).

-  **Multiply start:** deferred. The frontfill machinery in ``beginMultiply()`` makes the
   position adjustment more involved; low priority since Multiply start snaps to the
   current loop's cycle boundary which is a natural and musically acceptable snap point.

-  **Insert end:** not needed — Insert end does not use ``waitingForSync`` and takes
   effect at the exact press sample already.

-  **Insert start:** deferred for the same reason as Multiply start.

-  **Double log lines:** each button press produces two identical log entries because
   both channel instances (0:0 and 0:1) receive the same event and log independently.
   Expected behaviour, not a bug.

-  **Sync-source change:** the late-hit state (``bSyncPulseReceived``,
   ``lGlobalSamplesSinceSync``, ``lLastSyncPeriodSamples``) is reset to its initial
   condition whenever ``Engine::update_sync_source()`` is called — i.e., whenever the
   user changes the sync source. This prevents stale period measurements from the old
   source affecting the first presses on the new one.

-  ``SYNCWDEBUG`` **is off by default.** Uncomment ``//#define SYNCWDEBUG``
   in ``plugin.cc`` to enable all debug output.  When enabled:

   - a ``sl_loaded.txt`` sentinel is written at binary-load time confirming
     the experimental build is running.
   - ``sl_debug.log`` is created and overwritten fresh on each engine
     start. Both files land in ``$HOME/.sooperlooper/logs/`` (created if it
     does not already exist -- SooperLooper's own per-user directory, the
     same one ``ALB_SYNC_PULSE_LOG`` uses), falling back to ``/tmp`` if
     ``$HOME`` is unset or that directory can't be created; the debug log
     falls back further to ``stderr`` as a last resort.
   - ``SL_LOG`` startup banners (instantiate, activate) go to the log file.
   - ``SYNCWDBG()``-wrapped ``SL_LOG_RT()`` calls write per-press timing
     diagnostics to the same file.

   To watch the log in real time::

       tail -f ~/.sooperlooper/logs/sl_debug.log

   When ``SYNCWDEBUG`` is enabled, ``SL_LOG_RT`` calls ``fflush()`` after
   every write.  This forces an immediate kernel ``write()`` syscall on every
   button press, acquiring the C stdio mutex.  Both operations can block the
   RT thread; **xruns are the expected cost of enabling this mode.**  The
   fflush ensures log data is flushed to disk even if the process crashes
   mid-session, which makes post-mortem diagnosis reliable.  When
   ``SYNCWDEBUG`` is not defined (the default), none of this code exists:
   ``SL_LOG`` / ``SL_LOG_RT`` / ``SYNCWDBG`` expand to nothing, and the only
   output is the ``fprintf(stderr, ...)`` banner in ``engine.cpp`` after
   session load.


Test Suite
----------

The stock test harness lives in ``src/test_plugin/``.  It wraps ``plugin.cc``
via SWIG and runs each looper instance as a real JACK client, taking it through
its states from Python.  JACK must be running before the tests are started; a
dummy backend suffices::

    jackd -d dummy &
    make
    python3 -m pytest

Note: use ``python3 -m pytest`` rather than the bare ``pytest`` command to
ensure all imports resolve against the active virtual environment.

Python 3 migration
~~~~~~~~~~~~~~~~~~

The original tests targeted Python 2.7 with ``nose`` / ``nose_parameterized``.
The following files were updated to run under Python 3 with ``pytest``:

.. list-table::
   :header-rows: 1

   * - File
     - Change
   * - ``engine_wrapper.py``
     - ``dict.iteritems()`` → ``dict.items()``; integer division ``/ 2`` → ``// 2``
   * - ``testdef_simple_state_after_record.py``
     - Removed unused ``from nose_parameterized import parameterized`` import
   * - ``testdef_simple_state_off.py``, ``_offmuted.py``, ``_record_undo.py``, ``_record_undoall.py``
     - ``from nose_parameterized import parameterized`` → ``from parameterized import parameterized``
   * - ``makefile``
     - ``-I/usr/include/python2.7/`` replaced with ``$(shell python3-config --includes)``
   * - ``pytest.ini``
     - New file.  Sets ``python_files = testdef_*.py`` so ``pytest`` collects
       the ``testdef_`` prefix convention and ignores the SWIG-generated
       ``test_engine.py``.
   * - ``README``
     - Updated with Python 3 dependencies, build, and run instructions.

``testdef_simple_state.py`` and ``testdef_simple_state_overdub.py`` required no
changes.  Install dependencies with::

    pip install pytest parameterized swig

Test harness additions for late-hit testing
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The C++ test harness (``test_looper.hpp`` / ``test_looper.cpp``,
``test_engine.hpp``, ``test_engine.swg``) was extended with the following
additions to support the late-hit tests:

.. list-table::
   :header-rows: 1

   * - Addition
     - Description
   * - ``TestLooper::sync_inject_value`` (``float``, public)
     - When ``sync_inject_enabled`` is true, ``run()`` fills ``_our_syncin_buf``
       with this value before each JACK callback.  Use ``2.0`` to inject a sync
       pulse (above the ``> 1.5`` threshold that sets ``bSyncPulseReceived``);
       use ``0.0`` for no signal.  Note: ``1.0`` must not be used as a
       background value — ``STATE_TRIG_START`` and ``STATE_TRIG_STOP`` exit on
       ``pfSyncInput[n] > 0.0``, so a constant ``1.0`` would cause them to
       leave the wait states immediately.
   * - ``TestLooper::sync_inject_enabled`` (``bool``, public)
     - Enables the sync buffer injection above.
   * - ``TestLooper::process_count`` (``volatile int``, public)
     - Incremented at the top of every ``run()`` call.  Python tests busy-wait
       on this to synchronise with exact JACK callback counts rather than using
       ``time.sleep()``, making timing independent of JACK period size.
   * - ``TestLooper::get_buffer_size()`` (returns ``int``)
     - Returns ``_buffersize`` (JACK period size in samples).  Return type is
       ``int`` rather than ``nframes_t`` so that SWIG maps it to a plain Python
       integer rather than an opaque ``SwigPyObject``.
   * - ``TestEngine::get_sample_rate()`` (returns ``int``)
     - Returns ``jack_get_sample_rate(jack)``.  Used together with
       ``get_buffer_size()`` to convert buffer counts to milliseconds when
       setting the correction window.
   * - ``g_late_hit_window_ms`` exposed via SWIG
     - Declared ``extern float g_late_hit_window_ms;`` in ``test_engine.swg``
       (both inside and outside the ``%{ %}`` block).  Accessible in Python as
       ``test_engine.cvar.g_late_hit_window_ms``.  SWIG places writable C
       globals in a ``cvar`` object on the module; writing directly to
       ``test_engine.g_late_hit_window_ms`` silently creates a Python attribute
       and does not affect the C variable.
   * - ``g_late_hit_max_quant_unit_fraction`` exposed via SWIG
     - Same mechanism as above.  Accessible as
       ``test_engine.cvar.g_late_hit_max_quant_unit_fraction``.

Test harness additions for audio-content testing
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Added alongside ``cancel_record_mute``.  Before these the harness could only
assert on *state* values; it can now assert on what actually landed in the
loop buffer, which is how the fade-residue bugs were found and fixed.

.. list-table::
   :header-rows: 1

   * - Addition
     - Description
   * - ``TestLooper::input_inject_value`` (``float``, public)
     - The constant fed to the plugin's audio input each block.  ``run()``
       has never read the real JACK input port -- it always synthesised a
       buffer -- so this only makes that constant settable.  Defaults to
       ``-0.7``, the previously hardcoded value, so every test written
       before this behaves identically.
   * - ``TestLooper::output_min`` / ``output_max`` (``float``, public)
     - Running min/max of channel 0's output, sampled after
       ``descriptor->run()``.  They accumulate across blocks because one
       block covers only a fraction of a loop period, so a single block can
       miss a contaminated region entirely.
   * - ``TestLooper::output_last`` (``float``, public)
     - Last sample of the most recent block.
   * - ``TestLooper::reset_output_stats()``
     - Resets the three above.  Typical use: reset, spin a known number of
       callbacks, then read the range.
   * - ``engine_wrapper.command_ordinals``
     - Command name -> ordinal, parsed from ``event.hpp``.  The number passed
       to ``request_cmd()`` is the ``Event::command_t`` ordinal, and the
       positional ``commands`` tuple only covers the first 24 values, so
       anything past those must come from the header.

Set ``DryLevel`` to 0 from the test so the output is purely loop content.

.. note::

   The harness is still hardcoded to **one** channel
   (``test_engine.cpp``: ``TestLooper(jack, 0, 1)``) while the live rig runs
   ``sooperlooper -c 2``.  Making it configurable was considered and
   deferred.  Note that channels above 0 have State, LoopLength,
   LoopPosition and output wired to slave/dummy ports, so raising the
   count alone would add no observability.

Late-hit test cases (``testdef_late_hit.py``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The base class ``LateHitTestCase`` sets the correction window to exactly
``(WITHIN_BUFS + 1)`` buffer-periods via ``test_engine.cvar.g_late_hit_window_ms``
in ``setUp()``, and restores the default (100 ms) in ``tearDown()``.  All
timing uses ``wait_buffers(n)`` (busy-wait on ``process_count``) rather than
``time.sleep()`` so the tests are independent of JACK period size.  One
``wait_buffers(1)`` settle call follows every ``engine.request()`` before any
``assertState()`` to avoid a race between ``request_pending`` clearing and
``descriptor->run()`` completing.

.. list-table::
   :header-rows: 1

   * - Test
     - What it verifies
   * - ``TestLateStart.testLateStart_DirectlyRecording``
     - Core late-start case: RECORD within the window after a pulse enters
       ``STATE_RECORD`` directly.  Would fail on unpatched code (gets
       ``WaitStart`` instead).
   * - ``TestLateStart.testNoCorrection_SyncOff``
     - Regression: sync OFF still reaches ``STATE_RECORD`` immediately (stock
       path unaffected).
   * - ``TestLateStart.testNoCorrection_NoPulse``
     - ``bSyncPulseReceived`` guard: no correction fires before the first sync
       pulse has been seen.
   * - ``TestLateStart.testNoCorrection_TooLate``
     - Window boundary: T = ``OUTSIDE_BUFS × buf`` > window →
       ``STATE_TRIG_START`` (no correction).
   * - ``TestLateStart.testNoCorrection_OnTime``
     - On-time press (pulse held on during RECORD) produces ``Recording`` or
       ``WaitStart`` and does not crash.
   * - ``TestLateStop.testLateStop_DirectlyPlaying``
     - Core late-stop case: second RECORD within window after a pulse enters
       ``STATE_PLAY`` directly.
   * - ``TestLateStop.testNoCorrection_Stop_TooLate``
     - Window boundary on stop: T > window → ``STATE_TRIG_STOP`` (no
       correction).
   * - ``TestLateStop.testNoCorrection_Stop_NoPriorPulse``
     - ``bSyncPulseReceived`` guard on stop: recording started with sync off
       (no pulse ever seen); stop goes to ``WaitStop``.
   * - ``TestLateStop.testLateStop_LoopLengthTrimmed``
     - Loop length after a late stop is positive and less than the total
       recording duration (the T-sample trim is visible).
   * - ``TestWindowControl.testWindowBoundaryExact``
     - Pins the window arithmetic from both sides in a single test: T =
       ``WITHIN_BUFS × buf`` fires when window = ``WITHIN_BUFS + 1`` buffers
       and does not fire when window = ``WITHIN_BUFS − 1`` buffers.
   * - ``TestSyncSourceReset.testAfterSyncOff_NoCorrectionOnNextRecord``
     - After toggling sync off and back on without a new pulse, the first
       press does not fire the correction spuriously.

Not yet tested
~~~~~~~~~~~~~~

The following behaviours are covered by the patch but do not yet have
automated tests:

-  **Exact T offset after late start.**  The tests verify that ``STATE_RECORD``
   is entered directly, but do not assert that ``dCurrPos == T`` (the loop
   content begins at the sync boundary).

-  **Backfill audio content.**  The ring buffer read position
   (``lInputBufWritePos − T``) and the per-sample copy are not verified
   against the expected source samples.

-  **Multiply-end correction** (``multStopInWindow``, guarded by
   ``LATE_HIT_MULTIPLY_CORRECTION``).

-  **Two-channel** ``bLateHitBypassed`` behaviour in internal sync mode.  A
   two-channel ``TestLooper`` (``_chan_count = 2``) and a sync source pointing
   to one of the looper instances would be needed to test this.

-  **C6 reset via an actual sync-source change engine event.**  The current
   test toggles the ``Sync`` port, which does not call
   ``Engine::update_sync_source()``.  A test that sends an OSC sync-source
   change and verifies ``bSyncPulseReceived`` is cleared would more faithfully
   exercise the C6 fix.

