================================================================================
SooperLooper Modifications — Live Performance Code Review
================================================================================

:Document type:  Critique / improvement guideline
:Scope:          All ``albBegin``/``albEnd`` blocks in plugin.cc, plugin.hpp,
                 engine.cpp, engine.hpp, looper.cpp, looper.hpp, audio_driver.hpp,
                 jack_audio_driver.hpp; surrounding stock code on hot paths.
:Lens:           Live-performance suitability — correctness, robustness,
                 RT-safety / latency.
:Status:         Audit only; no code changes proposed in this document.
                 Each item names the failure mode, where it lives, and a
                 recommended direction for a future agent or developer.


How to read this document
==========================

Items are graded on three axes:

- **Severity**: CRITICAL (can crash or silently corrupt loops), HIGH
  (functional incorrectness without crash), MEDIUM (degraded UX, recovery
  required), LOW (style, structure, defence-in-depth).

- **Live-performance impact**: 1–5 scale. 5 = will bite during a gig.
  1 = a maintenance / hygiene concern only.

- **Confidence**: HIGH (verified in code), MEDIUM (consistent with code
  but not reproduced), LOW (theoretical, may not arise in practice).

An item with CRITICAL severity but live-impact 1 is unlikely to matter on
stage; one with MEDIUM severity and impact 5 should still be addressed
before a serious performance.

The artefact's intended audience is another agent or developer iterating on
this codebase.  Recommendations are stated as direction, not as code.


Executive summary
==================

**Status at a glance** (most recent revision)::

  C1  ✓ DONE    C2  ✓ DONE    C3  DEFERRED  C4  N/A      C5  ✓ DONE
  C6  OPEN      C7  N/A       C8  OPEN       C9  OPEN     C10 OPEN
  C1a OPEN

  H1  OPEN      H2  N/A       H3  N/A        H4  OPEN (small fix)

  R1  ACCEPTED  R2  OPEN      R3  N/A         R4  OPEN     R5  OPEN

  D1  OPEN      D2  OPEN      D3  N/A         D4  OPEN     D5  ✓ DONE

The modifications fall into three families.  Each family has distinct
risk characteristics:

1. **Late-hit window** (BLOCKS 1–12 in plugin.cc, two extern globals in
   engine.cpp, session save/load): the most invasive set of changes.
   Touches the RT hot path on every buffer.  Has at least one
   correctness bug actively observed in testing (window collapses to
   ~6 samples after first recording in internal sync mode) and several
   latent issues around inter-channel symmetry and counter overflow.

2. **JACK MIDI in/out ports + intra-block timestamping** (blocks in
   engine.cpp/.hpp, audio_driver.hpp, jack_audio_driver.hpp, looper.cpp/.hpp):
   architecturally clean and well-isolated.  The main live-performance
   concerns are JACK client lifecycle (reconnect/disconnect not handled),
   silent event drops on binding-lock contention, and minor MIDI clock
   ordering quirks.

3. **RT-safety toggles** (``ALB_RT_SAFE_COND_SIGNAL``,
   ``ALB_TEMPO_ONLY_WHEN_ROLLING``): both are net positive for xrun
   prevention.  Behavioural trade-offs (tempo changes not propagating
   when stopped; main-loop responsiveness depends on its existing
   ``timedwait`` timeout) are documented but worth re-evaluating against
   actual session needs.

A separate fourth family — **debug logging** (``SL_LOG``, ``SL_LOG_RT``,
``SYNCWDEBUG``, ``sl_dbg_open``, ``SL_LoadProbe``) — was recently
hardened.  Residual concerns are documented in the RT-safety section
but are no longer blocking.

Highest-priority items for a live-performance build (status as of
the most recent revision):

- **C1** ✓ IMPLEMENTED (lLastSyncPeriodSamples pollution): authoritative
  fTempo path added in BLOCK 7, sanity floor added in BLOCK 8.  One
  residual stylistic refinement tracked as C1a.
- **C2** ✓ IMPLEMENTED (inter-channel asymmetry in internal sync mode):
  ``bLateHitBypassed`` flag set by Looper on slave channels in internal
  sync mode.  Late-hit disabled in internal sync; consistent stock
  behaviour restored.  Zero JACK regression risk.
- **C3** DEFERRED (JACK client reconnect / disconnect): real-world risk
  judged low because peer applications also crash on JACK restart;
  improved error reporting deemed sufficient.
- **C4** N/A (JACK MIDI binding-lock contention): analysed and removed
  from active list.  Lock is never held during normal live performance;
  risk is confined to setup activities.  See C4 section for details.
- **C5** ✓ IMPLEMENTED (lGlobalSamplesSinceSync overflow): widened to
  ``unsigned long long``.  64-bit Linux is the realistic target;
  32-bit portability caveat noted but not relevant in practice.


Critical issues — correctness
==============================

C1: ``lLastSyncPeriodSamples`` pollution by loop-coordination pulses
--------------------------------------------------------------------

**Severity**: CRITICAL.  **Live impact**: 5.  **Confidence**: HIGH (observed).

**Status**: ✓ IMPLEMENTED — see section for implementation details.

**Where**: plugin.cc BLOCK 8 (sync-pulse scanner) and BLOCK 3
(``effectiveLateHitWindow``).

**Symptom (observed)**: in internal sync mode (sync source = Loop 1,
stereo looper), after the first recording the late-hit window collapses
from 4800 samples (the 100 ms ``msLimit``) to ~6 samples (a ~0.1 ms
``fractionLimit``) and never recovers.  The feature is effectively
disabled for all subsequent presses.

**Mechanism**: BLOCK 8 treats *any* ``pfSyncInput[n] > 1.5f`` as a
period marker and updates ``lLastSyncPeriodSamples`` to the elapsed
sample count since the previous such marker.  The scanner is agnostic
about the **meaning** of the marker.  In internal sync mode the slave
channel's ``pfSyncInput`` is wired to the master's ``pfSyncOutput``,
which carries:

- ``2.0f`` pulses at loop boundaries / state transitions (recording
  end → play start, undo, redo, etc.)
- ``1.0f`` markers at cycle starts (filtered out by the 1.5f threshold)

At transition points the master can emit two ``2.0f`` pulses within
a handful of samples (e.g. ``pfSyncOutput[0] = 2.0f`` at TRIG_STOP
end + a near-immediate ``2.0f`` when the loop re-enters play and hits
its sync boundary in the next or same buffer).  The scanner reads
this as "period = 6 samples", which then drives the fraction-cap
calculation in ``effectiveLateHitWindow`` to a useless value.

**Why it matters live**: the entire reason the late-hit window exists
is to forgive presses that arrive slightly after the intended beat.
If it silently disables itself, the performer thinks the feature is
working and is unaware that subsequent late presses are no longer
being corrected.

**Direction for fix**:

