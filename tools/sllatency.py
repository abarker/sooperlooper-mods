#!/usr/bin/env python3
"""sllatency -- measure audio round-trip latency, like jack_iodelay, and
(in a later step) check SooperLooper's latency compensation.

Plays its own click out of JACK output ports, records the click as it
comes back on an input (headphones held on a mic, or a loopback cable),
and compares the measured round trip with the latency JACK reports for
those ports -- the values set as QJackctl's input/output latency.

Everything is timed inside one JACK client, so the click times are known
to the sample and nothing depends on a DAW.  See mod_docs/ROADMAP.rst
entry 17 and sooperlooper_and_ardour_setup_guide.rst section 2.7.

Requires JACK, Python 3, numpy and the Python ``jack`` module.
"""
import argparse
import ctypes
import ctypes.util
import sys
import threading
import time

import numpy as np

try:
    import jack
except ImportError:
    sys.exit("sllatency: needs the Python 'jack' module (pip install JACK-Client)")


# ---------------------------------------------------------------- messages

def say(msg=""):
    print(msg, flush=True)


def explain(msg):
    """A short explanation of what the tool is doing and why."""
    for line in msg.strip().splitlines():
        say("   " + line.strip())


# ------------------------------------------------------ JACK port latency

class _Range(ctypes.Structure):
    _fields_ = [("min", ctypes.c_uint32), ("max", ctypes.c_uint32)]


_JACK_CAPTURE_LATENCY = 0
_JACK_PLAYBACK_LATENCY = 1
_libjack = None


def port_latency(port, playback):
    """(min, max) latency in frames JACK reports for a port, or None.

    The Python jack module does not wrap jack_port_get_latency_range(), so
    call it through ctypes on the same libjack, using the port's pointer.
    """
    global _libjack
    try:
        if _libjack is None:
            _libjack = ctypes.CDLL(ctypes.util.find_library("jack") or "libjack.so.0")
            _libjack.jack_port_get_latency_range.argtypes = [
                ctypes.c_void_p, ctypes.c_int, ctypes.POINTER(_Range)]
            _libjack.jack_port_get_latency_range.restype = None
        ptr = int(jack._ffi.cast("uintptr_t", port._ptr))
        r = _Range()
        mode = _JACK_PLAYBACK_LATENCY if playback else _JACK_CAPTURE_LATENCY
        _libjack.jack_port_get_latency_range(ptr, mode, ctypes.byref(r))
        return r.min, r.max
    except Exception:
        return None


# ------------------------------------------------------------------ click

def make_click(sr, level):
    """A 2 ms, 2 kHz tone burst with a smooth window.  A single-sample
    impulse would be cleaner in theory, but headphones and mics reproduce a
    short burst far more reliably."""
    n = int(0.002 * sr)
    t = np.arange(n) / sr
    return (level * np.sin(2 * np.pi * 2000.0 * t) * np.hanning(n)).astype(np.float32)


def build_output(sr, bpm, seconds, click, lead_in=0.5):
    """The whole output signal, and the sample index of each click."""
    total = int((lead_in + seconds + 0.5) * sr)
    out = np.zeros(total, dtype=np.float32)
    period = 60.0 * sr / bpm
    starts = []
    k = 0
    while True:
        s = int(round(lead_in * sr + k * period))
        if s + len(click) >= int((lead_in + seconds) * sr):
            break
        out[s:s + len(click)] += click
        starts.append(s)
        k += 1
    return out, np.array(starts)


# ------------------------------------------------------------- recording

