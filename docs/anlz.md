# ANLZ analysis files (`ANLZnnnn.DAT` / `.EXT` / `.2EX`): writer reference

Scope: everything needed to write rekordbox-compatible analysis files from
scratch, for CDJ/XDJ hardware, without rekordbox. Byte offsets are **relative to
the start of the tag** (the first byte of its four-character code) unless a
table says otherwise. Every multi-byte number in these files is **big-endian**
(unlike `export.pdb` and the `*SETTING.DAT` files, which are little-endian).

Items marked **UNCERTAIN** are not settled. Where sources disagree, both versions
are given along with any evidence that decides between them.

---

## 0. Sources and licences

| Tag | Project / document | Licence | What was taken |
|---|---|---|---|
| [DS] | Deep Symmetry, "Analysis Files" (crate-digger `doc/modules/ROOT/pages/anlz.adoc`), <https://djl-analysis.deepsymmetry.org/rekordbox-export-analysis/anlz.html> | EPL-2.0 | Tag layouts, phrase tables, colour table |
| [KSY] | crate-digger `src/main/kaitai/rekordbox_anlz.ksy`, <https://github.com/Deep-Symmetry/crate-digger> (commit 7c9d535) | EPL-2.0 OR MPL-2.0 OR LGPL-3.0-only | Field layout, enums |
| [DJL] | Deep Symmetry dysentery, "Track Metadata" (`track_metadata.adoc`), <https://djl-analysis.deepsymmetry.org/djl-analysis/track_metadata.html> | EPL-2.0 | Frames/half-frames, colour preview analysis, PWVC thresholds |
| [BL] | beat-link `WaveformDetail.java`, `WaveformPreview.java`, `CueList.java`, `Util.java`, <https://github.com/Deep-Symmetry/beat-link> (ef0aaa1) | EPL-2.0 | How a reader renders the data; colour code table; time conversion |
| [PRB] | pyrekordbox `pyrekordbox/anlz/*.py`, `docs/source/formats/anlz.md`, <https://github.com/dylanljones/pyrekordbox> (5feacbc) | MIT | Struct definitions, PVB2/PVDI, tag order |
| [RC] | rekordcrate `src/anlz.rs`, `src/xor.rs`, fixtures `data/anlz/sweep`, `data/complete_export`, <https://github.com/Holzhaus/rekordcrate> (14d54ed) | MPL-2.0 (fixture files included) | Layouts, PWVC, real rekordbox 6.6.1 files used below for verification |
| [RBOX] | rbox 0.1.7 crate `src/anlz/anlz.rs` (crates.io; GitHub repo now 404), <https://docs.rs/rbox> | GPL-3.0-only | Constants it asserts |
| [BK] | baken `crates/baken-export/src/anlz/**`, <https://github.com/M-Igashi/baken> (194ef79) | MIT | **Byte-exact writer rules** checked against rekordbox 7 exports, waveform derivation measured on 1070 tracks, ANLZ directory hash, PVB2 |
| [SB] | Seabass `docs/onelibrary-format.md`, `src/infrastructure/rekordbox/anlz_*codec*`, <https://github.com/sebasje/seabass> (397bb6e) | code: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL; docs CC-BY-SA-4.0; test fixtures CC0-1.0 | Survey of 7968 real PCOB sections, hardware results (XDJ-RX2, CDJ-3000X, OMNIS-DUO) |
| [FF] | fourfour `pioneer-usb-writer/reference-code/PIONEER.md`, <https://github.com/morizkraemer/fourfour> (64091d3) | **no licence file** (all rights reserved). Facts only, no code copied | CDJ-3000 (fw 3.19) behaviour: PPTH NUL, directory hash, re-analysis triggers |
| [RBX] | rbxport `crates/rbl-analysis/src/waveform.rs`, `crates/rbl-anlz/src/encode.rs`, `src/canvas/waveform.ts` (`~/projects/rbxport`, 1.6.0) | GPL-2.0-or-later. Facts only, no code copied | The three-band analysis cyanbox writes to PWV6/PWV7 (§4.12, §4.13), preview minimums, how its interface draws the three bands |
| [ME] | My own measurements, made by parsing the real rekordbox-written fixtures above (rekordcrate sweep + demo export: rekordbox 6.6.1; Seabass `anonymized_library`: a rekordbox 7.x stick with 1992 analysis sets) with throwaway scripts | n/a | Verification of constants, band order, value ranges |

---

## 1. Where the files live and how they are named

### 1.1 Directory: computed from the audio path (players do **not** trust `analyze_path`)

```
/PIONEER/USBANLZ/P{ppp}/{hhhhhhhh}/ANLZ{nnnn}.DAT   (+ .EXT, + .2EX)
```

[FF] (disassembly of rekordbox `CreateAnlzFileFolderPath()`, plus CDJ-3000
testing) and [BK] (checked against all 587 tracks of a real export) both report the
same algorithm. I also checked it against the rekordbox 6.6.1 demo export [ME]
(`/Contents/Loopmasters/UnknownAlbum/Demo Track 1.mp3` gives `P016/0000875E`,
which matches). Source: <https://github.com/M-Igashi/baken/blob/main/crates/baken-export/src/anlz/hash.rs>

```c
/* path: the USB-root-relative audio path exactly as stored in the DB, e.g.
 * "/Contents/Artist/Album/Track.flac", iterated as UTF-16 code units. */
uint32_t h = 0;
for (each UTF-16 code unit c of path) {
    h = h * 0x5BC9u + c;      /* uint32 wraparound */
    h = h * 0x93B5u + c;
}
uint32_t r = h % 200003u;    /* 0x30D43, prime */
uint32_t p = (r & 0x01) | ((r >> 1) & 0x02) | ((r >> 4) & 0x04) | ((r >> 4) & 0x08)
           | ((r >> 5) & 0x10) | ((r >> 8) & 0x20) | ((r >> 10) & 0x40);
sprintf(dir, "/PIONEER/USBANLZ/P%03X/%08X", p, r);   /* upper-case hex */
```

* Two tracks can hash to the same directory: about n²/400000 pairs for n tracks. rekordbox then
  numbers them in export order: `ANLZ0000.*` for the first, `ANLZ0001.*` for the next [BK].
* [FF] reports that a CDJ which **rejects** `ANLZ0000.DAT` (see PPTH below) runs its own analysis
  and writes `ANLZ0001.DAT` beside it. So an unexpected `ANLZ0001` on a stick
  usually means the player did not accept your file.
* The PDB `analyze_path` string and the OneLibrary `content.analysisDataFilePath`
  should still be written as `"/PIONEER/USBANLZ/Pppp/hhhhhhhh/ANLZ0000.DAT"`
  (rekordbox does so). [FF] and [BK] say CDJs ignore that field and recompute the path.
  **UNCERTAIN** for OneLibrary-only players: [SB] observed that the CDJ-3000X used the file
  `analysisDataFilePath` names, which is the same file anyway.

### 1.2 Which tags go in which file, in rekordbox's order

Observed in real rekordbox output ([ME] on rekordbox 6.6.1 and on the 1992-file
rekordbox 7 corpus; [BK] reproduces it byte for byte):

| File | Tag sequence (rekordbox order) |
|---|---|
| `.DAT` | `PPTH PVBR PQTZ PWAV PWV2 PCOB(hot) PCOB(memory)` |
| `.EXT` | `PPTH PWV3 PCOB(hot) PCOB(memory) PCO2(hot) PCO2(memory) PQT2 PWV5 PWV4` then `PVB2` (FLAC only) then `PSSI` (if phrase-analysed) |
| `.2EX` | `PPTH PWV7 PWV6 PWVC` then `PVDI` (some rekordbox 7 files) |

