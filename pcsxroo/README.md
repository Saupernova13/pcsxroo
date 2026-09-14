# PCSXROO

## 1. What it is

A fork of [PCSX2](https://github.com/PCSX2/pcsx2) that exposes the debugger over a loopback
JSON server and a `pcsxroo` command line client, so an agent or a script can run a whole PS2
debugging session - boot a game, drive the pad, set breakpoints and memchecks, step, read and
write memory and registers, search memory, disassemble and assemble, take screenshots and save
states - without touching the user interface. The Qt debugger window still works, and shares
the same stepping code.

It also carries `ps2ee`, a Python library and scripts for static and live analysis of PS2 EE
code, built for the
[Budokai Tenkaichi 3 60fps patch](https://github.com/Saupernova13/pcsx2-bt3-60fps).

What it is not: a PCSX2 to play games on. It runs from its own portable folder, is tuned for
unattended sessions, and its debug server grants full control of the emulator to anything on
the machine. Play on stock PCSX2. PCSXROO is not supported by the PCSX2 team - do not report
its problems to them.

Everything PCSXROO adds is in this `pcsxroo/` directory; the rest of the repository is PCSX2.

**Prebuilt downloads** for Windows, Linux and macOS are on the
[Releases page](https://github.com/Saupernova13/pcsxroo/releases); each release says what its
files are and what to do first. The rest of this document is for building it yourself.

## 2. Prerequisites

On Windows 10 or 11, x64:

- **Git.** The repository has no submodules; a plain clone is complete.
- **Visual Studio 2022 Build Tools** (or any Visual Studio 2022 edition) with the
  *Desktop development with C++* workload and the *C++ CMake tools for Windows* component.
  The build script finds the installation itself.
- **PCSX2's Windows dependency package.** Download `pcsx2-windows-dependencies.7z` from
  [pcsx2-windows-dependencies releases](https://github.com/PCSX2/pcsx2-windows-dependencies/releases/latest)
  and extract it so that `deps\lib` exists at the repository root.
- **A PS2 BIOS dumped from your own console.** None is included, and none ever will be.
- For `ps2ee` only: **Python 3.11 or newer** with
  `pip install capstone keystone-engine numpy zstandard`.

Linux and macOS are covered in [section 8](#8-linux-and-macos).

## 3. Build

```
git clone https://github.com/Saupernova13/pcsxroo.git
cd pcsxroo
pcsxroo\tools\build.cmd
```

The first run configures a Release build in `build\`, compiles everything, and installs the
emulator (`pcsxroo-qt.exe`), the CLI (`pcsxroo.exe`) and their runtime files into `bin\`.
Later runs are incremental. Release is required: a Devel build cannot boot a game.

Unit tests:

```
pcsxroo\tools\build.cmd unittests      build and run every suite, PCSX2's and PCSXROO's
pcsxroo\tools\test.cmd                 run them again without building
```

## 4. Portable data folder

PCSXROO keeps its settings, BIOS and memory cards beside the executable (a `portable.ini` in
`bin\` switches that on), so it never touches the PCSX2 you play on. Fill it one of two ways.

**From an existing PCSX2**, keeping your BIOS, controls, hotkeys and per-game settings:

```
pcsxroo\tools\seed-portable.ps1                        finds EmuDeck, Documents\PCSX2 or %APPDATA%\PCSX2
pcsxroo\tools\seed-portable.ps1 -From "D:\emu\PCSX2"
```

It changes exactly four things in the copy: removes RetroAchievements credentials, turns off
hardcore mode (which disables the debug server), clears folder overrides that point back at
the source install, and turns off start-fullscreen. It also fetches PCSX2's `patches.zip`.

**By hand:** create an empty `bin\portable.ini`, start `bin\pcsxroo-qt.exe` once, point the
setup wizard at a folder holding your BIOS, and leave RetroAchievements hardcore mode off.

## 5. First launch and the smoke test

```
bin\pcsxroo.exe launch                  start the emulator with the debug server on port 28110
bin\pcsxroo.exe status                  state, PCs, game identity
bin\pcsxroo.exe boot --bios             boot the BIOS with no disc
bin\pcsxroo.exe shutdown
```

`launch` with no game leaves an emulator with the server listening and no VM, which is where
most sessions start. To start it yourself instead: `bin\pcsxroo-qt.exe -debugserver 28110`,
adding `-pauseonentry` to halt at the ELF entry point without opening the debugger window.

The smoke test drives all of that end to end on a spare port:

```
pcsxroo\tools\smoke-test.ps1            server-only checks, no game needed
pcsxroo\tools\smoke-test.ps1 -Bios      also boots the BIOS and exercises the debugger
pcsxroo\tools\smoke-test.ps1 -Game "G:\roms\ps2\game.iso"
```

`pcsxroo\tools\doc-examples.ps1` re-runs the documented behaviours against a live emulator,
so documentation drift fails loudly.

The server is off by default and binds `127.0.0.1` only. It has no authentication: never
expose it to a network.

## 6. Using the CLI

```
bin\pcsxroo.exe boot "G:\roms\ps2\game.iso"
bin\pcsxroo.exe input press Start
bin\pcsxroo.exe bp add 0x12BBD0 --cond "a0 == 2"
bin\pcsxroo.exe run
bin\pcsxroo.exe wait --since 0
bin\pcsxroo.exe reg dump --category GPR
bin\pcsxroo.exe screenshot work\frame.png
```

| Document | For |
|---|---|
| [docs/agent-guide.md](docs/agent-guide.md) | Driving it: session shape, recipes, and the traps that cost time. Read first. |
| [docs/cli.md](docs/cli.md) | Every command, argument, result field, error and exit code, plus the wire protocol. |
| [AGENTS.md](AGENTS.md) | Orientation for agents, and how to work on PCSXROO itself. |
| `pcsxroo --help` | The command list, offline. |

Exit codes are the interface: 0 ok, 1 server error, 2 usage, 3 cannot connect or connection
lost, 4 timed out.

## 7. Using ps2ee

```
copy pcsxroo\local.json.example pcsxroo\local.json
python pcsxroo\tools\disas.py --help
```

`local.json` holds the game identity - serial, CRC, boot ELF name and memory layout. The
example is Budokai Tenkaichi 3 (SLUS-21678); edit it for another game. The offline tools
read the boot ELF from `pcsxroo\work\<elf_name>` (`SCRATCH_DIR` moves that folder); no game
file ships, and the BT3 repo's `extract-elf` pulls the ELF from your own disc image.

[docs/ps2ee.md](docs/ps2ee.md) lists every module and script, the other settings
(`PCSX2_DIR`, `PCSXROO_DIR`, `SCRATCH_DIR`) and how the BT3 repo finds this checkout.

## 8. Linux and macOS

CI builds PCSXROO and runs every unit test on Windows, Linux and macOS on every push, so the
emulator, the debug server and the CLI compile and pass their tests on all three. The helper
scripts in `pcsxroo/tools/` are Windows only; elsewhere use CMake directly, as CI does.

**Linux.** Install the packages listed in `.github/workflows/linux_build_qt.yml`, then:

```
.github/workflows/scripts/linux/build-dependencies-qt.sh "$HOME/deps"
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$HOME/deps" \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
ninja -C build
ninja -C build unittests
build/bin/pcsxroo launch
```

The emulator and the CLI both land in `build/bin/`, so `launch` finds the emulator beside
itself. On Linux it keeps PCSX2's name, `pcsx2-qt`, which the AppImage and Flatpak packaging
run by name; only Windows builds are called `pcsxroo-qt`.

**macOS.** Install Xcode, then `brew install cmake ninja nasm`, then:

```
.github/workflows/scripts/macos/build-dependencies.sh "$HOME/deps"
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$HOME/deps"
ninja -C build
ninja -C build unittests
build/pcsxroo/cli/pcsxroo launch --emulator build/pcsx2-qt/PCSX2.app/Contents/MacOS/PCSX2
```

On macOS the emulator is an app bundle that keeps PCSX2's name, and the CLI is not placed
beside it, so `launch` needs `--emulator`.

On both, the first run asks for a BIOS; leave RetroAchievements hardcore mode off.

## 9. Syncing with upstream PCSX2

PCSXROO edits as few upstream files as it can, and every edit carries a `PCSXROO:` comment:

```
git grep -n "PCSXROO:"
```

lists every place a sync can conflict. Everything else PCSXROO adds is under `pcsxroo/` and
never conflicts. To take a newer PCSX2:

```
git remote add upstream https://github.com/PCSX2/pcsx2.git     once
git fetch upstream
git merge upstream/master
```

Merge rather than rebase: the fork's `master` is published, and rewriting it breaks every
other clone. On a conflict, **upstream's side wins**; then re-apply PCSXROO's change at the
`PCSXROO:` site, keeping its marker. Afterwards run `pcsxroo\tools\build.cmd unittests` and
`pcsxroo\tools\smoke-test.ps1 -Bios`, and check `git grep -n "PCSXROO:"` still lists every
hook.

## 10. Licence

GPL-3.0, the same as PCSX2 - see [`COPYING.GPLv3`](../COPYING.GPLv3). That covers `ps2ee`
too.
