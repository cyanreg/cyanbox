#!/bin/sh
# Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md.
#
# Port check: the same small library exported by the Python dj and by rxbox,
# then export.pdb, exportExt.pdb, the settings files and the ANLZ .DAT/.EXT
# compared byte for byte (docs/OVERVIEW.md 7). Not run by meson: it needs
# dj's virtualenv and takes a while.
#
#   tests/export/port_check.sh            (after building rx_port_export)
#   BUILD=build-formats DJ=~/projects/dj/.venv/bin/dj tests/export/port_check.sh
#
# Both libraries live in one directory, so both derive the same per-library
# constant (crc32 of the root), and rxbox adds the tracks in path order, so
# its library ids are dj's track ids. Expected differences, each a fix
# rxbox carries in on purpose (docs/OVERVIEW.md 3):
#   - PWAV/PWV2 (.DAT): heights below 2 / 1 raised (Nexus firmware rejects a
#     preview with a zero column; rbxport);
#   - PCO2 hot cue entries (.EXT): the pad RGB after the colour code comes
#     from rekordbox's measured table, and an uncoloured A-D pad gets
#     rekordbox's default pad colour instead of dj's green;
#   - MP3 waveforms where the LAME header frame counts as a frame of
#     silence on the players' timeline (rbxport's measured rule): every
#     waveform tag of such a track is one MPEG frame (1152 samples, 3 or 4
#     detail columns) longer. libmp3lame at 320k writes plain stereo, the
#     Info frame says the same, so the frame counts for two.mp3 here. Grid
#     and cue times are the library's in both and agree;
#   - .2EX is not compared: rxbox has no PWVC gains (its waveforms do not
#     carry them) and writes the neutral 80/100/100.
# Anything else that differs is a porting bug.

set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${BUILD:-$ROOT/build-formats}
DJ=${DJ:-$HOME/projects/dj/.venv/bin/dj}
PY=${PY:-$(dirname "$DJ")/python}
TOOL=$BUILD/src/export/rx_port_export
W=$BUILD/port-check

[ -x "$TOOL" ] || { echo "build $TOOL first (ninja -C $BUILD src/export/rx_port_export)"; exit 1; }
[ -x "$DJ" ] || { echo "no dj at $DJ"; exit 1; }

rm -rf "$W"
mkdir -p "$W/music" "$W/tracks/music" "$W/playlists"

# track file bpm downbeat key codec-args
tone() {
    p=$(echo "60 / $2" | bc -l)
    ffmpeg -v error -nostdin -y -f lavfi \
        -i "aevalsrc='0.7*sin(2*PI*55*t)*exp(-14*mod(t-$3+$p*64,$p))+0.05*sin(2*PI*7000*t)*exp(-60*mod(t-$3+$p*64.5,$p))':s=44100:d=30" \
        -metadata title="$1" -metadata artist=Synth -metadata album=Synthetic $5 "$W/music/$1"
    cat > "$W/tracks/music/$1.toml" <<EOF
source = "$W/music/$1"

[grid]
bpm = $2
downbeat = $3

[key]
key = "$4"

[[cue]]
at = $3
EOF
    echo "track $W/music/$1 $2 $3 $4" >> "$W/spec"
    echo "cue $W/music/$1 - $3 0 -" >> "$W/spec"
}

: > "$W/spec"
tone one.flac 126.0 0.25 Am "-c:a flac"
tone two.mp3 128.0 0.5 F# "-c:a libmp3lame -b:a 320k"
tone three.wav 140.0 0.1 Cm "-c:a pcm_s16le"
tone four.m4a 122.0 0.3 Eb "-c:a aac -b:a 256k"
ffmpeg -v error -nostdin -y -f lavfi -i color=c=orange:s=320x320 -frames:v 1 "$W/music/front.jpg"

# A hot cue with a 4-beat loop on bar 5 of one.flac, and an uncoloured
# hot cue B, as dj's test hand-edits.
A=$(echo "0.25 + 16 * 60 / 126" | bc -l | sed 's/^\./0./')
B=$(echo "0.25 + 32 * 60 / 126" | bc -l | sed 's/^\./0./')
cat >> "$W/tracks/music/one.flac.toml" <<EOF

[[cue]]
at = $A
hot = "A"
loop = 4
name = "loop"

[[cue]]
at = $B
hot = "B"
EOF
echo "cue $W/music/one.flac A $A 4 loop" >> "$W/spec"
echo "cue $W/music/one.flac B $B 0 -" >> "$W/spec"

printf '../music/two.mp3\n../music/one.flac\n' > "$W/playlists/01 Set.m3u8"
echo "playlist Set $W/music/two.mp3 $W/music/one.flac" >> "$W/spec"

cat > "$W/dj.toml" <<EOF
music = ["music"]
[device]
waveform_color = "3band"
[player]
quantize = true
EOF
echo "setting device.waveform_color 3band" >> "$W/spec"
echo "setting player.quantize true" >> "$W/spec"

mkdir -p "$W/stick-dj" "$W/stick-rx"
echo "== dj export"
"$DJ" -c "$W/dj.toml" export "$W/stick-dj"
echo "== rxbox export"
"$TOOL" "$W/spec" "$W" "$W/stick-rx"

echo "== comparison"
"$PY" -I - "$W/stick-dj" "$W/stick-rx" "$HOME/projects/dj/src" <<'EOF'
import struct, sys
from pathlib import Path
sys.path.insert(0, sys.argv[3])
from dj.formats import anlz, pdb

dj, rx = Path(sys.argv[1]), Path(sys.argv[2])
bad = 0

def note(msg):
    print("  " + msg)

