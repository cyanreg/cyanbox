#!/bin/sh
# Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md.
#
# The interface's headless run (meson test --suite ui): builds a small
# library of synthetic tracks with the CLI, gives one of them hot cues,
# memory cues and loops, then runs the interface with the first track
# loaded and playing (CYANBOX_UI_AUTOLOAD) and saves frame 100
# (CYANBOX_UI_SCREENSHOT; --frames 120 bounds the run). Audio goes nowhere
# (--audio none), and the player stays off the session bus (--no-mpris).
# The first two tracks have one cover, which the library then holds once.
#
# It never runs on the user's session: it needs CYANBOX_UI_TEST=1 and a
# WAYLAND_DISPLAY of a private compositor (docs/HACKING.md, "Running the
# interface while developing"), else it reports skipped (77). With
# CYANBOX_UI_SWAYSOCK set to that private sway's socket it shoots every size
# in CYANBOX_UI_SIZES ("1600x900 1280x720" by default) by resizing the
# headless output; without it, one shot at the output's size. CYANBOX_UI_SHOW
# (passed through) opens panels and dialogs for the shot (src/ui/ui.cpp).
#
# Then the variants, each a run with input given at set frames
# (CYANBOX_UI_EVENTS) and the state the interface logs with the screenshot
# checked: CUE held while the window loses the keyboard is let go; Enter on
# another track while one plays loads it at once (the playing one pauses
# for the swap, the new one plays on, no question asked); the Add browser on
# a folder it cannot read says so; its finder finds what is typed under
# the folder and ticks the row Enter is pressed on; the tree's finder
# lists what it finds and opens the one Enter is pressed on; Delete after a click on a cue in the
# player's cue list deletes that cue and nothing else (with
# CYANBOX_UI_SWAYSOCK only: the click needs a known size). Then the Player
# settings dialog, shot open, and a value picked in it with two clicks,
# which must land in the library's setting table, and a player field's
# tooltip saying what it does (the clicks and the pointer with
# CYANBOX_UI_SWAYSOCK only). Then the interface following two of those
# settings: the overview type for the overview and the row previews, and
# the key display for the deck's KEY. Then the Link decks in the deck's
# title row with made-up players, the track menu's "Load in deck" items under the
# players' Eject/Load Lock, Link > Push settings offered and clicked, a
# track added to the TAG LIST and shown, one
# added to the History and shown, the History's sets with a separator
# between them (none sorted by title), the players given deck numbers in the
# status bar's Link menu, and the status bar's Link and LINK CUE words lit
# by where they stand. Last, File > Remove missing tracks
# (CYANBOX_UI_SHOW=remove-missing): it lists the fourth track and a fifth
# whose folder is gone too, names that folder, and a click on Remove takes
# both out of the library, the rest kept. Then Album Artists: opened and an
# artist shown with two clicks (with CYANBOX_UI_SWAYSOCK only).
#
# usage: screenshot.sh CYANBOX OUTDIR

set -eu

[ "${CYANBOX_UI_TEST:-0}" = 1 ] || { echo "CYANBOX_UI_TEST is not 1: skipped"; exit 77; }
[ -n "${WAYLAND_DISPLAY:-}" ] || { echo "no WAYLAND_DISPLAY: skipped"; exit 77; }
case "$WAYLAND_DISPLAY" in
wayland-0|wayland-1)
    echo "$WAYLAND_DISPLAY looks like the user's session; use a private headless sway: skipped"
    exit 77;;
