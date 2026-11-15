#!/usr/bin/env python3
"""
scenery_text.py - give protos their own name and examine text, from
mod/ids/scenery_text.tsv (scenery on the Redwell maps) and
mod/ids/proto_text.tsv (items, critters and scenery everywhere else).

    python tools/scenery_text.py --dry-run
    python tools/scenery_text.py             # write protos + pro_*.msg, deploy, validate

A proto's name is message <id> and its examine text <id>+1 in its type's
pro_*.msg (proto.cc protoGetMessage). Vanilla protos share ids ("Plant" / "A
mutated cornstalk." covers every corn stalk AND the marijuana bushes), so
changing a vanilla line in place would change unrelated objects. Instead each
listed proto is pointed at a mod entry: its name is allocated in
mod/ids/msg_<type>.txt (idempotent by name), its description is set for that
name in msg_<type>_desc.txt (replacing any earlier one), and the proto's
message id at offset 4 is rewritten. A vanilla-range proto gets a foc-layer
override copy in mod/data/proto/<type>, like the item overrides we ship.
The type (items, critters, scenery) comes from the pid.

Descriptions are keyed by name within a type, so rows of one type sharing a
name must share the text; the tool refuses a table that disagrees with itself.
"""

from __future__ import annotations

import argparse
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import port  # noqa: E402
from foclib import GameIndex  # noqa: E402

ROOT, MOD, IDS = port.ROOT, port.MOD, port.IDS
TABLES = [IDS / "scenery_text.tsv", IDS / "proto_text.tsv"]
TYPE_DIR = {0: "items", 1: "critters", 2: "scenery"}


def read_table() -> list[tuple[int, str, str]]:
    rows = []
    for table in TABLES:
        if not table.exists():
            continue
        for n, line in enumerate(table.read_text(encoding="utf-8").splitlines(), 1):
            if not line.strip() or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) < 3:
                sys.exit(f"{table.name}:{n}: need pid<TAB>name<TAB>description")
            pid = int(parts[0], 0)
            if pid >> 24 not in TYPE_DIR:
                sys.exit(f"{table.name}:{n}: 0x{pid:08X} is not an item, critter or scenery pid")
            rows.append((pid, parts[1].strip(), parts[2].strip()))
    descs: dict[tuple[int, str], str] = {}
    for pid, name, desc in rows:
        if descs.setdefault((pid >> 24, name.lower()), desc) != desc:
            sys.exit(f"{TYPE_DIR[pid >> 24]} {name!r} has two different descriptions")
    return rows


def set_desc(art_dir: str, name: str, desc: str) -> None:
    """Set (replace) the examine text for a name in msg_<type>_desc.txt."""
    p = port.msg_desc_path(art_dir)
    lines = p.read_text(encoding="utf-8").splitlines() if p.exists() else []
    out, done = [], False
    for l in lines:
        if "\t" in l and not l.startswith("#") and l.split("\t", 1)[0].strip().lower() == name.lower():
            if not done:
                out.append(f"{name}\t{desc}")
                done = True
            continue
        out.append(l)
    if not done:
        out.append(f"{name}\t{desc}")
    p.write_text("\n".join(out) + "\n", encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--no-deploy", action="store_true")
    args = ap.parse_args()

    rows = read_table()
    base = GameIndex()                        # pristine: vanilla/RPU protos
    full = GameIndex(pristine=False)
    touched = set()
    for pid, name, desc in rows:
        art_dir = TYPE_DIR[pid >> 24]
        out_dir = MOD / "data" / "proto" / art_dir
        sp = full.proto_path(pid)
        if sp is None:
            sys.exit(f"0x{pid:08X}: no such {art_dir} proto")
        fname = sp.replace("/", "\\").split("\\")[-1]
        dst = out_dir / fname
        data = bytearray(dst.read_bytes() if dst.exists() else base.vfs.read(sp))
        old = struct.unpack_from(">I", data, 4)[0]
        if args.dry_run:
            print(f"  0x{pid:08X} {art_dir}/{fname}  msg {old} -> '{name}'" + ("" if dst.exists() else "  (new override)"))
            continue
        reg = port.msg_registry_path(art_dir)
        idx = port.allocate(reg, name, 0)
        # allocate matches names case-insensitively: an older "corn" would
        # otherwise keep its lowercase spelling for our "Corn"
        entries = port.registry_entries(reg)
        if entries[idx] != name:
            lines = port.registry_lines(reg)
            k = [i for i, l in enumerate(lines) if not l.strip().startswith("#")][idx]
            lines[k] = name
            reg.write_text("\n".join(lines) + "\n", encoding="utf-8")
        set_desc(art_dir, name, desc)
        struct.pack_into(">I", data, 4, port.msg_id_for_index(idx))
        out_dir.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(bytes(data))
        touched.add(art_dir)
    if args.dry_run:
        print(f"{len(rows)} protos (dry run)")
        return 0
    for art_dir in sorted(touched):
        port.write_merged_msg(art_dir)
    print(f"{len(rows)} protos given their own name and examine text")
    if args.no_deploy:
        return 0
    for t in ("deploy.py", "validate.py"):
        rc = subprocess.run([sys.executable, str(ROOT / "tools" / t)], cwd=ROOT).returncode
        if rc:
            return rc
    return 0


if __name__ == "__main__":
    sys.exit(main())
