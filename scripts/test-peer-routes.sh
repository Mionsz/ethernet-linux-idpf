#!/bin/sh
set -eu
test "$(uname -s)" = FreeBSD || exit 2
test "$(id -u)" = 0 || exit 1
PAIR=$(ifconfig epair create)
JAIL=idpf_route_test_$$
cleanup() {
    jail -r "$JAIL" || true
    ifconfig "$PAIR" destroy
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP
jail -c name="$JAIL" path=/ vnet persist allow.raw_sockets=1
ifconfig "$PAIR" vnet "$JAIL"
ifconfig "${PAIR%a}b" vnet "$JAIL"
jexec "$JAIL" ifconfig lo0 up
jexec "$JAIL" ifconfig "$PAIR" inet 192.168.211.1/24 up
jexec "$JAIL" ifconfig "${PAIR%a}b" inet 192.168.211.2/24 up
echo '=== Before: automatic local host route ==='
jexec "$JAIL" route -n get 192.168.211.1
jexec "$JAIL" route delete -host 192.168.211.1
jexec "$JAIL" route add -host 192.168.211.1 -iface "${PAIR%a}b"
echo '=== After: explicit egress route ==='
jexec "$JAIL" route -n get 192.168.211.1
echo '=== Counters before one-sided route test ==='
jexec "$JAIL" netstat -bI "$PAIR"
jexec "$JAIL" netstat -bI "${PAIR%a}b"
jexec "$JAIL" netstat -bI lo0
RESULT=0
jexec "$JAIL" ping -n -S 192.168.211.2 -c 5 -i 0.2 -t 8 192.168.211.1 || RESULT=$?
echo "one-sided route ping exit: $RESULT"
echo '=== Counters after one-sided route test ==='
jexec "$JAIL" netstat -bI "$PAIR"
jexec "$JAIL" netstat -bI "${PAIR%a}b"
jexec "$JAIL" netstat -bI lo0