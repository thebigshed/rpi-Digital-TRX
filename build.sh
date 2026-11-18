#!/bin/bash
# Build the rpi-Digital-TRX SD card image.
#
#   ./build.sh               configure (when needed) and build -> output/images/sdcard.img
#   ./build.sh menuconfig    any other Buildroot target, e.g. menuconfig, clean,
#                            digital-transceiver-rebuild
#
# The app is built from app/ in this project.
set -euo pipefail
cd "$(dirname "$0")"
TOP=$PWD
BR=$TOP/buildroot
OUT=$TOP/output
FRAGMENT=$TOP/external/configs/dtrx.fragment

if [ ! -d "$BR" ]; then
    echo "Fetching Buildroot 2026.02.3..."
    git clone --depth 1 -b 2026.02.3 https://gitlab.com/buildroot.org/buildroot.git "$BR"
fi

# Configure on the first build, and again whenever dtrx.fragment changes.
if [ ! -f "$OUT/.config" ] || [ "$FRAGMENT" -nt "$OUT/.config" ]; then
    reconfigure=$([ -f "$OUT/.config" ] && echo yes || echo no)
    mkdir -p "$OUT"
    # Buildroot's Pi 4 configuration plus ours
    cat "$BR/configs/raspberrypi4_64_defconfig" "$FRAGMENT" > "$OUT/dtrx_defconfig"
    make -C "$BR" O="$OUT" BR2_EXTERNAL="$TOP/external" BR2_DEFCONFIG="$OUT/dtrx_defconfig" defconfig
    if [ "$reconfigure" = yes ]; then
        echo
        echo "Configuration changed. Packages that were removed stay in the image until a clean"
        echo "build: ./build.sh clean all"
        echo
    fi
fi

# Older versions built the app from ../Digital-Transceiver through this file.
rm -f "$OUT/local.mk"

make -C "$OUT" "$@"

if [ $# -eq 0 ]; then
    echo
    echo "Image: $OUT/images/sdcard.img"
fi
