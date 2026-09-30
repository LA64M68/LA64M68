# Non-Profit ARM64 Hybrid Services for M68k

For Amiga and Atari Computers

Non-Profit Open Source under **GNU General Public License v3.0 or later** —
see `LICENSE` and `NON-PROFIT`.

**Status:** in development.

---

## What this is

**LA64M68** (Linux-ARM64-M68k) is a hybrid M68k engine for Linux on ARM64.
It emulates the M68k core and its peripherals, and it can hand real Amiga
hardware (Zorro/PCI+) through to the machine via PiStorm on a Raspberry Pi 4B
or faster.

**Components:** vCPU · vMMU · vFPU · vRAM · vRTG · vNIC · vHID

The M68k core is based on the **Musashi** 68k emulator as used by the
**PiStorm** project. Provenance and licences of everything third-party are in
`THIRD-PARTY.md`.

---

## Self-contained by design

LA64M68 uses **its own directory and nothing else**. The directory holding the
`la64m68` executable is the program root, and the process makes it the working
directory on startup.

- No installation step, no `make install`
- Nothing in `/etc`, `/usr`, `/usr/local`, `$HOME` or XDG directories
- **No configuration file at all** — every setting is a command line parameter
- Relative paths are root-relative, no matter where it is launched from

Built in place or unpacked from a release archive, it behaves the same. Think
of it as a directory you can move around.

---

## Build

```sh
cmake -B build -S . -G Ninja
nice -n 10 cmake --build build -j2
cd build && ctest --output-on-failure
```

Requirements: a C11 compiler, CMake ≥ 3.20 and Ninja. Optional: libdrm
(VideoCore direct output), X11 (window output), pthreads.

The build writes `la64m68` and `plugins/*.so` into the program root, so the
built tree is already the runnable tree.

---

## Operate

Every setting is a parameter and every parameter has a built-in default, so
running with no arguments at all is valid.

```sh
./la64m68 --help                       # all options and their defaults
./scripts/run-smoke.sh                 # start check, no hardware contact
./scripts/run-emulated.sh              # full emulated device chain
./scripts/run-rom.sh path/to/rom.bin   # load and run a ROM image
./scripts/run-automagic.sh             # show what is resolved automatically
```

The example scripts are self-locating: they find the solution relative to
themselves, so they work from any working directory.

### Example

```sh
./la64m68 --rom roms/kick.rom --rom-addr 0x800 \
          --ram-kb 2048 --max-steps 200000 \
          --vrtg --vrtg-backend auto --vhid
```

### Debug output

```sh
LA64M68_MAX_DEBUG=1 ./la64m68 ...
```

Quiet by default. Diagnostic capability is always compiled in.

---

## Hardware safety

Talking to real Amiga hardware requires an explicit opt-in that **defaults to
off**, so an automated or unattended run physically cannot drive the bus.

PiStorm board variants share a single bus protocol and a single code path. The
variant is resolved automatically and reported; a parameter states what is
plugged in. Reconfiguring the PiStorm FPGA is the most dangerous operation
available and is separately gated, and must happen before any bus traffic.

### Input / output switch

`Ctrl+Alt+Pause` hands keyboard, mouse and the shown frame between the host
and LA64M68. The host keeps control by default, the chord is never passed on
to the guest, and switching releases any key still held there.

---

## Document index

| Document | Content |
|----------|---------|
| `LICENSE` | GNU General Public License v3.0 |
| `NON-PROFIT` | Non-Profit statement |
| `THIRD-PARTY.md` | third-party components, provenance, licences |
| `scripts/` | ready-made command lines |

---

## Scope

AmigaOS is studied as a bootstrap target to validate M68k peripherals, timing
and the framebuffer bridge. It is not a supported product feature, and no
support commitment or delivery is implied.
