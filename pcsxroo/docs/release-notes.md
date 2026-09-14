PCSXROO is a fork of PCSX2 that exposes the debugger over a loopback JSON server and the
`pcsxroo` command line client. It is a debugging rig, not a PCSX2 to play games on. Setup,
the command reference and the agent guide are in
[`pcsxroo/README.md`](https://github.com/Saupernova13/pcsxroo/blob/master/pcsxroo/README.md).

## Downloads

| File | What it is |
|---|---|
| `PCSXROO-<version>-windows-x64.zip` | The emulator (`pcsxroo-qt.exe`) and the CLI (`pcsxroo.exe`). Extract anywhere and run from that folder. It is portable: settings, BIOS and memory cards stay inside the folder. |
| `PCSXROO-<version>-linux-x64.AppImage` | The emulator. `chmod +x` it and run it. |
| `PCSXROO-<version>-macos-x64.tar.xz` | The emulator as an app bundle, for Intel and Apple Silicon Macs. |

## Before you start

- **You need a PS2 BIOS dumped from your own console.** None is included.
- **Leave RetroAchievements hardcore mode off.** It disables the debug server.
- **Start it with the debug server on:** `pcsxroo.exe launch` on Windows, or run the emulator
  with `-debugserver 28110`. The server binds `127.0.0.1` only and has no authentication -
  never expose it to a network.
- **The CLI ships in the Windows download only.** On Linux and macOS, build it from source
  (`pcsxroo/README.md`, section 8) or drive the server directly over its JSON protocol
  (`pcsxroo/docs/cli.md`).
- **On Linux and macOS PCSXROO shares its settings folder with an installed PCSX2**, since
  neither an AppImage nor an app bundle can run in portable mode. Back your PCSX2 settings up
  before running both.
- **The macOS app is not signed.** macOS will refuse to open it at first: right-click the app
  and choose Open, or run `xattr -dr com.apple.quarantine` on it.
- Updates are never downloaded automatically: PCSX2's updater stays off in PCSXROO builds.

PCSXROO is not supported by the PCSX2 team; do not report its problems to them. GPL-3.0, as
PCSX2.
