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
import os
import re
import shutil
import socket
import subprocess
import struct
import sys
import textwrap
import threading
import time

import numpy as np

try:
    import jack
except ImportError:
    sys.exit("sllatency: needs the Python 'jack' module (pip install JACK-Client)")


# ---------------------------------------------------------------- messages

# Narrow enough to read comfortably even in a wide terminal.
WIDTH = max(60, min(76, shutil.get_terminal_size((80, 24)).columns - 1))


def say(msg=""):
    """Print a line, wrapped to the terminal; continuation lines keep the
    line's own indent."""
    if len(msg) <= WIDTH:
        print(msg, flush=True)
        return
    indent = " " * (len(msg) - len(msg.lstrip()))
    print(textwrap.fill(msg.strip(), WIDTH, initial_indent=indent,
                        subsequent_indent=indent), flush=True)


def explain(msg):
    """A short explanation, reflowed to the terminal width.  Blank lines in
    the text separate paragraphs."""
    paras = textwrap.dedent(msg).strip().split("\n\n")
    for i, para in enumerate(paras):
        if i:
            print()
        print(textwrap.fill(" ".join(para.split()), WIDTH,
                            initial_indent="   ", subsequent_indent="   "),
              flush=True)


def heading(title):
    print()
    print(title)
    print("-" * len(title), flush=True)


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

    def __init__(self, client_name, out_targets, in_sources, probe_targets=()):
        self.client = jack.Client(client_name, no_start_server=True)
        self.sr = self.client.samplerate
        self.out_port = self.client.outports.register("click")
        # A second output for the monitoring-path check: it stands in for
        # SooperLooper's loop 0 output, feeding the same destinations.
        self.probe_targets = list(probe_targets)
        self.probe_port = (self.client.outports.register("probe")
                           if self.probe_targets else None)
        self.probe_signal = None
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
            if self.probe_port is not None:
                self.probe_port.get_array().fill(0.0)
            return
        a = self.pos
        b = min(a + frames, len(self.signal))
        n = b - a
        out[:n] = self.signal[a:b]
        out[n:] = 0.0
        if self.probe_port is not None:
            pout = self.probe_port.get_array()
            if self.probe_signal is not None:
                pout[:n] = self.probe_signal[a:b]
                pout[n:] = 0.0
            else:
                pout.fill(0.0)
        for rec, port in zip(self.recs, self.in_ports):
            rec[a:b] = port.get_array()[:n]
        self.pos = b
        if b >= len(self.signal):
            self.done.set()

    def run(self, signal, actions=(), extra_connections=(), probe_signal=None):
        """Play `signal`, recording the inputs.  `actions` is a list of
        (seconds after the start, callable) run from this thread while
        playing -- used to press SooperLooper's record button.
        `extra_connections` are (source, destination) port pairs connected
        for the run, e.g. the input into a temporary SooperLooper loop."""
        self.recs = [np.zeros(len(signal), dtype=np.float32) for _ in self.in_ports]
        self.pos = 0
        self.probe_signal = probe_signal
        self.done.clear()
        with self.client:
            for t in self.out_targets:
                self.client.connect(self.out_port, t)
            for t in self.probe_targets:
                self.client.connect(self.probe_port, t)
            for p, s in zip(self.in_ports, self.in_sources):
                self.client.connect(s, p)
            for src, dst in extra_connections:
                self.client.connect(src, dst)
            time.sleep(0.2)
            t0 = time.monotonic()
            self.signal = signal          # start playing on the next cycle
            for at, fn in sorted(actions, key=lambda a: a[0]):
                delay = t0 + at - time.monotonic()
                if delay > 0:
                    time.sleep(delay)
                fn()
            if not self.done.wait(timeout=len(signal) / self.sr + 10.0):
                raise RuntimeError("JACK stopped processing during the measurement")
            for src, dst in extra_connections:
                try:
                    self.client.disconnect(src, dst)
                except jack.JackError:
                    pass
        return self.recs


# ------------------------------------------------------------------- OSC

def _osc_pad(b):
    return b + b"\x00" * ((4 - len(b) % 4) % 4)


def _osc_str(s):
    return _osc_pad(s.encode() + b"\x00")


def osc_message(address, *args):
    tags = ","
    body = b""
    for a in args:
        if isinstance(a, str):
            tags += "s"; body += _osc_str(a)
        elif isinstance(a, int):
            tags += "i"; body += struct.pack(">i", a)
        else:
            tags += "f"; body += struct.pack(">f", float(a))
    return _osc_str(address) + _osc_str(tags) + body


def osc_parse(packet):
    def rstr(off):
        end = packet.index(b"\x00", off)
        return packet[off:end].decode(errors="replace"), (end + 4) & ~3
    addr, off = rstr(0)
    tags, off = rstr(off)
    args = []
    for t in tags[1:]:
        if t == "s":
            v, off = rstr(off)
        elif t == "i":
            v = struct.unpack(">i", packet[off:off + 4])[0]; off += 4
        elif t == "f":
            v = struct.unpack(">f", packet[off:off + 4])[0]; off += 4
        else:
            break
        args.append(v)
    return addr, args


class SooperLooper:
    """Just enough of SooperLooper's OSC interface for this tool."""

    def __init__(self, host="127.0.0.1", port=9951):
        self.addr = (host, port)
        self.rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.rx.bind(("127.0.0.1", 0))
        self.url = "osc.udp://127.0.0.1:%d/" % self.rx.getsockname()[1]
        self.tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    def send(self, address, *args):
        self.tx.sendto(osc_message(address, *args), self.addr)

    def _ask(self, address, *args, reply="/sllatency_reply", timeout=1.5):
        self.rx.settimeout(0.05)
        try:                                    # drop stale replies
            while True:
                self.rx.recvfrom(4096)
        except socket.timeout:
            pass
        self.send(address, *(list(args) + [self.url, reply]))
        self.rx.settimeout(timeout)
        try:
            while True:
                pkt, _ = self.rx.recvfrom(4096)
                a, vals = osc_parse(pkt)
                if a == reply:
                    return vals
        except socket.timeout:
            return None

    def ping(self):
        """Loop count, or None if SooperLooper does not answer."""
        r = self._ask("/ping")
        return r[2] if r and len(r) >= 3 else None

    def get(self, param, loop=None):
        """A global (loop None) or per-loop control value, or None."""
        r = (self._ask("/get", param) if loop is None
             else self._ask("/sl/%d/get" % loop, param))
        return r[2] if r and len(r) >= 3 else None

    def set(self, param, value, loop=None):
        if loop is None:
            self.send("/set", param, float(value))
        else:
            self.send("/sl/%d/set" % loop, param, float(value))

    def hit(self, command, loop):
        self.send("/sl/%d/hit" % loop, command)

    def loop_add(self, channels, seconds):
        self.send("/loop_add", int(channels), float(seconds))

    def loop_del(self, index):
        self.send("/loop_del", int(index))

    def wait_loop_count(self, n, timeout=5.0):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if self.ping() == n:
                return True
            time.sleep(0.1)
        return False


