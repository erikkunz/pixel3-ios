#!/bin/bash
# Build the Pixel 3 input probe (buttons over SPMI, touchscreen survey).
#
#   ./build.sh [STOCK_BOOT.img]     -> out/input.img
#
# Use the LineageOS boot.img: the touchscreen node only exists in the
# board overlay ABL merges into that tree. Toolchain notes: ../common/build.sh.
set -euo pipefail
# Absolute path first: a relative one is meant from the caller's directory.
stock=${1:-}
[ -z "$stock" ] || stock=$(cd "$(dirname "$stock")" && pwd)/$(basename "$stock")
cd "$(dirname "$0")"
. ../common/build.sh
build_image input input.c "$stock"
