#!/bin/sh
# Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md.
#
# The command line end to end, in a scratch directory: init a library, add a
# generated 128 BPM FLAC, show its grid, give it a colour (a hue, by degrees
# and by name), make a playlist, a dry-run export that says which colour
# the stick gets, a real export to a directory and inspect it, sync and
# pull refusing a directory that is no stick, settings, results of an
# older analyser counted and analysed again (by the export, not by its
# dry run, which leaves the library as it was), and the exit codes. The
# FLAC carries a comment tag, which the library leaves out unless
# import.comment says to take it.
# Then the edges: init over a library, a relative --library, an id that
# also names a directory, an unreadable file, output that cannot be
# written. No audio device and no window are used.
#
# Usage: cli_test.sh RXBOX SCRATCH_DIR

set -eu

rxbox=$1
dir=$2
# Some steps run from another directory.
case $rxbox in
/*) ;;
*) rxbox=$PWD/$rxbox ;;
esac
lib=$dir/lib
track=$dir/music/click128.flac

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

run() {
    echo "+ rxbox $*" >&2
    "$rxbox" --audio none -c "$lib" "$@"
}

rm -rf "$dir"
mkdir -p "$dir/music" "$dir/dry" "$dir/stick" "$dir/stale"

# 40 s of a 55 Hz kick on every beat at 128 BPM with a click on top.
beat=0.46875
ffmpeg -v error -nostdin -y -f lavfi \
    -i "aevalsrc=exprs='0.8*sin(2*PI*55*mod(t,$beat))*exp(-mod(t,$beat)*25)+0.2*sin(2*PI*2000*t)*exp(-mod(t,$beat)*200)':s=44100:d=40" \
    -ac 2 -c:a flac -metadata title="Click 128" -metadata artist="rxbox test" \
    -metadata comment="from the file" "$track"

run init > "$dir/init.out" || fail "init"
[ -f "$lib/library.db" ] || fail "init made no library.db"

run add "$dir/music" > "$dir/add.out" || { cat "$dir/add.out"; fail "add"; }
cat "$dir/add.out"
grep -q "added 1 track" "$dir/add.out" || fail "add did not add the track"

run show "$track" > "$dir/show.out" || fail "show"
cat "$dir/show.out"
bpm=$(sed -n 's/^  grid: \([0-9.]*\) BPM.*/\1/p' "$dir/show.out" | head -n 1)
[ -n "$bpm" ] || fail "show printed no grid"
awk -v b="$bpm" 'BEGIN { exit !(b > 127.5 && b < 128.5) }' || fail "the grid is at $bpm BPM, not 128"
id=$(sed -n 's/^  id \([0-9]*\).*/\1/p' "$dir/show.out")
run show "$id" | grep -q "click128.flac" || fail "show by id"

# The colour is one of rekordbox's eight, named with the legend's words
# for it; a hue in degrees between them (an older library may hold one)
# shows the colour the stick gets. none clears it.
grep -q "^  colour: none$" "$dir/show.out" || fail "a new track has a colour"
grep -q "^  tags: none" "$dir/show.out" || fail "a new track has tags"
run color 200 "$id" | grep -q "click128.flac: 200° (not one of the eight; the stick gets aqua)$" ||
    fail "color 200"
run show "$id" | grep -q "^  colour: 200° (not one of the eight; the stick gets aqua)$" ||
    fail "show does not print the hue"
run color none "$track" | grep -q ": none$" || fail "color none"
run color purple "$track" | grep -q ": purple (calm and dark, ambient)$" || fail "color purple"
run color 276 "$track" | grep -q ": purple (calm and dark, ambient)$" || fail "color 276 is purple"
run color 400 "$track" > /dev/null 2>&1 && fail "color took 400"
run color cyan "$track" > /dev/null 2>&1 && fail "color took cyan"
run colour aqua "$track" | grep -q ": aqua (calm, smooth)$" || fail "colour aqua"

# The legend: eight settings, the defaults until changed, trimmed; ""
# puts one back. Only the eight colours are keys.
run config > "$dir/config.out" || fail "config"
grep -q "^color.pink = dark and intense, hard$" "$dir/config.out" || fail "config lists no color.pink"
grep -q "^color.purple = calm and dark, ambient$" "$dir/config.out" || fail "config lists no color.purple"
run config color.aqua "  still water " | grep -q "^color.aqua = still water$" ||
    fail "config color.aqua"
run config color.aqua | grep -q "^color.aqua = still water$" || fail "config color.aqua reads back"
run show "$id" | grep -q "^  colour: aqua (still water)$" || fail "show does not use the legend"
run config color.aqua "" | grep -q "^color.aqua = calm, smooth (the default)$" ||
    fail "config color.aqua \"\" did not reset it"