# -------------------------------------------------------------- analysis

def _envelope(x, sr):
    w = max(1, int(0.00025 * sr))          # 0.25 ms smoothing
    return np.convolve(np.abs(x.astype(np.float64)), np.ones(w) / w, mode="same")


def onset(env, lo, hi, frac=0.2):
    """First sample in [lo, hi) where the envelope reaches `frac` of its
    peak in that window, with sub-sample interpolation; None if no clear
    transient (peak not well above the level just before the window, or
    the level already over the threshold where the window starts -- which
    is what a window of noise alone looks like)."""
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
        return None
    y0, y1 = seg[i - 1], seg[i]
    return lo + i - 1 + (thr - y0) / (y1 - y0)


def _click_ref(click, sr):
    """Where the onset rule places the start of the clean click itself, so
    returned clicks are compared like with like."""
    env_c = _envelope(click, sr)
    return onset(np.concatenate([np.zeros(2000), env_c]), 2000, 2000 + len(env_c)) - 2000


def measure_round_trip(rec, starts, click, sr, max_latency_ms):
    """Per-click lag (samples) from each click's start to its return."""
    ref = _click_ref(click, sr)
    env = _envelope(rec, sr)
    maxlag = int(max_latency_ms / 1000.0 * sr)
    lags = []
    for s in starts:
        o = onset(env, int(s), int(s) + maxlag + len(click))
        if o is not None:
            lags.append(o - (s + ref))
    return np.array(lags)


def second_returns(rec, starts, lags, click, sr, max_latency_ms,
                   min_gap_ms=2.0, min_ratio=0.3):
    """For each click with a measured first return (start + lag), look for a
    second, separate return at least `min_gap_ms` later that is at least
    `min_ratio` as strong -- the same sound arriving by a second route.
    Returns the second lags found (samples)."""
    ref = _click_ref(click, sr)
    env = _envelope(rec, sr)
    gap = int(min_gap_ms / 1000.0 * sr)
    maxlag = int(max_latency_ms / 1000.0 * sr)
    found = []
    for s, lag in zip(starts, lags):
        o1 = s + ref + lag
        lo1 = int(o1)
        first_pk = env[lo1:lo1 + len(click) + gap].max() if lo1 < len(env) else 0.0
        lo = lo1 + len(click) + gap
        hi = int(s) + maxlag + len(click)
        if hi <= lo or first_pk <= 0:
            continue
        seg = env[lo:hi]
        if seg.max() < min_ratio * first_pk:
            continue
        o2 = onset(env, lo, hi)
        if o2 is not None:
            found.append(o2 - (s + ref))
    return np.array(found)


def measure_loop_offset(rec, starts, click, sr, period):
    """Per-click offset (samples) of the clicks in SooperLooper's loop
    playback from the click schedule.  The loop is a whole number of beats
    long, so with correct latency compensation its recorded clicks play
    back exactly on the schedule; any offset is the compensation error.
    Searches within 45% of a beat either side, so each window holds one
    click; before the loop plays the output is silent and yields none."""
    ref = _click_ref(click, sr)
    env = _envelope(rec, sr)
    half = int(0.45 * period)
    offs = []
    for s in starts:
        lo = max(0, int(s) - half)
        o = onset(env, lo, int(s) + half)
        if o is not None:
            offs.append(o - (s + ref))
    return np.array(offs)


# ------------------------------------------------------------------ main

def physical_ports(client, is_input):
    """Physical ports; is_input=True gives playback (sink) ports."""
    return [p.name for p in client.get_ports(is_physical=True, is_audio=True,
                                             is_input=is_input)]


def default_input(client, sl_client):
    """The capture port feeding SooperLooper's first loop, if any (else its
    main input): that is the input a performance actually goes through."""
    for name in ("loop0_in_1", "common_in_1"):
        try:
            conns = client.get_all_connections("%s:%s" % (sl_client, name))
            phys = [c.name for c in conns if c.is_physical]
            if phys:
                return phys[0]
        except jack.JackError:
            pass
    caps = physical_ports(client, is_input=False)
    return caps[0] if caps else None


def _connected(client, prefix):
    """Every port the ports named prefix* are connected to, in order."""
    found = []
    for port in client.get_ports(prefix, is_audio=True):
        try:
            for c in client.get_all_connections(port):
                if c.name not in found:
                    found.append(c.name)
        except jack.JackError:
            pass
    return found


def input_candidates(client, sl_client, any_port=False):
    """Every capture port feeding SooperLooper's loop 0 (else its main
    inputs), in port order: a stereo loop is usually fed from two, and
    which one the headphones are nearest is not always known."""
    for base in ("loop0_in_", "common_in_"):
        found = []
        for port in client.get_ports("%s:%s" % (sl_client, base), is_audio=True):
            try:
                conns = client.get_all_connections(port)
            except jack.JackError:
                continue
            for c in conns:
                if (c.is_physical or any_port) and c.name not in found:
                    found.append(c.name)
        if found:
            return found
    return []


def strongest_input(outs, candidates, sr, bpm, level, max_latency_ms):
    """Play a few clicks and record every candidate input; return the one
    with the most clicks back, then the loudest, and each one's
    (clicks back, peak level)."""
    click = make_click(sr, level)
    signal, starts = build_output(sr, bpm, 2.5, click)
    rec = ClickRecorder("sllatency", outs, candidates)
    recs = rec.run(signal)
    heard = {}
    for port, r in zip(candidates, recs):
        lags = measure_round_trip(r, starts, click, sr, max_latency_ms)
        heard[port] = (len(lags), float(np.abs(r).max()) if len(r) else 0.0)
    best = max(candidates, key=lambda p: heard[p])
    return best, heard, len(starts)


