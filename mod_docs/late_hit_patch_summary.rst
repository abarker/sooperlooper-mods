SooperLooper Late-Hit Recording Modification Summary
====================================================

| **Base version:** SooperLooper 1.7.9
| **Modified files:** ``src/plugin.cc``, ``src/plugin.hpp``, ``src/engine.cpp``,
| ``src/engine.hpp``, ``src/looper.cpp``, ``src/looper.hpp``, ``src/audio_driver.hpp``,
| ``src/jack_audio_driver.hpp``, and some test files in ``src/test_plugin``.
| **All new code sections** are bracketed with ``albBegin`` / ``albEnd`` markers.
| In ``plugin.cc`` and ``plugin.hpp`` the markers are ``//====== albBegin`` /
| ``//====== albEnd``; in all other files they are ``//------ albBegin`` /
| ``//------ albEnd``. Search for ``albBegin`` to locate every changed block.


.. tip:: **Quick Start**

   The late-hit correction is active immediately after compiling — no session file
   changes are required. The compile-time defaults (100ms ms cap, 0.5 eighth-note
   fraction cap) are used whenever the session file does not contain the late-hit
   keys, and are a good starting point for most tempos.

   To change the values:

   1. **Run SooperLooper** with your usual setup and save a session file via
      *File → Save Session*, then exit.

   2. **Edit the session file** in a text editor. Find the ``<Globals>`` element
      and add the two late-hit keys at the end of its attribute list, before the
      closing ``/>``::

          late_hit_max_8th_fraction="0.5" late_hit_window_ms="100"

      A complete ``<Globals>`` element looks like this (your other values will
      differ)::

          <Globals tempo="108" eighth_per_cycle="1" common_dry="0" common_wet="1"
           input_gain="1" sync_source="-1" auto_disable_latency="1"
           jack_timebase_master="0" output_midi_clock="0" use_midi_start="0"
           use_midi_stop="0" send_midi_start_on_trigger="0" smart_eighths="0"
           late_hit_max_8th_fraction="0.5" late_hit_window_ms="100"/>

   3. **Restart SooperLooper** with the session file::

          sooperlooper --load-session yourfile.slsess

Problem Being Solved
--------------------

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
mode is not required. The default window is 100 ms; see *Configurable Window Size* below.

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
pressed on time.

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

Configurable Window Size
------------------------

The window size has two independent limits; the smaller of the two applies.

**Millisecond cap** — ``g_late_hit_window_ms`` (default 100 ms). This is an absolute
ceiling on how late a press can be and still qualify for correction. Its value is saved in
and loaded from the session file (``.slsess``) under the key ``late_hit_window_ms`` in
the ``<Globals>`` node — the same node that holds ``tempo``, ``eighth_per_cycle``, and
other engine-level settings. To change it, edit the session file in a text editor and
restart the engine. If the key is absent (older session files), the compile-time default
``LATE_HIT_WINDOW_MS`` (100 ms) is used.

**Fraction cap** — ``g_late_hit_max_8th_fraction`` (default 0.5, can be >1).
This limits the window to a fraction of one eighth note at the current tempo,
so the correction window scales automatically with tempo rather than being a
fixed duration. At slow tempos the ms cap is the binding limit; the fraction
cap becomes the effective limit above roughly 150 BPM with the default values,
where half an eighth note falls below 100 ms. Its value is stored in the
session file under the key ``late_hit_max_8th_fraction`` in the ``<Globals>``
node; if absent, the compile-time default ``LATE_HIT_MAX_8TH_FRACTION`` is
used.

``effectiveLateHitWindow()`` in ``plugin.cc`` evaluates both limits each buffer and
returns the smaller value in samples. The ms cap is additionally clamped to
``lInputBufSize − 1`` (32767 samples, approximately 682 ms at 48 kHz), ensuring the
window never exceeds the depth of the input ring buffer regardless of what
``g_late_hit_window_ms`` is set to. Setting the ms cap above this ceiling is silently
clamped to it; one practical use of this is to set ``g_late_hit_window_ms`` to a large
value so that the fraction cap alone governs the effective window at all normal playing
tempos.

Why Not a LADSPA Port
~~~~~~~~~~~~~~~~~~~~~

Every other engine parameter travels through the LADSPA port system: ``looper.cpp``
allocates a float, calls ``connect_port()``, and ``plugin.cc`` reads it via a pointer on
``SooperLooperI``. That is the correct mechanism for values that need OSC or GUI exposure
or that change at runtime.

