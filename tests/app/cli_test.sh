#!/bin/sh
# Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md.
#
# The command line end to end, in a scratch directory: init a library, add a
# generated 128 BPM FLAC, show its grid, make a playlist, a dry-run export,
# a real export to a directory and inspect it, settings, and the exit codes.
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
mkdir -p "$dir/music" "$dir/dry" "$dir/stick"

# 40 s of a 55 Hz kick on every beat at 128 BPM with a click on top.
beat=0.46875
ffmpeg -v error -nostdin -y -f lavfi \
    -i "aevalsrc=exprs='0.8*sin(2*PI*55*mod(t,$beat))*exp(-mod(t,$beat)*25)+0.2*sin(2*PI*2000*t)*exp(-mod(t,$beat)*200)':s=44100:d=40" \
    -ac 2 -c:a flac -metadata title="Click 128" -metadata artist="rxbox test" "$track"

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

run export "$dir/stick" > "$dir/export.out" || { cat "$dir/export.out"; fail "export"; }
cat "$dir/export.out"
"$rxbox" inspect "$dir/stick" > "$dir/inspect.out" || { cat "$dir/inspect.out"; fail "inspect"; }
cat "$dir/inspect.out"
grep -q "no problems" "$dir/inspect.out" || fail "inspect found problems"

run settings | grep -q "quantize" || fail "settings"
run check | grep -q "^OK" || fail "check"

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
for cmd in add analyze analyse show export inspect pull sync rescan check config settings init \
           playlist; do
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

rm -rf "$dir"
echo "cli: OK"
