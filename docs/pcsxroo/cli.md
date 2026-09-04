# PCSXROO debugger CLI

PCSXROO is a fork of PCSX2 that exposes the debugger over a loopback JSON server, so an
agent can run a complete PS2 debugging session without touching the user interface.

Everything the debugger window can do — breakpoints, memchecks, stepping, registers,
memory, disassembly, assembly, symbols, the stack, threads — is reachable from `pcsxroo`,
along with the session commands that would otherwise need the main window: booting,
savestates, screenshots, frame advance and patch reload.

## Quick start

```
tools\pcsxroo\build.cmd                 build the emulator and the CLI into bin\
tools\pcsxroo\seed-portable.ps1         copy BIOS and settings from an existing PCSX2

bin\pcsxroo.exe launch --pause-on-entry "G:\roms\ps2\game.iso"
bin\pcsxroo.exe bp add 0x12BBD0 --cond "a0 == 2" --desc "per-frame routine"
bin\pcsxroo.exe run
bin\pcsxroo.exe wait --timeout 60000
bin\pcsxroo.exe reg dump --category GPR
bin\pcsxroo.exe dis 0x12BBD0 20
bin\pcsxroo.exe asm 0x12BBD4 "nop"
bin\pcsxroo.exe screenshot work\frame.png
bin\pcsxroo.exe shutdown
```

Run `tools\pcsxroo\smoke-test.ps1` to check the whole path end to end. It needs no game;
add `-Bios` or `-Game <path>` to exercise the debugger against a running VM as well.

## Starting the server

The server is **off by default** and binds `127.0.0.1` only.

| How | What it does |
|---|---|
| `-debugserver <port>` | Turns it on for this run and overrides the ini. Survives settings reloads. |
| `EnableDebugServer` / `DebugServerPort` under `[EmuCore]` | Turns it on permanently. Default port 28110. |
| `-pauseonentry` | Halts at the ELF entry point **without** opening the debugger window. |

`-debugger` is PCSX2's existing flag: it also pauses on entry, but it opens the debugger
window, which is what this project exists to avoid.

RetroAchievements hardcore mode disables the server, exactly as it already disables PINE.
The server grants unrestricted memory access and process control with no authentication,
so it is strictly more powerful than PINE and gets the same treatment.

PINE is untouched and can run at the same time on its own port.

## CLI

```
pcsxroo [global flags] <group> <verb> [arguments]
```

| Global flag | Meaning |
|---|---|
| `--port N` | Server port. Default 28110, or `$PCSXROO_PORT`. |
| `--host H` | Default `127.0.0.1`. |
| `--cpu ee\|iop` | Which CPU to address. Default `ee`. |
| `--json` | Print the raw JSON reply instead of a rendered one. |
| `--timeout MS` | Request timeout. For `wait` and `step` this sets the command's own timeout too. |

### Exit codes

| Code | Meaning |
|---|---|
| 0 | Success |
| 1 | The server returned an error |
| 2 | Usage error |
| 3 | Could not connect — the emulator is not running |
| 4 | The command timed out |

3 and 4 are deliberately different: a script has to be able to tell "no breakpoint hit
yet" from "the emulator is gone".

### Commands

**Session** — `version`, `status`, `wait [--since N]`, `events`,
`launch [game] [--bios] [--pause-on-entry] [--emulator PATH] [--ready-timeout MS]`

With no game, `launch` starts the emulator idle with the server up and no VM. That is
deliberately not the same as booting: starting the emulator and starting a VM fail for
different reasons, and an agent that cannot tell them apart is stuck.

**Execution** — `run`, `resume`, `pause`, `step into|over|out`, `run-to <addr>`,
`frame-advance [count]`, `reset`, `shutdown`,
`boot <game> | boot --bios | boot --elf PATH [--pause-on-entry] [--fast-boot]`

`boot` starts a VM in an emulator that is already up, so an agent can boot, shut down and
boot something else without restarting the process. `resume` is an alias for `run`.

**Input** — `input press <button...> [--pad N] [--frames N]`,
`input set <button...> [--left-stick X,Y] [--right-stick X,Y]`,
`input release`, `input list [--pad N]`

`press` taps for a couple of frames; `set` holds until changed, and `set` with no buttons
releases that pad. Analog components run -1..1. Button names come from `input list` and are
the pad's own (`Cross`, `Start`, `L1`, `LUp`, ...), matched case insensitively.

Injection goes through the same entry point the real input sources use, so deadzone,
pressure and inversion settings all still apply. Held state is re-asserted every frame,
because the pad is repolled from the real controllers each frame and a single write would
be overwritten before the game saw it.

**Breakpoints** — `bp add <addr> [--cond EXPR] [--desc TEXT] [--temporary] [--disabled]`,
`bp remove|enable|disable <addr>`, `bp list [--include-temp]`, `bp clear`

**Memchecks** — `mc add <start> <end> --on read,write,change [--log] [--cond EXPR]`,
`mc remove <start> <end>`, `mc list`, `mc clear`

**Registers** (need a paused VM) — `reg list`, `reg get <name>`, `reg set <name> <value>`,
`reg dump [--category NAME]`

Names accept `a0`, `$a0`, `gpr:a0`, and the pseudo-registers `pc`, `hi`, `lo`.

