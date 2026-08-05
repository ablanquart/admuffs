#!/usr/bin/env bash
# setup-pi.sh - one-time Raspberry Pi setup for the ANAVI Infrared pHAT + admuffs.
# Installs build/runtime deps and enables the IR device-tree overlays.
set -euo pipefail

echo "== admuffs Pi setup =="

if [[ $EUID -ne 0 ]]; then
  echo "Re-run with sudo (needs to edit config.txt): sudo $0" >&2
  exit 1
fi

echo "-- installing packages --"
apt-get update
apt-get install -y build-essential cmake pkg-config \
    libcurl4-openssl-dev libncurses-dev libasound2-dev libssl-dev \
    lirc v4l-utils

# Locate the boot config (Bookworm+ uses /boot/firmware).
CFG=/boot/firmware/config.txt
[[ -f "$CFG" ]] || CFG=/boot/config.txt
echo "-- configuring IR overlays in $CFG --"

# config.txt is section-filtered: lines under a model header like [cm5] only
# apply to that model, and the stock file ENDS with such sections. Blindly
# appending would drop our lines into whatever section is last, where most
# boards never read them. So: emit an explicit [all] header, then the lines.
NEED_IR_RX=0; NEED_IR_TX=0
grep -qE "^\s*dtoverlay=gpio-ir(,|\s*$)" "$CFG" || NEED_IR_RX=1
grep -qE "^\s*dtoverlay=gpio-ir-tx" "$CFG"      || NEED_IR_TX=1
if [[ $NEED_IR_RX -eq 1 || $NEED_IR_TX -eq 1 ]]; then
  {
    echo ""
    echo "[all]"
    echo "# admuffs: ANAVI Infrared pHAT (IR is plain GPIO, not I2C)"
    [[ $NEED_IR_RX -eq 1 ]] && echo "dtoverlay=gpio-ir,gpio_pin=18"
    [[ $NEED_IR_TX -eq 1 ]] && echo "dtoverlay=gpio-ir-tx,gpio_pin=17"
  } >> "$CFG"
  echo "   added IR overlays under an [all] section"
else
  echo "   IR overlay lines already present (verify they are NOT under a"
  echo "   model-specific section like [cm5] -- run: admuffs --ir-check after boot)"
fi

echo
echo "Done. Reboot for the IR overlays to take effect:  sudo reboot"
echo "After reboot, verify:  ls -l /dev/lirc*"
echo "Then build:  cmake -S . -B build && cmake --build build -j\$(nproc)"
echo "And configure:  ./build/admuffs --setup"
