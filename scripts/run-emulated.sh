#!/bin/sh
# LA64M68 — full emulated run, no hardware contact.
#
# Everything the engine offers without touching the iron: vAGA chipset
# fallback, vRTG display, vNIC, vHID. Keyboard/mouse go to LA64M68 once it
# has focus (Ctrl+Alt+Pause switches between the host and LA64M68).
#
# Safe for unattended runs: no --pis-arm, therefore no bus traffic at all.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

exec "$root/la64m68" \
    --ram-kb 2048 \
    --max-steps 200000 \
    --chipset \
    --vrtg --vrtg-backend auto \
    --vnic --vnic-mode off \
    --vhid \
    "$@"
