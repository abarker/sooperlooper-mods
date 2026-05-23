Late-Hit Recording Correction
==============================

*A patch for SooperLooper 1.7.9 — overview and user guide*

----

The problem
-----------

When a performer presses a footswitch to start or stop recording
while a looper is locked to an external beat source, the press almost
never lands precisely on the beat boundary.  Human reaction time, MIDI
latency, and foot mechanics all conspire to make every button press
slightly early or slightly late relative to the intended boundary.

A late press is particularly problematic.  A press that arrives even
50 ms after the beat boundary causes the loop to start from the
"wrong" sample, producing a loop that is subtly out of phase with the
rest of the arrangement.  Overdubs recorded on top of it compound the
phase error.  The result is cumulative drift — loops that sound
progressively less locked even though the underlying tempo is rock-solid.

A press that arrives early is less damaging (the loop starts before
the boundary, so it captures a bit of extra audio at the top), but
early presses are also less common: it is cognitively easier to react
to a cue than to anticipate it.

----

What this patch does
--------------------

The late-hit correction detects button presses that arrive within a
short window after a sync boundary and retroactively moves the loop
start (or stop) point back to that boundary, as if the press had
landed exactly on time.

Two distinct corrections are applied depending on which press was late:

**Late start**: if RECORD is pressed slightly after a beat boundary,
the loop start is moved back to the boundary by back-filling the
leading audio from a short ring buffer that has been continuously
recording the input.  The performer hears no gap; the recorded loop
begins at the boundary.

**Late stop**: if the second RECORD press (which ends recording)
arrives slightly after a beat boundary, the loop end is trimmed back
to that boundary and the playback position is adjusted accordingly.
The loop length is correct; the overshoot audio is discarded.

Both corrections are invisible to the listener — the result is
indistinguishable from a perfectly timed press.

The window within which a press qualifies as "late" (rather than
simply wrong) is bounded by two limits, whichever is smaller: an
absolute millisecond cap (default 100 ms) and a fraction of an
eighth-note period at the current tempo.  The fraction cap ensures
that at fast tempos the window does not overlap with the next beat.

----

When does it activate?
-----------------------

The correction only activates when:

1. The looper's sync checkbox is enabled (the looper is locked to an
   external beat source — JACK Transport from Ardour, Hydrogen, etc.).
2. At least one sync boundary has been received since the looper was
   last activated.
3. The button press arrived after a sync boundary (not before it).
4. The time since that boundary is within the correction window.

When any of these conditions is not met, the press is handled
normally — the loop starts or stops exactly where the press landed.
This means the feature is completely inactive when the sync checkbox
is off, which is the typical state for free-form recording.

----

Known limitations
-----------------

**Internal sync mode (loop syncing to itself)**

When the looper is set to use its own playback as the sync source
rather than an external clock, the late-hit correction is disabled
on slave channels (all channels except the first).

The reason: in this mode, the first channel (master) uses a
"start immediately" sync setting, while subsequent channels (slaves)
use a "wait for sync" setting.  If only the slave applied the
late-hit correction, a stereo pair would record their loop starts
at different points — L and R would diverge by up to the window
duration.  Disabling the correction on all channels restores the
stock SooperLooper behaviour, which already achieves sample-accurate
alignment between channels via its normal sync mechanism.

The practical consequence: when using Loop 1 as the sync source for
a stereo loop, button presses that arrive slightly late are not
corrected.  The loop starts exactly where the press landed.  This is
the same behaviour as unpatched SooperLooper.

A more complete fix that enables both-channel correction in internal
sync mode is technically feasible but requires a larger code audit;
it is documented as future work.

**Window size and tempo changes**

The correction window has two components: a fixed millisecond cap and
a tempo-relative fraction cap.  Both are compile-time constants.  If
the tempo changes mid-session, the tempo-relative component adjusts
automatically (it is recomputed from the JACK Transport tempo every
buffer), but the millisecond cap does not change.  At very fast
tempos (above ~240 BPM with default settings) the fraction cap will
be smaller than the ms cap and will dominate.

**Counter overflow**

The internal timer that measures "time since last sync boundary" is a
64-bit sample counter, which overflows after approximately 12 million
years at 48 kHz.  This is not a practical concern.

