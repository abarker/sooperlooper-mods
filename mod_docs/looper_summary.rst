================================================================================
SooperLooper 1.7.9 — ``looper.hpp`` / ``looper.cpp`` Compilation Unit Summary
================================================================================

This is an earlier analysis of the code, especially with respect to latencies.
It discusses Mididings in the context of receiving MIDI input to the new Jack
MIDI port via a MIDI controller but remapped by software based on Mididings.

:Author: Analysis of source by Jesse Chappell <jesse@essej.net>
:Version: SooperLooper 1.7.9
:License: GNU General Public License v2+

.. contents:: Table of Contents
   :depth: 3


Overview
========

``looper.hpp`` and ``looper.cpp`` define the ``SooperLooper::Looper`` class,
which is the central per-loop management object in the SooperLooper engine.
Each ``Looper`` instance owns one or more **LADSPA plugin instances** (one per
audio channel) and is responsible for:

- Instantiating and wiring the low-level LADSPA looping engine (``plugin.cc``).
- Routing real-time audio buffers to/from those plugin instances each process
  block.
- Translating high-level ``Event`` objects (from OSC, MIDI, or the GUI) into
  LADSPA port writes that drive the plugin's state machine.
- Managing optional **sample-rate conversion** (via libsamplerate/SRC) and
  **time-stretching / pitch-shifting** (via RubberBand) on the way in and out
  of the plugin.
- Maintaining ancillary state: peak meters, dry/wet gain ramping, panning,
  latency compensation, XML session persistence, and loop-file I/O
  (libsndfile).

The class sits between the ``Engine`` (which drives the JACK audio callback)
and ``plugin.cc`` (which contains the actual sample-accurate looping DSP).


File Structure — ``looper.hpp``
================================

The header declares the ``Looper`` class inside the ``SooperLooper``
namespace.  Its public interface exposes:

.. list-table:: Key Public Members
   :widths: 35 65
   :header-rows: 1

   * - Member
     - Purpose
   * - ``Looper(driver, index, chan_count, loopsecs, discrete)``
     - Primary constructor: creates *chan_count* LADSPA instances.
   * - ``Looper(driver, XMLNode&)``
     - Restore-from-session constructor.
   * - ``run(offset, nframes)``
     - Called every audio process block (real-time thread).
   * - ``do_event(Event*)``
     - Translates an event into a pending command or control-port write.
   * - ``get_control_value(ctrl)``
     - Read any control port / derived value by symbolic name.
   * - ``set_port(n, val)``
     - Write a single LADSPA control port (with special-case handling).
   * - ``load_loop(fname)`` / ``save_loop(fname, fmt)``
     - libsndfile-backed loop file I/O (called off the audio thread).
   * - ``finish_state()``
     - Sends the complementary command to end whichever active state the
       plugin is currently in (Record, Overdub, Multiply, …).
   * - ``is_longpress(cmd)``
     - Returns ``true`` if *cmd*'s button has been held > 1 second.
   * - ``recompute_latencies()``
     - Recalculates the three latency compensation ports from JACK topology.

Protected data members of note:

- ``LADSPA_Handle * _instances`` — array of *_chan_count* plugin handles.
- ``LADSPA_Descriptor * descriptor`` — shared descriptor obtained from
  ``create_sl_descriptor()`` (lives in ``plugin.cc``).
- ``LADSPA_Data ports[LASTPORT]`` — flat array of all LADSPA port values
  (both control and status ports).
- ``nframes_t _down_stamps[Event::LAST_COMMAND+1]`` — per-command timestamps
  used by the longpress / doubletap detector.
- ``nframes_t _longpress_frames`` (= sample_rate × 1.0 s)
- ``nframes_t _doubletap_frames`` (= sample_rate × 0.5 s)
- ``nframes_t _running_frames`` — monotonically increasing frame counter,
  incremented each ``run()`` call.
- ``SRC_STATE`` arrays and ``RubberBandStretcher`` pointers for rate
  conversion and pitch/time-stretching.


Interaction with ``plugin.cc`` (the LADSPA Engine)
====================================================

Architecture
------------

``plugin.cc`` implements the SooperLooper looping engine as a standard LADSPA
plugin.  Its descriptor is retrieved at ``Looper`` construction time via::

    descriptor = create_sl_descriptor();   // defined in plugin.cc

``Looper`` treats this descriptor as a black box accessed only through the
four LADSPA API calls:

.. list-table:: LADSPA API Usage
   :widths: 30 70
   :header-rows: 1

   * - Call
     - When / Purpose
   * - ``descriptor->instantiate(descriptor, srate)``
     - Once per channel during ``initialize()``.
   * - ``descriptor->connect_port(handle, port, data_ptr)``
     - Before every ``run()`` call to point audio ports at current buffers;
       control ports are connected once to ``ports[]``.
   * - ``descriptor->activate(handle)``
     - Once after instantiation.
   * - ``descriptor->run(handle, nframes)``
     - Every process block (the real-time "tick").

Command / Control Protocol
--------------------------

LADSPA control ports are **passive floats** — the plugin reads them at the
start of each ``run()`` call.  SooperLooper exploits this for command
delivery via the special ``Multi`` port:

- To issue a command, ``Looper::run()`` writes the command integer to
  ``ports[Multi]`` before calling ``descriptor->run()``.
- After one block the plugin has consumed it, and ``Looper::run()`` resets
  ``ports[Multi] = -1``.
- A deferred-one-block mechanism prevents two identical commands in back-to-
  back calls from being collapsed.

All other control ports (feedback, rate, sync mode, quantize, latency
offsets, …) are connected permanently to the ``ports[]`` array so the plugin
sees updated values on every block without any additional copying.

Multi-Channel Sync
------------------

When ``_chan_count > 1``, channel 0 is the *sync master*:

- Its ``SyncOutput`` buffer feeds the ``SyncInput`` of every subordinate
  channel.
- Subordinate channels connect ``State``, ``LoopLength``, ``LoopPosition``,
  and related status ports to a dummy buffer so only channel 0 reports
  state back up to ``Looper``.
- Subordinate channels receive a *slave sync port* value (1.0 = chase, 2.0 =
  relative sync) rather than the user-facing ``Sync`` port.

Latency Compensation Ports
--------------------------

Three LADSPA ports are written by ``Looper`` and read by ``plugin.cc`` for
sample-accurate loop-point alignment:

- ``TriggerLatency`` — set to the JACK buffer size (jitter correction).
- ``InputLatency`` — JACK-reported latency to the physical input port.
- ``OutputLatency`` — JACK-reported latency to the physical output port.

These are recalculated on every buffer-size change, topology change, or when
``_auto_latency`` is toggled.


Pseudocode — Key Methods
=========================

``initialize()``
----------------

.. code-block:: text

    initialize(index, chan_count, loopsecs, discrete):
        zero / null all member variables
        set _longpress_frames  = samplerate × 1.0
        set _doubletap_frames  = samplerate × 0.5
        set _falloff_per_sample = 30 dB / samplerate   # peak-meter decay

        descriptor = create_sl_descriptor()            # from plugin.cc

        allocate arrays: _instances[chan_count],
                         _input_ports[chan_count],
                         _output_ports[chan_count],
                         SRC state pairs,
                         RubberBand in/out stretchers,
                         lowpass filters,
                         temp I/O buffers

        FOR each channel i:
            _instances[i] = descriptor->instantiate(descriptor, samplerate)
            sl_set_loop_index(_instances[i], index, i)

            IF discrete_io:
                create JACK input  port "loop{index}_in_{i+1}"
                create JACK output port "loop{index}_out_{i+1}"

            # Connect all scalar ports to the shared ports[] array
            FOR each port n in [0, LASTPORT):
                descriptor->connect_port(_instances[i], n, &ports[n])

            IF i > 0:
                # Slave channels: override sync & status ports
                connect Sync    → &_slave_sync_port
                connect State, LoopLength, LoopPosition, … → &_slave_dummy_port

            descriptor->activate(_instances[i])
            initialise SRC states and lowpass filter for channel i

        IF common_output_count > 1:
            create Panner and set default positions

        _ok = true


``run()``  (real-time audio thread)
------------------------------------

.. code-block:: text

    run(offset, nframes):
        TRY to acquire _loop_lock (non-blocking)
        IF lock failed:
            IF discrete_io:
                passthrough: outbuf[n] = inbuf[n] × dry_gain × input_gain
            RETURN

        _running_frames += nframes

        # --- Command dispatch ---
        IF request_pending:
            IF ports[Multi] == requested_cmd:
                ports[Multi] = -1          # defer: plugin already sees it
            ELSE:
                ports[Multi] = requested_cmd
                request_pending = false
                IF requested_cmd == RECORD AND state != Recording:
                    reset stretch_ratio and pitch_shift to unity
        ELSE IF ports[Multi] >= 0:
            ports[Multi] = -1              # clear stale command

        # --- Pending stretch ratio ---
        IF _pending_stretch:
            IF stretch_ratio was 1.0 AND new_ratio != 1.0:
                reset in/out stretchers
            apply new stretch ratio to both stretchers
            _pending_stretch = false
            recompute_latencies()

        # --- Sync port management ---
        IF using own sync buffer:
            ports[Sync] = 0; _slave_sync_port = 1.0
        ELSE IF relative_sync AND Sync > 0:
            ports[Sync] = 2.0; _slave_sync_port = 2.0
        ELSE:
            _slave_sync_port = 1.0

        # --- Peak meter falloff ---
        decay _input_peak and _output_peak by falloff_per_sample × nframes

        run_loops(offset, nframes)

        restore ports[Sync] to saved value


``run_loops()``  (called from run, real-time)
----------------------------------------------

.. code-block:: text

    run_loops(offset, nframes):
        compute per-sample deltas for dry, wet, input_gain
        resampled  = (Rate port != 1.0)
        stretched  = (stretch_ratio != 1.0)
        pitched    = (pitch_shift != 0.0)

        IF resampled:
            resample sync buffer via SRC at _src_in_ratio
            alt_frames = resampled frame count

        # --- Gather I/O buffers ---
        FOR each channel i:
            IF discrete_io:
                real_inbufs[i] = JACK input  buffer + offset
                outbufs[i]     = JACK output buffer + offset
            ELSE:
                outbufs[i]     = temp I/O buffer

            IF use_common_ins OR no discrete input:
                mix common engine input into temp buffer (applying input_gain)
            ELSE:
                apply input_gain to discrete input into temp buffer

            compute_peak(inbufs[i], nframes, _input_peak)

        # === Three mutually exclusive processing paths ===

        IF resampled:
            FOR each channel i:
                SRC-upsample input  → _src_in_buffer
                connect AudioInput / AudioOutput → _src_in_buffer
                descriptor->run(_instances[i], alt_frames)
                SRC-downsample output back to nframes
                IF frames generated < nframes: retry for remainder

        ELSE IF stretched OR pitched:
            # RubberBand output-stretch loop
            WHILE out_stretcher.available() < nframes:
                sampsReq = out_stretcher.getSamplesRequired()
                FOR each channel i:
                    zero input buffer (recording suppressed during stretch)
                    connect ports, descriptor->run(_instances[i], sampsReq)
                out_stretcher.process(outbufs, sampsReq)
            out_stretcher.retrieve(outbufs, nframes)

        ELSE:   # normal path
            FOR each channel i:
                connect AudioInput  → inbufs[i]
                connect AudioOutput → outbufs[i]
                IF i == 0:
                    connect SyncIn  → _use_sync_buf + offset
                    connect SyncOut → _our_syncout_buf + offset
                ELSE:
                    connect SyncIn  → _our_syncout_buf + offset   # ch0 out feeds ch1+ in
                    connect SyncOut → _dummy_buf
                descriptor->run(_instances[i], nframes)

        # --- Post-processing ---
        FOR each channel i:
            IF discrete_io AND real input exists:
                mix dry signal into output with ramped gain

            IF panner AND use_common_outs:
                distribute output into common engine output buffers

            compute_peak(outbufs[i], nframes, _output_peak)

        IF resampled:
            SRC-downsample sync output back to nframes

        update _curr_dry, _curr_wet, _curr_input_gain (snap if delta negligible)


``do_event()``  (event dispatcher, may be called from non-RT thread)
----------------------------------------------------------------------

