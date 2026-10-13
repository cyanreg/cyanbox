# The interface

What `src/ui` draws, and how each control behaves. The reference is
rekordbox's Export mode in the "1 PLAYER" layout and rbxport's copy of it; the
numbers that rbxport measured from rekordbox are used where they exist. The
engine behaviour behind every button is in `include/rxbox/player.h`.

There is exactly one player. rxbox prepares tracks for a stick (cues, loops,
grid, metadata, playlists); it is not a mixing tool, so there is no second
deck, no sync and no mixer.

## Window

Dark theme, no decorations at all: the window is borderless, which on
Wayland asks for client-side decoration and draws none, so sway and the
like draw no title bar either. Default
1600×900, everything resizes. Three bands, top to bottom:

```
┌───────────────────────────────────────────────────────────────┐
│ PLAYER (full width)                                           │  ~50 %
├───────────┬─────────────────────────────────────────┬─────────┤
│ playlists │ track table (search above it)           │  INFO   │  rest
├───────────┴─────────────────────────────────────────┴─────────┤
│ status bar                                                    │  22 px
└───────────────────────────────────────────────────────────────┘
```

The player takes half the body by default, or its natural height (every
row with a 204 px detail waveform) when that is more, so at 1600×900 it
keeps its natural height and at 1920×1080 the detail grows. The natural
height is what it was with two rows of pads (a 140 px control row and a
160 px detail): the 44 px the one-row control row saves went to the
detail waveform. Whatever the
player has beyond its fixed rows goes to the detail waveform. The deck
band / browser split and the tree / table split are draggable (ImGui
splitters) and remembered in the library's `ui.split_player` and
`ui.split_tree` settings. The info panel is always shown, 260 wide, right
of the table.

Colours (ImGui style): window background `#121212`, panels `#1E1E1E`,
raised buttons `#2B2B2B`, text `#E6E6E6`, dim text `#8A8A8A`, accent (the
player's frame while playing, active toggles) `#FF8C00` (rekordbox's
orange), quantize-on `#3CEB50`. The title row is tinted blue (`#1F3B73` at
low alpha), as rekordbox's deck 1 is.

## Player panel

The full width of the window. Rows, top to bottom:

1. **Title row** (height 64): the artwork (56×56, placeholder record icon
   when none; clicking it ejects a stopped track), then the title (26 px
   bold) over the artist (15 px), then the readouts right-aligned, each a
   10.5 px caps label over a 24 px value, centred on the row: key (classic
   by default, Camelot/Open Key per config), BPM as `128.00` with the
   tempo-adjusted value in orange when the slider is not at 0, the time
   (elapsed / remaining, toggled by clicking or `T`), the bar.beat counter
   (`17.3`), and the `GRID` button.
2. **Overview waveform** (height 64): the whole track. The preview tag of the
   selected palette (PWV6 3-band by default; PWAV blue; PWV4 RGB) drawn
   with `rx_wave_draw` as the stacked half, growing from the strip's floor
   (inset 13 above, clear of the cue markers along the top edge, 1 below),
   at the tag's own levels with no gain: rbxport's default overview
   (`overviewWaveform: "half"` in its src/lib/preferences.ts), the row
   previews' mode. Centred with a gain, as before, the three-band levels of
   a loud track filled the strip solid cream. Over it: memory cue markers (a red triangle head `#EA3323` at the top with a
   1 px line), hot cues (a filled square badge with the letter in the cue's
   colour, default pad colour when none), loop bands (translucent orange
   `#FF8C00` at 30 %; the active one brighter), the played part dimmed to
   55 %, the playhead (1 px white line, 2 px when playing). Click seeks;
   drag scrubs. The 1-second ticks are not drawn.
3. **Detail waveform** (204 at the player's natural height; it takes all
   the height the player has beyond its other rows): the detail tag
   (PWV7/PWV3/PWV5) scrolling under a fixed centre playhead (2 px, white, with a small triangle at the
   top and bottom). Shows `zoom_bars` bars (default 12; steps 0.25 0.5 1 2 4 8
   12 16 32 64 via mouse wheel over it or the `+`/`−` buttons at its right
   edge; `RST` resets to 12). Grid: beat lines in `#4C4C4C`, downbeats in
   `#9A9A9A` 1 px, with the bar number printed small above every downbeat
   when ≥ 4 px apart; at 64 bars only downbeats are drawn. Cue markers as on
   the overview but taller; loop bands likewise; the current loop's in/out
   edges in solid orange. Dragging scrubs (the engine plays grains); the wheel
   zooms. Right-click opens the cue context menu (set memory cue here, set hot
   cue A–H here, delete, colour, name).
