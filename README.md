# rxbox

rxbox prepares USB sticks for Pioneer/AlphaTheta CDJs and XDJs without
rekordbox. It keeps a library of your audio files in one SQLite database,
analyses them (beat grid, key, waveforms), lets you set cues, loops and the
grid in a rekordbox-style interface with one player, and writes a stick the
players read: both device libraries (`export.pdb` and the OneLibrary
`exportLibrary.db`), the analysis files, artwork and My Settings. After a gig
it reads back what the player saved on the stick (cues, play history,
settings) so you can take it into the library. It is written in C23, with
the interface in Dear ImGui on SDL3 and Vulkan under Wayland, and plays
through PipeWire. It is a preparation tool, not a mixer.

## Building

You need meson (1.4 or later), ninja, `cmake` (meson reads one subproject
through it), GCC with C23 support (the build checks for `stdckdint.h`) and a
C++17 compiler, `glslangValidator` (glslang) and `python3` for the shaders,
and these libraries, by pkg-config name:

- `libavformat`, `libavcodec`, `libavutil`, `libswresample`, `libswscale` (FFmpeg)
- `sqlite3`
- `libpipewire-0.3`
- `vulkan` (the interface, and the analysis, whose spectral front end runs
  on a GPU when there is one: Vulkan 1.3 with buffer device addresses; with
  no device the analysis runs on the CPU and gives the same results)
- `sdl3`, `wayland-client`, `fontconfig` (the interface)

Dear ImGui, the Vulkan Memory Allocator and tomlc17 are vendored in
`third_party/`. Signalsmith Stretch (the MASTER TEMPO pitch shifter) and
signalsmith-linear are git submodules in `subprojects/`, both MIT and
header-only, built in statically. The `ffmpeg` command is used only by the
tests, to make test audio.

```sh
git submodule update --init --recursive
meson setup build
ninja -C build
meson test -C build
```

This builds `build/rxbox` (the interface and every subcommand) and
`build/src/app/rxbox-cli` (the subcommands only, without the window code).
The interface and GPU tests are skipped unless you point them at a private
compositor; see `docs/HACKING.md`.

## The library

The library is a directory holding `library.db`: `-c DIR` / `--library DIR`,
else `$RXBOX_LIBRARY`, else `~/.local/share/rxbox`. `rxbox init [DIR]`
creates one. The database holds the file facts and tags, your edits to the
tags, grids, keys, cues, rating, colour, My Tags, play counts and history,
waveforms, artwork thumbnails, playlists and folders, and the settings. The
audio files stay where they are. A track whose file goes missing stays in
the library, greyed out, until the file comes back or is relocated. A
track's library id is also its id on every stick, so players keep their
cached data across exports.

## Analysis

Adding a track analyses its beat grid, key and waveforms. After that,
`rxbox analyze` and Analysis → Analyse all unanalysed in the interface
analyse only what a track lacks; `-f` (Re-analyse in the interface) does it
all again. A locked grid is never replaced unless you name the track:
`rxbox analyze --force TRACK`, or RE-ANALYSE on the deck.

The library keeps the version of the analyser that made each track's grid,
key and waveforms. When a new rxbox improves one of them, that version goes
up, and a result an older version made counts as missing, as do waveforms
stored in a format the new rxbox no longer reads: the interface analyses
those tracks again in the background when it opens the library, without
being asked, and `rxbox analyze`, adding a track and an export (before it
writes) do the same. Only the stale parts are redone, and locked grids are
kept. A grid you edit by hand keeps the version of the grid it was edited
from, so it is replaced like any other at the next upgrade of the grid
analyser: lock a grid you corrected to keep it. `rxbox check` counts the
stale tracks and `rxbox show` prints a track's versions. A library from
before versions were kept counts all its results as stale, so the first
start after that upgrade analyses the whole library again.

## Command line

`rxbox [OPTIONS] COMMAND [ARGS]`. A TRACK is an audio file in the library
(any path to it), a folder (every library track under it) or a library id.

| Command | What it does |
| --- | --- |
| `add PATH...` | add audio files or folders to the library and analyse them |
| `analyze [TRACK...]` | analyse what is missing or was made by an older analyser (the whole library by default); `-f` re-analyses, `-w beats,key,waves` limits it |
| `show TRACK...` | print the resolved grid, key, cues and analyser versions |
| `export STICK` | write the settings, every playlist and their tracks to a mounted stick (`--all`, `-p NAME`, `--prune`, `--compat`, `--ignore-unsynced`, `-n`) |
| `inspect STICK` | re-read and validate a stick |
| `sync STICK [--apply]` | list the cues, plays and settings a player saved on the stick that the library lacks; `--apply` takes them |
| `pull STICK` | take all of them (`sync --apply`) |
| `rescan` | check which audio files are missing (or back) |
| `check` | check the library database's integrity and count the tracks an older analyser analysed |
| `settings` | list the My Settings names and values |
| `config [KEY [VALUE]]` | show or change a setting (`analysis.bpm_min`, `export.key_notation`, `player.quantize` ...) |
| `init [DIR]` | create a library |
| `playlist list` | list the playlists |
| `playlist create NAME` | create a playlist (`Folder/Name` creates the folders) |
| `playlist add NAME TRACK...` | append tracks to a playlist |
| `playlist import FILE.m3u8` | make a playlist from an m3u8 file |
| `playlist export NAME FILE.m3u8` | write a playlist as an m3u8 file |

