# rxbox: overview and plan

rxbox is a DJ library manager and stick writer in C: the `dj` Python tool ported
to C23, with a rekordbox-style interface (SDL3, Vulkan, Dear ImGui), PipeWire
playback, and a new beat-grid analyser. It never needs rekordbox, AlphaTheta
software or a rekordbox database.

This document is the high-level survey written before any code: what the two
earlier attempts are, what each got right, what rxbox keeps, takes and fixes,
and how it is built. Format details stay in `docs/pdb.md`, `docs/anlz.md`,
`docs/onelibrary.md` and `docs/settings.md` (carried over from dj).

## 1. The two earlier attempts

### dj (`~/projects/dj`, Python, ~3.8k lines)

A command-line stick writer. Analyses tracks (grid, key, waveforms), keeps the
library as plain text (`dj.toml`, one TOML sidecar per track, m3u8 playlists)
and writes everything a player reads: `export.pdb`, the OneLibrary
`exportLibrary.db` (SQLCipher), ANLZ `.DAT/.EXT/.2EX`, artwork, My Settings.

What it got right, and rxbox keeps:

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

What it lacks, and rxbox adds:

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
- The measured 3-band waveform colours and stacking scales (section 6).
- The rekordbox keyboard map and the deck's control semantics (memory cue
  tolerance, beat-jump sizes, zoom steps in bars).
- Its beat-grid *rules* (kicks establish tempo, cuts need support, structure
  identifies beat 1) are sensible, but its implementation is 5k lines tuned on
  private playlists and is slow. rxbox writes its own (section 5).

Where dj is right and rbxport is wrong, and rxbox follows dj: OneLibrary hot-cue
`kind` numbering (A–H are 1–8), loop end for non-loops (repeat the start),
colour waveform byte fits.

## 2. What rxbox is

One binary, `rxbox`:

```
rxbox                      the interface
rxbox add <path>...        add files or folders to the library and analyse them
rxbox analyze [paths]      (re)analyse; only missing sidecar sections are filled
rxbox show <track>         print the resolved grid, key and cues
rxbox export <stick>       write a stick (both libraries, ANLZ, artwork, presets)
rxbox inspect <stick>      re-read and validate a stick
rxbox pull <stick>         copy cues the player saved on the stick into sidecars
rxbox settings             list the My Settings names and values
```

The library is a directory (`$RXBOX_LIBRARY`, `-c DIR`, default
`~/.local/share/rxbox`) holding `rxbox.toml`, `tracks/` (one sidecar per track,
mirroring the audio's absolute path), `playlists/` (m3u8 files, folders are
playlist folders) and `.cache/` (waveforms and decoded analysis products, safe
to delete). The sidecar format is dj's, plus an `id` that never changes once a
track is added; that id is the track id on every stick, so players keep their
cached artwork and waveforms across exports.

## 3. Fixes carried in from the earlier comparison

All of the items the earlier session listed as worth importing are in scope:

1. MP3 timeline rule (above). Grids and cues on LAME files were 26 ms early.
2. Preview waveform minimums (`PWAV` ≥ 2, `PWV2` ≥ 1).
3. The 64-entry hot-cue colour table and default pad colours.
4. Stable track ids across exports (via the library id).
5. Checks before writing anything: free space on the stick, no track row too
   long for a DeviceSQL page (named, not a crash).
6. `rxbox pull`: read player-saved cues from the stick's ANLZ files back into
   the sidecars, matched by track id.
7. More `inspect` checks: ANLZ embedded path matches the audio, folder matches
   the path hash, fixed track-row values, both libraries agree on hot-cue
   auto-load, declared cue counts match parsed ones.
8. Optional background colour in `[device]`.
9. Damaged MP3 frames play and analyse as silence rather than being dropped.

Plus: an optional "maximum compatibility" transcode (AAC and hi-res FLAC to
16/44.1 FLAC) at export, since nobody knows what players do with AAC priming.

## 4. Architecture