``g_late_hit_window_ms`` is intentionally **not** a LADSPA port. It is a plain file-scope
global in ``plugin.cc``, declared ``extern`` in ``engine.cpp`` and written during session
load from the ``<Globals>`` node. This avoids touching the OSC dispatch layer, the LADSPA
port descriptor table, and ``slgui``. The trade-off is that it only works in the
standalone build where both translation units share an address space. When ``plugin.cc``
is loaded as a true LADSPA plugin inside a DAW, ``engine.cpp`` is absent and the value
stays at the compile-time default for the session, which is acceptable behaviour. If the
value is ever promoted to a GUI control, the correct migration path is to add a proper
LADSPA port and remove the global.

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
       absolute millisecond cap and a fraction of one eighth note at the current
       tempo; whichever is smaller applies.
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

Variables marked **[NEW]** were added by this patch. All others are pre-existing
SooperLooper variables whose roles are clarified here because the patch depends on them.

Constants and Globals (``plugin.cc``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

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
   * - ``LATE_HIT_MAX_8TH_FRACTION``
     - **[NEW]**
     - ``#define float``
     - Compile-time default for the fraction cap (default 0.5). Initialises
       ``g_late_hit_max_8th_fraction``.
   * - ``g_late_hit_window_ms``
     - **[NEW]**
     - ``float`` (file-scope global)
     - Runtime window size in milliseconds. Initialised from ``LATE_HIT_WINDOW_MS``.
       Overwritten by ``engine.cpp`` during session load if ``late_hit_window_ms`` is
       present in the ``<Globals>`` node of the session file. Read at buffer-processing
       time via ``effectiveLateHitWindow()``. See the comment block in ``plugin.cc`` for
       the full rationale on why this is a global rather than a LADSPA port.
   * - ``g_late_hit_max_8th_fraction``
     - **[NEW]**
     - ``float`` (file-scope global)
     - Fraction of one eighth note that defines the upper limit of the window at a given
       tempo. ``effectiveLateHitWindow()`` uses this together with
       ``lLastSyncPeriodSamples`` to compute the fraction cap. At 0.5 (default), the
       window is capped at half an eighth note, preventing the correction from firing
       after the midpoint between two boundaries.
   * - ``effectiveLateHitWindow()``
     - **[NEW]**
     - ``static inline`` function
     - Computes the window in samples as ``min(ms_cap, fraction_cap)``, where
       ``ms_cap = g_late_hit_window_ms × 0.001 × fSampleRate`` (clamped to
       ``lInputBufSize − 1``) and ``fraction_cap = g_late_hit_max_8th_fraction ×
       lLastSyncPeriodSamples / eighthPerCycle``. Returns the ms cap alone if
       ``lLastSyncPeriodSamples`` is at its uninitialised sentinel (``ULONG_MAX``).
   * - ``sl_dbg_file``
     - **[NEW]**
     - ``static FILE*``
     - Handle for the debug log file (``/tmp/sl_debug.log`` or ``~/sl_debug.log``). Opened
       lazily by ``sl_dbg_open()``.
   * - ``SL_LOG(...)``
     - **[NEW]**
     - macro
     - Write a message to ``sl_dbg_file`` with an immediate ``fflush``. Use for messages
       that should always appear (startup banners). Not RT-safe; do not call from the
       audio callback.
   * - ``SL_LOG_RT(...)``
     - **[NEW]**
     - macro
     - Write a message to ``sl_dbg_file`` without ``fflush``. Used inside
       ``SYNCWDBG()`` in the RT callback. Not fully RT-safe (``fprintf`` acquires a
       stdio lock); causes xruns when ``SYNCWDEBUG`` is defined. Acceptable as a
       debug-only cost.
   * - ``SYNCWDEBUG``
     - **[NEW]**
     - ``#define`` (flag)
     - When defined, enables per-press timing diagnostics via ``SYNCWDBG()``. Comment out
       the ``#define SYNCWDEBUG`` line to suppress these messages while keeping banners.
       Commented out by default; disable it for any performance build to eliminate any
       associated xruns.  See the Known Issues section.
   * - ``SYNCWDBG(x)``
     - **[NEW]**
     - macro
     - Expands to ``x`` when ``SYNCWDEBUG`` is defined; expands to nothing otherwise.
       Wraps ``SL_LOG_RT()`` calls in the hot path that are only wanted during debugging.
   * - ``sl_load_probe``
     - **[NEW]**
     - ``static struct SL_LoadProbe``
     - C++ static initialiser that fires when the binary is loaded (before ``main``).
       Writes ``/tmp/sl_loaded.txt`` and a line to stderr confirming that the experimental
       build is running. Useful because ``slgui`` redirects stderr of the engine process.


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
   * - ``late_hit_max_8th_fraction``
     - **[NEW]**
     - Floating-point XML property in the ``<Globals>`` node of the ``.slsess`` session
       file. Written by ``Engine::save_session()`` and read by
       ``Engine::load_session()``. If absent (older session files), the compile-time
       default ``LATE_HIT_MAX_8TH_FRACTION`` is used. Edit with a text editor; a
       restart of the engine is required for the change to take effect.

Architecture Notes
------------------

-  **Why ``lGlobalSamplesSinceSync`` instead of ``lSamplesSinceSync``:**
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

-  **Why ``dCurrPos += T`` rather than ``lFirstPlayOffset`` for late stop:** Detecting
   the "first wrap" reliably to apply a rewind proved fragile (the wrap check fires
   inside a ``recenter`` guard at the wrong moment). Advancing ``dCurrPos`` by *T* at
   entry to ``STATE_PLAY`` achieves the same correction with no wrap detection needed.

-  **Why a global for ``g_late_hit_window_ms`` rather than a LADSPA port:** See the "Why
   Not a LADSPA Port" section above and the comment block in ``plugin.cc``. The short
   version: it is a setup-time preference, not a real-time control, and avoiding the port
   system keeps the change entirely within ``plugin.cc`` and ``engine.cpp``.

-  **Why the backfill read position is** ``lInputBufWritePos − T`` **(no lInputLatency):**
   ``pfInputLatencyBuf`` is a verbatim copy of ``pfInput`` with its write pointer at the
   current physical time. Normal recording from the sync boundary would have captured
   physical audio at ``T_sync − lInputLatency + i`` for sample *i*. Working backwards
   from ``lInputBufWritePos`` (which points to physical time ``T_now − lInputLatency``)
   this maps to offset ``lInputBufWritePos − T + i``, so ``readPos = lInputBufWritePos −
   T``. The ``lInputLatency`` term cancels exactly.

-  **Why ``lLastSyncPeriodSamples`` is seeded from tempo rather than measured pulses:**
   The sync bus carries two kinds of pulses above the 1.5 threshold: the true beat
   boundaries and lower-amplitude loop-coordination signals. Seeding from ``fTempo``
   (BLOCK 7) gives a period that is immune to these sub-threshold pulses, while BLOCK 8
   provides a measured update gated by a sanity floor to catch cases where no tempo is
   available.

Known Issues and Future Work
----------------------------

-  **Internal sync mode:** when SooperLooper drives itself (one loop as the sync source
   for others), late-hit correction is **disabled on all slave channels**. Both channels
   of a stereo pair then use the stock timing path and stay in phase with each other.
   Enabling correction on all channels in this mode would require a broader audit of the
   state machine; deferred. See ``bLateHitBypassed`` in the Glossary.

-  **Sync-source change:** the late-hit state (``bSyncPulseReceived``,
   ``lGlobalSamplesSinceSync``, ``lLastSyncPeriodSamples``) is reset to its initial
   condition whenever ``Engine::update_sync_source()`` is called — i.e., whenever the
   user changes the sync source. This prevents stale period measurements from the old
   source affecting the first presses on the new one.

-  **Window size and sample rate:** the effective maximum window is
   ``(lInputBufSize − 1) / fSampleRate`` seconds — approximately 682 ms at 48 kHz,
   743 ms at 44.1 kHz, 341 ms at 96 kHz. Setting ``g_late_hit_window_ms`` above this
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

-  **``SYNCWDEBUG`` is off by default.** Uncomment ``#define SYNCWDEBUG`` in
   ``plugin.cc`` to enable per-press timing diagnostics. Output is written to
   ``/tmp/sl_debug.log`` (falling back to ``~/sl_debug.log`` if ``/tmp`` is not
   writable); the file is overwritten fresh on each engine start. To watch it in
   real time::

       tail -f /tmp/sl_debug.log

   When active, ``SL_LOG_RT`` uses ``fprintf``, which is not fully RT-safe and will
   cause xruns; this is accepted as the cost of debug mode. Startup banners via
   ``SL_LOG`` are unaffected by this setting.


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
   * - ``g_late_hit_max_8th_fraction`` exposed via SWIG
     - Same mechanism as above.  Accessible as
       ``test_engine.cvar.g_late_hit_max_8th_fraction``.

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

