#!/usr/bin/env python3
"""
slice_building.py - cut a building render into Fallout wall pieces, one per hex, the way the
original walls are cut, and its roof into roof tiles, each with its own art (and the walls their
own protos), and write a gecko stamp that lays the whole building down at once.

    python tools/slice_building.py house.png mansn --height 260 --preview build/mansn.png
    python tools/slice_building.py house.png mansn --height 260 --line 10,480 420,786 900,666 \\
        --wall-top 330 --stamp "Mansion" --write
    (in gecko: Tools > Cut Building into Walls, through tools/gecko_assets.py)

The render must be made with the Fallout camera (blender_fallout_dirs.py's, or orthographic at
X 64.3 / Y 0 / Z 150.1 degrees) on a transparent or plain white background, with the walls along
Blender's X or Y axis: then the foot of every wall runs the way a Fallout wall can run.

WHERE A WALL'S PIECES GO (measured, not guessed)
  A wall is one piece per hex (tools/wallkit.py). tile.cc tileToScreenXY: hex y+1 is (+16, +12)
  on screen, so a y-run (one hex column) runs down-right at 37 degrees; hex x+1 is (-32, 0) or
  (-16, +12) by column parity, so an x-run (one hex row) runs up-right at 14 degrees. The plain
  pieces of Fallout 2's own runs say which screen columns each hex draws (object.cc
  objectGetRect: left = hex x + 16 - width/2 + x offset):
      x-run, even x   dv1000.frm  32 px wide, x offset 0    owns [cx-16, cx+16)
      x-run, odd x    dv1004.frm  16 px wide, x offset 8    owns [cx, cx+16)
      y-run           dv1019.frm  16 px wide, x offset -7   owns [cx-15, cx+1)
  (cx, cy: the hex centre, tileToScreenXY + (16, 8)). The spans tile a run exactly, so cutting
  on them gives the slices a vanilla wall has, each drawn by the hex it stands on. Their feet sit
  on the run's own line: cy+6 at an even x-run hex, cy+2 at a y-run hex (y offsets 6 and 2).

WHERE A ROOF'S TILES GO
  A roof is roof tiles on the square grid, drawn over everything and hidden while the player
  stands on a roofed square (tile.cc tileRenderRoofsInRect, tile_fill_roof). tileSetCenter puts
  the square grid's origin at the hex grid's minus (16, 2); squareTileToRoofScreenXY steps x+1
  by (-48, +12) and y+1 by (+32, +24), then lifts the tile 96 px. A tile is an 80x36 picture
  drawn at that point, colour 0 see-through (_dark_trans_buf_to_buf); the square's own share of
  the screen is the parallelogram (0,12) (48,0) (80,24) (32,36) inside it, the shape a vanilla
  tile is drawn in (brick01.frm). So each roof pixel goes to the square whose parallelogram holds
  it; a tile left with a few pixels hands them to neighbouring tiles whose 80x36 box covers them,
  and is not made, to save slots. A tile is a new picture in a spare line of tiles.lst
  (tile_slots.py: 4096 tile ids at most, the base uses 4077, and repeats and empty lines are what
  is spare), named PREFIXr01.frm, PREFIXr02.frm, ...

THE CUT
  --line is the foot of the building in the picture, left to right: two points for one wall,
  three for a corner (left end, corner, right end), more for more corners; 'auto' guesses it
  as the lowest V of the two wall slopes that nothing hangs below, so the feet (wall bottoms,
  posts, stilts) decide it and an eave does not. Each stretch must run within 10 degrees of 14
  up-right or 37 down-right (scaling keeps angles). The first point goes on the foot line of hex
  START at the left edge of its span; at each corner the next stretch turns onto the other axis at
  the hex under the corner. --wall-top is the walls' height above the foot, in picture pixels
  ('auto': 45% of the building's height; 'none': no roof, everything is wall): whatever is above
  that line is roof. Every screen column of the rest belongs to the run of the stretch over it
  (the end runs carry on under any overhang), and in that run to the hex whose span holds it. A
  piece is that hex's columns, trimmed to its opaque rows, with header offsets that put each pixel
  back where it was: walls and roof together draw exactly as rendered.

THE PIECES
  The picture is cut out (a transparent background is kept, else image_scenery.cut_out), scaled to
  --height game pixels (or --scale), its outline darkened (--rim) the way pre-renders fringe
  against black, and put in the palette (colours 1-228, no dither); --colour walls first moves its
  colours toward vanilla walls (ADB1000, MMB1000, DV1000). It is not relit: a render is lit
  already. Each wall piece's proto is a copy of the plain run piece's (x-run 0x0300029A, dv1000,
  East/West light; y-run 0x030002AD, dv1019, North/South), so it blocks its hex and turns
  see-through like a wall when the player is behind it. Wall names are PREFIX001.frm, PREFIX002.frm,
  and so on; art and protos are appended to mod/ids/art_walls.txt and proto_walls.txt and listed
  in keep_protos.txt, so repack_art.py keeps them below the engine's 4096 cap.

THE STAMP
  A gecko pattern (pattern/PatternSerializer.cpp) in gecko's patterns\\Custom folder: the wall
  pieces at their hex offsets from START and the roof tiles at their square offsets from START's
  square, whose column and row parity gecko keeps when stamping (PatternStamper::plan), so slices
  and tiles meet wherever it is put. repack_art.py reads that folder, so the stamp's art ids follow
  a repack. Use it after Tools > Apply New Art: Edit > Stamp Pattern > Custom.

Not made: doors, an inside. The building is a shell whose foot blocks; the ground behind the front
walls does not block, so put blockers there if the player can walk round it. The roof is drawn
over everything, so a critter standing at a front wall lower than it is tall has its head drawn
under the roof's edge (a warning says so below 72 px); and the roof hides while the player stands
on a square it covers, as any Fallout roof does.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

sys.path.insert(0, str(Path(__file__).resolve().parent))

import image_scenery as isc  # noqa: E402
import tile_slots as ts  # noqa: E402
from foclib import GameIndex, parse_lst  # noqa: E402
from ids import ENGINE_ART_NAME_MAX, FOC_START  # noqa: E402
from mirror_scenery import next_index, registry_text  # noqa: E402
from replace_pc_from_ref import decode_frm, encode_frm, load_8bit_palette  # noqa: E402
from scale_critter import FIXED  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
IDS = ROOT / "mod" / "ids"
ART = ROOT / "mod" / "data" / "art" / "walls"
PROTO = ROOT / "mod" / "data" / "proto" / "walls"
from foclib import config as _config  # noqa: E402
PATTERNS = _config.patterns_dir() / "Custom"

START = (100, 101)          # even column (y-runs), odd row (x-runs), as tools/wallsets.py lays runs
TEMPLATE = {"x": 0x0300029A, "y": 0x030002AD}
AXIS_ANGLE = {"x": -math.degrees(math.atan(0.25)), "y": math.degrees(math.atan(0.75))}
AXIS_NAME = {"x": "14 degrees up-right (hex x-run)", "y": "37 degrees down-right (hex y-run)"}
MAX_OFF_AXIS = 10.0
WARN_OFF_AXIS = 4.0
AUTO_WALL_TOP = 0.45        # of the building's height
LOW_WALL = 72               # game px: a critter at the wall is about this tall
SMALL_TILE = 40             # px: a roof tile with fewer is folded into its neighbours if it can be
TILE_W, TILE_H = 80, 36
WALL_REFS = ["ADB1000.FRM", "MMB1000.FRM", "DV1000.FRM"]
PLAYER_FRM = "art\\critters\\hmjmpsaa.frm"
GROUND, GRID, WALL_HEX, PLAYER_HEX = (60, 52, 40), (88, 78, 60), (90, 200, 220), (150, 150, 150)
CUT, ROOF_CELL = (230, 60, 200), (240, 150, 40)


class Refused(Exception):
    """A request the caller should show as a plain message."""


# ------------------------------------------------------------------------------ the grids

def hex_centre(x: int, y: int) -> tuple[int, int]:
    """tile.cc tileToScreenXY with the view at column 0, plus (16, 8): the point a sprite's
    bottom centre is put on (object.cc objectGetRect)."""
    v3 = 199 - x
    return 48 * (v3 // 2) + (32 if v3 & 1 else 0) + 16 * y + 16, -12 * (v3 // 2) + 12 * y + 8


def span(axis: str, x: int, y: int) -> tuple[int, int]:
    """The screen columns [lo, hi) a run's piece on hex (x, y) draws (see the module notes)."""
    cx, _ = hex_centre(x, y)
    if axis == "y":
        return cx - 15, cx + 1
    return (cx - 16, cx + 16) if x % 2 == 0 else (cx, cx + 16)


