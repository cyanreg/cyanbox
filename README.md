[cyanbox](#cyanbox)
===============
A DJ library manager that prepares USB sticks for Pioneer/AlphaTheta CDJs
and XDJs without rekordbox, and serves the library to players over PRO DJ
LINK. It analyses beat grids, keys and waveforms, tags tracks with an audio
model on the GPU, and has a rekordbox-style interface with one player for
setting cues, loops and grids. It is a preparation tool, not a mixer.

Written in C23, with the interface in Dear ImGui on SDL3 and Vulkan under
Wayland, playing through PipeWire.

Features
--------
 * One [library](#the-library) in a single SQLite database; the audio files
   stay where they are and are never written to
 * [Beat grids](#beat-grids) that follow live drummers and tempo ramps beat
   by beat (dynamic grids), and straight sections for everything else
 * [Key detection](#keys) more accurate than libKeyFinder on GiantSteps,
   with classic, Camelot and Open Key notation and rekordbox's Traffic
   Light
 * Waveforms in all three of the players' styles: blue, RGB and 3-band
 * Analysis on the GPU through Vulkan, with the same results on the CPU
 * [Automatic re-analysis](#analyser-versions) of what an older analyser
   made, in the background
 * [Tagging](#tagging) with an audio-language model on the GPU: a colour, a
   comment and a genre
 * A [rekordbox-style interface](#the-interface): hot cues A–H, memory cues
   and loops, beat loops, beat jump, quantize, MASTER TEMPO and grid editing
 * Gapless [playthrough](#playthrough) of playlists and albums
 * [USB export](#exporting) for every player generation: both device
   libraries (`export.pdb` and the OneLibrary `exportLibrary.db`), the
   analysis files, artwork and My Settings
 * Incremental exports written straight to the stick, with its real speed
   and time left
 * [Older players](#older-players): WAV or AIFF sticks for players that read
   no FLAC, romaji titles for players that show Latin-1 only
 * [Sync after a gig](#syncing-after-a-gig): take back the cues, plays and My
   Settings the players saved on the stick
 * [PRO DJ LINK](#pro-dj-link): players browse and load the library over
   Ethernet with no stick, as with rekordbox's Link Export
 * [LINK CUE](#link-cue): the deck heard in a DJM's headphone cue (so far
   on emulated mixers only)
 * [MPRIS](#mpris) media controls
 * Playlists, folders and m3u8 import and export
 * A [command line](#cli) for adding, analysing, tagging, exporting and
   syncing

Compiling
---------
Dependencies:

 * meson (1.4 or later), ninja and cmake (meson reads one subproject
   through it)
 * GCC with C23 support (the build checks for `stdckdint.h`) and a C++17
   compiler
 * glslang (`glslangValidator`) and python3, for the shaders
 * FFmpeg (libavformat, libavcodec, libavutil, libswresample, libswscale)
 * SQLite (3.44 or later)
 * PipeWire (`libpipewire-0.3`)
 * Vulkan, for the interface and the analysis
 * SDL3, wayland-client and fontconfig, for the interface

Optional dependencies, each used only when found:

 * sd-bus, from libsystemd, libelogind or basu, for [MPRIS](#mpris)
   (`-Dmpris`; LGPL-2.1 or later, linked dynamically)
 * MeCab with its system dictionary (Debian: `libmecab-dev` and
   `mecab-ipadic-utf8`), to read kanji as romaji for Latin-1 players
   (`-Dmecab`)
 * ICU (`icu-i18n`), to write other scripts as Latin for Latin-1 players
   (`-Dicu`)

Dear ImGui, the Vulkan Memory Allocator and tomlc17 are vendored in
`third_party/`. Signalsmith Stretch (MASTER TEMPO) and signalsmith-linear
are git submodules in `subprojects/`, header-only and MIT, built in
statically. Beat This!'s weights (MIT), which every beat grid is made with,
are in `models/` (`models/README.md`). The `ffmpeg` command is used only by
the tests, to make test audio.

```sh
git submodule update --init --recursive
meson setup build
ninja -C build
meson test -C build
```

This builds `build/cyanbox`, the interface with every command, and
`build/src/app/cyanbox-cli`, the commands without the window code. The
interface and GPU tests are skipped unless pointed at a private compositor;
see `docs/HACKING.md`.

The analysis runs its spectral front end on a GPU with Vulkan 1.3 and
buffer device addresses when there is one, and on the CPU otherwise. The
tagging model needs a GPU with 16- and 8-bit storage and about 7 GB of
memory.

CLI
---
`cyanbox [OPTIONS] COMMAND [ARGS]`. Without a command, cyanbox opens the
[interface](#the-interface). A `TRACK` is an audio file in the library (any
path to it), a folder (every library track under it) or a library id. The
commands open no window and no audio device.

| Command                         | Description                                                                                  |
|---------------------------------|----------------------------------------------------------------------------------------------|
|                                 | **Library**                                                                                  |
| `init [DIR]`                    | Create a library                                                                             |
| `add PATH...`                   | Add audio files or folders and analyse them                                                  |
| `show TRACK...`                 | Print the colour, grid, key, cues, analyser versions, genre, comment and the model's tags   |
| `color COLOUR TRACK...`         | Set the colour: a name (`aqua`), `none`, or a hue in degrees (the stick gets the nearest)    |
| `rescan`                        | Check which audio files are missing, or back                                                 |
| `check`                         | Check the database's integrity and count the tracks an older analyser analysed              |
| `config [KEY [VALUE]]`          | Show or change a [setting](#settings)                                                        |
| `settings`                      | List the players' My Settings names and values                                               |
|                                 | **Analysis**                                                                                 |
| `analyze [TRACK...]`            | Analyse what is missing or stale, the whole library by default                               |
| `tag TRACK...`                  | [Tag](#tagging) the tracks with the model                                                    |
|                                 | **Sticks**                                                                                   |
| `export STICK`                  | Write the settings, every playlist and their tracks to a mounted stick                       |
| `inspect STICK`                 | Re-read and validate a stick                                                                 |
| `sync STICK [--apply]`          | List what a player saved on the stick that the library lacks; `--apply` takes it            |
| `pull STICK`                    | The same as `sync --apply`                                                                   |
|                                 | **Playlists**                                                                                |
| `playlist list`                 | List the playlists                                                                           |
| `playlist create NAME`          | Create a playlist (`Folder/Name` creates the folders)                                        |
| `playlist add NAME TRACK...`    | Append tracks to a playlist                                                                  |
| `playlist remove NAME TRACK...` | Take tracks out of a playlist (they stay in the library)                                     |
| `playlist delete NAME`          | Delete a playlist (its tracks stay in the library)                                           |
| `playlist import FILE.m3u8`     | Make a playlist from an m3u8 file                                                            |
| `playlist export NAME FILE.m3u8`| Write a playlist as an m3u8 file                                                             |
|                                 | **PRO DJ LINK**                                                                              |
| `link serve [-i IF]`            | Serve the library to [players](#pro-dj-link) until interrupted                               |
| `link browse -i IF [HOST]`      | Browse a served library as a player does (a fake CDJ-3000, for diagnostics)                  |

| Option                          | Description                                                                                  |
|---------------------------------|----------------------------------------------------------------------------------------------|
|                                 | **General options**                                                                          |
| `-c`, `--library DIR`           | The library (default `$CYANBOX_LIBRARY`, else `~/.local/share/cyanbox`)                          |
| `-v`, `--verbose`               | More messages                                                                                |
| `--device NAME`                 | The GPU whose name contains NAME, for the analysis and the interface (`$CYANBOX_GPU_DEVICE`)  |
| `--no-gpu`                      | Analyse on the CPU only, with the same results (`CYANBOX_ANALYSIS_GPU=0`)                      |
| `-h`, `--help`                  | Print usage                                                                                  |
| `-V`, `--version`               | Print the version                                                                            |
|                                 | **Analysis options**                                                                         |
| `-f`, `--force`                 | Re-analyse what is there too (a locked grid only when the track is named)                    |
| `-w`, `--what LIST`             | Any of `beats,key,waves` (default all)                                                       |
|                                 | **Export options**                                                                           |
| `--all`                         | Every track in the library, not only those in playlists                                      |
| `-p`, `--playlist NAME`         | Only this playlist and its tracks (repeatable)                                               |
| `--prune`                       | Delete files on the stick that are no longer exported                                        |
| `--compat`, `--no-compat`       | Transcode AAC and hi-res files to 16-bit/44.1 kHz FLAC (on by default)                       |
| `--format F`                    | The stick's audio: `copy`, `flac`, `wav` or `aiff`, see [older players](#older-players)     |
| `--ignore-unsynced`             | Export although the stick has player changes not synced back                                 |
| `-n`, `--dry-run`               | Print what would be copied and deleted, and write nothing                                    |
|                                 | **Interface options**                                                                        |
| `--audio pipewire\|none`        | The audio output; `none` plays to no device at all                                           |
| `--audio-sink NAME`             | The PipeWire node to play to (default: the session manager's choice)                         |
| `--no-mpris`                    | Stay off the D-Bus session bus                                                               |
| `--pro-link`                    | Serve the library to players over [PRO DJ LINK](#pro-dj-link) while the interface runs     |
| `--validation`                  | Enable the Vulkan validation layer                                                           |

`cyanbox --help` lists every option, those of `link serve` and `link browse`
included.

Settings
--------
The settings live in the library. `cyanbox config` lists them, `cyanbox config
KEY VALUE` changes one and `cyanbox config KEY ""` puts its default back. A
value a key cannot take, or a key cyanbox does not know, is refused.

| Key                                    | Default            | Description                                                                 |
|----------------------------------------|--------------------|-----------------------------------------------------------------------------|
| `analysis.bpm_min`, `analysis.bpm_max` | none               | The tempo range the beat analyser keeps to                                  |
| `analysis.snap_bpm`                    | `true`             | Snap to a whole BPM when the track allows                                   |
| `analysis.allow_sections`              | `true`             | Tempo sections where the tempo or phase really changes                      |
| `analysis.auto_cues`                   | `true`             | A memory cue on the first downbeat where the drums come in                  |
| `analysis.what`                        | `beats,key,waves`  | What the interface's Analyse does                                           |
| `import.comment`                       | `false`            | Take the file's comment tag when a track is added                           |
| `export.key_notation`                  | `classic`          | How `export.pdb` names keys: `classic`, `camelot` or `open`                 |
| `export.pdb`, `export.onelibrary`      | `true`             | Which device libraries an export writes                                     |
| `export.compat_transcode`              | `true`             | Transcode AAC and hi-res files to 16-bit/44.1 kHz FLAC                      |
| `export.format`                        | `copy`             | The stick's audio, see [older players](#older-players)                      |
| `export.device_name`                   | the stick's label  | The name the players show for the stick                                     |
| `export.background_color`              | `default`          | The players' background for the stick's library, or one of the eight colours |
| `link.mp3`                             | `false`            | Serve tracks over PRO DJ LINK as 320 kbps MP3 instead of 16-bit WAV         |
| `audio.sink`                           | the default output | The PipeWire node to play to                                                |
| `ml.model`                             | `models/...gguf`   | The tagging model's file (also `$CYANBOX_ML_MODEL`)                           |
| `color.pink` ... `color.purple`        | the legend         | What each colour stands for, in the words the [tagging](#tagging) picks by  |
| `ui.traffic_light`                     | `related3`         | The browser's key highlight (View → Traffic Light)                          |
| `ui.detail_bars`                       | `12`               | How many bars the detail waveform shows                                     |

The players' My Settings are settings too, as `player.*`, `mixer.*` and
`device.*`: `cyanbox settings` lists their names and values, and see
[My Settings](#my-settings).

The library
-----------
The library is a directory holding `library.db`: `-c DIR` or `--library
DIR`, else `$CYANBOX_LIBRARY`, else `~/.local/share/cyanbox`. `cyanbox init [DIR]`
creates one. It holds the tracks' tags and your edits to them, grids, keys,
cues, ratings, colours, My Tags, play counts and history, waveforms,
artwork thumbnails, playlists and folders, and the settings. The audio
files stay where they are, and cyanbox never writes to them.

A folder is added album by album, each album in disc and track order, so an
album sits in the collection as it runs. The browser's Albums and Album
Artists views list the collection by its tags.

A track whose file goes missing stays in the library, greyed out, until the
file comes back or is relocated. A track's library id is also its id on
every stick, so the players keep their cached data across exports.

Whatever the library holds that is not valid, or that cyanbox does not know,
is removed when it opens, and the log says what went: a colour that is not
one of the eight, a grid that is not valid (the track is analysed again), a
cue at an impossible time, a setting a key does not take. A library on
read-only media is cleaned in memory only.

A track's colour is one of rekordbox's eight, pink, red, orange, yellow,
green, aqua, blue and purple, or none. What each colour means is up to
you: the [legend](#tagging) says it in words, which the tagging model picks
by.

The comment field is left for the tagging: cyanbox reads a file's tags when
it adds the track but leaves its comment tag out. The `import.comment`
setting takes the file's comment instead.

Beat grids
----------
Adding a track analyses its beat grid, key and waveforms. The grid is made
with Beat This!, a neural beat and downbeat tracker run on the GPU, and
cyanbox's own analysis of the same audio, which keeps it steady where the
network's beats wobble (`models/`). Upgrading from a
version without it analyses every unlocked grid again. Sequenced music
gets straight sections, one per tempo or phase change. Where the tempo
moves, as with a live drummer, rubato or a ramp, the grid follows the music
beat by beat instead: a dynamic grid, like rekordbox's "dynamic" analysis,
with the deck showing the local tempo. The players play both as they are.

The grid can be edited on the deck: type the BPM, tap it, nudge the grid,
set the downbeat, split and merge sections, double or halve the tempo.
Cues set in bars follow. A locked grid is never replaced by an analysis
unless you name the track: `cyanbox analyze --force TRACK`, RE-ANALYSE on the
deck, or a forced re-analysis of that one track.

`docs/BEATS.md` describes the analyser and how well it works.

Keys
----
| Key detector                     | GiantSteps key (604 tracks) | Time for 5 minutes of audio |
|----------------------------------|-----------------------------|-----------------------------|
| libKeyFinder 2                   | 60.4 %, MIREX 0.674         | ~700 ms                     |
| cyanbox                            | 64.7 %, MIREX 0.704         | ~110 ms                     |

Keys show in the players' key display, classic names or Camelot codes, on
the deck, in the browser and in the info panel; the stick can carry
classic, Camelot or Open Key names (`export.key_notation`). While a track
is loaded, the browser's Key column lights every key that mixes with it, as
rekordbox's Traffic Light does, from the same key to Related 3 (View →
Traffic Light). `docs/KEY.md` describes the detector.

Analyser versions
-----------------
The library keeps the version of the analyser that made each track's grid,
key and waveforms. When a new cyanbox improves one of them, what an older
version made counts as missing: the interface analyses those tracks again
in the background when it opens the library, and `cyanbox analyze`, adding a
track and exporting do the same. Only the stale parts are redone, and
locked grids are kept.

A grid you edit by hand keeps the version it was edited from, so lock a
grid you corrected to keep it through the next upgrade. `cyanbox check`
counts the stale tracks and `cyanbox show` prints a track's versions.

Tagging
-------
cyanbox can listen to a track with MOSS-Audio-4B (OpenMOSS, Apache-2.0), an
audio-language model, on the GPU, and tag it with:

 * a colour, the one of the eight whose description fits the track best
 * a comment: who sings, in what language, and the mood in three words
   (`Male vocals, English. Uplifting, energetic, anthemic.`)
 * a genre (`Hardstyle`), unless you typed one

The colours' descriptions are the legend. By default:

| Colour | Description                    |
|--------|--------------------------------|
| pink   | dark and intense, hard         |
| red    | intense, peak time             |
| orange | energetic and bright, driving  |
| yellow | bright, uplifting              |
| green  | groovy and relaxed, warm       |
| aqua   | calm, smooth                   |
| blue   | deep, melancholic              |
| purple | calm and dark, ambient         |

The model gives each description a probability and the most probable wins,
so the eight colours are the model's own division of the music. Describe
what can be heard (energy, brightness, darkness, groove), not where a track
goes in a set. Change the words in Analysis settings or with `cyanbox config
color.red "..."`; tracks tagged before keep their colour until they are
tagged again, and `cyanbox show` and the info panel say when a colour was
picked with words since changed.

Tagging only runs when you ask: the track menu's Analyse tags, Analysis →
Analyse tags of selection, or `cyanbox tag TRACK...`. Never when a track is
added, at startup or before an export. The model loads once per batch (3.5
s, 4 GB of the GPU's memory, up to 7 GB while it listens), takes about 6 s
a track on an RX 6900 XT, takes tracks asked for meanwhile into the same
batch, and unloads after the last one or when stopped (Analysis → Stop
tagging). The colours' probabilities and the model's measures (intensity,
club, mood, vocals) are kept too.

The model's file, `moss-audio-4b-instruct-q4_k.gguf` (4.1 GB, not in git),
goes in `models/` at the top of the source tree: cyanbox looks there beside
its executable, beside the executable's directory and in the current
directory. `ml.model` or `$CYANBOX_ML_MODEL` names another file.
`docs/ML.md` describes what the model can tell and how it runs.

The interface
-------------
`cyanbox` without a command opens the interface (Wayland with Vulkan):

```sh
cyanbox                                          # the default library and output
cyanbox --library ~/dj/library                   # another library
cyanbox --audio-sink alsa_output.usb-headphones  # play to this PipeWire node
cyanbox --audio none                             # no audio device at all
cyanbox --pro-link                               # serve the library to players
```

It is laid out as rekordbox's Export mode with one player:

 * **The player**: the artwork, title and readouts (key, BPM with the local
   tempo of a dynamic grid, elapsed or remaining time, bar and beat), the
   overview and the scrolling detail waveform with the grid and every cue
 * **Cues and loops**: hot cues A–H with colours and names, memory cues and
   loops, beat loops from 1/4 to 32 beats, loop in, out, halve and double,
   and a list of every cue of the track
 * **Transport**: CUE and PLAY, quantize, beat jump from Fine to 32 bars,
   tempo with ±6, ±10, ±16 % and WIDE ranges, and MASTER TEMPO
 * **Grid tools**: the grid editing above
 * **The browser**: Collection, Albums, Album Artists, playlists and folders,
   a search field, and a sortable table with waveform previews, artwork,
   key, BPM, time and five-star ratings
 * **The info panel**: the selected track's tags, rating, colour and My
   Tags, all editable, its file's facts, and what the tagging model said

The player starts as the players will, from their [My Settings](#my-settings):
quantize, beat jump size, tempo range, MASTER TEMPO, time mode, waveform
colour and position, overview type, key display and hot cue colours. The
status bar shows the audio output, the number of tracks and the GPU.
`docs/UI.md` describes every control, and Help → Keyboard map lists the
keys.

### Playthrough
With a playlist or an album in view, a track that plays to its end is
followed by the row after it with no gap: the next track is queued half a
minute ahead and taken over at the last sample. The title row shows what
follows as NEXT, and the deck stops after the last track.

### MPRIS
The player is an MPRIS 2 media player on the D-Bus session bus
(`org.mpris.MediaPlayer2.cyanbox`), so media keys, `playerctl` and a
desktop's media controls show the loaded track and drive it: play, pause,
stop (back to the cue point, as CUE), seek, volume, next and previous (the
rows after and before it), and a library track opened by its `file://` URL.
It is built when sd-bus is found (`-Dmpris`). `--no-mpris` and
`--pro-link` keep the interface off the bus, and the commands never go on
it.

Exporting
---------
File → Export to USB, or `cyanbox export STICK`, writes the settings, the
playlists and the tracks in them: every playlist by default, only the
chosen ones (`-p NAME`), or the whole library (`--all`).

 * **Incremental**: audio files already on the stick with the same size and
   modification time are skipped, and only the analysis files and artwork
   that differ are written. The settings and both databases are rewritten
   every time.
 * **Analysed first**: tracks that lack an analysis, or have a stale one,
   are analysed before anything is written.
 * **Direct**: the audio is written straight to the stick
   (O_DIRECT where the file system takes it, no temporaries, no fsync), so
   the progress bar, MB/s and time left are the stick's own, and it is
   synced once at the end.
 * **Checked**: before writing, the export checks the free space in whole
   clusters, that every track's row fits a database page and that no two
   tracks map to the same path, and warns when the stick is neither FAT32
   nor exFAT. Missing and
   unanalysed tracks are reported and skipped. `-n` prints what would be
   copied and deleted and writes nothing.
 * **Safe**: files no longer exported are only deleted after you confirm
   (`--prune` on the command line), and a stick holding cues, history or
   settings a player saved that were not synced back is not overwritten
   unless you say so (`--ignore-unsynced`, or Export anyway).
 * **Compatible**: AAC and hi-res files go on the stick as 16-bit/44.1 kHz
   FLAC by default (`export.compat_transcode`, `--compat`, `--no-compat`).

### What is written to the stick

| Path                                                    | Contents                                                     |
|---------------------------------------------------------|--------------------------------------------------------------|
| `/Contents/<Artist>/<Album>/<file>`                     | The audio, as it is or encoded                               |
| `/PIONEER/rekordbox/export.pdb`                         | The DeviceSQL library older players read                     |
| `/PIONEER/rekordbox/exportExt.pdb`                      | My Tags, when any track has one                              |
| `/PIONEER/rekordbox/exportLibrary.db`                   | The OneLibrary database (SQLCipher) newer players read       |
| `/PIONEER/USBANLZ/.../ANLZ0000.DAT`, `.EXT`, `.2EX`     | Beat grid, cues and loops, waveforms (blue, RGB and 3-band)  |
| `/PIONEER/Artwork/...`                                  | Cover art, 80 and 240 px                                     |
| `/PIONEER/MYSETTING.DAT`, `MYSETTING2.DAT`, `DJMMYSETTING.DAT`, `DEVSETTING.DAT` | My Settings                         |
| `/PIONEER/cyanbox/sync.txt`                               | What this export wrote, so a player's later changes are told |

Both databases are written by default, so one stick works on every player
generation.

The tests check the writers against files rekordbox wrote: the fixed tables
of `export.pdb` are byte-identical to rekordbox 7.2.18's, its
`exportLibrary.db` opens with every page's HMAC verified and its schema
and rows equal cyanbox's, the My Settings files are byte-identical to
rekordbox 7.2.8's, and rekordbox's analysis files read back. These check
the files; the tests do not run a player.

### Older players
Players before the CDJ-2000NXS2 (the CDJ-900 and its generation) read no
FLAC. `export.format` (or `--format` for one export) picks the stick's
audio:

| Format | Description                                                      |
|--------|------------------------------------------------------------------|
| `copy` | Each file as it is (the default)                                 |
| `flac` | Lossless files as FLAC                                           |
| `wav`  | Everything but MP3 as 16-bit WAV, for players that read no FLAC  |
| `aiff` | Everything but MP3 as 16-bit AIFF, for players that read no FLAC |

A file not copied is encoded from its decoded samples as it is written,
the very audio the analysis was made of, with no copy kept anywhere.
Encoding runs ahead of the stick, so it costs no time, and a record on the
stick lets the next export skip what is already there.

For players that show Latin-1 only (the CDJ-900), Japanese titles go as
romaji, with kanji read by MeCab, and other scripts as Latin through ICU.
Without MeCab, kana still go as romaji and kanji as they are.

### My Settings
The players' My Settings (quantize, auto cue level, tempo range, the
mixer's fader curves, the waveform colour...) go on the stick for the
sections the library has values for, and the players apply them when you
LOAD My Settings. Over PRO DJ LINK a CDJ-3000 or CDJ-2000NXS2 LOADs the
player settings from cyanbox too (MY SETTINGS > LOAD with cyanbox as the
source; a CDJ-2000 or 900 loads them only from a stick), a DJM-A9 the
mixer settings, and every player takes the device ones as it browses
cyanbox. File → Player settings (`Ctrl+,`) shows every one of them in
Player, Mixer and Device tabs (hover a player setting's name for what it
does), and `cyanbox config player.NAME VALUE` sets one. The tempo range defaults to ±6 %, not
rekordbox's ±10 %, and Eject/Load Lock to Lock.

Syncing after a gig
-------------------
`cyanbox sync STICK`, or File → Sync from USB, reads what the players left on
the stick and compares it with the library: hot cues set or cleared, memory
cues and loops added or removed, plays from the history, and My Settings
changed on the player. The command line lists them and `--apply` (or
`cyanbox pull STICK`) takes them all; the interface shows one row per change
to accept or reject. A play the library already has within a minute is not
counted twice.

PRO DJ LINK
-----------
With `--pro-link`, or `cyanbox link serve` on the command line, cyanbox does
what rekordbox's Link Export does: it joins the players' network as a
computer, appears on their SOURCE screen, and the players browse the
library and load its tracks with their grids, cues and waveforms, with no
stick. Without it, cyanbox never opens a Link socket.

 * **Every Ethernet player** that speaks standard PRO DJ LINK, from the
   CDJ-900 to the CDJ-3000X. Every track is served as a 16-bit WAV decoded
   as it is read (or a 320 kbps MP3 with `link.mp3`), so a player plays the
   very samples the grid and cues were made on, whatever the codec.
 * **Plug and play**: cyanbox serves on every Ethernet interface as soon as
   it is up with an address, and joins the link at once, before any player
   is on.
 * **The players in view**: the PRO DJ LINK window (Link → Players…) and
   the deck's title row show each player, what it has loaded, its state,
   key, tempo and time left over the track's overview.
 * **Your deck numbers**: players number themselves in the order they
   start. A click on `Link: 2 players` in the status bar lets you give each
   one the deck number it should show, so the title row goes left to right
   as the decks stand. The numbers stay with the players' hardware, and
   the players themselves keep their own.
 * **Load from cyanbox**: Load in deck N in the track menu, or drop a track on
   a player, unless it plays and Eject/Load Lock is on (it is by default).
 * **Edits on the deck are kept**: a cue saved or deleted on a player (MEMORY,
   a hot cue pad) and a rating given there go into the library, as with
   rekordbox.
 * **TAG LIST** is shared with the interface for as long as cyanbox runs.
 * **Plays and history**: a track a player plays for 30 s counts as a play,
   and one it plays straight on for 1.5 minutes goes on the History.

Tried with two real CDJ-900s, a real CDJ-3000 and DJM-A9 (a 1.5 hour set
without a fault), and emulated CDJ-900, CDJ-2000, CDJ-2000NXS2, CDJ-3000
and CDJ-3000X players running Pioneer's own firmware.

PRO DJ LINK has no authentication and no encryption. While cyanbox serves,
anything on those networks can read the library's audio, artwork and
analysis (never other files, and nothing can be written), so use a network
with only the players on it. `docs/LINK.md` describes the protocol, the
security and what was tested.

### LINK CUE
With `--pro-link`, while a DJM is on the link, the interface's deck is
heard only in its headphone cue, as rekordbox's Link Monitor feeds it.
When the deck starts playing, cyanbox asks the mixer for the cue and
streams the deck to it, paced by the mixer's clock. It never poses as a
player, and gives way to a player that takes the cue over until the deck
starts again. With no mixer on the link the deck plays through PipeWire
as it does without `--pro-link` (or as `--audio` says): it moves to the
mixer's cue as soon as one is heard, and back two seconds after the last
one goes quiet, playing on throughout. The status bar says where the
deck is heard and where the cue stands.

LINK CUE works end to end on an emulated DJM-A9 (firmware 1.22) and
DJM-900NXS (firmware 1.32), but has not yet been tried on a real mixer.

Documentation
-------------
 * `docs/OVERVIEW.md`: what cyanbox is, and the decisions behind it
 * `docs/HACKING.md`: code style, layout, tests, running the interface while
   developing
 * `docs/UI.md`: the interface, control by control
 * `docs/BEATS.md`: the beat-grid analyser
 * `docs/KEY.md`: the key detector
 * `docs/ML.md`: the tagging model
 * `docs/LINK.md`: PRO DJ LINK and LINK CUE
 * `docs/pdb.md`, `docs/anlz.md`, `docs/onelibrary.md`, `docs/settings.md`:
   the stick formats

cyanbox is under the BSD 2-Clause licence (`LICENSE.md`).
