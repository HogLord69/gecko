#!/usr/bin/env python3
"""
compose_map.py - build a map from a recipe (mod/maps_src/<name>.compose.json)
by lifting whole levels or boxes out of other games' maps, converting their
ids into the mod's (port_stamp.Converter), and adding the mod's own exits.

    python tools/compose_map.py mod/maps_src/rwfac1.compose.json --preview
    python tools/compose_map.py mod/maps_src/rwfac1.compose.json --write [--force]
    python tools/compose_map.py mod/maps_src/mdbank.compose.json --out build/midland   # then tools/install_map.py

Each mod elevation is built from one or more `lift`s. A lift copies the
floor, the roof and the scenery and walls of a box of a source map (all of
it when no rect is given), shifted by `shift` squares. What never comes
across: critters (the mod's scripts put people in), misc objects (the source's
exit grids and scroll blockers), loose items (the source's loot), and stairs,
ladders and elevators (they carry the source game's map links). Containers
come across empty. `drop` lists art-name regexes to leave out (logos,
signs, things that name the source game's places).

Recipe:
  {
    "name": "RWFAC1", "id": 214, "script": "SCRIPT_RWFAC1",
    "port_tiles": false,      (optional: take no new tiles.lst lines; tiles already ported are used)
    "start": {"hex": [x, y], "elev": 0, "rot": 2},
    "levels": [
      {"elev": 0,
       "lift": [{"source": "olympus", "map": "lvfilter", "elev": 0,
                 "rect": [x0, y0, x1, y1], "shift": [dx, dy]}],
       "drop": ["^logo"],
       "exits": [{"hexes": [x0, y0, x1, y1], "to": 200, "tile": 12345,
                  "elev": 0, "rot": 2}]}
    ]
  }

Squares are 0..99 (a hex is half a square each way). `to` is a maps.txt id,
-2 the world map. The written map goes to mod/data/maps and
base/mods/foc/maps; ported art lands above the 4096 cap, so run
tools/repack_art.py after, then map_fixup.py (flags) and the blocker tools.
"""

from __future__ import annotations

import argparse
import json
import random
import re
import shutil
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import port  # noqa: E402
from foclib import GameIndex, MapFile, parse_lst  # noqa: E402
from foclib.formats import hex_xy, pid_type  # noqa: E402
from mapbuild import HEX_W, MapBuilder, MapObjectSpec, extras_for  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
AUTHORED = ROOT / "mod" / "data" / "maps"
DEPLOYED = ROOT / "base" / "mods" / "foc" / "maps"
IDS_H = ROOT / "scripts_src" / "foc" / "foc_ids.h"
EXIT_GRID = 0x05000010
WORLD_EXIT_FID = 0x05000021         # ext2grd1, the brown twin of the exit grid's own picture (tools/exit_fix.py --colours)
BLOCKING_HEX = 0x02000043                  # scenery 67, Secret Blocking Hex
SCROLL_BLOCKER = 0x0500000C
from foclib import config as _config  # noqa: E402
STAMPS = _config.stamps_dir()
DEFAULT_ROCKS = [0x0200004A, 0x0200004B, 0x0200004E, 0x0200004F, 0x02000050, 0x02000051,
                 0x020001AC, 0x020001AD, 0x020001AE, 0x020001AF, 0x02000116, 0x02000117]
DIRS = {0: (-1, HEX_W - 1, HEX_W, HEX_W + 1, 1, -HEX_W),
        1: (-HEX_W - 1, -1, HEX_W, 1, 1 - HEX_W, -HEX_W)}
LINKED_SCENERY = (1, 2, 3, 4)          # stairs, elevator, ladder up, ladder down


def script_number(name: str) -> int:
    m = re.search(rf"#define\s+{re.escape(name)}\s+\((\d+)\)", IDS_H.read_text(encoding="utf-8"))
    if not m:
        sys.exit(f"{name} is not in foc_ids.h (register the script and run gen_ids.py)")
    return int(m.group(1))


