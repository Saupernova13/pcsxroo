# PCSXROO

This is a fork of PCSX2 that exposes the debugger over a loopback JSON server and a
`pcsxroo` command line client, so an agent can run a whole PS2 debugging session - boot a
game, drive the pad, set breakpoints, read memory and registers, take screenshots - without
touching the user interface.

Everything PCSXROO adds lives in this `pcsxroo/` directory. The repository root's
`AGENTS.md` is PCSX2's own guide and still applies to emulator code - including its rule
never to open issues or pull requests against the PCSX2 project from here.

There are two ways to be here, and they want different documents.

## Using PCSXROO to debug a game

Set it up with [README.md](README.md). Then read **[docs/agent-guide.md](docs/agent-guide.md)**:
it is task-shaped and lists the traps. **[docs/cli.md](docs/cli.md)** is the exhaustive
command and protocol reference, and `pcsxroo --help` carries the same command list.
**[docs/ps2ee.md](docs/ps2ee.md)** covers the Python analysis tooling in `ps2ee/` and `tools/`.

```
pcsxroo\tools\build.cmd                  build emulator + CLI into bin\
pcsxroo\tools\seed-portable.ps1          copy BIOS/settings from an existing PCSX2
pcsxroo\tools\smoke-test.ps1 -Bios       check the whole path end to end

bin\pcsxroo.exe launch
bin\pcsxroo.exe boot "G:\roms\ps2\game.iso"
bin\pcsxroo.exe input press Start
bin\pcsxroo.exe pause
bin\pcsxroo.exe reg dump --category GPR
```

The four things most likely to cost you time:

- **Always pass `--since` to `wait`**, or you will block for a stop that already happened.
- **Registers, `stack`, `step` and `run-to` need a paused VM**; everything else does not.
- **Screenshots need a *running* VM** - a paused GS never presents a frame.
- **Expressions have no `$`, and their numbers are hex**: `--cond "a0 == 2"`, not `$a0`; and
  `--cond "t5 == 0x1ADD9D0"`, because a bare `28170704` means `0x28170704` and never matches.

Exit codes are the interface: 0 ok, 1 server error, 2 usage, 3 cannot connect or connection
lost, 4 timed out. 3 and 4 differ on purpose - "no breakpoint yet" is not "the emulator is
gone".

## Working on PCSXROO itself

- Build with `pcsxroo\tools\build.cmd`, test with `pcsxroo\tools\build.cmd unittests` or
  `pcsxroo\tools\test.cmd`. **Release is the default and is required**: a Devel build cannot
  boot a VM at all, failing hard just after the game database loads.
  `PCSXROO_BUILD_TYPE=Devel` exists for core work that never needs to run a game.
- Where the code is:

  | Path | What |
  |---|---|
  | `pcsxroo/server/` | Debug server, commands, JSON, dispatch, memory search, and `DebuggerControl` (stepping and stop events, shared with the Qt debugger window). Compiled into the core. |
  | `pcsxroo/cli/` | The `pcsxroo` client. Links neither PCSX2 nor Qt. |
  | `pcsxroo/tests/` | `pcsxroo_test`, run as part of `unittests`. |
  | `pcsxroo/ps2ee/`, `pcsxroo/tools/` | Python library and scripts; the build and test scripts. |
  | `pcsxroo/docs/` | The documents above. |

- The only upstream files the fork edits are build wiring (`CMakeLists.txt`,
  `pcsx2/CMakeLists.txt`, `pcsx2/pcsx2.vcxproj` and its `.filters`, `pcsx2-qt/CMakeLists.txt`),
  the config flag (`pcsx2/Config.h`, `pcsx2/Pcsx2Config.cpp`), the VM lifecycle
  (`pcsx2/VMManager.*`), the debugger step refactor (`pcsx2-qt/Debugger/DebuggerWindow.*`),
  branding (`pcsx2-qt/MainWindow.ui`) and unattended start (`pcsx2-qt/QtHost.cpp`,
  `pcsx2-qt/Translations.cpp`). Every such edit carries a `PCSXROO:` comment; keep it that
  way, so `git grep -n "PCSXROO:"` finds every conflict site on an upstream sync. Add new
  code under `pcsxroo/` rather than editing more upstream files.
- A new server source file goes in three places: `pcsx2/CMakeLists.txt`,
  `pcsx2/pcsx2.vcxproj` and `pcsx2/pcsx2.vcxproj.filters`, each marked `PCSXROO:`. Check with
  `.github/workflows/scripts/windows/validate-vs-filters.ps1`. A new test goes in
  `pcsxroo/tests/CMakeLists.txt`.
- **PCSX2 builds with exceptions disabled** (`-fno-exceptions`, `_HAS_EXCEPTIONS=0`). A `try`
  block does not compile on GCC or Clang, and a throw ends the emulator. Report bad input
  through `Fail`, and never call anything that throws on it (`std::stoi` and friends).
- Anything touching VM or CPU state must go through
  `DebugServerDispatch::RunOnCPUThreadWithTimeout`, never `Host::RunOnCPUThread(fn, true)` -
  that one cannot time out and will hang a client on a wedged emulator. A task that has not
  started by the timeout is cancelled, so capturing locals by reference is safe.
- CI builds and runs the unit tests on Windows, Linux and macOS on every push. The helper
  scripts in `pcsxroo/tools/` are Windows only.
- **Releasing:** push an annotated tag named `pcsxroo-vX.Y.Z` on a green `master`
  (`git tag -a pcsxroo-v1.0.0 -m "PCSXROO 1.0.0"`, then `git push origin pcsxroo-v1.0.0`).
  `.github/workflows/pcsxroo_release.yml` builds all three platforms and publishes them as a
  GitHub Release with `pcsxroo/docs/release-notes.md` as its notes. Never tag PCSXROO `vX.Y.Z`:
  a build at a tag of PCSX2's shape switches on PCSX2's auto-updater, which offers to replace
  PCSXROO with upstream PCSX2. That workflow and `.github/README.md` are the only files PCSXROO
  adds outside `pcsxroo/`.
- Commits are conventional (`fix(cli): ...`) and end with ` (AI-assisted)` when an AI wrote
  them.
