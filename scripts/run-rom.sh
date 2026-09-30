#!/bin/sh
# LA64M68 — load a ROM image and run it (emulated, no hardware contact).
#
#   ./scripts/run-rom.sh path/to/rom.bin
#   ./scripts/run-rom.sh path/to/rom.bin 0x800
#
# The ROM is loaded raw at the given address (default 0x800) and its first
# two longs are installed as the reset vectors.
set -eu

if [ $# -lt 1 ]; then
    echo "usage: $0 ROM [load-address]" >&2
    exit 2
fi

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
rom=$1
addr=${2:-0x800}

exec "$root/la64m68" \
    --rom "$rom" --rom-addr "$addr" --rom-vectors \
    --ram-kb 2048 \
    --max-steps 200000 \
    "$@"
