# Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md.

"""The key detector's evaluation (docs/KEY.md).

    gen_key_fixtures.py synth

writes tests/data/analysis/keys_synth.txt, the synthetic tracks test_key.c
generates and must name correctly: every key with each of the six
progressions of test_key.c (three major, three minor), each plain, detuned
(10..40 cents either way) and with a sparse bass, at rates and tempos drawn
from a fixed seed (mostly 44.1 and 48 kHz, house and drum & bass tempos).
The last column marks the quick subset `meson test` runs (two tonics of each
progression and variant, and the cases the previous detector got wrong or
mistuned by more than 3 cents); CYANBOX_KEY_FULL=1 test_key runs all of them.

    gen_key_fixtures.py fetch DIR

clones the GiantSteps key and GiantSteps MTG key datasets (annotations) into
DIR and downloads their audio, the two-minute Beatport previews, from the
JKU mirror the datasets' own audio_dl.sh uses (Beatport's server as the
fallback), checking each file against the dataset's md5.

    gen_key_fixtures.py gs DIR --set key|mtg --prog PROG [--out FILE]

runs a key program over a GiantSteps set: each preview is decoded with the
ffmpeg CLI to float32 mono at 44.1 kHz (what cb_analyse hands the key
detector) and piped to `PROG - 44100` (test_key's file mode, or anything that
prints a key name in dj's spelling, optionally followed by a confidence).
The answers go to FILE as a table; then the score: plain accuracy, the
MIREX weighted score (mir_eval's weighted_score: 1 same key, 0.5 a fifth
above, 0.3 relative, 0.2 parallel), the kinds of error, the accuracy on
major and on minor tracks, and the accuracy per confidence bin. "key" is the
GiantSteps key set (604 tracks, the standard EDM benchmark: the held-out
test); "mtg" the GiantSteps MTG key set (1486 tracks, of which those with a
single key annotated with confidence 2, the usual filter: the development
set every choice in key.c was tuned on).

    gen_key_fixtures.py features DIR --set key|mtg --prog TEST_KEY --out FILE.npz

decodes a set as above and keeps the detector's own chroma vectors of each
track and of its 20-second segments (`test_key --features`).

    gen_key_fixtures.py train FILE.npz

fits the key templates of src/analysis/key.c on those features (the
development set: GiantSteps MTG) and prints them as C, with the major-key
bias, the softmax temperature and the confidence calibration: a multinomial
logistic regression over the 24 keys whose weights are shared by all tonics
(each track's chroma rotated to every tonic), L2-regularised; the bias that
makes the detector name as many tracks major as the annotators did, the
temperature and calibration fitted on five-fold cross-validated answers,
so they describe tracks the templates have not seen.

    gen_key_fixtures.py score FILE...

prints the score of earlier result tables again.

    gen_key_fixtures.py real LIST --prog PROG [--work DIR]

the consistency check on real tracks without ground truth: every third
track of LIST (one path per line; 20 of build-check/acceptance/list.txt) is
decoded at its own rate and analysed whole, as its first and second halves,
at -20 dB, peak-normalised, and resampled to 44.1, 48 and 22.05 kHz. The key
must not depend on the level or the rate; the halves show how stable it is
over the track.
"""

import argparse
import hashlib
import random
import shlex
import subprocess
import sys
import tempfile
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np

DATA = Path(__file__).resolve().parent.parent / "data" / "analysis"

MAJOR = ["C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"]
MINOR = [k + "m" for k in MAJOR]

# test_key.c's PROGS: name, minor
PROGS = [("axis", False), ("twofive", False), ("royal", False),
         ("aeolian", True), ("minorfive", True), ("andalusian", True)]
VARIANTS = ["plain", "detuned", "sparse"]


# ------------------------------------------------------------------- synth

# Cases the previous detector (dj's port) named wrongly or mistuned by more
# than 3 cents, kept in the quick subset: it named seven aeolian loops by
# their sixth degree (Fm as Db), and read the tuning up to 7.6 cents off.
BEFORE_MISSES = {"aeolian-Fm-plain", "aeolian-F#m-plain", "aeolian-Gm-plain", "aeolian-Bbm-plain",
                 "aeolian-Fm-detuned", "aeolian-F#m-detuned", "aeolian-Bbm-detuned",
                 "twofive-D-plain", "minorfive-Fm-sparse", "minorfive-Am-sparse", "twofive-E-sparse"}


