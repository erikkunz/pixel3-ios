#!/bin/bash
# Build the Pixel 3 XNU shim with a payload linked in, as a boot image for
# `fastboot boot`.
#
#   ./build.sh PAYLOAD.bin [STOCK_BOOT.img]     -> out/shim.img
#
# PAYLOAD.bin comes from tools/mkpayload.py (kernel + ADT + boot-args).
# With a stock boot.img (e.g. LineageOS's), its DTBs and load addresses
# are reused. Toolchain notes: ../common/build.sh.
set -euo pipefail
cd "$(dirname "$0")"
[ $# -ge 1 ] || { sed -n '2,9p' "$0"; exit 1; }
payload=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
. ../common/build.sh
CFLAGS="$CFLAGS -DPAYLOAD=\"$payload\""
build_image shim "shim.c payload.S" "${2:-}"
