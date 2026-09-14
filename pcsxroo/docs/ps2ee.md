# ps2ee - the generic EE analysis library

`pcsxroo/ps2ee/` and the 13 scripts in `pcsxroo/tools/` are the generic half of the
Budokai Tenkaichi 3 60fps tooling. They were extracted from the sibling
[pcsx2-bt3-60fps](https://github.com/Saupernova13/pcsx2-bt3-60fps) repo on 2026-09-12 with
their history intact; the game-specific half stayed there under `tools/game/`.

## Setting up

Python 3.11 or newer, and four packages:

```
pip install capstone keystone-engine numpy zstandard
```

Settings come from the environment or from `pcsxroo/local.json` (gitignored), environment
first:

| Key | Needed by | Default |
|---|---|---|
| `GAME` | anything that knows the game: ELF, save states, layout bounds | none - see below |
| `PCSX2_DIR` | tools that read a stock PCSX2 install (inis, cheats, save states) | EmuDeck, `%APPDATA%\PCSX2`, `Documents\PCSX2`, `~/.config/PCSX2` or `~/Library/Application Support/PCSX2`, whichever exists |
| `PCSXROO_DIR` | tools that read PCSXROO's portable data | this repo's `bin/` |
| `SCRATCH_DIR` | extracted ELFs and work files | `pcsxroo/work/` (gitignored) |

`GAME` is a JSON object, so it lives in `local.json`. Copy `pcsxroo/local.json.example`,
which carries the Budokai Tenkaichi 3 (SLUS-21678) identity, to `pcsxroo/local.json` and edit
it for another game. A consumer can bind one in code instead with `config.bind()`; the BT3
repo does, through its `tools/game/identity.py`.

The offline tools read the game's boot ELF from `SCRATCH_DIR`, named as `elf_name` in the
identity - `pcsxroo/work/SLUS_216.78` by default. Nothing here ships a game file: the BT3
repo's `extract-elf` tool pulls the ELF out of your own disc image.

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

## The 13 scripts

Run any of them with `python pcsxroo/tools/<tool>.py --help` - the docstrings are the
reference. Each finds the library through `pcsxroo/tools/_bootstrap.py`, so they run from
any working directory.

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

Transport: **PINE** = works against stock PCSX2 with PINE enabled, **offline** = needs no
emulator at all. The tools that drive PCSXROO itself stayed in the BT3 repo.

## The BT3 seam

The BT3 repo imports this library through its own `tools/_bootstrap.py`, which finds this
checkout from, in order, the `PCSXROO_REPO` environment variable, a `PCSXROO_REPO` key in
its `local.json`, or a sibling checkout named `pcsxroo`, and adds `<pcsxroo>/pcsxroo` to
`sys.path` so `import ps2ee` resolves.

## Licence and conventions

This code is GPL-3.0 like the rest of PCSXROO (`COPYING.GPLv3`), unlike its MIT counterpart
in the BT3 repo - a one-way door accepted at migration time. Commits touching it follow
PCSXROO's convention and end with `(AI-assisted)` when an AI wrote them.
