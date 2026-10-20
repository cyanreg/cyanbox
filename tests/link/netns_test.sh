#!/bin/bash
# Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md.
#
# `cyanbox link serve` and `cyanbox link browse` on the private two-host
# network of netns.sh: rekordbox's real ports, real broadcasts, sockets
# tied to an interface; the server finding the Ethernet interface by
# itself; serving two at once on the same ports, one that gets its
# address only after the server started among them; a hot cue the player
# saves kept in the library; a player reading on with its handles after
# the server's address went and came back; and the fence (a browse that
# never announces itself gets nothing).
#
#   tests/link/netns_test.sh CYANBOX-CLI AUDIO.wav DIR
#
# Skips (77) where unprivileged user namespaces are not allowed.

set -u
cli=$1 audio=$2 dir=$3
here=$(dirname "$0")
rm -rf "$dir" && mkdir -p "$dir" || exit 1
"$cli" -c "$dir/lib" add "$audio" > "$dir/add.log" 2>&1 || { cat "$dir/add.log"; exit 1; }

out=$("$here/netns.sh" -w 1 "$cli -c $dir/lib link serve" -- \
      "timeout 60 $cli link browse -i \$CBB --hold 1 --save-cue A@1.5" 2>&1)
status=$?
[ $status -eq 77 ] && exit 77
echo "$out"
fail=0
check() {
    if ! grep -qE "$1" <<< "$out"; then
        echo "netns_test: missing: $1"
        fail=1
    fi
}
[ $status -eq 0 ] || { echo "netns_test: the browse failed ($status)"; fail=1; }
check "^source 169\.254\.1\.1, device 17"
check "^all tracks \(1004\): 1 row"
check "track info \(2102\): /Contents/.*, [0-9]+ bytes"
check "beat grid \(2204\): [0-9]+ bytes, [0-9]+ beats"
check "file over NFS: [0-9]+ bytes in [0-9]+ ms \([0-9.]+ MB/s\)$"
check "^link up as device 17 on cba"
check "playing track 1 from 17"
# The hot cue the player saved: in the answer, said by the server, and in
# the library after it.
check "^saved hot cue A at 0:01\.500 on track 1: the source lists [0-9]+ cues?$"
check "^player 3 saved hot cue A at 0:01\.500 on "
shown=$("$cli" -c "$dir/lib" show "$audio" 2>&1)
grep -qE "cue hot A +0:01\.500" <<< "$shown" || {
    echo "netns_test: the saved hot cue is not in the library:"; echo "$shown"; fail=1; }

# Two interfaces on host A: cbc served at once, cba only once it has its
# address back, three seconds after the server started; the player on B
# loads from it all the same, and no service is refused the ports.
out=$("$here/netns.sh" -w 1 "ip link add cbc type veth peer name rxd; \
      ip addr add 10.99.0.1/24 dev cbc; ip link set cbc up; ip link set rxd up; \
      ip addr flush dev \$CBA; \
      (sleep 3; ip addr add \$ADDR_A/16 broadcast 169.254.255.255 dev \$CBA) & \
      $cli -c $dir/lib link serve" -- \
      "timeout 60 $cli link browse -i \$CBB --hold 1" 2>&1)
status=$?
echo "$out"
[ $status -eq 0 ] || { echo "netns_test: the browse after the address came failed ($status)"; fail=1; }
check "^link up as device 17 on cbc"
check "^link up as device 17 on cba"
check "file over NFS: [0-9]+ bytes"
grep -q "cannot serve" <<< "$out" && { echo "netns_test: a service was refused"; fail=1; }

# The server's address taken away once the player has read the file, and
# put back three seconds later, as a cable or a USB adapter comes back:
# the service there stops and starts again, and the player reads the file
# again with the handles it had (a deck reads on, never mounting again).
rm -f "$dir/again.log"
out=$("$here/netns.sh" -w 1 "(while ! grep -q 'file over NFS:' $dir/again.log 2>/dev/null; do \
      sleep 0.2; done; ip addr del \$ADDR_A/16 dev \$CBA; sleep 3; \
      ip addr add \$ADDR_A/16 broadcast 169.254.255.255 dev \$CBA) & \
      $cli -c $dir/lib link serve" -- \
      "set -o pipefail; timeout 60 $cli link browse -i \$CBB --fetch-again 12 | \
      tee $dir/again.log" 2>&1)
status=$?
echo "$out"
[ $status -eq 0 ] || { echo "netns_test: reading on after the address came back failed ($status)"; fail=1; }
check "is gone or has another address: no longer serving there"
check "^file over NFS again: [0-9]+ bytes$"

# A browse that never announces itself is refused.
out=$("$here/netns.sh" -w 6 "$cli -c $dir/lib link serve -i \$CBA" -- \
      "timeout 60 $cli link browse -i \$CBB --quiet \$ADDR_A" 2>&1)
echo "$out"
grep -q "no database" <<< "$out" || { echo "netns_test: a quiet browse was answered"; fail=1; }
grep -q "file over NFS" <<< "$out" && { echo "netns_test: a quiet browse read a file"; fail=1; }

rm -rf "$dir"
exit $fail
