"""Paths and game identity for the PS2 analysis tooling.

Everything is discovered from the environment or a local override file, so the
tools stay portable across machines and games. Nothing machine-specific or
game-specific is hardcoded here; per-game constants live in a
:class:`GameIdentity` that the consumer binds before using anything that needs
them (game ini paths, save-state discovery, text bounds, the safe zone).

The identity is bound one of two ways:

- automatically, from a ``GAME`` block in ``local.json`` next to the tools,
- programmatically, with ``config.bind(...)`` before any tool runs.

Either way the values themselves live with the consumer, not in this package:

    {"PCSX2_DIR": "D:/emu/PCSX2",
     "SCRATCH_DIR": "D:/scratch/ps2",
     "GAME": {"serial": "SLUS-21678", "crc": "428113C2",
              "elf_name": "SLUS_216.78", "game": "Dragon Ball Z: Budokai Tenkaichi 3",
              "text_base": 1048576, "text_end": 2896832, "data_base": 2896896,
              "bss_end": 3361784, "gp_base": 3162736,
              "safe_zone": 983040, "safe_zone_size": 32768}}
"""

from __future__ import annotations

import json
import os
from dataclasses import dataclass
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
PCSXROO = Path(__file__).resolve().parents[1]    # pcsxroo/ - local.json and work/ live here
TOOLS = PCSXROO                                   # kept for callers that predate the move
LOCAL_JSON = TOOLS / "local.json"

EE_RAM_SIZE = 32 * 1024 * 1024          # the PS2 spec, not a per-game value


@dataclass
class GameIdentity:
    """Everything this package needs to know about the target game."""

    serial: str
    crc: str
    elf_name: str
    game: str
    text_base: int = 0
    text_end: int = 0
    data_base: int = 0
    bss_end: int = 0
    gp_base: int = 0
    safe_zone: int = 0
    safe_zone_size: int = 0


# --- local overrides -------------------------------------------------------

_local_cache: tuple[Path, dict] | None = None


def _local() -> dict:
    global _local_cache
    if _local_cache is None or _local_cache[0] != LOCAL_JSON:
        _local_cache = (
            LOCAL_JSON,
            json.loads(LOCAL_JSON.read_text()) if LOCAL_JSON.exists() else {},
        )
    return _local_cache[1]


def _setting(name: str, default=None):
    if name in os.environ:
        return os.environ[name]
    if name in _local():
        return _local()[name]
    return default


def _identity_from_local() -> GameIdentity | None:
    game = _local().get("GAME")
    return GameIdentity(**game) if isinstance(game, dict) else None


identity: GameIdentity | None = _identity_from_local()


def bind(ident: GameIdentity) -> None:
    """Bind the game identity programmatically (before any tool runs)."""
    global identity
    identity = ident


def require_identity() -> GameIdentity:
    """The bound identity, or a clear failure when none was bound."""
    if identity is None:
        raise RuntimeError(
            f"no game identity bound - copy {LOCAL_JSON.with_name('local.json.example')} to {LOCAL_JSON.name} "
            "and fill in its \"GAME\" block, or call config.bind(config.GameIdentity(...)) first"
        )
    return identity


SCRATCH_DIR = Path(_setting("SCRATCH_DIR", str(TOOLS / "work")))

# --- emulator discovery ----------------------------------------------------

def _pcsx2_dir_candidates() -> list[Path]:
    """Where PCSX2 keeps its data on each platform. An unset variable adds nothing: joined
    onto an empty string it would become a path relative to wherever the tool was run."""
    candidates = []
    appdata = os.environ.get("APPDATA")
    if appdata:
        candidates += [Path(appdata) / "EmuDeck" / "Emulators" / "PCSX2-Qt", Path(appdata) / "PCSX2"]
    profile = os.environ.get("USERPROFILE")
    if profile:
        candidates.append(Path(profile) / "Documents" / "PCSX2")
    home = Path.home()
    candidates.append(Path(os.environ.get("XDG_CONFIG_HOME") or home / ".config") / "PCSX2")
    candidates.append(home / "Library" / "Application Support" / "PCSX2")
    return candidates


