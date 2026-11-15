"""
safety.py - two guards for tools that write the game's maps.

    from foclib.safety import backup_map, spent

backup_map(path)
    Call it right before overwriting a map. If `path` is one of the game's
    maps (mod/data/maps or base/mods/foc/maps, not a build or preview folder)
    and the file exists, a copy goes to _safety_backups/<tool>/<name>.<time>
    first. Returns the copy's path, or None when nothing needed keeping.

spent(reason)
    First line of main() in a one-off script whose job is done. The map it
    changed has been worked on since, by hand or by other tools, so a second
    run would undo or double that work. It stops with `reason` unless the
    command line has --again (which it takes off the command line).
"""

from __future__ import annotations

import shutil
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
BACKUPS = ROOT / "_safety_backups"
GAME_MAP_DIRS = (ROOT / "mod" / "data" / "maps", ROOT / "base" / "mods" / "foc" / "maps")


def is_game_map(path: Path) -> bool:
    """`path` is in one of the folders the game's own maps live in."""
    try:
        parent = Path(path).resolve().parent
    except OSError:
        return False
    return any(parent == d.resolve() for d in GAME_MAP_DIRS)


def backup_map(path: Path, tool: str | None = None) -> Path | None:
    path = Path(path)
    if not path.is_file() or not is_game_map(path):
        return None
    tool = tool or Path(sys.argv[0]).stem or "tools"
    # the two folders hold the same names: say which one this copy came from
    where = "deployed" if "base" in path.resolve().parts[-5:-2] else "authored"
    dest = BACKUPS / tool / f"{path.name}.{where}.{time.strftime('%Y%m%d_%H%M%S')}"
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(path, dest)
    return dest


def spent(reason: str) -> None:
    if "--again" in sys.argv:
        sys.argv.remove("--again")
        print("--again: running a one-off script a second time")
        return
    sys.exit(f"{Path(sys.argv[0]).name} has done its job and is kept for the record: {reason}\n"
             "Running it again would redo that work on a map that has changed since. "
             "If that is really wanted, add --again.")
