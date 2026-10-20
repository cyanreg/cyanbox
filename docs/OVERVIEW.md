# cyanbox: overview and plan

cyanbox is a DJ library manager and stick writer in C: the `dj` Python tool ported
to C23, with a rekordbox-style interface (SDL3, Vulkan, Dear ImGui), PipeWire
playback, and a new beat-grid analyser. It never needs rekordbox, AlphaTheta
software or a rekordbox database.

This document is the high-level survey written before any code: what the two
earlier attempts are, what each got right, what cyanbox keeps, takes and fixes,
and how it is built. Format details stay in `docs/pdb.md`, `docs/anlz.md`,
`docs/onelibrary.md` and `docs/settings.md` (carried over from dj).

## 1. The two earlier attempts

### dj (`~/projects/dj`, Python, ~3.8k lines)

A command-line stick writer. Analyses tracks (grid, key, waveforms), keeps the
library as plain text (`dj.toml`, one TOML sidecar per track, m3u8 playlists)
and writes everything a player reads: `export.pdb`, the OneLibrary
`exportLibrary.db` (SQLCipher), ANLZ `.DAT/.EXT/.2EX`, artwork, My Settings.

What it got right, and cyanbox keeps:

- **Byte-level format fidelity**, checked against real rekordbox files:
  export.pdb fixed tables byte-identical to rekordbox 7.2.18, settings files
  byte-identical to 7.2.8, the ANLZ directory hash, the OneLibrary schema and
  SQLCipher encryption, the blue waveform rule (99.5 % of columns).
- **Plain-text library you can keep in git.** Every correction and cue is in a
  small TOML file; analysis only fills in missing sections.
- **MP3 timing on the players' timeline** (no encoder-delay trimming).
- **Both libraries always written**, so one stick works on every player
  generation.
- A small, readable codebase with real tests and fixtures.

What it lacks, and cyanbox adds:

- No interface. Cues had to be typed into TOML files; waveforms could not be
  seen; nothing could be auditioned.
- Constant-tempo grids only, hand-split into sections.
- Several format findings that came out of rbxport after dj was written
  (section 3).

### rbxport (`~/projects/rbxport`, Rust + React/Tauri, ~80k lines, GPL)

A rekordbox replacement that edits a rekordbox `master.db`, exports sticks and
serves PRO DJ LINK. Its interface is a web view in a Tauri shell, which is what
makes it heavy and hard to trust. It is useful mostly as a source of measured
facts: its authors checked formats against rekordbox binaries and against
AlphaTheta firmware in an emulator.

What is worth taking from it (facts, not code; it is GPL):

- The MP3 timeline rule (`crates/rbl-core/src/mpeg.rs`): a leading Xing/Info
  frame counts as one frame of silence unless one of the first four frames
  changes MPEG version, layer, rate or channel mode. Verified on 200 files.
- Preview waveform minimums: `PWAV` height never below 2, `PWV2` never below 1.
  Nexus firmware rejects a preview with a zero column.
- rekordbox's complete 64-entry hot-cue colour palette
  (`crates/rbl-anlz/src/cues.rs`, `DEVICE_PALETTE`), and the default pad colours.
- The measured 3-band waveform colours and stacking scales (section 6), and
  its three-band analysis, which cyanbox writes as PWV7/PWV6 (section 3).
- The rekordbox keyboard map and the deck's control semantics (memory cue
  tolerance, beat-jump sizes, zoom steps in bars).
- Its beat-grid *rules* (kicks establish tempo, cuts need support, structure
  identifies beat 1) are sensible, but its implementation is 5k lines tuned on
  private playlists and is slow. cyanbox writes its own (section 5).

Where dj is right and rbxport is wrong, and cyanbox follows dj: OneLibrary hot-cue
`kind` numbering (A–H are 1–8), loop end for non-loops (repeat the start),
the RGB and blue waveform byte fits (PWAV, PWV2, PWV3, PWV4, PWV5).

## 2. What cyanbox is

One binary, `cyanbox`:

```
cyanbox                      the interface
cyanbox add <path>...        add files or folders to the library and analyse them
cyanbox analyze [paths]      (re)analyse; by default only what is missing or stale
cyanbox show <track>         print the resolved grid, key and cues
cyanbox export <stick>       write a stick (both libraries, ANLZ, artwork, presets)
cyanbox inspect <stick>      re-read and validate a stick
cyanbox sync <stick>         review what the player wrote on the stick (cues, history, presets); --apply
cyanbox pull <stick>         sync and accept everything
cyanbox settings             list the My Settings names and values
```