_CANDIDATE_PCSX2_DIRS = _pcsx2_dir_candidates()


def pcsx2_dir() -> Path:
    """Root of the PCSX2 install (the one holding inis/, cheats/, sstates/)."""
    explicit = _setting("PCSX2_DIR")
    if explicit:
        return Path(explicit)
    for candidate in _CANDIDATE_PCSX2_DIRS:
        if (candidate / "inis" / "PCSX2.ini").exists():
            return candidate
    raise FileNotFoundError(
        "Could not locate a PCSX2 install. Set PCSX2_DIR in the environment "
        "or in local.json."
    )


def cheats_dir() -> Path:
    return pcsx2_dir() / "cheats"


def sstates_dir() -> Path:
    return pcsx2_dir() / "sstates"


def game_ini() -> Path:
    i = require_identity()
    return pcsx2_dir() / "gamesettings" / f"{i.serial}_{i.crc}.ini"


def global_ini() -> Path:
    return pcsx2_dir() / "inis" / "PCSX2.ini"


def elf_path() -> Path:
    """Where extract-elf drops the extracted boot ELF."""
    return SCRATCH_DIR / require_identity().elf_name


def latest_state(slot: int | None = None) -> Path:
    """Most recent save state for the bound game, optionally pinned to a slot."""
    i = require_identity()
    pattern = (
        f"{i.serial} ({i.crc}).{slot:02d}.p2s" if slot is not None
        else f"{i.serial} ({i.crc}).*.p2s"
    )
    hits = [p for p in sstates_dir().glob(pattern) if p.suffix == ".p2s"]
    if not hits:
        raise FileNotFoundError(f"No save state matching {pattern}")
    return max(hits, key=lambda p: p.stat().st_mtime)


# --- PCSXROO ---------------------------------------------------------------
# The fork that exposes the debugger over a socket. It runs in portable mode,
# so its cheats/, sstates/ and snaps/ live next to the executable and are
# entirely separate from the PCSX2 install above.

_CANDIDATE_PCSXROO_DIRS = [
    REPO / "bin",
    REPO.parent / "pcsxroo" / "bin",
    REPO.parent / "PCSXROO" / "bin",
]


def pcsxroo_dir() -> Path:
    explicit = _setting("PCSXROO_DIR")
    if explicit:
        return Path(explicit)
    for candidate in _CANDIDATE_PCSXROO_DIRS:
        if (candidate / "pcsxroo.exe").is_file() or (candidate / "pcsxroo").is_file():
            return candidate
    raise FileNotFoundError(
        "Could not locate PCSXROO. Set PCSXROO_DIR in the environment or in "
        "local.json."
    )


def roo_cheat_file() -> Path:
    return pcsxroo_dir() / "cheats" / f"{require_identity().crc}.pnach"


def roo_snaps_dir() -> Path:
    return pcsxroo_dir() / "snaps"


def roo_game_ini() -> Path:
    """PCSXROO's own per-game settings, which is NOT game_ini().

    game_ini() points at the user's installed PCSX2. PCSXROO is a separate,
    portable build and keeps its per-game settings under its data root, not
    under inis/. The [Cheats] Enable list there is read at boot and is what
    decides whether a pnach group applies at all - a group missing from it is
    silently ignored, however correct the pnach is.
    """
    i = require_identity()
    return pcsxroo_dir() / "gamesettings" / f"{i.serial}_{i.crc}.ini"


def roo_enabled_cheats() -> list[str]:
    """The group names PCSXROO will honour, as of its last boot."""
    path = roo_game_ini()
    if not path.exists():
        return []
    names, in_cheats = [], False
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            in_cheats = stripped.lower() == "[cheats]"
            continue
        if in_cheats and stripped.lower().startswith("enable"):
            names.append(stripped.split("=", 1)[1].strip())
    return names