a) **Sanity floor on captured periods**: refuse to update
   ``lLastSyncPeriodSamples`` if the measured interval is less than
   some musically plausible floor (e.g. 1/32-note at the host's
   maximum supported tempo, which at 300 BPM and 48 kHz is roughly
   1500 samples).  This is the simplest fix and preserves the
   existing architecture.

b) **Decouple period measurement from pulse scanning**: when
   ``fTempo > 0`` (engine knows the tempo), derive
   ``lLastSyncPeriodSamples`` directly from
   ``fSampleRate * 60 / fTempo * (eighthPerCycle / 8.0)`` rather than
   from inter-pulse interval.  This is more correct but requires
   threading the tempo through to the plugin (it may already be
   available via the ``Tempo`` port).

c) **Disambiguate pulse types**: use a different sync-output value
   for true period markers vs. transition events (e.g. 2.0 for
   boundaries, 3.0 for tempo ticks).  Higher disruption to stock code.

Option (a) is the recommended starting point because it's local and
reversible.  Option (b) is the architecturally cleaner long-term
direction.

**Implementation status**: BOTH options (a) and (b) were implemented
together in a subsequent revision.

- plugin.cc BLOCK 7 now computes ``lLastSyncPeriodSamples`` directly
  from ``fTempo`` every buffer when
  ``fSyncMode >= 1.0f && fTempo > 0.0f && eighthPerCycle > 0``.
  Formula: ``fSampleRate × 30 / fTempo × eighthPerCycle`` (full cycle
  period, NOT divided by 2 as in syncSamples — the ``/2`` in
  syncSamples is for quantize-period purposes and would have
  produced a half-size window).  Authoritative, RT-cheap (one divide
  and two multiplies per buffer), and immune to pulse-bus noise.

- plugin.cc BLOCK 8 adds a sanity floor: the pulse-measured period
  is only written to ``lLastSyncPeriodSamples`` if it is at least
  ``fSampleRate / 40`` samples (≈ 1200 at 48 kHz, the period of a
  32nd note at 300 BPM).  Pulses arriving more frequently than this
  still trigger the counter reset and ``bSyncPulseReceived`` flag,
  but do not corrupt the period.

The fix is verified to leave JACK-mode behaviour unchanged when the
sync checkbox is off (the entire late-hit code path is gated on
``fSyncMode > 0.0f``).  When the sync checkbox is on, the new fTempo
path produces a window size that is more stable than the previous
pulse-measurement value because it tracks engine state rather than
sync-bus signal noise.  See C1a below for one remaining stylistic
refinement.

------

C1a: BLOCK 7 / BLOCK 8 ordering refinement (follow-up to C1)
-------------------------------------------------------------

**Severity**: LOW.  **Live impact**: 1 (narrow within-buffer race).
**Confidence**: HIGH.

**Status**: OPEN — one-condition wrap, zero risk; deferred.

**Where**: plugin.cc BLOCK 8 (pulse-period capture).

**Mechanism**: with the C1 fix in place, BLOCK 7 (fTempo path) and
BLOCK 8 (pulse path) can both update ``lLastSyncPeriodSamples`` within
the same buffer.  BLOCK 8 runs *after* BLOCK 7 in each ``run()`` call,
so in pulse-arrival buffers the authoritative fTempo value is briefly
overwritten by the pulse-measured value.  The next buffer's BLOCK 7
reasserts the correct value, so the overwrite lives at most one
buffer (~10 ms at 512 samples / 48 kHz).

**Why it (slightly) matters**: if a button press lands in a
pulse-arrival buffer, the late-hit window for that press is computed
from the pulse-measured period instead of the fTempo-derived period.
In a well-synced setup the two values agree closely; in a poorly-synced
setup (transport jitter, clock skew) they could disagree by a few
percent, affecting the window size for those specific presses.  This
is also the behavioural reason the in-source comment "fTempo > 0
supersedes anything written here" is technically misleading — the
comment refers to BLOCK 7 reasserting the value on the *next* buffer,
not within the same buffer.

**Direction for fix**: gate BLOCK 8's
``pLS->lLastSyncPeriodSamples = pLS->lGlobalSamplesSinceSync;`` on
``fTempo == 0.0f`` so the two paths are mutually exclusive.  Single
additional condition wrap around an existing line; no behavioural
change in any case BLOCK 7 already handled correctly.  Risk of new
bug is essentially zero — the change only restricts when an overwrite
happens, never expands it.

------

C2: Inter-channel asymmetry in internal sync mode
-------------------------------------------------

**Severity**: MEDIUM.  **Live impact**: 2.  **Confidence**: HIGH.

**Status**: ✓ IMPLEMENTED (Option C — late-hit bypass flag).

**Where**: looper.cpp new BLOCK 5 in ``run()``; plugin.cc BLOCKS 6, 9,
10, 11; plugin.hpp (new ``bLateHitBypassed`` member).

**Mechanism (original bug)**

In internal sync mode (``_use_sync_buf == _our_syncin_buf ||
_our_syncout_buf``), ``Looper::run()`` forces::

    ports[Sync] = 0.0f;       // channel 0 → fSyncMode = 0 (immediate)
    _slave_sync_port = 1.0f;  // channel 1+ → fSyncMode = 1 (sync-wait)

A button press delivered via ``ports[Multi]`` reaches both channels in
the same ``run_loops()`` call.  With ``fSyncMode = 0`` the master takes
the immediate start/stop path; with ``fSyncMode = 1`` the slave may
enter the late-hit window path, backfilling to the previous sync
boundary while the master already started at the press time.  Result:
L/R divergence of up to ``g_late_hit_window_ms`` ms — audible as a
"flam" on transients or stereo image smearing at loop boundaries.

**Fix (Option C — bypass flag)**

A new ``bool bLateHitBypassed`` member is added to ``SooperLooperI``
(plugin.hpp, new albBegin block).  A new albBegin block in
``Looper::run()`` — inserted after the ``_slave_sync_port`` setup
(BLOCK 4), before ``run_loops()`` — iterates over all instances::

    pLS->bLateHitBypassed = inInternalSync && (i > 0);

This uses a direct ``static_cast`` from ``LADSPA_Handle`` to
``SooperLooperI*`` — safe because looper.cpp already includes plugin.hpp
and both compile into the same executable.

The three ``withinWindow`` booleans in BLOCKS 9, 10, 11 of plugin.cc
each gain ``&& !pLS->bLateHitBypassed``.