def foot_y(axis: str, x: int, y: int, s: float) -> float:
    """Screen y of the run's foot line at screen column s; the run passes through hex (x, y)."""
    if axis == "y":
        cx, cy = hex_centre(x, y)
        return cy + 2 + 0.75 * (s - (cx - 7))
    cx, cy = hex_centre(x if x % 2 == 0 else x - 1, y)      # an even hex of the same row
    return cy + 6 - (s - cx) / 4


def run_hex_at(axis: str, x: int, y: int, s: int) -> tuple[int, int]:
    """The hex of the run through (x, y) whose span holds screen column s."""
    if axis == "y":
        lo, _ = span("y", x, y)
        ny = y + (s - lo) // 16
        if not 0 <= ny < 200:
            raise Refused("The building runs off the hex grid: make it smaller.")
        return x, ny
    for nx in range(200):
        lo, hi = span("x", nx, y)
        if lo <= s < hi:
            return nx, y
    raise Refused("The building runs off the hex grid: make it smaller.")


def roof_tile_origin(sx: int, sy: int) -> tuple[int, int]:
    """Top-left of square (sx, sy)'s roof tile, in hex_centre's frame (tileSetCenter: square grid
    origin = hex grid origin - (16, 2); squareTileToRoofScreenXY: x+1 (-48, +12), y+1 (+32, +24),
    96 up)."""
    m = 99 - sx
    return -16 + 48 * m + 32 * sy, -98 - 12 * m + 24 * sy


