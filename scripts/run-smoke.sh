#!/bin/sh
# LA64M68 — smoke run: no hardware, no ROM, four instructions.
#
# This is the "does it start" check and the default for automated work: the
# PiS hardware gate stays off (--pis-arm is not given), so no pin on the Amiga
# bus is driven.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
exec "$root/la64m68" "$@"
