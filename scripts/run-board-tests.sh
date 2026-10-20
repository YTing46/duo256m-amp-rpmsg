#!/bin/bash
# run-board-tests.sh - test the mainline AMP stack on a Duo256M booted from
# scripts/build-image.sh's image. Talks to the board over ADB (DUO_USB=adb
# images) or SSH over USB NCM (DUO_USB=ncm images), whichever answers.
#
# Environment:
#   DUO_TRANSPORT    adb or ssh (default: adb if a device is attached, else ssh)
#   DUO_HOST         board address for ssh (default: 192.168.42.1)
#   DUO_SSH_KEY      private key authorized in the image
#                    (default: $DUO_BUILD_DIR/keys/duo_ed25519)
#   DUO_BUILD_DIR    as for build-image.sh (default: <repo>/build)
#   DUO_RESULTS_DIR  where result folders go (default: $DUO_BUILD_DIR/results)
#
# Exit status 0 only if every check passes.
set -uo pipefail

REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
B=${DUO_BUILD_DIR:-$REPO/build}
HOST=${DUO_HOST:-192.168.42.1}
KEY=${DUO_SSH_KEY:-$B/keys/duo_ed25519}
RES=${DUO_RESULTS_DIR:-$B/results}/$(date +%Y%m%d-%H%M%S)
SSH=(ssh -i "$KEY" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
     -o LogLevel=ERROR -o ConnectTimeout=5 "root@$HOST")

T=${DUO_TRANSPORT:-}
if [ -z "$T" ]; then
	if command -v adb >/dev/null 2>&1 && [ "$(timeout 5 adb get-state 2>/dev/null)" = device ]; then
		T=adb
	else
		T=ssh
	fi
fi

# run <command>: board shell command; stdout without the CRs adb's pty adds
# (this adbd predates exit-status forwarding, so callers check output/files)
# fetch <board file> <local file>
case $T in
adb)
	command -v adb >/dev/null 2>&1 || { echo "no adb on this host" >&2; exit 1; }
	run()   { adb shell "$*" | tr -d '\r'; }
	fetch() { adb pull "$1" "$2" >/dev/null 2>&1; }
	up()    { [ "$(timeout 5 adb get-state 2>/dev/null)" = device ]; }
	;;
ssh)
	[ -f "$KEY" ] || { echo "no SSH key at $KEY (set DUO_SSH_KEY)" >&2; exit 1; }
	run()   { "${SSH[@]}" "$*"; }
	fetch() { "${SSH[@]}" "cat $1" > "$2"; }
	up()    { "${SSH[@]}" true 2>/dev/null; }
	;;
*)
	echo "DUO_TRANSPORT must be adb or ssh" >&2; exit 1 ;;
esac

mkdir -p "$RES"
pass=0; fail=0
ok()  { echo "PASS  $*" | tee -a "$RES/summary.txt"; pass=$((pass + 1)); }
bad() { echo "FAIL  $*" | tee -a "$RES/summary.txt"; fail=$((fail + 1)); }

echo "transport: $T" | tee "$RES/summary.txt"
echo "waiting for the board ..."
for _ in $(seq 1 60); do up && break; sleep 2; done
up || { echo "board not reachable over $T" >&2; exit 1; }

# 1. system state
run 'uname -a; cat /proc/cmdline; echo; cat /sys/class/remoteproc/remoteproc0/state;
     cat /sys/class/remoteproc/remoteproc0/firmware; ls -l /dev/rpmsg* 2>&1;
     cat /mnt/diag-boot/trace.txt 2>&1; readlink /mnt/system/usb.sh' > "$RES/system.txt" 2>&1
run 'dmesg' > "$RES/dmesg.txt" 2>&1
grep -q "^running" "$RES/system.txt" && ok "remoteproc0 running" || bad "remoteproc0 not running"
grep -q "rpmsg_ctrl0" "$RES/system.txt" && ok "/dev/rpmsg_ctrl0 present" || bad "/dev/rpmsg_ctrl0 missing"
if grep -iE "Oops|BUG:|Unable to handle|scheduling while atomic|WARNING:" "$RES/dmesg.txt" \
	> "$RES/dmesg-problems.txt"; then
	bad "kernel warnings in dmesg (dmesg-problems.txt)"