def roof_square_of(px: np.ndarray, py: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """The square whose roof tile's parallelogram holds the screen points (px, py): solve
    p = (-16, -86) + a (48, -12) + b (32, 24), the parallelogram's left corner stepping by m = 99 - sx
    and by sy. Pixel centres (x + 0.5) never fall on an edge, so each pixel has one square."""
    dx, dy = px + 16.0, py + 86.0
    a = np.floor((dx * 24 - dy * 32) / 1536.0).astype(int)
    b = np.floor((dx * 12 + dy * 48) / 1536.0).astype(int)
    return 99 - a, b


def tile_cell_mask() -> np.ndarray:
    """The pixels of an 80x36 tile inside its own square's parallelogram."""
    ys, xs = np.mgrid[0:TILE_H, 0:TILE_W]
    ox, oy = roof_tile_origin(50, 50)
    sx, sy = roof_square_of(xs + ox + 0.5, ys + oy + 0.5)
    return (sx == 50) & (sy == 50)


# ------------------------------------------------------------------------------ the picture

def wall_ref_lab(gi: GameIndex, pal: np.ndarray, names: list[str]) -> np.ndarray:
    walls = [n.split(",")[0] for n in parse_lst(gi.vfs.read("art\\walls\\walls.lst"))]
    low = [n.lower() for n in walls]
    out = []
    for n in names:
        if n.lower() not in low:
            continue
        m = decode_frm(gi.vfs.read("art\\walls\\" + walls[low.index(n.lower())]))
        px = np.frombuffer(m["frames"][0][0].px, np.uint8)
        out.append(pal[px[px > 0]])
    if not out:
        raise Refused("none of the vanilla wall pictures for --colour walls are in the game")
    return isc.to_lab(np.concatenate(out).astype(np.float32))


def quantise(rgb: np.ndarray, pal: list) -> np.ndarray:
    """scale_critter.Quant's choice (nearest fixed colour 1-228, by the first colour seen with
    each 6-bit key), done once per key instead of once per pixel."""
    keys = (rgb[:, 0].astype(np.int32) >> 2) << 12 | (rgb[:, 1].astype(np.int32) >> 2) << 6 | (rgb[:, 2].astype(np.int32) >> 2)
    uniq, first, inverse = np.unique(keys, return_index=True, return_inverse=True)
    reps = rgb[first].astype(np.int64)
    fixed = np.array(FIXED)
    fpal = np.array([pal[j] for j in FIXED], np.int64)
    best = np.empty(len(uniq), np.uint8)
    for a in range(0, len(uniq), 4096):
        d = ((reps[a:a + 4096, None, :] - fpal[None, :, :]) ** 2).sum(axis=2)
        best[a:a + 4096] = fixed[d.argmin(axis=1)]
    return best[inverse.ravel()]


def alpha_of(picture: Image.Image, white: int, sat: int) -> np.ndarray:
    rgba = np.asarray(picture.convert("RGBA"))
    if "A" in picture.getbands() and rgba[..., 3].min() < 255:
        return np.where(rgba[..., 3] >= 128, 255, 0).astype(np.uint8)
    return isc.cut_out(np.ascontiguousarray(rgba[..., :3]), white, sat)


def prepare(picture: Image.Image, height: int | None, scale: float | None, pal: list, ref,
            white: int = 150, sat: int = 28, rim: float = 0.25):
    """The picture cut out, scaled and in the palette: (index array, (x0, y0, s)) where a picture
    pixel p lands at (p - (x0, y0)) * s."""
    import cv2
    rgb = np.ascontiguousarray(np.asarray(picture.convert("RGB")))
    alpha = alpha_of(picture, white, sat)
    ys, xs = np.nonzero(alpha)
    if not len(ys):
        raise Refused("Nothing left of the picture once its background is taken out.")
    y0, y1, x0, x1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
    rgb, alpha = rgb[y0:y1, x0:x1], alpha[y0:y1, x0:x1]
    if scale is None:
        if not height:
            raise Refused("Give a height in game pixels, or a scale.")
        scale = height / rgb.shape[0]
    w, h = max(1, round(rgb.shape[1] * scale)), max(1, round(rgb.shape[0] * scale))
    if w * h > 4000 * 2000:
        raise Refused(f"{w} x {h} game pixels is far too big for one building.")
    a = alpha.astype(np.float32) / 255.0
    pm = np.dstack([rgb * a[..., None], a * 255]).astype(np.float32)
    small = cv2.resize(pm, (w, h), interpolation=cv2.INTER_AREA if scale < 1 else cv2.INTER_LANCZOS4)
    sa = np.clip(small[..., 3] / 255.0, 0, 1)
    opaque = sa >= 0.5
    col = np.clip(small[..., :3] / np.maximum(sa, 1e-3)[..., None], 0, 255)
    lab = isc.to_lab(col[opaque])
    if ref is not None:
        lab = isc.shade(lab, ref)
    edge = opaque & ~ndimage.binary_erosion(opaque)
    lab[edge[opaque], 0] *= (1.0 - rim)
    idx = np.zeros(opaque.shape, np.uint8)
    idx[opaque] = quantise(isc.to_rgb(lab), pal)
    return idx, (int(x0), int(y0), float(scale))


def auto_line(picture: Image.Image, white: int = 150, sat: int = 28) -> list[tuple[float, float]]:
    """A guess at the foot, in picture pixels. The walls' feet can only run at the two wall
    slopes, so the foot is the lowest V of those slopes (37 degrees down to a corner, 14 up from
    it) that nothing in the picture hangs below: its corner is where the V is highest, touching
    the building's lowest points (wall bottoms, posts, stilts), not an eave or a roof edge.
    A corner at either end leaves one straight wall."""
    alpha = alpha_of(picture, white, sat) > 0
    cols = np.nonzero(alpha.any(axis=0))[0]
    if not len(cols):
        raise Refused("Nothing left of the picture once its background is taken out.")
    x = cols.astype(np.float64)
    env = np.array([np.nonzero(alpha[:, c])[0].max() for c in cols], np.float64)   # lowest pixel per column
    down, up = 0.75, 0.25
    # for a corner at x[i]: the V must pass at or below every column, so its corner y is at least
    #   env[j] + down * (x[i] - x[j])   for columns left of it, and
    #   env[j] + up * (x[j] - x[i])     for columns right of it
    left_need = np.maximum.accumulate(env - down * x) + down * x
    right_need = np.maximum.accumulate((env + up * x)[::-1])[::-1] - up * x
    need = np.maximum(left_need, right_need)
    best = need.min()
    ties = np.nonzero(need <= best + 0.5)[0]
    i = int(ties[len(ties) // 2])
    cx, cy = float(x[i]), float(need[i])
    lo, hi = float(x[0]), float(x[-1])
    width = hi - lo + 1
    pts = []
    if cx - lo > 0.12 * width:
        pts.append((lo, cy - down * (cx - lo)))
    pts.append((cx, cy))
    if hi - cx > 0.12 * width:
        pts.append((hi, cy - up * (hi - cx)))
    if len(pts) == 1:                              # a narrow piece: one wall, the longer way
        pts = [(lo, cy - down * (cx - lo)), (cx, cy)] if cx - lo >= hi - cx else [(cx, cy), (hi, cy - up * (hi - cx))]
    return pts


def auto_wall_top(picture: Image.Image, white: int = 150, sat: int = 28) -> float:
    """The guess at the walls' height, in picture pixels: a share of the building's height."""
    rows = np.nonzero((alpha_of(picture, white, sat) > 0).any(axis=1))[0]
    return round(AUTO_WALL_TOP * (rows.max() - rows.min() + 1), 1)


# ------------------------------------------------------------------------------ the cut

def axis_of(p, q) -> tuple[str, float, float]:
    """The wall axis a stretch of the foot runs along, its angle and how far off the axis it is."""
    ang = math.degrees(math.atan2(q[1] - p[1], q[0] - p[0]))
    axis = min(AXIS_ANGLE, key=lambda a: abs(ang - AXIS_ANGLE[a]))
    return axis, ang, abs(ang - AXIS_ANGLE[axis])


def foot_rows(line: list[tuple[float, float]], width: int) -> np.ndarray:
    """The foot's row at every column, the end stretches carried on past the ends."""
    xs = np.array([p[0] for p in line])
    ys = np.array([p[1] for p in line])
    c = np.arange(width, dtype=np.float64)
    out = np.interp(c, xs, ys)
    left = c < xs[0]
    out[left] = ys[0] + (ys[1] - ys[0]) / (xs[1] - xs[0]) * (c[left] - xs[0])
    right = c > xs[-1]
    out[right] = ys[-1] + (ys[-1] - ys[-2]) / (xs[-1] - xs[-2]) * (c[right] - xs[-1])
    return out


def roof_tiles(roof_idx: np.ndarray, tx: int, ty: int) -> list[dict]:
    """The roof pixels (placed at (tx, ty) on the screen) cut into roof tiles."""
    rs, cs = np.nonzero(roof_idx)
    if not len(rs):
        return []
    px, py = cs + tx, rs + ty
    sx, sy = roof_square_of(px + 0.5, py + 0.5)
    if sx.min() < 0 or sx.max() > 99 or sy.min() < 0 or sy.max() > 99:
        raise Refused("The roof runs off the square grid: make the building smaller.")
    tiles: dict[tuple[int, int], dict] = {}
    for key in sorted(set(zip(sx.tolist(), sy.tolist())), key=lambda k: (k[1], -k[0])):
        ox, oy = roof_tile_origin(*key)
        tiles[key] = {"square": key, "origin": (ox, oy), "idx": np.zeros((TILE_H, TILE_W), np.uint8)}
    for x, y, a, b, v in zip(px.tolist(), py.tolist(), sx.tolist(), sy.tolist(), roof_idx[rs, cs].tolist()):
        t = tiles[(a, b)]
        t["idx"][y - t["origin"][1], x - t["origin"][0]] = v
    # fold slivers into neighbours whose 80x36 box covers them (a tile may draw past its own cell)
    for key in sorted(tiles, key=lambda k: int((tiles[k]["idx"] > 0).sum())):
        t = tiles[key]
        n = int((t["idx"] > 0).sum())
        if n >= SMALL_TILE:
            break
        ys, xs = np.nonzero(t["idx"])
        gx, gy = xs + t["origin"][0], ys + t["origin"][1]
        homes = []
        for x, y in zip(gx.tolist(), gy.tolist()):
            best = None
            for other in tiles.values():
                if other is t:
                    continue
                ox, oy = other["origin"]
                if ox <= x < ox + TILE_W and oy <= y < oy + TILE_H and other["idx"][y - oy, x - ox] == 0:
                    if best is None or (other["idx"] > 0).sum() > (best["idx"] > 0).sum():
                        best = other
            if best is None:
                break
            homes.append((best, x, y))
        if len(homes) == n:
            for (other, x, y), v in zip(homes, t["idx"][ys, xs].tolist()):
                other["idx"][y - other["origin"][1], x - other["origin"][0]] = v
            del tiles[key]
    return [t for t in tiles.values() if (t["idx"] > 0).any()]


def cut(idx: np.ndarray, line: list[tuple[float, float]], wall_top: float | None = None):
    """Slice a prepared picture along its foot line (in prepared-picture pixels); with wall_top
    (prepared-picture pixels above the foot), whatever is above the walls becomes roof tiles."""
    if len(line) < 2:
        raise Refused("The foot line needs at least two points.")
    if any(b[0] <= a[0] for a, b in zip(line, line[1:])):
        raise Refused("Give the foot line's points from left to right.")
    warnings = []
    stretches = []
    for i, (p, q) in enumerate(zip(line, line[1:])):
        axis, ang, off = axis_of(p, q)
        if off > MAX_OFF_AXIS:
            runs = f"{abs(ang):.0f} degrees {'down' if ang > 0 else 'up'}-right" if round(abs(ang)) else "level"
            raise Refused(f"Stretch {i + 1} of the foot runs {runs}, but a Fallout wall runs "
                          f"at 14 degrees up-right or 37 degrees down-right. Was the picture rendered with the "
                          f"Fallout camera, and are the points on the foot of the walls?")
        if off > WARN_OFF_AXIS:
            warnings.append(f"stretch {i + 1} is {off:.0f} degrees off a wall's line; its foot will float or sink a little")
        stretches.append(axis)

    # the first point on the foot of START, at the left edge of its span
    a0 = stretches[0]
    lo, _ = span(a0, *START)
    tx = int(round(lo - line[0][0]))
    ty = int(round(foot_y(a0, *START, lo) - line[0][1]))
    runs = [(a0, START)]                          # each stretch's axis and a hex of its run
    bounds = [-10 ** 9]                           # the screen column each stretch starts at
    for i in range(1, len(line) - 1):
        s = int(round(line[i][0] + tx))
        corner = run_hex_at(runs[-1][0], *runs[-1][1], s)
        runs.append((stretches[i], corner))
        bounds.append(s)
    bounds.append(10 ** 9)

    h, w = idx.shape
    roof = []
    if wall_top:
        above = np.arange(h)[:, None] < (foot_rows(line, w) - wall_top)[None, :]
        roof = roof_tiles(np.where(above, idx, 0).astype(np.uint8), tx, ty)
        idx = np.where(above, 0, idx).astype(np.uint8)
        if wall_top < LOW_WALL:
            warnings.append(f"the walls are {wall_top:.0f} px tall: a critter standing at a front wall has its head "
                            f"drawn under the roof's edge (the roof is drawn over everything)")

    owner: dict[tuple[int, int], list[int]] = {}
    axis_of_hex: dict[tuple[int, int], str] = {}
    k = 0
    for c in range(w):
        s = c + tx
        while s >= bounds[k + 1]:
            k += 1
        axis, through = runs[k]
        hx = run_hex_at(axis, *through, s)
        owner.setdefault(hx, []).append(c)
        axis_of_hex.setdefault(hx, axis)

    pieces, empty = [], 0
    for hx, cols in sorted(owner.items(), key=lambda kv: kv[1][0]):
        c0, c1 = min(cols), max(cols) + 1
        part = np.zeros((h, c1 - c0), np.uint8)
        part[:, [c - c0 for c in cols]] = idx[:, cols]
        rows = np.nonzero(part.any(axis=1))[0]
        if not len(rows):
            empty += 1
            continue
        r0, r1 = rows.min(), rows.max() + 1
        part = part[r0:r1]
        ph, pw = part.shape
        cx, cy = hex_centre(*hx)
        xo = (c0 + tx) - cx + pw // 2             # objectGetRect: left = cx - w/2 + xo
        yo = (r0 + ty) - cy + ph - 1              #                top  = cy - h + 1 + yo
        if not (-32768 <= xo < 32768 and -32768 <= yo < 32768):
            raise Refused("A piece's offsets do not fit an FRM: the building is too big.")
        pieces.append({"hex": hx, "axis": axis_of_hex[hx], "idx": part, "xo": int(xo), "yo": int(yo)})
    if not pieces:
        raise Refused("No wall piece has anything in it: the wall top is at or below the foot.")
    summary = {
        "pieces": len(pieces), "empty_hexes": empty, "roof_tiles": len(roof),
        "stretches": [{"axis": a, "along": AXIS_NAME[a]} for a in stretches],
        "warnings": warnings, "origin": [tx, ty],
    }
    return pieces, roof, summary


# ------------------------------------------------------------------------------ the preview

def draw_frame(canvas: Image.Image, pal: np.ndarray, part: np.ndarray, cx: int, cy: int, xo: int, yo: int, ox: int, oy: int):
    """Draw a frame where the engine would: left = cx - w/2 + xo, top = cy - h + 1 + yo."""
    h, w = part.shape
    im = Image.fromarray(np.dstack([pal[part], np.where(part > 0, 255, 0).astype(np.uint8)]), "RGBA")
    canvas.alpha_composite(im, (cx - w // 2 + xo + ox, cy - h + 1 + yo + oy))


def draw_tile(canvas: Image.Image, pal: np.ndarray, tile: dict, ox: int, oy: int):
    """Draw a roof tile at its origin (tileRenderRoof: the picture's top-left at the square's point)."""
    part = tile["idx"]
    im = Image.fromarray(np.dstack([pal[part], np.where(part > 0, 255, 0).astype(np.uint8)]), "RGBA")
    canvas.alpha_composite(im, (tile["origin"][0] + ox, tile["origin"][1] + oy))


def hexagon(cx, cy):
    return [(cx, cy - 8), (cx + 16, cy - 4), (cx + 16, cy + 4), (cx, cy + 8), (cx - 16, cy + 4), (cx - 16, cy - 4)]


def preview(gi: GameIndex, pal, pieces, roof, out: Path) -> dict:
    pal = np.array(pal, np.uint8)
    boxes = []
    for p in pieces:
        cx, cy = hex_centre(*p["hex"])
        h, w = p["idx"].shape
        boxes.append((cx - w // 2 + p["xo"], cy - h + 1 + p["yo"], w, h))
    for t in roof:
        boxes.append((t["origin"][0], t["origin"][1], TILE_W, TILE_H))
    building = list(boxes)
    left = min(b[0] for b in building)
    player = None
    try:
        meta = decode_frm(gi.vfs.read(PLAYER_FRM))
        f = meta["frames"][2][0]
        pidx = np.frombuffer(f.px, np.uint8).reshape(f.h, f.w)
        row = max(p["hex"][1] for p in pieces)
        x = min(p["hex"][0] for p in pieces if p["hex"][1] == row)
        while x < 199:
            cx, cy = hex_centre(x, row)
            if cx - f.w // 2 + meta["xo"][2] + f.w + 12 <= left:
                break
            x += 1
        player = ((x, row), pidx, meta["xo"][2], meta["yo"][2])
        cx, cy = hex_centre(x, row)
        boxes.append((cx - f.w // 2 + player[2], cy - f.h + 1 + player[3], f.w, f.h))
    except (FileNotFoundError, KeyError, IndexError):
        pass
    x0 = min(b[0] for b in boxes) - 30
    y0 = min(b[1] for b in boxes) - 24
    x1 = max(b[0] + b[2] for b in boxes) + 30
    y1 = max(b[1] + b[3] for b in boxes) + 40
    W, H = x1 - x0, y1 - y0
    cell = tile_cell_mask()
    edge = cell & ~ndimage.binary_erosion(cell)

    def panel(cuts: bool) -> Image.Image:
        im = Image.new("RGBA", (W, H), GROUND + (255,))
        d = ImageDraw.Draw(im)
        for y in range(200):
            for x in range(200):
                cx, cy = hex_centre(x, y)
                if x0 - 16 <= cx <= x1 + 16 and y0 - 8 <= cy <= y1 + 8:
                    d.polygon(hexagon(cx - x0, cy - y0), outline=GRID)
        for p in pieces:
            cx, cy = hex_centre(*p["hex"])
            d.polygon(hexagon(cx - x0, cy - y0), outline=WALL_HEX)
        if player:
            (px, py), pidx, pxo, pyo = player
            cx, cy = hex_centre(px, py)
            d.polygon(hexagon(cx - x0, cy - y0), outline=PLAYER_HEX)
            draw_frame(im, pal, pidx, cx, cy, pxo, pyo, -x0, -y0)
        for p in pieces:
            cx, cy = hex_centre(*p["hex"])
            draw_frame(im, pal, p["idx"], cx, cy, p["xo"], p["yo"], -x0, -y0)
        for t in roof:                                        # roofs are drawn after every object
            draw_tile(im, pal, t, -x0, -y0)
        if cuts:
            d = ImageDraw.Draw(im)
            for b in boxes[:len(pieces)]:
                d.line([(b[0] - x0, b[1] - y0), (b[0] - x0, b[1] + b[3] - 1 - y0)], fill=CUT)
            ov = np.zeros((H, W, 4), np.uint8)
            for t in roof:
                ox, oy = t["origin"][0] - x0, t["origin"][1] - y0
                ys, xs = np.nonzero(edge)
                ok = (xs + ox >= 0) & (xs + ox < W) & (ys + oy >= 0) & (ys + oy < H)
                ov[ys[ok] + oy, xs[ok] + ox] = ROOF_CELL + (255,)
            im.alpha_composite(Image.fromarray(ov, "RGBA"))
        return im

    top, bottom = panel(False), panel(True)
    sheet = Image.new("RGBA", (W, 2 * H + 26), (30, 30, 30, 255))
    sheet.alpha_composite(top, (0, 0))
    sheet.alpha_composite(bottom, (0, H + 26))
    ImageDraw.Draw(sheet).text((6, H + 7), "where it is cut: wall pieces (pink cuts, blue hexes), roof tiles (orange)",
                               fill=(220, 220, 220))
    out.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(out)
    return {"preview": str(out), "width": max(b[0] + b[2] for b in building) - left,
            "height": max(b[1] + b[3] for b in building) - min(b[1] for b in building)}


# ------------------------------------------------------------------------------ writing

def wall_names_taken(gi: GameIndex) -> set[str]:
    taken = {n.split(",")[0].strip().lower() for n in parse_lst(gi.vfs.read("art\\walls\\walls.lst"))}
    taken |= {l.strip().lower() for l in registry_text(IDS / "art_walls.txt").splitlines()
              if l.strip() and not l.startswith("#")}
    if ART.is_dir():
        taken |= {p.name.lower() for p in ART.iterdir()}
    return taken


def tile_names_taken(gi: GameIndex) -> set[str]:
    taken = {n.split(",")[0].strip().lower() for n in parse_lst(gi.vfs.read("art\\tiles\\tiles.lst"))}
    taken |= {n.lower() for n in ts.read_slots().values()}
    if ts.TILE_DIR.is_dir():
        taken |= {p.name.lower() for p in ts.TILE_DIR.iterdir()}
    return taken


def wall_name(prefix: str, i: int) -> str:
    return f"{prefix}{i:03d}.frm"


def tile_name(prefix: str, i: int) -> str:
    return f"{prefix}r{i:02d}.frm"


def prefix_problem(prefix: str, count: int, taken: set[str], tiles: int = 0, tiles_taken: set[str] = frozenset()) -> str | None:
    if not re.fullmatch(r"[a-z0-9_]{1,5}", prefix or ""):
        return "The name is 1 to 5 letters, digits or _ (the pieces add 3 characters, and the game keeps 8)."
    if count > 999:
        return "More than 999 wall pieces: make the building smaller."
    if tiles > 99:
        return "More than 99 roof tiles: make the building smaller."
    clash = [wall_name(prefix, i) for i in range(1, count + 1) if wall_name(prefix, i) in taken]
    clash += [tile_name(prefix, i) for i in range(1, tiles + 1) if tile_name(prefix, i) in tiles_taken]
    if clash:
        return f"'{clash[0][:-4]}' is already a picture in the game. Pick another name."
    assert all(len(wall_name(prefix, i)) <= ENGINE_ART_NAME_MAX for i in range(1, count + 1))
    assert all(len(tile_name(prefix, i)) <= ENGINE_ART_NAME_MAX for i in range(1, tiles + 1))
    return None


def stamp_path(stamp_name: str) -> Path:
    safe = re.sub(r'[<>:"/\\|?*]', "", stamp_name).strip().rstrip(".")
    if not safe:
        raise Refused("Give the stamp a name.")
    return PATTERNS / f"{safe}.json"


def tile_frm_bytes(part: np.ndarray) -> bytes:
    """A tile FRM as vanilla's are (brick01.frm): one 80x36 frame, fps 0, no offsets."""
    meta = dict(version=4, fps=0, action=0, fpd=1, xo=[0] * 6, yo=[0] * 6)
    return encode_frm(meta, {0: [(TILE_W, TILE_H, 0, 0, part.tobytes())]}, split=True)


def write(gi: GameIndex, pieces, roof, prefix: str, stamp_name: str, note: str) -> dict:
    """Write the wall pieces, the roof tiles and the stamp; everything is checked first."""
    problem = prefix_problem(prefix, len(pieces), wall_names_taken(gi), len(roof), tile_names_taken(gi) if roof else set())
    if problem:
        raise Refused(problem)
    stamp = stamp_path(stamp_name)
    if stamp.exists():
        raise Refused(f"There is already a stamp called '{stamp.stem}'. Pick another name.")
    slots = ts.spare_slots() if roof else []
    if len(roof) > len(slots):
        raise Refused(f"The roof needs {len(roof)} roof tiles but only {len(slots)} spare tile slots are left. "
                      f"Make it smaller, or keep the roof in the walls.")
    templates = {a: gi.vfs.read(gi.proto_path(pid)) for a, pid in TEMPLATE.items()}

    objects, made = [], []
    for i, p in enumerate(pieces, 1):
        name = wall_name(prefix, i)
        art_idx = next_index(IDS / "art_walls.txt", FOC_START["art_walls"] - 1)      # art is 0-based
        pid_idx = next_index(IDS / "proto_walls.txt", FOC_START["proto_walls"])
        proto_file = f"{pid_idx:08d}.pro"
        if (PROTO / proto_file).exists() or (ART / name).exists():
            raise Refused(f"{proto_file} or {name} already exists: the registry and mod/data disagree")
        proto = bytearray(templates[p["axis"]])
        pid, fid = 0x03000000 | pid_idx, 0x03000000 | art_idx
        struct.pack_into(">I", proto, 0, pid)
        struct.pack_into(">I", proto, 8, fid)
        ART.mkdir(parents=True, exist_ok=True)
        PROTO.mkdir(parents=True, exist_ok=True)
        (ART / name).write_bytes(isc.frm_bytes(p["idx"], p["xo"], p["yo"]))
        (PROTO / proto_file).write_bytes(bytes(proto))
        with open(IDS / "art_walls.txt", "a", newline="\r\n") as fh:            # the registry is CRLF
            fh.write(name + "\n")
        with open(IDS / "proto_walls.txt", "a", newline="\r\n") as fh:
            fh.write(proto_file + "\n")
        with open(IDS / "keep_protos.txt", "a", newline="\r\n") as fh:
            fh.write(f"0x{pid:08X}  # {name}, wall piece of '{stamp.stem}' {note}\n")
        objects.append({"dxHex": p["hex"][0] - START[0], "dyHex": p["hex"][1] - START[1],
                        "proPid": pid, "frmPid": fid, "direction": 0, "flags": 0})
        made.append({"name": name[:-4], "pid": f"0x{pid:08X}"})

    roof_entries, tiles_made = [], []
    base_sq = (START[0] // 2, START[1] // 2)       # PatternStamper::plan: the anchor hex's square
    for i, t in enumerate(roof, 1):
        name = tile_name(prefix, i)
        tid = slots.pop(0)
        ts.TILE_DIR.mkdir(parents=True, exist_ok=True)
        (ts.TILE_DIR / name).write_bytes(tile_frm_bytes(t["idx"]))
        ts.add_slot(tid, name)
        roof_entries.append({"dxTile": t["square"][0] - base_sq[0], "dyTile": t["square"][1] - base_sq[1], "tileId": tid})
        tiles_made.append({"name": name[:-4], "tile": tid})

    pattern = {
        "name": stamp.stem, "version": 1,
        "variants": [{"label": "default", "anchorHex": START[1] * 200 + START[0],
                      "objects": objects, "floor": [], "roof": roof_entries}],
        "source": {"made by": "tools/slice_building.py", "picture": note},
    }
    stamp.parent.mkdir(parents=True, exist_ok=True)
    stamp.write_text(json.dumps(pattern, indent=1), encoding="utf-8")
    return {"made": made, "roof_made": tiles_made, "stamp": str(stamp)}


# ------------------------------------------------------------------------------ main

def parse_points(values: list[str]) -> list[tuple[float, float]] | None:
    if not values or values == ["auto"]:
        return None
    pts = []
    for v in values:
        try:
            x, y = (float(t) for t in v.split(","))
        except ValueError:
            raise Refused(f"'{v}' is not a point: write X,Y in picture pixels")
        pts.append((x, y))
    return pts


def run(picture_path: Path, height, scale, line_args, colour: str, rim: float, wall_top_arg="none"):
    """Prepare and cut: (gi, pal, wall pieces, roof tiles, summary). wall_top_arg is 'none' (no
    roof), 'auto', or the walls' height above the foot in picture pixels."""
    gi = GameIndex(pristine=False)
    pal = load_8bit_palette(gi)
    picture = Image.open(picture_path)
    line = parse_points(line_args) or auto_line(picture)
    if wall_top_arg in (None, "", "none"):
        wall_top = None
    elif wall_top_arg == "auto":
        wall_top = auto_wall_top(picture)
    else:
        try:
            wall_top = float(wall_top_arg)
        except ValueError:
            raise Refused(f"'{wall_top_arg}' is not a wall height: give picture pixels, auto or none")
        if wall_top <= 0:
            raise Refused("The wall height must be above the foot.")
    ref = wall_ref_lab(gi, np.array(pal, np.float32), WALL_REFS) if colour == "walls" else None
    idx, (x0, y0, s) = prepare(picture, height, scale, pal, ref, rim=rim)
    pieces, roof, summary = cut(idx, [((x - x0) * s, (y - y0) * s) for x, y in line],
                                wall_top * s if wall_top else None)
    summary["line"] = [[round(x, 1), round(y, 1)] for x, y in line]
    summary["wall_top"] = wall_top
    summary["wall_top_game"] = round(wall_top * s, 1) if wall_top else None
    summary["scale"] = s
    return gi, pal, pieces, roof, summary


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("picture", type=Path)
    ap.add_argument("prefix", help="piece names: PREFIX001.frm ..., roof tiles PREFIXr01.frm ... (1 to 5 letters)")
    ap.add_argument("--height", type=int, help="the building's height in game pixels")
    ap.add_argument("--scale", type=float, help="instead of --height: game pixels per picture pixel")
    ap.add_argument("--line", nargs="+", default=["auto"], help="the foot, left to right: X,Y X,Y [X,Y ...] or auto")
    ap.add_argument("--wall-top", default="auto",
                    help="the walls' height above the foot in picture pixels, auto, or none (roof stays in the walls)")
    ap.add_argument("--colour", choices=["none", "walls"], default="none")
    ap.add_argument("--rim", type=float, default=0.25)
    ap.add_argument("--stamp", default="", help="the stamp's name (default: the prefix)")
    ap.add_argument("--preview", type=Path)
    ap.add_argument("--write", action="store_true")
    args = ap.parse_args()
    try:
        gi, pal, pieces, roof, summary = run(args.picture, args.height, args.scale, args.line, args.colour,
                                             args.rim, args.wall_top)
        along = ", ".join(s["along"] for s in summary["stretches"])
        roof_text = f", {len(roof)} roof tiles above {summary['wall_top_game']} px" if roof else ""
        print(f"{summary['pieces']} wall pieces along {along}{roof_text}; foot {summary['line']}")
        for w in summary["warnings"]:
            print("warning:", w)
        if args.preview:
            print("preview", preview(gi, pal, pieces, roof, args.preview)["preview"])
        if args.write:
            out = write(gi, pieces, roof, args.prefix.lower(), args.stamp or args.prefix, f"from {args.picture.name}")
            print(f"wrote {len(out['made'])} wall pieces, {len(out['roof_made'])} roof tiles and the stamp {out['stamp']}")
            print("then: tools/gen_ids.py, deploy.py, repack_art.py, validate.py (or Tools > Apply New Art in gecko)")
    except Refused as e:
        sys.exit(str(e))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