```
src/core      model, sidecars (tomlc17 + a small writer), library, playlists, config, logging, threads
src/audio     libavformat/libavcodec decode to memory on the players' timeline, probe, tags, artwork, MP3/FLAC scanners
src/analysis  spectral front end (libavutil av_tx), onset, tempo, grid, downbeat, key, waveforms
src/formats   export.pdb, ANLZ read/write, OneLibrary (sqlite3 + SQLCipher pages through OpenSSL), My Settings, colours
src/export    stick plan, writer, inspect, pull
src/player    PipeWire stream, the player, lock-free command queue, cue/loop/quantize controls, null backend for tests
src/gpu       SDL3 window (Wayland), Vulkan device + VMA, swapchain, Wayland frame-callback pacing, ImGui glue, waveform shader
src/ui        Dear ImGui screens (C++; the only C++ besides vendored code)
src/cli.c     the subcommands
```

Decisions and why:

- **C23, meson, one binary.** The user's house style (libcraw). C++ is used only
  for the ImGui-facing UI files and the vendored VMA/ImGui; everything else,
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
  the same bytes that go on the stick. ImGui draws everything else.
- **Whole track decoded into memory** for playback (float, native rate). No
  seek inaccuracy, sample-exact loops, and the deck plays exactly the samples
  the analyser saw, so grid and audio agree on MP3s too. Resampling and
  varispeed are one cubic interpolator in the real-time callback.
- **PipeWire natively** (`pw_stream`), one process callback that never
  allocates or locks; commands arrive through a lock-free ring, positions leave
  through atomics. The sink is selectable (`--audio-sink`, `PIPEWIRE_NODE`) and
  a `--audio none` backend drives the clock without a device, which is what the
  tests and the headless runs use. Nothing is ever routed to the Focusrite
  while testing.
- **Analysis on the CPU first**, multithreaded across tracks (12 cores here).
  Decoding dominates; the STFT/mel/onset front end is under a second a track
  with `av_tx`. The front end sits behind one interface so a Vulkan compute
  (or VkFFT) version can replace it; the user is happy to go there once the
  CPU version is validated.
- **Library stays plain text** (sidecars + m3u8), dj's design. SQLite is
  linked anyway for OneLibrary but is not the library. Two thousand sidecars
  parse in well under a second at startup; the interface sorts and filters in
  memory. The user would accept an SQLite library if it were well kept; this
  can be revisited without changing any interface above `rxbox/library.h`.

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

Layout, top to bottom (rekordbox's "1 PLAYER" Export layout). rxbox is a
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
drawn saturated by their strongest channel. The 3-band detail is the default,
as on a CDJ-3000; BLUE and RGB are selectable, since all three are on the stick.

Keyboard follows rekordbox's Export preset (space, C, M, I/O/R, 1–3 hot cues,
4–9 beat loops, N/B/X memory cues, cursor jumps, Q quantize).

## 7. Testing

- `meson test`: format writers against the rekordbox fixtures from dj
  (`tests/data`), sidecar round trips, grid arithmetic, cue-control semantics on
  the null audio backend, analysis on synthetic tracks, and waveform bytes
  compared with the Python implementation's output on a fixture.
- A port check: the same library exported by the Python `dj` and by `rxbox`
  must produce identical `export.pdb`, settings and ANLZ bytes when the
  sidecars are the same.
- The interface runs on a private headless sway (`WLR_BACKENDS=headless`), never
  on the user's session; screenshots with `grim`. `rxbox --frames N` runs N
  frames and exits for that.
- Audio tests use the null backend; headless runs use a PipeWire null sink,
  never the Focusrite.

## 8. Work split

Implemented by subagents against the interfaces in `include/rxbox/`:

| Module | Scope |
| --- | --- |
| core | model, sidecar read/write, library, playlists, config, logging, thread pool |
| audio | decode, probe, MP3 timeline rule, FLAC seek table, artwork |
| analysis/waveforms | byte-faithful port of dj's waveform encoders |
| analysis/beats+key | the new grid analyser, key detection, evaluation harness over ~/Music |
| formats+export | pdb, ANLZ, OneLibrary, settings, colours, plan, stick writer, inspect, pull |
| player | PipeWire, decks, controls, null backend |
| gpu | window, Vulkan, VMA, frame callback, ImGui glue, waveform shader |
| ui | the screens |
| cli/tests | subcommands, meson tests, headless run |
