# PCSXROO

This is a fork of PCSX2 that exposes the debugger over a loopback JSON server and a
`pcsxroo` command line client, so an agent can run a whole PS2 debugging session - boot a
game, drive the pad, set breakpoints, read memory and registers, take screenshots - without
touching the user interface.

There are two ways to be here, and they want different documents.

## Using PCSXROO to debug a game

Read **[pcsxroo/docs/agent-guide.md](pcsxroo/docs/agent-guide.md)** first: it is
task-shaped and lists the traps. **[pcsxroo/docs/cli.md](pcsxroo/docs/cli.md)** is the
exhaustive command and protocol reference. `pcsxroo --help` carries the same command list.
**[pcsxroo/docs/ps2ee.md](pcsxroo/docs/ps2ee.md)** covers the Python analysis tooling
under `pcsxroo/ps2ee/`.

```
pcsxroo\tools\build.cmd                          build emulator + CLI into bin\
pcsxroo\tools\seed-portable.ps1                  copy BIOS/settings from an existing PCSX2
pcsxroo\tools\smoke-test.ps1 -Bios               28 checks over the whole path

bin\pcsxroo.exe launch
bin\pcsxroo.exe --timeout 120000 boot "G:\roms\ps2\game.iso"
bin\pcsxroo.exe input press Start
bin\pcsxroo.exe pause
bin\pcsxroo.exe reg dump --category GPR
```

The four things most likely to cost you time:

- **Always pass `--since` to `wait`**, or you will block for a stop that already happened.
- **Registers, `stack`, `step` and `run-to` need a paused VM**; everything else does not.
- **Screenshots need a *running* VM** - a paused GS never presents a frame.
- **Expressions have no `$`**: `--cond "a0 == 2"`, not `$a0`.

Exit codes are the interface: 0 ok, 1 server error, 2 usage, 3 cannot connect, 4 timed out.
3 and 4 differ on purpose - "no breakpoint yet" is not "the emulator is gone".

## Working on PCSXROO itself

- Build with `pcsxroo\tools\build.cmd`, test with `pcsxroo\tools\test.cmd`. **Release is
  the default and is required**: a Devel build cannot boot a VM at all, failing hard just
  after the game database loads. `PCSXROO_BUILD_TYPE=Devel` exists for core work that never
  needs to run a game.
- New code lives in `pcsx2/DebugServer/` (server, commands, JSON, search),
  `pcsx2/DebugTools/DebuggerControl.*` (stepping and stop events, shared with the Qt
  debugger window) and `pcsxroo/cli/` (the client, which links neither PCSX2 nor Qt).
- Every new file under `pcsx2/` goes in three places: `pcsx2/CMakeLists.txt`,
  `pcsx2/pcsx2.vcxproj` and `pcsx2/pcsx2.vcxproj.filters`. Check with
  `.github/workflows/scripts/windows/validate-vs-filters.ps1`.
- Anything touching VM or CPU state must go through
  `DebugServerDispatch::RunOnCPUThreadWithTimeout`, never `Host::RunOnCPUThread(fn, true)` -
  that one cannot time out and will hang a client on a wedged emulator.
- The design and implementation history are in `docs/superpowers/`.

The upstream PCSX2 agent guide follows, and still applies to emulator code.

---
