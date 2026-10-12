# The interface

What `src/ui` draws, and how each control behaves. The reference is
rekordbox's Export mode in the "1 PLAYER" layout and rbxport's copy of it; the
numbers that rbxport measured from rekordbox are used where they exist. The
engine behaviour behind every button is in `include/rxbox/player.h`.

There is exactly one player. rxbox prepares tracks for a stick (cues, loops,
grid, metadata, playlists); it is not a mixing tool, so there is no second
deck, no sync and no mixer.

## Window

Dark theme, no title bar decorations beyond the compositor's. Default
1600×900, everything resizes. Three bands, top to bottom:

```
┌───────────────────────────────────────────────────────────────┐
│ PLAYER (full width)                                           │  ~40 %
├───────────┬───────────────────────────────────────────────────┤
│ playlists │ track table (search above it)                     │  rest
├───────────┴───────────────────────────────────────────────────┤
│ status bar                                                    │  22 px
└───────────────────────────────────────────────────────────────┘
```

The deck band / browser split and the tree / table split are draggable
(ImGui splitters) and remembered in `rxbox.toml` `[ui]`.

Colours (ImGui style): window background `#121212`, panels `#1E1E1E`,
raised buttons `#2B2B2B`, text `#E6E6E6`, dim text `#8A8A8A`, accent (the
player's frame while playing, active toggles) `#FF8C00` (rekordbox's
orange), quantize-on `#3CEB50`. The title row is tinted blue (`#1F3B73` at
low alpha), as rekordbox's deck 1 is.

## Player panel

The full width of the window. Rows, top to bottom:

1. **Title row** (height 56): the artwork (48×48, placeholder record icon
   when none), then title (bold) over artist, then the readouts right-aligned:
   key (classic by default, Camelot/Open Key per config), BPM as
   `128.00` with the tempo-adjusted value in orange when the slider is not at
   0, the time (elapsed / remaining, toggled by clicking or `T`), bar.beat
   counter (`17.3`).
2. **Overview waveform** (height 64): the whole track. The preview tag of the
   selected palette (PWV6 3-band by default; PWAV blue; PWV4 RGB) drawn
   centred with `rx_wave_draw` (inset 8 above, 2 below as rekordbox). Over
   it: memory cue markers (a red triangle head `#EA3323` at the top with a
   1 px line), hot cues (a filled square badge with the letter in the cue's
   colour, default pad colour when none), loop bands (translucent orange
   `#FF8C00` at 30 %; the active one brighter), the played part dimmed to
   55 %, the playhead (1 px white line, 2 px when playing). Click seeks;
   drag scrubs. The 1-second ticks are not drawn.
3. **Detail waveform** (height 160): the detail tag (PWV7/PWV3/PWV5) scrolling
   under a fixed centre playhead (2 px, white, with a small triangle at the
   top and bottom). Shows `zoom_bars` bars (default 12; steps 0.25 0.5 1 2 4 8
   12 16 32 64 via mouse wheel over it or the `+`/`−` buttons at its right
   edge; `RST` resets to 12). Grid: beat lines in `#4C4C4C`, downbeats in
   `#9A9A9A` 1 px, with the bar number printed small above every downbeat
   when ≥ 4 px apart; at 64 bars only downbeats are drawn. Cue markers as on
   the overview but taller; loop bands likewise; the current loop's in/out
   edges in solid orange. Dragging scrubs (the engine plays grains); the wheel
   zooms. Right-click opens the cue context menu (set memory cue here, set hot
   cue A–H here, delete, colour, name).
