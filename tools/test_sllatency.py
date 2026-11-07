#!/usr/bin/env python3
"""Known-answer tests for sllatency's round-trip measurement.

Starts a private dummy-backend JACK server (no hardware, no effect on a
running rig) and a helper client that stands in for the audio hardware:
it delays the click by a known number of samples and can blur it (a
simple low-pass, like a mic and headphones) and add noise.  The click
loops from sllatency's output, through the helper, back to its input, so
JACK adds one buffer for the loop; the expected round trip is the delay
plus one buffer.

The second part runs the whole tool, including SooperLooper: a real
sooperlooper engine on the same private server, with its home directory
pointed at a temporary folder.  If --session is given, a *copy* of that
session file is loaded into it (so the test uses real settings without
touching the file); otherwise loop 0 gets manual 743/743 latencies over
OSC.  The loop's measured offset must equal (delay + one buffer) minus
(input_latency + output_latency).

Usage: test_sllatency.py [--engine PATH] [--session FILE]   (about 90 s)
"""
import os
import subprocess
import sys
import threading
import time

import numpy as np
import jack

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import sllatency  # noqa: E402

SERVER = "sllat_test_%d" % os.getpid()
PERIOD = 256
SR = 48000


class FakeHardware:
    """Delay (samples), optional one-pole low-pass, optional noise."""

    def __init__(self, delay, lowpass_hz=None, noise=0.0, name="fakehw", gain=1.0):
        self.c = jack.Client(name, no_start_server=True)
        self.i = self.c.inports.register("in")
        self.o = self.c.outports.register("out")
        self.buf = np.zeros(delay + 8192, dtype=np.float32)
        self.delay = delay
        self.w = 0
        self.a = None if not lowpass_hz else np.exp(-2 * np.pi * lowpass_hz / SR)
        self.y = 0.0
        self.noise = noise
        self.gain = gain
        self.rng = np.random.default_rng(1)
        self.c.set_process_callback(self.process)

    def process(self, frames):
        x = self.i.get_array().astype(np.float64)
        if self.a is not None:
            y = np.empty_like(x)
            prev = self.y
            for k in range(len(x)):
                prev = (1 - self.a) * x[k] + self.a * prev
                y[k] = prev
            self.y = prev
            x = y
        n = len(self.buf)
        idx = (self.w + np.arange(frames)) % n
        self.buf[idx] = x
        ridx = (self.w - self.delay + np.arange(frames)) % n
        out = self.buf[ridx] * self.gain
        if self.noise:
            out += self.noise * self.rng.standard_normal(frames).astype(np.float32)
        self.o.get_array()[:] = out
        self.w = (self.w + frames) % n


def run_case(delay, lowpass_hz=None, noise=0.0):
    hw = FakeHardware(delay, lowpass_hz, noise)
    hw.c.activate()
    try:
        rec = sllatency.ClickRecorder("sllatency", ["fakehw:in"], ["fakehw:out"])
        click = sllatency.make_click(SR, 0.3)
        sig, starts = sllatency.build_output(SR, 120.0, 4.0, click)
        (r,) = rec.run(sig)
        lags = sllatency.measure_round_trip(r, starts, click, SR, 250.0)
        return lags, rec.xruns
    finally:
        hw.c.deactivate()
        hw.c.close()


def run_tool(delay, sl_port, sl_client, extra=(), answers=None, home=None, wire=(),
             noise=0.0):
    """Run the sllatency command line through the fake hardware; return
    (round trip, loop offset) parsed from its report, and the report.
    With `answers`, run interactively and feed them as typed input.  HOME
    is a temporary directory so the tool's remembered settings never
    touch the real one."""
    hw = FakeHardware(delay, noise=noise)
    hw.c.activate()
    for src, dst in wire:           # e.g. SooperLooper's main ports as "hardware"
        hw.c.connect(src, dst)
    import tempfile
    home = home or tempfile.mkdtemp(prefix="sllat_home_")
    cmd = [sys.executable, os.path.join(HERE, "sllatency.py"),
           "--out", "fakehw:in", "--in", "fakehw:out",
           "--sl-port", str(sl_port), "--sl-client", sl_client,
           "--as-hardware"] + list(extra)
    if answers is None:
        cmd.append("--yes")
    try:
        out = subprocess.run(cmd, input=answers, capture_output=True, text=True,
                             timeout=90, env=dict(os.environ, HOME=home)).stdout
    finally:
        hw.c.deactivate()
        hw.c.close()
    rt = off = None
    for line in out.splitlines():
        if "Round trip:" in line:
            rt = float(line.split("Round trip:")[1].split()[0])
        if "At SooperLooper's output the loop is" in line:
            off = float(line.split("the loop is")[1].split()[0])
    return rt, off, out