In internal sync mode: slave's late-hit correction is disabled; both
channels take the immediate path; behaviour returns to stock
SooperLooper (which already achieves sample-accurate alignment via the
master's ``pfSyncOutput`` pulse on ``STATE_RECORD`` entry).

**Why Option C over alternatives**

Option A (``_slave_sync_port = 0.0f``) would disable the slave's
sync-wait entirely, breaking multi-loop cross-sync scenarios.

Option B (``fSyncMode = -1.0f`` sentinel on master) would enable
late-hit on both channels, but requires a mandatory audit of all 21
active ``fSyncMode == 0.0f`` sites in the stock DSP state machine.
The change at each site is mechanical (``==`` → ``<=``) but missing
any one silently breaks the master in internal sync mode.  Option B
remains viable if full late-hit correction in internal sync becomes a
requirement; the audit is contained to internal-sync mode only and
carries **zero JACK regression risk**.

Option C was chosen: touches no stock DSP code, requires no audit, and
has zero JACK regression risk.

**Trade-off**: late-hit correction is inactive in internal sync mode.
Both channels correct each other's timing issues equally (both
uncorrected) rather than one being corrected and the other not.  For
the primary JACK sync workflow this is invisible.



C3: JACK client disconnect / reconnect — no handling
----------------------------------------------------

**Severity**: CRITICAL.  **Live impact**: 5 (if JACK restarts during
a gig).  **Confidence**: HIGH.

**Status**: DEFERRED — peer apps crash similarly on JACK restart; error reporting deemed sufficient.

**Where**: engine.cpp BLOCK 7 (port registration in ``initialize()``).
No teardown / re-registration code anywhere.

**Mechanism**: ``_jack_midi_in_port`` and ``_jack_midi_out_port`` are
registered once in ``Engine::initialize()`` and never touched again
except for reads/writes.  If the JACK server restarts (user kills it,
PipeWire-JACK reconfigures, hardware glitch triggers a JACK
auto-restart), the port handles become invalid.  The engine's next
``jack_port_get_buffer()`` call on a stale handle is undefined
behaviour — most likely a segfault.

The stock SooperLooper code does not register a JACK shutdown handler
either, so a JACK restart kills the engine regardless.  But the new
JACK MIDI ports widen the surface area where this matters.

**Why it matters live**: a JACK restart mid-performance is rare but
recoverable in stock SooperLooper (the audio path goes silent until
JACK is back; user can recover).  With the new MIDI ports, the
recovery path is more likely to crash.

**Direction for fix**:

a) **Minimum viable**: register a ``jack_on_shutdown()`` callback in
   the JACK driver.  In the callback, set a "JACK is gone" flag and
   null all JACK-dependent pointers.  All RT-callback consumers
   (process() in engine.cpp, the MIDI in/out loops) check the flag
   and return early.

b) **Better**: handle reconnection.  On JACK reconnect, re-register
   ports.  This requires either a polling thread or a JACK-provided
   notification.  Substantial work.

c) **Pragmatic**: document the limitation prominently and add an
   ``end_conversation``-style shutdown in the engine so a JACK loss
   at least exits cleanly rather than crashing.

For live performance, (a) is the minimum bar.


C4: JACK MIDI events silently dropped on binding-lock contention
----------------------------------------------------------------

**Severity**: LOW (analysed and dropped from active work).
**Live impact**: effectively 0 in normal use.  **Confidence**: HIGH.

**Status**: N/A — analysed and removed from active list; see section.

**Original concern**: ``MidiBridge::inject_midi()`` in the RT callback
uses a ``TentativeLockMonitor`` on ``_bindings_lock``.  If the main
thread holds the lock (binding modification), a MIDI event is silently
dropped.

**Analysis result**: the write sites on the main thread that hold
``_bindings_lock`` are: MIDI learn completion, add/remove binding via
GUI or OSC, query of all bindings (``GetAll``), clear all bindings,
and session-load of bindings.  Full trace of each:

- *Session load*: only at startup; any events dropped at that moment
  are inconsequential (foot controller not yet in active use).
- *MIDI learn*: a setup-only activity; not performed during a set.
- *Binding add/remove via GUI*: only while the binding editor is open
  and being edited; not a live-performance activity.
- *``GetAll`` query*: fired only on an explicit GUI request for the
  binding list — when the binding editor panel is opened, not on
  every normal state-polling cycle.  Regular loop-state polling
  (positions, record states, levels) does **not** touch the binding
  lock at all.

During normal live performance with slgui open but the binding editor
not in use, ``_bindings_lock`` is never held on the main thread.
``inject_midi``'s try-lock succeeds on every event with no contention.

**Conclusion**: the conditions for a drop do not arise in the target
use case.  A drop counter would not be visible to a performer in
real-time, and a full lock-free rewrite of the binding table is
disproportionate to the actual risk.  C4 is removed from the active
work list.


Critical issues — robustness
=============================

C5: ``lGlobalSamplesSinceSync`` overflow after ~25 hours
---------------------------------------------------------

**Severity**: CRITICAL.  **Live impact**: 1–2 (long rehearsals / festival
all-day sessions only).  **Confidence**: HIGH.

**Status**: ✓ IMPLEMENTED — widened to ``unsigned long long``.

**Where**: plugin.hpp (field type ``unsigned int``) and plugin.cc
BLOCK 8 (increment site).

**Mechanism**: ``lGlobalSamplesSinceSync`` is ``unsigned int`` (32-bit
on this platform) and increments once per sample.  At 48 kHz it
overflows after ``2^32 / 48000 ≈ 89,478`` seconds (24.85 hours).
After overflow it wraps to 0 — which the late-hit code reads as
"sync pulse just fired", potentially triggering ``withinWindow`` on a
press that is actually nowhere near a sync boundary.

**Why it matters live**: not a gig issue (no gig lasts 25 hours), but
a multi-day rehearsal rig running continuously could hit it.  More
concerning is that the moment of wrap is *deterministic*: if the rig
boots and then sits idle, the wrap will happen at exactly the same
time every session, which could surface as a mysterious "weird late-hit
behaviour after exactly 24 hours" report.

**Direction for fix**: widen to ``unsigned long long`` (64-bit).
``lLastSyncPeriodSamples`` should probably be widened to match.
Trivial change.

**Implementation status**: Implemented.

- plugin.hpp: ``unsigned int lGlobalSamplesSinceSync`` widened to
  ``unsigned long long``.

- plugin.cc BLOCK 8: ``unsigned int lGlobalSyncAtButtonPress`` widened
  to ``unsigned long long`` to match the struct field.  Three ``%u``
  format specifiers in ``SYNCWDBG(SL_LOG_RT(...))`` calls changed to
  ``%llu``.

``lLastSyncPeriodSamples`` was already declared as ``unsigned long``,
which on 64-bit Linux is 64-bit and does not overflow within any
practical session length.

**Portability caveat (not a current regression)**: the local variable
``T`` in BLOCKS 9, 10, 11 remains ``unsigned long``, which is 64-bit
on Linux x86-64 but 32-bit on hypothetical 32-bit builds.  On the
target platform no data is lost in the cast from ``unsigned long long``
to ``unsigned long``; on a 32-bit build the overflow horizon for ``T``
would remain at ~25 hours.  This is not a concern for the current
codebase but should be noted if 32-bit support is ever required.