The library is one SQLite database, `library.db`, in a directory
(`$CYANBOX_LIBRARY`, `-c DIR`, default `~/.local/share/cyanbox`). It holds
everything: file facts and tags, the user's overrides, grid, key, cues,
rating, colour, My Tags, play counts and history, the waveforms (the
previews the browser draws kept apart from the detail only the players
and the export read), the artwork thumbnails (each cover once, however
many tracks have it: an album's tracks share one, and a cover goes with
its last track), playlists and folders, and the settings. A track whose
file is missing stays in the library, greyed out, until the file returns or
is relocated. Every track has an id that never changes; that id is the track
id on every stick, so players keep their cached artwork and waveforms across
exports. dj's sidecar files are not read or written: nothing was ever
analysed with dj, and the user does not want the compatibility.

A track's colour is one of rekordbox's eight, kept as its hue in degrees
(the HSV hue angle of the RGB rekordbox draws it in, `cyanbox/colors.h`), or
none; a hue between them is removed when the library opens, as anything
else it holds that is not valid. The interface and the tagging model pick
one of the eight and store its hue: the model by which of a
legend of descriptions, one per colour and the user's to edit, fits the
track best. The comment is the user's: a
file's comment tag is left out on import unless the `import.comment`
setting asks for it, so the field starts empty for the DJ and for tagging
later. cyanbox never writes to the audio files.

After a gig, `cyanbox sync STICK` (and File → Sync from USB in the interface)
reads back what the player wrote to the stick: hot cues and memory cues it
saved into the analysis files, the play history it appended, and My
Settings it saved. The interface shows the differences for review; accepted
ones go into the library.

## 3. Fixes carried in from the earlier comparison

All of the items the earlier session listed as worth importing are in scope:

1. MP3 timeline rule (above). Grids and cues on LAME files were 26 ms early.
2. Preview waveform minimums (`PWAV` ≥ 2, `PWV2` ≥ 1).
3. The 64-entry hot-cue colour table and default pad colours.
4. Stable track ids across exports (via the library id).
5. Checks before writing anything: free space on the stick, no track row too
   long for a DeviceSQL page (named, not a crash).
6. `cyanbox sync` / `pull`: read player-saved cues, play history and presets
   from the stick back into the library, matched by track id, with review.
7. More `inspect` checks: ANLZ embedded path matches the audio, folder matches
   the path hash, fixed track-row values, both libraries agree on hot-cue
   auto-load, declared cue counts match parsed ones.
8. Optional background colour for the stick's library (`export.background_color`,
   the Export dialog's swatches).
9. Damaged MP3 frames play and analyse as silence rather than being dropped.
10. The three-band waveform levels are rbxport's analysis
    (`crates/rbl-analysis/src/waveform.rs`), not dj's fit: PWV7 is each band's
    peak over the column through one-pole crossovers at 200 Hz and 2 kHz, at
    the track's own level; PWV6 measures energy per band and normalises every
    band to the same mean, 92 times the track's mean RMS. dj's levels drew
    loud masters all cream, and fits to rekordbox's own bytes a blue waveform
    with a thin cream core; rbxport's shows the blue lows, amber mids and
    white highs on every kind of track, which is the picture the user wants
    (docs/anlz.md 4.12, 4.13; `src/analysis/waveform.c`).

Plus: the stick's format (`export.format`: FLAC, WAV for the players before
the CDJ-2000NXS2, or MP3), every track encoded from its decoded samples at
16-bit/44.1 kHz as it is written (cb_encode).

## 4. Architecture