def sl_tests(engine, session):
    import shutil, tempfile
    sys.path.insert(0, HERE)
    tmp = tempfile.mkdtemp(prefix="sllat_sl_")
    port = 19500 + os.getpid() % 400
    client = "sl_test"
    eng = subprocess.Popen([engine, "-S", SERVER, "-p", str(port), "-l", "1",
                            "-c", "2", "-j", client],
                           env=dict(os.environ, HOME=tmp),
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    fails = 0
    try:
        time.sleep(3.0)
        sl = sllatency.SooperLooper("127.0.0.1", port)
        if session:
            copy = os.path.join(tmp, "session_copy.slsess")
            shutil.copy(session, copy)
            sl.send("/load_session", copy, sl.url, "/err")
            time.sleep(2.0)
        else:
            sl.set("autoset_latency", 0.0, loop=0)
            sl.set("input_latency", 743.0, loop=0)
            sl.set("output_latency", 743.0, loop=0)
            sl.set("trigger_latency", 0.0, loop=0)
        il = sl.get("input_latency", loop=0); ol = sl.get("output_latency", loop=0)
        auto = sl.get("autoset_latency", loop=0)
        before = {g: sl.get(g) for g in ("sync_source", "tempo", "eighth_per_cycle")}
        before["loop0 quantize"] = sl.get("quantize", loop=0)
        print("SooperLooper loop 0: input_latency %s output_latency %s autoset %s%s"
              % (il, ol, auto, " (from a copy of %s)" % session if session else ""))
        for delay in (1230, 1486, 1000):
            rt, off, report = run_tool(delay, port, client)
            exp_rt = delay + PERIOD
            exp_off = exp_rt - (il + ol)
            # SooperLooper's own record/playback arithmetic leaves a fixed
            # offset of about 3 samples (0.06 ms); allow 5.
            ok = (rt is not None and off is not None and abs(rt - exp_rt) <= 0.5
                  and abs(off - exp_off) <= 5.0)
            fails += not ok
            print("%s delay %5d: round trip %s (expected %d), loop offset %s "
                  "(expected %+g)" % ("PASS" if ok else "FAIL", delay, rt, exp_rt,
                                      off, exp_off))
            if not ok:
                print(report)
        fails += autoset_row_tests(sl, port, client)
        fails += monitor_check_tests(sl, port, client)
        fails += quick_mode_test(sl, port, client)
        # A rig using SooperLooper's main (common) ports: the mic feeds the
        # main input and the main output goes to the speakers.  The
        # temporary loop must not play out through them (the mic would hear
        # it); the result must match the direct case above.
        rt, off, report = run_tool(1230, port, client, wire=(
            ("fakehw:out", "%s:common_in_1" % client),
            ("%s:common_out_1" % client, "fakehw:in")))
        exp_rt, exp_off = 1230 + PERIOD, 1230 + PERIOD - (il + ol)
        ok = (rt is not None and off is not None and abs(rt - exp_rt) <= 0.5
              and abs(off - exp_off) <= 5.0)
        fails += not ok
        print("%s main ports wired as the rig's I/O: round trip %s (expected %d), "
              "loop offset %s (expected %+g)" % ("PASS" if ok else "FAIL", rt, exp_rt,
                                                off, exp_off))
        if not ok:
            print(report)
        fails += stereo_input_test(port, client, il, ol)
        fails += apply_save_tests(sl, port, client, tmp, session, il, ol)
        fails += trigger_tool_tests(sl, port, client, tmp, session, il)
        n = sl.ping()
        after = {g: sl.get(g) for g in ("sync_source", "tempo", "eighth_per_cycle")}
        after["loop0 quantize"] = sl.get("quantize", loop=0)
        ok = n == 1 and after == before
        fails += not ok
        print("%s temporary loop removed and settings restored (loop count %s; "
              "before %s, after %s)" % ("PASS" if ok else "FAIL", n, before, after))
    finally:
        eng.terminate()
        try:
            eng.wait(timeout=5)
        except subprocess.TimeoutExpired:
            eng.kill()
        shutil.rmtree(tmp, ignore_errors=True)
    return fails


def chain_test(delay=32):
    """Through a non-hardware JACK program and back, like a DAW bus: the tool
    must report the program's own delay (the round trip minus the one period
    JACK adds to break the loop) and give no QJackctl advice."""
    hw = FakeHardware(delay)
    hw.c.activate()
    try:
        out = subprocess.run(
            [sys.executable, os.path.join(HERE, "sllatency.py"), "--no-sl", "--yes",
             "--out", "fakehw:in", "--in", "fakehw:out"],
            capture_output=True, text=True, timeout=90).stdout
    finally:
        hw.c.deactivate()
        hw.c.close()
    got = None
    flat = " ".join(out.split())
    if "program itself is" in flat:
        got = float(flat.split("program itself is")[1].split()[0])
    ok = got is not None and abs(got - delay) <= 0.5 and "restart JACK" not in flat
    print("%s chain through a JACK program: %s samples (expected %d), no QJackctl advice"
          % ("PASS" if ok else "FAIL", got, delay))
    if not ok:
        print(out)
    return 0 if ok else 1


def _flat(text):
    return " ".join(text.split())


def stereo_input_test(port, client, il, ol):
    """A stereo loop fed from two mics, no --in: the tool must try both and
    use the one that hears the click best.  The quiet mic has a different
    delay, so the round trip shows which one was used."""
    import tempfile
    quiet = FakeHardware(900, name="fakemic_quiet", gain=0.1)
    loud = FakeHardware(1230, name="fakemic_loud")
    for h in (quiet, loud):
        h.c.activate()
    quiet.c.connect("fakemic_quiet:out", "%s:loop0_in_1" % client)
    loud.c.connect("fakemic_loud:out", "%s:loop0_in_2" % client)
    cmd = [sys.executable, os.path.join(HERE, "sllatency.py"),
           "--out", "fakemic_quiet:in", "--out", "fakemic_loud:in",
           "--sl-port", str(port), "--sl-client", client,
           "--as-hardware", "--yes", "--no-monitor-check"]
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=90,
                             env=dict(os.environ, HOME=tempfile.mkdtemp(
                                 prefix="sllat_home_"))).stdout
    finally:
        for h in (quiet, loud):
            h.c.deactivate()
            h.c.close()
    rt = None
    for line in out.splitlines():
        if "Round trip:" in line:
            rt = float(line.split("Round trip:")[1].split()[0])
    ok = ("Using fakemic_loud:out as the mic input." in out and rt is not None
          and abs(rt - (1230 + PERIOD)) <= 0.5)
    print("%s stereo loop, no --in: used the louder mic (round trip %s, "
          "expected %d)" % ("PASS" if ok else "FAIL", rt, 1230 + PERIOD))
    if not ok:
        print(out)
    return 0 if ok else 1