4. **Control row** (height 120), three groups left to right:
   - **Transport** (left, 120 wide): `CUE` (round, orange ring, lit while a
     cue point exists; pressed = down), `PLAY/PAUSE` (round, green ring, lit
     when playing), under them `Q` quantize toggle (green when on) and the
     beat jump pair `◄ ►` with the size button between (`4Beats` default;
     click cycles Fine/4Beats/8Beats/16Beats/8Bars/16Bars/32Bars; right-click
     picks from a menu).
   - **Pads** (centre, 2×4 grid of 56×36 pads): mode tabs above: `HOT CUE`,
     `BEAT LOOP`, `BEAT JUMP`, `MEMORY`.
     - HOT CUE: pads A–H. Empty: dark with the dim letter. Set: filled in the
       cue's colour (default per slot, `rx_hot_cue_default_rgb`) with the
       letter and the position (`1:02.5`) small; a hot loop shows a loop
       glyph. Press = set/call per the engine rules; shift-click or the pad's
       `×` (top-right corner on hover) deletes; right-click = colour and name.
     - BEAT LOOP: 1/4 1/2 1 2 4 8 16 32 (and /2, ×2 as the last two pads of
       the second row? no: `LOOP IN`, `LOOP OUT`, `EXIT/RELOOP`, `/2`, `×2`,
       `MEMORY LOOP` as a button strip under the pads in this mode). The pad
       of the running loop length is lit orange.
     - BEAT JUMP: −32 −16 −8 −4 +4 +8 +16 +32 bars/beats (beats).
     - MEMORY: `MEMORY` (store cue point / active loop), `◄ CUE`, `CUE ►`,
       `DELETE`, and the memory cue list to the right (scrolling): each row
       with its colour dot, type (cue/loop with length in beats), position,
       name (editable on double-click), an `ACTIVE` toggle for loops, a `×`.
       Clicking a row calls it.
   - **Tempo** (right, 90 wide): vertical slider ±6/±10/±16/WIDE (range
     button cycles), 0 detent, the percentage readout (`+0.00%`), `RESET`.
     No master tempo and no sync: this is a preparation tool.
5. **Grid tools** (collapsible strip, height 28, toggled by the `GRID` button
   in the title row): BPM field (editable, Enter applies to the section under
   the playhead), `TAP` (tap tempo: ≥ 4 taps set the BPM, the first tap the
   downbeat if `SET` is then pressed), `◄ ►` nudge the grid ±5 ms (shift:
   ±1 ms), `SET DOWNBEAT` (the nearest beat becomes beat 1), `SPLIT` (a new
   section at the nearest beat), `MERGE` (remove the boundary nearest the
   playhead), `×2`, `÷2` (double/halve the BPM of the section), `LOCK`
   (grid_locked toggle), `RE-ANALYSE`. Every change goes through
   `rx_ctl_grid_changed` and is saved at once. Cues set in bars follow.

The frame of the panel glows orange while playing. Loading a track: the
panel shows "LOADING" over a dimmed waveform until the decode job returns,
then the player auto-cues to the first memory cue (or 0).

## Browser

**Playlist tree** (left, 220 default): `Collection` at the top (every
track), then the folders and playlists from the library. Click selects the
view. Right-click: new playlist, new folder, rename, delete (with confirm),
import m3u8, export m3u8. Drag tracks from the table onto a playlist to
add; drag playlists and folders within the tree to move them.

**Search field** above the table: filters the current view on title,
artist, album, genre, comment, file name with `rx_str_contains_fold`
(Japanese full-width aware). `Ctrl+F` focuses it; `Esc` clears.

**Track table** (ImGui table, resizable/reorderable/sortable columns,
virtualised with the clipper): default columns in order: `#`, Preview
(128 px: the PWV6 stacked-half waveform via `rx_wave_draw` in
`RX_WAVE_HALF_STACKED`, 1200 columns into 128 px), Artwork (36 px
thumbnail, textures cached per track with an LRU of ~400), Track Title,
Key, BPM, Time, Rating (five stars, click sets), Artist, Color (a dot),
Hot Cue (the letters, coloured), Genre, Year, Date Added; Plays and Last
Played exist but are hidden by default. Row height 40 when
artwork is shown, else 22. Selected rows highlighted; the focused row has a
1 px outline. A row whose file is missing is drawn greyed out with a "missing" glyph,
cannot be loaded or analysed, and offers Relocate… in its context menu;
an unanalysed row shows `—` for BPM/key and a thin flat preview; a row being
analysed shows a small spinner in the Preview cell.