esac
[ $# -eq 2 ] || { echo "usage: $0 CYANBOX OUTDIR" >&2; exit 2; }
command -v ffmpeg > /dev/null || { echo "no ffmpeg: skipped"; exit 77; }

CYANBOX=$1
OUT=$2
LIB=$OUT/library
AUDIO=$OUT/audio
rm -rf "$LIB" "$AUDIO"
mkdir -p "$LIB" "$AUDIO"
unset CYANBOX_LIBRARY

rx() {
    "$CYANBOX" -c "$LIB" "$@"
}

# A cover for the first two tracks: a gradient, embedded as the attached
# picture.
ffmpeg -nostdin -v error -y -f lavfi -i "gradients=s=320x320:c0=0xff8c00:c1=0x1f3b73:x0=0:y0=0:x1=320:y1=320:d=1" \
    -frames:v 1 "$OUT/cover.png"

# synth NAME BPM SECONDS TITLE ARTIST GENRE [COVER]: a kick on every beat
# (a 55 Hz sine with a fast decay), a 220 Hz tone on the first half of
# every bar, off-beat noise hats, and a bass line a fifth up every other bar.
synth() {
    name=$1 bpm=$2 dur=$3 title=$4 artist=$5 genre=$6 cover=${7:-}
    # A number, not "(60/128)": aevalsrc spins on that form.
    beat=$(awk "BEGIN { printf \"%.9f\", 60 / $bpm }")
    expr="0.75*sin(2*PI*55*t)*exp(-14*mod(t\,$beat))"
    expr="$expr+0.18*sin(2*PI*220*t)*lt(mod(t\,4*$beat)\,2*$beat)"
    expr="$expr+0.12*sin(2*PI*82.5*t)*lt(mod(t\,8*$beat)\,4*$beat)"
    expr="$expr+0.08*(random(0)-0.5)*exp(-60*mod(t+$beat/2\,$beat))"
    if [ -n "$cover" ]; then
        ffmpeg -nostdin -v error -y -f lavfi -i "aevalsrc=$expr:s=44100:d=$dur" -i "$cover" \
            -map 0:a -map 1:v -c:a flac -c:v png -disposition:v attached_pic -ac 2 \
            -metadata title="$title" -metadata artist="$artist" -metadata genre="$genre" \
            -metadata album="cyanbox test tones" -metadata date=2026 "$AUDIO/$name.flac"
    else
        ffmpeg -nostdin -v error -y -f lavfi -i "aevalsrc=$expr:s=44100:d=$dur" -c:a flac -ac 2 \
            -metadata title="$title" -metadata artist="$artist" -metadata genre="$genre" \
            -metadata album="cyanbox test tones" -metadata date=2026 "$AUDIO/$name.flac"
    fi
}

synth 01-house 128 48 "Warehouse Tool (Extended Mix)" "Test Tones" "House" "$OUT/cover.png"
synth 02-dnb 174 36 "Rolling Amen" "Breaks Unit" "Drum & Bass" "$OUT/cover.png"
synth 03-jpop 100 36 "夜明けのビート" "テスト・トーンズ" "J-Pop"
synth 04-techno 135 30 "Missing In Action" "Gone Away" "Techno"

rx init > /dev/null
rx add "$AUDIO" > "$OUT/add.log" 2>&1 || { cat "$OUT/add.log"; exit 1; }
rx playlist create "Warm-up" > /dev/null
rx playlist create "Peak/Main room" > /dev/null
rx playlist add "Warm-up" "$AUDIO/01-house.flac" "$AUDIO/03-jpop.flac" > /dev/null
rx playlist add "Peak/Main room" "$AUDIO/02-dnb.flac" "$AUDIO/04-techno.flac" > /dev/null

ID=$(rx show "$AUDIO/01-house.flac" | sed -n 's/^  id \([0-9]*\).*/\1/p' | head -n 1)
[ -n "$ID" ] || { echo "no id for the first track"; rx show "$AUDIO/01-house.flac"; exit 1; }
ID2=$(rx show "$AUDIO/02-dnb.flac" | sed -n 's/^  id \([0-9]*\).*/\1/p' | head -n 1)

# Hot cues, memory cues and loops on the first track, written straight
# into the library (no CLI command sets cues). 128 BPM: a bar is 1.875 s.
if command -v python3 > /dev/null; then
    python3 - "$LIB/library.db" "$ID" <<'EOF'
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
tid = int(sys.argv[2])
bar = 60 / 128 * 4
cues = [
    # time, hot, loop_end, loop_beats, active, name, color
    (1 * bar, 0, None, None, 0, "", ""),
    (4 * bar, 1, None, None, 0, "build", "blue"),
    (8 * bar, 2, 10 * bar, 8.0, 0, "", ""),
    (12 * bar, 4, None, None, 0, "drop", "pink"),
    (16 * bar, 7, None, None, 0, "", "green"),
    (0.0, -1, None, None, 0, "", ""),
    (8 * bar, -1, None, None, 0, "intro end", ""),
    (18 * bar, -1, 20 * bar, 8.0, 1, "outro loop", "orange"),
]
for c in cues:
    db.execute("INSERT INTO cue (track_id, time, grid_beat, hot, loop_end, loop_beats, active, name, color)"
               " VALUES (?, ?, NULL, ?, ?, ?, ?, ?, ?)", (tid, c[0], c[1], c[2], c[3], c[4], c[5], c[6]))
db.execute("UPDATE track SET rating = 4 WHERE id = ?", (tid,))
db.commit()
EOF
fi
# Its colour, a hue: orange's.
rx color orange "$ID" > /dev/null

# The fourth track goes missing.
rm -f "$AUDIO/04-techno.flac"

shoot() {
    png=$1
    rm -f "$png"
    CYANBOX_UI_AUTOLOAD=$ID CYANBOX_UI_AUTOLOAD_AT=${CYANBOX_UI_AUTOLOAD_AT:-13.5} \
    CYANBOX_UI_SCREENSHOT=$png timeout 120 \
        "$CYANBOX" -c "$LIB" --audio none --no-mpris --frames ${FRAMES:-120} > "$png.log" 2>&1 || {
        echo "cyanbox failed:"; cat "$png.log"; exit 1; }
    [ -s "$png" ] || { echo "no screenshot at $png"; cat "$png.log"; exit 1; }
    echo "screenshot: $png"
}

# logged NAME STATE: the run NAME's logged state matches STATE as well.
logged() {
    grep "ui test:" "$OUT/variant-$1.png.log" | grep -Eq "$2" || {
        echo "FAIL: variant $1: want \"$2\""; cat "$OUT/variant-$1.png.log"; exit 1; }
}

# variant NAME EVENTS STATE: a shot with CYANBOX_UI_EVENTS, whose logged
# state ("ui test: frame N track ID playing P position S popup Q") must
# match the extended regular expression STATE.
variant() {
    name=$1 events=$2 want=$3
    export CYANBOX_UI_EVENTS="$events"
    shoot "$OUT/variant-$name.png"
    unset CYANBOX_UI_EVENTS
    state=$(grep "ui test:" "$OUT/variant-$name.png.log" || true)
    echo "$name: $state"
    echo "$state" | grep -Eq "$want" || {
        echo "FAIL: variant $name: want \"$want\""; cat "$OUT/variant-$name.png.log"; exit 1; }
}

if [ -n "${CYANBOX_UI_SWAYSOCK:-}" ]; then
    for size in ${CYANBOX_UI_SIZES:-1600x900 1280x720}; do
        SWAYSOCK=$CYANBOX_UI_SWAYSOCK swaymsg -q output '*' resolution "$size"
        shoot "$OUT/cyanbox-$size.png"
    done
else
    shoot "$OUT/cyanbox.png"
fi

# The table showed both covered tracks, whose thumbnails the library
# stores as it makes them: one cover, two tracks linked to it.
if command -v python3 > /dev/null; then
    python3 - "$LIB/library.db" <<'EOF' || exit 1
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
covers, links = db.execute("SELECT (SELECT count(*) FROM artwork), "
                           "(SELECT count(DISTINCT artwork_id) || '/' || count(*) "
                           " FROM track_artwork)").fetchone()
print(f"covers: {covers}, track links (distinct/all) {links}")
if (covers, links) != (1, "1/2"):
    sys.exit("FAIL: want one cover for the two covered tracks")
EOF
fi

# The track plays from 13.5 s and its cue point is the memory cue at 0. C
# pauses it there; C held previews; the key comes up as the window loses
# the keyboard, which ImGui reports as every key cleared: the preview
# stops and the deck is back on the cue point.
variant cue-focus "70:down:C,72:up:C,80:down:C,88:up:C,88:focus-out" \
    "track $ID playing 0 position 0\.00 "

# Enter on the row above while the deck plays (the collection is the
# album, by title: "Rolling Amen" above "Warehouse Tool"): the deck
# pauses for the swap and the other track loads and plays on; nothing
# asks.
FRAMES=160
export CYANBOX_UI_SCREENSHOT_FRAME=140
variant enter-loads "70:down:UpArrow,71:up:UpArrow,75:down:Enter,76:up:Enter" \
    "track $ID2 playing 1 .*popup 0"
unset FRAMES CYANBOX_UI_SCREENSHOT_FRAME

# The Add browser on a folder it cannot read ($HOME/Music) says why.
if [ "$(id -u)" != 0 ]; then
    chmod -R u+rwx "$OUT/home" 2> /dev/null || true
    rm -rf "$OUT/home"
    mkdir -p "$OUT/home/Music/inside"
    chmod 000 "$OUT/home/Music"
    (
        export XDG_CACHE_HOME="${XDG_CACHE_HOME:-$HOME/.cache}" HOME="$OUT/home" CYANBOX_UI_SHOW=add
        shoot "$OUT/variant-add-noperm.png"
    ) || { chmod 755 "$OUT/home/Music"; exit 1; }
    chmod 755 "$OUT/home/Music"
fi

# The Add browser's finder: "felic" typed into its field finds, under
# $HOME/Music and every folder below, the two Felicidade files and Felix
# (the folder and the file in it, its letters further apart), not the
# hidden copy or the text file; the one in a folder of its own ranks
# first (its name starts a path component), the other Felicidade second
# (the word whole, ahead of Felix's scattered letters). Down moves the
# cursor to the second, Enter ticks it.
rm -rf "$OUT/findhome"
m="$OUT/findhome/Music"
mkdir -p "$m/AD:HOUSE 11 [FLAC]" "$m/Bossa" "$m/Felix the Cat" "$m/Notes" "$m/.hidden"
touch "$m/AD:HOUSE 11 [FLAC]/2.01 - Felicidade.flac" "$m/AD:HOUSE 11 [FLAC]/2.02 - Hot Vacation.flac" \
      "$m/Bossa/Felicidade (live).mp3" "$m/Felix the Cat/Theme.flac" "$m/Notes/felicidade.txt" \
      "$m/.hidden/Felicidade.flac"
(
    export XDG_CACHE_HOME="${XDG_CACHE_HOME:-$HOME/.cache}" HOME="$OUT/findhome" CYANBOX_UI_SHOW=add
    variant add-find "40:text:felic,60:down:DownArrow,62:up:DownArrow,70:down:Enter,72:up:Enter" \
        'find "felic" matches 4 of 8 best "Bossa/Felicidade \(live\)\.mp3" cursor "AD:HOUSE 11 \[FLAC\]/2\.01 - Felicidade\.flac" picked 1'
)
rm -rf "$OUT/findhome"

# The tree's finder: Ctrl+P gives it the keyboard; "pea" finds the
# playlist in the folder Peak (as "Peak/Main room") and nothing else, the
# list shown in the tree's place. Enter on "main" opens that playlist, on
# "tones" the album, and the field empties.
variant tree-find "40:down:Ctrl,41:down:P,43:up:P,44:up:Ctrl,50:text:pea" \
    'tree find "pea" matches 1 cursor "Peak/Main room"'
variant tree-find-playlist "40:down:Ctrl,41:down:P,43:up:P,44:up:Ctrl,50:text:main,70:down:Enter,72:up:Enter" \
    'view "Main room"'
variant tree-find-album "40:down:Ctrl,41:down:P,43:up:P,44:up:Ctrl,50:text:tones,70:down:Enter,72:up:Enter" \
    'view "cyanbox test tones"'

# Delete on a selected cue: a click on the cue list's first row (the
# memory cue at 0, where the list sits at 1600x900) selects it, Delete
# deletes it and only it (nine cues, eight left: a second Delete finds
# nothing selected and leaves the rest alone), Escape gives the keyboard
# back to the track list. The click needs a known layout, so only with
# CYANBOX_UI_SWAYSOCK to size the output.
if [ -n "${CYANBOX_UI_SWAYSOCK:-}" ]; then
    SWAYSOCK=$CYANBOX_UI_SWAYSOCK swaymsg -q output '*' resolution 1600x900
    variant cue-delete "70:click:1320:409,75:down:Delete,76:up:Delete,80:down:Delete,81:up:Delete,85:down:Escape,86:up:Escape" \
        "track $ID .*cues 8 cue-keys 0"
fi

# Player settings (CYANBOX_UI_SHOW=settings): a shot of the dialog on its
# Player tab. Then, at 1600x900 where the dialog sits centred, a click on
# the first field's combo (On Air Display, On by default) and one on its
# second item, Off: the library's setting table must hold
# player.on_air_display = false afterwards, as `cyanbox config` reads it
# back, so a change that never reaches the library fails here. Then the
# pointer resting on a label (Vinyl Speed Adjust, the right-hand column)
# shows that field's tooltip, and every player field has a description.
# The clicks need a known layout, so only with CYANBOX_UI_SWAYSOCK.
(
    export CYANBOX_UI_SHOW=settings
    shoot "$OUT/settings.png"
) || exit 1
if [ -n "${CYANBOX_UI_SWAYSOCK:-}" ]; then
    SWAYSOCK=$CYANBOX_UI_SWAYSOCK swaymsg -q output '*' resolution 1600x900
    was=$(rx config player.on_air_display)
    [ "$was" = "player.on_air_display is not set" ] || {
        echo "FAIL: settings-write: the library already has a value: $was"; exit 1; }
    export CYANBOX_UI_SHOW=settings
    variant settings-write "70:click:685:303,80:click:685:350" "track $ID .*popup 0"
    variant settings-tip "60:move:867:478" 'settings tip "vinyl_speed_adjust" undescribed ""$'
    unset CYANBOX_UI_SHOW
    now=$(rx config player.on_air_display)
    echo "settings-write: $now"
    [ "$now" = "player.on_air_display = false" ] || {
        echo "FAIL: settings-write: want player.on_air_display = false (the clicks are on the" \
             "first combo and its second item at 1600x900: $OUT/variant-settings-write.png)"
        cat "$OUT/variant-settings-write.png.log"; exit 1; }
fi

# The players' My Settings drive the interface: at their defaults the
# overview and the row previews are the stacked half and the deck's KEY is
# the classic name; with device.overview_waveform full and
# device.key_display alphanumeric set (by `cyanbox config`, as the Player
# settings dialog writes them) both are drawn centred and KEY is the
# Camelot code. The log says how the last frame drew them and what KEY
# read; the names to expect are the ones `cyanbox show` gives for the track.
keys=$(rx show "$AUDIO/01-house.flac" | sed -n 's/^  key: \([^ ]*\) (Camelot \([0-9]*[AB]\),.*/\1 \2/p')
[ -n "$keys" ] || { echo "FAIL: settings-follow: the first track has no key"; exit 1; }
classic=${keys% *}
camelot=${keys#* }
variant settings-default "" "track $ID .*overview half previews half key $classic\$"
rx config device.overview_waveform full > /dev/null
rx config device.key_display alphanumeric > /dev/null
variant settings-follow "" "track $ID .*overview centred previews centred key $camelot\$"

# The Link decks (CYANBOX_UI_LINK_DECKS: players made up, no network): two
# share the deck's title row. With the players' Eject/Load Lock at its
# default, Lock, a right click on the first row (at 1600x900, under the
# taller title row) opens the track menu, whose "Load in deck" items offer
# deck 1, which plays, greyed out, and deck 2 not; at Unlock, both.
(
    export CYANBOX_UI_LINK_DECKS="1:CDJ-900:$ID:pa:-1,2:CDJ-2000NXS2:$ID2:m:40"
    variant link-decks "" "link decks 2"
    # The status bar's words lit: Link's green on the link, yellow while
    # joining it; LINK CUE's unlit with no mixer, green while the cue is
    # cyanbox's (elected, or nobody's yet), yellow while the mixer has a
    # player elected, its LINK CUE on or off.
    logged link-decks 'link word "Link: 2 players" lit green$'
    logged link-decks 'link cue "LINK CUE: no mixer" lit none$'
    export CYANBOX_UI_LINK_JOINING=1
    variant link-joining "" 'link word "Link: 2 players" lit yellow$'
    unset CYANBOX_UI_LINK_JOINING
    for cue in "DJM-A9:1:17|LINK CUE: DJM-A9|green" "DJM-A9:1:0|LINK CUE: DJM-A9|green" \
               "DJM-A9:1:2|LINK CUE: DJM-A9 has player 2|yellow" \
               "DJM-A9:0:2|LINK CUE: off on DJM-A9|yellow"; do
        export CYANBOX_UI_LINK_CUE="${cue%%|*}"
        rest=${cue#*|}
        variant "link-cue-${CYANBOX_UI_LINK_CUE%%:*}-$(echo "${CYANBOX_UI_LINK_CUE#*:}" | tr : -)" "" \
            "link cue \"${rest%|*}\" lit ${rest#*|}\$"
    done
    unset CYANBOX_UI_LINK_CUE
    if [ -n "${CYANBOX_UI_SWAYSOCK:-}" ]; then
        SWAYSOCK=$CYANBOX_UI_SWAYSOCK swaymsg -q output '*' resolution 1600x900
        variant link-load-locked "60:rclick:600:571" 'load menu locked "1"'
        rx config player.eject_lock false > /dev/null
        variant link-load-unlocked "60:rclick:600:571" 'load menu locked ""'
        rx config player.eject_lock "" > /dev/null
        # Link > Push settings (where the last run's log says the menu bar's
        # Link is): offered with players on the link, and pushing; the
        # players made up, with no link to push on, it says so.
        bar=$(sed -n 's/.*link menu bar at \([0-9]*\) \([0-9]*\).*/\1:\2/p' \
              "$OUT/variant-link-load-unlocked.png.log" | tail -n 1)
        variant link-push-menu "60:click:$bar" 'push settings at [0-9]+ [0-9]+ enabled 1$'
        item=$(sed -n 's/.*push settings at \([0-9]*\) \([0-9]*\).*/\1:\2/p' \
               "$OUT/variant-link-push-menu.png.log" | tail -n 1)
        variant link-push "60:click:$bar,70:click:$item" \
            'status "Push settings: not on the link yet"$'
        # The TAG LIST (--pro-link only): Add to Tag List in the track
        # menu of the second row (the first, missing, has Relocate too),
        # then a click on the tree's Tag List shows it, that one track in it.
        variant tag-list "60:rclick:600:611,70:click:650:794,80:click:60:640" \
            'view "Tag List" rows 1'
        # The History (--pro-link only), kept in the library: Add to History
        # in the same menu, then a click on the tree's History shows it.
        variant history "60:rclick:600:611,70:click:650:813,80:click:60:667" \
            'view "History" rows 1'
        # Its sets: three tracks played 3 hours ago, 2 minutes apart, the
        # one just added among them, and one an hour ago: every play a row,
        # a separator, lower than a row, where more than 15 minutes pass,
        # saying when the set before ended and the next started; none
        # sorted by another column. A click on the latest play of the track
        # played twice selects that row alone, and Remove from History in
        # its menu takes off that play, not the track's other.
        if command -v python3 > /dev/null; then
            python3 - "$LIB/library.db" <<'PY' || exit 1
import sqlite3, sys, time
db = sqlite3.connect(sys.argv[1])
had = [r[0] for r in db.execute("SELECT track_id FROM deck_history")]
ids = [r[0] for r in db.execute("SELECT id FROM track ORDER BY id") if r[0] not in had]
now = int(time.time())
for tid, ago, player in zip(ids + had, (3 * 3600, 3 * 3600 - 240, 3600, 3 * 3600 - 120),
                            (1, 2, 1, 1)):
    db.execute("INSERT INTO deck_history (track_id, played_at, player) VALUES (?, ?, ?)",
               (tid, now - ago, player))
db.commit()
PY
            variant history-sets "60:click:60:667" \
                'history breaks 2 "Set ended [A-Z][a-z]+ [0-9]+ [A-Z][a-z]+ [0-9]{2}:[0-9]{2} . next started ([A-Z][a-z]+ [0-9]+ [A-Z][a-z]+ )?[0-9]{2}:[0-9]{2}"$'
            logged history-sets 'view "History" rows 5$'
            variant history-by-title "60:click:60:667,70:click:520:545" 'view "History" rows 5$'
            if grep -q "history breaks" "$OUT/variant-history-by-title.png.log"; then
                echo "FAIL: history-by-title: separators sorted by title"
                cat "$OUT/variant-history-by-title.png.log"; exit 1
            fi
            variant history-play "60:click:60:667,70:click:600:571" 'rows selected 1$'
            variant history-remove-menu "60:click:60:667,70:rclick:600:571" \
                'remove from history at [0-9]+ [0-9]+$'
            item=$(sed -n 's/.*remove from history at \([0-9]*\) \([0-9]*\).*/\1:\2/p' \
                   "$OUT/variant-history-remove-menu.png.log" | tail -n 1)
            variant history-remove "60:click:60:667,70:rclick:600:571,80:click:$item" \
                'view "History" rows 4$'
        fi

        # The deck numbers: a click on the status bar's Link button (where
        # the last run's log says it is) opens its menu, a row of numbers
        # for each player; a click on player 2's 1 gives it deck 1, and
        # player 1, which showed 1, takes its 2: the cells go player 2, then
        # player 1, and the library keeps each number's player by hardware
        # address (02:00:00:00:00:11 and :22). Run again, the numbers stand;
        # with the two players come up the other way round (the same
        # addresses, the numbers swapped), each keeps its deck. The players'
        # own numbers in the menu takes them all back.
        export CYANBOX_UI_LINK_DECKS="1:CDJ-900:$ID:pa:-1:17,2:CDJ-2000NXS2:$ID2:m:40:34"
        button=$(sed -n 's/.*link button at \([0-9]*\) \([0-9]*\).*/\1:\2/p' \
                 "$OUT/variant-history.png.log" | tail -n 1)
        variant link-menu "60:click:$button" 'link menu player 2 deck 1 at [0-9]+ [0-9]+'
        at() {
            sed -n "s/.*link menu $1 at \([0-9]*\) \([0-9]*\).*/\1:\2/p" \
                "$OUT/variant-link-menu.png.log" | tail -n 1
        }
        two_one=$(at "player 2 deck 1")
        own=$(at "own numbers")
        variant link-number "60:click:$button,70:click:$two_one" 'link decks 2 numbers 1=2 2=1$'
        given() {
            command -v python3 > /dev/null || return 0
            python3 - "$LIB/library.db" "$1" <<'EOF'
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
rows = db.execute("SELECT key, value FROM setting WHERE key LIKE 'ui.link_deck.%' ORDER BY key").fetchall()
got = " ".join(f"{k}={v}" for k, v in rows)
print(f"deck numbers: {got or 'none'}")
if got != sys.argv[2]:
    sys.exit(f"FAIL: want {sys.argv[2] or 'none'}")
EOF
        }
        given "ui.link_deck.1=02:00:00:00:00:22 ui.link_deck.2=02:00:00:00:00:11" || exit 1
        variant link-number-kept "" 'link decks 2 numbers 1=2 2=1$'
        export CYANBOX_UI_LINK_DECKS="1:CDJ-2000NXS2:$ID2:m:40:34,2:CDJ-900:$ID:pa:-1:17"
        variant link-number-moved "" 'link decks 2 numbers 1=1 2=2$'
        export CYANBOX_UI_LINK_DECKS="1:CDJ-900:$ID:pa:-1:17,2:CDJ-2000NXS2:$ID2:m:40:34"
        variant link-number-own "60:click:$button,70:click:$own" 'link decks 2 numbers 1=1 2=2$'
        given "" || exit 1
    fi
) || exit 1

# File > Remove missing tracks: a fifth track, in a folder of its own that
# then goes, as a drive not plugged in would. The dialog lists it and the
# fourth (missing since the start) and counts one in a folder gone; the
# log says where its Remove button is, and a click there (in a run of the
# same size) removes both. The other three stay, the loaded first among
# them.
mkdir -p "$OUT/gone"
cp "$AUDIO/02-dnb.flac" "$OUT/gone/05-gone.flac"
rx add "$OUT/gone/05-gone.flac" > "$OUT/add-gone.log" 2>&1 || { cat "$OUT/add-gone.log"; exit 1; }
rm -rf "$OUT/gone"
(
    export CYANBOX_UI_SHOW=remove-missing
    variant remove-missing-ask "" "remove-missing 2 gone 1 button [0-9]+ [0-9]+"
    xy=$(sed -n 's/.*remove-missing 2 gone 1 button \([0-9]*\) \([0-9]*\).*/\1:\2/p' \
         "$OUT/variant-remove-missing-ask.png.log" | tail -n 1)
    variant remove-missing "70:click:$xy" "track $ID .*popup 0"
) || exit 1
left=$(rx show "$AUDIO/01-house.flac" "$AUDIO/02-dnb.flac" "$AUDIO/03-jpop.flac" 2>&1 | grep -c '^  id ' || true)
[ "$left" = 3 ] || { echo "FAIL: remove-missing: want the three tracks there kept, have $left"; exit 1; }
for gone in "$AUDIO/04-techno.flac" "$OUT/gone/05-gone.flac"; do
    if rx show "$gone" 2>&1 | grep -q '^  id '; then
        echo "FAIL: remove-missing: $gone is still in the library"; rx show "$gone"; exit 1
    fi
done
echo "remove-missing: the two missing tracks removed, three kept"

# Album Artists: two of the album's three tracks get an album artist (an
# edit, as the library keeps one), which is then the album's; at 1600x900
# a click on the node's arrow opens it, one on its first row shows the
# artist's tracks, the whole album.
if [ -n "${CYANBOX_UI_SWAYSOCK:-}" ] && command -v python3 > /dev/null; then
    python3 - "$LIB/library.db" "$ID" "$ID2" <<'EOF' || exit 1
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
n = db.execute("UPDATE track SET album_artist = 'Test Tones' WHERE id IN (?, ?)",
               (int(sys.argv[2]), int(sys.argv[3]))).rowcount
db.commit()
if n != 2:
    sys.exit(f"FAIL: album-artists: {n} tracks given an album artist, want 2")
EOF
    SWAYSOCK=$CYANBOX_UI_SWAYSOCK swaymsg -q output '*' resolution 1600x900
    variant album-artists "70:click:19:593,80:click:100:621" 'view "Test Tones" rows 3'
fi
