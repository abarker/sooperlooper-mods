SooperLooper Late-Hit Correction — Patch Summary


| **Base version:** SooperLooper 1.7.9
| **Modified files:** ``src/plugin.cc``, ``src/plugin.hpp``, ``src/engine.cpp``,
| ``src/looper.cpp``
| **Unmodified files:** ``src/looper.cpp``, ``src/looper.hpp``, and all others
| **All new code sections** in ``plugin.cc`` are bracketed with ``//====== albBegin`` /
| ``//====== albEnd`` markers. Search for ``albBegin`` to locate every changed block.

--------------

Problem Being Solved
--------------------

SooperLooper’s quantize mode corrects *early* button presses by waiting for the next sync
boundary before acting. It does nothing for *late* presses — a Record or Stop hit that
arrives slightly after a sync boundary causes the loop to start one full quantization unit
late — the press is treated as belonging to the *next* boundary, not the one just passed —
and the loop length is inflated by that same unit. This is a persistent source of timing
errors in live performance.

--------------

What the Patch Does
-------------------

When the per-loop **Sync checkbox is on** and a **sync source is selected and producing
pulses**, a Record or Stop press that arrives within a configurable window (default 100
ms) **after** a sync boundary is corrected back to that boundary instead of waiting for
the next one. Quantize mode is not required.

“Sync active” means both conditions together: the Sync checkbox controls whether
operations wait for boundaries at all (``fSyncMode > 0``); the sync source selection
determines what signal arrives on the sync input and therefore when
``lGlobalSamplesSinceSync`` resets. With no source, T grows without bound and the window
never fires regardless of the checkbox.

Late Record Start
~~~~~~~~~~~~~~~~~

The press would normally go to ``STATE_TRIG_START`` and wait for the next boundary,
producing a loop that starts one full cycle late. Instead:

1. The loop buffer is created immediately via ``pushNewLoopChunk()``.
2. The *T* samples of audio that elapsed since the boundary are copied (“backfilled”)
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
2. The play position ``dCurrPos`` is set to ``lLoopLength + latency + T`` — the same
   formula as the normal ``STATE_TRIG_STOP`` path, plus *T* to compensate for having
   entered ``STATE_PLAY`` *T* samples late in real time.
3. The engine enters ``STATE_PLAY`` directly.

After the first natural wrap, ``dCurrPos`` lands at ``latency`` — identical to a normal
on-time loop. No rewind or special first-play handling is needed.

Both Late (start and stop)
~~~~~~~~~~~~~~~~~~~~~~~~~~

The two corrections are applied independently in sequence and compose correctly. An on-
time stop after a late start, or a late stop after an on-time start, each works the same
way.

On-Time Presses and Disabled Modes
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

All new code paths are gated by ``withinWindow`` / ``stopWithinWindow`` /
``multStopInWindow``. These flags are false when the press is on time, when sync is off
(``fSyncMode == 0``), or when no sync pulse has arrived recently (T exceeds the window).
The engine behaves identically to the unpatched version in all these cases. Quantize mode
is no longer a condition.

--------------

Configurable Window Size
------------------------

The window is stored as ``g_late_hit_window_ms`` (see Glossary). Its value is saved in and
loaded from the session file (``.slsess``) under the key ``late_hit_window_ms`` in the
``<Globals>`` node — the same node that holds ``tempo``, ``eighth_per_cycle``, and other
engine-level settings. To change it, edit the session file in a text editor and restart
the engine.

If the key is absent (older session files), the compile-time default
``LATE_HIT_WINDOW_MS`` (100 ms) is used. The value must be positive; an invalid or zero
value in the file is ignored and the default is preserved.

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

--------------