def synth():
    rng = random.Random(2026)
    lines = ["# name rate bpm tonic minor progression variant cents seed true-key quick",
             "# written by gen_key_fixtures.py synth; test_key.c synthesises each one"]
    for vi, variant in enumerate(VARIANTS):
        for pi, (prog, minor) in enumerate(PROGS):
            picks = {(5 * pi + 3 * vi) % 12, (5 * pi + 3 * vi + 7) % 12}
            for tonic in range(12):
                rate = rng.choices([44100, 48000, 22050, 96000], [12, 5, 2, 1])[0]
                bpm = rng.choice([118, 120, 122, 124, 126, 128, 130, 172, 174])
                cents = 0
                if variant == "detuned":
                    cents = rng.choice([-1, 1]) * rng.randint(10, 40)
                seed = rng.getrandbits(32)
                key = (MINOR if minor else MAJOR)[tonic]
                name = f"{prog}-{key}-{variant}"
                quick = tonic in picks or name in BEFORE_MISSES
                lines.append(f"{name} {rate} {bpm} {tonic} {int(minor)} {prog} {variant} {cents} {seed} {key} "
                             f"{int(quick)}")
    (DATA / "keys_synth.txt").write_text("\n".join(lines) + "\n")
    print(f"{len(lines) - 2} cases in {DATA / 'keys_synth.txt'}")


SETS = {
    # name: (repository, mirrors)
    "key": ("https://github.com/GiantSteps/giantsteps-key-dataset.git",
            ["https://www.cp.jku.at/datasets/giantsteps/backup/",
             "https://geo-samples.beatport.com/lofi/"]),
    "mtg": ("https://github.com/GiantSteps/giantsteps-mtg-key-dataset.git",
            ["https://www.cp.jku.at/datasets/giantsteps/mtg_key_backup/",
             "https://geo-samples.beatport.com/lofi/"]),
}


# ------------------------------------------------------------------- fetch

def _download(job):
    url_bases, md5, out = job
    if out.exists() and hashlib.md5(out.read_bytes()).hexdigest() == md5:
        return "ok"
    for base in url_bases:
        try:
            with urllib.request.urlopen(base + out.name, timeout=60) as r:
                data = r.read()
        except Exception:
            continue
        if hashlib.md5(data).hexdigest() == md5:
            out.write_bytes(data)
            return "ok"
    return "missing"


def fetch(root: Path):
    root.mkdir(parents=True, exist_ok=True)
    for name, (repo, mirrors) in SETS.items():
        src = root / f"gs-{name}"
        if not src.exists():
            subprocess.run(["git", "clone", "-q", "--depth", "1", repo, str(src)], check=True)
        audio = root / f"audio-{name}"
        audio.mkdir(exist_ok=True)
        jobs = []
        for m in sorted((src / "md5").glob("*.md5")):
            md5 = m.read_text().split()[0]
            jobs.append((mirrors, md5, audio / (m.stem + ".mp3")))
        with ThreadPoolExecutor(8) as pool:
            res = list(pool.map(_download, jobs))
        print(f"{name}: {res.count('ok')} of {len(jobs)} files, {res.count('missing')} missing")


# ----------------------------------------------------------------- scoring

PC = {"C": 0, "C#": 1, "Db": 1, "D": 2, "D#": 3, "Eb": 3, "E": 4, "F": 5, "F#": 6, "Gb": 6,
      "G": 7, "G#": 8, "Ab": 8, "A": 9, "A#": 10, "Bb": 10, "B": 11}


def parse_key(name):
    """(tonic, minor) from dj's spelling ("F#m", "Bb") or a dataset's
    ("A# minor", "e major"); None if it is neither."""
    name = name.strip()
    parts = name.split()
    if len(parts) == 2 and parts[1].lower() in ("major", "minor"):
        t = parts[0][:1].upper() + parts[0][1:]
        return (PC[t], parts[1].lower() == "minor") if t in PC else None
    if name.endswith("m") and name[:-1] in PC:
        return PC[name[:-1]], True
    return (PC[name], False) if name in PC else None


