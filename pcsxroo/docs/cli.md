# PCSXROO command reference

Complete reference for the debug server protocol and the `pcsxroo` CLI. Every JSON example
below is real output captured from a running emulator.

If you are an agent about to drive this for the first time, read
**[agent-guide.md](agent-guide.md)** first — it is task-shaped and covers the traps. This
file is the exhaustive list.

## Contents

- [Starting up](#starting-up)
- [Invocation](#invocation)
- [Guards: what each command needs](#guards-what-each-command-needs)
- [Addresses, expressions and values](#addresses-expressions-and-values)
- [Commands](#commands)
- [Wire protocol](#wire-protocol)
- [Errors](#errors)
- [Limitations](#limitations)

## Starting up

```
pcsxroo\tools\build.cmd              build emulator + CLI into bin\
pcsxroo\tools\seed-portable.ps1      copy BIOS/settings from an existing PCSX2
pcsxroo\tools\smoke-test.ps1 -Bios   verify the whole path end to end
```

Then either let the CLI start the emulator:

```
pcsxroo launch                       emulator up, server listening, no VM
pcsxroo boot "G:\roms\ps2\game.iso"  boot into it
```

or start it yourself with `pcsxroo-qt.exe -debugserver 28110 [-pauseonentry]`.

The server is **off by default** and binds `127.0.0.1` only.

| How to enable | Effect |
|---|---|
| `-debugserver <port>` | On for this run, overrides the ini, survives settings reloads. |
| `EnableDebugServer` / `DebugServerPort` in `[EmuCore]` | On permanently. Default port 28110. |
| `-pauseonentry` | Halt at the ELF entry point **without** opening the debugger window. |

PCSX2's own `-debugger` flag also pauses on entry but opens the debugger window, which is
what this project exists to avoid. RetroAchievements hardcore mode disables the server, the
same rule that already disables PINE. PINE is untouched and can run alongside on its own
port.

## Invocation

```
pcsxroo [global flags] <group> <verb> [arguments]
```

| Global flag | Default | Meaning |
|---|---|---|
| `--port N` | 28110, or `$PCSXROO_PORT` | Server port. |
| `--host H` | `127.0.0.1` | Server host. |
| `--cpu ee\|iop` | `ee` | Which CPU the command addresses. |
| `--json` | off | Print the raw JSON reply instead of rendering it. |
| `--timeout MS` | the server's own limits | For `wait`, `step`, `pause` and `frame-advance`: the command's own time limit (defaults 60000, 5000, 2000 and 10000). For any other command: give up after MS with no reply. At least 1. |
| `--help`, `-h` | | Print the command list and exit 0. Honoured anywhere on the line. |

Without `--timeout` the CLI waits as long as the server allows the command (a boot gets 60
seconds, a loadstate 30), so a slow command is never cut off by the client.

Global flags may appear before or after the command. Anything a command does not take - an
unknown option, a second comparison, an extra argument - is a usage error rather than being
ignored, so quote an expression that contains spaces: `eval "a0 == 2"`.

### Exit codes

| Code | Meaning |
|---|---|
| 0 | Success |
| 1 | The server returned an error |
| 2 | Usage error (bad arguments, unknown command or option) |
| 3 | Could not connect, or the connection was lost — the emulator is not running or has exited |
| 4 | No reply in time, or the server returned a `timeout` error |

**3 and 4 are deliberately different.** A script must be able to tell "no breakpoint hit
yet" from "the emulator is gone".

## Guards: what each command needs

| Requirement | Commands |
|---|---|
| Nothing (works with no VM) | `version`, `wait`, `subscribe` / `events` |
| **No** VM running | `boot` — fails with `bad_args` if one already is |
| A VM, running or paused | everything not listed elsewhere |
| A **paused** VM | `reg.get`, `reg.set`, `reg.dump`, `stack`, `step`, `run-to` |

Commands needing a VM return `no_vm` without one; commands needing a paused VM return
`not_paused`. `status` works in every state and simply reports less when there is no VM.

## Addresses, expressions and values

Anywhere an address is accepted, all of these work:

```
0x12BBD0        hex with a prefix
12BBD0          bare hex
1227728         decimal
main+0x40       an expression, resolved by the debugger's parser
[0x1B1F038]     a memory dereference
```

Literals resolve locally; anything else is evaluated on the CPU thread, which costs a round
trip and can come back as `bad_address`.

**Numbers inside an expression are hex, even without a prefix.** The two paths above read
numbers differently. A plain literal is decimal when it is all digits (`1227728`) and hex
when it has a prefix or a hex letter (`0x12BBD0`, `12BBD0`). Everything the debugger's parser
handles - an address like `main+0x40` or `1227728+4`, a `bp add --cond`, `eval` - reads
every bare number as hex, so `eval "10"` is 16 and `--cond "t5 == 28170704"` compares against
`0x28170704`. A decimal value in a condition is not an error, it simply never matches.
Write `0x` everywhere and the question never comes up.

**Registers in expressions have no `$`.** `a0 == 2` is valid; `$a0 == 2` is rejected with
`Invalid operator`. The `$` prefix is accepted *only* by `reg get` / `reg set`, which take a
register name rather than an expression. `==` is the equality operator; `=` is not.

Addresses come back as a number plus a `_hex` sibling — the number is for programs, the
string for logs.

## Commands

### version

No arguments. Works with no VM.

```json
{"emulator":"PCSXROO","pcsx2_base":"v2.9.26-20-g8a6c21a2c",
 "pcsx2_hash":"8a6c21a2cddb30eb2d662aaf6a3b3bcdbc81e6f4",
 "protocol_version":1,"port":28110}
```

### status

No arguments. Works in every state. `ee`, `iop` and `game` appear only when a VM exists.

```json
{"vm_state":"running","paused":false,
 "last_stop":{"seq":0,"reason":"none","cpu":"ee","pc":0,"pc_hex":"0x00000000",
              "bp_addr":0,"mem_addr":0,"mem_size":0,"mem_write":false},
 "ee":{"pc":532416,"pc_hex":"0x00081fc0"},
 "iop":{"pc":44692,"pc_hex":"0x0000ae94"},
 "game":{"serial":"20020207-164243","title":"PS2 BIOS (USA)","version":"",
         "crc":0,"crc_hex":"0x00000000"}}
```

`vm_state` is `shutdown`, `running`, `paused`, `stopping`, `resetting` or `initializing`.

> The PC is only trustworthy while paused — see [Limitations](#limitations).

### boot

Starts a VM in an emulator that is already up. Fails if one is already running.

| Argument | Type | Default | Notes |
|---|---|---|---|
| `path` | string | — | Game image: iso, cso, chd and so on. |
| `elf` | string | — | Boot an ELF directly. |
| `bios` | bool | false | Boot the BIOS with no disc. |
| `pause_on_entry` | bool | false | Halt at the entry point. |
| `fast_boot` | bool | ini | Skip the console intro. |

Exactly one of `path`, `elf` or `bios:true` is required. Returns
`{"booted":true,"vm_state":"running"}`. Booting takes tens of seconds; the server allows it
60, and the CLI waits for that without being told to.

```
pcsxroo boot "G:\roms\ps2\game.iso"
pcsxroo boot --bios
```

### run / resume, pause, reset, shutdown

No arguments. `resume` is a CLI alias for `run`.

`pause` blocks briefly for the resulting stop and returns a [stop event](#stop-events).
`run`, `reset` and `shutdown` return `{"vm_state":...}` or `{"stopping":true}`.

### step

| Argument | Type | Default |
|---|---|---|
| `mode` | `into` \| `over` \| `out` | `into` |
| `cpu` | `ee` \| `iop` | `ee` |
| `timeout_ms` | number | 5000 |

Needs a paused VM. Blocks until the step lands and returns the stop event.

```json
{"seq":2,"reason":"step","cpu":"ee","pc":532420,"pc_hex":"0x00081fc4",
 "bp_addr":532420,"bp_addr_hex":"0x00081fc4","mem_addr":0,"mem_size":0,"mem_write":false}
```

`out` returns `unsupported` when there is no caller frame to return to.

### run-to

`addr` (required), `cpu`. Needs a paused VM. Sets a temporary breakpoint and resumes.
**Returns immediately** — the target may never be reached, so you choose how long to wait
using `wait`.

### frame-advance

`count` (1–600, default 1), `timeout_ms` (default 10000). Advances that many frames and
pauses again. Returns `{"frames":2,"stop":{...}}`.

A breakpoint or memcheck that fires first ends the advance where it fires: `stop.reason`
says which, and `frames` is still the number requested. The frames that did not run are
dropped, so a later `run` runs freely instead of pausing when they would have run out. Any
other pause - `pause`, a step, a shutdown - cancels a pending advance the same way.

### wait

| Argument | Type | Default |
|---|---|---|
| `since` | number | 0 |
| `timeout_ms` | number | 60000, max 86400000 |

Works with no VM. Blocks until a stop with `seq` greater than `since`. Returns the stop
event, or a `timeout` error (exit 4).

**Always pass `--since`.** A breakpoint can fire between your `run` and your `wait`;
`--since N` returns immediately for a stop that already happened, whereas a bare `wait`
blocks until the *next* one. This is the most common way an unattended agent gets stuck.

### events / subscribe

`pcsxroo events` subscribes and streams stop events as they happen, one JSON line each,
flushed per line. Preferable to polling for a long unattended watch.

### bp.add / bp.remove / bp.list / bp.enable / bp.disable / bp.clear

| Argument | Type | Default | Used by |
|---|---|---|---|
| `addr` | address | — | add, remove, enable, disable |
| `cpu` | `ee` \| `iop` | `ee` | all |
| `enabled` | bool | true | add |
| `temporary` | bool | false | add |
| `condition` | expression | — | add |
| `description` | string | — | add |
| `include_temp` | bool | false | list |

```json
{"breakpoints":[{"addr":1048576,"addr_hex":"0x00100000","cpu":"ee","enabled":true,
                 "temporary":false,"stepping":false,"condition":"","description":"example"}]}
```

A condition that fails to parse aborts the whole command with `bad_args` — no breakpoint is
left behind. `bp.remove` on an address with none returns `{"removed":false}` rather than an
error, so cleanup is idempotent. `bp.clear` only clears the addressed CPU.

```
pcsxroo bp add 0x12BBD0 --cond "a0 == 2" --desc "per-frame routine"
```

### mc.add / mc.remove / mc.list / mc.clear

Memory watchpoints.

| Argument | Type | Default |
|---|---|---|
| `start`, `end` | address | required, `end` > `start` |
| `on` | array of `read`, `write`, `change` | required |
| `result` | array of `break`, `log` | `["break"]` |
| `condition`, `description` | string | — |

`mc.list` entries carry `num_hits`, `last_pc`, `last_addr` and `last_size`. Asking to watch
writes returns a `note` — see [Limitations](#limitations).

```
pcsxroo mc add 0x1B1F038 0x1B1F03C --on write,change
```

### reg.list / reg.get / reg.set / reg.dump

All need a **paused** VM.

`reg.list` returns every category with its register names. On the EE these are `GPR` (which
includes `pc`, `hi` and `lo`, so 35 entries), `CP0`, `FPR`, `FCR`, `VU0f`, `VU0i` and `GS`.
The IOP has `GPR` only.

Names accept `a0`, `$a0`, `gpr:a0`, `cp0:Status`, plus the pseudo-registers `pc`, `hi` and
`lo`. Matching is case insensitive.

```json
{"name":"pc","value":"00000000000000000000000000081fc0","value_u64":532416,
 "string":"00000000000000000000000000081fc0"}
```

`value` is the full 128 bits as 32 hex characters; `value_u64` is the low 64 bits.
`reg.set` accepts up to 32 hex characters or a number and returns `before` and `after`.
`reg.dump` takes an optional `category` and returns `pc` plus every register in it.

### mem.read / mem.write / mem.fill / mem.dump

| Argument | Type | Default | Used by |
|---|---|---|---|
| `addr` | address | — | all |
| `size` | number | 4 (read) | read, fill, dump — max 16 MiB |
| `data` | hex or base64 | — | write |
| `pattern` | hex bytes | — | fill, repeated to length |
| `path` | string | — | dump, written server side |
| `format` | `hex` \| `base64` | `hex` | read, write |

Reads are allowed while running; that is what live counter sampling needs.

```json
{"addr":1048576,"addr_hex":"0x00100000","size":4,
 "before":"00000000","after":"deadbeef","verified":true}
```

**`mem.write` always reads back.** A value the game rewrites every frame is otherwise
indistinguishable from a successful write, so check `verified`.

### mem.search

| Argument | Type | Default |
|---|---|---|
| `start`, `end` | address | required for a new search |
| `session` | number | — continue a previous result set |
| `type` | `u8`…`u64`, `i8`…`i64`, `f32`, `f64` | `u32` |
| `comparison` | see below | `eq` |
| `value` | number | — required by every comparison except `unknown`, `increased`, `decreased`, `changed` and `not_changed` |
| `max_results` | number | 1000, max 100000 |

On the command line a value is `0x` hex, decimal, a negative number such as `-1`, or a real
number such as `1.5` for `f32` and `f64`.

Comparisons: `eq`, `ne`, `gt`, `gte`, `lt`, `lte`, `increased`, `increased_by`, `decreased`,
`decreased_by`, `changed`, `changed_by`, `not_changed`, `unknown`.

A first pass needs a range and returns a `session` id. Delta comparisons (`increased`,
`changed`, …) are valid only on a follow-up pass; on a first pass they are refused with a
reason rather than silently returning nothing. Start with `--unknown` to snapshot a range.

```json
{"session":1,"count":264,"truncated":false,
 "results":[{"addr":1048580,"addr_hex":"0x00100004","value":0,"value_number":0.0}]}
```

Float `eq` is exact equality, which is what a memory searcher means; an epsilon would match
unrelated addresses. Sessions are capped in count and size and dropped on VM shutdown.

### dis / asm

`dis` takes `addr`, `count` (default 16, max 1024) and `simplify` (default true).

```json
{"instructions":[{"addr":1048584,"addr_hex":"0x00100008","opcode":1006764053,
                  "opcode_hex":"0x3c020015","text":"lui\tv0, 0x0015"}]}
```

A word that is not a valid instruction renders as `?????`. A `symbol` field appears when the
address falls inside a known function.

`asm` takes `addr` and `instructions` (a string or an array of strings), assembles **all** of
them before writing **any**, and returns `words` and `before`. A line that fails to assemble
is `bad_args` and memory is untouched — a half-applied patch is worse than none.

```
pcsxroo asm 0x12BBD4 "nop"
```

### sym.lookup / sym.find

`sym.lookup` takes `addr` and returns `{"found":false}` or the nearest symbol with `name`,
`symbol_addr`, `size` and `offset`. `sym.find` takes `name` and returns its address and
size, or `bad_args` when there is no such symbol. Both need symbols to have been loaded;
without them `found` is simply false.

### stack / threads / modules

No arguments beyond `cpu`. `stack` needs a **paused** VM and walks the thread whose status
is `run`; it returns an empty list when there is no such thread, which is normal in a BIOS
idle loop.

```json
{"threads":[{"tid":0,"pc":532416,"pc_hex":"0x00081fc0","entry":532416,
             "priority":128,"status":"run","wait_id":0}]}
```

`status` is `run`, `ready`, `wait`, `suspend`, `wait_suspend`, `dormant` or `bad`.

### eval

`expression` (required), `cpu`. Returns `{"expression":...,"value":N,"value_hex":"0x…"}`.
The escape hatch for anything the command set does not model.

### input.list / input.press / input.set / input.release

| Argument | Type | Default | Used by |
|---|---|---|---|
| `pad` | 0–1 | 0 | all |
| `buttons` | array of names | — | press, set |
| `analog` | `{"left":{"x":..,"y":..},"right":{…}}` | — | press, set |
| `duration_frames` | 1–600 | 2 | press |

`input.list` reports the controller type and the names it accepts:

```json
{"pad":0,"controller":"DualShock2","buttons":["Up","Right","Down","Left","Triangle",
 "Circle","Cross","Square","Select","Start","L1","L2","R1","R2","L3","R3","Analog",
 "Pressure","LUp","LRight","LDown","LLeft","RUp","RRight","RDown","RLeft"]}
```

`press` taps and releases; `set` holds until changed. `set` with no buttons releases that
pad; `release` releases every pad. Analog components run -1..1 and are split across the
half-axis binds the pad exposes, exactly as a real stick reports. Names are matched case
insensitively.

Injection goes through the same entry point the real input sources use, so deadzone,
pressure and inversion settings all still apply. Held state is re-asserted every frame,
because the pad is repolled from the real controllers each frame and a single write would be
overwritten before the game saw it.

```
pcsxroo input press Start
pcsxroo input set Cross --left-stick 1.0,0.0
pcsxroo input release
```

### savestate / loadstate

Exactly one of `slot` (0–9) or `path`. `savestate` also takes `wait_flush` (default false).

Saving compresses on a worker thread, so the reply is
`{"queued":true,"flushed":false,"slot":9}` and the file may not be on disk yet. Pass
`--wait-flush` when you need it there before continuing.

### screenshot

`path` (required). Returns `{"queued":true,"path":...}`.

**Needs the VM running.** The GS only presents a frame while executing, so a screenshot
requested while paused sits in the queue and no file ever appears. Resume, wait a moment,
then capture, and poll for the file rather than assuming it is there.

A breakpoint that keeps firing counts as paused for this purpose: if one is armed somewhere
hot, the VM re-pauses before it can present, and the capture never lands. Clear or disable
it first.

### patch.reload

No arguments. Re-reads pnach files, which are otherwise only read at boot.

## Wire protocol

TCP, loopback only, one UTF-8 JSON object per line terminated by `\n`. Requests are capped
at 1 MiB. `protocol_version` is `1`; check it with `version`. Up to 8 concurrent clients.

```json
{"id": 7, "cmd": "bp.add", "args": {"cpu": "ee", "addr": 1227728, "condition": "a0 == 2"}}
{"id": 7, "ok": true,  "result": {"addr": 1227728, "addr_hex": "0x0012bbd0"}}
{"id": 7, "ok": false, "error": {"code": "not_paused", "message": "..."}}
```

`id` is echoed back so a client that pipelines requests can match replies. `args` may be
omitted entirely.

### Stop events

Every stop gets a monotonically increasing `seq`, and the same shape is used by `wait`,
`pause`, `step` and the event stream:

```json
{"seq":2,"reason":"step","cpu":"ee","pc":532420,"pc_hex":"0x00081fc4",
 "bp_addr":532420,"bp_addr_hex":"0x00081fc4",
 "mem_addr":0,"mem_addr_hex":"0x00000000","mem_size":0,"mem_write":false}
```

`reason` is `breakpoint`, `memcheck`, `step`, `user`, `entry`, `vm_shutdown` or `none`.
Pushed event lines additionally carry `"event":"stop"` and no `id`.

Pauses the breakpoint machinery performs internally — resetting the recompilers when a
breakpoint is added to a running game — are **not** reported as stops. Without that, every
`bp add` would produce a spurious `user` stop for a waiting client to latch onto.

## Errors

| Code | Meaning |
|---|---|
| `no_vm` | No VM is running |
| `not_paused` | This command needs a paused VM |
| `bad_address` | The address did not resolve, or the range is unreadable |
| `bad_args` | Missing or invalid argument |
| `unknown_command` | No such command |
| `timeout` | The CPU thread did not respond, or a wait expired |
| `parse_error` | The request line was not valid JSON |
| `io_error` | A file could not be read or written |
| `unsupported` | A valid request the emulator cannot satisfy right now |

A malformed line is answered with `parse_error` and the connection survives.

## Limitations

- **Write memchecks do not see every write.** `Breakpoints.h` records that memchecks are
  "not used in the interpreter or HLE currently", and writes that bypass cached EE stores
  never reach them. A memcheck that never fires may be pointing at a perfectly correct
  address. `mc.add` says so in a `note` whenever you ask it to watch writes.
- **Registers need a paused VM.** There is no consistent way to sample the register file of
  a running recompiled CPU, so the server refuses rather than returning a torn value.
- **`status` reports a stale PC while running.** The recompiler only writes the program
  counter back at certain points. Pause first if the PC matters.
- **Screenshots need a running VM**, as above.
- **Savestates and screenshots are asynchronous** and reply `queued`.
- **PCSXROO must be built Release.** A Devel build cannot boot a VM at all — it fails hard
  just after the game database loads and before the GS opens. `build.cmd` defaults to
  Release; `PCSXROO_BUILD_TYPE=Devel` remains for core work that never boots a game.
- **The server has no authentication.** It grants unrestricted memory access and process
  control. It binds loopback only and defaults to off; never expose it to a network.
- **`mem.dump` writes files with the emulator's privileges**, server side.