class Composer:
    def __init__(self, recipe: dict, dry_run: bool):
        self.r = recipe
        self.dry = dry_run
        self.gi = GameIndex(pristine=False)
        levels = [lv["elev"] for lv in recipe["levels"]]
        st = recipe.get("start", {})
        sx, sy = st.get("hex", [100, 100])
        script = script_number(recipe["script"]) if recipe.get("script") else -1
        self.mb = MapBuilder(recipe["name"], elevations=tuple(levels),
                             start_tile=sy * HEX_W + sx, start_elevation=st.get("elev", levels[0]),
                             start_orientation=st.get("rot", 0), map_id=recipe.get("id", -1),
                             map_script_index=script)
        self.converters: dict[str, object] = {}
        self.stats: dict[str, int] = {}
        self._matcher = None

    def matcher(self):
        """port_stamp.TileMatcher: the closest-looking mod tile for a source tile
        the mod lacks (tiles cannot be ported past gecko's 12-bit tile ids)."""
        if self._matcher is None:
            from port_stamp import Foc, TileMatcher
            self._matcher = TileMatcher(Foc())
        return self._matcher

    def converter(self, source: str):
        if source not in self.converters:
            from port_stamp import Converter, Foc
            src = port.open_source(source)
            conv = Converter(Foc(), source, src, self.dry, self.r["name"].lower())
            if self.r.get("port_tiles") is False:
                # the recipe's own say: no new tiles.lst lines for it; a source tile already ported
                # (tile_slots.py) is used, any other takes the nearest-looking mod tile
                conv._spare = []
            self.converters[source] = (conv, src)
        return self.converters[source]

    def convert_pid(self, source: str, pid: int):
        """A source game's pid -> (mod pid, mod fid), or None. Fallout 2's
        own pids are the mod's already."""
        key = (source, pid)
        if not hasattr(self, "_pidcache"):
            self._pidcache = {}
        if key in self._pidcache:
            return self._pidcache[key]
        out = None
        if source == "fo2":
            pr = self.gi.proto(pid)
            out = (pid, pr.fid) if pr is not None else None
        else:
            conv, src = self.converter(source)
            spr = src.proto(pid)
            if spr is not None:
                np = conv.map_pid(pid)
                nf = conv.map_frm(pid, spr.fid, np) if np else None
                out = (np, nf) if np and nf is not None else None
        self._pidcache[key] = out
        return out

    def floor_tiles(self, level: dict):
        """Tiles solved by tools/tilewfc.py: "floor_tiles": [[sx, sy, tile id], ...]."""
        for sx, sy, t in level.get("floor_tiles", []):
            self.mb.set_floor(level["elev"], sx, sy, t)
        if level.get("floor_tiles"):
            print(f"  floor tiles: {len(level['floor_tiles'])} squares (tile grammar)")

    def grown_ground(self, level: dict):
        """Ground grown square by square from Fallout 2's own tile rules
        (tools/groundkit.py): {"rect": [sx0, sy0, sx1, sy1], "classes":
        [{"class": "road"|"walk"|"edge"|"dirt", "rect": [...]}, ...],
        "decay": {"crumble": 0.18, "drift": 0.12}, "palette": {class: [tile names]},
        "seed": n}. Squares not named are dirt."""
        gg = level.get("grown_ground")
        if not gg:
            return
        import random
        import groundkit
        rng = random.Random(gg.get("seed", self.r.get("seed", 1)))
        x0, y0, x1, y1 = gg["rect"]
        target = {(x, y): "dirt" for y in range(y0, y1 + 1) for x in range(x0, x1 + 1)}
        for c in gg.get("classes", []):
            cx0, cy0, cx1, cy1 = c["rect"]
            for y in range(cy0, cy1 + 1):
                for x in range(cx0, cx1 + 1):
                    if (x, y) in target:
                        target[(x, y)] = c["class"]
        if gg.get("paint"):
            from foclib import parse_lst
            if not hasattr(self, "_tiles_lst"):
                self._tiles_lst = [x.split(",")[0].lower() for x in parse_lst(self.gi.vfs.read("art\\tiles\\tiles.lst"))]
            tl = self._tiles_lst
            lines = set()
            for ln in gg.get("lines", []):
                lx0, ly0, lx1, ly1 = ln
                lines |= {(x, y) for y in range(ly0, ly1 + 1) for x in range(lx0, lx1 + 1)}
            names = groundkit.paint(target, rng, gg.get("wear", 0.35), gg.get("style", "ruin_town"), lines)
            for (x, y), nm in names.items():
                if nm.lower() in tl:
                    self.mb.set_floor(level["elev"], x, y, tl.index(nm.lower()))
            print(f"  painted ground: {len(names)} squares")
            return
        if gg.get("decay", True):
            d = gg.get("decay") if isinstance(gg.get("decay"), dict) else {}
            target = groundkit.decay_target(target, rng, d.get("crumble", 0.18), d.get("drift", 0.12))
        target = groundkit.edge_ring(target)
        pal = None
        if gg.get("palette"):
            G = groundkit.g(gg.get("style", "ruin_town"))
            byname = {n.lower(): t for t, n in G["names"].items()}
            pal = {c: [byname[n.lower()] for n in names if n.lower() in byname] for c, names in gg["palette"].items()}
        tiles = groundkit.grow(target, rng, pal, (x0, y0, x1, y1), gg.get("style", "ruin_town"))
        for (x, y), t in tiles.items():
            self.mb.set_floor(level["elev"], x, y, t)
        print(f"  grown ground: {len(tiles)} squares")

    def lines(self, level: dict):
        """Fences and walls along a line: "lines": [{"cells": [[hx, hy], ...],
        "family": "fence", "source": "fo2", "seed": n}]."""
        import random
        import wallkit
        for i, ln in enumerate(level.get("lines", [])):
            rng = random.Random(ln.get("seed", i))
            doors = [tuple(d) for d in ln.get("doors", [])]
            cells = {tuple(c) for c in ln["cells"]} | set(doors)
            n = wallkit.draw(self.add, self.convert_pid, ln["family"], cells, level["elev"], rng, doors,
                             ln.get("door_pref"), ln.get("source", "fo2"), None)
            print(f"  line {ln.get('name', i)} ({ln['family']}): {n} pieces")

    def floor_fill(self, level: dict):
        """Plain floors: "floor_fill": [{"rect": [sx0, sy0, sx1, sy1], "tiles": [names]}]."""
        import random
        from foclib import parse_lst
        if not level.get("floor_fill"):
            return
        if not hasattr(self, "_tiles_lst"):
            self._tiles_lst = [x.split(",")[0].lower() for x in parse_lst(self.gi.vfs.read("art\\tiles\\tiles.lst"))]
        tl = self._tiles_lst
        rng = random.Random(self.r.get("seed", 1) * 53 + level["elev"])
        for ff in level["floor_fill"]:
            ids = [tl.index(n.lower()) for n in ff["tiles"] if n.lower() in tl]
            x0, y0, x1, y1 = ff["rect"]
            for y in range(y0, y1 + 1):
                for x in range(x0, x1 + 1):
                    self.mb.set_floor(level["elev"], x, y, rng.choice(ids))

    def walls(self, level: dict):
        """Buildings drawn with a wall family learned from the original
        games (tools/wallkit.py), never copied: "buildings": [{"rect":
        [sx0, sy0, sx1, sy1] (squares, outer walls), "family": "nec",
        "doors": [[hx, hy], ...], "inner": [[hx0, hy0, hx1, hy1], ...] (inner
        wall lines, hexes), "floor": "tile name", "roof": "tile name",
        "seed": n, "broken": 0.0-1.0 (share of wall hexes left out, for
        ruins), "no_roof": true}]."""
        import random
        import wallkit
        de = level["elev"]
        G = None
        for i, bd in enumerate(level.get("buildings", [])):
            rng = random.Random(bd.get("seed", self.r.get("seed", 1) * 31 + i))
            sx0, sy0, sx1, sy1 = bd["rect"]
            hx0, hy0, hx1, hy1 = sx0 * 2, sy0 * 2, sx1 * 2 + 1, sy1 * 2 + 1
            cells = set()
            for x in range(hx0, hx1 + 1):
                cells.add((x, hy0))
                cells.add((x, hy1))
            for y in range(hy0, hy1 + 1):
                cells.add((hx0, y))
                cells.add((hx1, y))
            for (ax, ay, bx, by) in bd.get("inner", []):
                if ay == by:
                    for x in range(min(ax, bx), max(ax, bx) + 1):
                        cells.add((x, ay))
                else:
                    for y in range(min(ay, by), max(ay, by) + 1):
                        cells.add((ax, y))
            doors = [tuple(d) for d in bd.get("doors", [])]
            gaps = {tuple(g) for g in bd.get("gaps", [])}
            broken = bd.get("broken", 0)
            if broken:
                # ruins: whole stretches gone, not single hexes
                runs = sorted(cells)
                for c in runs:
                    if rng.random() < broken / 4 and c not in doors:
                        for k in range(rng.randint(2, 6)):
                            gaps.add((c[0] + k, c[1]))
                            gaps.add((c[0], c[1] + k))
            draw_cells = (cells - gaps) | set(doors)
            n = wallkit.draw(self.add, self.convert_pid, bd["family"], draw_cells, de, rng, doors,
                             bd.get("door_pref"), bd.get("source", "fo2"), bd.get("style", 0))
            # floor and roof
            if bd.get("floor") or bd.get("roof"):
                import groundkit
                from foclib import parse_lst
                if not hasattr(self, "_tiles_lst"):
                    self._tiles_lst = [x.split(",")[0].lower() for x in parse_lst(self.gi.vfs.read("art\\tiles\\tiles.lst"))]
                tl = self._tiles_lst

                def tid(name):
                    names = name if isinstance(name, list) else [name]
                    ids = [tl.index(nm.lower()) for nm in names if nm.lower() in tl]
                    return ids
                fl = tid(bd["floor"]) if bd.get("floor") else []
                rf = tid(bd["roof"]) if bd.get("roof") and not bd.get("no_roof") else []
                wear = bd.get("floor_wear", 0)
                nz = None
                if wear:
                    import groundkit
                    nz = groundkit.Noise(rng, 3)
                for sy in range(sy0, sy1 + 1):
                    for sx in range(sx0, sx1 + 1):
                        if fl and not (nz and 0.7 * nz(sx, sy) + 0.3 * rng.random() < wear):
                            self.mb.set_floor(de, sx, sy, rng.choice(fl))
                        if rf:
                            self.mb.set_roof(de, sx, sy, rng.choice(rf))
            print(f"  building {bd.get('name', i)} ({bd['family']}): {n} wall pieces, {len(doors)} doors")

    def add(self, pid, fid, tile, elev, rot=0, frame=0, extras=None):
        pr = self.gi.proto(pid)
        if extras is None:
            extras = extras_for(pid_type(pid), pid, pr.subtype if pr else None)
        self.mb.add(MapObjectSpec(pid=pid, fid=fid, tile=tile, elevation=elev,
                                  rotation=rot, frame=frame, extras=extras))

    def _ground_tile(self, src, tile_id: int) -> bool:
        """A source tile is ground (road, slab, sand or gravel), not a room's floor."""
        key = id(src)
        if not hasattr(self, "_src_tile_names"):
            self._src_tile_names = {}
        if key not in self._src_tile_names:
            self._src_tile_names[key] = [x.split(",")[0].lower() for x in parse_lst(src.vfs.read("art" + chr(92) + "tiles" + chr(92) + "tiles.lst"))]
        names = self._src_tile_names[key]
        return tile_id < len(names) and names[tile_id].startswith(("road", "curb", "edg", "dayg13"))

    def lift(self, level: dict, spec: dict):
        conv, src = self.converter(spec["source"])
        name = spec["map"] if spec["map"].lower().endswith(".map") else spec["map"] + ".map"
        raw = src.vfs.read(f"maps\\{name}")
        m = MapFile.parse(raw, src.proto)
        # "same_map": the source map's own index in its maps.txt. Its stairs and ladders that lead
        # to another level of the same map (that index, or 0 / -1, which the engine reads as "this
        # map") come across, led to the same hex moved by this lift's shift (lift the other level's
        # box with the same shift, and name the levels that are: "link_elevs": [0, 2]; a link to a
        # level not named stays behind; "link_elev_map": {"2": 1} when that level lands on another). "map_links": {source map index: mod map id} - stairs and
        # ladders to those maps come across too, led to the mod's map (unshifted lifts only).
        # Anything else (other maps, elevators) stays behind.
        same = spec.get("same_map")
        map_links = {int(k): v for k, v in spec.get("map_links", {}).items()}
        se = spec.get("elev", 0)
        ei = m.elevations.index(se)
        x0, y0, x1, y1 = spec.get("rect", [0, 0, 99, 99])
        dx, dy = spec.get("shift", [0, 0])
        de = level["elev"]
        drop = [re.compile(p, re.I) for p in level.get("drop", []) + spec.get("drop", [])]
        # "keep": [regex, ...] lifts only scenery/walls whose art matches (a
        # plane out of a boneyard); "no_floor": true leaves floor and roof behind
        keep = [re.compile(p, re.I) for p in spec.get("keep", [])]
        # "ground_only": true lifts road, slab and gravel tiles and leaves every other floor and
        # the roofs behind (a street taken without the buildings beside it); "no_walls": true
        # leaves walls and doors; "no_objects": true leaves everything that stands on it
        ground_only = spec.get("ground_only", False)
        # "no_roof": true leaves the roofs behind (a ruin lifted with its floor); "drop_tiles":
        # [regex, ...] leaves floor tiles of those names behind (the whole asphalt of a street
        # that ran past a ruin)
        no_roof = ground_only or spec.get("no_roof", False)
        drop_tiles = [re.compile(p, re.I) for p in spec.get("drop_tiles", [])]
        src_tiles = [x.split(",")[0].lower() for x in parse_lst(src.vfs.read("art\\tiles\\tiles.lst"))] if (ground_only or drop_tiles) else []
        sc_art = parse_lst(src.vfs.read("art\\scenery\\scenery.lst"))
        wl_art = parse_lst(src.vfs.read("art\\walls\\walls.lst"))

        def inside(sx, sy):
            return x0 <= sx <= x1 and y0 <= sy <= y1

        # Most levels pave the whole grid with one background tile around the
        # rooms; leave it behind so a lifted room keeps only its own floor.
        filler = None
        if not spec.get("keep_filler"):
            from collections import Counter
            cnt = Counter(v & 0xFFF for v in m.floors[ei] if (v & 0xFFF) > 1)
            if cnt:
                tid, c = cnt.most_common(1)[0]
                if c > 0.4 * sum(cnt.values()):
                    filler = tid
        # floored squares of the box, less isolated specks (background tiles
        # some maps sprinkle round their rooms): a square needs two floored
        # neighbours, applied twice
        fl = {k for k in range(10000) if inside(k % 100, k // 100)
              and (m.floors[ei][k] & 0xFFF) > 1 and (m.floors[ei][k] & 0xFFF) != filler}
        for _ in range(0 if ground_only else 2):
            fl = {k for k in fl if sum((k + d) in fl for d in (-1, 1, -100, 100)) >= 2}
        # "floor_inside": N keeps a lifted room floor only where the lifted walls enclose it: from
        # the square, at least N of the four directions reach one of those walls within eight
        # squares. A floor running out past a ruin's walls is left behind.
        floor_inside = int(spec.get("floor_inside", 0))
        room_floor = []
        tiles = 0
        for k in range(10000 if not spec.get("no_floor") else 0):
            sx, sy = k % 100, k // 100
            if not inside(sx, sy):
                continue
            tx, ty = sx + dx, sy + dy
            if not (0 <= tx < 100 and 0 <= ty < 100):
                continue
            v = m.floors[ei][k] & 0xFFF
            if ground_only and not (v < len(src_tiles) and src_tiles[v].startswith(("road", "curb", "edg"))):
                continue
            if drop_tiles and v < len(src_tiles) and any(p.search(src_tiles[v]) for p in drop_tiles):
                continue
            if k in fl:
                t = conv.map_tile(v, self.matcher)
                if t:
                    self.mb.set_floor(de, tx, ty, t)
                    tiles += 1
                    if floor_inside and not self._ground_tile(src, v):
                        room_floor.append((tx, ty))
            v = m.roofs[ei][k] & 0xFFF
            if v > 1 and not no_roof:
                t = conv.map_tile(v, self.matcher)
                if t:
                    self.mb.set_roof(de, tx, ty, t)
        n = skipped = links = 0
        n_before = len(self.mb.objects)
        for o in m.objects:
            if o.elevation != se or not (0 <= o.tile < 40000):
                continue
            hx, hy = hex_xy(o.tile)
            if not inside(hx // 2, hy // 2):
                continue
            t = pid_type(o.pid)
            if t in (1, 4, 5) or spec.get("no_objects"):
                continue
            pr = src.proto(o.pid)
            if t == 0 and not (pr and pr.subtype == 1):        # loose items (containers stay)
                continue
            if spec.get("no_walls") and (t == 3 or (t == 2 and pr is not None and pr.subtype == 0)):
                continue
            link = None
            if t == 2 and pr is not None and pr.subtype in LINKED_SCENERY:
                if same is None or pr.subtype == 2:
                    skipped += 1
                    continue
                v1, v2 = struct.unpack(">ii", raw[o.off + 0x58:o.off + 0x60])
                built, dmap = (v1, v2) if pr.subtype == 1 else (v2, v1)       # stairs: tile, map; ladders: map, tile
                d_elev, d_rot, d_tile = (built >> 29) & 7, (built >> 26) & 7, built & 0x3FFFFFF
                if dmap in (same, -1, 0):
                    dmap = 0                                            # this map
                    if "link_elevs" in spec and d_elev not in spec["link_elevs"]:
                        skipped += 1
                        continue
                    d_elev = int(spec.get("link_elev_map", {}).get(str(d_elev), d_elev))   # the level it is lifted onto here
                    tx, ty = d_tile % HEX_W + dx * 2, d_tile // HEX_W + dy * 2
                    if not (0 <= tx < 200 and 0 <= ty < 200):
                        skipped += 1
                        continue
                    built = (ty * HEX_W + tx) | (d_rot << 26) | (d_elev << 29)
                elif dmap in map_links and (dx, dy) == (0, 0):
                    dmap = map_links[dmap]
                else:
                    skipped += 1
                    continue
                link = (built, dmap) if pr.subtype == 1 else (dmap, built)
            lst = sc_art if t == 2 else wl_art if t == 3 else None
            art = lst[o.fid & 0xFFFFFF].lower() if lst and (o.fid & 0xFFFFFF) < len(lst) else ""
            if art and any(p.search(art) for p in drop):
                continue
            if keep and not (art and any(p.search(art) for p in keep)):
                continue
            pid = conv.map_pid(o.pid)
            fid = conv.map_frm(o.pid, o.fid, pid) if pid else None
            if pid is None or fid is None:
                skipped += 1
                continue
            nx, ny = hx + dx * 2, hy + dy * 2
            if not (0 <= nx < 200 and 0 <= ny < 200):
                continue
            self.add(pid, fid, ny * HEX_W + nx, de, o.rotation, o.frame, extras=link)
            n += 1
            links += link is not None
        bare = 0
        if floor_inside and room_floor:
            wsq = {((o2.tile % HEX_W) // 2, (o2.tile // HEX_W) // 2) for o2 in self.mb.objects[n_before:]
                   if o2.elevation == de and pid_type(o2.pid) == 3}
            ids = self.tile_ids()
            for tx, ty in room_floor:
                hit = sum(any((tx + ddx * d, ty + ddy * d) in wsq for d in range(0, 9)) for ddx, ddy in ((1, 0), (-1, 0), (0, 1), (0, -1)))
                if hit < floor_inside:
                    self.mb.set_floor(de, tx, ty, ids[f"edg500{self.SAND_BLOCK[ty % 10][tx % 10]}"])
                    bare += 1
        if not spec.get("quiet"):
            if bare:
                print(f"    {bare} squares of floor with no walls round them left behind")
            print(f"  lift {spec['source']}:{name} elev {se} -> elev {de}: {tiles} tiles, {n} objects ({skipped} left out)"
                  + (f", {links} stairs and ladders kept" if links else ""))

    def exits(self, level: dict):
        de = level["elev"]
        pr = self.gi.proto(EXIT_GRID)
        for ex in level.get("exits", []):
            # "hexes": a box, every floored hex in it; "hex_list": exactly these hexes (a source map's own grid)
            if "hex_list" in ex:
                cells = [tuple(h) for h in ex["hex_list"]]
            else:
                x0, y0, x1, y1 = ex["hexes"]
                cells = [(hx, hy) for hy in range(y0, y1 + 1) for hx in range(x0, x1 + 1)]
            placed = 0
            for hx, hy in cells:
                sq = (hy // 2) * 100 + hx // 2
                if self.mb.floors[de][sq] <= 1:
                    continue
                # brown to the world map, green to the next map of the place: Fallout 2 draws every world exit brown
                self.add(EXIT_GRID, WORLD_EXIT_FID if ex["to"] < 0 else pr.fid, hy * HEX_W + hx, de,
                         extras=(ex["to"], ex.get("tile", -1), ex.get("elev", 0), ex.get("rot", 0)))
                placed += 1
            print(f"  exits to {ex['to']}: {placed} hexes")

    def tunnels(self, level: dict):
        """Rock-lined passages between lifted pieces. Each tunnel is a path of
        square points joined by L-shaped runs (x first, then y), `width`
        squares wide. Its squares get cave floor (`floor` tile ids); wall and
        blocking scenery standing on them is removed, so a tunnel breaks into
        any room it reaches; every hex next to it with no floor gets a Secret
        Blocking Hex, and about one in `rock_every` of those a rock."""
        import random
        de = level["elev"]
        rng = random.Random(self.r.get("seed", 1) * 7 + de)
        for tn in level.get("tunnels", []):
            w = tn.get("width", 2)
            floor = tn.get("floor", [21, 22, 23, 24])
            rocks = [int(x, 16) if isinstance(x, str) else x for x in tn.get("rocks", DEFAULT_ROCKS)]
            pts = tn["path"]
            sq = set()

            def stamp(x, y):
                for ox in range(-(w // 2), w - w // 2):
                    for oy in range(-(w // 2), w - w // 2):
                        if 0 <= x + ox < 100 and 0 <= y + oy < 100:
                            sq.add((x + ox, y + oy))
            for (ax, ay), (bx, by) in zip(pts, pts[1:]):
                step = 1 if bx >= ax else -1
                for x in range(ax, bx + step, step):
                    stamp(x, ay)
                step = 1 if by >= ay else -1
                for y in range(ay, by + step, step):
                    stamp(bx, y)
            keep_floor = tn.get("keep_room_floor", True)
            for (x, y) in sq:
                if keep_floor and self.mb.floors[de][y * 100 + x] > 1 and (x, y) not in self._tunnel_sq.get(de, set()):
                    continue
                self.mb.set_floor(de, x, y, rng.choice(floor))
            self._tunnel_sq.setdefault(de, set()).update(sq)
            hexes = {(x * 2 + a, y * 2 + b) for (x, y) in sq for a in (0, 1) for b in (0, 1)}
            before = len(self.mb.objects)
            kept = []
            for o in self.mb.objects:
                if o.elevation == de and (o.tile % HEX_W, o.tile // HEX_W) in hexes:
                    t = pid_type(o.pid)
                    if t == 3 or (t == 2 and o.pid not in rocks):
                        pr = self.gi.proto(o.pid)
                        if t == 3 or (pr is not None and not (pr.flags & 0x10)):
                            continue
                kept.append(o)
            self.mb.objects = kept
            breached = before - len(kept)
            border = set()
            for (hx, hy) in hexes:
                tile = hy * HEX_W + hx
                for d in (DIRS[hx & 1]):
                    n = tile + d
                    nx, ny = n % HEX_W, n // HEX_W
                    if not (0 <= n < 40000) or abs(nx - hx) > 1 or (nx, ny) in hexes:
                        continue
                    if self.mb.floors[de][(ny // 2) * 100 + nx // 2] <= 1:
                        border.add(n)
            blk = self.gi.proto(BLOCKING_HEX)
            for k, n in enumerate(sorted(border)):
                self.add(BLOCKING_HEX, blk.fid, n, de)
                if rng.random() < 1.0 / tn.get("rock_every", 2):
                    rp = rng.choice(rocks)
                    pr = self.gi.proto(rp)
                    if pr is not None:
                        self.add(rp, pr.fid, n, de, rng.randrange(6))
            print(f"  tunnel {pts}: {len(sq)} squares, {breached} wall pieces broken through, {len(border)} border hexes")

    def scatter(self, level: dict):
        """Scatter scenery over floored, empty hexes of a square rect:
        {"pids": [...], "count": N, "rect": [x0, y0, x1, y1]}."""
        import random
        de = level["elev"]
        rng = random.Random(self.r.get("seed", 1) * 13 + de)
        taken = {o.tile for o in self.mb.objects if o.elevation == de}
        walls = [(o.tile % HEX_W, o.tile // HEX_W) for o in self.mb.objects
                 if o.elevation == de and pid_type(o.pid) == 3]
        for sc in level.get("scatter", []):
            pids = [int(x, 16) if isinstance(x, str) else x for x in sc["pids"]]
            x0, y0, x1, y1 = sc["rect"]
            # "clear_walls": N keeps the scatter N hexes off any wall, so no bush
            # lands in a doorway
            cw = sc.get("clear_walls", 0)
            near = {(wx + a, wy + b) for wx, wy in walls for a in range(-cw, cw + 1)
                    for b in range(-cw, cw + 1)} if cw else set()
            cells = [(hx, hy) for hy in range(y0 * 2, y1 * 2 + 2) for hx in range(x0 * 2, x1 * 2 + 2)
                     if self.mb.floors[de][(hy // 2) * 100 + hx // 2] > 1 and hy * HEX_W + hx not in taken
                     and (hx, hy) not in near]
            rng.shuffle(cells)
            n = 0
            for hx, hy in cells[:sc.get("count", 20)]:
                pid = rng.choice(pids)
                pr = self.gi.proto(pid)
                if pr is None:
                    continue
                t = hy * HEX_W + hx
                # facing: 0 unless the recipe lists real ones ("rots": [0, 2] for
                # cars, whose other directions are placeholder frames)
                self.add(pid, pr.fid, t, de, rng.choice(sc.get("rots", [0])))
                taken.add(t)
                n += 1
            print(f"  scatter {len(pids)} kinds in {sc['rect']}: {n}")

    def openings(self, level: dict):
        """Knock doorways through stamped walls: "openings": [[hx, hy], ...]
        removes the walls and blocking scenery on those hexes (not doors)."""
        de = level["elev"]
        holes = {hy * HEX_W + hx for hx, hy in level.get("openings", [])}
        # "clear": [[hx0, hy0, hx1, hy1], ...] empties rooms of their furniture
        # (scenery other than doors and blockers) so they can be furnished anew
        clear = level.get("clear", [])
        # "remove_at": [[hx, hy], ...] takes away the scenery standing on exactly those hexes (not
        # doors): one lifted piece, such as a sign naming the source game's places
        remove = {hy * HEX_W + hx for hx, hy in level.get("remove_at", [])}
        if not holes and not clear and not remove:
            return

        def in_clear(tile):
            x, y = tile % HEX_W, tile // HEX_W
            return any(x0 <= x <= x1 and y0 <= y <= y1 for x0, y0, x1, y1 in clear)
        kept = []
        cleared = 0
        for o in self.mb.objects:
            if o.elevation == de and o.tile in holes:
                t = pid_type(o.pid)
                pr = self.gi.proto(o.pid)
                if t == 3 or (t == 2 and pr is not None and pr.subtype != 0 and not (pr.flags & 0x10)):
                    continue
            if o.elevation == de and o.tile in remove and pid_type(o.pid) == 2:
                pr = self.gi.proto(o.pid)
                if pr is not None and pr.subtype != 0:
                    cleared += 1
                    continue
            if o.elevation == de and clear and pid_type(o.pid) in (0, 2) and in_clear(o.tile):
                pr = self.gi.proto(o.pid)
                if pr is not None and not (pid_type(o.pid) == 2 and pr.subtype == 0)                         and self.scenery_art_name(o.fid).lower() != "block.frm":
                    cleared += 1
                    continue
            kept.append(o)
        print(f"  openings: {len(self.mb.objects) - len(kept) - cleared} pieces removed on {len(holes)} hexes; "
              f"{cleared} furnishings cleared")
        self.mb.objects = kept

    def placed(self, level: dict):
        de = level["elev"]
        for ob in level.get("objects", []):
            pid = int(ob["pid"], 16) if isinstance(ob["pid"], str) else ob["pid"]
            pr = self.gi.proto(pid)
            if pr is None:
                sys.exit(f"no proto 0x{pid:08X}")
            hx, hy = ob["hex"]
            self.add(pid, pr.fid, hy * HEX_W + hx, de, ob.get("rot", 0))

    def ground(self, level: dict):
        """Outdoor ground: {"rect": [x0, y0, x1, y1], "tiles": [...], "weights": [...]}
        and road strips {"rect": [...], "tile": id} (squares, inclusive)."""
        import random
        de = level["elev"]
        rng = random.Random(self.r.get("seed", 1) * 17 + de)
        g = level.get("ground")
        if g:
            x0, y0, x1, y1 = g["rect"]
            for sy in range(y0, y1 + 1):
                for sx in range(x0, x1 + 1):
                    self.mb.set_floor(de, sx, sy, rng.choices(g["tiles"], g.get("weights"))[0])
        for rd in level.get("roads", []):
            x0, y0, x1, y1 = rd["rect"]
            for sy in range(y0, y1 + 1):
                for sx in range(x0, x1 + 1):
                    self.mb.set_floor(de, sx, sy, rd["tile"])

    # Fallout 2's sand block, as Sonora's Day Glow and Mexicali lay it (and the mod's own maps):
    # edg500<digit> at (x % 10, y % 10). tools/compose_grid.py prints a source map's phase.
    SAND_BLOCK = ['2130120310', '3050403075', '6417352146', '0205040304', '6326103701',
                  '2000302040', '3256245273', '0307030404', '0461326152', '1070501030']

    def tile_ids(self) -> dict:
        if not hasattr(self, "_tile_ids"):
            names = [x.split(",")[0].lower().rsplit(".", 1)[0] for x in parse_lst(self.gi.vfs.read("art\\tiles\\tiles.lst"))]
            self._tile_ids = {}
            for i, n in enumerate(names):
                self._tile_ids.setdefault(n, i)
        return self._tile_ids

    def sand(self, level: dict):
        """"sand": {"all": true} lays the sand block over the whole grid (the ground then runs on
        well past the map's edges); {"like": {"source", "map", "elev"}} lays it over every square
        that map has a floor on: the source's own frame of desert."""
        g = level.get("sand")
        if not g:
            return
        de = level["elev"]
        ids = self.tile_ids()
        if g.get("all"):
            # the whole grid: the ground runs on well past the map's edges, nothing stops in view
            for k in range(10000):
                x, y = k % 100, k // 100
                self.mb.set_floor(de, x, y, ids[f"edg500{self.SAND_BLOCK[y % 10][x % 10]}"])
            print("  sand: the whole grid, in the block's pattern")
            return
        like = g["like"]
        _conv, src = self.converter(like["source"])
        name = like["map"] if like["map"].lower().endswith(".map") else like["map"] + ".map"
        m = MapFile.parse(src.vfs.read(f"maps\\{name}"), src.proto)
        fl = m.floors[m.elevations.index(like.get("elev", 0))]
        n = 0
        for k in range(10000):
            if (fl[k] & 0xFFF) > 1:
                x, y = k % 100, k // 100
                self.mb.set_floor(de, x, y, ids[f"edg500{self.SAND_BLOCK[y % 10][x % 10]}"])
                n += 1
        print(f"  sand: {n} squares in the block's pattern, the shape of {like['source']}:{name}")

    def resand(self, level: dict):
        """"resand": true puts every plain sand square (edg5000-5007) back on the block's pattern
        by where it lies now, so a box lifted with a shift that isn't a whole ten leaves no seam.
        "tiles": [[x0, y0, x1, y1, "road02"], ...] then lays named tiles over squares: a curb
        across a street's mouth made asphalt, where two boxes meet."""
        de = level["elev"]
        ids = self.tile_ids()
        if level.get("resand"):
            plain = {ids[f"edg500{d}"] for d in "01234567"}
            fl = self.mb.floors[de]
            n = 0
            for k in range(10000):
                if fl[k] in plain:
                    x, y = k % 100, k // 100
                    t = ids[f"edg500{self.SAND_BLOCK[y % 10][x % 10]}"]
                    if t != fl[k]:
                        self.mb.set_floor(de, x, y, t)
                        n += 1
            print(f"  resand: {n} sand squares put back on the pattern")
        for x0, y0, x1, y1, name in level.get("tiles", []):
            for y in range(y0, y1 + 1):
                for x in range(x0, x1 + 1):
                    self.mb.set_floor(de, x, y, ids[name])

    # What stands beside a stretch of Day Glow's street but belongs to the building or the overpass
    # it ran past, not to the street: left behind when the stretch is laid as a street of ours.
    STREET_DROP = [r"^wlamp", r"^door", r"^bed", r"^shlf", r"^chair", r"^table", r"^stair", r"^robcomp", r"^mdesk", r"^tv0",
                   r"^sstove", r"^grand", r"^wdbeam", r"^daygl09[3-9]", r"^daygl1", r"^gener", r"^board0", r"^comp", r"^toilet",
                   r"^villa", r"^pnx", r"^inf10", r"^sanset", r"^bookcas", r"^gar0", r"^stmain", r"^mach0", r"^generat",
                   r"^city0[0-2]", r"^vault", r"^boxbig", r"^boxsml", r"^army", r"^daygl038", r"^fork",
                   r"^sign02[3-6]",                    # Mexicali's stop signs say ALTO
                   # what is San Diego's or Tijuana's alone: the border crossing and its overpass, the
                   # freeway signposts and street signs, named hotel and shop signs
                   r"^daygl00[6-9]", r"^daygl01[0-8]", r"^daygl02[78]", r"^daygl07[5-9]", r"^daygl091"]

    def streets(self, level: dict):
        """"streets": [street, ...] - straight streets laid from stretches of Day Glow's own
        (tools/dayglow_streets.py: each runs between two clean cross-sections, so any one follows
        any other without a seam), by default from one limit of the grid to the other: the tile
        work then runs well past the map's edges. A street:
          "axis"   "ns" (it runs north-south, its width along x) or "ew"
          "at"     the square its core starts on across its width: the east curb (ns), the north
                   curb (ew). The core is one curb square, the asphalt, one curb square.
          "width"  squares of asphalt (3 or 5: what Day Glow has most of)
          "start", "end"  where it runs from and to, along its length (default 0 and 99). It is
                   laid from start toward end, in either direction; what it starts against (a
                   lifted junction, another street) must be a clean cross-section there.
          "walk"   [low, high] squares of sidewalk taken along beyond the core on each side (2, 2)
          "objects" false leaves the wrecks, lamps and litter that stand on the stretches behind
                   (furniture and overpass parts of what the stretch ran past are always left:
                   STREET_DROP, or the street's own "drop")
          "seed"   another order of stretches
        Stretches are drawn by their length, so the street is as broken as Day Glow's are, and
        none follows itself. Lifts laid after it (a junction, a special stretch) lie over it."""
        specs = level.get("streets", [])
        if not specs:
            return
        import dayglow_streets
        cat = dayglow_streets.catalog()
        for st in specs:
            axis, width = st["axis"], int(st.get("width", 3))
            pool = cat.get(f"{axis}|{width}", [])
            if not pool:
                raise SystemExit(f"streets: Day Glow has no {axis} street with {width} squares of asphalt (tools/dayglow_streets.py)")
            start, end = int(st.get("start", 0)), int(st.get("end", 99))
            step = 1 if end >= start else -1
            wlo, whi = st.get("walk", [2, 2])
            rng = random.Random(st.get("seed", 0) * 7919 + st["at"] * 31 + start)
            weights = [p["b"] - p["a"] for p in pool]
            cur, last, laid, n_obj = start, None, [], 0
            while (cur <= end) if step > 0 else (cur >= end):
                pick = rng.choices(range(len(pool)), weights)[0]
                if pick == last and len(pool) > 1:
                    continue
                last = pick
                seg = pool[pick]
                a, n = seg["a"], seg["b"] - seg["a"]            # rows a .. b-1: from its clean row to the row before the next
                if step > 0:
                    take = min(n, end - cur + 1)
                    r0, r1, sh = a, a + take - 1, cur - a
                else:
                    take = min(n, cur - end + 1)
                    r0, r1, sh = a + n - take, a + n - 1, cur - (a + n - 1)
                lo, hi = seg["lo"] - wlo, seg["hi"] + whi
                if axis == "ns":
                    rect, shift = [lo, r0, hi, r1], [st["at"] - seg["lo"], sh]
                else:
                    rect, shift = [r0, lo, r1, hi], [sh, st["at"] - seg["lo"]]
                self.lift(level, {"source": "sonora", "map": seg["map"], "elev": 0, "rect": rect, "shift": shift,
                                  "ground_only": True, "no_walls": True, "no_objects": not st.get("objects", True),
                                  "drop": st.get("drop", self.STREET_DROP), "quiet": True})
                laid.append(n)
                cur += step * n
            print(f"  street {axis} at {st['at']}, {width} wide, {start} to {end}: {len(laid)} stretches of Day Glow's, "
                  f"{min(laid)} to {max(laid)} squares long")

    def reach(self, level: dict):
        """"edge": [x0, y0, x1, y1] - the map's edges in squares (where its exit grids and scroll
        blockers stand). Reports how far the tile work runs on past each one, and stops the build
        when it ends within "past" squares (default 12) of an edge without having reached the
        grid's own limit, or when an edge is that near the grid's limit (0 and 99, past which
        nothing can be laid): nothing may stop in view."""
        if "edge" not in level:
            return
        de = level["elev"]
        fl = self.mb.floors[de]
        x0, y0, x1, y1 = level["edge"]
        need = int(level.get("past", 12))

        def run(cells, dx, dy):
            best = 99
            for x, y in cells:
                k = 0
                while 0 <= x + dx * (k + 1) < 100 and 0 <= y + dy * (k + 1) < 100 and fl[(y + dy * (k + 1)) * 100 + x + dx * (k + 1)] > 1:
                    k += 1
                at_limit = not (0 <= x + dx * (k + 1) < 100 and 0 <= y + dy * (k + 1) < 100)
                best = min(best, 99 if at_limit else k)
            return best
        sides = {"north": run([(x, y0) for x in range(x0, x1 + 1)], 0, -1), "south": run([(x, y1) for x in range(x0, x1 + 1)], 0, 1),
                 "east": run([(x0, y) for y in range(y0, y1 + 1)], -1, 0), "west": run([(x1, y) for y in range(y0, y1 + 1)], 1, 0)}
        print("  past the map's edges the tile work runs on: " + ", ".join(
            f"{k} {'to the grid limit' if v == 99 else f'{v} squares'}" for k, v in sides.items()))
        short = [k for k, v in sides.items() if v < need]
        if short:
            raise SystemExit(f"the tile work stops within {need} squares of the map's {', '.join(short)} edge: lay it on (sand all, streets to the limit)")
        # the grid itself ends at 0 and 99: an edge nearer to that than the view reaches shows black
        near = [k for k, v in (("north", y0), ("south", 99 - y1), ("east", x0), ("west", 99 - x1)) if v < need]
        if near:
            raise SystemExit(f"the map's {', '.join(near)} edge is within {need} squares of the grid's limit, where nothing can be laid: move the map in")

    def ground_edges(self, level: dict):
        """"ground_edges": true ends every road and sidewalk that sand lies hard against (the cut
        edge of a lifted box) the way Day Glow ends one: the road square takes its broken edge
        piece, the slab square its curb with sand over it, each by its four neighbours
        (tools/dayglow_ground.py). Squares of a building are left as they are. What no edge piece
        can end is listed: move that box's edge."""
        if not level.get("ground_edges"):
            return
        import dayglow_ground as dgr
        de = level["elev"]
        keep = set(getattr(self, "_dayglow_inside", {}).get(de, set()))
        g = dgr.Ground(self.gi, self.mb.floors[de])
        before = len(g.faults())
        n = g.heal(keep)
        left = g.faults()
        print(f"  ground edges: sand lay wrongly in {before} places; {n} squares given their edge piece, {len(left)} places left")
        for x, y, sd in left[:12]:
            print(f"    still at fault: square ({x},{y}) {g.at(x, y)} with sand on its {sd}")

    # a room's floor as Day Glow lays it: one board tile with its own border all round
    ROOM_FLOOR = {"mid": "tp155", "n": "tp157", "s": "tp159", "w": "tp160", "e": "tp158",
                  "nw": "tp164", "ne": "tp161", "sw": "tp163", "se": "tp162"}

    def dayglow(self, level: dict):
        """"dayglow": [building, ...] - buildings of our own design with walls laid piece by piece
        as Sonora's Day Glow lays them (tools/dayglow_walls.py, which documents the wall plan's
        keys: xrows, ycols, ext_rows, ext_cols, doors, gaps, windows; plus seed and corner). A
        building is written out here or is a saved plan stood at a place: {"plan": "bank",
        "at": [sx, sy]} (tools/dayglow_plan.py; mod/maps_src/buildings/). Also:
          "sets": [{"kind": "vault_door", "at": [x, y]}, ...]  set pieces stood as Sonora stands
                   them, blockers included (tools/dayglow_plan.py sets)
          "rooms": [[east_col, north_row, west_col, south_row, floor], ...]  the hex lines of the
                   walls round each room. Its floor is laid as Day Glow lays one: a board tile
                   (floor: tp155, the default, or tp153, tp154, tp156) inside its own border, the
                   border's south and east sides under those walls; or "slab", bare concrete.
          "pieces": [[art, hx, hy], ...]  scenery set by hand, each by Sonora's own name for it
          "apron": N  the sidewalk the building stands on: slab under its north and west walls
                   and N squares out from every wall, wherever sand lies now, each piece chosen
                   by its neighbours as Day Glow's are (tools/dayglow_survey.py's table).
          "apron_skip": [[x0, y0, x1, y1], ...]  squares of it left as sand: drifts over the
                   sidewalk, which Day Glow has along about a tenth of a building's edge.
          "storeys": 2  the roof shows a second storey (tools/dayglow_roof.py); "roof": false
                   leaves the roof off (a building open to the sky)
          "paving": [[x0, y0, x1, y1], ...]  boxes of squares paved the way the sidewalk is, curbs
                   and all: a square, a yard. "apron_skip" leaves drifts in it too.
        At the foot of the dark walls the slab is laid in shadow, as Day Glow lays it: the row of
        squares south of the south wall, the squares the west wall stands on ("shadow": false
        leaves it out, as Mexicali does).
        An upper floor is a building on a level of its own with "apron": 0, "shadow": false and
        "black_edge": true (Day Glow's black wall pieces outside the dark walls); its stairs and
        the ground floor's name each other's level ("stairs_up" / "stairs_down" with "to")."""
        specs = level.get("dayglow", [])
        if not specs:
            return
        import dayglow_walls as dw
        de = level["elev"]
        mdl = dw.model()
        conv, _src = self.converter("sonora")
        ids = self.tile_ids()
        names = {v: k for k, v in ids.items()}
        sand = {ids[f"edg500{d}"] for d in "01234567"}
        fl = self.mb.floors[de]

        def put(src_pid, src_fid, h, extras=None):
            pid = conv.map_pid(src_pid)
            fid = conv.map_frm(src_pid, src_fid, pid) if pid else None
            if pid is None or fid is None:
                return False
            self.add(pid, fid, h[1] * HEX_W + h[0], de, extras=extras)
            return True
        import dayglow_plan
        for b in specs:
            b = dayglow_plan.resolve(b)       # a saved plan stood here, its set pieces stood
            built = dw.lay(b, seed=b.get("seed", 0), corner=b.get("corner", 0))
            missing = 0
            for h, art in sorted(built["walls"].items(), key=lambda kv: (kv[0][1], kv[0][0])):
                missing += not put(*mdl["pid"][art], h)
            for h in sorted(built["blocks"]):
                missing += not put(*mdl["block_wall"], h)
            for h, spid in built.get("fixed", {}).items():
                spid = int(spid, 16) if isinstance(spid, str) else spid
                missing += not put(spid, _src.proto(spid).fid, h)
            for h, art in built["doors"].items():
                missing += not put(*mdl["door_pid"][art], h)
            for art, hx, hy, *more in b.get("pieces", []):
                # stairs lead somewhere: {"to": [level, hx, hy]} after the piece's note
                to = next((v["to"] for v in more if isinstance(v, dict) and "to" in v), None)
                link = ((to[2] * HEX_W + to[1]) | (to[0] << 29), 0) if to else None
                # a piece set by hand: by Sonora's name for its art as it shows on dayglow_atlas.py's
                # sheets, or by Sonora's proto id ("0x00000150", its Cashier) for a piece of another
                # of its maps or a container, or "foc:0x02001F1C" for a piece only this mod has; a
                # fourth entry says what it is there for
                if art.startswith("foc:"):
                    # one of the mod's own pieces (a statue, a pew), by the mod's proto id
                    fpid = int(art[4:], 16)
                    pr = self.gi.proto(fpid)
                    ok = pr is not None
                    if ok:
                        self.add(fpid, pr.fid, hy * HEX_W + hx, de, extras=link)
                elif art.startswith("0x"):
                    spid = int(art, 16)
                    pr = _src.proto(spid)
                    ok = pr is not None and put(spid, pr.fid, (hx, hy), link)
                else:
                    ok = art in mdl["scenery_pid"] and put(*mdl["scenery_pid"][art], (hx, hy), link)
                if not ok:
                    raise SystemExit(f"dayglow {b.get('id')}: no piece {art!r} (Sonora has none, or the mod lacks it)")
            inside = set()
            if not hasattr(self, "_dayglow_inside"):
                self._dayglow_inside = {}
            for east, north, west, south, *rest in b.get("rooms", []):
                kind = rest[0] if rest else "tp155"
                x0, x1 = east // 2, west // 2 - 1
                y0, y1 = north // 2 + 1, south // 2
                for y in range(y0, y1 + 1):
                    for x in range(x0, x1 + 1):
                        inside.add((x, y))
                        if kind == "slab":
                            self.mb.set_floor(de, x, y, ids["curb16"])
                            continue
                        k = ("n" if y == y0 else "s" if y == y1 else "") + ("w" if x == x1 else "e" if x == x0 else "")
                        self.mb.set_floor(de, x, y, ids[self.ROOM_FLOOR[k]] if k else ids[kind])
            self._dayglow_inside.setdefault(de, set()).update(inside)
            n_apron = 0
            if b.get("apron") or b.get("paving"):
                wall_sq = {(hx // 2, hy // 2) for hx, hy in built["walls"]} | {(hx // 2, hy // 2) for hx, hy in built["doors"]}
                self._dayglow_inside[de].update(wall_sq)
                k = int(b.get("apron", 0))
                want = {(x + a, y + c) for x, y in wall_sq for a in range(-k, k + 1) for c in range(-k, k + 1)} if k else set()
                # "paving": boxes of squares paved as sidewalk is (a square, a yard), with its curbs
                want |= {(x, y) for x0, y0, x1, y1 in b.get("paving", []) for x in range(x0, x1 + 1) for y in range(y0, y1 + 1)}
                skip = {(x, y) for x0, y0, x1, y1 in b.get("apron_skip", []) for x in range(x0, x1 + 1) for y in range(y0, y1 + 1)}
                pave = {q for q in want if q not in inside and q not in skip and 0 <= q[0] < 100 and 0 <= q[1] < 100
                        and (fl[q[1] * 100 + q[0]] in sand or fl[q[1] * 100 + q[0]] <= 1)}
                # at the wall itself there is always sidewalk: a lifted street's asphalt or gravel
                # that reaches the building is paved over for one square
                at_wall = {(x + a, y + c) for x, y in wall_sq for a in (-1, 0, 1) for c in (-1, 0, 1)}
                pave |= {q for q in at_wall if q not in inside and q not in skip and 0 <= q[0] < 100 and 0 <= q[1] < 100
                         and names.get(fl[q[1] * 100 + q[0]], "").startswith(("road", "edg"))}
                rules = json.loads((ROOT / "build" / "dayglow_survey" / "rules.json").read_text())["curb_sig"]

                def cls(q):
                    if q in pave:
                        return "c"
                    if q in inside or q in wall_sq:
                        return "b"
                    t = fl[q[1] * 100 + q[0]] if 0 <= q[0] < 100 and 0 <= q[1] < 100 else 0
                    n = names.get(t, "")
                    return "r" if n.startswith("road") else "c" if n.startswith("curb") or n.startswith("dayg13") else "g" if t in sand else "o"
                for (x, y) in sorted(pave):
                    if (x, y) in wall_sq:
                        name = "curb16"                     # the slab runs on under the wall
                    else:
                        sig = cls((x, y - 1)) + cls((x - 1, y)) + cls((x, y + 1)) + cls((x + 1, y))
                        name = dw.slab_piece(rules, sig, x, y)
                    self.mb.set_floor(de, x, y, ids[name])
                    n_apron += 1
            # An upper floor stands over nothing: outside its dark walls Day Glow sets black wall
            # pieces (a row of black02 and a row of black01 south of a south wall, a column of
            # black03 west of a west wall, black05 at its head and black04 below the corner), so
            # nothing is seen or walked on past the wall, even through a breach.
            if b.get("black_edge"):
                bk = {n: 0x03000004 + n for n in (1, 2, 3, 4, 5)}
                for y, lo, hi in b["xrows"]:
                    if y in b.get("ext_rows", []):
                        for x in range(lo, hi + 1):
                            if x % 2 == 0:
                                put(bk[2], _src.proto(bk[2]).fid, (x, y + 1))
                            else:
                                put(bk[1], _src.proto(bk[1]).fid, (x, y + 2))
                for x in b.get("ext_cols", []):
                    ys = [v for cx, top, bot, _how in b.get("ycols", []) if cx == x for v in (top, bot)]
                    if ys:
                        put(bk[5], _src.proto(bk[5]).fid, (x + 1, min(ys) + 1))
                        for y in range(min(ys) + 3, max(ys) + 2):
                            put(bk[3], _src.proto(bk[3]).fid, (x + 1, y))
                        put(bk[4], _src.proto(bk[4]).fid, (x + 1, max(ys) + 2))
            # the shadow at the foot of the dark walls, as Day Glow lays it (tools/dayglow_ground.py)
            n_shadow = 0
            if b.get("shadow", True):
                import dayglow_ground as dgr
                g = dgr.Ground(self.gi, fl)
                srng = random.Random(b.get("seed", 0) * 101 + 7)
                for y, lo, hi in b["xrows"]:
                    if y in b.get("ext_rows", []):
                        run = [((sx, y // 2 + 1), (sx, y // 2 + 2)) for sx in range(hi // 2, lo // 2 - 1, -1)]
                        n_shadow += g.lay_shadow("south", run, srng)
                for x in b.get("ext_cols", []):
                    ys = [v for cx, top, bot, _how in b.get("ycols", []) if cx == x for v in (top, bot)]
                    if ys:
                        run = [((x // 2, sy), (x // 2 + 1, sy)) for sy in range(min(ys) // 2, max(ys) // 2 + 1)]
                        n_shadow += g.lay_shadow("west", run, srng)
            # the roof, as Day Glow lays one (tools/dayglow_roof.py): tar paper over a one-storey
            # building, the second storey's wall faces as well over a two-storey one ("storeys": 2).
            # Not over an upper floor (its own level is never seen from outside) or a building
            # given "roof": false. Roofs of two buildings that touch hide together in the game.
            n_roof = 0
            if b.get("xrows") and not b.get("black_edge") and b.get("roof", True):
                import dayglow_roof
                roofed = getattr(self, "_dayglow_roofs", {}).setdefault(de, {})
                if not hasattr(self, "_dayglow_roofs"):
                    self._dayglow_roofs = {de: roofed}
                me = b.get("id") or b.get("plan") or str(len(roofed))
                for (sx, sy), name in dayglow_roof.roof(b, int(b.get("storeys", 1)), b.get("seed", 0)).items():
                    if not (0 <= sx < 100 and 0 <= sy < 100):
                        continue
                    if name not in ids:
                        raise SystemExit(f"dayglow {me}: no roof tile {name} in the mod's tiles.lst")
                    self.mb.set_roof(de, sx, sy, ids[name])
                    roofed[(sx, sy)] = me
                    n_roof += 1
                touch = sorted({roofed[n] for (sx, sy), who in roofed.items() if who == me
                                for n in ((sx + 1, sy), (sx - 1, sy), (sx, sy + 1), (sx, sy - 1))
                                if roofed.get(n, me) != me})
                if touch:
                    print(f"  WARNING dayglow {me}: its roof touches the roof of {', '.join(touch)}: they hide together")
            print(f"  dayglow {b.get('id', '?')}: {len(built['walls'])} wall pieces, {len(built['blocks'])} blockers, "
                  f"{len(built['doors'])} doors, {len(b.get('rooms', []))} rooms, {len(b.get('pieces', []))} pieces, {n_apron} squares of sidewalk, {n_shadow} in shadow"
                  + (f", {n_roof} roof tiles ({int(b.get('storeys', 1))} storey{'s' if int(b.get('storeys', 1)) > 1 else ''})" if n_roof else "")
                  + (f"; {missing} pieces the mod has no proto for" if missing else ""))

    def stamps(self, level: dict):
        """Gecko stamps from the library (mod ids): {"file": "Ruins/....json",
        "at": [hx, hy], "variant": 0}. Objects only; a stamp's floor/roof, if
        any, is laid too."""
        de = level["elev"]
        for st in level.get("stamps", []):
            path = STAMPS / st["file"]
            d = json.loads(path.read_text(encoding="utf-8"))
            v = d["variants"][st.get("variant", 0)]
            ax, ay = st["at"]
            # "shell": true keeps the building and leaves its furniture behind
            # (walls, doors, and scenery whose art matches "shell_keep"), so
            # the rooms can be furnished for their use here
            shell = st.get("shell", False)
            shell_keep = [re.compile(x, re.I) for x in st.get("shell_keep", [])]
            n = 0
            for o in v["objects"]:
                pid, fid = o["proPid"], o["frmPid"]
                if (pid >> 24) in (1, 5):
                    continue
                pr = self.gi.proto(pid)
                if pr is None:
                    continue
                if shell and (pid >> 24) == 2 and pr.subtype != 0:
                    art = self.scenery_art_name(fid)
                    if not any(k.search(art) for k in shell_keep):
                        continue
                hx, hy = ax + o["dxHex"], ay + o["dyHex"]
                if 0 <= hx < 200 and 0 <= hy < 200:
                    self.add(pid, fid, hy * HEX_W + hx, de, o.get("direction", 0))
                    n += 1
            print(f"  stamp {st['file']} at {st['at']}: {n} objects")

    def scenery_art_name(self, fid: int) -> str:
        if not hasattr(self, "_sc_lst"):
            self._sc_lst = parse_lst(self.gi.vfs.read("art\\scenery\\scenery.lst"))
        i = fid & 0xFFF
        return self._sc_lst[i] if i < len(self._sc_lst) else ""

    def stamp_fill(self, level: dict):
        """Pack stamps into hex blocks, shelf by shelf, `gap` hexes apart:
        {"rect": [hx0, hy0, hx1, hy1], "files": [...], "gap": 3}. Stamps that
        don't fit are left out (and said so)."""
        for blk in level.get("stamp_fill", []):
            x0, y0, x1, y1 = blk["rect"]
            gap = blk.get("gap", 3)
            x, y, shelf = x0, y0, 0
            placed = []
            for f in blk["files"]:
                d = json.loads((STAMPS / f).read_text(encoding="utf-8"))
                v = d["variants"][0]
                xs = [o["dxHex"] for o in v["objects"]] or [0]
                ys = [o["dyHex"] for o in v["objects"]] or [0]
                w, h = max(xs) - min(xs) + 1, max(ys) - min(ys) + 1
                if x + w > x1:
                    x, y, shelf = x0, y + shelf + gap, 0
                if y + h > y1:
                    print(f"  stamp_fill {blk['rect']}: no room for {f}")
                    continue
                placed.append({"file": f, "at": [x - min(xs), y - min(ys)]})
                x += w + gap
                shelf = max(shelf, h)
            self.stamps({"elev": level["elev"], "stamps": placed})

    def scroll_ring(self, level: dict):
        """Scroll blockers (misc 0x0500000C) on a double hex-rect outline
        {"rect": [hx0, hy0, hx1, hy1]}: the camera can't centre past it."""
        de = level["elev"]
        sr = level.get("scroll_ring")
        if not sr:
            return
        pr = self.gi.proto(SCROLL_BLOCKER)
        x0, y0, x1, y1 = sr["rect"]
        hexes = set()
        for k in (0, 1):
            for x in range(x0 - k, x1 + k + 1):
                hexes.add((x, y0 - k))
                hexes.add((x, y1 + k))
            for y in range(y0 - k, y1 + k + 1):
                hexes.add((x0 - k, y))
                hexes.add((x1 + k, y))
        for hx, hy in sorted(hexes):
            if 0 <= hx < 200 and 0 <= hy < 200:
                self.add(SCROLL_BLOCKER, pr.fid, hy * HEX_W + hx, de)
        print(f"  scroll ring: {len(hexes)} blockers")

    def build(self) -> bytes:
        self._tunnel_sq = {}
        for level in self.r["levels"]:
            print(f"elevation {level['elev']}")
            self.ground(level)
            self.sand(level)
            self.grown_ground(level)
            self.floor_fill(level)
            self.floor_tiles(level)
            self.streets(level)
            for spec in level.get("lift", []):
                self.lift(level, spec)
            self.resand(level)
            self.dayglow(level)
            self.ground_edges(level)
            self.reach(level)
            self.stamps(level)
            self.walls(level)
            self.lines(level)
            self.stamp_fill(level)
            self.openings(level)
            self.tunnels(level)
            self.placed(level)
            self.scatter(level)
            self.exits(level)
            self.scroll_ring(level)
        for src, (conv, _s) in self.converters.items():
            s = conv.stats
            print(f"  converter {src}: kept {s['kept']}, ledger {s['ledger']}, art match {s['artmatch']}, "
                  f"ported {s['ported']}, unresolved {s['unresolved']}")
        return self.mb.to_bytes()


def olympus_pids() -> set[int]:
    """Protos ported from Olympus 2207 (mod/assets.csv). Those are the owner's
    to use; a composed map may not contain any."""
    import csv
    out = set()
    with (ROOT / "mod" / "assets.csv").open(encoding="utf-8") as f:
        for r in csv.DictReader(f):
            if r["source"] == "olympus" and r["kind"] == "proto":
                try:
                    out.add(int(r["dest_id"], 16))
                except ValueError:
                    pass
    return out


def write(name: str, data: bytes, force: bool):
    for d in (AUTHORED, DEPLOYED):
        p = d / name
        if p.exists() and not force:
            sys.exit(f"{p} exists; --force replaces it (the old copy goes to _safety_backups/composed)")
    for d in (AUTHORED, DEPLOYED):
        p = d / name
        if p.exists():
            bk = ROOT / "_safety_backups" / "composed"
            bk.mkdir(parents=True, exist_ok=True)
            shutil.copy2(p, bk / f"{name}.{time.strftime('%Y%m%d_%H%M%S')}")
        d.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
        print(f"wrote {p.relative_to(ROOT)}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("recipe")
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--preview", action="store_true", help="render build/compose/<name>_<elev>.png")
    ap.add_argument("--out", type=Path, help="write the map into this folder (for tools/install_map.py), not the game's")
    args = ap.parse_args()
    recipe = json.loads(Path(args.recipe).read_text(encoding="utf-8"))
    c = Composer(recipe, dry_run=not (args.write or args.out))
    data = c.build()
    olym = olympus_pids()
    n_olym = sum(1 for o in c.mb.objects if o.pid in olym)
    if n_olym:
        msg = f"{n_olym} objects are Olympus assets, which are the owner's only"
        if args.write:
            sys.exit(f"refusing to write {name}: {msg}")
        print(f"WARNING {msg}")
    name = recipe["name"].upper() + ".MAP"
    if args.write:
        write(name, data, args.force)
    if args.out:
        args.out.mkdir(parents=True, exist_ok=True)
        (args.out / name).write_bytes(data)
        print(f"wrote {args.out / name}")
    try:
        m = MapFile.parse(data, c.gi.proto)
    except ValueError as e:
        print(f"{name}: not readable with the deployed protos yet ({e}). "
              f"Next: python tools/deploy.py, python tools/repack_art.py, python tools/deploy.py")
        return 0
    print(f"{name}: {len(m.objects)} objects, elevations {m.elevations}")
    if args.preview:
        import render
        from PIL import Image  # noqa: F401
        out = ROOT / "build" / "compose"
        out.mkdir(parents=True, exist_ok=True)
        from PIL import ImageDraw
        for ei, e in enumerate(m.elevations):
            img = render.render_map(c.gi, m, ei)
            pts = []
            for sq, tid in enumerate(m.floors[ei]):
                if tid > 1:
                    pts.append(render.square_xy(sq))
            for o in m.objects:
                if o.elevation == e and 0 <= o.tile < 40000:
                    x, y = render.hex_xy_screen(o.tile)
                    pts.append((x - 100, y - 200))
                    pts.append((x + 100, y + 60))
            if pts:
                minx = min(p[0] for p in pts)
                miny = min(p[1] for p in pts)
                dr = ImageDraw.Draw(img)
                for sy in range(0, 100, 4):
                    for sx in range(0, 100, 4):
                        if m.floors[ei][sy * 100 + sx] <= 1:
                            continue
                        x, y = render.hex_xy_screen(sy * 2 * 200 + sx * 2)
                        cx, cy = x + 16 - minx, y + 8 - miny
                        dr.rectangle([cx - 2, cy - 2, cx + 2, cy + 2], fill=(255, 255, 0, 255))
                        dr.text((cx + 4, cy - 6), f"{sx},{sy}", fill=(255, 255, 0, 255))
            img = img.convert("RGB")
            img = img.resize((img.width // 2, img.height // 2))
            p = out / f"{recipe['name'].lower()}_{e}.png"
            img.save(p)
            print(f"preview {p.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