def key_name(k):
    return (MINOR if k[1] else MAJOR)[k[0]]


def mirex(ref, est):
    """mir_eval.key.weighted_score"""
    if est is None:
        return 0.0
    d = (est[0] - ref[0]) % 12
    if ref[1] == est[1]:
        return 1.0 if d == 0 else 0.5 if d == 7 else 0.0
    if d == (3 if ref[1] else 9):
        return 0.3
    return 0.2 if d == 0 else 0.0


def error_kind(ref, est):
    if est is None:
        return "none"
    d = (est[0] - ref[0]) % 12
    if ref[1] == est[1]:
        return {0: "correct", 7: "fifth-up", 5: "fifth-down"}.get(d, "other")
    if d == (3 if ref[1] else 9):
        return "relative"
    return "parallel" if d == 0 else "other"


def score(rows, title):
    """rows: (id, truth, estimate, confidence)"""
    n = len(rows)
    if not n:
        return
    refs = [parse_key(r[1]) for r in rows]
    ests = [parse_key(r[2]) for r in rows]
    conf = np.array([r[3] for r in rows])
    right = np.array([e == t for t, e in zip(refs, ests)])
    w = np.array([mirex(t, e) for t, e in zip(refs, ests)])
    kinds = [error_kind(t, e) for t, e in zip(refs, ests)]
    print(f"{title}: {n} tracks, accuracy {100 * right.mean():.1f} %, MIREX {w.mean():.3f}")
    print("  " + ", ".join(f"{k} {100 * kinds.count(k) / n:.1f} %" for k in
                           ["correct", "fifth-up", "fifth-down", "relative", "parallel", "other", "none"]
                           if kinds.count(k)))
    for m, label in ((False, "major"), (True, "minor")):
        sel = np.array([t[1] == m for t in refs])
        if sel.any():
            print(f"  {label} tracks: {sel.sum()}, accuracy {100 * right[sel].mean():.1f} %, "
                  f"MIREX {w[sel].mean():.3f}; named {label}: "
                  f"{sum(1 for e in ests if e and e[1] == m)}")
    edges = [0, 0.2, 0.4, 0.6, 0.8, 0.9, 1.0001]
    print("  confidence  tracks  accuracy")
    for lo, hi in zip(edges, edges[1:]):
        sel = (conf >= lo) & (conf < hi)
        if sel.any():
            print(f"  {lo:.1f}-{min(hi, 1):.1f}  {sel.sum():6d}  {100 * right[sel].mean():6.1f} %")
    for th in (0.5, 0.8, 0.9):
        sel = conf >= th
        if sel.any():
            print(f"  confidence >= {th}: {100 * sel.mean():.1f} % of tracks, accuracy "
                  f"{100 * right[sel].mean():.1f} %")


def read_results(path):
    rows = []
    for line in Path(path).read_text().splitlines():
        if line.startswith("#"):
            continue
        f = line.split("\t")
        rows.append((f[0], f[1], f[2], float(f[3])))
    return rows


# -------------------------------------------------------------- giantsteps

def gs_truth(root, which):
    """{track id: key name} of a set"""
    truth = {}
    if which == "key":
        for p in sorted((root / "gs-key" / "annotations" / "key").glob("*.key")):
            k = parse_key(p.read_text())
            if k:
                truth[p.name.split(".")[0]] = key_name(k)
    else:
        lines = (root / "gs-mtg" / "annotations" / "annotations.txt").read_text().splitlines()
        for line in lines[1:]:
            f = line.split("\t")
            if len(f) < 3 or f[2].strip() != "2":
                continue
            k = parse_key(f[1])
            if k:
                truth[f[0]] = key_name(k)
    return truth


def run_prog(prog, path, rate=44100):
    """Decodes `path` to float32 mono at `rate` and returns PROG's (key,
    confidence) on it."""
    dec = subprocess.Popen(["ffmpeg", "-v", "error", "-nostdin", "-i", f"file:{path}", "-map", "0:a:0",
                            "-ac", "1", "-ar", str(rate), "-f", "f32le", "-"],
                           stdout=subprocess.PIPE)
    out = subprocess.run([*shlex.split(prog), "-", str(rate)], stdin=dec.stdout,
                         capture_output=True, text=True).stdout
    dec.stdout.close()
    dec.wait()
    return parse_output(out)


