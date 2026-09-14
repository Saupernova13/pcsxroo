# PCSXROO

A fork of PCSX2 that exposes the debugger over a loopback JSON server and a `pcsxroo`
command line client, so an agent can run a complete PS2 debugging session without touching
the user interface.

```
pcsxroo\tools\build.cmd              build the emulator and the CLI into bin\
pcsxroo\tools\seed-portable.ps1      bring over BIOS, controls and settings
pcsxroo\tools\smoke-test.ps1 -Bios   verify the whole path (28 checks)

bin\pcsxroo.exe launch
bin\pcsxroo.exe --timeout 120000 boot "G:\roms\ps2\game.iso"
bin\pcsxroo.exe input press Start
bin\pcsxroo.exe bp add 0x12BBD0 --cond "a0 == 2"
bin\pcsxroo.exe run
bin\pcsxroo.exe wait --since 0
bin\pcsxroo.exe reg dump --category GPR
bin\pcsxroo.exe screenshot work\frame.png
```

47 commands: booting, pad input, breakpoints, memchecks, stepping, registers, memory,
chained memory search, disassembly, assembly, symbols, stack, threads, savestates,
screenshots, frame advance and patch reload. The debugger window still works exactly as
it did and shares the same core code.

## Documentation

| Document | For |
|---|---|
| [pcsxroo/docs/agent-guide.md](pcsxroo/docs/agent-guide.md) | Driving it: recipes, workflows, the traps that cost time |
| [pcsxroo/docs/cli.md](pcsxroo/docs/cli.md) | Every command, argument, result field and error code |
| [AGENTS.md](AGENTS.md) | Orientation for agents, and how to work on PCSXROO itself |
| `pcsxroo --help` | The same command list, offline |

## Notes

PCSXROO runs in portable mode from its own folder and never writes to the PCSX2 install
it was seeded from. It must be built **Release** - a Devel build cannot boot a game at
all.

The debug server is off by default, binds loopback only, and has no authentication: it
grants full memory and process control. Never expose it to a network.

---

The upstream PCSX2 README follows.