.. code-block:: text

    do_event(ev):

        SWITCH ev->Type:

        CASE type_cmd_hit:      # momentary "tap" — no notion of hold duration
            cmd = ev->Command
            requested_cmd = cmd
            request_pending = true

            IF cmd IN {RECORD_OR_OVERDUB, RECORD_OR_OVERDUB_EXCL,
                       RECORD_OR_OVERDUB_SOLO}:
                IF _down_stamps[cmd] > 0
                   AND _running_frames < _down_stamps[cmd] + _doubletap_frames:
                    # Second tap within 0.5 s → override to UNDO_TWICE
                    requested_cmd = UNDO_TWICE
                _down_stamps[cmd] = _running_frames   # record this tap time

        CASE type_cmd_down:     # note-on / button-press
            cmd = ev->Command
            requested_cmd = cmd
            request_pending = true

            IF cmd IN {RECORD_OR_OVERDUB, …}:
                IF _down_stamps[cmd] > 0
                   AND _running_frames < _down_stamps[cmd] + _doubletap_frames:
                    requested_cmd = UNDO_TWICE
            _down_stamps[cmd] = _running_frames

        CASE type_cmd_up / type_cmd_upforce:   # note-off / button-release
            cmd = ev->Command
            IF state != Playing  OR  cmd IN {REVERSE, DELAY, UNDO, REDO}:

                IF ev->Type == type_cmd_upforce:
                    # Unrounded Multiply/Insert special case
                    IF quantize==0 AND ((state==Multiplying AND cmd==MULTIPLY)
                                     OR (state==Inserting  AND cmd==INSERT)):
                        cmd = RECORD   # signals "end without rounding"
                    requested_cmd = cmd
                    request_pending = true

                ELSE IF _down_stamps[cmd] > 0
                        AND _running_frames > _down_stamps[cmd] + _longpress_frames:
                    # Long press detected on release
                    requested_cmd = cmd
                    request_pending = true
                    IF cmd == UNDO:               requested_cmd = UNDO_ALL
                    ELSE IF cmd == REDO:          requested_cmd = REDO_ALL
                    ELSE IF cmd IN {RECORD_OR_OVERDUB, …}:
                        requested_cmd = UNDO_ALL  # one-button convenience

            _down_stamps[cmd] = 0    # reset for next press

        CASE type_control_change:
            IF control IN [TriggerThreshold, State):   # scalar control range
                IF control == Rate:
                    update _src_in_ratio / _src_out_ratio
                    set SRC ratios for all channels + sync
                    update lowpass filter cutoffs
                SWITCH control:
                    DryLevel  → _target_dry = value
                    WetLevel  → _target_wet = value; conditionally ports[WetLevel]
                    Quantize  → round value then fall through
                    default   → ports[control] = value

            ELSE IF control == InputGain:     _targ_input_gain = value
            ELSE IF control == RelativeSync:  _relative_sync = (value > 0)
            ELSE IF control == UseCommonIns:  _use_common_ins = (value > 0)
            ELSE IF control == UseCommonOuts: _use_common_outs = (value > 0)
            ELSE IF control == AutosetLatency: recompute_latencies()
            ELSE IF control == PitchShift:
                _pitch_shift = value
                out_stretcher->setPitchScale(2^(value/12))
            ELSE IF control == StretchRatio:
                _pending_stretch_ratio = clamp(value, 0.25, 4.0)
                _pending_stretch = true
            ELSE IF control == TempoStretch:
                compute stretch ratio from loop tempo vs. input tempo
                _pending_stretch = true
            ELSE IF control IN {PanChannel1..4}: update panner positions


``load_loop()`` and ``save_loop()``
-------------------------------------

.. code-block:: text

    load_loop(fname):
        acquire _loop_lock (blocking — this is NOT the audio thread)
        open fname with libsndfile
        verify sinfo.frames <= free loop memory
        save current port values (threshold, sync, fade, latencies, quantize)
        temporarily zero those ports
        issue MUTE_ON then RECORD commands (via ports[Multi] + run(0 frames))

        WHILE frames_left > 0:
            read up to 64 K interleaved frames from file
            deinterleave into per-channel inbufs[]
            (duplicate last channel if file has fewer channels than loop)
            FOR each channel i: descriptor->run(_instances[i], nframes)
            frames_left -= nframes

        issue UNKNOWN then RECORD/MUTE/PAUSE command to finalise state
        restore saved port values

    save_loop(fname, format):
        # No lock needed — called from main work thread
        open fname for writing with libsndfile (WAV Float32 or PCM16)
        frames_left = LoopLength port × samplerate

        WHILE frames_left > 0:
            FOR each channel i:
                nframes = sl_read_current_loop_audio(_instances[i],
                                                     outbufs[i], nframes, looppos)
            interleave channels into bigbuf[]
            sf_writef_float(sfile, bigbuf, nframes)
            frames_left -= nframes; looppos += nframes


Longpress and Doubletap — Logic and Latency Analysis
======================================================

Threshold Values
----------------

Both thresholds are set once in ``initialize()`` from the sample rate:

============= =========================================================
Threshold     Value
============= =========================================================
Longpress     ``_longpress_frames = lrint(samplerate × 1.0)``  — 1 second
Doubletap     ``_doubletap_frames = lrint(samplerate × 0.5)``  — 0.5 second
============= =========================================================

Longpress Operation
-------------------

**Mechanism.** Longpress is a *release-time* detection.  On a
``type_cmd_down`` event the command fires immediately (``request_pending =
true``) and the timestamp ``_down_stamps[cmd]`` is set.  On the subsequent
``type_cmd_up`` event the code checks::

    if (_down_stamps[cmd] > 0  &&
        _running_frames > _down_stamps[cmd] + _longpress_frames)
    {
        // long press — fire the long-press variant
    }

If a long press is detected, the command is *re-issued* as its "all" variant:

- ``UNDO``  →  ``UNDO_ALL``
- ``REDO``  →  ``REDO_ALL``
- ``RECORD_OR_OVERDUB`` (and ``_EXCL``, ``_SOLO``, ``_END_SOLO``)  →
  ``UNDO_ALL``

**Latency implication.** The initial ``type_cmd_down`` command fires with
**no added latency** — it is sent the same process block in which the
note-on arrives.  The long-press variant fires only when the button is
released (after > 1 s), so there is inherently no ambiguity-induced
wait on the note-on path.

The only latency cost is on the *note-off* side: ``type_cmd_up`` does
nothing unless longpress is confirmed, so for a genuinely long press the
secondary action (UNDO_ALL) is deferred until the physical button-release
event arrives, which by definition is > 1 second after note-on.  This is
by design — the hold duration *is* the intent signal.

Doubletap Operation
-------------------

**Mechanism.** Doubletap is detected on ``type_cmd_hit`` **and**
``type_cmd_down`` for the three ``RECORD_OR_OVERDUB`` command variants.
On each event::

    if (_down_stamps[cmd] > 0  &&
        _running_frames < _down_stamps[cmd] + _doubletap_frames)
    {
        requested_cmd = Event::UNDO_TWICE;
    }
    _down_stamps[cmd] = _running_frames;

**What it does.** The ``RECORD_OR_OVERDUB`` command is designed for
one-button workflows: first press starts a Record→Overdub cycle; double-
tapping within 0.5 s instead fires ``UNDO_TWICE``, returning the loop to
a pre-record state in a single physical gesture.

**Latency implication.** Unlike a naïve "wait to see if a second tap
comes" design, SooperLooper does **not** defer the first tap.  The first
``RECORD_OR_OVERDUB`` fires immediately on the first event.  Only when
the *second* tap arrives within 0.5 s is the override to ``UNDO_TWICE``
applied.  The effective sequence is therefore:

1. First tap  → ``RECORD_OR_OVERDUB`` fires immediately (zero latency).
2. Second tap within 0.5 s → ``UNDO_TWICE`` fires immediately (zero
   additional latency beyond the physical inter-tap gap).

There is no "sit and wait" window — the system is reactive at each event.
The consequence is that a doubletap produces two commands (first the
normal one, then UNDO_TWICE), not one deferred command.  This is
appropriate for a looper: the record state entered on tap 1 is
immediately reversed by the UNDO_TWICE on tap 2.

``type_cmd_upforce`` — The SUS / Unrounded Path
-------------------------------------------------

``type_cmd_upforce`` is a special release event that bypasses the
longpress check entirely and fires unconditionally on note-off.  It is
used for **SUS (Sustain Until Release)** bindings:

- Note-on  → ``type_cmd_down``  → command fires immediately.
- Note-off → ``type_cmd_upforce`` → same command fires again (ending the
  sustained state).

There is a special sub-case: if the looper is in Multiplying or Inserting
state, the loop is **not** quantized (``ports[Quantize] == 0``), and the
command matches the active state, then the up-force command is silently
changed to ``RECORD``.  This signals to the plugin that the user released
the button before a full cycle completed — an "unrounded" end — which the
plugin interprets differently from a normal end-of-multiply.

Combined with longpress, SUS, and doubletap, the complete decision tree for
a command received at note-off is:

.. code-block:: text

    note-off arrives with cmd:
        IF type_cmd_upforce:
            IF state IN {Multiplying, Inserting}
               AND cmd matches state AND quantize == 0:
                send RECORD     # unrounded end
            ELSE:
                send cmd        # normal SUS release
        ELSE IF type_cmd_up:
            IF state != Playing  OR  cmd IN {REVERSE, DELAY, UNDO, REDO}:
                IF hold_time > 1.0 s:
                    send UNDO_ALL / REDO_ALL / UNDO_ALL   # longpress variant
                # else: no action on release (only note-on fires)
            # else (Playing state, non-special cmd): silently discard


Summary: Latency Sources and Mitigations
==========================================

.. list-table::
   :widths: 30 35 35
   :header-rows: 1

   * - Scenario
     - Latency added
     - Notes
   * - Simple hit / tap (``type_cmd_hit``)
     - One process block (JACK buffer size)
     - Command is queued this block, ``ports[Multi]`` written next block.
   * - SUS note-on (``type_cmd_down``)
     - One process block
     - Same as above; note-off fires independently.
   * - SUS note-off (``type_cmd_upforce``)
     - One process block
     - Fires unconditionally; no wait for longpress check.
   * - Longpress — note-on action
     - One process block
     - Note-on fires the normal command immediately.
   * - Longpress — note-off action
     - Physical hold time (> 1 s) + one process block
     - UNDO_ALL fires only after button release; hold duration is the
       intent, so this is not incidental latency.
   * - Doubletap — first tap
     - One process block
     - Normal command fires with no wait.
   * - Doubletap — second tap
     - Physical inter-tap gap + one process block
     - UNDO_TWICE fires when second note-on arrives; no speculative delay.
   * - Trigger latency compensation
     - Negative (early) by ``_buffersize`` samples
     - ``ports[TriggerLatency]`` shifts the plugin's internal loop-point
       backward to compensate for the JACK callback jitter.
   * - Input/output latency compensation
     - Negative (early) by hardware round-trip latency
     - ``ports[InputLatency]`` and ``ports[OutputLatency]`` compensate for
       the audio interface's internal buffering so loop boundaries align
       with the physical world.

The design philosophy is clear: **commands always fire at the earliest
possible moment** (note-on for most operations, note-off only for SUS and
longpress-variant actions).  Neither longpress nor doubletap introduce a
speculative dwell period before the primary action fires.  The only
"cost" is that a doubletap produces an intermediate state (brief entry
into Record mode) before UNDO_TWICE cleans it up — a trade-off that is
entirely appropriate for live looping usage.


Dependencies and Related Files
================================

.. list-table::
   :widths: 30 70
   :header-rows: 1

   * - File / Library
     - Role
   * - ``plugin.cc`` / ``plugin.hpp``
     - LADSPA looping engine; implements the state machine and DSP.
   * - ``engine.hpp``
     - Owns the collection of ``Looper`` instances; drives the JACK
       callback; provides common I/O buffers and port lookup.
   * - ``audio_driver.hpp``
     - Abstracts JACK port creation, buffer access, and latency queries.
   * - ``event.hpp`` / ``event_nonrt.hpp``
     - Event type definitions and command/control enumerations.
   * - ``panner.hpp``
     - Simple equal-power panner used for common output mixing.
   * - ``filter.hpp``
     - ``OnePoleFilter`` used as an anti-aliasing lowpass before/after SRC.
   * - ``command_map.hpp``
     - Bidirectional string↔enum mapping used by XML persistence.
   * - libsamplerate (SRC)
     - Real-time sample-rate conversion for the ``Rate`` port (playback
       speed control).
   * - RubberBand
     - Real-time time-stretching and pitch-shifting (``StretchRatio``,
       ``PitchShift``, ``TempoStretch`` controls).
   * - libsndfile
     - Loop file I/O (WAV Float32 and PCM16 formats).
   * - libpbd (``xml++.h``, ``NonBlockingLock``)
     - XML session serialisation and the non-blocking mutex used to protect
       the loop during file I/O.


