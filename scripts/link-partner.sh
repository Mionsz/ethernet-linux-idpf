#!/bin/sh
set -eu
IFACE=${IDPF_IFACE:-idpf0}
PEER_IFACE=${PEER_IFACE:-ice0}
PEER_MODULE=${PEER_MODULE:-if_ice}
TESTIP=${TESTIP:-192.168.211.1/24}
TESTPEER=${TESTPEER:-192.168.211.2}
JAIL=${IDPF_PEER_JAIL:-idpf_peer_$IFACE}
case "$IFACE:$PEER_IFACE:$JAIL" in
    *[!a-zA-Z0-9_.:-]*|'') echo 'Invalid interface or jail name' >&2; exit 1 ;;
esac
STATE="/var/run/idpf-link-$IFACE"
SELF=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)/link-partner.sh
die() { echo "[FAIL] link partner: $*" >&2; exit 1; }
usage() {
    echo 'Usage: link-partner.sh up|down|status|confirm|exec COMMAND...|with COMMAND...'
    echo 'Environment: IDPF_IFACE, PEER_IFACE, PEER_MODULE, TESTIP, TESTPEER, IDPF_PEER_JAIL'
}
ACTION=${1:---help}
shift "$(( $# > 0 ? 1 : 0 ))"
case "$ACTION" in --help|-h) usage; exit 0 ;; esac
test "$(uname -s)" = FreeBSD || die 'FreeBSD required'
test "$(id -u)" = 0 || die 'root required'

