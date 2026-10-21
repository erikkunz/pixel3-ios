#!/bin/bash
# Live kernel console from the phone over USB (macOS), no recovery visit.
#
#   tools/usblog.sh [LOGFILE]          (default work/live-<date>.txt)
#
# Boot an iOS image with boot-arg sweetusb=2: the kernel then acts as a USB
# CDC-ECM network adapter (ios-sweet's sweet_usb.c) with host-side MAC
# 02:00:5e:77:ee:01, answers on 192.168.7.2 and streams its console as UDP
# to 192.168.7.1:5140 (the last 64 KiB first, then live). This waits for the
# adapter, gives it 192.168.7.1/24 (sudo) and prints and saves the stream.
# `ping 192.168.7.2` checks the link.
set -euo pipefail
cd "$(dirname "$0")/.."
mac=02:00:5e:77:ee:01
out=${1:-work/live-$(date +%Y%m%d-%H%M%S).txt}
mkdir -p "$(dirname "$out")"

echo "waiting for the phone's USB Ethernet ($mac)..." >&2
ifname=
until [ -n "$ifname" ]; do
	ifname=$(ifconfig | awk -v m="$mac" '/^[a-z]/ {i = $1} $1 == "ether" && $2 == m {sub(":", "", i); print i}')
	[ -n "$ifname" ] || sleep 1
done
sudo ifconfig "$ifname" inet 192.168.7.1 netmask 255.255.255.0 up
echo "$ifname up: 192.168.7.1 <-> phone 192.168.7.2; live log on udp/5140 -> $out" >&2

exec python3 -u - "$out" <<'EOF'
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('192.168.7.1', 5140))
with open(sys.argv[1], 'ab') as log:
    while True:
        data, _ = s.recvfrom(65536)
        log.write(data)
        log.flush()
        sys.stdout.write(data.decode('utf-8', 'replace').replace('\0', ''))
        sys.stdout.flush()
EOF
