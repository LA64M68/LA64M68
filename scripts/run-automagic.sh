#!/bin/sh
# LA64M68 — what happens automatically.
#
# Run this from anywhere. It is the "automagic" example: it shows what LA64M68
# resolves on its own, without configuration and without hard-coded paths.
#
# Automatic behaviour covered here:
#
#   program root   LA64M68 uses only its own directory. It finds that
#                  directory from the executable itself and starts there, so
#                  where you launch it from does not matter. No /etc, no
#                  /usr/local, no /usr, no HOME, no XDG.
#
#   no config file Everything is a command line parameter. No ini, no
#                  lookup paths. Defaults are built in, so no arguments at
#                  all is a valid run.
#
#   PiS variant    The PiStorm boards share one bus protocol, so there is one
#                  code path. The variant is auto-resolved and reported; pass
#                  --pis-variant to state what is on the bench.
#
#   hardware gate  --pis-arm defaults to OFF. An unattended run cannot drive
#                  the Amiga bus, and the emulated chipset takes over instead.
#
#   focus switch   Ctrl+Alt+Pause hands keyboard/mouse and the shown frame
#                  between the host and LA64M68.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

echo "la64m68-automagic: script located the solution at $root"
echo "la64m68-automagic: starting from $(pwd) on purpose -- the program"
echo "                    resolves its own root and goes there itself"
echo

# LA64M68_MAX_DEBUG=1 makes the engine report what it resolved. Without it
# the run stays quiet, which is the default.
LA64M68_MAX_DEBUG=1 exec "$root/la64m68" \
    --ram-kb 2048 \
    --max-steps 4096 \
    --chipset \
    "$@"
