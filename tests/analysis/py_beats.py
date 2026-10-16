#!/usr/bin/env python3
# Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md.
"""Runs dj's Python beat analyser over a list of files and writes a CSV
with the columns cyanbox-eval writes, so `cyanbox-eval --compare` can measure
how the two analysers agree.

Usage (any python3; it re-executes itself under dj's venv):

    tests/analysis/py_beats.py [-j JOBS] [--bpm-range LO HI] -o dj.csv FILE... | -l LIST

Only the beat stage of dj runs (log-mel + analyse_beats, as
dj.analysis.analyse_samples does it, without the key), on audio decoded
exactly as dj decodes it: ffmpeg to 44.1 kHz mono f32. Paths are handed to
ffmpeg with a "file:" prefix so names containing ':' ("AD:HOUSE 11/...")
are not read as a protocol.
"""

import argparse
import csv
import os
import subprocess
import sys
import time

DJ = os.path.expanduser("~/projects/dj")
DJ_PYTHON = os.path.join(DJ, ".venv/bin/python")

COLUMNS = ["path", "duration", "bpm", "sections", "downbeat", "confidence",
           "flags", "first_onset", "ms", "tag_bpm"]


def _reexec_in_venv():
    if os.path.realpath(sys.prefix) != os.path.realpath(os.path.join(DJ, ".venv")) \
            and os.path.exists(DJ_PYTHON) and not os.environ.get("PY_BEATS_REEXEC"):
        os.environ["PY_BEATS_REEXEC"] = "1"
        os.execv(DJ_PYTHON, [DJ_PYTHON, os.path.abspath(__file__)] + sys.argv[1:])


def _decode(path):
    import numpy as np
    out = subprocess.run(["ffmpeg", "-v", "error", "-nostdin", "-i", "file:" + path,
                          "-map", "0:a:0", "-ac", "1", "-ar", "44100", "-f", "f32le", "-"],
                         capture_output=True)
    if out.returncode:
        raise RuntimeError(out.stderr.decode(errors="replace").strip())
    return np.frombuffer(out.stdout, dtype=np.float32)


def _one(job):
    path, bpm_range = job
    sys.path.insert(0, os.path.join(DJ, "src"))
    for var in ("OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS"):
        os.environ.setdefault(var, "1")
    from dj.analysis import FPS, RATE, log_mel, spectrum
    from dj.analysis.beats import analyse_beats
    spectrum.FFT_WORKERS = 1
    try:
        x = _decode(path)
        t0 = time.process_time()
        mel = log_mel(x)
        b = analyse_beats(x, RATE, mel, FPS, bpm_range=bpm_range)
        ms = (time.process_time() - t0) * 1000.0
    except Exception as e:      # noqa: BLE001 - one bad file must not stop the run
        print(f"{path}: {e}", file=sys.stderr)
        return None
    dur = len(x) / RATE
    s = b.grid.sections[0]
    bar = 4 * 60.0 / s.bpm
    down = s.start - bar * int(s.start // bar)
    return {
        "path": path,
        "duration": f"{dur:.3f}",
        "bpm": f"{s.bpm:.4f}",
        "sections": f"{s.start:.5f}:{s.bpm:.4f}:1",
        "downbeat": f"{down:.5f}",
        "confidence": f"{b.confidence:.3f}",
        "flags": "",
        "first_onset": f"{b.first_onset:.3f}",
        "ms": f"{ms:.0f}",
        "tag_bpm": "",
    }


def main():
    _reexec_in_venv()
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("-l", "--list", help="file with one path per line")
    ap.add_argument("-j", "--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    ap.add_argument("--bpm-range", nargs=2, type=float, metavar=("LO", "HI"))
    ap.add_argument("files", nargs="*")
    a = ap.parse_args()
    paths = list(a.files)
    if a.list:
        with open(a.list, encoding="utf-8") as f:
            paths += [ln.rstrip("\n") for ln in f if ln.strip()]
    bpm_range = tuple(a.bpm_range) if a.bpm_range else None

    from concurrent.futures import ProcessPoolExecutor
    rows = []
    with ProcessPoolExecutor(max_workers=a.jobs) as ex:
        for i, r in enumerate(ex.map(_one, [(p, bpm_range) for p in paths]), 1):
            if r:
                rows.append(r)
                print(f"[{i}/{len(paths)}] {r['bpm']} {os.path.basename(r['path'])}",
                      file=sys.stderr)
    with open(a.output, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=COLUMNS)
        w.writeheader()
        w.writerows(rows)


if __name__ == "__main__":
    main()