* `PVB2` position relative to `PSSI`: [PRB] lists `... PWV4, PVB2, PSSI`. No fixture with both was
  available. **UNCERTAIN**, but follow [PRB].
* [FF] claims the CDJ "reads sections sequentially, order matters". That is unverified, but reproducing
  rekordbox's order costs nothing.
* [PRB]'s order list (`... PQT2, PWV3, PWV4, PWV5 ...`) is **wrong**. Real files put `PWV3` first
  and `PWV5` before `PWV4`.
* [FF] states rekordbox does not put `PWV2` in the `.DAT`. That is **contradicted** by every real
  rekordbox file examined [ME]: all 1992 `.DAT` files plus the 6.6.1 demo files contain `PWV2`.
* The key (musical key) is **not** in any ANLZ file. There is no `PKEY` tag. Key lives in
  `export.pdb` / `exportLibrary.db`.

---

## 2. Time units

Sources: [DJL] "Frames" sidebar; [BL] `Util.java`; [BK] waveform.rs; [PRB] db6 docs.

| Unit | Definition | Used by |
|---|---|---|
| millisecond | track time at 100 % pitch | `PQTZ` beat time, `PCPT`/`PCP2` cue and loop times, master.db `InMsec` |
| **half-frame** | 1/150 s (6.667 ms) | one waveform-detail column (`PWV3`/`PWV5`/`PWV7`); dbserver cue positions; OneLibrary `cue.in150FramePerSec`; master.db `InFrame` |
| CDJ frame | 1/75 s (the player display shows `mm:ss:ff.f` with `ff.f` = 00.0 to 74.5) | display only |
| µs | microseconds | OneLibrary `cue.inUsec`/`outUsec` (observed as ms × 1000) |

Conversions used by beat-link [BL]: `half_frame = ms * 15 / 100` (truncating),
or `round(ms * 0.15)`; `ms = half_frame * 100 / 15`.

**Waveform-detail column grid** [BK], verified [ME]: column `i` covers
samples `[round(i*rate/150), round((i+1)*rate/150))` of the mono mix `(L+R)/2`,
and the column count is `ceil(total_samples * 150 / sample_rate)`. On the
rekordbox 6.6.1 demo MP3s, `ceil(7604352 * 150 / 44100) = 25866`, which equals the
`PWV3`/`PWV5`/`PWV7` entry count, and `7604352` is the `PVBR` trailer (MPEG
frames × 1152). So for MP3, rekordbox's timeline counts **every MPEG audio frame from sample 0**
(Xing/Info frame excluded) and does not trim LAME encoder delay or padding.
**UNCERTAIN** (inference from counts only): cue and beat times on MP3 are therefore
presumably relative to the start of the first audio frame, with no gapless correction.

---

## 3. File header: `PMAI`

Source: [DS] "Analysis File Header", [BK] section.rs, [FF], [ME].

| Off | Size | Value | Meaning |
|---|---|---|---|
| 0x00 | 4 | `"PMAI"` | magic |
| 0x04 | 4 | `0x0000001C` | len_header (always 28) |
| 0x08 | 4 | n | len_file: total file size in bytes (header + all tags) |
| 0x0C | 4 | `0x00000001` | unknown |
| 0x10 | 4 | `0x00010000` | unknown |
| 0x14 | 4 | `0x00010000` | unknown |
| 0x18 | 4 | `0x00000000` | unknown |

Bytes 0x0C–0x1B are identical in rekordbox 6.6.1 and 7.x files (`00000001 00010000 00010000 00000000`).
[FF]: CDJ-3000-generated files have zeros here, and both forms are accepted. Write rekordbox's values.

### Generic tag envelope (all tags)

| Off | Size | Meaning |
|---|---|---|
| 0x00 | 4 | fourcc (ASCII) |
| 0x04 | 4 | len_header: bytes of "header" part, a fixed per-tag constant listed below |
| 0x08 | 4 | len_tag: whole tag including the 12-byte envelope; next tag starts at `tag + len_tag` |

Readers walk tags by `len_tag`. `len_header` is **not** derivable and must be written with
the per-tag constant. [FF] notes that a wrong `PQTZ` len_header (20 instead of 24) is "unknown, may cause
re-analysis".

---

## 4. Tag reference

### 4.1 `PPTH`: audio path (all three files)

Source: [DS] §Path, [FF] §2, [BK] section.rs.

| Off | Size | Value |
|---|---|---|
| 0x00 | 4 | `"PPTH"` |
| 0x04 | 4 | `0x10` |
| 0x08 | 4 | `0x10 + len_path` |
| 0x0C | 4 | len_path = (UTF-16 code units + 1) × 2 (**includes** the terminating NUL) |
| 0x10 | len_path | path, **UTF-16BE**, terminated by `00 00` |

* The path is the USB-root-relative audio path (`/Contents/Artist/Album/Track.mp3`). It is the same
  string that is hashed for the directory and stored in the DB.
* **Hardware gotcha** [FF] (CDJ-3000 fw 3.19): without the terminating `00 00`
  (with len_path not counting it), the CDJ finds the file, rejects it, and re-analyses (creating
  `ANLZ0001.DAT`).
* rekordbox's *local* (non-export) analysis files in rekordbox 7 store `"?/<file name>"`
  here. Older versions store the full local path. The exporter rewrites it [BK]. Always write the stick path.

### 4.2 `PVBR`: MP3 seek index (`.DAT`)

Sources: [DS] §VBR, [KSY], [PRB], [BK] assemble.rs/rewrite.rs, [ME].

| Off | Size | Value |
|---|---|---|
| 0x00 | 4 | `"PVBR"` |
| 0x04 | 4 | `0x10` |
| 0x08 | 4 | `1620` (0x654), constant |
| 0x0C | 4 | `0` |
| 0x10 | 400 × 4 | `idx[400]`, u32 each |
| 0x650 | 4 | trailer |

What rekordbox writes (verified [BK] and [ME]):

* **CBR MP3**: all 400 entries `0`; trailer = (number of MPEG audio frames, *excluding* a Xing/Info
  header frame) × 1152 = total samples. Demo track 1: `7604352` = 6601 × 1152 (172.4 s).
  [BK]'s frame walk skips an ID3v2 tag, then the first frame if it contains `Xing` or `Info`.
* **AAC/M4A**: in the rekordbox 7 corpus [ME], 231 `.DAT` files have entries 0 **and** trailer 0
  (that matches the AAC tracks of that library).