Trigger Latency: Deep Dive
===========================

The Core Problem: Block-Based Processing vs. Continuous Time
-------------------------------------------------------------

JACK (and any block-based audio API) does not process audio sample-by-sample.
It delivers audio to your callback in chunks — the *buffer size*, typically
64–1024 samples.  At 48 kHz with a 256-sample buffer, each block represents
about 5.3 ms of audio time.

From the plugin's perspective, time is **quantized to block boundaries**.  It
runs for exactly ``nframes`` samples, then stops and waits for the next
callback.  It has no mechanism to say "start recording at sample 137 of this
block" — it either records the whole block or it doesn't.

``do_event()`` can be called from a non-real-time thread (OSC handler, MIDI
callback, GUI thread) at *any* wall-clock moment.  When that happens it sets
``request_pending = true``, but the command is only written to ``ports[Multi]``
and delivered to the plugin at the **start of the next** ``run()`` **call** —
i.e., at the next block boundary.

There is therefore an inherent delay between when the user's finger hits a
button and when the plugin acts on it: anywhere from zero to one full buffer
period, depending on where in the current block the event arrived::

    Block N                         Block N+1
    |←—————— buffersize frames ————→|←—————— buffersize frames ————→|
    |                    ↑           |                                |
    |                event arrives   |                                |
    |                (mid-block)     |                                |
    |                                ↑                                |
    |                          plugin sees command,                   |
    |                          starts recording here                  |

The audio that passed through during the gap is already gone — played through
the plugin in pass-through mode without being recorded.  That gap is the
**trigger jitter**, bounded by ``[0, buffersize)`` samples.

What ``TriggerLatency`` Does Inside the Plugin
-----------------------------------------------

``ports[TriggerLatency]`` tells ``plugin.cc`` to **shift its internal
loop-start marker backward** by that many samples when it commits a record
endpoint.  When the plugin transitions into Record state at the start of block
N+1, it knows the user's intent was ``TriggerLatency`` samples earlier, so it
marks the loop start at "now minus ``TriggerLatency``" rather than at the raw
block boundary.  This accomplishes two things:

1. **Accurate loop length.** Record start and end points reflect the actual
   musical duration the performer intended, not one shortened by a random
   sub-block offset.

2. **Sync-grid coherence.** When locked to MIDI clock or another loop,
   record boundaries must align to the sync grid.  A random sub-block offset
   would cause drift on every take.

The plugin can shift the marker backward because it maintains a circular input
buffer with enough headroom to look into the recent past.  The ``InputLatency``
port serves a complementary purpose — it accounts for the latency of the
physical audio interface (ADC pipeline, hardware buffer stages) so that
captured audio aligns with the real-world sound rather than its digitised,
delayed representation.

Why ``_buffersize`` Is the Right Value
---------------------------------------

.. code-block:: cpp

    ports[TriggerLatency] = _buffersize; // jitter correction

Setting it to exactly ``_buffersize`` is a **worst-case** correction.  Since
jitter is uniformly distributed over ``[0, buffersize)`` samples:

- **Best case** (event at block start): correction overshoots by
  ``buffersize`` samples — a small, fixed, constant error.
- **Worst case** (event at block end): correction is exactly right.
- **Average case**: overcorrection by ``buffersize / 2`` samples.

A constant offset of up to one buffer period (~5 ms at 48 kHz / 256 samples)
is **far less damaging** than random jitter.  A constant offset means the loop
starts slightly early — a performer can compensate by adjusting their playing.
Random jitter means loop boundaries drift relative to the beat on every take,
which is musically unusable.  This is the same philosophy used in DAW latency
compensation: accept a fixed, known delay rather than a variable, unknown one.

Sources of Jitter in Practice
------------------------------

**1. JACK Buffer-Boundary Jitter (the primary source)**

As described above, bounded by ``[0, buffersize)`` samples.  This is what
``TriggerLatency`` directly compensates for.

**2. JACK MIDI Timestamping (unexploited precision)**

JACK's MIDI API provides sample-accurate timestamps for MIDI events within a
block — each event carries an ``nframes_t time`` field indicating the exact
sample within the current block at which it arrived.  However, SooperLooper
does **not** use these intra-block timestamps for command dispatch.  The
command is queued and applied uniformly at the next block boundary, regardless
of the event's offset within the block.  This means even with perfect
timestamps available, sub-block precision is not exploited and the full block
of jitter remains.  See the next section for a discussion of what exploiting
these timestamps would require.

**3. Thread Scheduling Jitter (non-RT event delivery)**

Commands arriving via OSC (UDP socket) or the GUI are delivered by
non-real-time threads.  Linux's CFS scheduler can delay non-RT threads by
tens of milliseconds under load.  This means ``do_event()`` may be called up
to ~20–50 ms after the user acted.  Since ``do_event()`` is not the audio
thread, this only affects when the command gets queued; the plugin still sees
it at the next block boundary after queuing.  This is an additional,
unbounded jitter source on top of the block-boundary jitter for OSC/GUI
commands.  JACK MIDI events handled inside the RT callback avoid this source
entirely.

**4. USB MIDI Jitter**

A USB MIDI controller introduces up to ~1 ms of hardware jitter (USB
full-speed has 1 ms frame intervals; USB high-speed has 125 µs).  At 48 kHz,
1 ms ≈ 48 samples — less than one buffer period at typical settings, so it
is subsumed into the block-boundary jitter and requires no separate treatment.

**5. The One-Block Same-Command Deferral**

There is a subtle additional latency source in ``run()``:

.. code-block:: cpp

    if (request_pending) {
        if (ports[Multi] == requested_cmd) {
            ports[Multi] = -1;   // defer till next call
        } else {
            ports[Multi] = requested_cmd;
            request_pending = false;
        }
    }

If the plugin already holds the same command value in ``ports[Multi]`` from the
previous block (e.g., two identical rapid commands), the new command is
deferred by one additional block.  This prevents the plugin from conflating
two distinct events that happen to carry the same integer value — the plugin
must see a ``-1`` between them.  In normal use this adds no latency, but a
rapidly repeated identical command could be delayed by one extra buffer period.

Full Latency Budget at a Typical Setting
-----------------------------------------

At 48 kHz, 256-sample buffer:

.. list-table::
   :widths: 42 20 38
   :header-rows: 1

   * - Source
     - Samples
     - Time
   * - Block-boundary jitter (worst case)
     - 0 – 256
     - 0 – 5.3 ms
   * - ``TriggerLatency`` correction
     - −256
     - −5.3 ms
   * - Net trigger error after correction (constant, not random)
     - 0 – 256
     - 0 – 5.3 ms
   * - Hardware input latency (``InputLatency``)
     - varies
     - ~1 – 10 ms (interface-dependent)
   * - Hardware output latency (``OutputLatency``)
     - varies
     - ~1 – 10 ms (interface-dependent)
   * - Non-RT scheduling jitter (OSC / GUI commands only)
     - 0 – ~1 000
     - 0 – 20 ms

The three compensation ports together aim to make the plugin's effective loop
boundary align with the real-world sound as heard by the performer, accounting
for both the command-timing uncertainty and the physical round-trip through the
audio hardware.


Possible Latency Reductions
============================

Several opportunities exist in the code to reduce latency beyond what the
current implementation achieves.  They are listed roughly in order of
impact and implementation difficulty.

1. Exploit JACK MIDI Intra-Block Timestamps
-------------------------------------------

**Potential saving: 0 to buffersize−1 samples per trigger event.**

This is the single largest available improvement.  JACK MIDI delivers each
event with a sample-accurate ``time`` offset within the current block.  If
the MIDI handling code passed that offset down to ``do_event()`` and stored
it alongside the pending command, ``run()`` could set::

    ports[TriggerLatency] = actual_sample_offset_within_block;

instead of the fixed worst-case::

    ports[TriggerLatency] = _buffersize;

At the average case (event mid-block) this halves the residual trigger error.
At best (event near block end) it reduces the error to near zero.

This requires modest changes: ``do_event()`` would need an additional
``nframes_t intra_block_offset`` parameter, the MIDI dispatch path in the
engine would need to supply it, and ``run()`` would use the stored value in
place of ``_buffersize``.  The compensation would then be exact rather than
worst-case.

Note that OSC and GUI commands cannot benefit from this, since they arrive
outside the RT callback with no JACK timestamp at all.  But most
latency-critical commands (record, overdub start/stop) are typically bound to
MIDI foot controllers, so the improvement would be felt where it matters most.

2. Split the Block on Command Arrival (Sub-Block Scheduling)
-------------------------------------------------------------

**Potential saving: same as above, but at the DSP level.**

A more invasive — but more complete — approach is to split the current buffer
at the point the MIDI event timestamp indicates, run the plugin for
``[0, event_offset)`` frames in the old state and then for
``[event_offset, nframes)`` frames in the new state, all within a single
``run()`` call.  This is the approach used by many professional DAW MIDI
engines and by JACK-aware soft-synths.

The pseudocode for a single channel would become:

.. code-block:: text

    run_loops_with_event(offset, event_offset, nframes):
        # First sub-block: run in current state (no command)
        descriptor->run(_instances[i], event_offset)

        # Apply command exactly here
        ports[Multi] = requested_cmd
        descriptor->run(_instances[i], 0)   # state transition, 0 frames
        ports[Multi] = -1

        # Second sub-block: run in new state
        descriptor->run(_instances[i], nframes - event_offset)

This eliminates trigger jitter entirely for MIDI events.  The cost is
increased complexity in ``run_loops()``, the need to advance buffer pointers
between sub-blocks, and the requirement that ``plugin.cc``'s ``run()``
correctly handles a zero-frame call as a state-transition signal (which the
existing ``load_loop()`` code already relies on, so there is precedent).

3. Eliminate the Same-Command One-Block Deferral
-------------------------------------------------

**Potential saving: one buffer period for rapid repeated commands.**

The deferral in ``run()``:

.. code-block:: cpp

    if (ports[Multi] == requested_cmd) {
        ports[Multi] = -1;   // defer
    }

exists because the plugin cannot distinguish "this is the same command held
over from last block" from "this is a new, identical command".  Two
solutions are possible without changing the plugin:

- **Use a sequence number or generation counter** alongside the command
  value.  Write a composite value such as
  ``(generation_bit << 8) | cmd`` to ``ports[Multi]`` and have the
  plugin mask off the low byte for the command.  This would require a
  coordinated change to ``plugin.cc``.

- **Force a ``-1`` frame** between commands: call ``descriptor->run(0)``
  with ``ports[Multi] = -1`` immediately after issuing each command,
  then re-apply the new command in the same ``run()`` call.  This costs
  one extra LADSPA ``run()`` invocation per command but eliminates the
  extra block of deferral with no protocol change.

4. Reduce the JACK Buffer Size
-------------------------------

**Potential saving: proportional to buffer size reduction.**

This is not a code change but a deployment choice.  Every latency figure
in this document scales linearly with ``_buffersize``.  Reducing from 256
to 128 samples halves all block-boundary jitter and the trigger latency
correction.  At 48 kHz:

.. list-table::
   :widths: 25 25 25 25
   :header-rows: 1

   * - Buffer size
     - Block duration
     - Worst-case jitter
     - ``TriggerLatency``
   * - 512 samples
     - 10.7 ms
     - 10.7 ms
     - 10.7 ms
   * - 256 samples
     - 5.3 ms
     - 5.3 ms
     - 5.3 ms
   * - 128 samples
     - 2.7 ms
     - 2.7 ms
     - 2.7 ms
   * - 64 samples
     - 1.3 ms
     - 1.3 ms
     - 1.3 ms

The practical floor is determined by the audio interface's capabilities and
the CPU budget available for the RT thread.  64 samples is achievable on
modern hardware with low-latency interfaces; below that, xruns become likely.

5. Prioritise the OSC / Event Delivery Thread
----------------------------------------------

**Potential saving: 0 – 20 ms for non-MIDI commands.**

Commands arriving via OSC or the GUI suffer from Linux CFS scheduling jitter
on the delivery thread.  Setting the event handler thread to
``SCHED_FIFO`` at a moderate real-time priority (below the JACK RT thread)
would reduce this jitter from tens of milliseconds to sub-millisecond.
SooperLooper already uses real-time priorities for the audio thread; extending
that to the OSC receiver is a low-risk change.  The benefit is limited to
non-MIDI command paths, but for performers using networked OSC controllers
(tablets, custom hardware) this can be the dominant latency source.

