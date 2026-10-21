#!/bin/bash
# dxb runs patches/*.sh from the XNU source root before applying *.patch.
# Creates bsd/dev/sweet_usb.c (DWC3 device mode, CDC-ECM, live console log)
# from ios-sweet's patches 0072-0077 and 0081 at a pinned commit, taking only
# that file; 0108-blueline-usb-hooks.patch adds the build entry, the start
# call and the log helpers. The Pixel 3 shares every address the driver
# uses with sweet (DWC3 0xa600000, qscratch 0xa6f8800, QUSB2 v2 PHY
# 0x88e2000, GCC usb30_prim_gdsc 0xf004).
set -euo pipefail
sweet_sha=4c7cc91ea672b3ec539ad1d45f5c96784aa43f25
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
git init -q "$tmp"
git -C "$tmp" fetch -q --depth 1 https://github.com/dipa-studio/ios-sweet "$sweet_sha"
git -C "$tmp" checkout -q FETCH_HEAD
for n in 0072 0073 0074 0075 0076 0077 0081; do
	git apply --include=bsd/dev/sweet_usb.c "$tmp"/xnu/patches/$n-*.patch
done
test -s bsd/dev/sweet_usb.c
echo "sweet_usb.c staged: $(wc -l < bsd/dev/sweet_usb.c) lines"
