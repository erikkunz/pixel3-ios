#!/bin/bash
# Build the Pixel 3 input probe (buttons over SPMI, touchscreen survey).
#
#   ./build.sh [STOCK_BOOT.img]     -> out/input.img
#
# Use the LineageOS boot.img: the touchscreen node only exists in the
# board overlay ABL merges into that tree. Toolchain notes: ../common/build.sh.
set -euo pipefail
cd "$(dirname "$0")"
. ../common/build.sh
build_image input input.c "${1:-}"