6. Apply Pending Stretch Ratio Within the Current Block
--------------------------------------------------------

**Potential saving: one buffer period when changing stretch ratio.**

The stretch ratio update in ``run()`` is applied at the top of the block,
meaning any ``_pending_stretch`` flagged by ``do_event()`` takes effect on
the *next* ``run()`` call, not the current one:

.. code-block:: cpp

    if (_pending_stretch) {
        // applied here — always at the start of the NEXT block
        _stretch_ratio = _pending_stretch_ratio;
        ...
        _pending_stretch = false;
    }

If the pending flag is checked and applied *before* ``run_loops()`` is called
within the *same* callback in which ``do_event()`` set it (which requires the
event to arrive within the RT callback), the stretch change would take effect
one block sooner.  In practice, stretch ratio changes are not timing-critical
in the way record/overdub commands are, so this is a low-priority
improvement.

Summary of Improvements
------------------------

.. list-table::
   :widths: 35 20 25 20
   :header-rows: 1

   * - Improvement
     - Saving (typical)
     - Complexity
     - Affects
   * - JACK MIDI intra-block timestamps (``TriggerLatency`` formula)
     - 0 – buffersize samples (exact, non-variable)
     - Low–Medium
     - MIDI commands
   * - Sub-block buffer splitting
     - 0 – buffersize samples (exact, audio-complete)
     - High
     - MIDI commands
   * - Eliminate same-command deferral
     - 0 – buffersize samples (rare)
     - Low
     - All commands
   * - Reduce JACK buffer size
     - Proportional
     - Config change
     - All commands
   * - RT-priority OSC thread
     - 0 – 20 ms
     - Low
     - OSC / GUI commands only
   * - In-block stretch ratio application
     - One block period
     - Low
     - Stretch changes


Clarifications and Corrections from Extended Analysis
======================================================

Does the RT-Priority OSC Thread Speed Up ``looper.cpp`` ↔ ``plugin.cc``?
--------------------------------------------------------------------------

No.  The RT-priority OSC thread improvement applies **only** to the path
from an external network socket into the ``request_pending`` flag inside
``Looper``.  The communication between ``looper.cpp`` and ``plugin.cc``
happens entirely within the JACK RT audio callback, synchronously, in the
same thread::

    JACK callback → Engine::run() → Looper::run() → run_loops()
                                                   → descriptor->run()

These are direct compiled-in C function calls — ``plugin.cc`` is a LADSPA
``.so`` whose ``run()`` is called inline.  There is no thread boundary, no
queue, and no lock on that path.  It is already as fast as a C function call
and cannot be made faster by scheduling policy changes.

Does the Timestamp-Based ``TriggerLatency`` Approach Introduce Variability?
----------------------------------------------------------------------------

**No — when implemented with the correct formula.**  An earlier part of this
document stated the approach saves "0 to buffersize/2 samples (average)", which
implied residual variability.  That framing was imprecise.  With the correct
formula the correction is exact and the residual is zero.

The correct formula is::

    TriggerLatency = buffersize − actual_intra_block_offset

not ``= actual_intra_block_offset``.  Derivation: a MIDI event at intra-block
offset ``k`` means the user's intent was at sample ``k`` of block N.  The
plugin acts at sample 0 of block N+1.  The true delay is
``(buffersize − k)`` samples.  Setting ``TriggerLatency = buffersize − k``
compensates exactly.  Residual error is zero and non-variable.

The current code sets ``TriggerLatency = buffersize`` (constant worst-case).
The improvement replaces a fixed overcorrection with an exact correction.
Neither version introduces variability; the improved version simply achieves
accuracy.

The remaining distinction between this approach and sub-block buffer splitting
is **not** timing accuracy — both are exact — but **audio content**:

.. list-table::
   :widths: 40 30 30
   :header-rows: 1

   * - Property
     - Timestamp-based ``TriggerLatency``
     - Sub-block buffer splitting
   * - Loop marker position
     - Exact
     - Exact
   * - Audio from samples ``[k, buffersize)`` of block N
     - **Lost** (plugin was in old state)
     - **Captured**
   * - Hard transient at loop start
     - May be slightly clipped
     - Complete
   * - Implementation complexity
     - Low–Medium
     - High

Does Sub-Block Splitting Eliminate Trigger Latency Entirely?
------------------------------------------------------------

Yes, completely and without any residual variability.  It is the only approach
that also captures the audio samples between the event timestamp and the block
boundary.  The cost is higher implementation complexity.


Mididings Timestamp Behaviour — Correction
===========================================

An earlier analysis of the mididings pipeline stated that mididings would
assign output timestamp ``0`` to remapped events, destroying the original
intra-block offset and making the SooperLooper timestamp improvements moot.
**This is incorrect for the** ``jack-rt`` **backend.**  Inspection of the
actual mididings source (``src/backend/jack.cc``) reveals a more careful
design.

How mididings Stores Timestamps
---------------------------------

In ``fill_input_queue()``, mididings converts each incoming JACK MIDI event
into its internal ``MidiEvent`` type using the **absolute** frame counter::

    MidiEvent ev = buffer_to_midi_event(
        jack_ev.buffer, jack_ev.size,
        port, _current_frame + jack_ev.time);   // absolute frame

``jack_ev.time`` is the JACK intra-block offset (0 … nframes−1).
``_current_frame`` is a monotonically increasing counter of total frames
processed.  The resulting ``frame`` field in the ``MidiEvent`` is therefore
the exact sample-accurate absolute position of the event — the intra-block
offset is fully preserved.

How mididings Writes Output Timestamps
----------------------------------------

In ``write_event()``, the output intra-block offset is computed from the
stored absolute frame::

    if (frame >= _current_frame) {
        // event received within current period, zero delay
        write_at_frame = frame - _current_frame;        // restores original offset
    } else if (frame >= _current_frame - nframes) {
        // event received during last period, one period delay
        write_at_frame = frame - _current_frame + nframes;
    } else {
        // older event — send immediately
        write_at_frame = 0;
    }

When mididings processes an event in the **same JACK period** in which it
arrived (the normal case with ``jack-rt``), ``frame >= _current_frame`` is
true and ``write_at_frame = frame − _current_frame = jack_ev.time`` — the
original intra-block offset is reproduced exactly on the output port.

Implications for the Mididings Pipeline
-----------------------------------------

.. list-table::
   :widths: 30 35 35
   :header-rows: 1

   * - mididings backend
     - Timestamp behaviour
     - Effect on SooperLooper improvements
   * - ``jack-rt``
     - Intra-block offset **preserved** through remapping
     - Timestamp-based ``TriggerLatency`` and sub-block splitting work
       correctly; improvements are fully effective
   * - ``jack`` (buffered)
     - Events processed in a separate thread; at least one extra period of
       latency added; offset information effectively lost
     - Improvements to SooperLooper would yield no benefit; use
       ``jack-rt`` instead
   * - OSC output from mididings
     - Exits the RT domain entirely; non-RT thread scheduling jitter applies
     - RT-priority OSC thread in SooperLooper (#5) partially helps; all
       timestamp-based improvements are irrelevant on this path

The practical recommendation is therefore: **use** ``jack-rt`` **in
mididings**.  The documentation warns that Python ``Process()`` units can
cause xruns in this mode, but pure C++ remapping units (``Transpose``,
``Channel``, ``Filter``, ``KeySplit``, and similar) are explicitly described
as safe.  With ``jack-rt`` and no Python callbacks, the timestamp pipeline
is accurate end-to-end and all SooperLooper improvements remain valid.


Custom C++ JACK MIDI Processor — Complexity Assessment
=======================================================

The hypothetical replacement for mididings — a custom C++ JACK client that
performs MIDI remapping while explicitly preserving intra-block timestamps —
is not especially complex.  A complete, working implementation requires
roughly 250–400 lines of C++, depending on the richness of the remapping
rules.

Minimal Skeleton
-----------------

The core structure is straightforward:

.. code-block:: cpp

    // Compile: g++ -O2 -o midi_remap midi_remap.cc $(pkg-config --libs jack)

    #include <jack/jack.h>
    #include <jack/midiport.h>
    #include <cstring>
    #include <vector>

    static jack_client_t *client;
    static jack_port_t   *midi_in, *midi_out;

    // --- Remapping logic (edit to taste) ---
    static bool remap_event(const jack_midi_event_t &in,
                            uint8_t *out_buf, size_t &out_len)
    {
        // Example: transpose note-on/off on ch1 up by 2 semitones,
        //          pass everything else through unchanged.
        memcpy(out_buf, in.buffer, in.size);
        out_len = in.size;
        uint8_t status = in.buffer[0] & 0xF0;
        uint8_t chan   = in.buffer[0] & 0x0F;
        if ((status == 0x90 || status == 0x80) && chan == 0) {
            out_buf[1] = (uint8_t)(in.buffer[1] + 2);   // semitones
        }
        return true;   // return false to drop event
    }

    static int process_cb(jack_nframes_t nframes, void *)
    {
        void *in_buf  = jack_port_get_buffer(midi_in,  nframes);
        void *out_buf = jack_port_get_buffer(midi_out, nframes);
        jack_midi_clear_buffer(out_buf);

        uint32_t n = jack_midi_get_event_count(in_buf);
        for (uint32_t i = 0; i < n; ++i) {
            jack_midi_event_t ev;
            jack_midi_event_get(&ev, in_buf, i);

            uint8_t  mapped[32];
            size_t   mapped_len = 0;
            if (remap_event(ev, mapped, mapped_len)) {
                // Write at ORIGINAL intra-block offset — timestamp preserved
                jack_midi_event_write(out_buf, ev.time, mapped, mapped_len);
            }
        }
        return 0;
    }

    int main()
    {
        client   = jack_client_open("midi_remap", JackNoStartServer, NULL);
        midi_in  = jack_port_register(client, "in",  JACK_DEFAULT_MIDI_TYPE,
                                      JackPortIsInput,  0);
        midi_out = jack_port_register(client, "out", JACK_DEFAULT_MIDI_TYPE,
                                      JackPortIsOutput, 0);
        jack_set_process_callback(client, process_cb, NULL);
        jack_activate(client);
        // sleep / wait for signal
        pause();
        jack_client_close(client);
    }

The key line is ``jack_midi_event_write(out_buf, ev.time, …)`` — passing
``ev.time`` (the original intra-block offset) directly as the output
timestamp.  Timestamp preservation costs nothing; it is simply a matter of
not discarding the value.

What Makes It More Complex
---------------------------

The skeleton above is complete for simple one-to-one remappings.  The
additional complexity comes from:

**One-to-many fanout.** If a single input event must produce multiple output
events (e.g. a note-on that also triggers a CC), each output event needs a
timestamp.  The first should use ``ev.time``; subsequent ones should use
``ev.time + 1``, ``ev.time + 2``, etc. (JACK requires events in non-
decreasing timestamp order within a buffer).  This is a few lines of bookkeeping.

**Stateful transformations.** Chord expansion, arpeggiators, or scene-based
routing that depends on previously received events require a small state
structure — a few dozen lines.

**Runtime configurability.** If mapping rules need to change while the
process is running (e.g. switching presets via another MIDI message), the
remapping table must be protected by a lock-free mechanism (e.g. an atomic
pointer swap or a lock-free ringbuffer command queue from a non-RT config
thread).  This adds ~50–100 lines but is well-understood boilerplate.

**OSC-to-JACK translation.** If the remapper must also forward some events
as OSC to SooperLooper, it needs a UDP send socket in the process callback.
UDP ``sendto()`` from the RT thread is technically lock-free on Linux in
practice, but is not RT-safe in the strict sense and can occasionally stall.
The cleaner approach is to write OSC-destined events to a lock-free ringbuffer
and have a non-RT thread drain it — adding ~80 lines and re-introducing the
non-RT scheduling jitter on the OSC path.

Comparison to mididings ``jack-rt``
-------------------------------------

Given that mididings with ``jack-rt`` **already preserves timestamps
correctly**, the main reasons to write a custom C++ processor rather than
using mididings are:

- The remapping logic involves Python ``Process()`` callbacks (unsafe in RT).
- The required transformations are not expressible in mididings' patch language.
- Deployment simplicity (a single binary vs. a Python + Boost dependency tree).
- Sub-100 µs worst-case callback execution time is needed (C++ with no Python
  GIL contention).

For typical foot-controller remapping (note-to-CC, channel filter, scene
switching), mididings ``jack-rt`` with pure C++ units is equivalent in timing
behaviour to a custom implementation and is significantly less work to
maintain.


Using ``Process()`` with mididings and the Impact on ``jack-rt``
=================================================================

The Problem: ``Process()`` is a Python Callback in the RT Thread
-----------------------------------------------------------------

The mididings patch::

    mididings_run(
        Process(main_handler) >>
        Output(port=config.PORT_INDEX, channel=config.CHANNEL)
    )

uses ``Process()``, which calls the Python function ``main_handler`` **directly
inside the JACK process callback** when ``jack-rt`` is selected.  This is
explicitly flagged as unsafe in the mididings documentation:

    *Python code is not realtime safe, so it's recommended not to use the*
    *Process() unit in conjunction with the jack-rt backend.*
    *With the jack-rt backend, it [slow Python code] causes xruns which are*
    *potentially audible, may cause loss of MIDI events, and also affect*
    *other JACK clients!*

The reasons Python is not RT-safe:

- **The GIL (Global Interpreter Lock).** Any other Python thread holding the
  GIL when the JACK callback fires will block the callback until it is
  released.  This is unbounded and can easily exceed the JACK deadline.
- **Memory allocation.** Python allocates heap memory for almost every
  operation — string formatting, list manipulation, object creation.  The
  ``malloc``/``free`` path is not lock-free and can block on a system mutex.
- **I/O and GUI operations.** If ``main_handler`` updates a display, writes to
  a file, sends a network packet, or calls any Tk/Qt/GTK function, these are
  all potentially blocking operations that will cause xruns.

The documentation does note a pragmatic middle ground: "In practice, if you
require sample-accurate MIDI processing with zero latency, but your JACK
period size is large enough, you can often get away with jack-rt running Python
code in a realtime context."  In other words, with a large buffer (512 or
1024 samples) and a fast, simple Python handler that does no I/O, xruns may be
rare enough in practice to be tolerable.  But this is a gamble, not a
guarantee, and it will break under load.