class SLRun:
    """Reads SooperLooper's settings, sets up a temporary loop for the
    measurement, and restores everything afterwards."""

    GLOBALS = ("sync_source", "tempo", "eighth_per_cycle", "auto_disable_latency")
    LATENCY = ("autoset_latency", "input_latency", "output_latency", "trigger_latency")

    def __init__(self, sl, sl_client, bpm):
        self.sl = sl
        self.client = sl_client
        self.bpm = bpm
        self.loops = sl.ping()
        self.saved = {g: sl.get(g) for g in self.GLOBALS}
        self.loop0 = ({k: sl.get(k, loop=0) for k in self.LATENCY}
                      if self.loops else {})
        # Autoset reads the common ports' latency instead of the loop's own
        # when these are on (Looper::recompute_latencies()).
        self.loop0_common = ({k: sl.get(k, loop=0)
                              for k in ("use_common_ins", "use_common_outs")}
                             if self.loops else {})
        self.common_dry = sl.get("dry", loop=-2)
        # SooperLooper spaces its sync pulses by loop 0's quantize setting
        # (Engine::calculate_tempo_frames() reads the first loop), not the
        # recording loop's; with loop 0 unquantized there are no pulses and
        # the temporary loop would record unquantized.
        self.loop0_quantize = sl.get("quantize", loop=0) if self.loops else None
        self.index = None

    def describe(self):
        l0 = self.loop0
        if not l0:
            explain("SooperLooper has no loops yet; the temporary loop uses its "
                    "default settings.")
            return
        auto = l0.get("autoset_latency")
        explain("""
            SooperLooper's current latency settings (loop 1): input %s, output
            %s, trigger %s samples, %s.
        """ % (_n(l0.get("input_latency")), _n(l0.get("output_latency")),
               _n(l0.get("trigger_latency")),
               "chosen automatically (autoset on)" if auto else "set by hand (autoset off)"))
        print()
        explain("""
            Input and output latency are what this tool checks.  Trigger latency
            is separate: it is how late SooperLooper takes a command to arrive
            after the press, and it only changes where overdub, multiply,
            insert, replace and substitute start taking input.  Autoset sets one buffer.  This
            tool leaves it as it is.
        """)
        if self.saved.get("auto_disable_latency") and (self.common_dry or 0) > 0:
            print()
            explain("""
                WARNING: auto_disable_latency is on and SooperLooper's input
                monitor (common dry) is up, so its latency compensation is
                switched OFF.
            """)

    def set_up(self):
        """Add the temporary loop and configure it and the tempo."""
        sl = self.sl
        sl.loop_add(1, 30.0)
        if not sl.wait_loop_count(self.loops + 1):
            raise RuntimeError("SooperLooper did not add the temporary loop")
        self.index = i = self.loops
        # A loop that behaves like loop 0 for latency, outputs only itself,
        # and records in whole cycles of one beat, synced to its own tempo.
        # It uses loop 0's current values (whether set by hand or by autoset)
        # with autoset off: its own ports go only to this tool, so autoset
        # would come out differently for it.
        sl.set("autoset_latency", 0.0, loop=i)
        if self.loop0:
            for k in ("input_latency", "output_latency", "trigger_latency"):
                sl.set(k, self.loop0[k], loop=i)
        # Only its own ports, which this tool connects: no input from the
        # main inputs, and no output to the main outputs (which may reach
        # the speakers, and the mic).
        for k, v in (("dry", 0.0), ("wet", 1.0), ("sync", 1.0), ("quantize", 1.0),
                     ("use_common_ins", 0.0), ("use_common_outs", 0.0)):
            sl.set(k, v, loop=i)
        if self.loop0_quantize is not None and self.loop0_quantize != 1.0:
            sl.set("quantize", 1.0, loop=0)  # cycle; see __init__
        sl.set("eighth_per_cycle", 2.0)
        sl.set("tempo", self.bpm)
        sl.set("sync_source", -3.0)          # SooperLooper's internal tempo
        time.sleep(0.3)

    def restore(self):
        sl = self.sl
        if self.index is not None:
            sl.loop_del(self.index)
            sl.wait_loop_count(self.loops)
        for g in ("sync_source", "tempo", "eighth_per_cycle"):
            if self.saved.get(g) is not None:
                sl.set(g, self.saved[g])
        if self.loop0_quantize is not None and self.loop0_quantize != 1.0:
            sl.set("quantize", self.loop0_quantize, loop=0)

    def port(self, direction):
        return "%s:loop%d_%s_1" % (self.client, self.index, direction)


def _n(v):
    return "?" if v is None else "%g" % v


# ------------------------------------------------------- choosing outputs

def output_feeds(client):
    """{physical playback port: [ports feeding it]}, in port order."""
    return {p.name: [q.name for q in client.get_all_connections(p)]
            for p in client.get_ports(is_physical=True, is_audio=True, is_input=True)}


def suggest_outputs(feeds):
    """Guess the outputs the performer hears: those a DAW's master bus
    feeds, else the (up to two) most-fed outputs, else the first two."""
    names = list(feeds)
    master = [n for n in names if any("master" in f.lower() for f in feeds[n])]
    if master:
        return master
    busy = [n for n in sorted(names, key=lambda n: -len(feeds[n])) if feeds[n]][:2]
    return sorted(busy, key=names.index) if busy else names[:2]


def sl_heard_outputs(feeds, sl_dests):
    """The physical outputs SooperLooper's loop is heard through: those its
    output feeds directly, or through a DAW bus -- a destination named
    ".../audio_in N" (Ardour's naming) counts as reaching whatever that
    bus's ".../audio_out" ports feed."""
    reach = set(sl_dests)
    for d in sl_dests:
        if "/audio_in" in d:
            reach.add(d.split("/audio_in")[0] + "/audio_out")
    def heard(feeders):
        return any(f in reach or f.rsplit(" ", 1)[0] in reach for f in feeders)
    return [n for n in feeds if n in reach or heard(feeds[n])]


def choose_outputs(client, interactive, sl_dests=()):
    feeds = output_feeds(client)
    names = list(feeds)
    sl_heard = sl_heard_outputs(feeds, sl_dests) if sl_dests else []
    guess = sl_heard or suggest_outputs(feeds)
    if sl_heard:
        say("   Physical outputs, and what feeds each (* = the ones SooperLooper's "
            "loop 1 is heard through):")
    elif sl_dests:
        say("   Physical outputs, and what feeds each (* = suggested: where "
            "SooperLooper's loop 1 is heard could not be traced through JACK, so "
            "these are a guess -- what a DAW's master feeds, else the busiest):")
    else:
        say("   Physical outputs, and what feeds each (* = suggested for the click):")
    print()
    for i, n in enumerate(names, 1):
        say("   %s %d  %s" % ("*" if n in guess else " ", i, n))
        f = feeds[n]
        print(textwrap.fill(", ".join(f) if f else "(nothing)", WIDTH,
                            initial_indent="          <- " if f else "          ",
                            subsequent_indent="             "), flush=True)
    if not interactive:
        return guess
    print()
    explain("""
        The click is played on these outputs and picked up by the mic, so
        choose outputs the headphones or speaker by the mic play%s.  Press
        Enter for the * outputs, or type numbers (for example: 3 4).
    """ % (" -- to check SooperLooper, the ones it is heard through"
           if sl_dests else ""))
    print()
    ans = input("   Outputs: ").strip()
    print()
    picked = [names[int(t) - 1] for t in ans.replace(",", " ").split()
              if t.isdigit() and 1 <= int(t) <= len(names)]
    return picked or guess


