#!/bin/bash
# Build the Pixel 3 probe and wrap it in a boot image for `fastboot boot`.
#
#   ./build.sh [STOCK_BOOT.img]     -> out/probe.img
#
# With a stock boot.img (e.g. LineageOS's), its DTBs and load addresses
# are reused. Without one, the mainline sdm845-google-blueline.dtb in
# ../dtb/ is used. Toolchain notes: ../common/build.sh.
set -euo pipefail
cd "$(dirname "$0")"
. ../common/build.sh
build_image probe "probe.c secondary.S" "${1:-}"