* **FLAC/WAV/AIFF**: all zero ([BK], [FF]). FLAC seeking uses `PVB2` instead (§4.15).
* **VBR MP3**: entries are non-zero. **UNCERTAIN / not publicly reverse-engineered.** [PRB] guesses
  "frame indices of the times"; no fixture with a non-zero table was available. Safe options:
  transcode to CBR (baken's `--cdjsafe` does this, and its notes mention "rekordbox's VBR cue-offset
  problem"), or write zeros and accept degraded seeking (untested).

### 4.3 `PQTZ`: beat grid (`.DAT`)

Sources: [DS] §Beat Grid, [BK] grid.rs, [FF], [ME].

| Off | Size | Value |
|---|---|---|
| 0x00 | 4 | `"PQTZ"` |
| 0x04 | 4 | `0x18` (24, **not** 20) |
| 0x08 | 4 | `0x18 + 8 * n` |
| 0x0C | 4 | `0` |
| 0x10 | 4 | `0x00080000` |
| 0x14 | 4 | n = number of beats |
| 0x18 | 8 × n | beats |

Beat entry (8 bytes):

| Off | Size | Meaning |
|---|---|---|
| 0 | 2 | beat_number: position in bar, 1..4 (1 = downbeat) |
| 2 | 2 | tempo: BPM × 100 at this beat (e.g. 12800 = 128.00) |
| 4 | 4 | time: ms from track start at 100 % pitch |

* [DS] text says the 0x10 word is "`00800000`". Real files hold **`0x00080000`** ([ME], all files; [FF],
  [RBOX] asserts it, [BK]). The DS hex grouping is a typo.
* rekordbox expansion of a (TEMPO-marker style) grid, reproduced byte for byte by [BK]:
  each segment `k` with start `S` s, `bpm`, first bar position `B` produces beats at
  `round(S*1000 + i*60000/bpm)` ms, numbered from `B` cycling 1..4. A segment followed by
  another holds `round((S_next - S)*bpm/60)` beats (≥1). The last segment continues while the
  **unrounded** time is `< duration_ms`. Every beat carries its segment's tempo.
* Tempo is per beat, so variable-tempo grids are just a list.
* An empty grid is legal: n = 0, len_tag = 24. [BK]: the player then detects BPM itself while playing,
  and quantize and beat sync cannot use a grid.

### 4.4 `PQT2`: "extended" beat grid (`.EXT`)

Sources: [PRB] structs.py/docs, [RBOX], [BK] grid.rs, [ME]. Semantics **largely unknown**.

| Off | Size | Value (observed) |
|---|---|---|
| 0x00 | 4 | `"PQT2"` |
| 0x04 | 4 | `0x38` (56) |
| 0x08 | 4 | `0x38 + 2 * n` (or `0x38` when n = 0) |
| 0x0C | 4 | `0` |
| 0x10 | 4 | `0x01000002` ([PRB] also saw `0x02000002`) |
| 0x14 | 4 | `0` |
| 0x18 | 8 | beat entry (PQTZ format): **first** beat of the grid |
| 0x20 | 8 | beat entry: **last** beat of the grid |
| 0x28 | 4 | n = number of 2-byte entries = number of beats in `PQTZ` |
| 0x2C | 4 | unknown (e.g. `0x022B06EA`, `0x012A2517`, `0x014E1827`), not understood |
| 0x30 | 4 | `0` |
| 0x34 | 4 | `0` |
| 0x38 | 2 × n | entries, meaning unknown |

Observations [ME]: in demo track 1 (128 BPM, 368 beats), 0x18 = `(1, 12800, 25 ms)` and 0x20 = `(4, 12800, 172056 ms)`,
which are the first and last PQTZ beats. The 2-byte entries are values < 0x400. On a
constant 120 BPM track they alternate (`01E7 0214 01E7 0214 ... 0242 0214 ... 026F`, steps of ~45).
They look like sub-beat phase corrections, but this is **UNCERTAIN**. [PRB] reads them as `(beat u8, unknown u8)`,
which does not fit the data.

**What to write**: the "empty" form rekordbox emits when it has no extended grid ([BK]):
56 bytes, all zero except 0x04 = 0x38, 0x08 = 0x38, 0x10 = `01 00 00 02`. [BK] sticks with this
empty PQT2 were played successfully on CDJ-3000 (fw 3.22) and CDJ-2000NXS2 (1.85/1.87).

### 4.5 `PCOB` + `PCPT`: legacy cue lists (`.DAT` **and** `.EXT`)

Sources: [DS] §Cue List, [KSY], [BK] cues.rs (byte-exact fixtures from a rekordbox 7 export), [SB] `anlz_legacy_cue_codec.hpp`
(survey of 7968 real PCOB sections), [ME].

Each file has exactly **two** PCOB tags: first the hot-cue list, then the memory list.

PCOB header:

| Off | Size | Value |
|---|---|---|
| 0x00 | 4 | `"PCOB"` |
| 0x04 | 4 | `0x18` |
| 0x08 | 4 | `0x18 + 0x38 * n` |
| 0x0C | 4 | list type: `0` = memory cues/loops, `1` = hot cues/loops |
| 0x10 | 2 | `0` (in all 7968 surveyed) |
| 0x12 | 2 | n = number of PCPT entries |
| 0x14 | 4 | memory_count, see below |

**memory_count** (named by [DS], meaning unknown there). Measured in [SB] and [BK]:
* hot list (empty or not): `0xFFFFFFFF`
* empty memory list: `0xFFFFFFFF`
* non-empty memory list: **index of the last entry** = n − 1
* **Hardware gotcha** [SB] (XDJ-RX2 fw 1.43): a memory list holding entries but with `0xFFFFFFFF` here
  is treated as empty. The player drops the cue and **hangs** on its next memory-loop save
  (power cycle needed).
* [FF] describes 0x0C as "cue count (1 for hot)". That is a misreading; it is the list type.

PCPT entry (always 56 bytes):

| Off | Size | Value |
|---|---|---|
| 0x00 | 4 | `"PCPT"` |
| 0x04 | 4 | `0x1C` |
| 0x08 | 4 | `0x38` |
| 0x0C | 4 | hot_cue: 0 = memory cue; 1 = A, 2 = B, … 8 = H |
| 0x10 | 4 | status: `0` normal; `4` = this memory loop is the track's **active loop** |
| 0x14 | 4 | `0x00010000` |
| 0x18 | 2 | order_first: in memory lists the **previous** entry's index (`0xFFFF` for the first); `0xFFFF` in hot lists |
| 0x1A | 2 | order_last: in memory lists the **next** entry's index (`0xFFFF` for the last); `0xFFFF` in hot lists |
| 0x1C | 1 | type: `1` = cue point, `2` = loop |
| 0x1D | 1 | `0` |
| 0x1E | 2 | `0x03E8` (1000) |
| 0x20 | 4 | time (ms) |
| 0x24 | 4 | loop end (ms) if type 2, else `0xFFFFFFFF` |
| 0x28 | 16 | zeros (non-zero only in a few loops/old entries) |

* [DS] text says 0x14 is "`00100000`". Real data is **`0x00010000`** ([KSY], [PRB], [BK], [SB]). The DS text is a typo.
* [DS] describes order_first as "ffff, 0000, 2, 3…" and order_last as "1, 2, …, ffff". The correct
  reading is a doubly-linked list by index (prev/next) [BK] [SB]. Example with 3 memory entries:
  `(FFFF,0001) (0000,0002) (0001,FFFF)`.
* An older rekordbox writer generation is visible in real data: `status = 1`, word at 0x14 = `0`,
  loop_time = `0` for non-loops [SB]. Write the majority form shown above.
* The active loop flag (status 4) is set **only** in the `.DAT` memory list; at most one per
  track [BK].

**Which cues go where (critical)** [SB] (survey: 64 hot entries in `.DAT` never had hot_cue > 3;
55 in `.EXT` never had hot_cue < 4), [BK] (verified on a rekordbox 7 export):

| File | PCOB hot list | PCOB memory list |
|---|---|---|
| `.DAT` | hot cues **A–C** only | **all** memory cues/loops |
| `.EXT` | hot cues **D–H** only | always **empty** |

Writing all hot cues into both gives old players pads they should not have [SB].

Ordering inside lists: [SB] found hot lists ordered by **descending** hot-cue number (45/51
sections; 6 older ones ascending). [BK] writes the reverse of the rekordbox-XML order (i.e.
database order), and memory lists are in that order too (not necessarily sorted by time). No
evidence that players care. Descending hot-cue number is a safe choice.

**Hardware gotcha** [SB]: the XDJ-RX2 (fw 1.43) and the **CDJ-3000X** display the
**legacy PCOB lists**, not PCO2. A file whose PCOB hot list held 1 pad and PCO2 held 5 showed
**one** pad on the 3000X. Cue edits written only to PCO2 are invisible on those players.
Keep PCOB and PCO2 consistent.

### 4.6 `PCO2` + `PCP2`: extended cue lists (`.EXT`, NXS2+)

Sources: [DS] §Extended Cue List, [KSY], [BK] cues.rs, [SB] anlz_cue_codec, [RC] test, [ME] survey of
433 entries in the rekordbox 7 corpus.

Two PCO2 tags in `.EXT`: hot list first, memory list second. Each holds **all** cues of its kind
(A–H in one list).

PCO2 header:

| Off | Size | Value |
|---|---|---|
| 0x00 | 4 | `"PCO2"` |
| 0x04 | 4 | `0x14` |
| 0x08 | 4 | `0x14 + Σ len_entry` |
| 0x0C | 4 | list type: 0 memory, 1 hot |
| 0x10 | 2 | n entries |
| 0x12 | 2 | `0` |

PCP2 entry (variable length):

| Off | Size | Value |
|---|---|---|
| 0x00 | 4 | `"PCP2"` |
| 0x04 | 4 | `0x10` |
| 0x08 | 4 | len_entry (see below) |
| 0x0C | 4 | hot_cue (0 memory, 1..8 = A..H) |
| 0x10 | 1 | type: 1 point, 2 loop |
| 0x11 | 1 | `0` |
| 0x12 | 2 | `0x03E8` |
| 0x14 | 4 | time (ms) |
| 0x18 | 4 | loop end (ms) or `0xFFFFFFFF` |
| 0x1C | 1 | color_id: memory-cue colour, row of the PDB/OneLibrary colour table (0 none, 1 Pink, 2 Red, 3 Orange, 4 Yellow, 5 Green, 6 Aqua, 7 Blue, 8 Purple). Hot cues use the bytes after the comment instead |
| 0x1D | 7 | first byte `0x01` (421/433 entries; `0x03` in 12), rest normally zero; [SB] saw bytes 0x1E–0x1F vary (e.g. `01 A8`) in some entries. Meaning unknown, write `01 00 00 00 00 00 00` |
| 0x24 | 2 | loop_numerator (quantized loop length in beats) |
| 0x26 | 2 | loop_denominator |
| 0x28 | 4 | len_comment: bytes of comment incl. NUL; 0 if no comment |
| 0x2C | len_comment | comment, UTF-16BE + `00 00` (len is even, ≥ 4 when non-zero) |
| 0x2C+L | 1 | hot cue color_code (index into §4.6.1 table; 0 = none/default/custom) |
| +1 | 1 | red |
| +2 | 1 | green |
| +3 | 1 | blue |
| +4 | 40 | optional trailer, normally zeros |

* Quantized loops: numerator and denominator are powers of two; if num > 1 then den = 1, and vice versa.
  Both are 0 for non-loops and manual loops [DS]. Example: 8-beat loop = `(8,1)`, half beat = `(1,2)`.
* **len_entry variants seen in real rekordbox data** [ME]: `0x2C + L` (no colour bytes: 197 of 200
  memory entries, 3 hot), `0x30 + L` (colour, no trailer: 155 hot entries), `0x58 + L` (colour +
  40-byte trailer: 75 hot, 2 memory). Readers must accept truncated entries [DS] [KSY]. Writers can
  use `0x2C + L + 4 + 40` (= 88 with no comment), as [BK] does; hardware accepted it [BK].
* [RC] contains a real rekordbox entry whose trailer is not all zero (`00c17000`, `02307761…10 00`).
  Meaning unknown. Zeros work [BK].
* [BK] writes the rekordbox 7 form for the default green: `color_code 0, RGB 1A FF 00`. The
  rekordbox 7 corpus [ME] shows `00 00 FF 00` (75×) and `15 00 FF 00` (46×) for green. The
  RGB bytes are an LED colour, "similar but not identical" to the GUI colour [DS].
* rekordbox also writes **code 0 with an arbitrary RGB** (custom colours, e.g. `00 EF 81 30`,
  `00 4D 00 FF`) [ME] [RC]. beat-link treats code 0 + non-zero RGB as an "embedded colour" [BL].

#### 4.6.1 Hot cue colour codes

GUI colours per code (rekordbox 4×4 palettes), from [BL] `CueList.findRekordboxColor` (EPL-2.0),
identical to [RC]'s table. Format `code: #rrggbb`:

```
01 #305aff  02 #5073ff  03 #508cff  04 #50a0ff  05 #50b4ff  06 #50b0f2  07 #50aee8  08 #45acdb
09 #00e0ff  0a #19daf0  0b #32d2e6  0c #21b4b9  0d #20aaa0  0e #1fa392  0f #19a08c  10 #14a584
11 #14aa7d  12 #10b176  13 #30d26e  14 #37de5a  15 #3ceb50  16 #28e214  17 #7dc13d  18 #8cc832
19 #9bd723  1a #a5e116  1b #a5dc0a  1c #aad208  1d #b4c805  1e #b4be04  1f #bab404  20 #c3af04
21 #e1aa00  22 #ffa000  23 #ff9600  24 #ff8c00  25 #ff7500  26 #e0641b  27 #e0461e  28 #e0301e
29 #e02823  2a #e62828  2b #ff376f  2c #ff2d6f  2d #ff127b  2e #f51e8c  2f #eb2da0  30 #e637b4
31 #de44cf  32 #de448d  33 #e630b4  34 #e619dc  35 #e600ff  36 #dc00ff  37 #cc00ff  38 #b432ff
39 #b93cff  3a #c542ff  3b #aa5aff  3c #aa72ff  3d #8272ff  3e #6473ff
00 = none (old CDJs show green)
```

LED RGB bytes rekordbox actually stored next to some codes [ME] (rekordbox 7 corpus):
`01→00 00 FF`, `08→00 C4 FF`, `13→00 FF 30`, `15→00 FF 00`, `17→33 FF 00`, `20→FF E8 00`,
`24→FF 8C 00`, `26→FF 5E 00`, `2B→FF 00 17`, `3C→4D 00 FF`. **UNCERTAIN**: the full code-to-LED table
is not published, and [BL] logs warnings because it assumes LED = GUI colour.

#### 4.6.2 Agreement between lists

rekordbox's own exports sometimes disagree between PCOB and PCO2 by 1–52 ms for the same cue [SB],
so players tolerate small differences. Seabass uses a 500 ms tolerance. Keep them identical.

### 4.7 `PWAV`: monochrome preview, 400 columns (`.DAT`)

Sources: [DS] §Waveform Preview, [BK] waveform.rs, [ME].

| Off | Size | Value |
|---|---|---|
| 0x00 | 4 | `"PWAV"` |
| 0x04 | 4 | `0x14` |
| 0x08 | 4 | `0x14 + 400` = 420 |
| 0x0C | 4 | len_data = 400 |
| 0x10 | 4 | `0x00010000` (DS text says `00100000`, a typo; real files are `0x00010000` [ME] [RBOX]) |
| 0x14 | 400 | one byte per column: bits 0–4 height (0..31), bits 5–7 whiteness (0..7) |

Rendering (nexus players, [DS]/[BL]): higher whiteness means a whiter/less saturated blue. [BL] draws
whiteness ≥ 5 as "intense" (116,246,244), otherwise (43,89,255).

Observed ranges in 1992 real rekordbox 7 `.DAT` files [ME]: the per-file maximum height is **25** in 1320
files and never higher, so rekordbox normalises the loudest column to 25, not 31. Whiteness values
are only **0..5** (never 6 or 7).

Derivation (approximation fitted by [BK], not byte-exact): split the detail columns into 400
consecutive ranges (`k*n/400 .. (k+1)*n/400`); height = `round(24 * rms_k / max_k rms)`; whiteness of
column k = mean whiteness of the detail columns in range `k/2` of a 200-way split (whiteness defined in §4.9).
The real per-file maximum of 25 (above) suggests rekordbox's scale is 25 rather than [BK]'s 24.
**UNCERTAIN**.

### 4.8 `PWV2`: tiny preview, 100 columns (`.DAT`)

Sources: [DS] §Tiny Waveform Preview, [BK], [ME].

Same layout as PWAV with len_data = 100 (len_tag 120). Each byte: bits 0–3 height 0..15, **bits
4–7 zero** (verified [ME]: high nibble always 0). Used by the pre-nexus CDJ-900 [DS].
[BK] approximation: `round(15 * sqrt(rms_k / max rms))` over a 100-way split.

* [RBOX] declares the bitfield as `unused:B4, height:B4` in LSB-first order, which would put the height in
  the **high** nibble. Real data contradicts it.

### 4.9 `PWV3`: monochrome scrolling detail, 150 columns/s (`.EXT`)

Sources: [DS] §Waveform Detail, [DJL], [BK] (**byte-exact rule**), [ME].

| Off | Size | Value |
|---|---|---|
| 0x00 | 4 | `"PWV3"` |
| 0x04 | 4 | `0x18` |
| 0x08 | 4 | `0x18 + n` |
| 0x0C | 4 | len_entry_bytes = `1` |
| 0x10 | 4 | n = number of columns = `ceil(total_samples * 150 / rate)` |
| 0x14 | 4 | `0x00960000` (0x96 = 150) |
| 0x18 | n | entries: bits 0–4 height, bits 5–7 whiteness |

**How rekordbox computes it** ([BK], reproduces rekordbox 7.2 byte for byte on 99.5 % of
columns over 1070 tracks):

```
x[t]          = (L[t] + R[t]) / 2   (mono mix; generally mean over channels)
peak_i        = max |x[t]| over column i
lp_i          = max |LP150(x)[t]| over column i
                LP150 = 2nd-order RBJ low-pass, fc = 150 Hz, Q = 1/sqrt(2), run continuously
track_peak    = max_i peak_i
height_i      = min(31, floor(31.5 * (peak_i / track_peak)^2))
whiteness_i   = (peak_i == 0) ? 7 : 7 - min(7, floor(8 * clamp(lp_i / peak_i, 0, 1)))
byte_i        = (whiteness_i << 5) | height_i
```

So height is peak amplitude, squared and normalised to the whole track's peak. Whiteness is
inversely related to how much of the column's peak is below ~150 Hz: bass gives dark blue (0–1)
and treble gives white (7). The rekordbox sweep fixture confirms this [ME] (frequencies assume a log sweep): white = 7 at ~900 Hz
and up, 1 at ~110 Hz, 0 at ~40 Hz. Silence is encoded as `0xE0` (whiteness 7, height 0).

### 4.10 `PWV4`: colour preview, 1200 columns × 6 bytes (`.EXT`, NXS2)

Sources: [DS] §Color Preview, [DJL] §color-preview-analysis, [BL], [PRB], [BK], [ME].

| Off | Size | Value |
|---|---|---|
| 0x00 | 4 | `"PWV4"` |
| 0x04 | 4 | `0x18` |
| 0x08 | 4 | `0x18 + 7200` = 7224 |
| 0x0C | 4 | len_entry_bytes = `6` |
| 0x10 | 4 | len_entries = `1200` |
| 0x14 | 4 | `0` ([RBOX] asserts 0; [ME] confirms) |
| 0x18 | 7200 | 1200 × 6 bytes |

Byte meanings (bytes 2–5 are 0..127 in practice, and [PRB] masks them with 0x7F; bytes 0–1 can exceed 127,
e.g. 154 and 205 in the sweep):

| Byte | Meaning |
|---|---|
| 0 | unknown, "whiteness"-related [DJL]. Behaves like an overall level: in the rekordbox sweep it is 102 for the stereo part and 51 for the single-channel parts, constant within each |
| 1 | unknown, inversely related to byte 0 (sweep: 154 / 205, i.e. byte0 + byte1 = 256 there). [BK] fit on music: `b1 ≈ 203 − 0.49·b0` |
| 2 | energy in the lower part of the spectrum ([DJL]: "bottom half, drops ~10 kHz"; the sweep, assuming a log sweep, shows it rising only below ~1 kHz) **UNCERTAIN** |
| 3 | low-band energy |
| 4 | mid-band energy |
| 5 | high-band energy; also the height of the front (brighter) layer in [BL] |

The band assignment of bytes 3–5 is confirmed by the rekordbox sweep [ME]: byte 5 dominates at the
high-frequency start, byte 4 at mid, byte 3 at low.

[BL] rendering: back height = max(b3,b4,b5); front height = b5; colour =
`(b3, b4, b5) * maxLevel / backHeight` with maxLevel 255 (front) or 191 (back).

[BK] generator (approximate, fitted on music): `b3..b5 = round(PWV6_band * [2.4, 1.6, 1.1])`,
`b2 = 0.91 * sqrt(b3² + b4² + b5²)`, `b0 = 0.88 * max(b3,b4,b5) + 38`, `b1 = 203 − 0.49 * b0`,
all clamped to 0..127 (b1 to 0..255). The rekordbox sweep does **not** follow these factors (e.g.
PWV4 mid = 55 where PWV6 mid = 78). They are fits, not rekordbox's algorithm. **UNCERTAIN**.

### 4.11 `PWV5`: colour scrolling detail, 150 columns/s × 2 bytes (`.EXT`, NXS2)

Sources: [DS] §Color Detail, [DJL], [BL], [BK], [ME].

| Off | Size | Value |
|---|---|---|
| 0x00 | 4 | `"PWV5"` |
| 0x04 | 4 | `0x18` |
| 0x08 | 4 | `0x18 + 2n` |
| 0x0C | 4 | len_entry_bytes = `2` |
| 0x10 | 4 | n (same n as PWV3 and PWV7) |
| 0x14 | 4 | `0x00960305` |
| 0x18 | 2n | big-endian u16 per column |

```
bit 15..13  "red"   = low-band intensity   (0..7)
bit 12..10  "green" = mid-band intensity   (0..7)
bit  9.. 7  "blue"  = high-band intensity  (0..7)
bit  6.. 2  height  (0..31), close to the PWV3 height but NOT identical (see below)
bit  1.. 0  0
```

* Height vs PWV3 [ME]: per-column equal in 68 % (demo track 1), 91 % (demo track 2) and 97 % (sweep) of
  columns. The rest differ in both directions by up to ±29. It is not a time shift (any offset ≠ 0
  matches worse). PWV5 height ≤ PWV3 height in 83 % of columns. [BK] simply reuses the PWV3 height, and
  CDJs accepted it. rekordbox's exact rule is **UNCERTAIN**.

* The band semantics come from the rekordbox sweep [ME] (frequencies assume a log sweep): pure bass gives `0xE07C` (r=7, h=31), ~3–7 kHz gives
  `0x03FC`/`0x0BFC` (b=7), ~300 Hz gives `0x1C7C` (g=7). The low two bits were 0 in all 25866 + 19208 + 22500
  columns examined. [RC] calls them "fine height sub-steps" without evidence. Write 0.
* [RC] and [RBOX] decode this with an LSB-first `modular_bitfield` (`red:B3` in the *low* bits of
  byte 0). That does **not** match real data (it would decode a pure-bass column as r=0, g=4, b=3). Use the
  big-endian layout above ([DS], [BL]).
* [BL] builds `Color(red, blue, green)`, i.e. it maps bits 12–10 to screen blue and bits 9–7 to
  screen green. That is a rendering choice of beat-link, not file semantics. **UNCERTAIN** whether CDJs do the same.
* [BK] generator (approximate): per column, band envelopes `e_low, e_mid, e_high` (filters in §4.12);
  `level(b) = round(7 * sqrt(b / max(e)))`; `u16 = level(low)<<13 | level(mid)<<10 |
  level(high)<<7 | height<<2`.

### 4.12 `PWV7`: 3-band scrolling detail (`.2EX`, CDJ-3000)

Sources: [DS] §3-Band Detail, [DJL], [RC], [BK], [FF], [ME].

| Off | Size | Value |
|---|---|---|
| 0x00 | 4 | `"PWV7"` |
| 0x04 | 4 | `0x18` |
| 0x08 | 4 | `0x18 + 3n` |
| 0x0C | 4 | len_entry_bytes = `3` |
| 0x10 | 4 | n (same as PWV3/PWV5) |
| 0x14 | 4 | `0x00960000` |
| 0x18 | 3n | per column: 3 one-byte band levels, 0..127 |

**Band order: CONFLICT. Measurement favours (low, mid, high).**

* [DS]/[DJL]/[BL]/[RBOX]: bytes are **(mid, high, low)**. beat-link draws LOW from byte 2 (×0.4),
  MID from byte 0 (×0.3), HIGH from byte 1 (×0.06).
* [RC] (sweep test), [BK] (writer, music-fitted), [FF]: bytes are **(low, mid, high)**.
* [ME], on the rekordbox-analysed 20 kHz→20 Hz sweep (rekordcrate fixture): at the high-frequency start the
  columns are `[0,0,50]`, `[0,2,118]`, mid-sweep `[8,78,1]`, at the bass end `[81,17,0]`, `[80,0,0]`. So
  **byte 0 = low, byte 1 = mid, byte 2 = high**. Write that order.

Value range: 0..127 (127 seen as max) [ME] [BK]. Display colours [DS]: low = dark blue, mid =
amber/orange, high = white. The detail view overlays all three on one axis (low/mid overlap
drawn brown, high drawn last, on top), while the preview stacks them.

[BK]'s generator (approximation fitted to rekordbox 7.2):

```
filters on the mono mix, continuous over the track (RBJ biquads, Q = 1/sqrt(2)):
  low  = LP(300 Hz)
  mid  = LP(1500 Hz) ∘ HP(250 Hz)
  high = HP(2500 Hz)
band_peak[i][j] = max |filtered_j| over column i
env[i][j]       = max(band_peak[i][j], env[i-1][j] * release[j]),  release = {0.97, 0.93, 0.90}
gain[j]         = clamp(round(target[j] / max_i env[i][j]), floor[j], cap[j])
                  target = {78, 105, 115}, floor = {80, 80, 95}, cap = {267, 300, 500}
byte[i][j]      = min(127, round(scale[j] * gain[j] * env[i][j])),  scale = {1.25, 1.15, 0.6}
```

The rekordbox sweep (assuming a logarithmic sweep) puts the effective crossovers near ~300 Hz and
~2.5 kHz, and the high band falls off again above ~10 kHz (`[0,0,2]` at ~16 kHz) [ME]. **UNCERTAIN**.

**What cyanbox writes: [RBX]'s rule**, not [BK]'s (since packed waveform format version 4,
`src/analysis/waveform.c`). [BK]'s levels drew a loud master's overview almost all cream (format
version 1); refitted to rekordbox's own PWV7 (versions 2 and 3) they gave, as rekordbox's files
have, a high band whose mean is 0.13 of the low band's: drawn, a blue waveform with a thin cream
core. [RBX]'s draws the lows, mids and highs all visibly, which is the picture wanted:

```
one-pole low-passes on the mono mix, continuous over the track, in double:
  lp_f(x)[i] = x[i] * (1 - c) + lp_f(x)[i-1] * c,   c = exp(-2 pi f / rate)
  low  = lp_200(x)
  mid  = lp_2000(x) - lp_200(x)
  high = x - lp_2000(x)
band_peak[i][j] = max |band_j| over column i                (columns as in §4.9)
byte[i][j]      = floor(255 * min(1, band_peak[i][j])) >> 1  (0..127)
```

So a full-scale peak is 127 and every track is drawn at its own level (a quiet track is a small
waveform), where rekordbox's PWV7 reaches 126..127 in every band of its two demo tracks [ME] and
127 on all 80 tracks [RBX] measured.
The one-pole filters overlap widely: a steady 1 kHz tone at 0.5 writes `[12, 50, 24]`, 100 Hz
`[57, 25, 2]`, 8 kHz `[1, 14, 53]` at 44.1 kHz. rbxport cuts columns of `floor(rate / 150)`
samples in float; cyanbox keeps the grid of §4.9 in double, and on the same samples its bytes
equal rbxport's on all but 0..3 in 100k (the float rounding) at 44.1 and 48 kHz.

### 4.13 `PWV6`: 3-band preview, 1200 columns (`.2EX`)

Sources: [DS] §3-Band Preview, [DJL], [BK], [ME].

| Off | Size | Value |
|---|---|---|
| 0x00 | 4 | `"PWV6"` |
| 0x04 | 4 | `0x14` (note: no unknown word, unlike PWV7) |
| 0x08 | 4 | `0x14 + 3600` = 3620 |
| 0x0C | 4 | len_entry_bytes = `3` |
| 0x10 | 4 | len_entries = `1200` |
| 0x14 | 3600 | per column: 3 band bytes, **same order as PWV7** (low, mid, high per [ME]; the [DS] conflict as above) |

[BK] fit: PWV6 = mean of the PWV7 columns in the range × `{0.56, 0.71, 2.0}`, clamped 0..127. My
measurement of Σ PWV6 / Σ (range mean of PWV7) per band [ME]: demo track 1 `{0.45, 0.75, 2.36}`, demo
track 2 `{0.47, 0.48, 2.10}`, sweep `{0.82, 0.97, 1.29}`. So PWV6 is not a fixed rescale of PWV7. It is
probably computed with its own per-track normalisation; the [BK] factors are a reasonable music average.
**UNCERTAIN**. [DS]: the preview draws the three bands *stacked*.
[BL] heights: low×0.49, +mid×0.32, +high×0.25 on a 0..~31 pixel scale.

**What cyanbox writes: [RBX]'s overview**, measured on the samples rather than taken from PWV7
("measured against nine rekordbox references" there; an approximation, not rekordbox's algorithm):

```
1200 columns of samples: column b holds samples [b * N / 1200, (b + 1) * N / 1200)  (N samples)
  low  = (mean of lp_200(x)^2 over the column) ^ 0.45     (lp_200 as in §4.12)
  mid  = mean |LP_2000(HP_200(x))| over the column        (RBJ biquads, Q = 1/sqrt(2))
  high = mean |HP_2000(x)| over the column
  (corners at most 0.45 * rate; an empty column is 0)
v[b] = (raw[b] + raw[b-1]) / 2,  raw[-1] = raw[0]         (two-column trailing average)
target  = 92 * mean over the detail columns (§4.9) of the column's RMS
byte[b][j] = clamp(round_half_away(v[b][j] * target / mean_b(raw[b][j])), 0, 127)
             (0 if that band's mean is below 1e-6)
```

Every band therefore has the same mean over the track (92 times its mean RMS: 20..35 on the
dance tracks of cyanbox's acceptance library, about 3 on a track at -26 dB RMS), and the overview
shows where each band is loud, whatever the mix's balance. cyanbox's bytes equal rbxport's exactly
on the same samples. rbxport's interface draws the deck overview stacked from the baseline, as
its row preview (low/128, then mid/256, then high/128: its default "half" Full/Preview Waveform
setting); drawn centred, the three equal bands overlap into a mostly cream core.

### 4.14 `PWVC`: 3-band gain calibration (`.2EX`)

Sources: [DJL] §vocal config, [RC] (`Waveform3BandCalibration`), [BK], [PRB], [ME].

| Off | Size | Value |
|---|---|---|
| 0x00 | 4 | `"PWVC"` |
| 0x04 | 4 | `0x0E` (14) |
| 0x08 | 4 | `0x14` (20) |
| 0x0C | 2 | `0` |
| 0x0E | 2 | low gain |
| 0x10 | 2 | mid gain |
| 0x12 | 2 | high gain |

* Meaning: **CONFLICT**. [DJL] calls these "vocal detection thresholds (low, mid, high)", observed
  80–114, 80–146, 98–159. [RC] and [BK] call them per-band **gains** for the 3-band waveform. [BK]'s
  generator produces them as the gain used to normalise PWV7 (floors 80/80/95, caps 267/300/500).
  Real values [ME]: low is almost always 80 (`0x50`), mid ~80–200, high ~80–500 (`0x1F4` seen;
  demo: `5B 8A 18C`, `91 F9 1F4`; sweep: `50 58 77`). These ranges exceed the thresholds [DJL] observed,
  which supports the "gain" reading.
* Writers: emit the gains you used to scale PWV7 (the [BK] rule), or `80, 100, 100` when unsure.
* cyanbox writes [BK]'s gains, computed as [BK] does, although its PWV7 is [RBX]'s and not scaled by
  them (§4.12). [RBX] authors no PWVC (only PWV6 and PWV7 in `.2EX`) and carries a rekordbox-written
  one through unchanged next to its own PWV7, so its sticks pair rekordbox's per-track gains with a
  fixed-scale PWV7 too. [BK]'s gains are in rekordbox's range (seabass's ~2100 `.2EX`, median
  80/108/109), and no reader is known to scale PWV7 by them.

### 4.15 `PVB2`: FLAC seek table (`.EXT`, FLAC only)

Sources: [PRB] structs.py + docs, [BK] flac.rs (matches rekordbox byte for byte on 508/508 FLACs).

| Off | Size | Value |
|---|---|---|
| 0x00 | 4 | `"PVB2"` |
| 0x04 | 4 | `0x20` |
| 0x08 | 4 | `0x20 + 20 * count` |
| 0x0C | 4 | `0` |
| 0x10 | 8 | total_samples (from STREAMINFO) |
| 0x18 | 4 | count (400) |
| 0x1C | 4 | entry size = `20` |
| 0x20 | 20 × count | entries |

Entry: `u64 first_sample, u64 byte_offset, u32 block_size`.

Rule [BK]: `step = total_samples / 400` (integer). Entry `i` describes the FLAC **frame that contains
sample `i*step`**: that frame's first sample, its byte offset **measured from the first audio
frame** (i.e. from the end of the metadata blocks), and its block size. Consecutive entries can repeat
the same frame.

* **CONFLICT** on offset base: [PRB] says "from the start of the audio file". [BK] (byte-exact on
  508 files) says from the first frame. Use [BK].
* **CONFLICT** for short tracks: [PRB] says tracks of ≤ 400 frames get one entry per frame. [BK]
  always produces 400 entries. **UNCERTAIN**.
* [FF] says "PVB2 8032 bytes, all zeros for lossless". That is contradicted by [BK]/[PRB]. Only FLAC
  gets PVB2, and it is a real table.
* Gotcha [BK]: the table is tied to the exact bytes of the file. Re-encoding or retagging a FLAC
  (anything that shifts frames) makes it stale. rekordbox copies the old table on re-export, so
  regenerate it.

### 4.16 `PSSI`: song structure / phrases (`.EXT`, rekordbox ≥ 6 exports; used by CDJ-3000)

Sources: [DS] §Song Structure, [KSY], [PRB], [RC] xor.rs, [BK] pssi.rs, [ME] (1983 real tags).

| Off | Size | Value |
|---|---|---|
| 0x00 | 4 | `"PSSI"` |
| 0x04 | 4 | `0x20` |
| 0x08 | 4 | `0x20 + 24 * n` |
| 0x0C | 4 | len_entry_bytes = `0x18` (24) |
| 0x10 | 2 | n = len_entries |
| 0x12 | 2 | mood: 1 high, 2 mid, 3 low (**masked from here on**) |
| 0x14 | 6 | zeros (all 1983 real tags) |
| 0x1A | 2 | end_beat: beat number where the last phrase ends |
| 0x1C | 2 | unknown; observed `0x0101` (1188×), `0x0001` (731×), `0x0100` (40×), `0x0000` (24×) |
| 0x1E | 1 | bank (Lighting mode): 0 default(=Cool), 1 Cool, 2 Natural, 3 Hot, 4 Subtle, 5 Warm, 6 Vivid, 7 Club 1, 8 Club 2. [DS] has also seen `0xF6`/`0xF9`; treat values > 8 as unknown. All real files examined: 0 |
| 0x1F | 1 | `0` |
| 0x20 | 24 × n | phrase entries |

Phrase entry (24 bytes):

| Off | Size | Meaning |
|---|---|---|
| 0x00 | 2 | index, 1-based, incrementing |
| 0x02 | 2 | beat: 1-based beat number (index into PQTZ) where the phrase starts. It runs until the next entry's beat or end_beat |
| 0x04 | 2 | kind (see tables) |
| 0x06 | 1 | 0 |
| 0x07 | 1 | k1 |
| 0x08 | 1 | 0 |
| 0x09 | 1 | k2 |
| 0x0A | 1 | 0 |
| 0x0B | 1 | b: 0 means only beat2 is used; 1 means beat2, beat3 and beat4 are used ("Up 3" only) |
| 0x0C | 2 | beat2 |
| 0x0E | 2 | beat3 |
| 0x10 | 2 | beat4 |
| 0x12 | 1 | 0 |
| 0x13 | 1 | k3 |
| 0x14 | 1 | 0 |
| 0x15 | 1 | fill: non-zero if the phrase ends with fill-in beats |
| 0x16 | 2 | beat_fill: beat where the fill starts |

Phrase kind labels [DS]:

| kind | low mood (3) | mid mood (2) | high mood (1) |
|---|---|---|---|
| 1 | Intro | Intro | Intro 1/2 (k1) |
| 2 | Verse 1 | Verse 1 | Up 1/2/3 (k2,k3) |
| 3 | Verse 1 | Verse 2 | Down |
| 4 | Verse 1 | Verse 3 | – |
| 5 | Verse 2 | Verse 4 | Chorus 1/2 (k1) |
| 6 | Verse 2 | Verse 5 | Outro 1/2 (k1) |
| 7 | Verse 2 | Verse 6 | – |
| 8 | Bridge | Bridge | – |
| 9 | Chorus | Chorus | – |
| 10 | Outro | Outro | – |

High-mood variants [DS]: kind 1: k1=1 → Intro 1, k1=0 → Intro 2. kind 2: (k2,k3) = (0,0) Up 1, (0,1)
Up 2, (1,0) Up 3. kind 6: k1=1 → Outro 1, k1=0 → Outro 2. **kind 5: CONFLICT**. [DS] says k1=1 → Chorus 1 and
k1=0 → Chorus 2; [PRB] docs say the reverse.

**XOR masking.** rekordbox keeps PSSI in clear text in its local analysis files and masks it when it
**exports** to a stick [BK] (verified 308/314 tracks; the other 6 had been re-analysed). Every
PSSI in the real exports I examined was masked [ME]:

```c
static const uint8_t base[19] = {0xCB,0xE1,0xEE,0xFA,0xE5,0xEE,0xAD,0xEE,0xE9,0xD2,
                                 0xE9,0xEB,0xE1,0xE9,0xF3,0xE8,0xE9,0xF4,0xE1};
uint16_t n = be16(tag + 0x10);                 /* len_entries, NOT masked */
for (size_t i = 0; 0x12 + i < len_tag; i++)
    tag[0x12 + i] ^= (uint8_t)(base[i % 19] + n);  /* mod 256; involution */
```

Readers detect masking by checking that the raw mood at 0x12 is in 1..3 ([KSY] uses `raw_mood > 20`).
Whether players *require* the mask is **UNCERTAIN**: no test without it is known. Write it masked, as
rekordbox does. Phrase analysis cannot be generated from known rules; [BK] omits PSSI for
self-generated analyses and the CDJ-3000 accepted the files.

### 4.17 `PVDI`: vocal detection (`.2EX`, some rekordbox 7 files)

Sources: [PRB] (`PVDI = u1:u32, u2:u32, len_confidence:u32, confidence[len]`), [ME].

`len_header 0x18`. In the 21 instances examined: `len_tag 0x18`, words `0x00000400`, `0x56220001`, then
`len_confidence = 0` (the fixture was anonymised, so it may have been stripped). Not needed by any known
player. **UNCERTAIN**. Omit it.

---

## 5. Which players use which tags; what happens if something is missing

Sources: [DS], [DJL], [FF] (CDJ-3000 fw 3.19), [BK] (CDJ-3000 fw 3.22, CDJ-2000NXS2 1.85/1.87),
[SB] (XDJ-RX2 1.43, CDJ-3000X, OMNIS-DUO), [RC] `setting.rs` (DEVSETTING waveform colour),
rekordbox OneLibrary guide <https://cdn.rekordbox.com/files/20260317132052/OneLibrary-Compatible-USB-Device-Export-.pdf>.

| Tag | Consumer |
|---|---|
| PPTH | everything. Path must match the hashed directory and be NUL-terminated [FF] |
| PVBR | MP3 playback/seeking on all players |
| PQTZ | beat grid, quantize, sync, beat counters (all players) |
| PWV2 | pre-nexus CDJ-900 overview [DS] |
| PWAV | nexus-era blue overview (CDJ-2000nexus etc.) |
| PWV3 | blue scrolling waveform (nexus; later players in "BLUE" waveform colour) |
| PCOB (.DAT) | hot cues A–C, memory cues: pre-NXS2 players; **also shown by XDJ-RX2 and CDJ-3000X** [SB] |
| PCOB (.EXT) | hot cues D–H in legacy form [SB] [BK] |
| PCO2 | NXS2 and later: cue colours, comments, A–H, quantized loop sizes |
| PQT2 | unknown consumer; empty form accepted [BK] |
| PWV4 / PWV5 | NXS2-style RGB overview / scrolling waveform (DEVSETTING waveform colour = RGB) |
| PWV6 / PWV7 / PWVC | CDJ-3000-style 3-band waveforms (DEVSETTING waveform colour = 3Band) |
| PSSI | CDJ-3000 phrase display ("waveform divisions = phrase", MYSETTING2) and Lighting; rekordbox Performance/Lighting |
| PVB2 | FLAC seeking (players that play FLAC: NXS2 and later) |
| PVDI | none known |

Failure behaviours that have been observed:

* `.EXT` missing → **CDJ-3000 re-analyses every track on load** [FF].
* `.DAT` missing → re-analysis [FF].
* PPTH without NUL (or path mismatch) → file rejected, player writes `ANLZ0001.DAT` [FF].
* wrong `P###/########` directory → analysis never found [FF] [BK].
* `PQTZ` with 0 beats → player detects BPM on the fly; quantize/sync unavailable [BK].
* `PSSI` absent → no phrase data; tracks still load and play (CDJ-3000 3.22) [BK].
* `.2EX` absent on CDJ-3000 → [FF] claims it "works but shows lower-quality waveforms" (presumably falls back to
  RGB/blue). For OneLibrary players [FF] calls `.2EX` "required". Both claims are **UNCERTAIN**: no
  hardware report is cited.
* Cue edits only in PCO2 → not visible on XDJ-RX2 / CDJ-3000X [SB].
* PCOB memory list with entries but `memory_count = 0xFFFFFFFF` → XDJ-RX2 drops the cue and hangs on
  the next memory-loop save [SB].
* Players that analyse locally (CDJ-3000) write their own ANLZ files with `PMAI` header words zeroed,
  `PCOB` memory_count `0`, and `PWV2` present. The CDJ accepts both its own form and rekordbox's [FF].

---

## 6. Writer checklist

1. Compute the directory from the exact UTF-16 path string you store in the DBs (§1.1); number
   collisions `ANLZ0000`, `ANLZ0001`, … in export order.
2. Write all three files for every track, each starting with the identical `PPTH` (NUL-terminated).
3. Use the per-tag `len_header` constants: PMAI 0x1C, PPTH 0x10, PVBR 0x10, PQTZ 0x18, PQT2 0x38,
   PWAV/PWV2 0x14, PWV3/4/5/7 0x18, PWV6 0x14, PWVC 0x0E, PCOB 0x18, PCPT 0x1C, PCO2 0x14, PCP2 0x10,
   PSSI 0x20, PVB2 0x20, PVDI 0x18.
4. Use the fixed words exactly: PQTZ `0x00080000`; PWAV/PWV2/PCPT `0x00010000`; PWV3/PWV7 `0x00960000`;
   PWV5 `0x00960305`; PWV4 `0`; PQT2 `0x01000002`; PMAI tail `1, 0x10000, 0x10000, 0`.
5. Keep n identical across PWV3/PWV5/PWV7 (`ceil(samples*150/rate)`); PWAV 400, PWV2 100, PWV4/PWV6 1200.
6. Cues: hot A–C into `.DAT` PCOB, D–H into `.EXT` PCOB, all into PCO2. Set memory_count and prev/next links
   correctly. Mark at most one active loop (status 4, `.DAT` memory list only).
7. PVBR: CBR MP3 → zeros + frames×1152 trailer; non-MP3 → zeros. Avoid VBR MP3 or accept unknown behaviour.
8. FLAC: add PVB2 built from the final file bytes.
9. PSSI: only if you have real phrase data; mask it.
10. Values in the 3-band and colour tags: keep within 0..127.

---

## 7. Open questions (collected)

* VBR MP3 `PVBR` index format.
* `PQT2` entries and the word at 0x2C.
* `PWV4` bytes 0–2 exact definitions; exact rekordbox filters for PWV4/5/6/7 (only fitted approximations are public).
* `PWVC`: gain vs vocal threshold (evidence favours gain).
* PWV6/PWV7 band order: [DS]/[BL] (mid, high, low) vs measured (low, mid, high).
* High-mood "Chorus 1/2" k1 mapping.
* Whether players require PSSI masking.
* LED RGB for each hot-cue colour code.
