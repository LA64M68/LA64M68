#!/bin/sh
# LA64M68 — run against real Amiga hardware (PiStorm).
#
# !! THIS ONE DRIVES THE AMIGA BUS !!
#
# --pis-arm is the hardware gate and is deliberately not the default. Only
# pass it when the machine on the bench is the machine you mean to talk to,
# and only after the checklist below.
#
# Checklist before arming (see 0-POOL/notes/2026-09-30-hardware-schutz-*):
#   1. right machine on the bench, nothing to be damaged by a bus cycle
#   2. --pis-variant matches what is actually plugged in
#   3. max-N = 3 attempts, one checksum = one attempt
#   4. last-known-good state saved before deploying
#   5. the Amiga is halted if you also pass --fpga-bitstream
#      (reconfiguring the FPGA pulls the bus owner away mid-cycle -- software
#      cannot check this, it is the operator's call)
#
# Usage:
#   ./scripts/run-iron.sh                 # auto-detect the PiS variant
#   ./scripts/run-iron.sh 32lite          # force a variant
#   ./scripts/run-iron.sh 32lite fw.svf   # and load an FPGA bitstream
set -eu

variant=${1:-auto}
bitstream=${2:-}

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

set -- --pis --pis-arm --pis-variant "$variant" --ram-kb 2048 --max-steps 200000

if [ -n "$bitstream" ]; then
    set -- "$@" --fpga-bitstream "$bitstream"
fi

echo "la64m68: ARming the PiS bus -- variant '$variant'" >&2
exec "$root/la64m68" "$@"