------

C6: ``bSyncPulseReceived`` never reset on sync-source change
------------------------------------------------------------

**Severity**: HIGH.  **Live impact**: 2.  **Confidence**: MEDIUM.

**Status**: OPEN.

**Where**: plugin.cc BLOCK 8 sets the flag; BLOCK 6 resets it only in
``activate()``.  No path resets it on sync-source change.

**Mechanism**: the flag exists to prevent late-hit from firing before
any real sync has been observed (since calloc-zeroed state would
otherwise satisfy ``T <= shiftSamples`` spuriously).  If the user
switches sync source mid-session (e.g. from JackSync to internal Loop 1,
or to NoSync and back), the flag stays true.  ``lLastSyncPeriodSamples``
carries over from the previous sync source.  ``lGlobalSamplesSinceSync``
keeps running on its old value.

The next late-hit decision uses period data from the *old* sync source,
which may be wildly wrong for the new one.

**Why it matters live**: the performer changes sync source between
songs (jam mode vs. metronome mode).  The first few presses after the
switch can fire the late-hit incorrectly.

**Direction for fix**: hook the sync-source change event (engine.cpp
has ``update_sync_source()``) and call into each loop to reset
``bSyncPulseReceived``, ``lGlobalSamplesSinceSync``, and
``lLastSyncPeriodSamples`` (back to ULONG_MAX sentinel).  A
straightforward additional path in engine.cpp.

------

C7: ``_pending_event_offset`` ordering vs. multiple events per buffer
----------------------------------------------------------------------

**Severity**: N/A — verified safe by code audit.  **Live impact**: 0.
**Confidence**: HIGH.

**Status**: CLOSED — invariant holds; comment added to looper.cpp.

**Verification**

The concern was that ``_pending_event_offset`` (set in ``do_event()``)
might not be consumed by a subsequent ``run()`` before the end of the
buffer, or might leak into the next callback and incorrectly zero
``TriggerLatency`` for an ALSA event.

Audit of ``Engine::process()`` shows the invariant is structurally
guaranteed:

1. For each event at ``fragpos = N``, the engine calls
   ``run(usedframes, N - usedframes)`` **before** ``do_event(evt)``
   (not after).  ``do_event()`` sets ``_pending_event_offset = N``.

2. After the last event, the engine unconditionally fires a final
   ``run(usedframes, nframes - usedframes)`` for all instances — even
   if ``nframes - usedframes == 0``.  That zero-length run still enters
   BLOCK 5, which resets ``_pending_event_offset = 0``.

3. ALSA/OSC events always have ``FragmentPos() == 0``, so
   ``_pending_event_offset = 0`` after their ``do_event()``.  BLOCK 5's
   gate (``if (_pending_event_offset > 0)``) is dormant; ``TriggerLatency``
   is never incorrectly zeroed for ALSA events regardless of ordering.

4. Multiple events at the same ``fragpos = N``: the intermediate
   zero-length run between them resets ``_pending_event_offset = 0``, and
   the following ``do_event()`` sets it back to ``N`` (same value).
   Result is identical to a single event at that position.

5. The "lingering offset contaminating next buffer" scenario cannot occur
   because the unconditional final run always resets the value before the
   JACK callback returns.

**Action taken**: an invariant comment added to looper.cpp BLOCK 2
(``do_event``) documenting items 1–5 and citing this analysis.  No code
change needed.

------

C8: ``pushNewLoopChunk`` failure recovery is silent
---------------------------------------------------

**Severity**: HIGH.  **Live impact**: 2–3 (only on memory exhaustion).
**Confidence**: HIGH.

**Status**: OPEN.

**Where**: plugin.cc BLOCK 10 (late-start failure path) — now goes to
``STATE_OFF`` instead of crashing.

**Mechanism**: the recent fix correctly avoids the
``loop == NULL && state == STATE_PLAY`` crash by entering ``STATE_OFF``.
But ``STATE_OFF`` is silent — the performer pressed RECORD, nothing
happened, no LED feedback, no OSC notification.  They keep playing,
unaware no recording happened.

**Why it matters live**: a missed loop attempt mid-performance is
catastrophic.  The performer needs an *immediate* indication that the
attempt failed.

**Direction for fix**: emit a state-change event the GUI / OSC clients
can subscribe to.  The existing event mechanism likely already supports
this (the ``State`` output port changes); ensure that downstream
clients distinguish "intended STATE_OFF (user pressed undo)" from
"unexpected STATE_OFF (OOM)".  Adding a separate ``Error`` output port
is heavier but unambiguous.

------

C9: Backfill audio is stale if input was silent before press
-------------------------------------------------------------

**Severity**: MEDIUM.  **Live impact**: 2.  **Confidence**: HIGH.

**Status**: OPEN.

**Where**: plugin.cc BLOCK 10 (backfill loop reads from
``pfInputLatencyBuf``).

**Mechanism**: the input latency ring buffer is written every sample
unconditionally — there is no concept of "valid data start".  If the
performer was silent for the past 100 ms (the late-hit window's
default size) and then plays a note immediately followed by pressing
RECORD slightly late, the backfill captures the silence before the
note.  Fine, no problem.

The problem case: a long silent gap before the press.  The ring buffer
still contains audio from *much earlier* (whenever the buffer last
wrapped during real input).  If the press happens within the late-hit
window after a sync boundary but the boundary itself was in silence,
the loop's leading T samples could be old, unrelated audio from the
ring buffer's previous wrap.

In practice this is rarely audible because the boundary is "now minus
T" and T is at most 100 ms — but with a fast tempo and a long silent
gap before a downbeat, the leading edge of the loop could carry an
audible ghost.

**Why it matters live**: a quiet sustained note at the start of a
loop could be preceded by a phantom tail from an earlier riff.

**Direction for fix**: track the most recent input-non-silent sample
position.  If the requested backfill range extends earlier than that,
zero those samples instead.  Cheap to track (one threshold check
per sample, one max() per sample).

------

C10: ``loop->lLoopStart + i`` aliasing concern in backfill
-----------------------------------------------------------

**Severity**: MEDIUM.  **Live impact**: 1.  **Confidence**: MEDIUM.

**Status**: OPEN.

**Where**: plugin.cc BLOCK 10 backfill copy loop.

**Mechanism**: the backfill writes ``T`` samples into
``pLS->pSampleBuf[(loop->lLoopStart + i) & pLS->lBufferSizeMask]``.
This relies on:

- ``loop->lLoopStart`` having been set sensibly by ``pushNewLoopChunk``
- ``pLS->lBufferSize`` being a power of two (mask validity)
- No other chunk occupying the same ``T`` samples

``pushNewLoopChunk`` is stock code; it places new chunks contiguously
after the previous tail.  As long as the chunk is freshly allocated
(which it is in the late-hit path), the range
``[loop->lLoopStart, loop->lLoopStart + T)`` is exclusive to this
chunk and the write is safe.

