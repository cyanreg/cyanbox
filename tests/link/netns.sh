#!/bin/bash
# Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md.
#
# A private two-host network for PRO DJ LINK: runs SERVER on host A and,
# once it is up, CLIENT on host B, each in a network namespace of its own,
# joined by a veth pair (A: interface rxa, 169.254.1.1; B: rxb,
# 169.254.1.2; broadcast 169.254.255.255). Real ports, real broadcasts,
# and nothing of it reaches any other network: the namespaces have no
# other interface. Needs unprivileged user namespaces, ip(8), nsenter(1);
# no root.
#
#   tests/link/netns.sh [-w SECONDS] SERVER-COMMAND -- CLIENT-COMMAND
#
# The commands run through bash -c; $RXA and $RXB name the interfaces and
# $ADDR_A and $ADDR_B the addresses. The server's output is printed after
# the client exits; the script exits with the client's status. -w is how
# long the server gets to start (default 1 second). Used by the link
# suite and by hand to run rxbox against rbxport's client and server.
#
# Everything runs in a PID namespace of its own too, so that whatever the
# commands start ends with the script: a server command of several
# commands is a shell whose children a kill of it would leave running.

set -u
wait_s=1
if [ "${1:-}" = -w ]; then
    wait_s=$2
    shift 2
fi
server=""
while [ $# -gt 0 ] && [ "$1" != "--" ]; do
    server="$server $1"
    shift
done
[ "${1:-}" = "--" ] && shift
client="$*"
if [ -z "$server" ] || [ -z "$client" ]; then
    echo "usage: $0 [-w SECONDS] SERVER-COMMAND -- CLIENT-COMMAND" >&2
    exit 1
fi
if ! unshare -rnmp --fork --mount-proc true 2>/dev/null; then
    echo "netns: no unprivileged user namespaces here" >&2
    exit 77
fi

log=$(mktemp "${TMPDIR:-/tmp}/rxbox-netns.XXXXXX")
trap 'rm -f "$log"' EXIT

export RXA=rxa RXB=rxb ADDR_A=169.254.1.1 ADDR_B=169.254.1.2 SERVER="$server" CLIENT="$client" \
       WAIT_S="$wait_s" LOG="$log"
unshare -rnmp --fork --mount-proc --kill-child bash -c '
    ip link set lo up
    # Host B: a namespace of its own, held by a process until the end.
    unshare -n sleep 100000 &
    holder=$!
    while [ "$(readlink /proc/$holder/ns/net)" = "$(readlink /proc/self/ns/net)" ]; do
        sleep 0.01
    done
    ip link add $RXA type veth peer name $RXB netns $holder
    ip addr add $ADDR_A/16 broadcast 169.254.255.255 dev $RXA
    ip link set $RXA up
    nsenter -t $holder -n bash -c "ip link set lo up
        ip addr add $ADDR_B/16 broadcast 169.254.255.255 dev $RXB
        ip link set $RXB up"
    bash -c "$SERVER" > "$LOG" 2>&1 &
    server=$!
    sleep "$WAIT_S"
    nsenter -t $holder -n bash -c "$CLIENT"
    status=$?
    kill $server 2>/dev/null
    wait $server 2>/dev/null
    kill $holder 2>/dev/null
    echo "--- server (host A) ---"
    cat "$LOG"
    exit $status
'