Glossary
--------

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
   * - ``g_late_hit_window_ms``
     - **[NEW]**
     - ``float`` (file-scope global)
     - Runtime window size in milliseconds. Initialised from ``LATE_HIT_WINDOW_MS``.
       Overwritten by ``engine.cpp`` during session load if ``late_hit_window_ms`` is
       present in the ``<Globals>`` node of the session file. Read at buffer-processing
       time in both late-hit code paths. See the comment block in ``plugin.cc`` for the
       full rationale on why this is a global rather than a LADSPA port.
   * - ``sl_dbg_file``
     - **[NEW]**
     - ``static FILE*``
     - Handle for the debug log file (``/tmp/sl_debug.log`` or ``~/sl_debug.log``). Opened
       lazily by ``sl_dbg_open()``.
   * - ``SL_LOG(...)``
     - **[NEW]**
     - macro
     - Write a message to ``sl_dbg_file`` with an immediate ``fflush``. Use for messages
       that should always appear (startup banners).
   * - ``SYNCWDEBUG``
     - **[NEW]**
     - ``#define`` (flag)
     - When defined, enables per-press timing diagnostics via ``SYNCWDBG()``. Comment out
       the ``#define SYNCWDEBUG`` line to suppress these messages while keeping banners.
   * - ``SYNCWDBG(x)``
     - **[NEW]**
     - macro
     - Expands to ``x`` when ``SYNCWDEBUG`` is defined; expands to nothing otherwise.
       Wraps ``SL_LOG()`` calls in the hot path that are only wanted during debugging.
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
     - ``unsigned int``
     - Continuously-running sample counter that increments every sample in **all** engine
       states and resets to zero on each sync pulse (``pfSyncInput[n] > 1.5``). Valid at
       button-press time regardless of which state the engine is in. Contrast with the
       pre-existing ``lSamplesSinceSync``, which only increments during ``STATE_RECORD``
       and ``STATE_PLAY`` and is therefore useless at button-press time from
       ``STATE_OFF``.
   * - ``bSyncPulseReceived``
     - **[NEW]**
     - ``bool``
     - Set to ``true`` when the first sync pulse arrives after instantiation; never
       cleared. Guards against the startup case where ``lGlobalSamplesSinceSync`` is
       counting “time since engine start” rather than “time since last pulse” — without
       this flag, a press 1–100ms after startup would incorrectly satisfy the window
       condition. Initialised ``false`` by ``calloc``.
   * - ``bLateStartPending``
     - **[NEW]**
     - ``bool``
     - Set ``true`` when a late-start backfill correction fires. Cleared in
       ``STATE_TRIG_STOP`` after adjusting ``dCurrPos`` and ``lFramesUntilFilled``.
       Ensures only ``lOutputLatency`` (not ``lOutputLatency + lInputLatency``) is added
       to ``dCurrPos`` on stop, so playback begins near position ``lOutputLatency`` rather
       than ``latency``, avoiding the first ``lInputLatency`` samples of backfill being
       delayed to after the first wrap. Initialised ``false`` by ``calloc``.
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
     - Length of ``pfInputLatencyBuf`` in samples (32768). Used as the modulus for the
       wrap-around read in the backfill.
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

``LoopChunk`` represents one recorded loop. Multiple ``LoopChunk``\ s can be stacked on a
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
     - ``unsigned int`` (local)
     - Snapshot of ``pLS->lGlobalSamplesSinceSync`` taken **before** scanning the current
       buffer for sync pulses. This is the value used for all window tests. The snapshot
       is necessary because a sync pulse arriving mid-buffer should not retroactively
       change the phase reading for a button press that logically arrived at the buffer
       start.
   * - ``shiftSamples``
     - **[NEW]**
     - ``unsigned long`` (local)
     - ``g_late_hit_window_ms`` converted to samples: ``g_late_hit_window_ms × 0.001 ×
       fSampleRate``. Computed fresh at each button-press test so it automatically tracks
       sample-rate changes between sessions.
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
     - Late-start gate: true when ``T > 0 && T ≤ shiftSamples && fSyncMode > 0``. Quantize
       mode is not a condition; the correction fires for any external sync source.
   * - ``stopWithinWindow``
     - **[NEW]**
     - ``bool`` (local)
     - Late-stop gate: ``T > 0 && T ≤ shiftSamples && fSyncMode > 0 && loop ≠ NULL``.
   * - ``trimmedLength``
     - **[NEW]**
     - ``unsigned long`` (local)
     - Computed loop length after trim: ``(unsigned long)dCurrPos − T``. Protected by a
       guard requiring at least ``xfadeSamples`` of remaining content; if the guard fails,
       the code falls back to ``STATE_TRIG_STOP``.
   * - ``readPos``
     - **[NEW]**
     - ``unsigned long`` (local)
     - Start offset into ``pfInputLatencyBuf`` for the backfill read: ``lInputBufWritePos
       − T − lInputLatency``. The ``lInputLatency`` term matches the offset used by normal
       recording, ensuring seamless continuity between backfilled and live-recorded
       samples. Wrap-around handled for all valid values.
   * - ``multStopInWindow``
     - **[NEW]**
     - ``bool`` (local)
     - Multiply-end gate: ``T > 0 && T ≤ shiftSamples && fSyncMode > 0 && lCycles ≥ 2``.
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
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

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


