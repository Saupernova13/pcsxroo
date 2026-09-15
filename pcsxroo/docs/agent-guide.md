# PCSXROO agent guide

You are driving a PlayStation 2 emulator and its debugger from a command line. Nothing here
needs a mouse, a window, or a person watching.

The full command list is in [cli.md](cli.md). This file is what to actually do, in what
order, and what will bite you.

## The shape of a session

```
pcsxroo launch                                  # emulator up, no VM yet
pcsxroo boot "G:\roms\ps2\game.iso"            # boot takes tens of seconds; the CLI waits
pcsxroo input press Start                       # get past the splash screens
pcsxroo pause                                   # now registers and the stack are readable
pcsxroo reg get a0
pcsxroo resume
pcsxroo shutdown
```

`launch` starts the emulator; `boot` starts a VM inside it. They are separate because they
fail for different reasons, and an agent that cannot tell "the emulator never started" from
"this game will not boot" is stuck. `launch` with no game leaves you with a live server and
no VM, which is a fine place to be.

Check exit codes rather than parsing text:

| Code | What to do |
|---|---|
| 0 | Continue. |
| 1 | The server said no. Read `error.code` — it is actionable. |
| 2 | You typed the command wrong - an unknown option or an extra argument counts. Fix the command, do not retry. |
| 3 | The emulator is not running, or exited mid-request. `launch` it. |
| 4 | Timed out. For `wait`, that usually means "not yet", not "broken". |

Add `--json` when you need to parse a reply. The human rendering is for logs.

## Six things that will waste your time

**1. Always pass `--since` to `wait`.**

```
seq=$(pcsxroo --json status | jq .result.last_stop.seq)
pcsxroo bp add 0x12BBD0
pcsxroo run
pcsxroo wait --since "$seq"
```

A breakpoint can fire between your `run` and your `wait`. With `--since`, a stop that
already happened is returned immediately; without it you block waiting for the *next* one,
which may never come. This is the single most common way an unattended session hangs.

**2. Registers and the stack need a paused VM.** `reg get`, `reg set`, `reg dump`, `stack`,
`step` and `run-to` all return `not_paused` otherwise. There is no consistent way to read
the register file of a running recompiled CPU, so the server refuses rather than handing
you a torn value.

**3. `status` reports a stale PC while running.** The recompiler only writes the program
counter back at certain points, so the value you see mid-execution is a hint at best.
`pause` first if the PC matters.

**4. Screenshots need the VM *running*.** The GS only presents a frame while executing. A
screenshot requested while paused sits in the queue and no file appears. Resume, wait a
second, then capture — and remember the reply is `queued`, so poll for the file.

A breakpoint that keeps firing counts as paused here. If one is armed somewhere hot the VM
re-pauses before it can present a frame, and your capture silently never arrives. Clear it
before you look at the screen.

**5. Registers in expressions have no `$`, and numbers in them are hex.** `--cond "a0 == 2"`
works; `--cond "$a0 == 2"` is rejected with `Invalid operator`. The `$` form is accepted only
by `reg get` / `reg set`, which take a name rather than an expression. Equality is `==`,
never `=`.

The number trap is quieter: the debugger's parser reads every bare number as hex, so
`--cond "t5 == 28170704"` means `t5 == 0x28170704`. Nothing complains - the breakpoint is
armed and simply never fires, which looks exactly like code that never runs. Write `0x`
on every number in a condition, `eval` or address expression. A plain all-digit address
such as `bp add 1227728` is the one place a bare number is decimal.

**6. A write memcheck that never fires may be aimed at a perfectly correct address.**
PCSX2's memchecks do not observe every write path — `mc add` tells you so in a `note`.
If you are hunting something that changes and the watchpoint stays silent, use
`mem search` instead of concluding the address is wrong.

## Recipes

### Get a game to a state you can debug

Games open on unskippable logos. Drive through them:

```
pcsxroo boot "G:\roms\ps2\game.iso"
for i in 1 2 3 4 5 6; do sleep 5; pcsxroo input press Start; done
pcsxroo screenshot work/where-am-i.png    # look before you act
```

