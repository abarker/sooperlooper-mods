#!/usr/bin/env python3
"""Write the timeline diagrams for mod_docs/latency_and_trigger_latency.rst.

Usage:
    latency_diagrams.py [OUTDIR]

OUTDIR defaults to ../images next to this script.  Writes three SVG files:

* latency_press_timeline.svg -- one press: the command and the note's
  sound reaching SooperLooper by different paths, and the output delay;
* latency_recording_edges.svg -- where each recording edge lands around
  the press;
* latency_first_pass_gap.svg -- after a record stop, the part of the loop
  that cannot be heard on the first pass.

Positions are computed from the example figures below, so the pictures
stay to scale if they change.  The style follows images/sync_boundaries.svg.
"""
import os
import sys

# Example setup (samples at 48 kHz).
PERIOD = 256
CL = 685      # command latency: press to the engine acting on it
IL = 743     # audio input latency setting
OL = 743     # audio output latency setting
TL_DEFAULT = PERIOD

FONT = 'font-family="DejaVu Sans, Arial, Helvetica, sans-serif" font-size="13"'
INK, MUTED, FAINT = "#27272a", "#52525b", "#d4d4d8"
BLUE, BLUE_D, BLUE_L, BLUE_LL = "#2563eb", "#1d4ed8", "#bfdbfe", "#dbeafe"
ORANGE, ORANGE_D, ORANGE_L, ORANGE_LL = "#ea580c", "#c2410c", "#fed7aa", "#ffedd5"
GREEN, GREEN_D, GREEN_L = "#15803d", "#14532d", "#bbf7d0"
RED, RED_L = "#b91c1c", "#fee2e2"
MINUS = "−"


class Svg:
    def __init__(self, w, h):
        self.w, self.h, self.parts = w, h, []
        self.add('<rect x="0" y="0" width="%d" height="%d" fill="#ffffff"/>' % (w, h))
        self.add('<defs>'
                 '<marker id="arr" markerWidth="10" markerHeight="8" refX="9" refY="4" '
                 'orient="auto" markerUnits="userSpaceOnUse"><path d="M0,0 L10,4 L0,8 z" fill="%s"/></marker>'
                 '<marker id="arrs" markerWidth="10" markerHeight="8" refX="1" refY="4" '
                 'orient="auto" markerUnits="userSpaceOnUse"><path d="M10,0 L0,4 L10,8 z" fill="%s"/></marker>'
                 '</defs>' % (MUTED, MUTED))

    def add(self, s):
        self.parts.append("  " + s)

    def text(self, x, y, s, fill=INK, anchor="start", size=None, bold=False, italic=False):
        attrs = 'x="%.1f" y="%.1f" fill="%s" text-anchor="%s"' % (x, y, fill, anchor)
        if size:
            attrs += ' font-size="%d"' % size
        if bold:
            attrs += ' font-weight="bold"'
        if italic:
            attrs += ' font-style="italic"'
        s = s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
        self.add("<text %s>%s</text>" % (attrs, s))

    def line(self, x1, y1, x2, y2, stroke=MUTED, width=1, dash=None, end=False, start=False):
        a = 'x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="%s" stroke-width="%s"' % (
            x1, y1, x2, y2, stroke, width)
        if dash:
            a += ' stroke-dasharray="%s"' % dash
        if end:
            a += ' marker-end="url(#arr)"'
        if start:
            a += ' marker-start="url(#arrs)"'
        self.add("<line %s/>" % a)

    def rect(self, x, y, w, h, fill, stroke=None):
        a = 'x="%.1f" y="%.1f" width="%.1f" height="%.1f" fill="%s"' % (x, y, w, h, fill)
        if stroke:
            a += ' stroke="%s"' % stroke
        self.add("<rect %s/>" % a)

    def dot(self, x, y, fill, r=6):
        self.add('<circle cx="%.1f" cy="%.1f" r="%d" fill="%s"/>' % (x, y, r, fill))

    def lane(self, y, x0, x1, title, sub):
        self.line(x0, y, x1, y, stroke=INK, width=2, end=True)
        self.text(12, y - 3, title, bold=True)
        self.text(12, y + 13, sub, fill=MUTED, size=12)

    def bar(self, x0, x1, y, h, fill, stroke, label, label_fill):
        self.rect(x0, y, x1 - x0, h, fill, stroke)
        self.text((x0 + x1) / 2, y + h / 2 + 4.5, label, fill=label_fill, anchor="middle", size=12)

    def write(self, path):
        head = ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
                'viewBox="0 0 %d %d" %s>' % (self.w, self.h, self.w, self.h, FONT))
        with open(path, "w", encoding="utf-8") as f:
            f.write("\n".join([head] + self.parts + ["</svg>"]) + "\n")