def monitor_check_tests(sl, port, client):
    """The monitoring-path check.  A fake DAW bus with a known delay sits
    between loop 0's output and the fake hardware, as Ardour's monitor bus
    does on the rig; the tool must measure that delay through the mic path,
    say when nothing comes back, and notice a second route."""
    fails = 0
    out1 = "%s:loop0_out_1" % client
    buses = []
    wired = []
    c = jack.Client("monitor_check_test", no_start_server=True)
    c.activate()
    def bus(name, delay):
        b = FakeHardware(delay, name=name)
        b.c.activate()
        buses.append(b)
        c.connect(out1, "%s:in" % name)
        wired.append((out1, "%s:in" % name))
        return b
    try:
        # Heard through one bus: must measure its 32-sample delay.
        bus("fakebus", 32)
        _, _, report = run_tool(1230, port, client, wire=[("fakebus:out", "fakehw:in")])
        flat = _flat(report)
        got = None
        if "path you hear it by:" in flat:
            got = float(flat.split("path you hear it by:")[1].split()[0])
        ok = got is not None and abs(got - 32) <= 1.0 and "second copy" not in flat
        fails += not ok
        print("%s monitoring path measured through a 32-sample bus: %s" %
              ("PASS" if ok else "FAIL", got))
        if not ok:
            print(report)
        # Not heard: the bus goes nowhere.
        _, _, report = run_tool(1230, port, client)
        ok = "Nothing came back from the path" in _flat(report)
        fails += not ok
        print("%s monitoring path not heard is reported" % ("PASS" if ok else "FAIL"))
        if not ok:
            print(report)
        # Not heard, with a quiet, noisy mic (the rig, 2026-09-26: the noise
        # alone once read as a return 1472 samples before the direct one).
        rt, _, report = run_tool(1230, port, client, extra=["--level", "0.03"],
                                 noise=0.002)
        flat = _flat(report)
        ok = (rt is not None and abs(rt - (1230 + PERIOD)) <= 1.0
              and "Nothing came back from the path" in flat)
        fails += not ok
        print("%s monitoring path not heard, noisy mic: reported as not heard "
              "(round trip %s)" % ("PASS" if ok else "FAIL", rt))
        if not ok:
            print(report)
        # Two routes: a second bus at 300 samples also reaches the hardware.
        bus("fakebus2", 300)
        _, _, report = run_tool(1230, port, client, wire=[("fakebus:out", "fakehw:in"),
                                                          ("fakebus2:out", "fakehw:in")])
        flat = _flat(report)
        got2 = None
        if "second copy of the click came back" in flat:
            got2 = float(flat.split("second copy of the click came back")[1].split()[0])
        ok = got2 is not None and abs(got2 - 300) <= 2.0
        fails += not ok
        print("%s second route reported: %s samples (expected 300)" %
              ("PASS" if ok else "FAIL", got2))
        if not ok:
            print(report)
    finally:
        for src, dst in wired:
            try:
                c.disconnect(src, dst)
            except jack.JackError:
                pass
        for b in buses:
            b.c.deactivate()
            b.c.close()
        c.deactivate()
        c.close()
    return fails


def quick_mode_test(sl, port, client):
    """--reported: no clicks; suggests JACK's latencies for the path loop 0
    is wired through (here the dummy server's own hardware ports)."""
    wires = (("system:capture_1", "%s:loop0_in_1" % client),
             ("%s:loop0_out_1" % client, "system:playback_1"))
    before = {k: sl.get(k, loop=0) for k in ("use_common_ins", "use_common_outs")}
    c = jack.Client("quick_mode_test", no_start_server=True)
    c.activate()
    try:
        for src, dst in wires:
            c.connect(src, dst)
        cap = sllatency.port_latency(c.get_port_by_name("system:capture_1"), False)[1]
        pb = sllatency.port_latency(c.get_port_by_name("system:playback_1"), True)[1]
        _, _, report = run_tool(1230, port, client, extra=["--reported"])
        row = [l for l in report.splitlines() if "suggested (JACK's reports):" in l]
        got = tuple(int(v) for v in row[0].split("suggested (JACK's reports):")[1].split()[:2]) \
            if row else None
        ok = got == (cap, pb) and "Round trip:" not in report
        print("%s quick mode suggests JACK's figures: %s (expected %s), no clicks played"
              % ("PASS" if ok else "FAIL", got, (cap, pb)))
        if not ok:
            print(report)
        return 0 if ok else 1
    finally:
        for src, dst in wires:
            try:
                c.disconnect(src, dst)
            except jack.JackError:
                pass
        c.deactivate()
        c.close()
        for k, v in before.items():
            sl.set(k, v, loop=0)


def autoset_row_tests(sl, port, client):
    """The "autoset would use" row must follow SooperLooper's own rule: with
    use_common_ins/outs on it reads the common ports, which the rig leaves
    unconnected (JACK reports 0), not loop 0's own ports."""
    fails = 0
    before = {k: sl.get(k, loop=0) for k in ("use_common_ins", "use_common_outs")}
    wires = (("system:capture_1", "%s:loop0_in_1" % client),
             ("%s:loop0_out_1" % client, "system:playback_1"))
    c = jack.Client("autoset_row_test", no_start_server=True)
    c.activate()
    try:
        for src, dst in wires:
            c.connect(src, dst)
        cap = sllatency.port_latency(c.get_port_by_name("system:capture_1"), False)[1]
        pb = sllatency.port_latency(c.get_port_by_name("system:playback_1"), True)[1]
        for common, expect, short in ((1.0, (0, 0), True), (0.0, (cap, pb), False)):
            sl.set("use_common_ins", common, loop=0)
            sl.set("use_common_outs", common, loop=0)
            time.sleep(0.3)
            _, _, report = run_tool(1230, port, client)
            row = [l for l in report.splitlines() if "autoset would use:" in l]
            got = tuple(int(v) for v in row[0].split("autoset would use:")[1].split()[:2]) \
                if row else None
            said_short = "Autoset would come out short" in report
            ok = got == expect and said_short == short
            fails += not ok
            print("%s autoset row with use_common %d: %s (expected %s), short note %s"
                  % ("PASS" if ok else "FAIL", common, got, expect, said_short))
            if not ok:
                print(report)
    finally:
        for src, dst in wires:
            try:
                c.disconnect(src, dst)
            except jack.JackError:
                pass
        c.deactivate()
        c.close()
        for k, v in before.items():
            sl.set(k, v, loop=0)
    return fails


def _session_file(sl, tmp, session, name):
    """A session file to save into: a copy of the given one, or one written
    by SooperLooper itself."""
    import shutil
    path = os.path.join(tmp, name)
    if session:
        shutil.copy(session, path)
    else:
        sl.send("/save_session", path, sl.url, "/err")
        time.sleep(1.0)
    return path


def _check_saved(path, orig_text, new_in, new_out):
    """Only the latency values and auto_latency may differ; backup exists."""
    import glob, re
    now = open(path).read()
    lines_a, lines_b = orig_text.splitlines(), now.splitlines()
    changed = [(x, y) for x, y in zip(lines_a, lines_b) if x != y]
    ok = len(lines_a) == len(lines_b) and all(
        ("input_latency" in x or "output_latency" in x or "auto_latency" in x)
        for x, _ in changed)
    ok = ok and ('name="input_latency" value="%d"' % new_in) in now
    ok = ok and ('name="output_latency" value="%d"' % new_out) in now
    ok = ok and bool(glob.glob(path + ".bak-*"))
    return ok, len(changed)