def parse_output(out):
    tok = out.split()
    for i, t in enumerate(tok):
        if t == "-" or parse_key(t):
            try:
                conf = float(tok[i + 1])
            except (IndexError, ValueError):
                conf = 0.0
            return (t if t != "-" else "-"), conf
    return "-", 0.0


def gs(root, which, prog, out, jobs):
    truth = gs_truth(root, which)
    audio = root / f"audio-{which}"
    ids = [i for i in truth if (audio / f"{i}.LOFI.mp3").exists()]
    print(f"{which}: {len(truth)} annotated tracks, {len(ids)} with audio")

    def one(i):
        key, conf = run_prog(prog, audio / f"{i}.LOFI.mp3")
        return i, truth[i], key, conf

    with ThreadPoolExecutor(jobs) as pool:
        rows = list(pool.map(one, ids))
    if out:
        Path(out).write_text("# id truth estimate confidence\n" +
                             "".join(f"{r[0]}\t{r[1]}\t{r[2]}\t{r[3]:.4f}\n" for r in rows))
    score(rows, f"GiantSteps {which}")


# ---------------------------------------------------------------- training

def features(root, which, prog, out, jobs):
    truth = gs_truth(root, which)
    audio = root / f"audio-{which}"
    ids = [i for i in truth if (audio / f"{i}.LOFI.mp3").exists()]

    def one(i):
        dec = subprocess.Popen(["ffmpeg", "-v", "error", "-nostdin", "-i", f"file:{audio / f'{i}.LOFI.mp3'}",
                                "-map", "0:a:0", "-ac", "1", "-ar", "44100", "-f", "f32le", "-"],
                               stdout=subprocess.PIPE)
        txt = subprocess.run([*shlex.split(prog), "--features", "-", "44100"], stdin=dec.stdout,
                             capture_output=True, text=True).stdout
        dec.stdout.close()
        dec.wait()
        lines = txt.strip().splitlines()
        rows = np.array([[float(v) for v in line.split()] for line in lines[1:]])
        return float(lines[0].split()[0]), rows[0], rows[1:]

    with ThreadPoolExecutor(jobs) as pool:
        res = list(pool.map(one, ids))
    keys = np.array([parse_key(truth[i])[0] + 12 * parse_key(truth[i])[1] for i in ids])
    seg_n = np.array([len(r[2]) for r in res])
    np.savez(out, ids=np.array(ids), keys=keys, tuning=np.array([r[0] for r in res]),
             x=np.array([r[1] for r in res]), seg=np.concatenate([r[2] for r in res]), seg_n=seg_n)
    print(f"{which}: features of {len(ids)} tracks in {out}")


def _rot(X):
    """(n, 36) -> (n, 12, 36): rotation k puts pitch class k on interval 0"""
    Xr = X.reshape(len(X), 3, 12)
    return np.stack([np.roll(Xr, -k, axis=2).reshape(len(X), -1) for k in range(12)], axis=1)


def _scores(W, X, bias=0.0):
    R = _rot(X)
    s = np.concatenate([R @ W[0], R @ W[1]], axis=1)
    s[:, :12] += bias
    return s


def _fit(X, y, lam=2.5e-7):
    from scipy.optimize import minimize
    R = _rot(X)
    n, d = len(y), X.shape[1]

    def f(w):
        W = w.reshape(2, d)
        S = np.concatenate([R @ W[0], R @ W[1]], axis=1)
        S -= S.max(1, keepdims=True)
        P = np.exp(S)
        P /= P.sum(1, keepdims=True)
        loss = -np.log(P[np.arange(n), y] + 1e-12).mean() + lam * (w ** 2).sum()
        G = P
        G[np.arange(n), y] -= 1
        G /= n
        g = np.stack([np.einsum("nk,nkd->d", G[:, :12], R), np.einsum("nk,nkd->d", G[:, 12:], R)])
        return loss, g.ravel() + 2 * lam * w

    r = minimize(f, np.zeros(2 * d), jac=True, method="L-BFGS-B", options=dict(maxiter=2000))
    return r.x.reshape(2, d)


