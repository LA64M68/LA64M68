# Third-party components

Provenance and licence of everything third-party that LA64M68 is built on.
This project is GNU GPLv3+; the components below are used under their own
permissive terms, whose notices are reproduced here.

## Musashi 68k core — via PiStorm32l

| | |
|---|---|
| Component | Musashi 68000/68010/68020 emulator core |
| Upstream | Karl Stenerud, *Musashi* |
| As used through | `captain-amygdala/pistorm32l` (PiStorm32-lite emulator), commit `cc0244c` |
| License | MIT |
| Copyright | Copyright (c) 2021 PiStorm developers |

LA64M68's M68k work is based on the Musashi solution of PiStorm, as described
in the project announcement. Musashi is used for opcode and semantics
verification and as the reference for the instruction set.

MIT licence text:

```
Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## Lib(X)SVF — ISC

Only relevant if the FPGA programming helper is ever shipped as a tool:

| | |
|---|---|
| Component | Lib(X)SVF `xsvftool-gpio` |
| Upstream | Clifford Wolf / RIEGL Research ForschungsGmbH |
| License | ISC |
| Copyright | Copyright (c) 2009 RIEGL Research ForschungsGmbH, (c) 2009 Clifford Wolf |

ISC licence text:

```
Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
```

## Not redistributed

Vendor FPGA programming files produced by Intel/Altera Quartus are output
under the Intel Quartus Prime EULA and are **not** redistributed by this
project, in any form. Programming such a device locally is the use the vendor
terms permit.

Any third-party hardware design documents referenced during development are
study material only and are not copied, forked or redistributed here.

---

Before a release, this file must list exactly what the released archive
contains. Anything not listed here is either this project's own GPLv3+ code or
must not be shipped.