The ``jack`` (buffered) Backend: The Safe Conservative Option
--------------------------------------------------------------

With the buffering ``jack`` backend, Python code that does not finish in
time merely causes MIDI events from mididings to be slightly delayed, rather
than causing xruns.  Specifically, it adds exactly one JACK period of
latency to all MIDI events.  At 256 samples / 48 kHz that is 5.3 ms; at 128
samples it is 2.7 ms.

For a looper used with a foot controller, a one-period extra latency is
usually acceptable — it is constant and therefore compensable.  The downside
is that the mididings ``jack`` backend processes events in a separate thread,
which means:

- Intra-block timestamps are not preserved on the output (as established
  earlier in this document).
- The SooperLooper timestamp-based ``TriggerLatency`` improvements described
  in this document cannot benefit from sub-buffer precision for commands
  routed through mididings.
- The current ``TriggerLatency = _buffersize`` worst-case correction in
  SooperLooper is the right setting and should not be changed.

This is the simplest and most robust architecture if ``main_handler`` does
anything non-trivial.

Architectural Alternatives When GUI / Logic Must Stay in Python
----------------------------------------------------------------

If sub-buffer timestamp precision is desired (i.e. to benefit from the
SooperLooper improvements described in this document), the core requirement is
to keep Python entirely **off** the RT JACK callback path.  Three architectures
achieve this, in increasing order of effort:

**Option A — Split the mididings patch into RT and non-RT parts**

Restructure the mididings patch so that the time-critical MIDI output uses
only pure C++ units, while the Python ``main_handler`` is invoked for
side-effects only (display, state tracking) via a non-blocking mechanism.

mididings provides ``Call()`` as a non-RT alternative to ``Process()``.
``Call()`` puts the event on a queue and invokes the Python function from
a **non-RT thread**, so it never blocks the JACK callback:

.. code-block:: python

    from mididings import *
    from mididings.extra import Call

    def update_display(ev):
        # runs in non-RT thread — safe to do GUI, I/O, etc.
        my_gui.update(ev.note, ev.velocity)
        # return value is ignored; cannot modify or suppress the event

    config(backend='jack-rt')
    mididings_run(
        # RT path: pure C++ units, timestamps preserved
        Output(port=config.PORT_INDEX, channel=config.CHANNEL) //
        # Non-RT side-effect: Python called asynchronously
        Call(update_display)
    )

The ``//`` operator runs both branches in parallel (Fork).  The ``Output``
branch runs synchronously in C++ with full timestamp fidelity.  The
``Call`` branch posts to a thread-safe queue without blocking the callback.
The display update happens slightly later (sub-millisecond on a lightly loaded
system) but never causes xruns.

The limitation is that ``Call()`` receives a copy of the event and **cannot
modify or filter** it — it is for observation only.  If ``main_handler``
currently transforms events (changes note numbers, suppresses some events,
generates new ones), those transformations must be rewritten as pure mididings
C++ units.

**Option B — Two separate JACK clients**

Run two separate processes: one for RT MIDI and one for Python logic.

.. code-block:: text

    Physical MIDI controller
           ↓
    [Process A: mididings jack-rt, pure C++ units only]
           ↓ JACK MIDI (timestamps preserved)
    [SooperLooper]

    [Process A]  ──→ OSC/UDP ──→  [Process B: Python GUI + logic]
                  (non-blocking send from RT thread, or via ringbuffer)

Process A handles all RT MIDI remapping.  It also sends a notification (OSC
or a named pipe) to Process B for display purposes.  Process B never touches
the JACK callback and can run any Python it likes.

This is the cleanest architecture but requires splitting the existing
``main_handler`` logic into two parts: the RT-safe remapping rules (expressed
as mididings C++ units) and the display/statekeeping Python (in Process B).

**Option C — Replace mididings with a custom C++ JACK client**

As described in the previous section, a custom C++ JACK client (roughly 250–
400 lines) can perform all remapping with full timestamp preservation, zero
Python in the RT path, and no mididings dependency.  Python GUI/logic runs in
a separate thread or process communicating via a lock-free queue or OSC.

This is the highest-effort option but gives the most control and the lowest
worst-case latency.

Summary: Which Option to Use
------------------------------

.. list-table::
   :widths: 22 13 13 13 13 13 13
   :header-rows: 1

   * - Architecture
     - Xrun risk
     - Extra latency
     - Timestamps preserved
     - Python GUI safe
     - Event modification in Python
     - Effort
   * - Current (``Process()``, ``jack-rt``)
     - **High**
     - None
     - Yes (if no xrun)
     - No
     - Yes
     - (existing)
   * - ``Process()``, ``jack`` buffered
     - None
     - +1 period
     - No
     - Yes
     - Yes
     - Trivial (change one config line)
   * - Option A (``Call()`` + C++ units, ``jack-rt``)
     - None
     - None
     - Yes
     - Yes (async)
     - Only via C++ units
     - Low–Medium
   * - Option B (two processes, ``jack-rt``)
     - None
     - None
     - Yes
     - Yes (separate process)
     - Only via C++ units
     - Medium
   * - Option C (custom C++ client)
     - None
     - None
     - Yes
     - Yes (separate thread/process)
     - Only via C++ logic
     - High

For most performers, the **immediate pragmatic fix** is to switch to
``backend='jack'`` (one config line change).  The one-period latency it adds
(~2.7–5.3 ms at typical buffer sizes) is constant and will not be perceptible
in live looping.  The SooperLooper timestamp improvements then become
irrelevant, but the looper will behave correctly and xrun-free.

If sub-buffer timing precision is genuinely needed (very small JACK buffers,
tight sync with external clock), **Option A** is the recommended path: it
keeps mididings, keeps Python for logic/display via ``Call()``, and moves
only the event transformation rules to C++ units — which for simple remapping
(note filtering, channel mapping, CC conversion) is a small change to the
existing mididings patch.


Adding Custom C++ Units to mididings: Requirements and Complexity
=================================================================

No Plugin Mechanism Exists — Recompilation Is Mandatory
--------------------------------------------------------

There is no dynamic plugin or extension-loading mechanism in mididings.
Inspection of ``src/python_module.cc`` shows that every unit class is
registered inside a single ``BOOST_PYTHON_MODULE(_mididings)`` block, compiled
into the monolithic ``_mididings.so`` Python extension.  To add a new C++
unit you must:

1. Modify the mididings C++ source (add a new ``.hh``/``.cc`` unit file in
   ``src/units/``).
2. Register the new class with Boost.Python in ``python_module.cc``.
3. Add the new source file to the ``sources`` list in ``setup.py``.
4. Recompile the entire package.

The modern community fork makes step 4 straightforward::

    git clone https://github.com/mididings/mididings
    cd mididings
    python -m venv venv
    source venv/bin/activate
    # ... edit sources ...
    pip install .

The build is a standard Python C-extension build via ``setup.py``/
``pyproject.toml``, pulling in Boost.Python and Jack headers.  On a typical
Debian/Ubuntu system the build dependencies are::

    sudo apt install libboost-python-dev libjack-jackd2-dev python3-dev

Anatomy of a Custom Unit
------------------------

A mididings unit inherits from one of two base classes in
``src/units/base.hh``:

- ``Unit`` — processes one event in-place; returns ``true`` to keep it,
  ``false`` to drop it.
- ``UnitEx`` — can replace one event with zero, one, or many output events
  (needed for sequence generation or fan-out).

A simple transformer (modify or filter single events) using ``Unit``:

.. code-block:: cpp

    // src/units/my_units.hh
    #pragma once
    #include "../midi_event.hh"
    #include "base.hh"

    namespace mididings { namespace units {

    class MyRemapper : public Unit {
    public:
        MyRemapper(int input_note, int output_note, int suppress_note)
            : _in(input_note), _out(output_note), _suppress(suppress_note)
        {}

        bool process(MidiEvent & ev) override {
            if (ev.type != MIDI_EVENT_NOTEON &&
                ev.type != MIDI_EVENT_NOTEOFF) return true;  // pass through
            if (ev.note.note == _suppress) return false;     // drop it
            if (ev.note.note == _in)       ev.note.note = _out;  // remap
            return true;
        }

    private:
        int _in, _out, _suppress;
    };

    }} // namespace

For sequence generation (one input event → multiple output events), you
inherit from ``UnitEx`` and write to the output event list instead of
modifying in place.  This is meaningfully more complex — roughly 40–80
additional lines — but follows the same pattern as the existing generator
units in ``src/units/generators.cc``.

The Boost.Python registration in ``python_module.cc`` is boilerplate::

    bp::class_<MyRemapper, bp::bases<Unit>>("MyRemapper",
        bp::init<int,int,int>());

After recompiling, the unit is available in Python as::

    from _mididings import MyRemapper
    # or wrap it in a nice Python function in mididings/__init__.py

Why Generic Python Wrapping Cannot Help
----------------------------------------

ctypes, cffi, SWIG, and Cython all ultimately call back into the Python
interpreter through its C API.  The RT-safety problem is not about the
language interface — it is structural:

- **The GIL (Global Interpreter Lock)** is a process-wide mutex.  Any Python
  thread holding it when the JACK callback fires will block the callback for
  an unbounded time.  There is no way to call Python from an RT context
  without acquiring the GIL, regardless of how the call is wrapped.
- **CPython's memory allocator** (``malloc``/``free`` under the hood of every
  Python object creation) is not lock-free and can block on the OS heap mutex.
- **Cython** compiles to C but still calls CPython API functions (``Py_INCREF``,
  ``PyObject_Call``, etc.) for every non-trivial operation.  It reduces
  overhead but does not eliminate GIL or allocation issues.

The only way to keep Python entirely out of the RT callback path while still
performing event transformation is to move the transformation logic into true
C++ — either as a custom mididings unit (requiring recompile) or as a
standalone JACK client.


Practical Recommendation Given a 512-Sample JACK Buffer Floor
==============================================================

At 512 samples / 48 kHz, one JACK period = **10.7 ms**.  This figure
dominates all other latency considerations in the pipeline.  The table below
shows the total mididings-to-plugin command latency under each architecture:

.. list-table::
   :widths: 40 20 20 20
   :header-rows: 1

   * - Architecture
     - mididings latency
     - TriggerLatency correction
     - Net loop-boundary error
   * - ``jack-rt``, ``Process()`` (current)
     - 0 ms (but xrun risk)
     - −10.7 ms (worst-case)
     - 0–10.7 ms constant
   * - ``jack`` buffered
     - +10.7 ms (one extra period)
     - −10.7 ms (worst-case)
     - 0–10.7 ms constant
   * - ``jack-rt``, C++ units only + ``Call()``
     - 0 ms (no xrun risk)
     - −10.7 ms (worst-case)
     - 0–10.7 ms constant
   * - ``jack-rt``, exact timestamp patch in SooperLooper
     - 0 ms
     - −exact offset
     - 0 ms (exact)