def ask(question, default=False):
    """A yes/no question, wrapped to the terminal; the answer goes after
    its last line."""
    text = textwrap.fill(question + " [%s]" % ("Y/n" if default else "y/N"),
                         WIDTH - 2, initial_indent="   ", subsequent_indent="   ")
    lines = text.split("\n")
    for line in lines[:-1]:
        print(line)
    ans = input(lines[-1] + " ").strip().lower()
    return default if not ans else ans.startswith("y")


# ------------------------------------------------------- JACK's settings

def jack_settings():
    """JACK's buffer settings and QJackctl's extra latencies:
    {period, nperiods, in_extra, out_extra}.  Read through jackdbus's own
    interface (``jack_control dp``) -- only when jackdbus is the server
    running, checked by asking D-Bus whether the name is owned, which
    starts nothing.  Empty when not available (plain jackd, PipeWire's
    JACK, another OS); the report then speaks in totals."""
    try:
        owned = subprocess.run(
            ["dbus-send", "--session", "--print-reply",
             "--dest=org.freedesktop.DBus", "/org/freedesktop/DBus",
             "org.freedesktop.DBus.NameHasOwner", "string:org.jackaudio.service"],
            capture_output=True, text=True, timeout=5).stdout
        if "boolean true" not in owned:
            return {}
        txt = subprocess.run(["jack_control", "dp"], capture_output=True,
                             text=True, timeout=5).stdout
    except (OSError, subprocess.SubprocessError):
        return {}
    keys = {"period": "period", "nperiods": "nperiods",
            "input-latency": "in_extra", "output-latency": "out_extra"}
    out = {}
    for line in txt.splitlines():
        k = keys.get(line.split(":", 1)[0].strip())
        m = re.search(r":(\d+)\)\s*$", line)
        if k and m:
            out[k] = int(m.group(1))
    return out


# ---------------------------------------------------------- session file

CONFIG = os.path.join(os.path.expanduser("~"), ".sooperlooper", "sllatency.conf")


def remembered_session_file():
    try:
        with open(CONFIG) as f:
            for line in f:
                k, _, v = line.partition("=")
                if k.strip() == "session_file" and v.strip():
                    return v.strip()
    except OSError:
        pass
    return None


def remember_session_file(path):
    try:
        os.makedirs(os.path.dirname(CONFIG), exist_ok=True)
        with open(CONFIG, "w") as f:
            f.write("# written by sllatency\nsession_file = %s\n" % path)
    except OSError:
        pass


def save_latency_to_session(path, il, ol):
    """Change only the latency values in a SooperLooper session file (and
    auto_latency to "no", so they are used), keeping a backup copy.
    Everything else in the file stays byte-for-byte as it was, unlike
    SooperLooper's /save_session, which writes the whole live state."""
    with open(path) as f:
        text = f.read()
    text, n_in = re.subn(r'(<Control name="input_latency" value=")[^"]*(")',
                         r"\g<1>%d\g<2>" % il, text)
    text, n_out = re.subn(r'(<Control name="output_latency" value=")[^"]*(")',
                          r"\g<1>%d\g<2>" % ol, text)
    text, _ = re.subn(r'(\bauto_latency=")[^"]*(")', r"\g<1>no\g<2>", text)
    if not (n_in and n_out):
        raise RuntimeError("no latency values found in %s" % path)
    backup = path + time.strftime(".bak-%Y%m%d-%H%M%S")
    shutil.copy2(path, backup)
    tmp = path + ".sllatency-tmp"
    with open(tmp, "w") as f:
        f.write(text)
    os.replace(tmp, path)
    return backup, n_in


def split_total(total, base_in, base_out):
    """Split a total latency like base_in/base_out (JACK's own split),
    sharing the difference evenly; the two parts always sum to total."""
    r = total - (base_in + base_out)
    return base_in + r // 2, base_out + (r - r // 2)


# SooperLooper states in which loop 0 makes no sound (plugin.cc STATE_*):
# off, muted, paused, off-muted.
SILENT_STATES = (0.0, 10.0, 14.0, 20.0)


def loop0_destinations(sl_client, include_common):
    """The ports SooperLooper's loop 0 output feeds (and its main outputs,
    when loop 0 uses them): where its sound goes on its way to be heard."""
    c = jack.Client("sllatency_dest", no_start_server=True)
    try:
        pats = ["^%s:loop0_out_" % re.escape(sl_client)]
        if include_common:
            pats.append("^%s:common_out_" % re.escape(sl_client))
        dests = []
        for pat in pats:
            for port in c.get_ports(pat, is_audio=True, is_output=True):
                for d in c.get_all_connections(port):
                    if d.name not in dests:
                        dests.append(d.name)
        return dests
    finally:
        c.close()


def sl_paths(sl_ports):
    """JACK's latency along the path SooperLooper's audio actually takes: an
    unconnected port reports 0, so take the larger of the loop's own and the
    common port."""
    def path(a, b):
        vals = [v for v in (sl_ports.get(a), sl_ports.get(b)) if v is not None]
        return max(vals) if vals else None
    return path("loop0_in_1", "common_in_1"), path("loop0_out_1", "common_out_1")


