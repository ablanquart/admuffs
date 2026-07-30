#!/usr/bin/env bash
# record-ir-key.sh - capture one IR keypress from your real remote and print it
# as a JSON command entry ready to paste into data/ir_codes/<brand>.json.
#
# Usage:  ./scripts/record-ir-key.sh [KEY_NAME] [rx_device]
#   e.g.  ./scripts/record-ir-key.sh KEY_MUTE /dev/lirc1
#
# Point the remote at the ANAVI pHAT's IR receiver and press the button ONCE
# when prompted. Requires ir-ctl (apt install v4l-utils).
set -euo pipefail

KEY="${1:-KEY_MUTE}"
DEV="${2:-}"

if ! command -v ir-ctl >/dev/null 2>&1; then
  echo "ir-ctl not found: sudo apt install v4l-utils" >&2
  exit 1
fi

# Auto-pick the receive-capable device if none given.
if [[ -z "$DEV" ]]; then
  for d in /dev/lirc0 /dev/lirc1 /dev/lirc2 /dev/lirc3; do
    [[ -e "$d" ]] || continue
    if ir-ctl -d "$d" --features 2>/dev/null | grep -qi "receive"; then
      DEV="$d"; break
    fi
  done
  if [[ -z "$DEV" ]]; then
    echo "No receive-capable /dev/lirc* device found (run: admuffs --ir-check)" >&2
    exit 1
  fi
fi

echo "Recording from $DEV -- point the remote at the pHAT's receiver and" >&2
echo "press '$KEY' on the remote ONCE (10s timeout)..." >&2

RAW="$(timeout 10 ir-ctl -d "$DEV" -r --one-shot 2>/dev/null || true)"
if [[ -z "$RAW" ]]; then
  # older ir-ctl versions lack --one-shot; fall back to a timed capture
  RAW="$(timeout 4 ir-ctl -d "$DEV" -r 2>/dev/null || true)"
fi
if [[ -z "$RAW" ]]; then
  echo "Nothing received. Fresh batteries? Right device? (rx is the RECEIVE" >&2
  echo "one in 'admuffs --ir-check'; you may need sudo.)" >&2
  exit 1
fi

# ir-ctl -r prints lines like "pulse 8981" / "space 4462" / "timeout 23330".
# Keep the first burst: pulses/spaces up to the first timeout marker.
echo "$RAW" | awk -v key="$KEY" '
  $1 == "timeout" { exit }
  $1 == "pulse" || $1 == "space" { v[n++] = $2 }
  END {
    if (n == 0) { print "no pulse data parsed" > "/dev/stderr"; exit 1 }
    # trailing space carries no information; drop it so the list ends on a pulse
    if (n % 2 == 0) n--
    printf "\"%s\": {\"carrier_hz\": 38000, \"pulses\": [", key
    for (i = 0; i < n; i++) printf "%s%d", (i ? "," : ""), v[i]
    print "]},"
    printf "captured %d pulse/space durations\n", n > "/dev/stderr"
  }'