Actions: double-click loads the track into the player (a playing track
is never replaced without confirmation); drag onto the player panel loads
it; context menu: Load, Analyse (BPM/grid, key, both; force),
Remove from playlist, Remove from library, Edit info, Show in folder
(prints the path to the status bar and copies it), Set colour, Set rating.
Keyboard: up/down move the focus, Enter loads the focused track, Delete
removes it from the playlist.

**Info panel**: an "INFO" tab on the right of the table (toggle, 260 wide)
showing the focused track's editable fields: title, artist, album, album
artist, genre, label, remixer, composer, original artist, comment, year,
track/disc numbers, rating, colour, My Tags (comma separated), file facts
(type, size, rate, bit depth, bit rate, duration, path). Edits save on
focus loss as overrides in the library (the file's own tags are kept).

## Status bar

Left: the last message (`rx_app_set_status`). Middle: analysis progress
(`Analysing 12/340: Artist - Title`) with a thin bar; export progress
likewise. Right: audio state (`PipeWire 48 kHz · rxbox-null`, `Audio: none`
under `--audio none`, or the error in red), the library track count, the
GPU name.

## Dialogs and menus

A menu bar at the very top: **File** (Add files/folders… (ImGui file
browser: a simple directory lister with a path field, since there is no
native dialog), Export to USB…, Sync from USB…, Inspect USB…, Rescan
missing files, Quit),
**View** (waveform palette 3Band/Blue/RGB, key notation, show grid tools,
show info panel, ImGui demo), **Analysis** (analyse selection, analyse all
unanalysed, re-analyse selection, analysis settings…), **Help** (about,
keyboard map).

- **Analysis settings**: what to analyse (BPM/grid, key), BPM range presets
  (auto (default), 70–180, 98–195, 118–236, 58–115), integer snap, allow
  tempo sections, add memory cue at first downbeat.
- **Export**: destination path (with a list of mounted removable volumes
  read from /proc/mounts under /run/media and /media), whole library or
  selected playlists, prune, compatibility transcode, then a progress view
  with the stage, item, bar and a log; cancel; the result summary.
- **Sync from USB**: pick the mount, read the stick, then a table of the
  differences the player left (hot cues set/cleared, memory cues added/
  removed, plays from the history, settings changed), one checkbox per row
  (all accepted by default), Apply, and a summary.
- **Keyboard map** (read-only list).
- **About**.

## Keyboard

rekordbox's Export preset for player 1 (no shifted second-player set):

| Key | Action |
| --- | --- |
| Space | Play/Pause |
| C | Cue (press/hold/release semantics) |
| Q | Quantize |
| T | Time mode |
| M | Memory cue set |
| B / N | Previous / next memory cue |
| X | Delete memory cue |
| I / O / R | Loop in / out / exit-reloop |
| 4 5 6 7 8 9 | 1 2 4 8 16 32 beat loop |
| / and \ | Loop ÷2 / ×2 |
| 1 2 3 (and Ctrl+1..3) | Hot cue A B C set/call (clear) |
| F1–F8 … | not bound in v1 |
| ← → | Beat jump reverse / forward |
| Ctrl+← Ctrl+→ | Shift grid left/right (5 ms) |
| Ctrl+G | Toggle grid tools |
| A S D F G H J K L ; | Memory cue 1–10 |
| Ctrl+F | Search |
| Enter | Load the focused track |
| Ctrl+Q | Quit |

Hot cue pads D–H have no default keys (rekordbox binds none).

## Drawing rules worth repeating

- Waveforms are drawn only through `rx_wave_draw`; never re-rasterised on
  the CPU. The overview maps the whole column range to the rectangle; the
  detail maps `[playhead − zoom/2, playhead + zoom/2]` in bars converted to
  columns through the grid (bars → seconds via `rx_grid_offset_beats`, then
  × 150).
- The played part of the overview is dimmed with `dim`.
- While playing, the interface requests a redraw every frame (`rx_ui_frame`
  returns true); stopped, it draws nothing until input arrives.
- Everything is laid out from `rx_window_scale` so HiDPI works.