def train(path, folds=5):
    from scipy.optimize import minimize, minimize_scalar
    z = np.load(path)
    X, y, seg, seg_n = z["x"], z["keys"], z["seg"], z["seg_n"]
    n = len(y)
    seg_of = np.repeat(np.arange(n), seg_n)
    # Cross-validated scores: each track scored by templates fitted without it
    fold = np.arange(n) % folds
    Wf = [_fit(X[fold != f], y[fold != f]) for f in range(folds)]
    S = np.zeros((n, 24))
    for f in range(folds):
        S[fold == f] = _scores(Wf[f], X[fold == f])
    W = _fit(X, y)

    # The bias: the least that names as many tracks major as annotated
    share = (y < 12).mean()
    bias = 0.0
    while (np.concatenate([S[:, :12] + bias, S[:, 12:]], 1).argmax(1) < 12).mean() < share and bias < 1:
        bias += 0.001
    Sb = S.copy()
    Sb[:, :12] += bias
    est = Sb.argmax(1)

    # The segments' answers under the same fold's templates, and their
    # agreement with the whole (silent segments left out, as key.c does)
    seg_best = np.zeros(len(seg), int)
    for f in range(folds):
        sel = fold[seg_of] == f
        seg_best[sel] = _scores(Wf[f], seg[sel], bias).argmax(1)
    live = seg.sum(1) > 0
    agree = np.array([(seg_best[(seg_of == i) & live] == est[i]).mean() if (live & (seg_of == i)).any()
                      else 0.0 for i in range(n)])

    def nll_t(t):
        Z = Sb * t
        Z -= Z.max(1, keepdims=True)
        P = np.exp(Z)
        P /= P.sum(1, keepdims=True)
        return -np.log(P[np.arange(n), y] + 1e-12).mean()

    T = minimize_scalar(nll_t, bounds=(0.1, 1000), method="bounded").x
    Z = Sb * T
    Z -= Z.max(1, keepdims=True)
    P = np.exp(Z)
    P /= P.sum(1, keepdims=True)
    p = np.clip(P.max(1), 1e-6, 1 - 1e-6)
    lp = np.log(p / (1 - p))
    right = est == y

    def nll_c(v):
        q = 1 / (1 + np.exp(-(v[0] * lp + v[1] * agree + v[2])))
        return -(right * np.log(q + 1e-12) + (~right) * np.log(1 - q + 1e-12)).mean()

    cal = minimize(nll_c, [1.0, 0.0, 0.0], method="Nelder-Mead", options=dict(xatol=1e-6, fatol=1e-9)).x
    conf = 1 / (1 + np.exp(-(cal[0] * lp + cal[1] * agree + cal[2])))
    rows = [(str(i), key_name((k % 12, k >= 12)), key_name((e % 12, e >= 12)), c)
            for i, k, e, c in zip(z["ids"], y, est, conf)]
    score(rows, f"{path}: {folds}-fold cross-validation")
    print(f"bias {bias:.3f} (named major {100 * (est < 12).mean():.1f} %, annotated {100 * share:.1f} %), "
          f"temperature {T:.4f}, calibration {cal[0]:.5f} {cal[1]:.5f} {cal[2]:.5f}")
    print("static const double TEMPLATES[2][NB_FEAT] = {")
    for m in range(2):
        vals = [f"{v:+.6f}" for v in W[m]]
        print("    {")
        for b in range(6):
            print("        " + ", ".join(vals[6 * b:6 * b + 6]) + ",")
        print("    },")
    print("};")
    print(f"static const double MAJOR_BIAS = {bias:.3f};")
    print(f"static const double TEMPERATURE = {T:.4f};")
    print(f"static const double CAL_A = {cal[0]:.5f};")
    print(f"static const double CAL_B = {cal[1]:.5f};")
    print(f"static const double CAL_C = {cal[2]:.5f};")


# -------------------------------------------------------------- real tracks