class ClickRecorder:
    """Plays a prepared signal and records input ports, sample-aligned."""

    def __init__(self, client_name, out_targets, in_sources):
        self.client = jack.Client(client_name, no_start_server=True)
        self.sr = self.client.samplerate
        self.out_port = self.client.outports.register("click")
        self.in_ports = [self.client.inports.register("in_%d" % i)
                         for i in range(len(in_sources))]
        self.out_targets = out_targets
        self.in_sources = in_sources
        self.xruns = 0
        self.done = threading.Event()
        self.signal = None
        self.recs = None
        self.pos = 0
        self.client.set_process_callback(self._process)
        self.client.set_xrun_callback(self._xrun)

    def _xrun(self, delay):
        self.xruns += 1

    def _process(self, frames):
        out = self.out_port.get_array()
        if self.signal is None or self.done.is_set():
            out.fill(0.0)
            return
        a = self.pos
        b = min(a + frames, len(self.signal))
        n = b - a
        out[:n] = self.signal[a:b]
        out[n:] = 0.0
        for rec, port in zip(self.recs, self.in_ports):
            rec[a:b] = port.get_array()[:n]
        self.pos = b
        if b >= len(self.signal):
            self.done.set()

    def run(self, signal):
        self.recs = [np.zeros(len(signal), dtype=np.float32) for _ in self.in_ports]
        self.pos = 0
        self.done.clear()
        with self.client:
            for t in self.out_targets:
                self.client.connect(self.out_port, t)
            for p, s in zip(self.in_ports, self.in_sources):
                self.client.connect(s, p)
            time.sleep(0.2)
            self.signal = signal          # start playing on the next cycle
            if not self.done.wait(timeout=len(signal) / self.sr + 10.0):
                raise RuntimeError("JACK stopped processing during the measurement")
        return self.recs


# -------------------------------------------------------------- analysis

def _envelope(x, sr):
    w = max(1, int(0.00025 * sr))          # 0.25 ms smoothing
    return np.convolve(np.abs(x.astype(np.float64)), np.ones(w) / w, mode="same")


def onset(env, lo, hi, frac=0.2):
    """First sample in [lo, hi) where the envelope reaches `frac` of its
    peak in that window, with sub-sample interpolation; None if no clear
    transient (peak not well above the level just before the window)."""
    if hi <= lo:
        return None
    seg = env[lo:hi]
    pk = seg.max()
    floor = np.median(env[max(0, lo - 2000):lo]) if lo > 0 else 0.0
    if pk <= 0 or pk < 4.0 * floor + 1e-6:
        return None
    thr = frac * pk
    i = int(np.argmax(seg > thr))
    if i == 0:
        return float(lo)
    y0, y1 = seg[i - 1], seg[i]
    return lo + i - 1 + (thr - y0) / (y1 - y0)


def measure_round_trip(rec, starts, click, sr, max_latency_ms):
    """Per-click lag (samples) from each click's start to its return."""
    env_c = _envelope(click, sr)
    ref = onset(np.concatenate([np.zeros(2000), env_c]), 2000, 2000 + len(env_c))
    ref -= 2000
    env = _envelope(rec, sr)
    maxlag = int(max_latency_ms / 1000.0 * sr)
    lags = []
    for s in starts:
        o = onset(env, int(s), int(s) + maxlag + len(click))
        if o is not None:
            lags.append(o - (s + ref))
    return np.array(lags)


# ------------------------------------------------------------------ main

def physical_ports(client, is_input):
    """Physical ports; is_input=True gives playback (sink) ports."""
    return [p.name for p in client.get_ports(is_physical=True, is_audio=True,
                                             is_input=is_input)]