```
src/core      model, grid arithmetic, the SQLite library (tracks, cues, waveforms, playlists, settings), notation
src/audio     one FFmpeg path on the players' timeline (libavformat, libavcodec, libswresample) as pipelines of chunks, decodes to memory, encoding, probe, tags, artwork, MP3/FLAC scanners
src/analysis  spectral front end (libavutil av_tx, or the GPU), onset, tempo, grid, downbeat, key, waveforms
src/formats   export.pdb, ANLZ read/write, OneLibrary (sqlite3 + SQLCipher pages), My Settings, colours
src/export    stick plan, writer, inspect, pull
src/player    PipeWire stream, the player, lock-free command queue, the decks' decoding threads, cue/loop/quantize controls, MASTER TEMPO, null backend for tests
src/gpu       SDL3 window (Wayland), Vulkan device + VMA, swapchain, Wayland frame-callback pacing, ImGui glue, waveform shader;
              the compute context without a window and the analysis' STFT and envelope shaders (cb_compute);
              the Vulkan set-up the window, the compute context and the model share, each a device of its own (cb_vkdev)
src/ml        MOSS-Audio-4B on Vulkan compute, a device of its own: a track's genre, colour and comment, only when asked (docs/ML.md)
src/ui        Dear ImGui screens (C++, as are only vendored code and the player's Signalsmith Stretch interface)
src/cli.c     the subcommands
```

Decisions and why:

- **C23, meson, one binary.** The user's house style (libcraw). C++ is used only
  for the ImGui-facing UI files, the vendored VMA/ImGui and the small C
  interface to Signalsmith Stretch's templates; everything else,
  including every rule about formats and timing, is C and testable without a
  window.
- **Vulkan through VMA, SDL3 only for the window and input.** The Vulkan
  boilerplate (instance, device, VMA allocator, buffers, images) is taken from
  libcraw's `tests/vkdev.c`, `lib/vk_*.c` and `tools/gui/gpu.c`. No libplacebo:
  the picture is UI, not video, and sRGB is enough.