run config color.aqua | grep -q "^color.aqua = calm, smooth (the default)$" ||
    fail "config color.aqua is not back to the default"
run config color.cyan x > /dev/null 2>&1 && fail "config took color.cyan"

# The file's comment tag was read and left out: the comment is the user's.
run config | grep -q "^import.comment = false$" || fail "config does not list import.comment"
if python3 -c "import sqlite3" 2> /dev/null; then
    python3 - "$lib/library.db" <<'EOF' || fail "the file's comment went into the library"
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
n = db.execute("SELECT count(*) FROM track WHERE file_comment IS NULL AND comment IS NULL").fetchone()[0]
sys.exit(0 if n == 1 else 1)
EOF
fi

# Analysing again finds nothing to do.
run analyze | grep -q "nothing to analyse" || fail "analyze redid finished work"

run playlist create Warmup || fail "playlist create"
run playlist add Warmup "$track" || fail "playlist add"
run playlist list > "$dir/list.out" || fail "playlist list"
cat "$dir/list.out"
grep -q "Warmup (1 track)" "$dir/list.out" || fail "playlist list does not show the track"

run export --dry-run "$dir/dry" > "$dir/dry.out" || { cat "$dir/dry.out"; fail "export --dry-run"; }
cat "$dir/dry.out"
[ -z "$(ls -A "$dir/dry")" ] || fail "the dry run wrote to the stick"
grep -q "is not a mount point" "$dir/dry.out" || fail "no warning about a stick that is not mounted"
grep -q "Click 128 \[colour 193.5° (aqua)\]$" "$dir/dry.out" || fail "the dry run does not say the colour"

run export "$dir/stick" > "$dir/export.out" || { cat "$dir/export.out"; fail "export"; }
cat "$dir/export.out"
"$rxbox" inspect "$dir/stick" > "$dir/inspect.out" || { cat "$dir/inspect.out"; fail "inspect"; }
cat "$dir/inspect.out"
grep -q "no problems" "$dir/inspect.out" || fail "inspect found problems"

# sync and pull of a directory that holds no stick (a mistyped mount
# point) fail; they do not report a stick with nothing to sync.
run sync "$dir/stick" | grep -q "^nothing to sync" || fail "sync after the export"
for c in sync pull; do
    run "$c" "$dir/dry" > "$dir/nostick.out" 2>&1 && fail "$c of a directory that is not a stick"
    grep -q "not a stick" "$dir/nostick.out" || { cat "$dir/nostick.out"; fail "$c does not say why"; }
done
run sync --apply "$dir/dry" > /dev/null 2>&1 && fail "sync --apply of a directory that is not a stick"

run settings | grep -q "quantize" || fail "settings"
run check > "$dir/check.out" || { cat "$dir/check.out"; fail "check"; }
grep -q "^OK" "$dir/check.out" || fail "check"

# show prints the analyser versions, and nothing this rxbox analysed is
# older than it. Results at version 0 (a library from before the versions)
# are counted by check, said by show, and analysed again by analyze.
run show "$track" | grep -q "^  analyser versions: beats [0-9]*, key [0-9]*, waves [0-9]*$" ||
    fail "show prints no analyser versions, or calls them old"
grep -q "^no track has results from an older analyser" "$dir/check.out" ||
    { cat "$dir/check.out"; fail "check counts stale tracks in a new library"; }
