# ps2ee - the generic EE analysis library

`tools/pcsxroo/ps2ee/` and the 13 scripts beside it are the generic half of the
BT3 60fps tooling. They were extracted from the sibling `pcsx2-bt3-60fps` repo
on 2026-09-12 with their history intact (graft merge `a8d24e33e`); the
game-specific half stayed in that repo under `tools/game/`.

## Library

| Module | What it does |
|---|---|
| `asm` | MIPS assembler for safe-zone hook code. |
| `ciso` | Random-access reader for CISO/ZISO disc images plus a minimal ISO9660 walker. |
| `config` | Paths and game identity for the PS2 analysis tooling. |
| `diff` | Differential RAM scanning across save states. |
| `disasm` | MIPS disassembly, function boundaries and cross-references for EE code. |
| `eemem` | A view over EE main memory, from a save state, a raw dump, or the ELF. |
| `live` | Safe installation of safe-zone trampolines into a running game. |
| `pine` | PINE IPC client for a running PCSX2. |
| `pnach` | Parse, emit, validate and deploy PCSX2 pnach files. |
| `roo` | PCSXROO debug-server client. |
| `savestate` | Reader for PCSX2 `.p2s` save states. |

`config` takes a game identity - serial, CRC, ELF name, layout bounds - from a
`"GAME"` block in `tools/pcsxroo/local.json`, or programmatically via
`config.bind()`. The BT3 repo binds its own identity through its
`tools/game/identity.py`. Scratch output defaults to `tools/pcsxroo/work/`
(gitignored), overridable with `SCRATCH_DIR`.

## The 13 scripts

Run any of them with `python tools/pcsxroo/<tool>.py --help` - the docstrings
are the reference.

| Tool | Transport | What it does |
|---|---|---|
| `countdown.py` | offline | Find countdown timers in a saved fighter trace, at byte granularity. |
| `dataxref.py` | offline | Find the code that touches a global, by address rather than by call graph. |
| `disas.py` | offline | Disassemble EE code around one or more addresses. |
| `looptree.py` | offline | Walk the call tree under a loop, then scan only that code for step constants. |
| `mkhalf.py` | offline | Turn `field += 1.0` sites into `field += 0.5`, one trampoline each. |
| `phasetimer.py` | offline | Find the fighter state machine's phase timers. |
| `probe-loop.py` | PINE | Disable one call in a loop at a time, live, to see what it drives. |
| `radar.py` | offline | Static scan for frame-pacing constants - a fast standalone pass. |
| `ramdiff.py` | offline | Differential memory search across save states. |
| `setup-pcsx2.py` | PINE | Inspect and adjust the PCSX2 settings this workflow depends on. |
| `tickcount.py` | offline | Find every integer `field = field + 1` in the binary - the frame counters. |
| `tickstep.py` | offline | Find every `field += 1.0` in the binary, including the hoisted ones. |
| `xref.py` | offline | Find who calls a function, and where its address is stored. |

Transport: **PINE** = works against stock PCSX2 with PINE enabled, **offline**
= needs no emulator at all. PCSXROO-transport tools stayed in the BT3 repo.

## The BT3 seam

The sibling `pcsx2-bt3-60fps` repo imports this library through its own
`tools/_bootstrap.py`, which adds `../pcsxroo/tools/pcsxroo` to `sys.path`.
A checkout of this repo next to that one is required for its tools to run; it
stays on branch `fix/frame-advance-input` until the PCSXROO work merges.

## Licence and conventions

This code lives under pcsxroo's GPLv3 (`COPYING.GPLv3`), unlike its MIT
counterpart in the BT3 repo - a one-way door accepted at migration time.
Commits touching it follow pcsxroo's convention and carry `(AI-assisted)`.
