#!/bin/sh
# rpi-Digital-TRX post-build: runs after Buildroot's own Pi 4 post-build script.
set -eu
TARGET_DIR="$1"

# Mount the boot partition at /boot, where the user puts settings.json and wpa_supplicant.conf.
mkdir -p "${TARGET_DIR}/boot"
grep -q '^/dev/mmcblk0p1' "${TARGET_DIR}/etc/fstab" || \
    echo '/dev/mmcblk0p1  /boot  vfat  defaults,noatime  0  0' >> "${TARGET_DIR}/etc/fstab"

# The app runs as root and talks to the system-wide PulseAudio. Let local programs use its
# socket without the pulse-access group: Buildroot only creates its users and groups later,
# when the filesystem image is made.
sed -i 's/^load-module module-native-protocol-unix$/load-module module-native-protocol-unix auth-anonymous=1/' \
    "${TARGET_DIR}/etc/pulse/system.pa"

# Safe shutdown (Menu held on the front panel): after the filesystems are unmounted or read-only,
# write "Safe to unplug" on the display. The Pi 4 keeps 5 V on the header after it halts, so the
# message stays up until the power is pulled.
grep -q -- '--lcd-message' "${TARGET_DIR}/etc/inittab" || \
    sed -i '\|^::shutdown:/bin/umount -a -r|a ::shutdown:/usr/bin/digital-transceiver --lcd-message "Digital TRX" "Safe to unplug"' \
        "${TARGET_DIR}/etc/inittab"