if python3 -c "import sqlite3" 2> /dev/null; then
    python3 - "$lib/library.db" <<'EOF' || fail "cannot make the results old"
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
db.execute("UPDATE track SET beats_version = 0, key_version = 0, waves_version = 0")
db.commit()
db.close()
EOF
    run show "$track" | grep -q "beats 0 (older than [0-9]*)" ||
        fail "show does not say the grid is older"
    run check > "$dir/check.out" || fail "check of a stale library"
    grep -q "^1 track has results from an older analyser" "$dir/check.out" ||
        { cat "$dir/check.out"; fail "check does not count the stale track"; }
    run analyze > "$dir/stale.out" || { cat "$dir/stale.out"; fail "analyze of a stale track"; }
    cat "$dir/stale.out"
    grep -q "^analysing 1 track (1 made by an older analyser)" "$dir/stale.out" ||
        fail "analyze did not analyse the stale track again"
    run check | grep -q "^no track has results from an older analyser" ||
        fail "the track is still stale after analyze"
    run analyze | grep -q "nothing to analyse" || fail "analyze redid finished work"

    # The export's analysis uses the same notion: a dry run on a library
    # whose waveforms an older analyser made, then on one whose stored
    # waveforms are format version 1 (before the recalibrations), says it
    # would make them again and leaves the library as it was (a dry run
    # writes nothing); the export makes them again before it writes.
    library_sum() {
        cat "$lib/library.db" "$lib/library.db-wal" 2> /dev/null | cksum
    }
    python3 - "$lib/library.db" <<'EOF' || fail "cannot make the waveforms old"
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
db.execute("UPDATE track SET waves_version = 0")
db.commit()
db.close()
EOF
    before=$(library_sum)
    run export --dry-run "$dir/dry" > "$dir/dry-stale.out" ||
        { cat "$dir/dry-stale.out"; fail "export --dry-run of stale waveforms"; }
    grep -q "^1 track would be analysed first (1 made by an older analyser)" "$dir/dry-stale.out" ||
        { cat "$dir/dry-stale.out"; fail "the dry run does not say it would analyse stale waveforms"; }
    [ "$(library_sum)" = "$before" ] || fail "the dry run changed the library"
    run analyze > /dev/null || fail "analyze of the stale waveforms"
    python3 - "$lib/library.db" <<'EOF' || fail "cannot make the waveforms an older format"
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
for track_id, data in db.execute("SELECT track_id, data FROM waveform").fetchall():
    data = bytearray(data)
    data[4:8] = (1).to_bytes(4, "little")
    db.execute("UPDATE waveform SET data = ? WHERE track_id = ?", (bytes(data), track_id))
db.commit()
db.close()
EOF
    run show "$track" | grep -q "waves [0-9]* (older format)" ||
        fail "show does not say the waveforms are an older format"
    run check | grep -q "^1 track has results from an older analyser" ||
        fail "check does not count waveforms of an older format"
    before=$(library_sum)
    run export --dry-run "$dir/dry" > "$dir/dry-stale.out" ||
        { cat "$dir/dry-stale.out"; fail "export --dry-run of an older waveform format"; }
    cat "$dir/dry-stale.out"
    grep -q "^1 track would be analysed first (1 made by an older analyser)" "$dir/dry-stale.out" ||
        fail "the dry run does not say it would analyse waveforms of an older format"
    [ "$(library_sum)" = "$before" ] || fail "the dry run changed the library"
    [ -z "$(ls -A "$dir/dry")" ] || fail "the dry run wrote to the stick"
    run check | grep -q "^1 track has results from an older analyser" ||
        fail "the dry run analysed the stale track"
    run export "$dir/stale" > "$dir/stale.out" || { cat "$dir/stale.out"; fail "export of stale waveforms"; }
    grep -q "^analysing 1 track (1 made by an older analyser)" "$dir/stale.out" ||
        { cat "$dir/stale.out"; fail "the export did not analyse waveforms of an older format"; }
    run check | grep -q "^no track has results from an older analyser" ||
        fail "the export left the waveforms stale"
else
    echo "no python3 with sqlite3: the stale library is not tested" >&2
fi

# Taking a track out of a playlist (by id), then deleting a playlist; the
# track stays in the library.
run playlist create Spare > /dev/null || fail "playlist create Spare"
run playlist add Spare "$id" > /dev/null || fail "playlist add Spare"
run playlist remove Spare "$id" | grep -q "removed 1 track from Spare; 0 left" ||
    fail "playlist remove by id"
run playlist add Spare "$track" > /dev/null || fail "playlist add Spare again"
run playlist remove Spare "$track" | grep -q "removed 1 track from Spare; 0 left" ||
    fail "playlist remove by path"
run playlist remove Warmup Spare 2> /dev/null && fail "playlist remove of a track that is not one"
run playlist delete Spare | grep -q "deleted playlist Spare" || fail "playlist delete"
run playlist list | grep -q "Spare" && fail "the deleted playlist is still listed"
run show "$id" | grep -q "click128.flac" || fail "the track went with the playlist"
run playlist delete Spare 2> /dev/null && fail "deleting a playlist twice"

# init over a library keeps it; a relative --library is the same library.
run init | grep -q "already holds a library (1 track)" || fail "init over a library"
(cd "$dir" && "$rxbox" --audio none -c lib show "$id") | grep -q "click128.flac" ||
    fail "a relative --library"

# An id that also names a directory here (with no tracks under it) is the id.
(cd "$dir" && mkdir -p "$id" && "$rxbox" --audio none -c lib show "$id") > "$dir/idir.out" 2>&1 ||
    { cat "$dir/idir.out"; fail "an id that names a directory"; }
grep -q "click128.flac" "$dir/idir.out" || fail "show by an id that names a directory"