**Multiply-end correction**

The same late-hit window is also applied to the end of a *Multiply*
operation (pressing RECORD to end a cycle multiplication).  The
internal sync mode limitation above applies here too.

----

Glossary
--------

**sync boundary**
    The moment when the looper's sync source (JACK Transport or its
    own loop playback) crosses a beat or cycle boundary.  Sync
    boundaries arrive as a short pulse on the looper's internal sync
    bus.

**sync mode** (per channel)
    Controls how a channel responds to a button press.  Mode 0 means
    "act immediately on the press."  Mode 1 means "wait for the next
    sync boundary before acting."  The late-hit correction only
    activates in mode 1.

**late-hit window**
    The maximum elapsed time between a sync boundary and a button
    press for the press to qualify as "late" rather than simply
    off-beat.  Expressed both as an absolute value in milliseconds
    and as a fraction of an eighth-note period; whichever is smaller
    is used.

**T**
    The number of audio samples that elapsed between the most recent
    sync boundary and the moment the button press was detected.  The
    correction undoes this displacement.

**back-fill**
    The process of copying the most recent T samples from the input
    ring buffer into the beginning of the newly created loop, so that
    the loop contains audio from the sync boundary rather than from
    the press time.

**internal sync mode**
    A configuration where the looper uses its own loop playback as
    the sync source, rather than an external clock.  In SooperLooper's
    GUI, this corresponds to selecting Loop 1 as the sync source
    with the sync checkbox on.

**JACK Transport**
    The JACK Audio Connection Kit's global transport system, which
    provides a shared tempo and position to all connected applications.
    Ardour and Hydrogen can act as JACK Transport masters.

----

Appendix: Implementation reference
-----------------------------------

*This section is for developers reading or modifying the patch code.
It is not needed to understand or use the feature.*

Changes are confined to five files and are all marked with
``//------ albBegin`` … ``//------ albEnd`` (looper.cpp, looper.hpp,
audio_driver.hpp, engine.hpp) or ``//====== albBegin`` …
``//====== albEnd`` (plugin.cc, plugin.hpp) block markers.

plugin.cc blocks
~~~~~~~~~~~~~~~~

**BLOCK 1** — Global constants and debug infrastructure.
Defines ``g_late_hit_window_ms`` (100 ms default) and
``g_late_hit_max_8th_fraction`` (0.5 default), the ``SL_LOG_RT`` RT-safe
logging macro, and the ``SYNCWDEBUG`` compile-time flag that enables
detailed sync diagnostic output.  Purely additive.

**BLOCK 2** — ``#ifdef SYNCWDEBUG`` flag guard.
Separates the debug-output machinery from the feature code.

**BLOCK 3** — ``effectiveLateHitWindow()``.
Computes the window in samples: ``min(ms_cap, fraction_cap)``.
Returns the ms cap alone when ``lLastSyncPeriodSamples`` is the
uninitialised sentinel (``ULONG_MAX``).

**BLOCK 4** — Instantiate banner.
Writes a startup log line when a plugin instance is created, confirming
the late-hit patch is active.

**BLOCK 5** — Activate banner and late-hit state reset.
Writes a per-activation log line and resets ``lGlobalSamplesSinceSync``,
``bSyncPulseReceived``, ``lLastSyncPeriodSamples``, and
``bLateHitBypassed`` to their start-of-session values.

**BLOCK 6** — Late-hit state initialisation in ``activate()``.
Explicit zero/sentinel initialisation of the five late-hit state
members of ``SooperLooperI``.

**BLOCK 7** — ``lLastSyncPeriodSamples`` seeding from tempo.
Runs every buffer when ``fSyncMode >= 1.0f && fTempo > 0.0f``.
Computes the full cycle period from ``fTempo`` directly, providing an
authoritative value immune to sync-pulse measurement noise.  Also
falls back to ``syncSamples`` via the original sentinel guard.