def main():
    ap = argparse.ArgumentParser(
        prog="sllatency",
        description="Measure JACK round-trip latency with a click (step 1 of the "
                    "SooperLooper latency tool).")
    ap.add_argument("--out", action="append",
                    help="JACK port to play the click to (repeatable). "
                         "Default: the first two physical playback ports.")
    ap.add_argument("--in", dest="inp",
                    help="JACK port the click comes back on. "
                         "Default: the first physical capture port.")
    ap.add_argument("--bpm", type=float, default=120.0)
    ap.add_argument("--seconds", type=float, default=8.0,
                    help="how long to play the click (default 8)")
    ap.add_argument("--level", type=float, default=0.3,
                    help="click amplitude, 0-1 (default 0.3)")
    ap.add_argument("--max-latency", type=float, default=250.0,
                    help="largest round trip searched, ms (default 250)")
    ap.add_argument("--tolerance", type=float, default=1.0,
                    help="difference treated as nothing to fix, ms (default 1)")
    ap.add_argument("--yes", action="store_true",
                    help="do not wait for Enter before playing the click")
    args = ap.parse_args()

    say("sllatency: audio round-trip measurement")
    probe = jack.Client("sllatency_probe", no_start_server=True)
    sr = probe.samplerate
    outs = args.out or physical_ports(probe, is_input=True)[:2]
    inp = args.inp or (physical_ports(probe, is_input=False) or [None])[0]
    if not outs or not inp:
        sys.exit("sllatency: could not find ports; give --out and --in")
    lat_out = port_latency(probe.get_port_by_name(outs[0]), playback=True)
    lat_in = port_latency(probe.get_port_by_name(inp), playback=False)
    blocksize = probe.blocksize
    probe.close()

    say()
    say("Setup")
    explain("""
        The click goes out of: %s
        and should come back on: %s
        Hold the headphones fed by those outputs against the mic on that
        input, or connect a cable from the output to the input.  Start
        quietly: the click level is %.2f of full scale.
    """ % (", ".join(outs), inp, args.level))
    if not args.yes:
        input("   Press Enter when ready... ")

    click = make_click(sr, args.level)
    signal, starts = build_output(sr, args.bpm, args.seconds, click)
    say()
    say("Measuring")
    explain("""
        Playing %d clicks at %g BPM over %.0f s and recording the input in
        the same JACK cycles, so each click's return can be timed to the
        sample.
    """ % (len(starts), args.bpm, args.seconds))
    rec = ClickRecorder("sllatency", outs, [inp])
    (inrec,) = rec.run(signal)

    lags = measure_round_trip(inrec, starts, click, sr, args.max_latency)
    say()
    say("Result")
    if rec.xruns:
        say("   WARNING: %d xrun(s) during the run -- the result may be wrong; "
            "run it again." % rec.xruns)
    if len(lags) < max(3, len(starts) // 2):
        say("   Only %d of %d clicks came back clearly.  Raise --level, move the "
            "headphones closer to the mic, or check the connections." % (len(lags), len(starts)))
        if not len(lags):
            return 1
    med = float(np.median(lags))
    spread = float(np.percentile(lags, 75) - np.percentile(lags, 25))
    ms = lambda f: f * 1000.0 / sr
    say("   Measured round trip: %.1f samples (%.2f ms) from %d clicks, spread %.1f samples."
        % (med, ms(med), len(lags), spread))
    if lat_out and lat_in:
        reported = lat_out[1] + lat_in[1]
        diff = med - reported
        explain("""
            JACK reports %d samples of playback latency on %s and %d of
            capture latency on %s: %d in total (buffer size %d).  These
            include QJackctl's extra output/input latency settings.
        """ % (lat_out[1], outs[0], lat_in[1], inp, reported, blocksize))
        if abs(ms(diff)) <= args.tolerance:
            say("   Difference %+.1f samples (%+.2f ms): within %.1f ms -- nothing to change."
                % (diff, ms(diff), args.tolerance))
        else:
            say("   Difference %+.1f samples (%+.2f ms): JACK's latency settings are %s."
                % (diff, ms(diff), "too low" if diff > 0 else "too high"))
            explain("""
                Change QJackctl's input latency plus output latency by %+d
                samples in total (for example %+d on each), then restart JACK.
                A loopback measures only the sum; how it is split does not
                matter for recording alignment.
            """ % (int(round(diff)), int(round(diff / 2))))
    else:
        say("   Could not read JACK's reported latencies for these ports.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