for name in ("MYSETTING.DAT", "MYSETTING2.DAT", "DJMMYSETTING.DAT", "DEVSETTING.DAT"):
    a, b = dj / "PIONEER" / name, rx / "PIONEER" / name
    if a.exists() != b.exists():
        note(f"{name}: only in {'dj' if a.exists() else 'rxbox'}"); bad += 1
    elif a.exists():
        same = a.read_bytes() == b.read_bytes()
        note(f"{name}: {'identical' if same else 'DIFFERENT'}"); bad += not same

def tables(path):
    b = path.read_bytes()
    _, ts = pdb.read_header(b)
    return b, ts

for name in ("export.pdb", "exportExt.pdb"):
    a, b = dj / "PIONEER/rekordbox" / name, rx / "PIONEER/rekordbox" / name
    if a.exists() != b.exists():
        note(f"{name}: only in {'dj' if a.exists() else 'rxbox'}"); bad += 1
        continue
    if not a.exists():
        note(f"{name}: written by neither (no My Tags)")
        continue
    if a.read_bytes() == b.read_bytes():
        note(f"{name}: identical"); continue
    ba, ta = tables(a)
    bb, tb = tables(b)
    for t in range(len(ta)):
        rows_a = [pos for _, _, pos in pdb.iter_rows(ba, ta[t])]
        rows_b = [pos for _, _, pos in pdb.iter_rows(bb, tb[t])]
        if len(rows_a) != len(rows_b):
            note(f"{name} table {t}: {len(rows_a)} rows in dj, {len(rows_b)} in rxbox"); bad += 1
            continue
        # Compare each page's bytes from 0x18 on (the header words before
        # hold page numbers and sequences, which follow the layout).
        pa = sorted({p // 4096 for p in rows_a})
        pb = sorted({p // 4096 for p in rows_b})
        for x, y in zip(pa, pb):
            if ba[x * 4096 + 0x18:(x + 1) * 4096] != bb[y * 4096 + 0x18:(y + 1) * 4096]:
                if t == pdb.T_TRACKS:
                    for i, (pa_, pb_) in enumerate(zip(rows_a, rows_b)):
                        da, db = pdb.read_track(ba, pa_), pdb.read_track(bb, pb_)
                        diff = {k: (da[k], db[k]) for k in da if da[k] != db[k]}
                        if diff:
                            note(f"{name} track {da['file_path']}: {diff}")
                else:
                    note(f"{name} table {t}: page differs")
                bad += 1
    hdr_same = ba[:0x1C] == bb[:0x1C]
    note(f"{name}: header {'identical' if hdr_same else 'differs'}")

def tracks(stick):
    b = (stick / "PIONEER/rekordbox/export.pdb").read_bytes()
    _, ts = pdb.read_header(b)
    return {t["file_path"]: t for t in (pdb.read_track(b, p) for _, _, p in pdb.iter_rows(b, ts[pdb.T_TRACKS]))}

expected = {b"PWAV", b"PWV2"}
waves = {b"PWAV", b"PWV2", b"PWV3", b"PWV4", b"PWV5"}
ta, tb = tracks(dj), tracks(rx)
for path in sorted(ta):
    if path not in tb:
        note(f"{path}: not on the rxbox stick"); bad += 1; continue
    # The MP3 rule: the detail waveform one frame longer in rxbox.
    shifted = False
    if path.endswith(".mp3"):
        ea = dict(anlz.parse((dj / (ta[path]["analyze_path"][1:-4] + ".EXT")).read_bytes()))
        eb = dict(anlz.parse((rx / (tb[path]["analyze_path"][1:-4] + ".EXT")).read_bytes()))
        na = struct.unpack(">I", ea[b"PWV3"][16:20])[0]
        nb = struct.unpack(">I", eb[b"PWV3"][16:20])[0]
        shifted = nb - na in (3, 4)
        if shifted:
            note(f"{path}: waveforms {nb - na} columns longer (header frame counted: expected)")
    for ext in (".DAT", ".EXT"):
        fa = dj / (ta[path]["analyze_path"][1:-4] + ext)
        fb = rx / (tb[path]["analyze_path"][1:-4] + ext)
        A, B = anlz.parse(fa.read_bytes()), anlz.parse(fb.read_bytes())
        if [f for f, _ in A] != [f for f, _ in B]:
            note(f"{path}{ext}: tag order {[f.decode() for f, _ in A]} vs {[f.decode() for f, _ in B]}"); bad += 1
            continue
        for (f, x), (_, y) in zip(A, B):
            if x == y or (shifted and f in waves):
                continue
            if f in expected:
                ha = [v & (31 if f == b"PWAV" else 15) for v in x[20:]]
                hb = [v & (31 if f == b"PWAV" else 15) for v in y[20:]]
                floor = 2 if f == b"PWAV" else 1
                other = sum(1 for p, q in zip(ha, hb) if p != q and not (p < floor and q == floor))
                note(f"{path}{ext} {f.decode()}: {sum(p != q for p, q in zip(x, y))} bytes differ"
                     f"{' (all the preview minimum)' if not other else f', {other} not explained'}")
                bad += other > 0
            elif f == b"PCO2" and len(x) == len(y):
                ca, cb = anlz.read_cues(x), anlz.read_cues(y)
                rest = [(p, q) for p, q in zip(ca, cb) if {**p, "color": 0} != {**q, "color": 0}]
                note(f"{path}{ext} PCO2: colours {[c['color'] for c in ca]} -> {[c['color'] for c in cb]}"
                     f"{'' if not rest else f'; other fields differ: {rest}'}")
                bad += bool(rest)
            else:
                note(f"{path}{ext} {f.decode()}: DIFFERENT ({len(x)} vs {len(y)} bytes)"); bad += 1

print("port check:", "only the documented differences" if not bad else f"{bad} unexplained difference(s)")
sys.exit(1 if bad else 0)
EOF