--------------

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
   the “first wrap” reliably to apply a rewind proved fragile (the wrap check fires
   inside a ``recenter`` guard at the wrong moment). Advancing ``dCurrPos`` by *T* at
   entry to ``STATE_PLAY`` achieves the same correction with no wrap detection needed.

-  **Why a global for ``g_late_hit_window_ms`` rather than a LADSPA port:** See the “Why
   Not a LADSPA Port” section above and the comment block in ``plugin.cc``. The short
   version: it is a setup-time preference, not a real-time control, and avoiding the port
   system keeps the change entirely within ``plugin.cc`` and ``engine.cpp``.

--------------

Known Issues and Future Work
----------------------------

-  **Input latency awareness:** the backfill read position now incorporates
   ``lInputLatency``: ``readPos = lInputBufWritePos - T - lInputLatency``. This matches
   the offset used by normal recording, ensuring seamless continuity between backfilled
   and live-recorded samples. Constraint: ``T + lInputLatency`` must be less than
   ``lInputBufSize`` (32768 samples); satisfied for all reasonable values
   within the 100ms window.

-  **Spurious trigger at startup:** ``lGlobalSamplesSinceSync`` is zeroed by ``calloc``
   at instantiation and counts samples upward until the first sync pulse resets it to 0.
   The window condition is ``T > 0 && T <= shiftSamples``, where ``shiftSamples`` is the
   100ms window in samples (~4410 at 44.1kHz). Without a guard, a press arriving 1–100ms
   after startup would satisfy this condition even though
   no sync pulse has ever occurred — T is “time since engine
   start,” not “time since last pulse.”

   The fix is ``bSyncPulseReceived``, a new ``bool`` on ``SooperLooperI``
   (``false`` by ``calloc``). Set ``true`` on the first pulse, never cleared.
   All three window conditions include ``&& pLS->bSyncPulseReceived``, so the
   T comparison is never evaluated until a real reference point exists.

   The ``unsigned int`` wraparound (~27h at 44.1kHz with no pulses) is
   treated as acceptable for a live performance tool.

-  **Multiply end:** correction is implemented and pending test confirmation (disable
   with ``LATE_HIT_MULTIPLY_CORRECTION 0`` if regressions appear).

-  **Multiply start:** deferred. The frontfill machinery in ``beginMultiply()`` makes the
   position adjustment more involved; low priority since Multiply start snaps to the
   current loop’s cycle boundary which is a natural and musically acceptable snap point.

-  **Insert end:** not needed — Insert end does not use ``waitingForSync`` and takes
   effect at the exact press sample already.

-  **Insert start:** deferred for the same reason as Multiply start.

-  **Double log lines:** each button press produces two identical log entries because
   both channel instances (0:0 and 0:1) receive the same event and log independently.
   Expected behaviour, not a bug.

-  **``SYNCWDEBUG`` is on by default.** Comment out ``#define SYNCWDEBUG`` in
   ``plugin.cc`` before a production build to suppress per-press log messages. Startup
   banners via ``SL_LOG`` are unaffected.