def scale(lo, hi, x0, x1):
    k = (x1 - x0) / float(hi - lo)
    return lambda s: x0 + (s - lo) * k


def press_timeline(path):
    """One press at P: the command (CL) and the note's sound (IL) reach
    SooperLooper at different times; its output is heard OL later."""
    W, H = 880, 370
    x = scale(-150, 1650, 190, 860)
    yP, ySL = 60, 248
    s = Svg(W, H)
    P, A, S, Hd = x(0), x(CL), x(IL), x(CL + OL)

    # guides through both lanes
    for gx in (P, A, S):
        s.line(gx, yP, gx, ySL, stroke=FAINT, dash="4,4")

    s.lane(yP, 190, 860, "Performer", "played and heard")
    s.lane(ySL, 190, 860, "SooperLooper", "JACK's timeline at its ports")

    s.dot(P, yP, GREEN)
    s.text(P, yP - 14, "P: foot down, note struck", fill=GREEN_D, anchor="middle")
    s.dot(Hd, yP, BLUE)
    s.text(Hd + 6, yP - 14, "earliest a response is heard: P + CL + OL", fill=BLUE_D, anchor="end")

    # the three delays as bars, each split into its parts
    y = 104
    s.text(12, y + 14, "CL = %d" % CL, bold=True, fill=ORANGE_D)
    s.text(96, y + 14, "command", fill=MUTED, size=12)
    s.bar(P, x(CL - PERIOD), y, 22, ORANGE_LL, ORANGE, "pedal, MIDI, USB", ORANGE_D)
    s.bar(x(CL - PERIOD), A, y, 22, ORANGE_L, ORANGE, "1 JACK period", ORANGE_D)

    y = 144
    s.text(12, y + 14, "IL = %d" % IL, bold=True, fill=BLUE_D)
    s.text(96, y + 14, "audio in", fill=MUTED, size=12)
    s.bar(P, x(IL - PERIOD), y, 22, BLUE_LL, BLUE, "converter, USB", BLUE_D)
    s.bar(x(IL - PERIOD), S, y, 22, BLUE_L, BLUE, "1 JACK period", BLUE_D)

    y = 184
    s.text(12, y + 14, "OL = %d" % OL, bold=True, fill=GREEN)
    s.text(96, y + 14, "audio out", fill=MUTED, size=12)
    s.bar(A, Hd, y, 22, "#dcfce7", GREEN, "JACK playback periods, USB, converter", GREEN_D)
    s.line(Hd, y, Hd, yP + 8, stroke=GREEN, width=1.5, end=True)

    s.dot(A, ySL, ORANGE)
    s.text(A - 6, ySL + 22, "A = P + CL: the command arrives", fill=ORANGE_D, anchor="end")
    s.dot(S, ySL, BLUE)
    s.text(S + 6, ySL + 22, "P + IL: the note's sound arrives", fill=BLUE_D)
    s.text(P, ySL + 22, "P", fill=MUTED, anchor="middle")

    # the overdub start's wait
    yb = ySL + 44
    s.line(A, yb, S, yb, stroke=INK, width=1.5)
    s.line(A, yb - 5, A, yb + 5, stroke=INK, width=1.5)
    s.line(S, yb - 5, S, yb + 5, stroke=INK, width=1.5)
    s.text(S + 10, yb + 4, "an overdub start waits IL %s trigger_latency after A," % MINUS, size=12)
    s.text(S + 10, yb + 21, "then takes input from the sound played at P", size=12)
    s.text(S + 10, yb + 38, "(exactly, when trigger_latency = CL)", fill=MUTED, size=12)
    s.write(path)