The crucial observation is that **the net loop-boundary error is the same
for the current code, the buffered backend, and the C++ units approach** —
all three land at "0 to 10.7 ms constant" because ``TriggerLatency``
compensates for the full block in each case.

The only architecture that achieves zero residual error at 512 samples is
the combination of ``jack-rt`` with exact intra-block timestamps propagated
from the MIDI source all the way through to the SooperLooper patch in
``plugin.cc``.  Given the effort required (timestamp plumbing in
SooperLooper, possibly custom C++ mididings units), this is not worth
pursuing unless the application requires tighter-than-10.7 ms loop boundary
precision — which is unusual at these buffer sizes.

**The recommended action** is therefore simply::

    config(backend='jack')   # change 'jack-rt' to 'jack'

One line.  ``main_handler`` continues to work unchanged in Python.
The extra 10.7 ms of latency is constant, compensated, and below the
typical perceptual threshold (~30 ms) for foot-controller response.
xruns in Ardour and other clients are eliminated.

If the system is later upgraded to support smaller JACK buffers (e.g. by
switching to a dedicated audio interface or an RT kernel), the architecture
can be revisited: at 128 samples the extra period is 2.7 ms and the
timestamp improvements become meaningful enough to justify the engineering
effort of Option A (``Call()`` + C++ units) or a custom JACK client.


Sub-Block Splitting Requires an End-to-End Clean Timestamp Chain
=================================================================

Why the mididings Stage Cannot Be Ignored
------------------------------------------

The sub-block buffer splitting improvement in SooperLooper (see
``Possible Latency Reductions``, item 2) operates on the ``.time`` field of
each incoming ``jack_midi_event_t`` — the intra-block sample offset at which
the MIDI event arrived.  For the split to be accurate, that field must carry
the original sub-block timestamp produced by the hardware MIDI interface.

The value in ``.time`` is not generated by SooperLooper.  It is written by
whichever JACK client is upstream — in this setup, mididings.  Specifically
it is the ``write_at_frame`` value computed in mididings'
``JACKBackend::write_event()`` (``src/backend/jack.cc``):

.. code-block:: cpp

    if (frame >= _current_frame) {
        write_at_frame = frame - _current_frame;    // same period: original offset
    } else if (frame >= _current_frame - nframes) {
        write_at_frame = frame - _current_frame + nframes;  // one period late
    } else {
        write_at_frame = 0;   // older: send at start of block
    }

The outcome depends entirely on whether the event arrives at
``write_event()`` within the same JACK period in which it was received from
hardware.

What Each mididings Backend Delivers to SooperLooper
------------------------------------------------------

**``jack-rt`` backend, pure C++ units.**
The event is processed and written within the same JACK period.
``frame >= _current_frame`` is true, so
``write_at_frame = frame − _current_frame = original_jack_ev.time``.
SooperLooper receives the original hardware intra-block offset.
The sub-block splitting patch works as designed and achieves zero residual
trigger jitter.

**``jack-rt`` backend with ``Process()`` (Python callback).**
Assuming the Python callback completes before the JACK deadline (which is
not guaranteed), the event is still written in the same period.
The timestamp calculation is the same as above — the original offset is
preserved.  The sub-block splitting patch would work, but the Python
callback introduces xrun risk that would affect Ardour and all other JACK
clients.

**``jack`` buffered backend.**
The event is received from hardware in period N, placed on a ringbuffer,
processed by the non-RT Python thread during period N or N+1, placed on
the output ringbuffer, and finally written to the JACK output port during
period N+1's process callback.  At that point, ``_current_frame`` is at
the start of period N+1, and the stored ``frame`` value is from period
N-1 or earlier.  This means
``frame < _current_frame − nframes``, so ``write_at_frame = 0``.

**Every event from the buffered backend arrives at SooperLooper with
timestamp 0** — the start of the block.  The sub-block splitting patch
would always split at sample 0, which is identical to the current behaviour
of running the entire block in the new state.  The patch provides zero
benefit.

The End-to-End Constraint as a Table
--------------------------------------

.. list-table::
   :widths: 38 18 18 26
   :header-rows: 1

   * - mididings configuration
     - Timestamp at SooperLooper input
     - Sub-block patch beneficial?
     - Obstacle
   * - ``jack`` buffered + ``Process()``
     - Always 0
     - No
     - Timestamps destroyed by buffer delay
   * - ``jack-rt`` + ``Process()``
     - Original offset preserved
     - Yes
     - xrun risk from Python in RT callback
   * - ``jack-rt`` + C++ units + ``Call()``
     - Original offset preserved
     - Yes
     - Requires rewriting ``main_handler`` as C++ mididings units
   * - Custom standalone C++ JACK client
     - Original offset preserved
     - Yes
     - Requires full reimplementation of remapper in C++

The Minimum Viable Patch Set
-----------------------------

To gain the full benefit of sub-block splitting in SooperLooper, both of
the following changes are necessary simultaneously:

1. **SooperLooper patch** — implement sub-block buffer splitting in
   ``Looper::run_loops()`` so that the plugin is called twice per block
   (in old state for ``[0, event_offset)`` frames, then in new state for
   ``[event_offset, nframes)`` frames).

2. **Upstream mididings change** — replace ``Process(main_handler)`` with
   pure C++ mididings units (using ``Call()`` for non-RT GUI/logic
   side-effects), enabling ``jack-rt`` without xrun risk and preserving
   the intra-block timestamp through the remapper.

Neither change alone is sufficient.  The SooperLooper patch without the
mididings change is wasted effort at the buffered backend — timestamps come
in as 0 and the split always occurs at sample 0.  The mididings change
without the SooperLooper patch preserves timestamps faithfully through to
SooperLooper, but SooperLooper's existing code discards sub-block precision
by applying the command at the next block boundary with a fixed
``TriggerLatency = _buffersize`` correction.