def apply_save_tests(sl, port, client, tmp, session, il, ol):
    """Step 3: scripted --apply --save, then the interactive questions."""
    fails = 0
    delay = 1486                          # loops heard 256 - 3 samples late
    # scripted
    path = _session_file(sl, tmp, session, "scripted.slsess")
    orig = open(path).read()
    rt, off, report = run_tool(delay, port, client,
                               ["--apply", "--save", "--session-file", path])
    exp_total = int(il + ol) + int(round(off))
    got = (sl.get("input_latency", loop=0), sl.get("output_latency", loop=0))
    saved_ok, nchanged = _check_saved(path, orig, int(got[0]), int(got[1]))
    ok = (got[0] is not None and int(got[0] + got[1]) == exp_total
          and sl.get("autoset_latency", loop=0) == 0.0 and saved_ok)
    fails += not ok
    print("%s scripted --apply --save: live %s, total %s (expected %d), file "
          "changed only in %d latency line(s), backup kept"
          % ("PASS" if ok else "FAIL", got, None if got[0] is None else got[0] + got[1],
             exp_total, nchanged))
    if not ok:
        print(report)
    # put loop 0 back for the interactive run
    for k, v in (("input_latency", il), ("output_latency", ol)):
        sl.set(k, v, loop=0)
    time.sleep(0.3)
    # interactive: Enter to start, y to apply, y to save, then the file path
    path2 = _session_file(sl, tmp, session, "interactive.slsess")
    orig2 = open(path2).read()
    rt, off, report = run_tool(delay, port, client,
                               answers="\ny\ny\n%s\n" % path2)
    got2 = (sl.get("input_latency", loop=0), sl.get("output_latency", loop=0))
    saved_ok2, n2 = _check_saved(path2, orig2, int(got2[0]), int(got2[1]))
    # Two separate measurements: allow them to differ by a sample.
    ok2 = (got2[0] is not None and abs((got2[0] + got2[1]) - (got[0] + got[1])) <= 1
           and saved_ok2 and "Save them in a session file?" in report)
    fails += not ok2
    print("%s interactive apply and save (asked for the file path): live %s, "
          "file changed in %d line(s)" % ("PASS" if ok2 else "FAIL", got2, n2))
    if not ok2:
        print(report)
    for k, v in (("input_latency", il), ("output_latency", ol)):
        sl.set(k, v, loop=0)
    return fails


def session_save_tests():
    """Saving to the session file, without JACK or SooperLooper: symlinks,
    permissions, a file with no latency values, and the dialog -- the
    notice before the question, the file picker (a stand-in here) chosen,
    canceled or unavailable, the file used last time as its starting
    point, and scripted --save."""
    import tempfile, io, contextlib, types, builtins, shutil
    fails = 0
    def check(ok, msg):
        nonlocal fails
        fails += not ok
        print("%s %s" % ("PASS" if ok else "FAIL", msg))

    tmp = tempfile.mkdtemp(prefix="sllat_save_")
    body = ('<Loop auto_latency="yes">\n'
            '<Control name="input_latency" value="743"/>\n'
            '<Control name="output_latency" value="743"/>\n</Loop>\n') * 2
    target = os.path.join(tmp, "real.slsess")
    def fresh():
        with open(target, "w") as f:
            f.write(body)
    fresh()
    os.chmod(target, 0o640)
    link = os.path.join(tmp, "link.slsess")
    os.symlink(target, link)
    real, backup, n = sllatency.save_latency_to_session(link, 608, 880)
    text = open(target).read()
    check(os.path.islink(link) and real == target and n == 2
          and text.count('value="608"') == 2 and text.count('"880"') == 2
          and 'auto_latency="yes"' not in text,
          "symlink followed: the link stays a link, its target is changed")
    check(os.path.dirname(backup) == tmp and open(backup).read() == body,
          "backup next to the real file, holding the old contents")
    check((os.stat(target).st_mode & 0o777) == 0o640, "permissions kept (0640)")

    bad = os.path.join(tmp, "bad.slsess")
    with open(bad, "w") as f:
        f.write("<nothing/>\n")
    before = set(os.listdir(tmp))
    try:
        sllatency.save_latency_to_session(bad, 1, 2)
        raised = False
    except ValueError:
        raised = True
    check(raised and set(os.listdir(tmp)) == before, "no latency values: error, nothing written")

    # The dialog, with a stand-in SooperLooper, picker and typed answers.
    class FakeSL:
        def __init__(self): self.v = {}
        def set(self, k, v, loop=None): self.v[k] = float(v)
        def get(self, k, loop=None): return self.v.get(k)
    class Tty(io.StringIO):
        def isatty(self): return True
    saved = (sllatency.CONFIG, sllatency.pick_session_file, builtins.input, sys.stdin)
    sllatency.CONFIG = os.path.join(tmp, "sllatency.conf")
    picked_from = []
    def run_dialog(answers, picker_result="none", session_file=None, interactive=True,
                   save=False):
        feed = iter(answers)
        builtins.input = lambda prompt="": (print(prompt), next(feed))[1]
        def picker(initial):
            picked_from.append(initial)
            return None if picker_result == "none" else picker_result
        sllatency.pick_session_file = picker
        sys.stdin = Tty()
        args = types.SimpleNamespace(session_file=session_file, apply=True, save=save)
        out = io.StringIO()
        try:
            with contextlib.redirect_stdout(out):
                rc = sllatency.offer_apply_and_save(FakeSL(), args, interactive, 743, 743,
                                                    608, 880)
        finally:
            builtins.input = saved[2]
            sys.stdin = saved[3]
        return rc, " ".join(out.getvalue().split())
    try:
        rc, out = run_dialog(["y", "y"], picker_result=target)
        check(rc == 0 and "Saved in" in out and picked_from[-1] is None
              and out.index("NAME.bak-YYYYMMDD-HHMMSS") < out.index("Save them in a session file?"),
              "backup explained before the question; picker's file saved to")
        fresh()
        rc, out = run_dialog(["y", "y"], picker_result=target)
        check(picked_from[-1] == target, "next time the picker starts at the file used last time")
        rc, out = run_dialog(["y", "y"], picker_result="")
        check(rc == 0 and "No session file chosen; nothing saved." in out,
              "picker canceled: nothing saved, and it says so")
        fresh()
        rc, out = run_dialog(["y", "y", ""], picker_result="none")
        check(rc == 0 and "Saved in" in out and ("[%s]" % target) in out,
              "no picker possible: typed instead, Enter takes the file used last time")
        rc, out = run_dialog(["y", "y", bad], picker_result="none")
        check(rc == 1 and "has no latency values to change" in out,
              "not a session file: plain message, no traceback")
        rc, out = run_dialog([], interactive=False, save=True)
        check(rc == 1 and "--save needs --session-file" in out,
              "scripted --save without --session-file: refused")
        fresh()
        rc, out = run_dialog([], interactive=False, save=True, session_file=target)
        check(rc == 0 and "Saved in" in out, "scripted --save --session-file: saved")
    finally:
        sllatency.CONFIG, sllatency.pick_session_file = saved[0], saved[1]
        shutil.rmtree(tmp, ignore_errors=True)
    return fails