**BLOCK 8** — Per-sample sync-pulse scan and counter.
Increments ``lGlobalSamplesSinceSync`` each sample (when
``fSyncMode >= 1.0f``).  On each sync pulse above the sanity floor,
captures the counter to ``lLastSyncPeriodSamples``, resets the
counter, and sets ``bSyncPulseReceived``.  Sanity floor
(``fSampleRate / 40``) rejects spurious sub-period loop-coordination
pulses.  Snapshot ``lGlobalSyncAtButtonPress`` taken before the scan.

**BLOCK 9** — Late-stop check (RECORD pressed during STATE_RECORD).
``stopWithinWindow`` computed from T, ``shiftSamples``, ``fSyncMode``,
``bSyncPulseReceived``, ``loop != NULL``, and ``!bLateHitBypassed``.
When true: loop length trimmed, ``dCurrPos`` advanced, loop enters
``STATE_PLAY`` directly.  When false: existing ``STATE_TRIG_STOP`` path
(unchanged stock behaviour).

**BLOCK 10** — Late-start check (RECORD pressed from idle).
``withinWindow`` computed.  When true: ``pushNewLoopChunk`` called,
back-fill of T samples from ``pfInputLatencyBuf``, ``dCurrPos = T``,
loop enters ``STATE_RECORD``.  When false: existing ``STATE_TRIG_START``
wait-for-sync path.

**BLOCK 11** — Multiply-end late-hit check.
``multStopInWindow`` computed; ``!bLateHitBypassed`` gate included.
When true: multiply cycle end aligned to sync boundary.

**BLOCK 12** — ``STATE_TRIG_STOP`` playback-start offset.
Additive: advances ``dCurrPos`` by the combined output+input latency at
the TRIG_STOP → STATE_PLAY transition, so playback position is correct
accounting for hardware latency.

plugin.hpp additions
~~~~~~~~~~~~~~~~~~~~

Five new members in ``SooperLooperI``, in two albBegin blocks:

- ``lGlobalSamplesSinceSync`` (``unsigned long long``): running
  sample counter since last sync boundary; widened from ``unsigned int``
  by the C5 fix.
- ``bSyncPulseReceived`` (``bool``): set true on first sync pulse.
- ``lLastSyncPeriodSamples`` (``unsigned long``): measured cycle period.
- ``bLateHitBypassed`` (``bool``): set by Looper on slave channels in
  internal sync mode; disables the withinWindow checks for that channel.

looper.cpp additions
~~~~~~~~~~~~~~~~~~~~

**Constructor block**: ``_slave_sync_port`` initial value corrected from
``1.0f`` to ``ports[Sync]`` (stock comment included per D5 convention).

**BLOCK 4** (in ``run()``): fixes ``_slave_sync_port`` so that slave
channels mirror the master's sync setting in non-internal-sync mode.

**BLOCK 5** (in ``run()``): C2 fix — iterates over all plugin instances
before calling ``run_loops()``, setting ``bLateHitBypassed = true`` on
slave channels (index > 0) when in internal sync mode.

Known bugs fixed
~~~~~~~~~~~~~~~~

**C1** — ``lLastSyncPeriodSamples`` corruption by loop-coordination
pulses: fixed by fTempo direct computation (BLOCK 7) and sanity floor
(BLOCK 8).

**C2** — Stereo channel late-hit divergence in internal sync mode:
fixed by ``bLateHitBypassed`` flag (looper.cpp BLOCK 5, plugin.cc
BLOCKS 9/10/11).  Trade-off: late-hit inactive in internal sync mode;
both channels use stock immediate behaviour consistently.

**C5** — ``lGlobalSamplesSinceSync`` 32-bit overflow: fixed by
widening to ``unsigned long long``.

Deferred
~~~~~~~~

**C1a** — In pulse-arrival buffers, BLOCK 8 can transiently overwrite
BLOCK 7's fTempo-derived value with the pulse-measured value.  Both
values agree in steady state; the overwrite self-corrects within one
buffer (~10 ms).  Fix: gate BLOCK 8 period capture on ``fTempo == 0.0f``.
One-condition wrap, zero risk, deferred.

**C2 Option B** — Full late-hit correction in internal sync mode
(both channels corrected).  Requires auditing 21 ``fSyncMode == 0.0f``
comparison sites in the stock DSP state machine, changing each to
``<= 0.0f``.  Mechanical but consequential; deferred.
