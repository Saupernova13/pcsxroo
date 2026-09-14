# PCSXROO

A fork of [PCSX2](https://github.com/PCSX2/pcsx2) that exposes the debugger over a loopback JSON
server and a `pcsxroo` command line client, so an agent or a script can run a whole PS2 debugging
session - boot a game, drive the pad, set breakpoints, read memory and registers, take screenshots -
without touching the user interface. It is a debugging rig, not a PCSX2 for playing games.

**Start here: [`pcsxroo/README.md`](../pcsxroo/README.md)** - prerequisites, building, first launch,
the command line and the Python tooling.

Everything PCSXROO adds lives under [`pcsxroo/`](../pcsxroo). The rest of this repository is PCSX2,
and every place the fork edits an upstream file carries a `PCSXROO:` comment. PCSX2's own README is
[`README.md`](../README.md).

PCSXROO was built to develop the
[Budokai Tenkaichi 3 60fps patch](https://github.com/Saupernova13/pcsx2-bt3-60fps), whose tools drive
it through the `ps2ee` library in `pcsxroo/ps2ee/`.