def buzz_tests():
    """A buzzing amp by the mic (sharp 120 Hz spikes, locked to the click
    rate as mains is) with a weak click return: the round trip must come
    out right, or not at all -- never a buzz spike's time.  Before the
    band filter, 0.002 buzz with a 0.010 return gave 390 for 1486, which
    is how a rig run once read 74 samples.  No JACK needed."""
    fails = 0
    delay = 1486
    for buzz, ret in ((0.002, 0.010), (0.005, 0.030), (0.010, 0.030), (0.050, 0.010)):
        click = sllatency.make_click(SR, 0.3)
        sig, starts = sllatency.build_output(SR, 120.0, 10.0, click)
        n = len(sig)
        t = np.arange(n) / SR
        rng = np.random.default_rng(5)
        rec = (buzz * np.exp(-((t * 120.0) % 1.0) * 40.0) * np.sign(np.sin(2 * np.pi * 60 * t))
               + 0.0005 * rng.standard_normal(n))
        rec[delay:] += ret / 0.3 * sig[:n - delay]
        lags = sllatency.measure_round_trip(rec.astype(np.float32), starts, click, SR,
                                            250.0, PERIOD - 2)
        med = float(np.median(lags)) if len(lags) else None
        ok = med is None or abs(med - delay) <= 1.0
        fails += not ok
        print("%s buzz %.3f, return %.3f: %s" % ("PASS" if ok else "FAIL", buzz, ret,
              "%d clicks, %.1f (expected %d)" % (len(lags), med, delay) if med is not None
              else "not measured (no wrong answer)"))
    return fails


def jackdbus_settings_tests():
    """jackdbus's settings are used only when it is running the server this
    tool is connected to (a registered but stopped jackdbus keeps its saved
    settings, which once showed in a run on another server)."""
    import types
    fails = 0
    dp = ("--- get driver parameters\n"
          "              period: Frames per period (uint:set:1024:256)\n"
          "            nperiods: Number of periods (uint:set:2:3)\n"
          "       input-latency: Extra input latency (frames) (uint:set:0:343)\n"
          "      output-latency: Extra output latency (frames) (uint:set:0:343)\n")
    real_run = sllatency.subprocess.run
    saved_env = os.environ.get("JACK_DEFAULT_SERVER")
    def fake(status):
        def run(cmd, **kw):
            out = {"dbus-send": "   boolean true", "status": "--- status\n" + status,
                   "dp": dp}[cmd[0] if cmd[0] == "dbus-send" else cmd[1]]
            return types.SimpleNamespace(stdout=out)
        return run
    cases = [("stopped", "stoped", 256, None, {}),
             ("started, other period", "started", 1024, None, {}),
             ("started, other server named", "started", 256, "sltest", {}),
             ("started, same period", "started", 256, None,
              {"period": 256, "nperiods": 3, "in_extra": 343, "out_extra": 343})]
    try:
        for name, status, bs, server, want in cases:
            sllatency.subprocess.run = fake(status)
            if server:
                os.environ["JACK_DEFAULT_SERVER"] = server
            else:
                os.environ.pop("JACK_DEFAULT_SERVER", None)
            got = sllatency.jack_settings(bs)
            ok = got == want
            fails += not ok
            print("%s jackdbus settings, %s: %s" % ("PASS" if ok else "FAIL", name,
                                                   "used" if got else "ignored"))
    finally:
        sllatency.subprocess.run = real_run
        if saved_env is None:
            os.environ.pop("JACK_DEFAULT_SERVER", None)
        else:
            os.environ["JACK_DEFAULT_SERVER"] = saved_env
    return fails


def jack_report_tests():
    """JACK's result section, with jackdbus-style settings (the test server
    is plain jackd, which never reports them): absolute new values, the
    correction shared evenly, the buffers part kept."""
    import io, contextlib
    js = {"period": 256, "nperiods": 3, "in_extra": 343, "out_extra": 343}
    fails = 0
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        sllatency.jack_report(js, 599, 855, 1454 + 101.0, 1.0, SR)
    text = out.getvalue()
    new = [l.split() for l in text.splitlines() if l.strip().startswith("extra ")]
    got = [(int(r[-2]), int(r[-1])) for r in new]
    buffers = [l.split() for l in text.splitlines() if l.strip().startswith(("capture", "playback"))]
    ok = (got == [(343, 393), (343, 394)] and "not amounts" in text
          and [int(b[2]) for b in buffers] == [256, 512])
    fails += not ok
    print("%s JACK advice as absolute values, correction split evenly: %s" %
          ("PASS" if ok else "FAIL", got))
    if not ok:
        print(text)
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        sllatency.jack_report(js, 599, 855, 1458.0, 1.0, SR)
    ok = "nothing to change" in out.getvalue() and "restart JACK" not in out.getvalue()
    fails += not ok
    print("%s JACK within tolerance: nothing to change" % ("PASS" if ok else "FAIL"))
    return fails