The concern is theoretical: if a future change reuses chunks instead
of allocating fresh ones (memory pool exhaustion fallback, say), the
backfill could clobber another chunk's tail.  Today this is not a
real bug.

**Direction for fix**: add a comment near the backfill that documents
the invariant ("``pushNewLoopChunk`` returns a fresh range; backfill
safe up to ``lBufferSize``").  No code change.


High-priority issues — JACK MIDI
=================================

H1: MIDI Start and first Clock at the same frame offset
-------------------------------------------------------

**Severity**: HIGH.  **Live impact**: 2.  **Confidence**: HIGH.

**Status**: OPEN.

**Where**: engine.cpp BLOCK 9 (JACK MIDI output generation).

**Mechanism**: when transport transitions from stopped to rolling:

1. MIDI Start (0xFA) is written at offset 0
2. ``_jack_clock_tick_phase`` is reset to 0.0
3. The while loop immediately emits the first clock tick at offset 0
   (because ``phase = 0.0 < nframes``)

So two MIDI events appear at the exact same timestamp.  JACK MIDI
permits this, but many external devices have firmware that processes
events sequentially without considering simultaneous arrivals.
Behaviour varies — some treat Start before Clock (correct), some
treat them as out of order, some drop one.

**Why it matters live**: an external synth fed clock from SooperLooper
may exhibit a beat-1 phase offset that's noticed only after several
bars.

**Direction for fix**: after writing MIDI Start, set
``_jack_clock_tick_phase = tpf`` (one tick into the future) so the
first clock fires one tick after Start.  Or write Start at offset 0
and skip the first clock tick of the buffer entirely.

------

H2: Clock continues firing when transport is stopped
-----------------------------------------------------

**Severity**: N/A — confirmed intentional, matching ALSA.
**Status**: analysed and closed.

The JACK clock path (BLOCK 9) generates ticks whenever ``_tempo > 0``,
regardless of transport rolling state.  This was noted as a possible
issue and marked for verification against the ALSA clock thread.

**Verification result**: the ALSA clock thread in ``MidiBridge`` runs
continuously at whatever tempo was last given to it via
``tempo_clock_update()``.  It has no awareness of JACK transport state
and does not gate on rolling.  When JACK transport stops, the
``ALB_TEMPO_ONLY_WHEN_ROLLING`` guard (engine.cpp BLOCK 11) prevents
a new ``set_tempo()`` call from the JackSync branch, so the MidiBridge
thread simply continues clocking at the last known tempo — exactly
what the JACK path does.

Both paths are continuous clock when tempo is known.  The comment in
BLOCK 9 ("matching the ALSA clock thread's behaviour") is accurate.

**One genuine JACK-only addition**: the JACK path sends MIDI Stop
(0xFC) on transport stop transitions.  The ALSA path does not (it has
no concept of transport state).  This makes the JACK output *more*
correct than ALSA with respect to the MIDI spec, not less.

No action required.

------

H3: ``_jack_clock_tick_phase`` not reset on transport stop
-----------------------------------------------------------

**Severity**: N/A — non-issue given H2 analysis.  **Status**: closed.

Since the clock runs continuously (H2 confirmed), not resetting phase
on stop is correct and consistent.  An explicit ``= 0.0`` on stop
would be harmless but serves no purpose while the clock is running.
The asymmetry (start resets phase, stop does not) is intentional: the
phase carries across the stop/start gap so that downstream devices
which continue to count ticks stay in phase.

No action required.

------

H4: ``jack_midi_event_write`` return value never checked
---------------------------------------------------------

**Severity**: MEDIUM.  **Live impact**: 1.  **Confidence**: HIGH.

**Status**: OPEN — small fix recommended (explicit buffer-size ``#define``); see below.

**Where**: engine.cpp BLOCK 9 (clock write loop) and BLOCK 6 (port
registration).

**Mechanism**: ``jack_midi_event_write`` returns non-zero if the MIDI
output buffer is full.  The current code ignores this.  In practice the
JACK MIDI buffer holds far more data than the clock path ever produces:
at the most demanding realistic case (300 BPM, 512-sample buffer,
48 kHz) the clock loop emits about 5 one-byte events per buffer.  Each
JACK MIDI event consumes approximately 9 bytes of buffer space (JACK's
internal framing overhead + 1 byte of data), so the maximum usage is
~45 bytes per buffer.  The default JACK MIDI buffer is typically 4 KB,
giving ~90× headroom.

**Direction for fix**: make the buffer capacity explicit rather than
relying on the JACK default.  Add a ``#define`` at the top of the BLOCK
6 area in engine.cpp and pass it to ``jack_port_register``::

    #ifndef SL_JACK_MIDI_OUT_BUFSIZE
    #define SL_JACK_MIDI_OUT_BUFSIZE 4096
    #endif
    // ...
    _jack_midi_out_port = jack_port_register(jclient, "midi_out",
                                             JACK_DEFAULT_MIDI_TYPE,
                                             JackPortIsOutput,
                                             SL_JACK_MIDI_OUT_BUFSIZE);

With an explicit 4096-byte request and a documented worst-case usage
of ~45 bytes, the return value check becomes a formal no-op and can
be added as a debug assertion without cluttering the live path::

    if (jack_midi_event_write(buf, offset, &byte, 1) != 0) {
        // should never happen with SL_JACK_MIDI_OUT_BUFSIZE bytes
    }

A warning to the debug log is not worth the effort for the expected
failure rate of zero.


High-priority issues — RT safety
=================================

R1: ``SL_LOG_RT`` still uses ``fprintf`` to a buffered ``FILE *``
-----------------------------------------------------------------

**Severity**: MEDIUM.  **Live impact**: 0 in release builds, 1–2 in
debug builds.  **Confidence**: HIGH (xruns confirmed in testing).

**Status**: ACCEPTED — confirmed xrun source in ``SYNCWDEBUG`` debug
builds.  Accepted as the known cost of enabling debug output.  Zero
impact in release builds (``SYNCWDEBUG`` undefined).

**Where**: plugin.cc BLOCK 1 (``SL_LOG_RT`` macro), called from
inside ``SYNCWDBG(...)`` in BLOCKS 9, 10, 11.

**Mechanism**: ``fprintf`` is not RT-safe — it acquires a per-``FILE``
lock (``flockfile``) and eventually triggers a ``write()`` syscall when
the stdio buffer fills.  Both can block the RT thread.  Xruns were
observed and confirmed to disappear when ``SYNCWDEBUG`` was commented
out.

The truly safe fix (SPSC ring buffer drained by a non-RT thread,
~60 lines) was evaluated and consciously declined: the effort is
disproportionate to the benefit since the issue only manifests in
debug builds.

**Accepted state**: comment out ``#define SYNCWDEBUG`` or undefine
it at compile time for any performance build.  The macro is a
compile-time switch with zero cost when off.


R2: ``sl_dbg_open`` data race (residual)
-----------------------------------------

**Severity**: LOW.  **Live impact**: 1.  **Confidence**: HIGH.

**Where**: plugin.cc BLOCK 1.

**Status**: OPEN — one-line fix (``std::call_once``); low priority.

**Mechanism**: the previously identified two-channel race on
``sl_dbg_open()`` is reduced by ``SL_LOG_RT`` (which doesn't call
``sl_dbg_open``).  ``SL_LOG`` is now only called from ``instantiate``
and ``activate``, both non-RT.  In the standalone build the LADSPA
host instantiates channels sequentially on the main thread, so no
race occurs in practice.

The race is theoretically possible if the LADSPA host instantiates
channels concurrently from multiple threads.  Not the case here.

**Direction for fix**: optional — wrap ``sl_dbg_open()`` in a
``std::call_once`` or equivalent.  One-line fix; cost is the
``<mutex>`` include.

------

R3: ``ALB_RT_SAFE_COND_SIGNAL`` suppresses signal — main-loop responsiveness depends on timeout
------------------------------------------------------------------------------------------------

**Severity**: N/A — timeout verified as 10 ms; delay is imperceptible.
**Live impact**: 0.  **Confidence**: HIGH.

**Status**: CLOSED — timeout confirmed, effect is negligible.

**Verification**

The concern was that suppressing ``pthread_cond_signal`` would leave
the main event loop waiting for its ``timedwait`` timeout before
processing tempo changes, potentially causing noticeable GUI lag.

The actual timeout is **10 ms**, stated explicitly in the stock code::

    // wake up every 10 ms for servicing auto-update parameters
    const long up_interval = AUTO_UPDATE_STEP * 1000; // 10ms

So the maximum delay from signal suppression before the main loop
wakes is one timeout cycle: **10 ms**.

**What is delayed by up to 10 ms** (non-RT consumers):

- GUI OSC auto-update messages (tempo, loop state, positions).  The
  GUI likely polls at 50–100 ms anyway; 10 ms jitter is invisible.
- ALSA clock thread ``tempo_clock_update()`` call.  For a JACK sync
  user this is secondary.
- OSC replies to tempo queries.

**What is completely unaffected** (RT-generated):

- JACK MIDI clock output (BLOCK 9, generated in the RT process
  callback at exact sample accuracy).
- Sync pulses to the looper (``generate_sync()``, RT).
- All loop recording, playback, and state-machine transitions.

A 10 ms delay before the GUI tempo display updates after a tempo
change is imperceptible and has no effect on performance.  No action
required.

------

R4: ``ALB_TEMPO_ONLY_WHEN_ROLLING`` skips tempo updates when stopped
---------------------------------------------------------------------

**Severity**: MEDIUM.  **Live impact**: 2.  **Confidence**: HIGH.

**Status**: OPEN — needs confirmation; may already work.

**Where**: engine.cpp BLOCK 12.

**Mechanism**: when transport is stopped, ``set_tempo()`` is not
called.  Rationale: avoid the non-RT-safe ``tempo_clock_update()``
firing during locate transitions.

The side effect: if the user changes the tempo (via the GUI or OSC)
while transport is stopped, the change is not seen by the engine
until transport starts rolling.  The first beat after start uses
the *old* tempo.

This is a real correctness issue in some workflows: a band changes
tempo between songs while transport is stopped, then starts — and
the first bar is at the wrong tempo until the engine catches up.

**Direction for fix**: distinguish tempo changes from *JACK Transport*
(which is what BLOCK 12 gates) from tempo changes from OSC / GUI
(which should propagate immediately).  The current gate only affects
the JACK-derived ``info.bpm`` path; OSC/GUI tempo changes go through
``set_tempo()`` directly.  So this concern may already be moot — but
worth verifying with a test (change tempo via OSC with transport
stopped, then start, check first-beat tempo).

------

R5: ``Engine`` constructor null-init of JACK members not atomic
----------------------------------------------------------------

**Severity**: LOW.  **Live impact**: 1.  **Confidence**: MEDIUM.

**Status**: OPEN — one-line comment; lowest priority.

**Where**: engine.cpp BLOCK 5 (constructor init).

**Mechanism**: the constructor sets ``_jack_midi_in_port = nullptr``
etc. before ``initialize()`` is called.  Between construction and
initialization the engine is not yet wired into the JACK callback,
so there is no concurrent reader.  Safe by construction.

If a future refactor changes that lifecycle — e.g. the JACK callback
runs before ``initialize()`` finishes — the null pointers protect
against crashes but the RT thread will silently produce no MIDI.
A clear comment about the lifecycle invariant is the only
maintenance need.

**Direction for fix**: add a comment.  No code change.


Latent design observations
===========================

D1: ``g_late_hit_window_ms`` as a file-scope global
----------------------------------------------------

**Severity**: LOW.  **Live impact**: 1.  **Confidence**: HIGH.

**Status**: OPEN — future enhancement.

**Where**: plugin.cc BLOCK 1.

The handoff documents the rationale: avoid the LADSPA port system
for a config-time value.  The cost is that the value cannot be
changed at runtime (OSC, GUI) — only by editing the session file
and reloading.  For a live-performance feature this might be a
limitation: the performer wants to widen the window on songs with
slow tempo, narrow it on fast songs.

**Direction**: revisit the global-vs-port decision once the feature
stabilises.  If runtime tuning is desired, promoting to a real
control port is the standard path.

------

D2: ``SYNCWDEBUG`` is compile-time, not runtime
------------------------------------------------

**Severity**: LOW.  **Live impact**: 1.  **Confidence**: HIGH.

**Status**: OPEN — future enhancement.

**Where**: plugin.cc BLOCK 2.

Toggling debug verbosity requires recompile + restart.  For a
performance workflow, this is friction.

**Direction**: replace ``#ifdef SYNCWDEBUG`` with a runtime boolean
(possibly settable via OSC or session file).  The performance cost
of an always-evaluated ``if (g_syncwdebug)`` guard around each call
is negligible compared to the ``fprintf`` itself.

------

D3: Rate ≠ 1.0 playback uses ``run_loops`` without SRC interpolation
---------------------------------------------------------------------

**Severity**: LOW (stock behaviour).  **Live impact**: 1.
**Confidence**: HIGH.

**Status**: N/A — confirmed stock behaviour; not a regression.

**Where**: looper.cpp BLOCK 5; original stock ``Looper::run()``.

**Background**: in the original SooperLooper 1.7.9 stock code, the
call site reads:

.. code-block:: cpp

    run_loops (offset, nframes);
    /*
        if (ports[Rate] == 1.0f) {
            run_loops (offset, nframes);
        }
        else {
    #ifdef HAVE_SAMPLERATE
            run_loops_resampled (offset, nframes);
    #else
            run_loops (offset, nframes);
    #endif
        }
    */

The ``/* ... */`` block was already commented out in stock code.
``run_loops_resampled`` has never been active in this codebase.
Variable-rate playback (Rate port ≠ 1.0) has always been handled
by ``run_loops()`` using ``dCurrPos`` stepping without SRC
anti-aliasing.

An earlier audit session incorrectly labelled this a "silent
regression introduced by the TriggerLatency wrapper patch" (XRUN 3
in a previous iteration of this document).  That was wrong: BLOCK 5
correctly preserved the stock ``run_loops()``-only behaviour; the
rate-dispatch block was commented out before any of these
modifications existed.  At one point during patching the ``/* */``
delimiters were accidentally removed, activating the block and
causing a linker error (``run_loops_resampled`` is referenced by
the ``#ifdef HAVE_SAMPLERATE`` guard but its body does not exist
at link time); this was subsequently corrected.

**Current state**: BLOCK 5 calls ``run_loops (offset, nframes)``
unconditionally, wrapped in the TriggerLatency save/restore.  This
is the correct and intentional behaviour, matching stock.

**Direction**: if SRC-quality rate shifting is ever desired, the
correct path is to implement and test ``run_loops_resampled`` from
scratch (or port it from a version of SooperLooper where it was
complete) and then expose the dispatch through BLOCK 5.  This is
future work and not a regression to fix.

------

D4: No regression test infrastructure
--------------------------------------

**Severity**: LOW.  **Live impact**: 1 (compounds future changes).
**Confidence**: HIGH.

**Status**: OPEN — future work.

There are no automated tests for any of the new behaviour: late-hit,
JACK MIDI in/out, TriggerLatency zeroing, slave sync.  Each change
has been validated by hand-testing.  This is appropriate for the
exploratory phase but becomes a liability as the codebase grows.

**Direction**: when the modifications stabilise, capture the
hand-test procedures as a runbook (record loop with X, expect Y).
Automate the ones that don't require human ears (state-machine
transitions, port registration, session save/load round-trip).