else
	ok "no oops/BUG/WARNING in dmesg"
fi

# 2. pingpong
pingpong() {	# <output file> <label>
	local n
	run /root/bench/mailbox_pingpong_test > "$1" 2>&1
	n=$(grep -c "ThreadX_data.DATA" "$1")
	[ "$n" -eq 16 ] && ok "$2 16/16" || bad "$2 $n/16"
}
pingpong "$RES/pingpong.txt" "pingpong"

# 3. RTT (plain), decomposition, payload sweep. Output stays on the board
# until the run ends: streaming it to the host meanwhile puts USB traffic on
# the only Linux core and inflates the tail.
bench() {	# <name> <env+cmd>
	run "$2 > /tmp/$1.csv 2> /tmp/$1.summary; echo \$? > /tmp/$1.rc" >/dev/null
	fetch "/tmp/$1.csv" "$RES/$1.csv"
	fetch "/tmp/$1.summary" "$RES/$1.summary"
	[ "$(run "cat /tmp/$1.rc")" = 0 ]
}
rows() { echo $(($(wc -l < "$RES/$1.csv") - 1)); }
bench rtt_bench 'RPMSG_RTT_ITERS=10000 /root/bench/rpmsg_rtt_bench' \
	&& ok "rpmsg_rtt_bench: $(grep p50 "$RES/rtt_bench.summary")" || bad "rpmsg_rtt_bench (rtt_bench.summary)"
bench rtt_decomp 'RPMSG_RTT_ITERS=2000 /root/bench/rpmsg_rtt_decomp' \
	&& ok "rpmsg_rtt_decomp ($(rows rtt_decomp) rows)" || bad "rpmsg_rtt_decomp (rtt_decomp.summary)"
bench rtt_payload 'RPMSG_RTT_ITERS=500 /root/bench/rpmsg_rtt_payload' \
	&& ok "rpmsg_rtt_payload ($(rows rtt_payload) rows)" || bad "rpmsg_rtt_payload (rtt_payload.summary)"

# 4. warm firmware restart, then pingpong again
run 'R=/sys/class/remoteproc/remoteproc0; echo stop > $R/state; sleep 1; echo start > $R/state; sleep 2; cat $R/state' \
	> "$RES/restart.txt" 2>&1
pingpong "$RES/pingpong-after-restart.txt" "pingpong after rproc stop/start"

# 5. p50/p99 per segment and per payload size
python3 -I - "$RES/rtt_decomp.csv" "$RES/rtt_payload.csv" > "$RES/report.txt" 2>&1 <<'EOF'
import csv, sys
from collections import defaultdict

def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(len(v) * p))] / 1e3

rows = [r for r in csv.DictReader(open(sys.argv[1])) if r.get("status", "ok") == "ok"]
print("decomposition (us)")
for k in ["rtt_ns", "syscall_in", "notify", "transport", "turnaround", "workwait", "wake"]:
    v = [float(r[k]) for r in rows if r.get(k) not in (None, "")]
    if v:
        print(f"  {k:12} p50={pct(v, .5):7.1f}  p99={pct(v, .99):7.1f}  n={len(v)}")

size = defaultdict(list)
for r in csv.DictReader(open(sys.argv[2])):
    if r.get("status", "ok") == "ok":
        size[int(r["payload_len"])].append(float(r["rtt_ns"]))
print("payload sweep, RTT p50 (us)")
for s in sorted(size):
    print(f"  {s:4}B {pct(size[s], .5):7.1f}  n={len(size[s])}")
EOF
cat "$RES/report.txt"

echo
echo "pass=$pass fail=$fail  results: $RES"
[ "$fail" -eq 0 ]