`rxbox --help` has the full list of options. The subcommands open no window
and no audio device. Analysing (`add`, `analyze`, the interface) runs the
beat analyser's transforms on the GPU when there is one, without a window:
`--device NAME` picks the GPU whose name contains NAME (or
`RXBOX_GPU_DEVICE`), `--no-gpu` (or `RXBOX_ANALYSIS_GPU=0`) keeps everything
on the CPU, with the same results.

## The interface

`rxbox` without a command opens the interface (a Wayland session with Vulkan
is needed):

```sh
rxbox                               # the default library, the default PipeWire output
rxbox --library ~/dj/library        # another library
rxbox --audio-sink alsa_output.usb-headphones   # play to this PipeWire node
rxbox --audio none                  # no audio device at all
```

`--audio-sink NAME` (or the `audio.sink` setting) connects the player to that
PipeWire node and nowhere else; without it the session manager picks the
default output. `--device NAME` prefers the GPU whose name contains NAME,
for the window and for the analysis.
The status bar shows the audio output (`PipeWire 48 kHz · <sink>`, or
`Audio: none`), the number of tracks and the GPU. The layout and every
control are described in `docs/UI.md`.

## Exporting

One export writes the settings, the playlists and the tracks in them;
nothing else. In the interface it is File → Export to USB; on the command
line `rxbox export STICK`.

- What is exported: every playlist and the union of their tracks by
  default; only the chosen playlists (`-p NAME`, or "Selected playlists"),
  or the whole library (`--all`).
- Tracks that lack an analysis, or have one an older analyser made, are
  analysed first.
- Audio files already on the stick with the same size and modification
  time are skipped. The analysis files, artwork, settings files and both
  databases are rewritten every time.
- Files on the stick that are no longer exported are only deleted after
  confirmation: the interface lists them and asks once; the command line
  deletes them only with `--prune` and otherwise reports how many there are.
- If the stick holds changes a player made that are not synced back (hot
  cues, memory cues, play history, My Settings), the export refuses to
  overwrite them: sync first, or export anyway (`--ignore-unsynced`, or
  "Export anyway" in the interface).
- Before writing, it checks the free space, that every track row fits a
  DeviceSQL page, that no two tracks map to the same stick path, and that
  the destination is a FAT32 or exFAT mount (a warning otherwise). Missing
  and unanalysed tracks are reported and skipped. `-n` / `--dry-run` prints
  what would be copied and deleted and writes nothing.
- `--compat` (or the dialog's checkbox) transcodes AAC and hi-res files to
  16-bit/44.1 kHz FLAC on the stick.

## Syncing after a gig

`rxbox sync STICK` (File → Sync from USB in the interface) reads what the
player left on the stick and compares it with the library: hot cues set or
cleared, memory cues and loops added or removed, plays from the history, and
My Settings values changed on the player. The command line lists them and
`--apply` takes them all (`rxbox pull STICK` is the same); the interface
shows one row per change, each accepted or rejected, and applies the
accepted ones. Plays are not counted twice when the library already has a
play of the same track within a minute.

## What is written to the stick

| Path | Contents |
| --- | --- |
| `/Contents/<Artist>/<Album>/<file>` | the audio (or its compatibility transcode) |
| `/PIONEER/rekordbox/export.pdb` | the DeviceSQL library older players read |
| `/PIONEER/rekordbox/exportExt.pdb` | My Tags, when any track has one |
| `/PIONEER/rekordbox/exportLibrary.db` | the OneLibrary database (SQLCipher) newer players read |
| `/PIONEER/USBANLZ/.../ANLZ0000.DAT`, `.EXT`, `.2EX` | beat grid, cues and loops, waveforms (blue, RGB and 3-band) |
| `/PIONEER/Artwork/...` | cover art, 80 and 240 px |
| `/PIONEER/MYSETTING.DAT`, `MYSETTING2.DAT`, `DJMMYSETTING.DAT`, `DEVSETTING.DAT` | My Settings, for the sections you set (`rxbox settings` lists the names) |
| `/PIONEER/rxbox/sync.txt` | what this export wrote, so later changes can be told to be a player's |

Both databases are written by default (`export.pdb` and `export.onelibrary`
settings), so one stick works on every player generation.

The tests check the writers against files rekordbox wrote (in `tests/data`,
from the Seabass project's CC0 fixtures):

- `export.pdb`: the fixed tables (colours, columns, menus, sort orders) are
  byte-identical to rekordbox 7.2.18's, and the reader and structural check
  accept rekordbox's file; the `exportExt.pdb` tag page equals a rekordbox
  export's.
- `exportLibrary.db`: the SQLCipher code opens rekordbox 7.2.18's database
  (every page's HMAC verifies), and its schema and browse rows equal the
  ones rxbox writes.
- My Settings: the files are byte-identical to rekordbox 7.2.8's for the
  same values, and each decodes and re-encodes to the same bytes.
- ANLZ: the directory hash matches rekordbox's vectors, and rekordbox's
  analysis files read back (cues, colours, previews).
- Waveform bytes equal those of `dj`, the Python tool rxbox was ported from,
  apart from two documented minimums the players' firmware needs.

These are checks of the files; the test suite does not run a player.

## Documentation

- `docs/OVERVIEW.md`: what rxbox is, the decisions behind it, and what it
  took from the earlier tools
- `docs/HACKING.md`: code style, layout, tests, running the interface while
  developing
- `docs/UI.md`: the interface, control by control
- `docs/BEATS.md`: the beat-grid analyser
- `docs/pdb.md`, `docs/anlz.md`, `docs/onelibrary.md`, `docs/settings.md`:
  the stick formats

rxbox is under the BSD 2-Clause licence (`LICENSE.md`).