------

D5: ``albBegin``/``albEnd`` blocks mix new and modified code
-------------------------------------------------------------

**Severity**: LOW.  **Live impact**: 0.  **Confidence**: HIGH.

**Status**: ✓ IMPLEMENTED across all eight modified files.

The block convention is good for marking patch surface but
inconsistent in practice — some blocks contain only new code (clean
additions), others wrap existing stock code with modifications
(e.g. looper.cpp BLOCK 3, BLOCK 5).  This makes line-by-line review
harder: a reviewer cannot trust that everything inside a block is
"new".  The ``run_loops_resampled`` linker error was a direct
consequence of this gap: the stock ``/* */`` comment around the
rate-dispatch block was not documented inside BLOCK 5, so a subsequent
edit accidentally removed the comment delimiters and activated dead code.

**Implementation status**: COMPLETED for all uploaded files.
A full diff of every modified file against the stock SooperLooper 1.7.9
originals was performed.  The following changes were made:

- **looper.cpp**: the constructor ``_slave_sync_port`` assignment
  (``1.0f`` → ``ports[Sync]``) had no alb markers — wrapped in a new
  albBegin/albEnd block with stock comment added.  BLOCKS 3, 4, and 5
  had their modified-stock code documented with
  ``// STOCK (sooperlooper 1.7.9):`` comments at the top of each block.