def suggest_tests():
    """Output suggestion, from JACK's routing (no server needed)."""
    import io, contextlib
    fails = 0
    # Checking SooperLooper: suggest the outputs its loop is heard through,
    # followed through a DAW bus (this rig, 2026-09-26) or wired directly.
    rig2 = {"system:playback_1": ["ardour:Master/audio_out 1"],
            "system:playback_2": ["ardour:Master/audio_out 2"],
            "system:playback_3": ["ardour:SL_vol_mon/audio_out 1"],
            "system:playback_4": ["ardour:SL_vol_mon/audio_out 2"]}
    dests = ["ardour:SL_vol_mon/audio_in 1", "ardour:sooperlooper1/audio_in 1",
             "ardour:SL_vol_mon/audio_in 2", "ardour:sooperlooper2/audio_in 1"]
    direct = {"system:playback_1": ["ardour:Master/audio_out 1"],
              "system:playback_2": ["sooperlooper:loop0_out_1"]}
    for feeds, d, want in ((rig2, dests, ["system:playback_3", "system:playback_4"]),
                           (direct, ["system:playback_2"], ["system:playback_2"]),
                           (rig2, ["ardour:elsewhere/audio_in 1"], [])):
        got = sllatency.sl_heard_outputs(feeds, d)
        ok = got == want
        fails += not ok
        print("%s outputs SooperLooper is heard through: %s" % ("PASS" if ok else "FAIL", got))
    # No questions: the traced outputs, or no default at all (exit asking
    # for --out) -- never a guess from a master bus or the busiest outputs.
    real = sllatency.output_feeds
    try:
        for feeds, d, want in ((rig2, dests, ["system:playback_3", "system:playback_4"]),
                               (rig2, ["ardour:elsewhere/audio_in 1"], SystemExit),
                               (rig2, [], SystemExit)):
            sllatency.output_feeds = lambda client, f=feeds: f
            try:
                with contextlib.redirect_stdout(io.StringIO()):
                    got = sllatency.choose_outputs(None, False, d)
            except SystemExit as e:
                got = SystemExit
                msg = str(e)
            ok = got == want and (got != SystemExit or "--out" in msg)
            fails += not ok
            print("%s outputs with no questions: %s" % ("PASS" if ok else "FAIL",
                  "exit asking for --out" if got == SystemExit else got))
    finally:
        sllatency.output_feeds = real
    return fails


# ------------------------------------------------------------ --trigger

def synth_presses(n, il, late, sr=SR, spacing=0.9, level=0.5, noise=0.002,
                  foot=0.1, scatter=0, gesture=(), seed=5, strike=0.0, bounce=0.0):
    """Synthetic mic audio and command frames for n presses: each press's
    click (a sharp broadband burst) at press + il, after a softer foot
    sound (foot x level, broadband, 12 to 3 ms before it) and a low thump;
    its command at press + late (+ up to +/-scatter, + 300 ms for the
    presses in `gesture`).  strike: a sharp burst of strike x level 28 ms
    before the click (a finger or foot hitting the cap, as on the rig);
    bounce: one of bounce x level 3 ms after it.  Returns (audio,
    [(frame, 0, bytes)], clicks)."""
    rng = np.random.default_rng(seed)
    total = int((n * spacing + 2.0) * sr)
    audio = (noise * rng.standard_normal(total)).astype(np.float64)
    k = np.arange(int(0.003 * sr))
    t = np.arange(int(0.02 * sr))
    events, clicks = [], []
    for i in range(n):
        press = int((1.0 + i * spacing) * sr) + int(rng.integers(0, 256))
        click = press + il
        clicks.append(click)
        a = click - int(0.012 * sr)
        audio[a:a + int(0.009 * sr)] += foot * level * rng.standard_normal(int(0.009 * sr))
        th = click - int(0.006 * sr)
        audio[th:th + len(t)] += 2 * level * np.sin(2 * np.pi * 80 * t / sr) * np.exp(-t / (0.005 * sr))
        audio[click:click + len(k)] += level * rng.standard_normal(len(k)) * np.exp(-k / (0.0008 * sr))
        for at, lvl in ((click - int(0.028 * sr), strike), (click + int(0.003 * sr), bounce)):
            if lvl:
                audio[at:at + len(k)] += (lvl * level * rng.standard_normal(len(k))
                                          * np.exp(-k / (0.0008 * sr)))
        cmd = press + late + (int(rng.integers(-scatter, scatter + 1)) if scatter else 0)
        if i in gesture:
            cmd += int(0.3 * sr)
        events.append((cmd, 0, bytes([0xB0, 22, 127])))
        events.append((cmd + int(0.15 * sr), 0, bytes([0xB0, 22, 0])))   # release
    return audio.astype(np.float32), events, clicks


class FakePedal:
    """A pedal with a mic against it: plays synth_presses' audio on "mic"
    and sends their commands on "cmd", frame-exact, from its first cycle."""

    def __init__(self, il, late, n=12, spacing=0.5, start=3.0, name="fakepedal"):
        self.c = jack.Client(name, no_start_server=True)
        self.ao = self.c.outports.register("mic")
        self.mo = self.c.midi_outports.register("cmd")
        audio, events, _ = synth_presses(n, il, late, spacing=spacing)
        lead = int((start - 1.0) * SR)          # synth_presses starts at 1 s
        self.audio = np.concatenate([np.zeros(lead, dtype=np.float32), audio])
        self.events = sorted((f + lead, d) for f, _, d in events)
        self.f0 = None
        self.c.set_process_callback(self.process)

    def process(self, frames):
        f = self.c.last_frame_time
        if self.f0 is None:
            self.f0 = f
        p = f - self.f0
        buf = self.ao.get_array()
        seg = self.audio[p:p + frames]
        buf[:len(seg)] = seg
        buf[len(seg):] = 0.0
        self.mo.clear_buffer()
        for fr, d in self.events:
            if p <= fr < p + frames:
                self.mo.write_midi_event(fr - p, d)


