#!/bin/bash
# dxb runs patches/*.sh from the XNU source root before applying *.patch.
# Creates bsd/dev/sweet_usb.c (DWC3 device mode, CDC-ECM, live console log),
# bsd/dev/sweet_poke.c (register scripts) and bsd/dev/sweet_files.c (TCP file
# server): ios-sweet's patches 0072-0077 and 0081 at a pinned commit, then
# ios-flame's (Pixel 4, MIT) 0088, 0089 and 0091 on top, taking only these
# files. Flame's driver routes the DWC3's DMA through its own apps-SMMU
# context bank with translation off, as ABL does, and forces high speed.
# 0108-blueline-usb-hooks.patch adds the build entry, the start call and the
# log helpers; 0122-blueline-usb-flame.patch the SDM845 differences (GCC
# clocks, QUSB2 PHY used as ABL left it, SMMU stream 0x740).
set -euo pipefail
sweet_sha=4c7cc91ea672b3ec539ad1d45f5c96784aa43f25
flame_sha=745c4341af86089efb0df851fa8a27fb8face624
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
for repo in sweet flame; do
	git init -q "$tmp/$repo"
done
git -C "$tmp/sweet" fetch -q --depth 1 https://github.com/dipa-studio/ios-sweet "$sweet_sha"
git -C "$tmp/sweet" checkout -q FETCH_HEAD
git -C "$tmp/flame" fetch -q --depth 1 https://github.com/NWylynko/ios-flame "$flame_sha"
git -C "$tmp/flame" checkout -q FETCH_HEAD
for n in 0072 0073 0074 0075 0076 0077 0081; do
	git apply --include=bsd/dev/sweet_usb.c "$tmp"/sweet/xnu/patches/$n-*.patch
done
for n in 0088 0089 0091; do
	git apply --include=bsd/dev/sweet_usb.c --include=bsd/dev/sweet_poke.c \
	    --include=bsd/dev/sweet_files.c "$tmp"/flame/xnu/patches/$n-*.patch
done
for f in sweet_usb.c sweet_poke.c sweet_files.c; do
	test -s bsd/dev/$f
	echo "$f staged: $(wc -l < bsd/dev/$f) lines"
done
