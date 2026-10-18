#!/usr/bin/env python3
"""Measure the time offset between recorded tracks from their onsets.

For the latency calibration in sooperlooper_and_ardour_setup_guide.rst
§2.7: with the headphones on the guitar mic, one Ardour take holds
Hydrogen's drums recorded directly (the reference), the guitar-mic
signal (the drums after the full output -> analog -> input chain) and
SooperLooper's loop output.  All three should line up; this reports how
far each target lags the reference.

Method: each track is reduced to an onset-strength signal (the rise of
a 1 ms RMS envelope), and the reference and target signals are
cross-correlated over a limited lag range.  The peak gives the offset;
it is refined to a fraction of a sample-block by parabolic
interpolation.  A positive result means the target is late.  Several
reference files (e.g. kick and snare) are summed.

The lag range must stay under half the spacing of the pattern's
hits, or a neighboring hit can win; the report shows the runner-up
peak so an ambiguous result is visible.

Usage:
    measure_track_offsets.py --ref DRUM.flac [--ref DRUM2.flac ...]
        [--from S] [--to S] [--max-lag MS] TARGET.flac [TARGET2.flac ...]

--from/--to limit the comparison to part of the take (recording seconds).
Needs ffmpeg and numpy.
"""
import argparse
import subprocess

import numpy as np

SR = 48000
DEC = 6                      # onset signal at 8 kHz: 0.125 ms blocks


def decode(path):
    raw = subprocess.run(
        ["ffmpeg", "-loglevel", "error", "-i", path, "-ac", "1",
         "-ar", str(SR), "-f", "f32le", "-acodec", "pcm_f32le", "-"],
        check=True, stdout=subprocess.PIPE).stdout
    return np.frombuffer(raw, dtype=np.float32).astype(np.float64)


def onset_strength(x):
    env = np.sqrt(np.convolve(x * x, np.ones(48) / 48.0, mode="same"))
    flux = np.maximum(np.diff(env, prepend=env[0]), 0.0)
    n = len(flux) // DEC * DEC
    flux = flux[:n].reshape(-1, DEC).sum(axis=1)
    return flux / (np.linalg.norm(flux) + 1e-12)


def xcorr_peak(ref, tgt, max_lag):
    """Lag (in onset-signal blocks) maximizing sum ref[t] * tgt[t + lag]."""
    n = 1 << int(np.ceil(np.log2(len(ref) + len(tgt))))
    c = np.fft.irfft(np.conj(np.fft.rfft(ref, n)) * np.fft.rfft(tgt, n), n)
    lags = np.arange(-max_lag, max_lag + 1)
    vals = c[lags % n]
    i = int(np.argmax(vals))
    # parabolic refinement
    frac = 0.0
    if 0 < i < len(vals) - 1:
        a, b, d = vals[i - 1], vals[i], vals[i + 1]
        den = a - 2 * b + d
        if den != 0:
            frac = 0.5 * (a - d) / den
    # runner-up peak at least 2 ms away
    mask = np.abs(lags - lags[i]) > int(0.002 * SR / DEC)
    j = int(np.argmax(np.where(mask, vals, -np.inf)))
    return lags[i] + frac, vals[i], lags[j], vals[j]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--ref", action="append", required=True)
    ap.add_argument("--from", dest="t_from", type=float, default=0.0)
    ap.add_argument("--to", dest="t_to", type=float, default=None)
    ap.add_argument("--max-lag", type=float, default=150.0,
                    help="largest offset searched, ms (default 150)")
    ap.add_argument("targets", nargs="+")
    args = ap.parse_args()

    ref = sum(decode(p) for p in args.ref)
    a = int(args.t_from * SR)
    b = int(args.t_to * SR) if args.t_to else len(ref)
    r = onset_strength(ref[a:b])
    max_lag = int(args.max_lag / 1000.0 * SR / DEC)
    for p in args.targets:
        t = onset_strength(decode(p)[a:b])
        m = min(len(r), len(t))
        lag, peak, lag2, peak2 = xcorr_peak(r[:m], t[:m], max_lag)
        ms = lag * DEC / SR * 1000.0
        print("%s: %+.2f ms (%+.0f samples) late vs reference; "
              "peak %.3f, runner-up %.3f at %+.1f ms"
              % (p.rsplit("/", 1)[-1], ms, lag * DEC, peak, peak2,
                 lag2 * DEC / SR * 1000.0))


if __name__ == "__main__":
    main()