`input list` tells you the exact button names the configured controller accepts. `press`
taps for two frames; `set` holds until you change it; `set` with no buttons releases the
pad.

```
pcsxroo input set Cross --left-stick 1.0,0.0   # hold Cross, stick fully right
pcsxroo input release
```

### Catch a routine and read its arguments

```
pcsxroo bp add 0x12BBD0 --cond "a0 == 2" --desc "per-frame routine, 30fps stride"
seq=$(pcsxroo --json status | jq .result.last_stop.seq)
pcsxroo run
pcsxroo --timeout 60000 wait --since "$seq"
pcsxroo reg dump --category GPR
pcsxroo dis 0x12BBD0 8
pcsxroo stack
```

A condition that will not parse aborts the whole `bp add`, so you never end up with an
unconditional breakpoint you did not ask for.

### Find a value that changes (replaces the GUI memory searcher)

Snapshot, then narrow. Delta comparisons only work on a follow-up pass.

```
pcsxroo mem search --range 0x100000:0x2000000 --unknown     # -> session 1
pcsxroo mem search --session 1 --increased-by 1             # ticked once
pcsxroo mem search --session 1 --increased-by 1             # and again
```

Add `--type f32` for floats, `--max N` to cap results.

### Patch an instruction live and see what changes

```
pcsxroo mem read 0x12BBD4 4                # keep the original
pcsxroo asm 0x12BBD4 "nop"
pcsxroo screenshot work/after.png
```

`asm` assembles every line before writing any, so a bad instruction leaves memory alone.
`mem write` reads back and reports `verified` — a value the game rewrites every frame looks
identical to a successful write otherwise, so always check it.

### Watch unattended without polling

```
pcsxroo events            # streams stop events until interrupted
```

Better than a polling loop for a long watch: each stop arrives as one JSON line, flushed
immediately.

### Save a state you can return to

```
pcsxroo savestate --slot 3 --wait-flush     # --wait-flush = actually on disk
pcsxroo loadstate --slot 3
```

Without `--wait-flush` the reply is `queued` and compression is still running.

## Choosing a stopping tool

| You want | Use |
|---|---|
| Stop at an address, repeatedly | `bp add` |
| Stop there once | `bp add --temporary`, or `run-to` |
| Stop only when a condition holds | `bp add --cond "a0 == 2"` |
| Stop when memory is touched | `mc add --on read,write,change` (read the caveat) |
| Advance one instruction | `step into` / `over` / `out` |
| Advance whole frames | `frame-advance N` |
| Find an address in the first place | `mem search` |

## When something looks broken

- **Everything times out (exit 4) after a while.** Check the emulator is alive at all with
  `pcsxroo version`, which needs no VM. If that answers, the CPU thread is busy or stopped;
  if it does not, the process is gone.
- **`no_vm` when a game is clearly running.** You are talking to a different emulator — check
  `--port`. Two instances can run at once on different ports.
- **A breakpoint never fires.** Confirm it is listed with `bp list`, that the code actually
  executes (put one somewhere you know runs and see it hit), and that you passed `--since`.
- **The emulator will not boot a game.** PCSXROO must be a Release build; a Devel build
  cannot boot at all. `pcsxroo\tools\build.cmd` defaults to Release.
- **Fresh install behaves oddly.** Run `pcsxroo\tools\seed-portable.ps1`. Without a BIOS the
  emulator stops at its setup wizard, before the server ever starts.

`pcsxroo\tools\smoke-test.ps1 -Bios` checks the whole path end to end and is the quickest
way to tell whether the problem is your commands or the build.

## Ground rules

- The debug server has **no authentication** and grants full memory and process control. It
  binds loopback only and is off by default. Do not expose it.
- PCSXROO keeps its data in its own folder (portable mode). It does not read or write the
  settings of the PCSX2 install it was seeded from.
- `mem.dump` and `screenshot` write files **on the emulator's side**, with its privileges.
- This is a fork of PCSX2. Per the upstream `AGENTS.md`, never open issues or pull requests
  against the PCSX2 project from here.