4. **Control row** (height 96: the 18 px labels, one 36 px row of pads,
   16 px, the 24 px strips, 2 px). There are no modes or tabs: every group
   is shown at once, left to right, 16 px apart. Each group but transport
   and tempo has a small dim caps label at its top (10.5 px bold); the pads
   sit under the labels, and the 24 px strips, the transport row, the lower
   MEMORY buttons and `MT` share the bottom line.
   - **Transport** (120 wide): `CUE` (round, orange ring, lit while a cue
     point exists; pressed = down) and `PLAY/PAUSE` (round, green ring, lit
     when playing), 56 across, centred on the pads; under them `Q` quantize toggle
     (green when on) and the beat jump pair `◄ ►` with the size button
     between (`4Beats` default; click cycles Fine/4Beats/8Beats/16Beats/
     8Bars/16Bars/32Bars; right-click picks from a menu). The arrows jump
     on the press and, held, again every 120 ms after 300 ms.
   - **HOT CUE**: pads A–H in one row of eight, 36 tall, 48 to 64 wide by
     the room (below). Empty: dark with
     the dim letter. Set: filled in the cue's colour (default per slot,
     `rx_hot_cue_default_rgb`) with the letter (15 px) over the name or
     position (`1:02.5`, 10.5 px); a hot loop shows a loop glyph. Press =
     set/call per the engine rules; shift-click or the pad's `×` (top-right
     corner on hover) deletes; right-click = colour and name. Under them
     the beat jump strip: `−32 −16 −8 −4 +4 +8 +16 +32` beats (with
     `◄`/`►` when they fit), repeating while held like the arrows.
   - **BEAT LOOP**: `1/4 1/2 1 2 4 8 16 32` beat loops in one row of eight
     like the hot cues, the pad of the running loop lit orange; under them
     the loop strip `IN`, `OUT`, `EXIT`/`RELOOP`, `÷2`, `×2`.
   - **MEMORY**: a 2×2 block of 96×26 buttons, the upper pair level with
     the pads, the lower on the strips' line: `MEMORY` (store the cue
     point or the active loop, `M`), `DELETE` (the memory cue at the
     playhead, `X`); `◄ CUE` and `CUE ►` (call the previous / next memory
     cue, `B` / `N`).
   - **CUES**: every cue of the track in one list sorted by time, rows 22
     tall in 12.5 px that scroll: memory cues with their colour dot, hot
     cues with their lettered badge, then the type (`CUE`, `LOOP 4`),
     position, bar.beat (only when the list is at least 290 wide), the
     name (a double-click or the row menu's Rename edits it in place;
     a hot cue's goes through the controls as the pad popup's does), an
     `ACTIVE` toggle for memory loops, a `×`. Clicking a row selects it
     (blue, as the table's rows) and calls the cue (a hot cue as its pad:
     press and release; a memory cue with `rx_ctl_memory_call`); the cue at
     the playhead has an orange bar at its left. While a row is selected
     the list has the keyboard: `Delete` deletes that cue (a hot cue
     through `rx_ctl_hot_cue_delete`, a memory cue as the row's `×`)
     instead of taking tracks out of the playlist, and `↑`/`↓` move the
     selection (without calling). After a Delete nothing is selected but
     the list keeps the keyboard, so a second Delete neither deletes the
     next cue nor removes a track. A click in the track table, `Esc` or
     another track loaded gives the keyboard back to the table. The
     selection follows the cue (its hot cue slot, or a memory cue's
     position), not the row, as cues come and go. Right-click: select,
     then rename, colour, delete. The count
     at the top right reads `3/10 memory · 2/8 hot`. The list takes the
     width the other groups leave, at least 240: the pads are
     `clamp((room − 16·3 − 196 − 240 − 2·7·4) / 16, 48, 64)` wide, where
     room is the space between transport and tempo, so they are 64 while
     the room allows (1920 wide: pads of 64, a list of 328) and shrink
     first (1600 wide: pads of 49, a list of 248). Once the pads are at 48
     the list shrinks; with less than 80 for it (below about 1400 wide) it
     is not drawn and MEMORY narrows into what is left, its labels
     shrinking. Narrower still, the groups are clipped short of tempo,
     never drawn over it.
   - **Tempo** (96 wide, the row's full height, having no caps label):
     vertical slider ±6/±10/±16/WIDE (range button
     cycles; the range always starts at ±6 %, as a CDJ switched on does:
     it is not kept in the layout, and an older layout's `TempoRange=` is
     ignored), 0 detent, the percentage readout (`+0.00%`) over its
     `TEMPO` caps, `RESET`, and, on the strips' line,
     `MT` (MASTER TEMPO, key lock: the pitch stays while the tempo
     changes), lit orange while the deck reports `master_tempo`. Its wish
     is kept in the layout (`MasterTempo=`) and given to the player on the
     first frame; a player that cannot keep the pitch refuses and the
     button stays off. No sync: this is a preparation tool.
5. **Grid tools** (collapsible strip, height 32 with 11 px legends, toggled
   by the `GRID` button in the title row): BPM field (editable, Enter applies to the section under
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
(the PWV6 stacked-half waveform via `rx_wave_draw` in
`RX_WAVE_HALF_STACKED`, its 1200 columns scaled to the cell; the one
column that stretches, taking the width the others leave: about 170 px at
1600×900, 490 at 1920×1080), Artwork (rbxport's 80 px column: the sleeve
fills the cell edge to edge, the row's full height, cropped as rbxport's
`object-fit: cover; object-position: center top` crops it, so the cover
keeps its full width and shows its top part, 80×40 of a square thumbnail
scaled to 80; without art, rbxport's record the column's width, anchored
at the top and cut by the row; textures cached per track with an LRU of
~400; the player's title-row sleeve stays square), Track Title
(260 px), Key, BPM, Time, Rating (five stars, click sets), Artist
(170 px), Year; Album, Location, Plays and Last Played exist but are
hidden by default (the header's context menu shows them). Track Title
and Artist are fixed widths, resizable by dragging their edges: stretched
they ran far wider than any title. There is no Date Added column (the
library keeps the date). There are no Color, Genre or Hot
Cue columns: the info panel shows and edits a track's colour and genre,
the player's cue list a loaded track's cues. Cell text is 14 px, the
title 15 px, `#` and the dates 12.5 px. The Key column has rekordbox's
Traffic Light: while a track is loaded, the Key cell of every key that
mixes with its key is filled green (`#3CEB50`, the whole cell, padding
included) with the key in black, the loaded track's own row included and
selected rows too (rbxport's `.cell[data-lit]`). The
reach is set in View > Traffic Light (setting `ui.traffic_light`): for a
loaded track in nL on the Camelot wheel (L its letter, M the other),
Same key lights nL, Related 1 adds nM, Related 2 adds (n−1)L and (n+1)L,
Related 3 (the default) adds (n−1)M and (n+1)M; the wheel wraps from 12
to 1. Playing A (11B) lights A, F♯m, D, E, Bm and D♭m, not A♭m. Off
turns it off. Narrower than the fixed columns and 100 px of preview
(about 1030 px) the table scrolls sideways rather than squeeze the
preview; at 1600×900 with the tree and the info panel nothing scrolls.
The table's saved column layout is kept under a new id since this column
set, so a layout saved for the old one starts from these defaults. Row height
40 when artwork is shown, else 22. Selected rows highlighted; the focused row has a
1 px outline. A row whose file is missing is drawn greyed out with a "missing" glyph,
cannot be loaded or analysed, and offers Relocate… in its context menu;
an unanalysed row shows `—` for BPM/key and a thin flat preview; a row being
analysed shows a small spinner in the Preview cell.

Actions: double-click loads the track into the player at once (a playing
track is paused and replaced; the new one waits at its auto-cue); drag
onto the player panel loads it; context menu: Load, Analyse (BPM/grid, key, both; force),
Remove from playlist, Remove from library, Show in folder
(prints the path to the status bar and copies it), Set colour, Set rating.
Keyboard: up/down move the focus, Enter loads the focused track, Delete
removes it from the playlist (unless a click on the player's cue list
has given that list the keyboard: then they act on the cues, until a
click in the table, `Esc` or another track). A click anywhere in the
table, rows or header, takes the keyboard back from the cue list.

**Info panel**: always shown right of the table (260 wide, at most 40 %
of the browser's width), the focused track's editable fields: title,
artist, album, album artist, genre, label, remixer, composer, original
artist, comment, year, track/disc numbers, rating, colour, My Tags (comma
separated), file facts (type, size, rate, bit depth, bit rate, duration,
path). Edits save on focus loss as overrides in the library (the file's
own tags are kept).

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
**View** (waveform colour 3Band/Blue/RGB, key notation, Traffic Light
off/same key/related 1–3, detail waveform centred/half, grid tools),
**Analysis** (analyse selection, analyse all
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
| ← → | Beat jump reverse / forward; held, again every 120 ms after 300 ms |
| Ctrl+← Ctrl+→ | Shift grid left/right (5 ms), once per press |
| Ctrl+G | Toggle grid tools |
| A S D F G H J K L ; | Memory cue 1–10 |
| Ctrl+F | Search |
| ↑ ↓ | Move the focus in the track table; after a click on a cue, move the cue list's selection |
| Enter | Load the focused track |
| Delete | Remove the selected tracks from the playlist; after a click on a cue, delete that cue (a hot cue or a memory cue) instead |
| Esc | Clear the search; give Delete and ↑ ↓ back to the track table |
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