def quick_report(args, interactive, sl_ports, sr):
    """--reported: suggest values from reported latencies only."""
    ms = lambda f: f * 1000.0 / sr
    heading("Reported latencies (nothing measured)")
    sl = SooperLooper(args.sl_host, args.sl_port)
    if sl.ping() is None:
        say("   SooperLooper did not answer on port %d." % args.sl_port)
        return 1
    sl_run = SLRun(sl, args.sl_client, args.bpm)
    l0 = sl_run.loop0
    if not l0:
        say("   SooperLooper has no loops, so there is nothing to set.")
        return 1
    jack_in, jack_out = sl_paths(sl_ports)
    if not jack_in or not jack_out:
        explain("""
            SooperLooper's loop 1 input or output is not connected to anything
            JACK can follow to the hardware, so there are no reported
            latencies to use.
        """)
        return 1
    sl_run.describe()
    print()
    explain("""
        JACK reports %d samples (%.2f ms) from the audio input to
        SooperLooper's input, and %d (%.2f ms) from SooperLooper's output to
        the speakers, including any delay a DAW reports for processing after
        SooperLooper.  Nothing was measured: these are only as accurate as
        QJackctl's latency settings and the DAW's reports.  Run without
        --reported, with the headphones on the mic, to measure instead.
    """ % (jack_in, ms(jack_in), jack_out, ms(jack_out)))
    il, ol = int(l0["input_latency"]), int(l0["output_latency"])
    print()
    row = "   %-34s %7s %7s %7s"
    say(row % ("SooperLooper latency (samples)", "input", "output", "total"))
    say(row % ("current (%s):" % ("autoset" if l0.get("autoset_latency") else "manual"),
               il, ol, il + ol))
    say(row % ("suggested (JACK's reports):", jack_in, jack_out, jack_in + jack_out))
    within = abs(ms((jack_in + jack_out) - (il + ol))) <= args.tolerance
    if within:
        print()
        say("   Within %.1f ms of the current values." % args.tolerance)
    return offer_apply_and_save(sl, args, interactive, il, ol, jack_in, jack_out,
                                within, what="suggested")


def offer_apply_and_save(sl, args, interactive, il, ol, new_in, new_out, within,
                         what="measured best"):
    """Ask (or follow --apply/--save) to apply new values to all loops and
    save them in the session file."""
    if (new_in, new_out) == (il, ol):
        return 0
    if interactive:
        print()
        apply_it = ask("Apply the %s values to SooperLooper now (all "
                       "loops, autoset off)?" % what, default=not within)
    else:
        apply_it = args.apply
    if not apply_it:
        return 0
    sl.set("autoset_latency", 0.0, loop=-1)
    sl.set("input_latency", new_in, loop=-1)
    sl.set("output_latency", new_out, loop=-1)
    time.sleep(0.3)
    got = (sl.get("input_latency", loop=0), sl.get("output_latency", loop=0))
    if got != (float(new_in), float(new_out)):
        say("   SooperLooper reports %s after applying -- not what was set." % (got,))
        return 1
    print()
    say("   Applied: input_latency %d, output_latency %d on all loops." % (new_in, new_out))

    print()
    explain("""
        These last until SooperLooper restarts.  To keep them, the tool can
        change only the latency values in the session file SooperLooper loads
        at startup (a backup copy is kept); the rest of the file stays exactly
        as it is.
    """)
    path = args.session_file
    if interactive:
        if not ask("Save them in the session file?"):
            return 0
        if not path:
            known = remembered_session_file()
            ans = input("   Session file%s: " % (" [%s]" % known if known else "")).strip()
            path = os.path.expanduser(ans) if ans else known
        save_it = bool(path)
    else:
        path = path or remembered_session_file()
        save_it = args.save and bool(path)
        if args.save and not path:
            say("   --save needs --session-file (or one remembered from an "
                "earlier run).")
    if save_it:
        if not os.path.isfile(path):
            say("   %s does not exist; nothing saved." % path)
            return 1
        remember_session_file(path)
        backup, n = save_latency_to_session(path, new_in, new_out)
        say("   Saved in %s (%d loop%s); the old file is %s."
            % (path, n, "" if n == 1 else "s", backup))
    return 0