def run_trigger_tool(sl_port, client, late, il, extra=(), answers=None):
    """--trigger through a FakePedal wired to SooperLooper's midi_in;
    returns (measured trigger latency or None, report)."""
    import tempfile
    pedal = FakePedal(il, late)
    pedal.c.activate()
    pedal.c.connect("fakepedal:cmd", "%s:midi_in" % client)
    cmd = [sys.executable, os.path.join(HERE, "sllatency.py"), "--trigger",
           "--in", "fakepedal:mic", "--presses", "8", "--seconds", "20",
           "--sl-port", str(sl_port), "--sl-client", client] + list(extra)
    if answers is None:
        cmd.append("--yes")
    try:
        out = subprocess.run(cmd, input=answers, capture_output=True, text=True, timeout=60,
                             env=dict(os.environ, HOME=tempfile.mkdtemp(prefix="sllat_home_"))
                             ).stdout
    finally:
        pedal.c.deactivate()
        pedal.c.close()
    tl = None
    for line in out.splitlines():
        if "Trigger latency:" in line:
            tl = int(line.split("Trigger latency:")[1].split()[0])
    return tl, out


def trigger_tool_tests(sl, port, client, tmp, session, il):
    """The whole --trigger run on the dummy server: it must find the
    pedal's port on SooperLooper's midi_in, read input_latency from
    SooperLooper, measure the known lateness, and apply and save it (the
    file changed only in trigger_latency and auto_latency)."""
    import glob
    fails = 0
    before = sl.get("trigger_latency", loop=0)
    late = 685
    import re
    tl, report = run_trigger_tool(port, client, late, int(il))
    rows = len(re.findall(r"(?m)^\s+\d+\s+-?\d+\s+-?[\d.]+ ms", report))
    ok = (tl is not None and abs(tl - late) <= 4 and "fakepedal:cmd" in report
          and rows == 8 and sl.get("trigger_latency", loop=0) == before)
    fails += not ok
    print("%s --trigger, lateness %d, IL %d: measured %s from %d presses (asked 8; the "
          "pedal sends 12), report only (trigger_latency still %s)"
          % ("PASS" if ok else "FAIL", late, il, tl, rows, sl.get("trigger_latency", loop=0)))
    if not ok:
        print(report)

    path = _session_file(sl, tmp, session, "trigger.slsess")
    orig = open(path).read()
    tl, report = run_trigger_tool(port, client, 900, int(il),
                                  ["--apply", "--save", "--session-file", path])
    now = open(path).read()
    changed = [(a, b) for a, b in zip(orig.splitlines(), now.splitlines()) if a != b]
    live = sl.get("trigger_latency", loop=0)
    ok = (tl is not None and abs(tl - 900) <= 4 and live == float(tl)
          and sl.get("autoset_latency", loop=0) == 0.0
          and ('name="trigger_latency" value="%d"' % tl) in now
          and all("trigger_latency" in a or "auto_latency" in a for a, _ in changed)
          and bool(glob.glob(path + ".bak-*")))
    fails += not ok
    print("%s --trigger --apply --save, lateness 900: measured %s, live %s, file changed "
          "only in %d trigger/auto line(s), backup kept"
          % ("PASS" if ok else "FAIL", tl, live, len(changed)))
    if not ok:
        print(report)

    # Interactive: Enter to start, y to apply, y to save, the file path.
    path2 = _session_file(sl, tmp, session, "trigger_i.slsess")
    tl, report = run_trigger_tool(port, client, 500, int(il), answers="\ny\ny\n%s\n" % path2)
    ok = (tl is not None and abs(tl - 500) <= 4 and sl.get("trigger_latency", loop=0) == float(tl)
          and ('name="trigger_latency" value="%d"' % tl) in open(path2).read()
          and "Apply trigger_latency" in report)
    fails += not ok
    print("%s --trigger interactive apply and save, lateness 500: measured %s"
          % ("PASS" if ok else "FAIL", tl))
    if not ok:
        print(report)
    sl.set("trigger_latency", before, loop=0)

    # The mic choice lists capture ports only (the rig run listed the
    # playback ports too, and choosing one failed to connect).
    c = jack.Client("portlist", no_start_server=True)
    caps = sllatency.physical_ports(c, is_input=False)
    plays = sllatency.physical_ports(c, is_input=True)
    c.close()
    ok = (caps and plays and all(":capture_" in p for p in caps)
          and all(":playback_" in p for p in plays))
    fails += not ok
    print("%s physical ports: capture %s, playback %s" % ("PASS" if ok else "FAIL", caps, plays))

    # No command at all (on the rig: the pedal's MIDI not arriving): a
    # clear message, not "not enough clear presses".
    q = jack.Client("silentpedal", no_start_server=True)
    q.outports.register("mic")
    cmd = q.midi_outports.register("cmd")
    # A client with no process callback stalls the recorder's (seen on the
    # dummy server); this one sends nothing.
    q.set_process_callback(lambda frames: cmd.clear_buffer())
    q.activate()
    try:
        r = subprocess.run([sys.executable, os.path.join(HERE, "sllatency.py"), "--trigger",
                            "--in", "silentpedal:mic", "--midi", "silentpedal:cmd",
                            "--seconds", "2", "--yes", "--sl-port", str(port),
                            "--sl-client", client],
                           capture_output=True, text=True, timeout=30)
    finally:
        q.deactivate()
        q.close()
    ok = r.returncode == 1 and "No press command came from silentpedal:cmd" in r.stdout
    fails += not ok
    print("%s --trigger with no commands: exit %d, says none came"
          % ("PASS" if ok else "FAIL", r.returncode))
    if not ok:
        print(r.stdout, r.stderr)
    return fails


