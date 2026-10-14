#!/bin/bash
# Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md.
#
# `rxbox link serve` and `rxbox link browse` on the private two-host
# network of netns.sh: rekordbox's real ports, real broadcasts, sockets
# tied to an interface; the server finding the Ethernet interface by
# itself; serving two at once on the same ports, one that gets its
# address only after the server started among them; and the fence (a
# browse that never announces itself gets nothing).
#
#   tests/link/netns_test.sh RXBOX-CLI AUDIO.wav DIR
#
# Skips (77) where unprivileged user namespaces are not allowed.

set -u
cli=$1 audio=$2 dir=$3
here=$(dirname "$0")
rm -rf "$dir" && mkdir -p "$dir" || exit 1
"$cli" -c "$dir/lib" add "$audio" > "$dir/add.log" 2>&1 || { cat "$dir/add.log"; exit 1; }

out=$("$here/netns.sh" -w 1 "$cli -c $dir/lib link serve" -- \
      "timeout 60 $cli link browse -i \$RXB --hold 1" 2>&1)
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
check "^link up as device 17 on rxa"
check "playing track 1 from 17"

# Two interfaces on host A: rxc served at once, rxa only once it has its
# address back, three seconds after the server started; the player on B
# loads from it all the same, and no service is refused the ports.
out=$("$here/netns.sh" -w 1 "ip link add rxc type veth peer name rxd; \
      ip addr add 10.99.0.1/24 dev rxc; ip link set rxc up; ip link set rxd up; \
      ip addr flush dev \$RXA; \
      (sleep 3; ip addr add \$ADDR_A/16 broadcast 169.254.255.255 dev \$RXA) & \
      $cli -c $dir/lib link serve" -- \
      "timeout 60 $cli link browse -i \$RXB --hold 1" 2>&1)
status=$?
echo "$out"
[ $status -eq 0 ] || { echo "netns_test: the browse after the address came failed ($status)"; fail=1; }
check "^link up as device 17 on rxc"
check "^link up as device 17 on rxa"
check "file over NFS: [0-9]+ bytes"
grep -q "cannot serve" <<< "$out" && { echo "netns_test: a service was refused"; fail=1; }

# A browse that never announces itself is refused.
out=$("$here/netns.sh" -w 6 "$cli -c $dir/lib link serve -i \$RXA" -- \
      "timeout 60 $cli link browse -i \$RXB --quiet \$ADDR_A" 2>&1)
echo "$out"
grep -q "no database" <<< "$out" || { echo "netns_test: a quiet browse was answered"; fail=1; }
grep -q "file over NFS" <<< "$out" && { echo "netns_test: a quiet browse read a file"; fail=1; }

rm -rf "$dir"
exit $fail