def main():
    ap = argparse.ArgumentParser(
        prog="sllatency",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description="Measure JACK's audio round-trip latency with a click, and\n"
                    "check SooperLooper's latency compensation.",
        epilog="""how to use:
  Start JACK and SooperLooper (with its usual session), hold the headphones
  against the mic SooperLooper records from (or the mic right against a
  speaker, or a cable from an output to that input) and run sllatency.  It
  asks which outputs the headphones or speaker play, plays a click for about 15 s, records it with a
  temporary SooperLooper loop (removed afterwards, settings restored), and
  then plays it for a few beats into wherever loop 1's output goes, to
  measure the path you hear SooperLooper through (DAW buses, plugins).  It
  reports whether JACK's and SooperLooper's latency settings are right and
  offers to apply and save new SooperLooper values.  Best run right after
  starting SooperLooper, with loop 1 silent; don't touch the pedals or
  record in a DAW during the run.

  For a quick starting point with no mic and no clicks, --reported suggests
  values from the latencies JACK (and a DAW) report instead.

examples:
  sllatency                                   ask as it goes
  sllatency --session-file ~/sl/my.slsess     also offer to save there
  sllatency --reported                        quick values, nothing measured
  sllatency --no-sl --in system:capture_2     round trip only
  sllatency --no-sl --out "ardour:BUS/audio_in 1" --in "ardour:BUS/audio_out 1"
                                              delay through a DAW bus (no mic)
  sllatency --yes --out system:playback_1     no questions, report only""")
    ap.add_argument("--out", action="append", metavar="PORT",
                    help="JACK port to play the click to (repeatable).  Default: "
                         "ask, suggesting the outputs a DAW's master feeds.")
    ap.add_argument("--in", dest="inp", metavar="PORT",
                    help="JACK port the click comes back on.  Default: the "
                         "capture port feeding SooperLooper's loop 1 -- if "
                         "several do (a stereo loop), whichever hears a few "
                         "test clicks best -- else the first physical capture "
                         "port.")
    ap.add_argument("--bpm", type=float, default=120.0, metavar="N",
                    help="click tempo (default 120)")
    ap.add_argument("--level", type=float, default=0.3, metavar="X",
                    help="click amplitude, 0-1 (default 0.3)")
    ap.add_argument("--max-latency", type=float, default=250.0, metavar="MS",
                    help="largest round trip searched, ms (default 250)")
    ap.add_argument("--tolerance", type=float, default=1.0, metavar="MS",
                    help="difference treated as nothing to fix, ms (default 1)")
    ap.add_argument("--sl-host", default="127.0.0.1", metavar="HOST",
                    help="SooperLooper's OSC host (default 127.0.0.1)")
    ap.add_argument("--sl-port", type=int, default=9951, metavar="N",
                    help="SooperLooper's OSC port (default 9951)")
    ap.add_argument("--sl-client", default="sooperlooper", metavar="NAME",
                    help="SooperLooper's JACK client name (default sooperlooper)")
    ap.add_argument("--session-file", metavar="FILE",
                    help="the SooperLooper session file loaded at startup, to "
                         "save new values in.  Remembered for later runs.")
    # Tests stand in for the audio interface with an ordinary JACK client.
    ap.add_argument("--as-hardware", action="store_true", help=argparse.SUPPRESS)
    # Debugging: save the recordings and click times to a .npz file.
    ap.add_argument("--dump", metavar="FILE", help=argparse.SUPPRESS)
    ap.add_argument("--reported", action="store_true",
                    help="no clicks and no mic: suggest SooperLooper values from "
                         "the latencies JACK (and a DAW) report -- a quick "
                         "starting point, not a measurement")
    ap.add_argument("--no-monitor-check", action="store_true",
                    help="skip measuring the path SooperLooper's output is "
                         "heard through; use the latency JACK reports for it")
    ap.add_argument("--no-sl", action="store_true",
                    help="measure the round trip only; leave SooperLooper alone")
    ap.add_argument("--yes", action="store_true",
                    help="no questions: use the suggested outputs, and apply or "
                         "save only if --apply / --save are given")
    ap.add_argument("--apply", action="store_true",
                    help="with --yes: apply the recommended SooperLooper values")
    ap.add_argument("--save", action="store_true",
                    help="with --yes and --apply: also save them in the session file")
    args = ap.parse_args()
    interactive = not args.yes

    say("sllatency: audio round-trip and SooperLooper latency check")
    heading("Setup")
    probe = jack.Client("sllatency_probe", no_start_server=True)
    sr = probe.samplerate
    blocksize = probe.blocksize
    # SooperLooper's port latencies: loop 0's own ports and the common ones.
    sl_ports = {}
    for name, playback in (("loop0_in_1", False), ("loop0_out_1", True),
                           ("common_in_1", False), ("common_out_1", True)):
        try:
            r = port_latency(probe.get_port_by_name(
                "%s:%s" % (args.sl_client, name)), playback=playback)
        except jack.JackError:
            r = None
        sl_ports[name] = r[1] if r else None
    if args.reported:
        probe.close()
        return quick_report(args, interactive, sl_ports, sr)
    sl_dests = []
    if not args.no_sl:
        sl_dests = _connected(probe, "%s:loop0_out_" % args.sl_client) or \
            _connected(probe, "%s:common_out_" % args.sl_client)
    outs = args.out or choose_outputs(probe, interactive, sl_dests)
    candidates = ([] if args.inp else
                  input_candidates(probe, args.sl_client, args.as_hardware))
    inp = args.inp or (candidates[0] if candidates
                       else default_input(probe, args.sl_client))
    if not outs or not inp:
        sys.exit("sllatency: could not find ports; give --out and --in")
    # Hardware ports mean a mic or cable loop through the audio interface.
    # Otherwise the click goes through a JACK program (e.g. a DAW bus) and
    # back: nothing to compare with QJackctl, and no SooperLooper check.
    try:
        hardware = args.as_hardware or all(
            probe.get_port_by_name(p).is_physical for p in outs + [inp])
    except jack.JackError:
        hardware = False
    lat_out = port_latency(probe.get_port_by_name(outs[0]), playback=True)
    lat_ins = {p: port_latency(probe.get_port_by_name(p), playback=False)
               for p in (candidates or [inp])}
    lat_in = lat_ins[inp]
    probe.close()
    js = jack_settings()

    sl_run = None
    if not args.no_sl and not hardware:
        say("   These are not hardware ports, so SooperLooper is not checked.")
    elif not args.no_sl:
        sl = SooperLooper(args.sl_host, args.sl_port)
        if sl.ping() is None:
            say("   SooperLooper did not answer on port %d; measuring the round "
                "trip only." % args.sl_port)
        else:
            sl_run = SLRun(sl, args.sl_client, args.bpm)

    # The monitoring-path check: play the click into wherever loop 0's
    # output goes, and time its return at the mic, to measure the delay
    # after SooperLooper as actually heard.
    probe_targets, monitor_note = [], None
    if sl_run and sl_run.loops and not args.no_monitor_check:
        state = sl_run.sl.get("state", loop=0)
        if state is not None and state not in SILENT_STATES:
            monitor_note = ("Loop 1 is making sound, so the path SooperLooper is "
                            "heard through is not measured; mute or clear it and "
                            "run again to include that.")
        else:
            probe_targets = loop0_destinations(
                args.sl_client, bool(sl_run.loop0_common.get("use_common_outs")))
            if not probe_targets:
                monitor_note = ("Loop 1's output is not connected to anything, so "
                                "there is no path to measure after SooperLooper.")

    print()
    say("   Click out:   %s" % ", ".join(outs))
    if len(candidates) > 1:
        say("   Mic input:   %s (SooperLooper's loop 1 records from these) -- "
            "whichever hears the click best, found with a few clicks first"
            % " or ".join(candidates))
    else:
        say("   Mic input:   %s" % inp)
    print()
    if hardware:
        explain("""
            Put the headphones fed by those outputs on or right next to the mic
            -- balancing them on the mic works -- or hold the mic right against
            a speaker playing them, or connect a cable from one of the outputs
            to the input.  With a speaker, keep the mic within a few
            centimetres: sound takes about 3 ms per metre through the air, and
            that would be counted as latency.  Don't wear the headphones: the
            click level is %.2f of full scale.
        """ % args.level)
    else:
        explain("""
            These are not hardware ports: the click goes into a JACK program
            and is recorded where it comes out, so no mic or headphones are
            involved.  Anything that output feeds will play the click.
        """)
    if sl_run:
        print()
        explain("""
            SooperLooper will also record a short loop of the click in a
            temporary loop, which is removed afterwards along with the tempo
            and sync changes.  Best run right after starting SooperLooper,
            and do not touch the pedals during the run (about %d s).
        """ % (22 if probe_targets else 15))
        if probe_targets:
            print()
            explain("""
                Then, for a few beats, the click is played into the ports loop
                1's output feeds, to measure the path you hear SooperLooper
                through:
            """)
            print()
            for t in probe_targets:
                say("       %s" % t)
            print()
            explain("Don't record in your DAW during the run.")
        print()
        sl_run.describe()
    if interactive:
        print()
        input("   Press Enter when ready... ")

    if len(candidates) > 1:
        heading("Finding the mic input")
        explain("""
            SooperLooper's loop 1 records from %s.  A few clicks on %s, to
            find which of those inputs hears them:
        """ % (" and ".join(candidates), " and ".join(outs)))
        print()
        inp, heard, n = strongest_input(outs, candidates, sr, args.bpm,
                                        args.level, args.max_latency)
        for p in candidates:
            say("   %-24s %d of %d clicks back, peak %.3f" % (p, heard[p][0], n,
                                                              heard[p][1]))
        print()
        say("   Using %s as the mic input." % inp)
        lat_in = lat_ins[inp]

    period = 60.0 * sr / args.bpm
    beat = 60.0 / args.bpm
    rec_secs = 4 * beat                       # a four-beat loop
    seconds = 1.0 + rec_secs + beat + 7.0     # record, close, then play ~7 s
    click = make_click(sr, args.level)
    signal, starts = build_output(sr, args.bpm, seconds, click)
    probe_signal, probe_starts = None, []
    if probe_targets:
        # After the direct clicks: a short pause, then clicks on the probe
        # output only, so the mic hears one route at a time.
        p0 = len(signal) + int(0.3 * sr)
        probe_starts = [p0 + int(round(k * period)) for k in range(8)]
        total = (probe_starts[-1] + len(click)
                 + int((args.max_latency / 1000.0 + 0.5) * sr))
        signal = np.concatenate([signal, np.zeros(total - len(signal),
                                                  dtype=np.float32)])
        probe_signal = np.zeros(total, dtype=np.float32)
        for st in probe_starts:
            probe_signal[st:st + len(click)] = click

    heading("Measuring")
    explain("""
        Playing %d clicks at %g BPM and recording the input in the same JACK
        cycles, so each click's return can be timed to the sample.
    """ % (len(starts), args.bpm))
    in_sources = [inp]
    actions, extra = [], []
    loop_len = None
    try:
        if sl_run:
            sl_run.set_up()
            in_sources.append(sl_run.port("out"))
            extra.append((inp, sl_run.port("in")))
            actions = [(1.0, lambda: sl_run.sl.hit("record", sl_run.index)),
                       (1.0 + rec_secs + 0.05, lambda: sl_run.sl.hit("record", sl_run.index))]
            print()
            explain("SooperLooper records four beats of it, then plays the loop back.")
        rec = ClickRecorder("sllatency", outs, in_sources, probe_targets)
        recs = rec.run(signal, actions=actions, extra_connections=extra,
                       probe_signal=probe_signal)
        if args.dump:
            np.savez(args.dump, mic=recs[0], starts=starts,
                     probe_starts=np.array(probe_starts), click=click, sr=sr)
        if sl_run:
            loop_len = sl_run.sl.get("loop_len", loop=sl_run.index)
    finally:
        if sl_run:
            sl_run.restore()

    ms = lambda f: f * 1000.0 / sr
    heading("Result: audio interface" if hardware else "Result: delay through the chain")
    if rec.xruns:
        say("   WARNING: %d xrun(s) during the run -- the result may be wrong; "
            "run it again." % rec.xruns)
    lags = measure_round_trip(recs[0], starts, click, sr, args.max_latency)
    if len(lags) < max(3, len(starts) // 2):
        say("   Only %d of %d clicks came back clearly.  Raise --level, move the "
            "headphones closer to the mic, or check the connections."
            % (len(lags), len(starts)))
        if not len(lags):
            return 1
    rt = float(np.median(lags))
    spread = float(np.percentile(lags, 75) - np.percentile(lags, 25))
    say("   Round trip: %.1f samples (%.2f ms) from %d clicks, spread %.1f."
        % (rt, ms(rt), len(lags), spread))
    if not hardware:
        through = rt - blocksize
        print()
        explain("""
            The click went from this tool through another JACK program and back
            into this tool.  JACK breaks such a loop by adding one period (%d
            samples), so the delay through the program itself is %.1f samples
            (%.2f ms).  There is no QJackctl setting to compare with.

            Caution: to the program, its output coming back to its input is a
            feedback loop.  Some programs react to that -- Ardour lights its
            "No Align" warning and reports a larger latency to JACK after
            every run -- and the effect can outlast the run and skew a
            SooperLooper check.  Restart the program before running one.
        """ % (blocksize, through, ms(through)))
        return 0
    if lat_out and lat_in:
        reported = lat_out[1] + lat_in[1]
        diff = rt - reported
        extras = ""
        if "in_extra" in js and "out_extra" in js:
            extras = ("  JACK's settings: period %s x %s, extra input latency %d, "
                      "extra output latency %d." % (js.get("period", "?"),
                      js.get("nperiods", "?"), js["in_extra"], js["out_extra"]))
        print()
        explain("""
            JACK reports %d samples of playback latency on %s and %d of
            capture latency on %s: %d in total.%s
        """ % (lat_out[1], outs[0], lat_in[1], inp, reported, extras))
        print()
        if abs(ms(diff)) <= args.tolerance:
            say("   JACK: off by %+.1f samples (%+.2f ms), within %.1f ms -- nothing "
                "to change." % (diff, ms(diff), args.tolerance))
        else:
            total = int(round(diff))
            say("   JACK: off by %+.1f samples (%+.2f ms): its latency settings are "
                "%s." % (diff, ms(diff), "too low" if diff > 0 else "too high"))
            if "in_extra" in js and "out_extra" in js:
                explain("""
                    In QJackctl, change extra input latency %d -> %d and extra
                    output latency %d -> %d, then restart JACK.  (A loopback
                    measures only the sum, so the split cannot be measured; an
                    even split is the usual choice.)
                """ % (js["in_extra"], js["in_extra"] + total // 2,
                       js["out_extra"], js["out_extra"] + total - total // 2))
            else:
                explain("""
                    Change QJackctl's extra input + output latency by %+d samples
                    in total, then restart JACK.
                """ % total)
    else:
        say("   Could not read JACK's reported latencies for these ports.")

    if not sl_run:
        return 0

    heading("Result: SooperLooper")
    beats = loop_len / beat if loop_len else None
    if beats is None or abs(beats - round(beats)) > 0.01:
        say("   The loop is %s beats long, not a whole number, so it was not "
            "recorded in sync and cannot be measured.  Check that SooperLooper's "
            "sync works (loop 1 quantize, sync source)."
            % ("?" if beats is None else "%.3f" % beats))
        return 1
    offs = measure_loop_offset(recs[1], starts, click, sr, period)
    if len(offs) < 4:
        say("   Only %d loop clicks found -- the loop may not have recorded.  Check "
            "that its input is connected." % len(offs))
        return 1
    e = float(np.median(offs))
    sp = float(np.percentile(offs, 75) - np.percentile(offs, 25))
    # Latency added after SooperLooper (e.g. DAW plugins on its route), as
    # JACK reports it: the playback latency on SooperLooper's output minus
    # the hardware output's.  Only meaningful when SooperLooper's output
    # actually reaches the hardware through JACK.
    reported_down = None
    outs_sl = [v for v in (sl_ports.get("loop0_out_1"), sl_ports.get("common_out_1"))
               if v is not None]
    sl_out = max(outs_sl) if outs_sl else None
    if sl_out is not None and lat_out and sl_out >= lat_out[1]:
        reported_down = sl_out - lat_out[1]
    # The same, measured: the click played into loop 0's destinations comes
    # back at the mic later than the direct click by exactly that delay.
    measured_down = None
    second = None
    if probe_targets:
        # A return sooner than the direct round trip cannot be the probe:
        # nothing after SooperLooper takes negative time.
        found = []
        for st in probe_starts:
            lg = measure_round_trip(recs[0], [st], click, sr, args.max_latency)
            if len(lg) and lg[0] > rt - 0.001 * sr:
                found.append((st, float(lg[0])))
        if len(found) >= len(probe_starts) // 2:
            measured_down = float(np.median([lg for _, lg in found])) - rt
            sec = second_returns(recs[0], [st for st, _ in found],
                                 [lg for _, lg in found], click, sr, args.max_latency)
            if (len(sec) >= len(found) // 2
                    and np.percentile(sec, 75) - np.percentile(sec, 25) < 0.001 * sr):
                second = float(np.median(sec)) - rt
    say("   At SooperLooper's output the loop is %+.1f samples (%+.2f ms) from the "
        "live click (%d clicks, spread %.1f)." % (e, ms(e), len(offs), sp))
    print()
    if measured_down is not None:
        downstream = measured_down
        rep_txt = ("" if reported_down is None else
                   "  JACK reports %d samples for it%s." % (
                       reported_down,
                       "" if abs(reported_down - measured_down) < 2.0 else
                       " -- %.0f samples %s" % (abs(reported_down - measured_down),
                                               "short" if reported_down < measured_down
                                               else "over")))
        explain("""
            After SooperLooper, measured through the path you hear it by: %.1f
            samples (%.2f ms).%s
        """ % (measured_down, ms(measured_down), rep_txt))
        if second is not None:
            print()
            explain("""
                A second copy of the click came back %.1f samples (%.2f ms)
                after SooperLooper: its output is heard by two routes at
                different delays, which smears or doubles the loop.  Check
                that only one route reaches the headphones.
            """ % (second, ms(second)))
    else:
        downstream = reported_down or 0
        if probe_targets:
            explain("""
                Nothing came back from the path SooperLooper's output feeds: it
                is not heard at this mic -- its monitoring is disconnected,
                muted, turned down, or goes to other speakers.  Using the
                latency JACK reports for it instead (%d samples).
            """ % downstream)
        elif monitor_note:
            explain(monitor_note + "  Using the latency JACK reports for it "
                    "instead (%d samples)." % downstream)
        elif reported_down:
            explain("""
                SooperLooper's output goes through more processing (e.g. DAW
                plugins) before the speakers: JACK reports %d samples more
                latency there than on %s -- %.2f ms added after SooperLooper.
            """ % (downstream, outs[0], ms(downstream)))
        elif sl_out is not None and lat_out and sl_out < lat_out[1]:
            explain("""
                SooperLooper's output does not seem to reach the speakers through
                JACK (its reported playback latency is below the hardware's), so
                any processing after it cannot be included.
            """)
    heard = e + downstream
    print()
    say("   As heard, loops play %+.1f samples (%+.2f ms) %s."
        % (heard, ms(heard), "late" if heard > 0 else "early"))

    l0 = sl_run.loop0
    if not l0:
        return 0
    il, ol = int(l0["input_latency"]), int(l0["output_latency"])
    total = il + ol + int(round(heard))
    # JACK's latency along the path the audio actually takes: an unconnected
    # port reports 0, so take whichever of the loop's own and the common
    # port is larger.
    def path(a, b):
        vals = [v for v in (sl_ports.get(a), sl_ports.get(b)) if v is not None]
        return max(vals) if vals else None
    jack_in = path("loop0_in_1", "common_in_1")
    jack_out = path("loop0_out_1", "common_out_1")
    if jack_in is not None and jack_out is not None and jack_in + jack_out > 0:
        new_in, new_out = split_total(total, jack_in, jack_out)
    else:
        jack_in = jack_out = None
        new_in, new_out = split_total(total, il, ol)
    # What autoset would set: the same rule SooperLooper uses.
    common = sl_run.loop0_common
    auto_in = sl_ports.get("common_in_1" if common.get("use_common_ins")
                           else "loop0_in_1")
    auto_out = sl_ports.get("common_out_1" if common.get("use_common_outs")
                            else "loop0_out_1")
    if auto_in is None or auto_out is None:
        auto_in = auto_out = None
    auto_misses = (auto_in is not None and jack_in is not None
                   and (auto_in < jack_in or auto_out < jack_out))
    print()
    row = "   %-34s %7s %7s %7s"
    say(row % ("SooperLooper latency (samples)", "input", "output", "total"))
    say(row % ("current (%s):" % ("autoset" if l0.get("autoset_latency") else "manual"),
               il, ol, il + ol))
    if auto_in is not None:
        say(row % ("SooperLooper's autoset would use:", auto_in, auto_out,
                   auto_in + auto_out))
    say(row % ("measured best:", new_in, new_out, total))
    print()
    if jack_in is not None:
        explain("""
            Autoset copies the latencies JACK reports for SooperLooper's ports.
            The measured best keeps JACK's split between input and output and
            corrects the total by what was measured.  The total sets how loops
            line up; input latency also sets where each recording starts.
        """)
        if auto_misses:
            print()
            explain("""
                Autoset would come out short here: with use_common_ins/outs on,
                it reads SooperLooper's common ports, which are not connected
                (JACK reports 0 latency for an unconnected port), not the loop's
                own ports that carry the audio.
            """)
        if not l0.get("autoset_latency"):
            print()
            explain("""
                Turning autoset on replaces the hand-set values in the running
                SooperLooper, and turning it off again does not bring them back.
                The session file keeps them until it is saved.
            """)
    else:
        explain("""
            JACK gives no latencies for SooperLooper's ports, so the measured
            best keeps the current split between input and output and corrects
            the total by what was measured.
        """)
    within = abs(ms(heard)) <= args.tolerance
    if within:
        print()
        say("   Within %.1f ms -- nothing needs changing." % args.tolerance)
    print()
    if not l0.get("autoset_latency"):
        explain("""
            Manual latencies include buffer-dependent latency: they are right
            for buffer size %d only; measure again after changing it.
        """ % blocksize)

    return offer_apply_and_save(sl_run.sl, args, interactive, il, ol,
                                new_in, new_out, within)

if __name__ == "__main__":
    sys.exit(main())
