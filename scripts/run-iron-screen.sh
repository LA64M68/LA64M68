#!/bin/sh
# LA64M68 — run against the real Amiga so the picture stays up long enough to
# photograph or inspect. Unlike a normal run this does NOT exit quickly.
#
# !! THIS DRIVES THE AMIGA BUS — --pis-arm is required and is not the default.
#
# The picture is driven by the guest: COLOR00 fills the screen when no
# bitplanes are configured (that is a real display state, not "no image").
#
# Usage:
#   ./scripts/run-iron-screen.sh            auto-detect the PiS variant
#   ./scripts/run-iron-screen.sh 32lite     state the variant on the bench
set -eu

variant=${1:-auto}
[ $# -gt 0 ] && shift          # drop the variant from "$@" so it is not
                               # passed on a second time
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

echo "la64m68: ARMed — PiS variant '$variant', focus on the guest." >&2
echo "la64m68: the picture should appear on the Amiga-CRT / RPi output." >&2

# 0 = run until SIGINT/SIGTERM: the guest keeps drawing, the screen holds.
exec "$root/la64m68" \
    --pis-arm --pis-variant "$variant" \
    --fpga-bitstream "$root/roms/bitstream.bin" \
    --kickstart "$root/roms/Kickstart39.rom" \
    --plugin "$root/plugins/libla64m68_amiga.so" \
    --display amiga-gfx --focus guest \
    --max-steps 0 \
    "$@"