def trigger_analysis_tests():
    """Offline: the --trigger analysis on synthetic presses."""
    fails = 0

    def check(ok, msg):
        nonlocal fails
        fails += not ok
        print("%s %s" % ("PASS" if ok else "FAIL", msg))

    # Which messages are presses, and one press per cluster.
    # Only messages like the first press count (note 60 on channel 1 here).
    ev = [(100, 0, bytes([0x90, 60, 100])), (120, 1, bytes([0x90, 60, 90])),   # same press
          (5000, 0, bytes([0x80, 60, 0])), (6000, 0, bytes([0x90, 60, 0])),     # releases
          (9000, 0, bytes([0xB0, 1, 127])),                  # another message: ignored
          (12000, 0, bytes([0x91, 60, 100])),                # other channel: ignored
          (20000, 1, bytes([0x90, 60, 80])), (20100, 0, bytes([0xC0, 3]))]
    got = sllatency.press_commands(ev, SR)
    check(got == [(100, 0), (20000, 1)],
          "press grouping: %s (expected [(100, 0), (20000, 1)])" % got)
    check(sllatency.describe_key(sllatency.press_key(ev)) == "note 60 on channel 1"
          and sllatency.describe_key((0xB4, 13)) == "controller 13 on channel 5",
          "press key described: %s" % sllatency.describe_key(sllatency.press_key(ev)))

    # A long-press action (another controller, 400 ms after some presses,
    # as the rig's record button sends when held): not counted.
    audio, ev, _ = synth_presses(20, 743, 685)
    ev = ev + [(f + int(0.4 * SR), 0, bytes([0xB0, 65, 127]))
               for f, _, d in ev[::4] if d[2]]
    cmds = sllatency.press_commands(ev, SR)
    rows = sllatency.analyse_presses(audio, SR, cmds, 743)
    med, kept, out = sllatency.trigger_estimate(rows, SR)
    check(len(cmds) == 20 and med is not None and abs(med - 685) <= 4 and len(kept) == 20,
          "long-press messages ignored: %d presses, median %s, %d kept" % (len(cmds), med,
                                                                           len(kept)))

    for il, late, scatter in ((743, 685, 0), (743, 685, 40), (256, 300, 0), (1200, 1500, 20)):
        audio, ev, _ = synth_presses(20, il, late, scatter=scatter)
        rows = sllatency.analyse_presses(audio, SR, sllatency.press_commands(ev, SR), il)
        med, kept, out = sllatency.trigger_estimate(rows, SR)
        # The onset is where a 0.25 ms (centred) envelope rises through half
        # its peak: within a few samples of the burst's start.  With
        # scatter, the median of 20 draws moves too.
        ok = med is not None and abs(med - late) <= 4 + scatter / 3.0 and len(kept) >= 18
        check(ok, "IL %d, lateness %d, scatter +/-%d: median %s, %d kept, statuses %s"
              % (il, late, scatter, med, len(kept),
                 sorted(set(r["status"] for r in rows))))

    audio, ev, _ = synth_presses(20, 743, 685, gesture=(3, 11))
    rows = sllatency.analyse_presses(audio, SR, sllatency.press_commands(ev, SR), 743)
    med, kept, out = sllatency.trigger_estimate(rows, SR)
    flagged = [i for i, r in enumerate(rows) if r["status"] != "ok"]
    check(med is not None and abs(med - 685) <= 4 and flagged == [3, 11]
          and all(rows[i]["status"] in ("late", "weak") for i in flagged),
          "gesture-delayed presses 3 and 11: flagged %s (%s), median %s"
          % (flagged, [rows[i]["status"] for i in flagged], med))

    # Without the half-peak rule the foot sound (0.1 of the click) would be
    # taken; with a foot sound at 0.4 the onset must still be the click.
    audio, ev, _ = synth_presses(20, 743, 685, foot=0.4)
    rows = sllatency.analyse_presses(audio, SR, sllatency.press_commands(ev, SR), 743)
    med, kept, out = sllatency.trigger_estimate(rows, SR)
    check(med is not None and abs(med - 685) <= 4,
          "louder foot sound (0.4 of the click): median %s (expected about 685)" % med)

    # The rig's hand presses (2026-11-06): the finger hitting the cap 25-35
    # ms before the snap, up to twice as loud; a small bounce after it.
    # The loudest-sound rule took the strike on half of them (~20 ms high).
    # At twice the click, the random bursts put a few clicks under 0.3 of
    # their strike (as one rig press was); those must come out flagged
    # late, not in the median.
    audio, ev, _ = synth_presses(20, 743, 685, strike=2.0, bounce=0.15)
    rows = sllatency.analyse_presses(audio, SR, sllatency.press_commands(ev, SR), 743)
    med, kept, out = sllatency.trigger_estimate(rows, SR)
    rest = [r["status"] for r in rows if r not in kept]
    check(med is not None and abs(med - 685) <= 4 and len(kept) >= 16
          and all(s == "late" for s in rest),
          "strike 2x the click 28 ms before it, bounce 0.15 after: median %s, %d kept, "
          "others %s (expected about 685, 16 or more, late)" % (med, len(kept), rest))

    audio, ev, _ = synth_presses(20, 743, 685, level=0.006)
    rows = sllatency.analyse_presses(audio, SR, sllatency.press_commands(ev, SR), 743)
    med, kept, out = sllatency.trigger_estimate(rows, SR)
    weak = sum(r["status"] in ("weak", "unclear") for r in rows)
    check(med is None and weak >= 15,
          "faint clicks: %d of 20 weak or unclear, no estimate (median %s)" % (weak, med))
    return fails


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--engine", default=os.path.join(HERE, "..", "src", "sooperlooper"))
    ap.add_argument("--session", help="SooperLooper session file to load a copy of")
    args = ap.parse_args()
    os.environ["JACK_DEFAULT_SERVER"] = SERVER
    os.environ["JACK_NO_START_SERVER"] = "1"
    jackd = subprocess.Popen(["jackd", "-n", SERVER, "-d", "dummy", "-r", str(SR),
                              "-p", str(PERIOD)],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    fails = (suggest_tests() + jack_report_tests() + jackdbus_settings_tests()
             + buzz_tests() + session_save_tests() + trigger_analysis_tests())
    try:
        time.sleep(2.0)
        cases = [
            (0, None, 0.0, 0.5),
            (100, None, 0.0, 0.5),
            (1486, None, 0.0, 0.5),         # about the rig's 743 + 743
            (1486, 4000.0, 0.0, 1.5),       # blurred like a mic (the filter itself adds ~1 sample)
            (1486, 4000.0, 0.003, 1.5),     # blurred and noisy
        ]
        for delay, lp, noise, tol in cases:
            lags, xruns = run_case(delay, lp, noise)
            expect = delay + PERIOD
            med = float(np.median(lags)) if len(lags) else float("nan")
            ok = len(lags) >= 6 and abs(med - expect) <= tol and xruns == 0
            fails += not ok
            print("%s delay %5d lowpass %-6s noise %.3f: %2d clicks, median %8.2f, "
                  "expected %d (+/-%.1f), xruns %d"
                  % ("PASS" if ok else "FAIL", delay, lp, noise, len(lags), med,
                     expect, tol, xruns))
        fails += chain_test()
        fails += sl_tests(args.engine, args.session)
    finally:
        jackd.terminate()
        jackd.wait(timeout=5)
    print("all passed" if not fails else "%d failed" % fails)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
