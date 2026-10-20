#!/bin/bash
# Wait for the phone in fastboot, then `fastboot boot IMAGE` (RAM only).
#
#   tools/fbboot.sh IMAGE [MINUTES]      (default: wait up to 30 minutes)
#
# Every fastboot call gets a time limit: after a big flash or a crashed
# boot, ABL can list the device and then hang on the next command, which
# would otherwise block forever. A hung attempt is killed and retried;
# holding Power + Volume Down restarts fastboot if it stays hung.
set -uo pipefail
img=$1
minutes=${2:-30}
[ -f "$img" ] || { echo "no such image: $img" >&2; exit 1; }

limit() {	# SECONDS CMD...: run CMD, kill it after SECONDS
	local secs=$1; shift
	perl -e 'alarm shift; exec @ARGV' "$secs" "$@"
}

deadline=$(( $(date +%s) + minutes * 60 ))
while [ "$(date +%s)" -lt "$deadline" ]; do
	if limit 5 fastboot devices 2>/dev/null | grep -q fastboot &&
	   limit 10 fastboot getvar product 2>&1 | grep -q "product: blueline"; then
		if limit 60 fastboot boot "$img"; then
			exit 0
		fi
		echo "fastboot boot failed or hung; retrying (Power + Volume Down if this repeats)" >&2
		sleep 5
	fi
	sleep 2
done
echo "no responsive fastboot within $minutes minutes" >&2
exit 1