- **Wayland frame callback.** The main loop draws only when the compositor has
  asked for a frame (`wl_surface.frame` on a private event queue, the way
  libcraw's `colour.c` wraps the display) *and* something changed. A window
  nobody can see gets no frames and the loop just services audio and events.
  MAILBOX presentation when offered, FIFO otherwise.
- **Waveforms are drawn by a fragment shader** reading the ANLZ column bytes
  from a storage buffer: per-pixel exact colours at any zoom, no CPU redraw, and
  the same bytes that go on the stick. Edges are anti-aliased by exact area
  vertically, and the scrolling detail view is box-filtered horizontally: each
  column a pixel covers is coloured on its own (the reach rule: each band
  reaches its level of the height, and every pixel is in the colour of exactly
  the bands that reach it) and the results are blended by the share of the
  pixel each column covers; wide columns are interpolated at the pixel's centre
  and a sharp attack's edge is split by area. The blending is done in light,
  not in the sRGB-encoded values the swapchain holds (which would give a
  column split over two pixels much less light than one inside a pixel). A
  one-column spike so keeps its light as it moves by fractions of a pixel
  instead of flickering, and so do the beat and cue lines drawn over it
  (`cb_vbar_draw`). The overview and the row previews, which do not scroll,
  keep each band's maximum over a pixel's columns. ImGui draws everything
  else.
- **The track decoded around the playhead** for playback. Loading opens
  the track's stream (cyanbox/player.h) in a few milliseconds: two
  pipelines (cyanbox/audio.h: libavformat, libavcodec, then libswresample
  writing float at the file's rate into reference-counted chunks), which a
  thread of the deck's own runs: a window of 4 s behind and 8 s ahead of
  the playhead, half a second at every cue point, and the target of every
  jump as it is asked for, so cues, hot cues and loops play at once and a
  click anywhere on the waveform decodes in a millisecond or two (median
  0.7 ms for a FLAC, 2.8 ms for an Opus, measured from the jump). The feed
  copies no samples: it puts each chunk's pointer where the real-time
  callback looks, which reads it without a reference and never waits; a
  chunk taken out is let go of once a callback has finished since. Decoding
  the whole track first took one to five seconds per load and 450 MB for
  ten minutes at 96 kHz. A seek flushes the decoder and starts it from
  enough before the target that the samples are the analyser's
  (cb_decode's) exactly, for every codec but the lossy ones' cold start
  (within 1e-4 for Opus): no seek inaccuracy, sample-exact loops, and grid
  and audio agree on MP3s too. Resampling and varispeed are one cubic
  interpolator in the real-time callback.
- **MASTER TEMPO with Signalsmith Stretch** (MIT, a git submodule in
  `subprojects/`, read through meson's CMake module): the deck's varispeed
  output is shifted back by 1 / (tempo + bend). The shifter's 120 ms delay
  is read ahead, so jumps, starts and stops are heard at once, and the
  position reported is the one heard. Chosen over Rubber Band, whose
  licence is the GPL, and which measured worse (and allocated inside its
  processing call on first use and on reset).
- **PipeWire natively** (`pw_stream`, 48 kHz F32P: planar float, the
  graph's own format, which the mix writes straight into), one process
  callback that never allocates or locks; commands arrive through a lock-free ring, positions leave
  through atomics. The sink is selectable (`--audio-sink`, `PIPEWIRE_NODE`) and
  a `--audio none` backend drives the clock without a device, which is what the
  tests and the headless runs use. Nothing is ever routed to the Focusrite
  while testing.
- **Analysis multithreaded across tracks, its front end on the GPU.** The
  jobs pool analyses many tracks at once (12 cores here). The beat
  analyser's spectral front end (the STFT, its mel and kick bands, and the
  band envelopes around beats) runs as Vulkan compute shaders when there is
  a device (`cyanbox/compute.h`, docs/BEATS.md section 1), on a compute
  context of its own that the pool's workers share: no window, so the CLI
  and the tests use it too, and the interface's is separate from the
  window's device (the frame loop keeps its queue to itself). The GPU only
  turns tables the C code builds into band energies; every decision stays
  in C, and the results equal the CPU path's to float rounding (the same
  grids on every synthetic track). Own shaders rather than VkFFT: window,
  transform, power and band sums fit one workgroup per frame with no
  intermediate buffers and no shader compilation at run time. Without a
  device, or with `--no-gpu`, everything runs on the CPU (`av_tx`), the
  same results. Decoding and the waveforms now take longer than the beats.
- **The library is SQLite.** One file, WAL mode, foreign keys, every edit a
  transaction, one versioned schema (a new library is made at it; nothing
  older is converted). The whole track list
  is also held in memory (about 1 kB a track; 9,000 tracks open in 0.07 s),
  written through, so the interface sorts, groups and filters without
  touching the database (a few ms at 9,000 tracks). Waveform bytes and
  artwork thumbnails are blobs in it, so a library is one file to back up;
  incremental auto-vacuum gives the space of what goes back to the disk.

## 5. Beat grid: what "robust, with sense and logic" means here

The grid a DJ player wants is piecewise constant: a tempo and a downbeat, and
a new section only where the music really changes tempo or phase. The
analyser is built around that, with each decision made from evidence that can
be inspected:

1. **Onset evidence from several bands at once**: a kick band (below 150 Hz),
   a broadband spectral flux with a max-filter across bands (SuperFlux) to
   ignore vibrato and sweeps, and a high band for hats. Each is timestamped at
   the window centre.
2. **Tempo from the whole track and from a tempogram over time.** Candidates
   from the autocorrelation; each rescored by the DFT magnitude on its own
   harmonic series (rejects 3/2 and 2/3 impostors), by whether kicks land on
   every beat or every other beat (the octave), and by a prior that is only a
   tie-breaker. A tempogram says whether one tempo holds throughout or where it
   changes, before any grid is fitted.
3. **A grid fitted, not tracked.** Phase from the folded beat shape; then every
   beat snapped to its nearest kick attack and a line fitted through them, which
   averages hundreds of beats down to well under a millisecond. Integer BPM is
   snapped to only when the drift over the track stays under a tolerance.
4. **Sections only with evidence.** The residuals of the fit are examined along
   the track. A sustained slope is a tempo change; a step is a phase cut; a
   stretch with no kicks is a breakdown that the grid holds through unchanged.
   Each section is refitted on its own beats, and the boundary is placed on
   the first beat the new tempo supports. Live-drummed material that jitters
   around one tempo stays on one line; it is not chopped into sections.
5. **Downbeat from structure.** Spectral novelty per half-beat at one, two,
   four and eight bars; only its peaks vote, by bar position. The half-beat
   positions are included so a grid that landed on the off-beat is moved by
   half a beat rather than merely renumbered. Bass-note changes and the first
   strong kick are weaker extra votes. Beat 1 must be on a kick.
6. **Honest output.** A confidence, and flags the interface shows: tempo
   change found, octave uncertain, phase uncertain, no clear kicks. The
   interface lets the DJ fix any of it in a few clicks (tap, nudge, set
   downbeat, split section) and cues set in bars follow the correction.

Evaluation uses `~/Music` (about 1,600 FLAC and 1,400 Opus files: Japanese
house/techno compilations, drum & bass, J-pop with live drums, soundtracks).
There is no ground truth, so the harness measures what can be measured: onset
alignment, octave agreement with the few BPM tags present, agreement between
the kick band and the full band, stability under the Python analyser and the
new one, and a hand-checked sample where the two disagree.

## 6. Interface

Layout, top to bottom (rekordbox's "1 PLAYER" Export layout). cyanbox is a
preparation tool for the stick, not a mixer, so there is exactly one player
and no sync:

- **One player across the top.** Cover art, title, artist, key, BPM and
  tempo, time readouts; the overview waveform of the whole track with cue
  markers, loop bands and the playhead; the detail waveform scrolling under a
  fixed centre playhead, zoomable in bars (12 bars default, 0.25 to 64); beat
  and bar lines; transport (CUE, PLAY/PAUSE), hot-cue pads A–H, memory cue
  controls (set, previous, next, delete) and list, loop controls (IN, OUT,
  EXIT/RELOOP, 1/4 … 32 beat auto loops, ÷2, ×2, memory loop, active loop),
  beat jump with rekordbox's sizes, QUANTIZE, tempo slider, grid tools (tap,
  nudge, shift, set downbeat, split/merge section, BPM field).
- **Below:** playlist tree on the left (folders, playlists, create/rename/
  delete, drag tracks in), the track table on the right (sortable columns,
  search, cover thumbnails, waveform preview column, hot cue letters, colour,
  rating, key, BPM, time), with double-click or drag to load a deck.
- **Status bar:** analysis queue, export progress, audio device, messages.

Waveform colours are matched to rekordbox as measured by rbxport: the 3-band
palette is blue `#0055E1`, amber `#FFA600`, white, and the combination colours
`#B4690A` (low+mid), `#D2DCFA` (low+high), `#FFF0D7` (mid+high), `#F5EBD7` (all);
the stacked row preview uses the measured 128/256/128 scales; the BLUE palette
shades a column from blue to cream by its whiteness bits; RGB columns are
drawn saturated by their strongest channel. The 3-band levels drawn are
rbxport's as well (section 3, item 10), so the same rules on the same bytes
give rbxport's picture. The 3-band detail is the default, as on a CDJ-3000;
BLUE and RGB are selectable, since all three are on the stick.

Keyboard follows rekordbox's Export preset (space, C, M, I/O/R, 1–3 hot cues,
4–9 beat loops, N/B/X memory cues, cursor jumps, Q quantize).

## 7. Testing

- `meson test`: format writers against the rekordbox fixtures from dj
  (`tests/data`), library round trips through SQLite, grid arithmetic, cue-control semantics on
  the null audio backend, analysis on synthetic tracks, and waveform bytes
  compared with the Python implementation's output on a fixture.
- A port check: the same tracks, grids and cues exported by the Python `dj`
  and by `cyanbox` must produce identical `export.pdb`, settings and ANLZ
  bytes, apart from the documented fixes.
- The interface runs on a private headless sway (`WLR_BACKENDS=headless`), never
  on the user's session; screenshots with `grim`. `cyanbox --frames N` runs N
  frames and exits for that.
- Audio tests use the null backend; headless runs use a PipeWire null sink,
  never the Focusrite.

## 8. Work split

Implemented by subagents against the interfaces in `include/cyanbox/`:

| Module | Scope |
| --- | --- |
| core | model, grid arithmetic, SQLite library, playlists, settings, notation, thread pool |
| audio | decode, probe, MP3 timeline rule, FLAC seek table, artwork |
| analysis/waveforms | byte-faithful port of dj's waveform encoders; PWV6/PWV7 by rbxport's rules |
| analysis/beats+key | the new grid analyser, key detection, evaluation harness over ~/Music |
| formats+export | pdb, ANLZ, OneLibrary, settings, colours, plan, stick writer, inspect, stick sync |
| player | PipeWire, decks, controls, MASTER TEMPO, null backend |
| gpu | window, Vulkan, VMA, frame callback, ImGui glue, waveform shader; compute context and the analysis' front-end shaders |
| ml | MOSS-Audio on Vulkan compute: a track's genre, colour and comment, on request only, one batch and one model at a time (docs/ML.md) |
| ui | the screens |
| cli/tests | subcommands, meson tests, headless run |