At 512 samples / 48 kHz, completing both changes saves at most **10.7 ms**
of worst-case trigger error compared to the current state.  The effort is
proportionate only if tight loop-boundary synchronisation (e.g. with
external MIDI clock or Hydrogen's transport) is a hard requirement.  For
free-running loops triggered manually, the current ``TriggerLatency``
compensation already produces musically acceptable accuracy and the
simpler ``backend='jack'`` configuration is the recommended path.


Sub-Block Splitting: Complete Architectural Analysis
=====================================================

.. note::
   This section supersedes earlier estimates and incorporates findings from
   all four source files: ``looper.hpp``, ``looper.cpp``, ``engine.cpp``,
   and ``midi_bridge.cpp``.

The Engine Already Implements Sub-Block Splitting
--------------------------------------------------

``Engine::process()`` already splits the JACK buffer at event boundaries.
For every event with a non-zero ``FragmentPos()``, the pattern is:

.. code-block:: text

    fragpos  = evt->FragmentPos()
    doframes = fragpos - usedframes

    looper->run(usedframes, doframes)   // pre-event: old state
    looper->do_event(evt)               // set request_pending
    ...
    looper->run(usedframes, nframes - usedframes)  // post-event: new state

``next_rt_event()`` merges and sorts both event queues by ``FragmentPos()``
chronologically.  This infrastructure has been in the engine from the
beginning.

The MIDI Path Is Entirely Non-RT — ``FragmentPos()`` Is Always Zero
--------------------------------------------------------------------

Reading ``midi_bridge.cpp`` reveals the critical architectural constraint.
All MIDI processing happens in a dedicated ``_midi_thread`` via ``poll()``
on a ``midi++`` port file descriptor — a completely separate POSIX thread
with no connection to the JACK process callback:

.. code-block:: text

    ALSA/midi++ port fd readable
        → poll() wakes _midi_thread
        → _port->read() feeds bytes to the midi++ Parser
        → Parser calls incoming_midi(..., timestamp)
        → queue_midi(b1, b2, b3, -1, timestamp)   ← framepos = -1
        → send_event(..., -1)
        → MidiCommandEvent(..., -1)                ← Engine: framepos = -1
        → createEvent(-1) → FragmentPos() = 0
        → Engine: run(0,0) [no-op], do_event(), run(0, nframes)

The ``-1`` framepos is not a bug.  It is the only correct value given this
architecture — there is no intra-block timing information in the non-RT
MIDI thread.  The engine's sub-block splitting infrastructure is real and
functional but receives ``fragpos = 0`` for every MIDI command, making the
pre-event ``run()`` call a permanent no-op.

Would Setting ``TriggerLatency`` to Zero via Config or OSC Work?
----------------------------------------------------------------

**No — it would make timing worse, not better.**

With ``fragpos = 0`` for all commands, every command fires at sample 0 of
the block in which the engine processes it.  The MIDI event physically
arrived sometime during the *previous* block — the non-RT thread can
process it at any point between two JACK callbacks.  The plugin marks
the loop start at sample 0 of the current block, but the user's intent
was between 0 and ``_buffersize`` samples earlier.

``TriggerLatency = _buffersize`` shifts the loop marker back by one full
block — the correct worst-case compensation for this architecture.
Setting it to zero removes all compensation and introduces uncompensated
random jitter of 0 to ``_buffersize`` samples per trigger.  Loop boundaries
become non-repeatable between takes.

``TriggerLatency = 0`` is only correct when a genuine sub-block split has
occurred at a known sample position, so the plugin fires at exactly the
right sample and needs no retrospective correction.  In the current code
that condition never occurs.

What Is Actually Required for Sub-Block Splitting
-------------------------------------------------

MIDI must be received **inside** ``Engine::process()`` — the JACK RT
callback — with the JACK intra-block ``.time`` field preserved.  This
requires adding a JACK MIDI input port and reading it in the RT thread.

``inject_midi()`` in ``MidiBridge`` already accepts a ``framepos`` and
passes it through the full chain correctly (it was intended for internal
injection from sync generation).  The required change is to call it from
the JACK RT callback with the actual JACK MIDI event timestamps:

.. code-block:: cpp

    // Engine::initialize() — register the port
    _jack_midi_in = jack_port_register(jack_client, "midi_in",
                                       JACK_DEFAULT_MIDI_TYPE,
                                       JackPortIsInput, 0);

    // Engine::process() — before the event loop, read JACK MIDI
    void* midi_buf = jack_port_get_buffer(_jack_midi_in, nframes);
    uint32_t n = jack_midi_get_event_count(midi_buf);
    for (uint32_t i = 0; i < n; ++i) {
        jack_midi_event_t ev;
        jack_midi_event_get(&ev, midi_buf, i);
        // ev.time is the exact intra-block sample offset
        _midi_bridge->inject_midi(ev.buffer[0], ev.buffer[1],
                                  ev.buffer[2], (long) ev.time);
    }

With this in place, ``createEvent(ev.time)`` stores the real offset,
``FragmentPos()`` returns it, the engine splits the block at the correct
sample, and the ``TriggerLatency`` fix becomes meaningful.

This approach also opens the direct JACK MIDI port that mididings would
connect to, eliminating the ALSA / ``a2jmidid`` bridge entirely.

``TriggerLatency`` Correction in ``Looper::run()``
---------------------------------------------------

Once the JACK MIDI port is in place, the ``TriggerLatency`` port must be
set to zero for events that were genuinely split, so the plugin does not
overcorrect.  ``do_event()`` can store the fragment position from the
event, and ``run()`` uses it:

.. code-block:: cpp

    // looper.hpp
    nframes_t _pending_event_offset;   // initialised to 0

    // do_event(): store the offset
    _pending_event_offset = (nframes_t) ev->FragmentPos();

    // run(): zero TriggerLatency when a real split occurred
    if (request_pending) {
        float saved_lat = ports[TriggerLatency];
        if (_pending_event_offset > 0)
            ports[TriggerLatency] = 0;
        ports[Multi] = requested_cmd;
        request_pending = false;
        _pending_event_offset = 0;
        run_loops(offset, nframes);
        ports[TriggerLatency] = saved_lat;
        return;
    }
    run_loops(offset, nframes);

Complete Patch Summary
----------------------

.. list-table::
   :widths: 30 18 12 40
   :header-rows: 1

   * - Change
     - File
     - Lines
     - Notes
   * - Register JACK MIDI input port
     - ``engine.cpp``, ``engine.hpp``
     - ~15
     - Adds ``_jack_midi_in`` member and ``jack_port_register()`` call
   * - Read JACK MIDI in ``process()``
     - ``engine.cpp``
     - ~20
     - Loop over ``jack_midi_get_event_count()`` / ``jack_midi_event_get()``
   * - Call ``inject_midi()`` with ``ev.time``
     - ``engine.cpp``
     - ~5
     - ``inject_midi()`` already correctly propagates ``framepos``
   * - Add ``_pending_event_offset`` member
     - ``looper.hpp``
     - 1
     - Boilerplate
   * - Store ``FragmentPos()`` in ``do_event()``
     - ``looper.cpp``
     - 3
     - One assignment plus reset
   * - Conditional ``TriggerLatency = 0`` in ``run()``
     - ``looper.cpp``
     - ~15
     - The only part requiring careful thought
   * - **Total**
     - 3 files
     - **~60 lines**
     - Substantially simpler than the original ~100–150 line estimate;
       all buffer arithmetic is free courtesy of the existing ``offset``
       parameter

The same-command deferral and gain ramp issues from the earlier analysis
do not apply: the engine already makes two separate ``run()`` calls, so
no internal restructuring of ``Looper::run()`` is needed.

The gain ramp distortion across the two ``run()`` calls is a pre-existing
latent behaviour that the patch does not introduce or worsen.

Impact on the ``a2jmidid`` Setup and OSC Timing
-------------------------------------------------

**JACK midi driver = none and a2jmidid.**
SooperLooper currently uses the ALSA sequencer interface for all MIDI
input.  With JACK's MIDI driver set to ``none``, SooperLooper's ALSA MIDI
ports are not automatically exposed as JACK MIDI ports — there is no JACK
MIDI driver active to bridge them.  The connection from mididings to
SooperLooper must therefore use either a direct ALSA MIDI connection
(visible in qjackctl's ALSA tab) or OSC.

The proposed JACK MIDI port patch registers a first-class JACK port
directly through the JACK API.  This port appears in the JACK graph
regardless of which MIDI driver JACK is using.  The rest of the setup
(hardware → ALSA → a2jmidid → JACK → mididings) is completely unchanged.
The patch adds a direct JACK connection point at the SooperLooper end only.

An additional benefit: bridging between ALSA and JACK MIDI via a2jmidid
always adds a constant delay of one JACK period to the MIDI data.  The
direct JACK MIDI connection added by the patch eliminates this extra period
for the mididings-to-SooperLooper leg, since that hop no longer traverses
any bridge.

**OSC commands.**
OSC is worse for timing than ALSA MIDI, not equivalent.  Both paths result
in ``createEvent(-1)`` → ``FragmentPos() = 0`` → command fires at the start
of the next block, so both share the same block-boundary jitter.  However,
the OSC listener runs under Linux's CFS scheduler and can be delayed 10–20
ms under load.  MidiBridge's dedicated ``_midi_thread`` with ``poll()`` is
more responsive in practice.  More importantly, ``TriggerLatency`` can only
compensate for the constant block-boundary component of jitter — it cannot
compensate for variable OSC scheduling jitter because that value is unknown
at the moment the block fires.

.. list-table::
   :widths: 25 22 22 31
   :header-rows: 1

   * - Path
     - Block-boundary jitter
     - Extra variable jitter
     - ``TriggerLatency`` compensates
   * - ALSA MIDI thread
     - 0–1 period
     - ~0 ms (dedicated poll thread)
     - Block jitter ✓
   * - OSC listener
     - 0–1 period
     - 0–20 ms (CFS-scheduled)
     - Block jitter only; variable remainder uncompensated
   * - JACK MIDI (with patch)
     - 0 (fired in RT callback at exact sample)
     - None
     - Exact; ``TriggerLatency = 0`` for these events

Setting ``TriggerLatency = 0`` via config or OSC without the JACK MIDI
patch would remove the only compensation for timing error that still
exists on the ALSA MIDI and OSC paths, introducing uncompensated random
jitter of 0 to ``_buffersize`` samples per trigger and making loop
boundaries non-repeatable between takes.

Mididings Integration With the JACK MIDI Port
----------------------------------------------

With the JACK MIDI input port added to SooperLooper:

- mididings connects its JACK MIDI output directly to SooperLooper's
  ``midi_in`` port — no ALSA bridge (``a2jmidid``) needed.
- With ``jack-rt`` backend and pure C++ units in mididings, timestamps are
  preserved end-to-end (as established earlier in this document).
- With ``jack-rt`` + ``Process()`` and a 512-sample buffer, timestamps are
  also preserved if the Python handler completes within the deadline — safe
  to test empirically.
- With the ``jack`` buffered backend, all output timestamps from mididings
  are 0, so ``TriggerLatency`` stays at ``_buffersize`` and ``do_event()``
  stores ``_pending_event_offset = 0``, correctly skipping the zero-latency
  path.  The patch degrades gracefully.


Sub-Block Splitting Patch: Corrected Engineering Assessment
============================================================

.. note::
   The following section supersedes and corrects the earlier sub-block
   splitting difficulty assessment, which was written before ``engine.cpp``
   was available.  Reading the engine reveals that the core of sub-block
   splitting is **already implemented**, changing the patch complexity
   estimate from ~100–150 lines to ~20–30 lines.

The Engine Already Implements Sub-Block Splitting
--------------------------------------------------

The ``Engine::process()`` loop in ``engine.cpp`` already splits the JACK
buffer at event boundaries using each event's ``FragmentPos()`` value.
The full pattern for every event with a non-zero fragment position is:

.. code-block:: text

    process(nframes):
        usedframes = 0

        for each event in time-sorted order (via next_rt_event()):
            fragpos  = evt->FragmentPos()         // intra-block position
            doframes = fragpos - usedframes

            for each looper:
                looper->run(usedframes, doframes)  // run in OLD state
                if event targets this looper:
                    looper->do_event(evt)          // set request_pending

            usedframes += doframes

        // final sub-block after all events:
        for each looper:
            looper->run(usedframes, nframes - usedframes)  // run in NEW state

The final ``run()`` call fires with ``request_pending`` already true.  At
the top of ``Looper::run()``, the command is immediately written to
``ports[Multi]``, and ``run_loops()`` executes the plugin in the new state
for exactly ``nframes - usedframes`` frames starting at the correct buffer
offset.  ``next_rt_event()`` merges and sorts both event queues by
``FragmentPos()`` to guarantee chronological ordering across MIDI and OSC
events.  **This is sub-block splitting.  It has been designed in from the
beginning.**

The entire "100–150 line patch" described in an earlier draft of this
document was solving a problem that is already solved in the engine.  That
estimate was made without access to ``engine.cpp``.

The Only Real Question: What Does ``MidiBridge`` Pass as ``framepos``?
-----------------------------------------------------------------------

Everything hinges on a single value in ``midi_bridge.cpp``.  MIDI events
enter the system through ``push_midi_command_event(type, cmd, instance,
framepos)`` and are stored as:

.. code-block:: cpp

    *evt = get_event_generator().createEvent(framepos);

``FragmentPos()`` then returns this stored value.

- If ``MidiBridge`` passes the JACK MIDI event's ``.time`` field (the
  intra-block sample offset) as ``framepos``, then ``FragmentPos()``
  returns the true hardware event position, ``doframes`` is non-zero, and
  **sub-block splitting already works correctly today with no code
  changes to** ``engine.cpp`` **or** ``looper.cpp``.

- If ``MidiBridge`` passes ``-1`` (the ``framepos`` default, meaning "now"),
  then ``createEvent(-1)`` creates an event at the current fragment
  generator position, which is ``0`` at the start of each block.
  ``doframes`` is therefore ``0``, the first ``run()`` call is a no-op,
  and the system degrades to full-block behaviour — functionally the same
  as without any sub-block splitting.

**``midi_bridge.cpp`` is the file that answers this question.**  It is the
only file still needed to determine whether sub-block splitting already
works or requires a one-to-two-line fix in the bridge.

``TriggerLatency`` Must Be Corrected Regardless
------------------------------------------------

Whether or not the MidiBridge fix is needed, the ``TriggerLatency`` port
requires a small correction to be accurate in the split case.

Currently ``ports[TriggerLatency]`` is set unconditionally to
``_buffersize`` in ``recompute_latencies()``.  When a MIDI event at
intra-block position ``k`` causes the engine to call
``run(k, nframes - k)`` with the command pending, the plugin marks the
loop start at the first sample of that sub-call, which is absolute sample
``k`` — exactly where the user intended.  Setting
``TriggerLatency = _buffersize`` then overcorrects, shifting the marker
back by a full block when no correction is needed.

The fix is confined to ``looper.cpp`` and ``looper.hpp``:

.. code-block:: cpp

    // looper.hpp: add one member, initialised to 0
    nframes_t _pending_event_offset;

    // looper.cpp, do_event() — store the fragment position from the event
    _pending_event_offset = (nframes_t) ev->FragmentPos();

    // looper.cpp, run() — apply TriggerLatency = 0 when split is valid
    if (request_pending) {
        float saved_lat = ports[TriggerLatency];
        if (_pending_event_offset > 0)
            ports[TriggerLatency] = 0;   // exact split; no correction needed
        ports[Multi] = requested_cmd;
        request_pending = false;
        _pending_event_offset = 0;
        run_loops(offset, nframes);
        ports[TriggerLatency] = saved_lat;  // restore for non-split blocks
        return;
    }
    run_loops(offset, nframes);

No changes to ``do_event()``'s signature are needed: it already receives
the ``Event*`` directly and can read ``FragmentPos()`` from it.

No changes to ``engine.cpp`` are needed for the splitting itself.

Revised Patch Summary
---------------------

.. list-table::
   :widths: 30 15 15 40
   :header-rows: 1

   * - Change
     - File
     - Lines
     - Notes
   * - Pass ``jack_ev.time`` as ``framepos``
     - ``midi_bridge.cpp``
     - 1–2
     - Only if MidiBridge currently passes ``-1``; may already be correct
   * - Add ``_pending_event_offset`` member
     - ``looper.hpp``
     - 1
     - Boilerplate
   * - Store ``FragmentPos()`` in ``do_event()``
     - ``looper.cpp``
     - 3
     - One-liner plus initialisation
   * - Conditional ``TriggerLatency = 0`` in ``run()``
     - ``looper.cpp``
     - ~15
     - The only logic that requires thought
   * - **Total**
     - 3 files
     - **~20–30**
     - Substantially simpler than the ~100–150 line earlier estimate

The same-command deferral interaction, buffer pointer arithmetic, and
gain ramp state issues described in the earlier draft were all predicated
on ``Looper::run()`` needing to call ``run_loops()`` twice internally.
Since the engine already makes two separate ``run()`` calls, those
complications do not arise.  Each ``run()`` call sees a clean state: no
command pending for the first, command pending for the second.

The only residual concern is the **gain ramp state** across the two
``run()`` calls: each call independently recomputes the per-sample gain
delta relative to its own ``nframes``, producing a slightly non-linear
ramp profile around the event boundary.  This is identical to a bug that
already exists latently whenever ``fragpos > 0`` and is not introduced
by the patch.  It is not audible in normal looping usage.

Next Step
---------

Upload ``midi_bridge.cpp`` (and optionally ``midi_bridge.hpp``) to
determine whether ``push_midi_command_event`` is already receiving
``jack_ev.time`` as ``framepos``.  If it is, the entire sub-block
splitting improvement reduces to the ~15-line ``TriggerLatency``
correction in ``looper.cpp`` alone.


Sub-Block Splitting Patch: Engineering Difficulty Assessment
=============================================================

The Three Processing Paths Are Not Equal
-----------------------------------------

``run_loops()`` contains three mutually exclusive processing paths.  Their
suitability for sub-block splitting differs substantially.

**Normal path** (``!resampled && !stretched && !pitched``): tractable.
The plugin processes raw buffers directly.  The existing ``offset``
parameter already advances all buffer pointers to the right position within
the JACK buffer, so calling the function twice with
``(offset, split)`` and ``(offset+split, nframes-split)`` automatically
handles all buffer arithmetic correctly at no additional cost.  This covers
the vast majority of real-world usage.

**Resampled path** (Rate port ≠ 1.0, using libsamplerate SRC): risky.
The ``SRC_STATE`` is a stateful streaming converter that maintains internal
history between calls.  The output frame count is not simply proportional
to input — the code already contains a retry loop to handle the
``output_frames_gen != nframes`` mismatch case.  Partial-block calls would
disrupt the SRC's internal state in non-obvious ways.  Leave unchanged in a
first patch.

**Stretched / pitched path** (RubberBand): risky for the same reason.
RubberBand operates on a latency-compensated internal queue;
``getSamplesRequired()`` and ``available()`` are queue-depth queries, not
simple counters.  Partial-block processing would feed unexpected chunk
sizes.  Leave unchanged.

The practical approach is to guard sub-block splitting with::

    if (!resampled && !stretched && !pitched && _pending_event_offset > 0)

and fall through to the existing single-block behaviour otherwise.

Required Changes
-----------------

**Change 1 — Add** ``_pending_event_offset`` **and plumb it through.**

A new member stores the intra-block MIDI event offset alongside the pending
command:

.. code-block:: cpp

    // looper.hpp
    nframes_t _pending_event_offset;   // set to 0 in initialise()

``do_event()`` gains an optional parameter (defaulting to 0 so all existing
call sites remain valid):

.. code-block:: cpp

    void do_event(Event *ev, nframes_t intra_block_offset = 0);
    // stores: _pending_event_offset = intra_block_offset;

In the Engine's JACK MIDI dispatch loop (``engine.cpp``), the call becomes:

.. code-block:: cpp

    looper->do_event(ev, jack_midi_ev.time);

This change crosses file boundaries (looper.hpp, looper.cpp, engine.cpp)
but is mechanical.  The default parameter means no existing call site
breaks; missed sites degrade gracefully to block-boundary behaviour.

*Risk: Low.*

**Change 2 — Restructure** ``run()`` **to split the** ``run_loops()`` **call.**

The current structure:

.. code-block:: cpp

    if (request_pending) {
        ports[Multi] = requested_cmd;
        request_pending = false;
    }
    run_loops(offset, nframes);

Becomes:

.. code-block:: cpp

    if (request_pending && !resampled && !stretched && !pitched
        && _pending_event_offset > 0)
    {
        nframes_t split = _pending_event_offset;
        // pre-command sub-block: plugin runs in current state
        run_loops(offset, split);
        // apply command exactly at the event boundary
        ports[Multi] = requested_cmd;
        request_pending = false;
        ports[TriggerLatency] = 0;          // exact split; no further correction
        // post-command sub-block: plugin runs in new state
        run_loops(offset + split, nframes - split);
        ports[TriggerLatency] = _buffersize; // restore for next non-split block
        ports[Multi] = -1;
    }
    else {
        if (request_pending) {
            // existing same-command deferral logic unchanged
            if (ports[Multi] == requested_cmd) {
                ports[Multi] = -1;
            } else {
                ports[Multi] = requested_cmd;
                request_pending = false;
            }
        }
        run_loops(offset, nframes);
    }

The buffer pointer arithmetic requires **no new code** — the existing
``offset`` parameter in ``run_loops()`` already advances all per-channel
audio buffers, sync buffers, and JACK port pointers to the correct
position within the block.

*Risk: Low for the buffer arithmetic.  Medium for the same-command deferral
interaction (see below).*

**Change 3 — The same-command deferral interaction.**

The existing deferral:

.. code-block:: cpp

    if (ports[Multi] == requested_cmd) {
        ports[Multi] = -1;   // defer one block
    }

is bypassed in the split path (the command is written directly at the split
point, not at the top of ``run()``).  This means two rapid identical
commands are no longer automatically separated by a ``-1`` transition on
the split path.  The plugin may treat them as a single event.

Options:

- Accept this for the split path.  In practice, two identical commands within
  the same block at a rate that could confuse the plugin is an edge case.
- Before the first sub-call, check and handle the deferral explicitly:
  if ``ports[Multi] == requested_cmd``, run the full block as a deferral
  and skip the split this time.

This is the most conceptually demanding part of the patch.  It is not
complex code — it is a one-condition branch — but it requires understanding
the plugin's state machine expectations well enough to judge the consequence
of skipping the ``-1`` separator.

*Risk: Medium.  Most likely source of a subtle bug in initial testing.*

**Change 4 — Gain ramp state across the split.**

At the top of ``run_loops()``:

.. code-block:: cpp

    float dry_delta = (_target_dry - _curr_dry) / max(1, nframes - 1);

The delta is computed relative to the full ``nframes``.  When called twice
with ``split`` and ``nframes - split`` frames respectively, each call
recomputes its own delta against its own smaller ``nframes``.  The first
call ramps faster than intended; the second call re-derives its delta from
wherever ``_curr_dry`` landed after the first.  The net effect is a slight
kink in the gain ramp profile at the split point.

The snap condition::

    if (dry_delta <= 0.00003f) { _curr_dry = _target_dry; }

may also snap prematurely on the first (short) sub-call.

**Pragmatic fix**: if no ramp is in progress at the moment of the split
(i.e. ``_curr_dry == _target_dry``, same for wet and input gain), there is
no issue and no action is needed.  For active ramps, accept the distortion —
it occurs only at the exact sample of a command trigger, lasts one block,
and is very unlikely to be audible in a live looping context.  Add a comment
noting the known limitation.

*Risk: Low–Medium.  Audibility is low; detectability in testing is low
(requires a gain change happening simultaneously with a command trigger).*

Summary Table
--------------

.. list-table::
   :widths: 32 15 15 38
   :header-rows: 1

   * - Change
     - Lines
     - Risk
     - Notes
   * - ``_pending_event_offset`` field + ``do_event()`` param
     - ~10
     - Low
     - Mostly boilerplate; default param protects call sites
   * - Engine MIDI dispatch: pass ``jack_ev.time``
     - ~5
     - Low
     - Mechanical; requires reading engine.cpp MIDI loop
   * - ``run()`` split logic + ``TriggerLatency`` adjust
     - ~25
     - Low–Medium
     - Buffer arithmetic is free; deferral interaction needs thought
   * - Same-command deferral interaction
     - ~10
     - Medium
     - Most likely source of first-attempt bug
   * - Gain ramp state
     - ~5 (comment + guard)
     - Low–Medium
     - Subtle; accept or guard with a ``_curr == _target`` check
   * - Guard resampled/stretched paths
     - ~5
     - Low
     - One ``if`` condition; leave those paths unchanged
   * - **Total**
     - **~60 new + ~40 restructured**
     - —
     - ~100–150 lines across 3 files

Overall character: **one genuinely tricky design decision** (the deferral
interaction) surrounded by largely mechanical work.  Not a project that
requires deep debugging — more likely one specific issue to track down on
first test, concentrated in the deferral logic or the gain ramp under
simultaneous-gain-and-command conditions.  The existing ``offset``
parameter making buffer arithmetic free is the key piece of good news: what
looks like the hardest part turns out to cost nothing.


Implementation Results
======================

Status
------

The patch was implemented and tested successfully.  SooperLooper now
responds to MIDI commands via a native JACK MIDI input port with no
ALSA bridge in the signal chain.  The ALSA MIDI path through
``MidiBridge`` remains fully operational in parallel for backward
compatibility.

Files Changed
-------------

.. list-table::
   :widths: 30 15 55
   :header-rows: 1

   * - File
     - Change
     - Method
   * - ``audio_driver.hpp``
     - Full replacement
     - Added ``get_jack_client()`` virtual method (default returns
       ``nullptr``); added ``struct _jack_client`` forward declaration
       so the type is known without pulling in ``<jack/jack.h>``
   * - ``jack_audio_driver.hpp``
     - Full replacement
     - Added ``get_jack_client() override`` returning ``_jack``
       (the member confirmed to hold the live JACK client handle)
   * - ``engine.hpp``
     - Full replacement
     - Added ``jack_port_t * _jack_midi_in_port`` private member;
       added ``struct _jack_port`` forward declaration
   * - ``engine.cpp``
     - Unified diff, ``patch -p1``
     - Four hunks: ``#include <jack/jack.h>`` and
       ``<jack/midiport.h>``; constructor null-init;
       ``initialize()`` port registration; ``process()`` MIDI read loop
   * - ``looper.hpp``
     - Full replacement
     - Added ``nframes_t _pending_event_offset`` protected member
   * - ``looper.cpp``
     - Unified diff, ``patch -p1``
     - Three hunks: ``initialize()`` zero; ``do_event()``
       ``FragmentPos()`` capture; ``run()`` ``TriggerLatency``
       scope block

Build Notes
-----------

The initial attempt used ``#include <jack/midiport.h>`` alone in
``engine.cpp``, relying on transitive inclusion of ``<jack/jack.h>``
through ``audio_driver.hpp``.  After ``audio_driver.hpp`` was changed
to use a forward declaration instead of a full include, that transitive
path broke.  The fix was to add ``#include <jack/jack.h>`` explicitly
alongside ``<jack/midiport.h>`` in ``engine.cpp``.

The initial diff files used ``/home/claude/`` absolute paths in their
headers, requiring ``patch -p4`` or manual file specification.  They
were regenerated with ``--label a/src/file --label b/src/file`` so
that ``patch -p1`` from the repository root works cleanly.

JACK Port Naming Under PipeWire
--------------------------------

The system runs PipeWire-JACK (JACK compatibility layer on top of
PipeWire).  When SooperLooper registers ``sooperlooper:midi_in``
(``JackPortIsInput``), PipeWire creates two aliases for the same port:

- ``midi_capture_5`` — PipeWire's alias in its capture namespace
- ``midi_playback_5`` — PipeWire's alias in its playback namespace

Both aliases refer to the same single ``sooperlooper:midi_in`` port.
The number 5 is PipeWire's sequential assignment across all registered
MIDI ports system-wide and has no functional significance.  The port
is confirmed working by connecting ``mididings`` JACK MIDI output →
``sooperlooper:midi_in`` in qjackctl and observing correct command
response.

The command ``jack_lsp -A | grep -A2 sooperlooper`` will show the
canonical ``sooperlooper:midi_in`` name alongside the PipeWire aliases.

Signal Chain After the Patch
------------------------------

.. code-block:: text

    Physical MIDI controller
        → UMC audio interface (hardware MIDI IN)
        → JACK MIDI (PipeWire-JACK, no a2jmidid needed)
        → mididings (jack-rt backend, Process() handler)
        → sooperlooper:midi_in  (new native JACK MIDI port)
        → inject_midi(b0, b1, b2, jack_ev.time)
        → createEvent(jack_ev.time) → FragmentPos() = exact offset
        → Engine::process() splits buffer at FragmentPos()
        → Looper::run() fires command at correct sample,
          TriggerLatency = 0 for that block

a2jmidid is no longer needed or running.  The ALSA MIDI path through
``MidiBridge`` (``_midi_thread`` / ``poll()``) remains active for any
connections made via the ALSA MIDI tab in qjackctl and for backward
compatibility with setups that do not use the JACK MIDI port.

Actual vs Estimated Complexity
--------------------------------

The original estimate of ~100–150 lines was made before ``engine.cpp``
was available for inspection.  Reading the engine revealed that the
sub-block buffer splitting infrastructure already existed in
``Engine::process()`` — it was designed in from the start.  The actual
patch cost was:

.. list-table::
   :widths: 45 15 40
   :header-rows: 1

   * - Component
     - Lines
     - Notes
   * - JACK MIDI port registration (``engine.cpp``)
     - ~20
     - ``initialize()`` block with ``get_jack_client()``
   * - JACK MIDI read loop (``engine.cpp``)
     - ~22
     - ``process()`` block before ``get_read_vector()``
   * - ``get_jack_client()`` virtual + override
     - ~6
     - ``audio_driver.hpp`` + ``jack_audio_driver.hpp``
   * - Forward declarations
     - ~6
     - ``audio_driver.hpp`` + ``engine.hpp``
   * - ``_pending_event_offset`` + ``do_event()``
     - ~8
     - ``looper.hpp`` + ``looper.cpp``
   * - ``TriggerLatency`` scope block in ``run()``
     - ~18
     - ``looper.cpp`` — the only part requiring careful thought
   * - **Total**
     - **~80 lines**
     - Across 6 files; no buffer arithmetic changes needed

The previously-feared same-command deferral interaction and gain ramp
issues did not arise in practice because the engine already calls
``run()`` twice from outside rather than requiring internal
restructuring of ``Looper::run()``.

Possible Future Addition: JACK MIDI Output Port
------------------------------------------------

SooperLooper can optionally send MIDI output: MIDI sync/clock (to
keep external devices in time with the loops) and MIDI feedback to
controllers (e.g. lighting footswitch LEDs).

A JACK MIDI output port has now been added to the code, but not yet
tested.