def real(list_path, prog, work, jobs):
    paths = [p for p in Path(list_path).read_text().splitlines() if p.strip()][::3]
    work.mkdir(parents=True, exist_ok=True)

    def key_of(x, rate):
        with tempfile.NamedTemporaryFile(dir=work, suffix=".f32") as f:
            x.astype(np.float32).tofile(f.name)
            r = subprocess.run([*shlex.split(prog), f.name, str(rate)], capture_output=True, text=True)
            return parse_output(r.stdout)

    def resampled(x, rate, to):
        r = subprocess.run(["ffmpeg", "-v", "error", "-nostdin", "-f", "f32le", "-ar", str(rate), "-ac", "1",
                            "-i", "-", "-ar", str(to), "-f", "f32le", "-"],
                           input=x.astype(np.float32).tobytes(), capture_output=True, check=True)
        return np.frombuffer(r.stdout, dtype=np.float32)

    def one(p):
        probe = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "a:0", "-show_entries",
                                "stream=sample_rate", "-of", "default=nw=1:nk=1", f"file:{p}"],
                               capture_output=True, check=True)
        rate = int(probe.stdout.split()[0])
        raw = subprocess.run(["ffmpeg", "-v", "error", "-nostdin", "-i", f"file:{p}", "-map", "0:a:0",
                              "-ac", "1", "-f", "f32le", "-"], capture_output=True, check=True).stdout
        x = np.frombuffer(raw, dtype=np.float32)
        h = len(x) // 2
        res = {"full": key_of(x, rate), "half1": key_of(x[:h], rate), "half2": key_of(x[h:], rate),
               "-20dB": key_of(x * 0.1, rate), "peak": key_of(x / max(np.abs(x).max(), 1e-9), rate)}
        for to in (44100, 48000, 22050):
            res[f"{to // 1000}k"] = key_of(resampled(x, rate, to), to) if to != rate else res["full"]
        return p, rate, res

    with ThreadPoolExecutor(jobs) as pool:
        results = list(pool.map(one, paths))
    cols = ["full", "half1", "half2", "-20dB", "peak", "44k", "48k", "22k"]
    print(f"{'track':40s} {'rate':>6s} " + " ".join(f"{c:>11s}" for c in cols))
    agree = {c: 0 for c in cols[1:]}
    for p, rate, res in results:
        name = Path(p).stem[:40]
        print(f"{name:40s} {rate:6d} " + " ".join(f"{res[c][0]:>5s} {res[c][1]:5.2f}" for c in cols))
        for c in cols[1:]:
            agree[c] += res[c][0] == res["full"][0]
    n = len(results)
    print("same key as the whole track: " + ", ".join(f"{c} {agree[c]}/{n}" for c in cols[1:]))
    print(f"named major: {sum(1 for _, _, r in results if not r['full'][0].endswith('m'))}/{n}, "
          f"mean confidence {np.mean([r['full'][1] for _, _, r in results]):.2f}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("synth")
    p = sub.add_parser("fetch")
    p.add_argument("dir", type=Path)
    p = sub.add_parser("gs")
    p.add_argument("dir", type=Path)
    p.add_argument("--set", choices=["key", "mtg"], default="key")
    p.add_argument("--prog", required=True)
    p.add_argument("--out")
    p.add_argument("--jobs", type=int, default=12)
    p = sub.add_parser("features")
    p.add_argument("dir", type=Path)
    p.add_argument("--set", choices=["key", "mtg"], default="mtg")
    p.add_argument("--prog", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--jobs", type=int, default=12)
    p = sub.add_parser("train")
    p.add_argument("file")
    p = sub.add_parser("score")
    p.add_argument("files", nargs="+")
    p = sub.add_parser("real")
    p.add_argument("list")
    p.add_argument("--prog", required=True)
    p.add_argument("--work", type=Path, default=Path("build-key/real"))
    p.add_argument("--jobs", type=int, default=6)
    a = ap.parse_args()
    if a.cmd == "synth":
        synth()
    elif a.cmd == "fetch":
        fetch(a.dir)
    elif a.cmd == "gs":
        gs(a.dir, a.set, a.prog, a.out, a.jobs)
    elif a.cmd == "features":
        features(a.dir, a.set, a.prog, a.out, a.jobs)
    elif a.cmd == "train":
        train(a.file)
    elif a.cmd == "score":
        for f in a.files:
            score(read_results(f), f)
    elif a.cmd == "real":
        real(a.list, a.prog, a.work, a.jobs)


if __name__ == "__main__":
    sys.exit(main())