- **plugin.cc BLOCKS 9 and 10**: stock comments added showing the
  original ``STATE_TRIG_STOP`` else-branch and ``STATE_TRIG_START``
  default case.  The scope-braces wrapping the ``default:`` case for
  BLOCK 10's local variables were outside the alb markers; moved inside.

- **engine.cpp**: already had ``// Original:`` comments in all
  modified blocks; no changes needed.

- **plugin.hpp, looper.hpp, engine.hpp**: all additive; no stock
  comments required.

**Remaining gaps (files not uploaded for D5 review)**:

``audio_driver.hpp`` and ``jack_audio_driver.hpp`` were uploaded in a
subsequent iteration and have now been reviewed.

- **audio_driver.hpp**: two additive albBegin blocks (``jack_client_t``
  forward declaration; ``get_jack_client()`` default virtual returning
  ``nullptr``) were already properly marked.  One real semantic unguarded
  change was found and wrapped: the ``AudioDriver`` class declaration
  had ``: public sigc::trackable`` added without an alb block.  The
  stock class had no base class; the change is a safe, source-compatible
  addition (enables automatic sigc++ slot cleanup on destruction).  Now
  wrapped with stock comment.  Eleven additional "unguarded" lines
  reported by the automated scanner are whitespace normalization only
  (field/method alignment, comment reformatting) — confirmed identical
  semantics to stock; no alb wrapping needed.

- **jack_audio_driver.hpp**: one albBegin block adding
  ``get_jack_client() override { return _jack; }`` — purely additive,
  properly marked, no unguarded changes.  No D5 work needed.

**D5 is now complete across all eight modified files.**


Prioritised improvement roadmap
================================

The following ordering reflects live-performance risk.  Items earlier
in the list should be addressed before a serious gig; items lower
down are appropriate for the next maintenance cycle.

**Pre-gig blockers**:

1. **C3** — JACK shutdown handler (one callback registration).
   Status: DEFERRED — peer applications fail similarly on JACK
   restart; improved error reporting deemed sufficient for current
   needs.
2. **C1** ✓ COMPLETED — ``lLastSyncPeriodSamples`` now derived from
   fTempo (BLOCK 7) with sanity floor in pulse path (BLOCK 8).  The
   most user-visible correctness bug actively present.
3. **C2** ✓ COMPLETED — ``bLateHitBypassed`` flag; slave late-hit
   disabled in internal sync mode; consistent with stock behaviour.
4. **C4** N/A — analysed and dropped; see C4 section.
5. **C8** — surface ``STATE_OFF`` recovery to the GUI so failed
   recordings don't look like silent successes.

**Should-fix before extended use**:

6. **C5** ✓ COMPLETED — ``lGlobalSamplesSinceSync`` widened to
   ``unsigned long long``; ``lGlobalSyncAtButtonPress`` widened to
   match; format specifiers updated.
7. **C6** — reset late-hit state on sync-source change.  Affects
   set-change workflows.
8. **H1** — MIDI Start + first clock at same offset.  Still open;
   small one-line fix in engine.cpp BLOCK 9.
   **H2** N/A — confirmed matches ALSA continuous-clock behaviour.
   **H3** N/A — follows H2; no action needed.
9. **H4** — make MIDI out buffer size explicit via
   ``#define SL_JACK_MIDI_OUT_BUFSIZE 4096`` passed to
   ``jack_port_register``; documents the safety margin.  Small change.
10. **R4** — verify OSC tempo changes propagate when transport is
    stopped (may already work, just needs confirmation).