def recording_edges(path):
    """Where each recording edge lands, in played time, around a press at P."""
    W, H = 880, 300
    lo, hi = -200, 600
    x = scale(lo, hi, 330, 850)
    s = Svg(W, H)
    yA = 46
    P = x(0)

    s.line(x(lo), yA, x(hi), yA, stroke=INK, width=2, end=True)
    s.text(12, yA + 4, "Performer time", bold=True)
    for t in range(lo, hi + 1, 100):
        s.line(x(t), yA - 5, x(t), yA + 5, stroke=INK)
        lab = "P" if t == 0 else ("%+d" % t).replace("-", MINUS)
        s.text(x(t), yA - 10, lab, fill=MUTED if t else INK, anchor="middle", size=12,
               bold=(t == 0))
    s.line(P, yA, P, H - 18, stroke=GREEN, width=1.5, dash="5,4")
    s.text(P + 6, H - 8, "the press", fill=GREEN_D, size=12)

    edge = CL - IL
    rows = [
        ("Record start or stop", "acts at A, on input played IL before A", edge, ORANGE,
         "P %s %d (1.2 ms early)" % (MINUS, -edge)),
        ("Overdub, Replace, Substitute end", "the same, 1 sample more", edge - 1, ORANGE,
         "P %s %d" % (MINUS, -(edge - 1))),
        ("Overdub-type start,", "trigger_latency = %d (measured CL)" % CL, CL - CL, GREEN,
         "P (exact)"),
        ("Overdub-type start,", "trigger_latency = %d (one period, default)" % TL_DEFAULT,
         CL - TL_DEFAULT, RED, "P + %d (9 ms late)" % (CL - TL_DEFAULT)),
    ]
    y = yA + 48
    for title, sub, at, col, val in rows:
        s.line(x(lo), y, x(hi), y, stroke=FAINT)
        s.text(12, y - 2, title)
        s.text(12, y + 14, sub, fill=MUTED, size=12)
        s.dot(x(at), y, col)
        if at > 0:
            s.text(x(at) + 10, y - 9, val, fill=col, size=12)
        else:
            s.text(x(at) - 10, y - 9, val, fill=col, size=12, anchor="end")
        y += 52
    s.write(path)


def first_pass_gap(path):
    """After a record stop at Pe: when the repeat is due, and when it can
    first be heard."""
    W, H = 880, 290
    x = scale(-300, 2300, 190, 860)
    yP, ySL = 70, 230
    s = Svg(W, H)
    Pe, due, A, heard = x(0), x(CL - IL), x(CL), x(CL + OL)

    s.lane(yP, 190, 860, "Performer", "played and heard")
    s.lane(ySL, 190, 860, "SooperLooper", "JACK's timeline")

    yb = yP + 8
    s.rect(due, yb, heard - due, 22, RED_L)
    s.text((due + heard) / 2, yb + 15.5, "IL + OL: not heard on the first pass", fill=RED,
           anchor="middle", size=12)
    s.rect(heard, yb, 850 - heard, 22, "#dcfce7")
    s.text((heard + 850) / 2, yb + 15.5, "the loop, in phase", fill=GREEN_D, anchor="middle",
           size=12)

    s.line(due, yP - 6, due, yb + 22, stroke=RED, width=1.5)
    s.text(due - 6, yb + 44, "repeat due: Pe + (CL %s IL)" % MINUS, fill=RED, anchor="end",
           size=12)
    s.dot(Pe, yP, GREEN)
    s.text(Pe, yP - 16, "Pe: stop pressed", fill=GREEN_D, anchor="middle")
    s.dot(heard, yP, BLUE)
    s.text(heard, yP - 16, "earliest heard: Pe + CL + OL", fill=BLUE_D, anchor="middle")

    s.line(Pe, yP + 6, A - 4, ySL - 8, stroke=ORANGE, width=1.5, end=True)
    s.text((Pe + A) / 2 - 8, (yP + ySL) / 2 + 22, "CL", fill=ORANGE_D, anchor="end", bold=True)
    s.line(A + 4, ySL - 8, heard - 2, yb + 26, stroke=GREEN, width=1.5, end=True)
    s.text((A + heard) / 2 + 12, (yP + ySL) / 2 + 22, "OL", fill=GREEN, bold=True)

    s.dot(A, ySL, ORANGE)
    s.text(A, ySL + 26, "A: the stop arrives; playback starts IL + OL into the loop",
           fill=ORANGE_D, size=12)
    s.write(path)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "images")
    for name, fn in (("latency_press_timeline.svg", press_timeline),
                     ("latency_recording_edges.svg", recording_edges),
                     ("latency_first_pass_gap.svg", first_pass_gap)):
        p = os.path.join(out, name)
        fn(p)
        print("wrote", os.path.normpath(p))


if __name__ == "__main__":
    main()
