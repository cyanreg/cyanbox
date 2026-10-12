#!/bin/sh
# Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md.
#
# The command line end to end, in a scratch directory: init a library, add a
# generated 128 BPM FLAC, show its grid, make a playlist, a dry-run export,
# a real export to a directory and inspect it, settings, and the exit codes.
# No audio device and no window are used.
#
# Usage: cli_test.sh RXBOX SCRATCH_DIR

set -eu

rxbox=$1
dir=$2
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

run export "$dir/stick" > "$dir/export.out" || { cat "$dir/export.out"; fail "export"; }
cat "$dir/export.out"
"$rxbox" inspect "$dir/stick" > "$dir/inspect.out" || { cat "$dir/inspect.out"; fail "inspect"; }
cat "$dir/inspect.out"
grep -q "no problems" "$dir/inspect.out" || fail "inspect found problems"

run settings | grep -q "quantize" || fail "settings"
run check | grep -q "^OK" || fail "check"

# Usage errors exit 1, failures 2.
set +e
"$rxbox" --no-such-option > /dev/null 2>&1
[ $? -eq 1 ] || fail "a usage error does not exit 1"
run show "$dir/no-such-track.flac" > /dev/null 2>&1
[ $? -eq 2 ] || fail "a failure does not exit 2"
set -e

rm -rf "$dir"
echo "cli: OK"