load_state() {
    test -f "$STATE/config" || die "no owned session for $IFACE"
    { read -r PEER_IFACE; read -r JAIL; read -r TESTIP; read -r TESTPEER;
      read -r OLD_MTU; read -r OLD_UP; read -r HOST_UP; } < "$STATE/config"
}
peer() { jexec "$JAIL" "$@"; }
address_present() {
    ifconfig "$IFACE" inet | awk -v ip="${TESTIP%/*}" \
        '$1 == "inet" && $2 == ip { found=1 } END { exit !found }'
}
configure_host() {
    if ! address_present; then
        touch "$STATE/host-address"
        ifconfig "$IFACE" inet "$TESTIP" alias
    fi
    ifconfig "$IFACE" up
}
down() {
    test -d "$STATE" || return 0
    load_state
    if test -f "$STATE/jail-created"; then
        if jls -j "$JAIL" >/dev/null 2>&1; then
            jail -r "$JAIL" || return 1
        fi
        ifconfig "$PEER_IFACE" >/dev/null || return 1
        ifconfig "$PEER_IFACE" mtu "$OLD_MTU" || return 1
        ifconfig "$PEER_IFACE" "$OLD_UP" || return 1
    fi
    if ifconfig "$IFACE" >/dev/null 2>&1; then
        if test -f "$STATE/host-address" && address_present; then
            ifconfig "$IFACE" inet "${TESTIP%/*}" -alias || return 1
        fi
        ifconfig "$IFACE" "$HOST_UP" || return 1
    fi
    rm -f "$STATE/config" "$STATE/host-address" "$STATE/jail-created"
    rmdir "$STATE"
    echo "[PASS] link partner returned $PEER_IFACE to host"
}
up() {
    case "${IDPF_ALLOW_HARDWARE:-0}" in 1|yes|true) ;; *) die 'set IDPF_ALLOW_HARDWARE=1' ;; esac
    test "$IFACE" != "$PEER_IFACE" || die 'driver and peer must differ'
    printf '%s\n%s\n' "$TESTIP" "$TESTPEER/24" | awk -F '[/ .]' '
        NF != 5 { exit 1 }
        { for (field=1; field<=4; field++)
              if ($field !~ /^[0-9]+$/ || $field > 255) exit 1
          if ($5 != 24) exit 1 }
    ' || die 'expected IPv4 addresses with /24 prefix'
    test "${TESTIP%/*}" != "$TESTPEER" || die 'addresses must differ'
    test "${TESTIP%.*}" = "${TESTPEER%.*}" || die 'addresses must share the /24 subnet'
    test "$(sysctl -n kern.features.vimage)" = 1 || die 'VNET unavailable'
    ifconfig "$IFACE" >/dev/null || die "missing driver interface $IFACE"
    if ! ifconfig "$PEER_IFACE" >/dev/null 2>&1; then
        test "$PEER_MODULE" = if_ice || die "missing peer interface $PEER_IFACE"
        kldload if_ice
    fi
    ifconfig "$PEER_IFACE" >/dev/null || die "missing peer interface $PEER_IFACE"
    if ifconfig "$PEER_IFACE" | awk '
        $1 == "inet" || ($1 == "inet6" && $2 !~ /^fe80:/) { found=1 }
        END { exit !found }'; then
        die "$PEER_IFACE already has addresses; refusing takeover"
    fi
    if jls -j "$JAIL" >/dev/null 2>&1; then
        die "jail $JAIL already exists"
    fi
    OLD_MTU=$(ifconfig "$PEER_IFACE" | awk 'NR==1 { for (field=1; field<NF; field++) if ($field=="mtu") print $(field+1) }')
    OLD_UP=down; HOST_UP=down
    if ifconfig "$PEER_IFACE" | grep -q '<UP,'; then OLD_UP=up; fi
    if ifconfig "$IFACE" | grep -q '<UP,'; then HOST_UP=up; fi
    umask 077
    mkdir "$STATE" || die "session already owned: $STATE"
    printf '%s\n' "$PEER_IFACE" "$JAIL" "$TESTIP" "$TESTPEER" "$OLD_MTU" "$OLD_UP" "$HOST_UP" > "$STATE/config"
    trap 'rc=$?; if test "$rc" -ne 0; then down || echo "[FAIL] cleanup required: sh $SELF down" >&2; fi' EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM HUP
    configure_host
    jail -c name="$JAIL" path=/ vnet persist allow.raw_sockets=1
    touch "$STATE/jail-created"
    ifconfig "$PEER_IFACE" vnet "$JAIL"
    peer ifconfig lo0 up
    peer ifconfig "$PEER_IFACE" inet "$TESTPEER/24" up
    echo "[PASS] peer $PEER_IFACE at $TESTPEER in VNET $JAIL"
    trap - EXIT INT TERM HUP
}
counters() {
    "$@" | awk '
        NR == 1 { for (field=1; field<=NF; field++) column[$field]=field }
        $3 ~ /^<Link#/ {
            if (!column["Ipkts"] || !column["Opkts"]) exit 1
            print $(column["Ipkts"]), $(column["Opkts"]); found=1; exit
        }
        END { if (!found) exit 1 }
    '
}
active() {
    ifconfig "$IFACE" | grep -q 'status: active' &&
        peer ifconfig "$PEER_IFACE" | grep -q 'status: active'
}
confirm() {
    load_state
    configure_host
    active || die 'both links must be active'
    route -n get "$TESTPEER" | awk -v iface="$IFACE" \
        '$1=="interface:" && $2==iface { ok=1 } END { exit !ok }' || die 'host route is not through the driver'
    peer route -n get "${TESTIP%/*}" | awk -v iface="$PEER_IFACE" \
        '$1=="interface:" && $2==iface { ok=1 } END { exit !ok }' || die 'peer route is not through the partner'
    set -- $(counters netstat -bI "$IFACE")
    test "$#" = 2 || die 'host counters unavailable'
    HOST_RX=$1; HOST_TX=$2
    set -- $(counters peer netstat -bI "$PEER_IFACE")
    test "$#" = 2 || die 'peer counters unavailable'
    PEER_RX=$1; PEER_TX=$2
    for direction in host peer; do
        if test "$direction" = host; then
            OUTPUT=$(ping -n -q -S "${TESTIP%/*}" -c 5 -i 0.2 -t 8 "$TESTPEER") || die 'host-to-peer ping failed'
        else
            OUTPUT=$(peer ping -n -q -S "$TESTPEER" -c 5 -i 0.2 -t 8 "${TESTIP%/*}") || die 'peer-to-host ping failed'
        fi
        printf '%s\n' "$OUTPUT"
        printf '%s\n' "$OUTPUT" | grep -Eq '^5 packets transmitted, 5 packets received,' || die "$direction traffic lost packets"
    done
    set -- $(counters netstat -bI "$IFACE")
    test "$#" = 2 || die 'host counters unavailable'
    test "$(( $1 - HOST_RX ))" -ge 10 && test "$(( $2 - HOST_TX ))" -ge 10 || die 'host RX/TX evidence insufficient'
    set -- $(counters peer netstat -bI "$PEER_IFACE")
    test "$#" = 2 || die 'peer counters unavailable'
    test "$(( $1 - PEER_RX ))" -ge 10 && test "$(( $2 - PEER_TX ))" -ge 10 || die 'peer RX/TX evidence insufficient'
    active || die 'link dropped during confirmation'
    echo '[PASS] bidirectional peer traffic, routes and RX/TX counters confirmed'
}
case "$ACTION" in
    up) up ;;
    down) down ;;
    status) load_state; ifconfig "$IFACE"; peer ifconfig "$PEER_IFACE" ;;
    confirm) confirm ;;
    exec) load_state; test "$#" -gt 0 || die 'missing peer command'; peer "$@" ;;
    with)
        test "$#" -gt 0 || die 'missing supervised command'
        up
        CHILD=
        finish() {
            RESULT=$?
            trap - EXIT INT TERM HUP
            if test -n "$CHILD"; then kill "$CHILD" 2>/dev/null || true; wait "$CHILD" 2>/dev/null || true; fi
            down || RESULT=1
            exit "$RESULT"
        }
        trap finish EXIT
        trap 'exit 130' INT
        trap 'exit 143' TERM HUP
        "$@" & CHILD=$!
        RESULT=0
        wait "$CHILD" || RESULT=$?
        CHILD=
        exit "$RESULT"
        ;;
    *) usage >&2; exit 1 ;;
esac