**Memory** — `mem read <addr> [size]`, `mem write <addr> <hex>`,
`mem fill <addr> <size> <pattern>`, `mem dump <addr> <size> <path>`

**Code and symbols** — `dis <addr> [count] [--raw]`, `asm <addr> "<instruction>"`,
`sym lookup <addr>`, `sym find <name>`

**Context** — `stack`, `threads`, `modules`, `eval "<expression>"`

**State and observation** — `savestate --slot N|--path P [--wait-flush]`,
`loadstate --slot N|--path P`, `screenshot <path>`, `patch reload`

### Addresses and expressions

Anywhere an address is taken, all of these work:

```
0x12BBD0        hex with a prefix
12BBD0          bare hex
1227728         decimal
main+0x40       an expression, resolved by the debugger's own parser
[0x1B1F038]     a memory dereference
```

`eval` exposes the same parser directly, and is the escape hatch for anything the command
set does not model.

**Registers in expressions have no `$`.** `a0 == 2` is a valid condition; `$a0 == 2` is
rejected with "Invalid operator". The `$` prefix is accepted only by `reg get` and
`reg set`, which take a register name rather than an expression.

## Wire protocol

TCP, loopback only, one UTF-8 JSON object per line, `\n` terminated. Requests are capped at
1 MiB. `protocol_version` is `1`; check it with `version`.

```json
{"id": 7, "cmd": "bp.add", "args": {"cpu": "ee", "addr": 1227728, "condition": "a0 == 2"}}
{"id": 7, "ok": true, "result": {"addr": 1227728, "addr_hex": "0x0012bbd0"}}
{"id": 7, "ok": false, "error": {"code": "not_paused", "message": "..."}}
```

Addresses come back as a number plus a `_hex` sibling: the number is what a program wants,
the string is what a person reads in a log.

Error codes: `no_vm`, `not_paused`, `bad_address`, `bad_args`, `unknown_command`, `timeout`,
`parse_error`, `io_error`, `unsupported`, `internal_error`.

### Stop events

Every stop gets a monotonically increasing sequence number.

```json
{"seq": 12, "reason": "breakpoint", "cpu": "ee", "pc": 1227728, "pc_hex": "0x0012bbd0", ...}
```

`reason` is one of `breakpoint`, `memcheck`, `step`, `user`, `entry`, `vm_shutdown`.

**Always pass `--since`** when polling. A breakpoint can fire between your `run` and your
`wait`; `wait --since N` returns immediately for a stop that already happened, whereas a
bare `wait` would block until the next one. This is the single most common way an
unattended agent gets stuck.

`pcsxroo events` subscribes instead of polling and streams stops as they happen.

## Worked example: finding a per-frame counter

```
pcsxroo launch --pause-on-entry "G:\roms\ps2\game.iso"
pcsxroo run

pcsxroo mem search --type u32 --unknown --range 0x100000:0x2000000   # snapshot everything
pcsxroo mem search --session 1 --increased-by 1                      # ticked once
pcsxroo mem search --session 1 --increased-by 1                      # and again
```

Each pass narrows the previous result set against current memory, the same way the GUI
memory searcher does, but live and scriptable.

## Known limitations

- **Write memchecks do not see every write.** `Breakpoints.h` records that memchecks are
  "not used in the interpreter or HLE currently", and writes that do not go through cached
  EE stores never reach them. A memcheck that never fires may be pointing at a perfectly
  correct address. `mc add` returns a `note` saying so whenever you ask it to watch writes.
- **Register access requires a paused VM.** There is no consistent way to sample the
  register file of a running recompiled CPU, so the server refuses rather than returning a
  torn value. Memory reads are allowed while running.
- **The server has no authentication.** It binds loopback only and defaults to off. Never
  expose it to a network.
- **`mem.dump` writes files with the emulator's privileges**, server side.
- **Savestates and screenshots are asynchronous.** Both reply `queued`. Use
  `savestate --wait-flush` when you need the file on disk before continuing. A screenshot
  needs the VM *running*: the GS only presents a frame while it executes, so one requested
  while paused sits in the queue and no file appears.
- **`status` reports a stale PC while the VM is running.** The recompiler only writes the
  program counter back at certain points, so the value is meaningful once paused and
  little more than a hint before that. `pause` first if the PC matters.
- **PCSXROO must be built Release.** A Devel build cannot boot a VM at all: it fails hard
  just after the game database loads. `tools\pcsxroouild.cmd` defaults to Release;
  `PCSXROO_BUILD_TYPE=Devel` is there for core work that does not need to run a game.

## Startup gotchas

These block an unattended start and are all handled by `seed-portable.ps1`, but are worth
knowing if you configure PCSXROO by hand:

- A settings file copied from another installation carries **directory overrides** that
  point back at the original, so PCSXROO looks for its BIOS in the wrong place.
- **Achievements** log in over the network during CPU thread startup, before the server
  exists.
- A **game list search path** triggers a recursive scan at startup.
- **Start-fullscreen** makes an unattended session fight for the display.
- A copied settings file also carries the original's **RetroAchievements credentials**.

PCSXROO additionally downgrades PCSX2's "Translation Error" dialog to a console warning:
in a Devel build it fires for any system locale with no shipped `.qm`, and a modal box
during startup blocks the CPU thread before the server ever starts.