# Usage errors exit 1, failures 2.
set +e
"$rxbox" --no-such-option > /dev/null 2>&1
[ $? -eq 1 ] || fail "a usage error does not exit 1"
"$rxbox" -c > /dev/null 2>&1
[ $? -eq 1 ] || fail "-c without a value does not exit 1"
run show --bogus "$id" > /dev/null 2>&1
[ $? -eq 1 ] || fail "an unknown option after the command does not exit 1"
run show "$dir/no-such-track.flac" > /dev/null 2>&1
[ $? -eq 2 ] || fail "a failure does not exit 2"

# An empty value is no value; a bad argument prints the command's usage.
"$rxbox" --frames "" > /dev/null 2>&1
[ $? -eq 1 ] || fail "--frames \"\" does not exit 1"
"$rxbox" --frames= > /dev/null 2>&1
[ $? -eq 1 ] || fail "--frames= does not exit 1"
"$rxbox" --frames " 5" > /dev/null 2>&1
[ $? -eq 1 ] || fail "--frames \" 5\" does not exit 1"
"$rxbox" -c "" show "$id" > /dev/null 2>&1
[ $? -eq 1 ] || fail "-c \"\" does not exit 1"
for cmd in add analyze analyse show color colour export inspect pull sync rescan check config \
           settings init playlist; do
    run $cmd --bogus > /dev/null 2> "$dir/usage.err"
    [ $? -eq 1 ] || { cat "$dir/usage.err"; fail "$cmd --bogus does not exit 1"; }
    grep -q "^usage: rxbox $cmd" "$dir/usage.err" ||
        { cat "$dir/usage.err"; fail "$cmd --bogus prints no usage line"; }
done
run analyze --what= > /dev/null 2> "$dir/usage.err"
[ $? -eq 1 ] && grep -q "^usage: rxbox analyze" "$dir/usage.err" || fail "analyze --what="
run playlist add Warmup > /dev/null 2> "$dir/usage.err"
[ $? -eq 1 ] || fail "playlist add without tracks does not exit 1"
grep -q "^usage: rxbox playlist add NAME TRACK" "$dir/usage.err" ||
    { cat "$dir/usage.err"; fail "playlist add prints no usage line"; }

# `--` ends a command's options: what follows is a name, even "-f".
run show -- "$id" | grep -q "click128.flac" || fail "show -- ID"
run show -- -f > /dev/null 2>&1
[ $? -eq 2 ] || fail "show -- -f took -f for an option"
run analyze -- --force > /dev/null 2>&1
[ $? -eq 2 ] || fail "analyze -- --force took --force for an option"
run rescan -- > /dev/null || fail "rescan --"
run rescan -- -v > /dev/null 2>&1
[ $? -eq 1 ] || fail "rescan -- -v took -v for an option"
run playlist -- list > /dev/null || fail "playlist -- list"

# Output that cannot be written is a failure.
if [ -w /dev/full ]; then
    "$rxbox" --help > /dev/full 2> /dev/null
    [ $? -eq 2 ] || fail "--help into a full disk exits 0"
    run playlist list > /dev/full 2> /dev/null
    [ $? -eq 2 ] || fail "playlist list into a full disk exits 0"
fi

# An unreadable file: one message from the command, and a failure.
cp "$track" "$dir/music/locked.flac"
chmod 000 "$dir/music/locked.flac"
if ! [ -r "$dir/music/locked.flac" ]; then
    run add "$dir/music/locked.flac" > "$dir/locked.out" 2>&1
    [ $? -eq 2 ] || { cat "$dir/locked.out"; fail "adding an unreadable file does not exit 2"; }
    n=$(grep -c "^rxbox: cannot add" "$dir/locked.out")
    [ "$n" -eq 1 ] || { cat "$dir/locked.out"; fail "the unreadable file is reported $n times"; }
fi
chmod 644 "$dir/music/locked.flac"
set -e

# Asked for, the file's comment comes with a new track again.
run config import.comment true > /dev/null || fail "config import.comment true"
mkdir -p "$dir/music2"
cp "$track" "$dir/music2/again.flac"
run add "$dir/music2" > /dev/null || fail "add with import.comment"
if python3 -c "import sqlite3" 2> /dev/null; then
    python3 - "$lib/library.db" <<'EOF' || fail "import.comment true did not take the comment"
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
rows = db.execute("SELECT path, file_comment FROM track ORDER BY id").fetchall()
ok = [c for p, c in rows if p.endswith("/again.flac")] == ["from the file"]
ok = ok and [c for p, c in rows if p.endswith("/click128.flac")] == [None]
sys.exit(0 if ok else 1)
EOF
fi

rm -rf "$dir"
echo "cli: OK"
