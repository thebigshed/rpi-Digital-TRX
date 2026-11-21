#!/bin/bash
# Build the rpi-Digital-TRX SD card image.
#
#   ./build.sh                     Raspberry Pi 4 -> output/images/sdcard.img
#   BOARD=zero2w ./build.sh        Raspberry Pi Zero 2 W -> output-zero2w/images/sdcard.img
#   ./build.sh menuconfig          any other Buildroot target, e.g. menuconfig, clean,
#                                  digital-transceiver-rebuild (with BOARD= as above)
#
# The app is built from app/ in this project. Each board has its own output folder; the
# downloaded sources are shared (buildroot/dl).
set -euo pipefail
cd "$(dirname "$0")"
TOP=$PWD
BR=$TOP/buildroot
BOARD=${BOARD:-rpi4}

case "$BOARD" in
    rpi4)   DEFCONFIG=raspberrypi4_64_defconfig;     OUT=$TOP/output ;;
    zero2w) DEFCONFIG=raspberrypizero2w_64_defconfig; OUT=$TOP/output-zero2w ;;
    *) echo "Unknown BOARD '$BOARD': use rpi4 (the default) or zero2w" >&2; exit 1 ;;
esac
COMMON=$TOP/external/configs/dtrx.fragment
BOARD_FRAGMENT=$TOP/external/configs/$BOARD.fragment

if [ ! -d "$BR" ]; then
    echo "Fetching Buildroot 2026.02.3..."
    git clone --depth 1 -b 2026.02.3 https://gitlab.com/buildroot.org/buildroot.git "$BR"
fi

# Configure on the first build, and again whenever a fragment changes.
if [ ! -f "$OUT/.config" ] || [ "$COMMON" -nt "$OUT/.config" ] || [ "$BOARD_FRAGMENT" -nt "$OUT/.config" ]; then
    reconfigure=$([ -f "$OUT/.config" ] && echo yes || echo no)
    mkdir -p "$OUT"
    # Buildroot's board configuration, then ours (all boards), then the board's own
    cat "$BR/configs/$DEFCONFIG" "$COMMON" "$BOARD_FRAGMENT" > "$OUT/dtrx_defconfig"
    make -C "$BR" O="$OUT" BR2_EXTERNAL="$TOP/external" BR2_DEFCONFIG="$OUT/dtrx_defconfig" defconfig
    if [ "$reconfigure" = yes ]; then
        echo
        echo "Configuration changed. Packages that were removed stay in the image until a clean"
        echo "build: BOARD=$BOARD ./build.sh clean all"
        echo
    fi
fi

# Older versions built the app from ../Digital-Transceiver through this file.
rm -f "$OUT/local.mk"

make -C "$OUT" "$@"

if [ $# -eq 0 ]; then
    echo
    echo "Image ($BOARD): $OUT/images/sdcard.img"
fi