**Maintenance / hygiene**:

11. **C1a** — gate BLOCK 8 period capture on ``fTempo == 0.0f``.
12. **C7** N/A — verified: ``do_event()`` → ``run()`` invariant holds;
    ``_pending_event_offset`` always reset before buffer end.  Comment
    added to looper.cpp BLOCK 2.
13. **C9** — input-non-silent tracking for backfill.
14. **C10** — backfill safety comment.
15. **R2** — ``std::call_once`` for ``sl_dbg_open``.
16. **R3** N/A — timeout is 10 ms; delay before non-RT consumers see a
    tempo change is imperceptible.  No action needed.
17. **R5** — engine constructor lifecycle comment.

**Future architecture**:

17. **D1** — promote ``g_late_hit_window_ms`` to a real port if
    runtime tuning is wanted.
18. **D2** — runtime ``SYNCWDEBUG`` toggle.
19. **D3** — Rate ≠ 1.0 without SRC is stock SooperLooper behaviour,
    not a regression.  If SRC-quality rate shifting is wanted, it
    requires implementing ``run_loops_resampled`` from scratch.
20. **D4** — runbook + automated tests for stable functionality.
21. **D5** ✓ COMPLETED across all eight modified files.  Stock-code
    comments added to all modified blocks; one unguarded ``sigc::trackable``
    base-class addition in ``audio_driver.hpp`` was wrapped.  The trailing-
    space removal in ``plugin.cc`` is whitespace-only; eleven whitespace-
    normalisation lines in ``audio_driver.hpp`` are semantic no-ops.
22. **R1** — lock-free log buffer: confirmed xrun source.  ``SL_LOG_RT``
    uses ``fprintf`` which acquires the C stdio mutex and makes a kernel
    ``write()`` syscall — both can block the RT thread.  The ring-buffer
    fix (~60 lines, engine.cpp drain) was evaluated and consciously
    deferred: ``SYNCWDEBUG`` is a compile-time flag, so xruns only occur
    in debug builds.  Accepted as the known cost of enabling that mode.

------

Notes on hot-path performance
==============================

The hot path of interest (called every buffer, 86–344 times per
second at 48 kHz with 128–512-sample buffers) is:

::

    Engine::process()
      → JACK MIDI read loop                  (BLOCK 8 in engine.cpp)
      → MidiBridge::inject_midi()            (stock)
      → get_read_vector() / event merge      (stock)
      → for each loop:
          → Looper::run()
            → request_pending bookkeeping    (BLOCK 3 in looper.cpp)
            → _slave_sync_port assignment    (BLOCK 4 in looper.cpp)
            → run_loops()  or                (BLOCK 5 in looper.cpp)
              run_loops_resampled()
                → runSooperLooper()          (plugin.cc; LADSPA run())
                  → input ring write         (stock; per-sample copy)
                  → sync pulse scan          (BLOCK 8 in plugin.cc; per-sample)
                  → button-press dispatch    (one-shot per buffer; BLOCKS 9–11)
                  → DSP state machine        (stock; per-sample)
      → JACK MIDI write loop                 (BLOCK 9 in engine.cpp)

Per-sample additions from this patch series (running unconditionally
every sample of every loop in every buffer, when ``fSyncMode >= 1.0f``):

1. One ``unsigned long long`` increment (``lGlobalSamplesSinceSync++``;
   widened from ``unsigned int`` by the C5 fix).
2. One float comparison (``pfSyncInput[n] > 1.5f``).
3. (Conditional on a sync pulse arriving) one integer comparison
   against ``lMinPlausiblePeriod`` (C1 floor) plus the period capture
   and counter reset.

These add roughly 2–3 cycles per sample, or about 0.1 % of the
sample-processing budget at 48 kHz on modern hardware.  Negligible.
When ``fSyncMode == 0.0f`` the entire block is gated off and per-sample
overhead is zero.

Per-buffer additions:

1. C1 fix — one float divide and two multiplies (``fSampleRate * 30 /
   fTempo * eighthPerCycle``) when ``fSyncMode >= 1.0f && fTempo > 0.0f``.
   Roughly 10–20 ns per buffer.
2. One unsigned-long computation of ``lMinPlausiblePeriod`` per buffer
   when ``fSyncMode >= 1.0f``.  Negligible.
3. JACK MIDI input read loop: ``O(events)``, typically 0–2 events
   per buffer.
4. JACK MIDI output write loop: ``O(ticks)``, typically 0–5 ticks
   per buffer.
5. TriggerLatency save / restore: two float assignments.
6. ``request_pending`` bookkeeping: one to three integer comparisons
   and assignments.

These add roughly 100–500 ns per buffer, or about 0.005 % of the
buffer budget at 512 samples / 48 kHz.  Negligible.

The only hot-path code with a potential pathological cost is the
``SYNCWDBG(SL_LOG_RT(...))`` calls on button presses.  These are
not per-sample; they fire on user input only.  At the worst credible
press rate (4 presses per second sustained), the logging overhead
is dominated by the ``write()`` syscall when the stdio buffer
flushes (~once per 100–200 presses).  An 8 KB stdio buffer drains
in ~10 µs on modern Linux, which is well under any plausible
buffer deadline.

Bottom line: per-buffer overhead from the patches is not a credible
xrun source.  If xruns are observed during JACK connection events,
the cause is almost certainly elsewhere — JACK's port-registration
callback, the LADSPA host's connect_port renegotiation, or the
mididings handler — none of which this patch series touches.


Closing note
=============

The patch series is, on the whole, well-designed and conservatively
scoped.  Most of the items listed above are refinements rather than
re-architecture.

Of the two highest-leverage items originally flagged by live-impact —
**C1** (lLastSyncPeriodSamples pollution, manifested in test logs) and
**C3** (JACK shutdown safety) — C1 has now been addressed via the
fTempo direct-computation path and a sanity floor on pulse capture.
C3 is deferred on the rationale that JACK restarts affect peer
applications similarly; improved error reporting is judged sufficient.
**C5** (counter overflow) was also completed in the same revision.

The audit deliberately includes some low-severity items.  They are
recorded not because they need imminent fixes but because, in
aggregate, they form the "defence-in-depth" stance appropriate for a
tool that will be trusted with a live performance.  Choose what to
take on based on rehearsal context and the next gig's stakes.

A small follow-up item (**C1a**) was introduced when implementing C1:
BLOCK 7 and BLOCK 8 can both write ``lLastSyncPeriodSamples`` in the
same buffer, with BLOCK 8 winning when a sync pulse arrives.  The
behaviour is functionally correct in steady state and self-corrects
within one buffer, but a one-condition refinement (gating BLOCK 8's
period capture on ``fTempo == 0.0f``) would make the two paths cleanly
exclusive and matches the existing in-source comment.  Risk of this
follow-up is essentially zero.
