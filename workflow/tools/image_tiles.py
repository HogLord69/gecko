#!/usr/bin/env python3
"""
image_tiles.py - make new floor or roof tiles from a top-down picture of a surface (a photo or a
texture: dirt, floorboards, a tiled floor, shingles, a rug), and a gecko stamp that lays a patch
of them.

    python tools/image_tiles.py dirt.jpg dirt --block 2 --preview build/dirt.png
    python tools/image_tiles.py dirt.jpg dirt --block 2 --repeat-edges --write
    python tools/image_tiles.py shingles.png shngl --block 2 --roof --write
    (in gecko: Tools > Make Tiles from Picture, through tools/gecko_assets.py)

THE GRID (measured, not guessed)
  A tile is one 80x36 picture, drawn with its top-left at its square's point, colour 0 see-through
  (tile.cc tileRenderFloor and tileRenderRoof both use _dark_trans_buf_to_buf). The square's own
  share of it is the parallelogram (0,12) (48,0) (80,24) (32,36), the shape a vanilla tile is
  drawn in (slice_building.tile_cell_mask): its edge from (0,12) to (48,0) runs to the square at
  x-1 and its edge from (0,12) to (32,36) to the square at y+1 (squareTileToScreenXY: x+1 is
  (-48, +12), y+1 is (+32, +24)). A roof tile is the same picture drawn 96 px higher, so one set of
  tiles serves either layer: --roof only changes the colour reference, the preview and the stamp.

THE PICTURE
  It is seen from straight above. Its middle square is used (a picture that is not square loses
  its long sides), --rotate turns it clockwise by any number of degrees (a turn that is not a whole
  quarter keeps the largest square that lies inside the turned picture, so no corner is empty), and
  it is laid over a block of --block x --block squares: its
  x axis along the up-right edges, its y axis along the down-right ones, the picture's top-left
  corner at a square's left corner. Each tile pixel inside its square's parallelogram takes the
  picture's colour at that point, from the picture shrunk first to the screen's density (50 px a
  square along x, 40 along y) and read with wrap-around, so the block's tiles meet each other and
  the next copy of the block without a seam where the picture itself repeats. --repeat-edges makes
  any picture repeat: the picture shifted by half its size is blended in over the outer quarter on
  each side, which carries the middle's pixels to the edges in the same order they had there.
  --even-light divides out the broad light of a photo lit more on one side, which would otherwise
  show as bands where the block repeats.

COLOURS
  Fallout's ground is darker and less colourful than a photo. Unless --own-colours, the picture
  keeps its own colours and contrast but never goes brighter or more colourful than Fallout's own
  tiles: the 90th percentile of lightness and of chroma (Lab) are brought down to those of the
  base game's most used floor tiles (or roof tiles, with --roof), never raised. Then each pixel
  takes the nearest of the palette's fixed colours 1-228 by Lab distance (no dithering, as the
  vanilla art is), never the cycling ones.

THE TILES
  PREFIX01.frm, PREFIX02.frm ... (a prefix of 1 to 6 letters, digits or _), one per square of the
  block, in spare lines of tiles.lst (tile_slots.py: 4096 tile ids at most). The block's tile for
  square (x, y) of a map is the one at column (-x mod block), row (y mod block).

THE STAMP
  A gecko pattern in patterns\\Custom (pattern/PatternSerializer.cpp) laying a patch of PATCH x
  PATCH squares (rounded up to whole blocks) on the floor or the roof, centred on the cursor. Its
  "tileRepeat" is the block's size: gecko snaps the patch to the block's own grid on the map
  (PatternStamper::plan), so every patch meets the last one. A 1x1 tile needs no stamp to paint:
  it is in the tile palette under its name.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parent))

import image_scenery as isc  # noqa: E402
import slice_building as sb  # noqa: E402
import tile_slots as ts  # noqa: E402
from foclib import GameIndex, parse_lst  # noqa: E402
from ids import ENGINE_ART_NAME_MAX  # noqa: E402
from replace_pc_from_ref import decode_frm, load_8bit_palette  # noqa: E402

TILE_W, TILE_H = sb.TILE_W, sb.TILE_H
CORNER = (0.0, 12.0)            # a square's left corner in its own tile
STEP_U = (48.0, -12.0)          # along the picture's x: toward the square at x-1
STEP_V = (32.0, 24.0)           # along the picture's y: toward the square at y+1
TEXELS = (50, 40)               # screen px a square spans along U (|STEP_U| = 49.5) and V (40)
ROOF_UP = 96                    # tile.cc squareTileToRoofScreenXY
MAX_BLOCK = 4
PATCH = 8                       # the stamp's patch, in squares (rounded up to whole blocks)
PREVIEW_PATCH = 6
EDGE_BAND = 0.25                # --repeat-edges: the share of each side that is blended
WORK_SIZE = 768                 # the picture is cut down to this before the work (it ends at 200 px)
TURN_MARGIN = 6                 # a turn that is not a quarter: px kept clear of the blurred corners
MAX_NAME = ENGINE_ART_NAME_MAX

# The base game's most used tiles (Fallout 2 + RPU, over its 176 maps), the colour references.
# Near-black ones (the black fill around maps) are skipped where they are read.
FLOOR_REFS = ["cav4009.frm", "cav4010.frm", "edg5000.frm", "edg5003.frm", "edg2003.frm", "edg5002.frm",
              "edg5001.frm", "edg5004.frm", "edg3000.frm", "edg5005.frm", "edg5006.frm", "edg4000.frm",
              "edg5007.frm", "road02.frm", "curb16.frm", "edg4001.frm", "shore10.frm", "edg4002.frm",
              "edg1001.frm", "edg2001.frm", "edg6008.frm", "edg2000.frm", "edg1002.frm", "edg6011.frm",
              "mstrflr1.frm", "edg4003.frm", "edg4008.frm", "edgs004.frm", "edg1000.frm", "edg6004.frm",
              "brn1000.frm", "crbm004.frm", "brn1001.frm"]
ROOF_REFS = ["tp153.frm", "tp155.frm", "wroof06.frm", "tp156.frm", "tp154.frm", "plk2000.frm", "ruf1000.frm",
             "ruf1001.frm", "fom1000.frm", "plk1000.frm", "tp157.frm", "tp158.frm", "tp031.frm", "tp159.frm",
             "tp096.frm", "tp160.frm", "tp030.frm", "tp090.frm", "tp095.frm", "tp091.frm", "tp097.frm",
             "tp029.frm", "tp099.frm", "adb029.frm", "tp098.frm", "wroof08.frm", "tp107.frm", "tp105.frm"]
DARK_REF = 10.0                 # a reference tile whose median lightness is below this is black fill

GROUND, GRID, PLAYER_HEX = sb.GROUND, sb.GRID, sb.PLAYER_HEX
SQUARE_LINE, BLOCK_LINE = (240, 150, 40), (90, 200, 220)


class Refused(Exception):
    """A request the caller should show as a plain message."""


# ------------------------------------------------------------------------------ the picture

def turned_side(side: float, degrees: float) -> float:
    """The side of the largest upright square inside a square of `side` turned by `degrees`."""
    r = np.radians(degrees)
    return side / (abs(np.cos(r)) + abs(np.sin(r)))


def square_picture(picture: Image.Image, degrees: float = 0.0) -> np.ndarray:
    """The picture's middle square as float RGB, turned `degrees` clockwise, at most WORK_SIZE on a
    side. A whole quarter turn keeps every pixel; any other keeps the largest square that lies
    inside the turned one, so no corner is empty."""
    if not np.isfinite(degrees):
        raise Refused("The turn is a number of degrees.")
    degrees = float(degrees) % 360.0
    quarters = round(degrees / 90.0)
    whole = abs(degrees - quarters * 90.0) < 1e-6
    if picture.mode in ("RGBA", "LA", "P"):
        rgba = picture.convert("RGBA")
        flat = Image.new("RGBA", rgba.size, (128, 128, 128, 255))
        flat.alpha_composite(rgba)
        picture = flat
    rgb = picture.convert("RGB")
    w, h = rgb.size
    if min(w, h) < 8:
        raise Refused("The picture is too small to make a tile from.")
    side = min(w, h)
    left, top = (w - side) // 2, (h - side) // 2
    rgb = rgb.crop((left, top, left + side, top + side))
    if side > WORK_SIZE:
        rgb = rgb.resize((WORK_SIZE, WORK_SIZE), Image.Resampling.LANCZOS)
    if whole:
        return np.ascontiguousarray(np.rot90(np.asarray(rgb, np.float32), -(quarters % 4)))
    n = rgb.size[0]
    rgb = rgb.rotate(-degrees, resample=Image.Resampling.BICUBIC)     # PIL turns anticlockwise
    inner = int(turned_side(n, degrees)) - TURN_MARGIN                # clear of the blurred rim
    if inner < 8:
        raise Refused("The picture is too small to make a tile from.")
    left = (n - inner) // 2
    return np.asarray(rgb.crop((left, left, left + inner, left + inner)), np.float32)


def repeat_edges(a: np.ndarray, band: float = EDGE_BAND) -> np.ndarray:
    """The picture made to repeat: across each axis in turn, the copy shifted by half its size is
    blended in over the outer `band` of each side. The shifted copy's edges are the picture's
    middle, in the order the pixels had there, so the result's opposite edges continue each other."""
    out = a
    for axis in (1, 0):
        n = out.shape[axis]
        x = (np.arange(n) + 0.5) / n
        d = np.minimum(x, 1 - x) / band                 # 0 at the edge, 1 at band from it
        w = np.clip(d, 0, 1)
        w = w * w * (3 - 2 * w)                         # smoothstep
        shape = [1, 1, 1]
        shape[axis] = n
        w = w.reshape(shape)
        out = out * w + np.roll(out, n // 2, axis=axis) * (1 - w)
    return out


def ref_lab(gi: GameIndex, pal: np.ndarray, names: list[str]) -> np.ndarray | None:
    """Lab of the reference tiles' pixels, each tile counted once, the black fill left out."""
    tiles = parse_lst(gi.vfs.read("art\\tiles\\tiles.lst"))
    low = {n.lower(): n for n in tiles}
    out = []
    for n in names:
        if n.lower() not in low:
            continue
        try:
            m = decode_frm(gi.vfs.read("art\\tiles\\" + low[n.lower()]))
        except (FileNotFoundError, KeyError, ValueError):
            continue
        px = np.frombuffer(m["frames"][0][0].px, np.uint8)
        px = px[px > 0]
        if not len(px):
            continue
        lab = isc.to_lab(pal[px].astype(np.float32))
        if np.median(lab[:, 0]) >= DARK_REF:
            out.append(lab[np.random.default_rng(0).choice(len(lab), min(len(lab), 1500), replace=False)])
    return np.concatenate(out) if out else None


def fallout_colours(lab: np.ndarray, ref: np.ndarray) -> np.ndarray:
    """Never brighter or more colourful than the reference: the 90th percentiles of lightness and
    chroma brought down to the reference's, the picture's own colours and contrast kept."""
    l_have, l_want = np.percentile(lab[:, 0], 90), np.percentile(ref[:, 0], 90)
    c_have = np.percentile(np.hypot(lab[:, 1], lab[:, 2]), 90)
    c_want = np.percentile(np.hypot(ref[:, 1], ref[:, 2]), 90)
    lf = min(1.0, l_want / max(l_have, 1e-3))
    cf = min(1.0, c_want / max(c_have, 1e-3))
    return np.stack([lab[:, 0] * lf, lab[:, 1] * cf, lab[:, 2] * cf], axis=1)


def even_light(a: np.ndarray) -> np.ndarray:
    """The picture's light made even: each pixel divided by the brightness of its broad
    surroundings (a blur a sixth of the picture wide, wrapping round), times the picture's mean,
    so a photo lit more on one side does not show bands where its block repeats."""
    from scipy import ndimage
    lum = a.mean(axis=2)
    broad = ndimage.gaussian_filter(lum, a.shape[0] / 6.0, mode="wrap")
    return a * (lum.mean() / np.maximum(broad, 1.0))[..., None]


def quantise_lab(rgb: np.ndarray, pal: list) -> np.ndarray:
    """The nearest of the fixed colours 1-228 by Lab distance (closer to what the eye sees than
    RGB: light, dusty browns otherwise go pink), once per 6-bit colour key."""
    keys = (rgb[:, 0].astype(np.int32) >> 2) << 12 | (rgb[:, 1].astype(np.int32) >> 2) << 6 | (rgb[:, 2].astype(np.int32) >> 2)
    uniq, first, inverse = np.unique(keys, return_index=True, return_inverse=True)
    reps = isc.to_lab(rgb[first].astype(np.float32))
    fixed = np.arange(1, 229)
    flab = isc.to_lab(np.array([pal[j] for j in fixed], np.float32))
    best = np.empty(len(uniq), np.uint8)
    for a in range(0, len(uniq), 4096):
        d = ((reps[a:a + 4096, None, :] - flab[None, :, :]) ** 2).sum(axis=2)
        best[a:a + 4096] = fixed[d.argmin(axis=1)]
    return best[inverse.ravel()]


def cell_uv() -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """For the pixels of a tile inside its own square: the mask, and each pixel centre's position
    in the square (0 to 1 along the picture's x and y), solving p - CORNER = u STEP_U + v STEP_V."""
    mask = sb.tile_cell_mask()
    ys, xs = np.nonzero(mask)
    dx, dy = xs + 0.5 - CORNER[0], ys + 0.5 - CORNER[1]
    det = STEP_U[0] * STEP_V[1] - STEP_U[1] * STEP_V[0]          # 1536
    u = (dx * STEP_V[1] - dy * STEP_V[0]) / det
    v = (dy * STEP_U[0] - dx * STEP_U[1]) / det
    return mask, u, v


def make_tiles(picture: Image.Image, block: int, pal: list, ref: np.ndarray | None, rotate: float = 0.0,
               repeat: bool = False, even: bool = False) -> dict[tuple[int, int], np.ndarray]:
    """The block's tiles, {(column, row): 36x80 palette indices}."""
    import cv2
    if not 1 <= block <= MAX_BLOCK:
        raise Refused(f"The block is 1 to {MAX_BLOCK} squares a side.")
    a = square_picture(picture, rotate)
    if even:
        a = even_light(a)           # first: the blend then joins evenly lit edges
    if repeat:
        a = repeat_edges(a)
    tw, th = TEXELS[0] * block, TEXELS[1] * block
    tex = cv2.resize(a, (tw, th), interpolation=cv2.INTER_AREA)
    mask, u, v = cell_uv()
    ys, xs = np.nonzero(mask)
    tiles: dict[tuple[int, int], np.ndarray] = {}
    samples = {}
    for i in range(block):
        for j in range(block):
            mx = ((i + u) * TEXELS[0] - 0.5).astype(np.float32).reshape(1, -1)
            my = ((j + v) * TEXELS[1] - 0.5).astype(np.float32).reshape(1, -1)
            samples[(i, j)] = cv2.remap(tex, mx, my, cv2.INTER_LINEAR, borderMode=cv2.BORDER_WRAP).reshape(-1, 3)
    rgb = np.concatenate([samples[k] for k in sorted(samples)])
    lab = isc.to_lab(np.clip(rgb, 0, 255))
    if ref is not None:
        lab = fallout_colours(lab, ref)
    idx = quantise_lab(isc.to_rgb(lab), pal)
    n = len(ys)
    for k, key in enumerate(sorted(samples)):
        t = np.zeros((TILE_H, TILE_W), np.uint8)
        t[ys, xs] = idx[k * n:(k + 1) * n]
        tiles[key] = t
    return tiles


def tile_for(tiles: dict, block: int, sx: int, sy: int) -> np.ndarray:
    """The block's tile on map square (sx, sy): the picture's x runs toward smaller x."""
    return tiles[((-sx) % block, sy % block)]


# ------------------------------------------------------------------------------ the preview

def floor_origin(sx: int, sy: int) -> tuple[int, int]:
    """Top-left of square (sx, sy)'s floor tile, in slice_building.hex_centre's frame."""
    x, y = sb.roof_tile_origin(sx, sy)
    return x, y + ROOF_UP


def preview(gi: GameIndex, pal, tiles: dict, block: int, roof: bool, out: Path) -> dict:
    """The patch as the game draws it beside the player (above), and with its squares and blocks
    outlined (below)."""
    pal = np.array(pal, np.uint8)
    n = max(PREVIEW_PATCH, block * 2)
    n = -(-n // block) * block
    s0 = 50 - n // 2
    squares = [(sx, sy) for sy in range(s0, s0 + n) for sx in range(s0 + n - 1, s0 - 1, -1)]
    lift = ROOF_UP if roof else 0
    origins = {sq: (floor_origin(*sq)[0], floor_origin(*sq)[1] - lift) for sq in squares}

    player = None
    try:
        meta = decode_frm(gi.vfs.read(sb.PLAYER_FRM))
        f = meta["frames"][2][0]
        pidx = np.frombuffer(f.px, np.uint8).reshape(f.h, f.w)
        hx = (2 * 50, 2 * 50) if not roof else (2 * (s0 + n // 2), 2 * (s0 + n) + 1)
        player = (hx, pidx, meta["xo"][2], meta["yo"][2])
    except (FileNotFoundError, KeyError, IndexError):
        pass

    boxes = [(ox, oy, TILE_W, TILE_H) for ox, oy in origins.values()]
    if player:
        cx, cy = sb.hex_centre(*player[0])
        h, w = player[1].shape
        boxes.append((cx - w // 2 + player[2], cy - h + 1 + player[3], w, h))
    x0 = min(b[0] for b in boxes) - 24
    y0 = min(b[1] for b in boxes) - 20
    x1 = max(b[0] + b[2] for b in boxes) + 24
    y1 = max(b[1] + b[3] for b in boxes) + 20
    W, H = x1 - x0, y1 - y0
    cell = sb.tile_cell_mask()
    from scipy import ndimage
    edge = cell & ~ndimage.binary_erosion(cell)

    def tile_image(idx: np.ndarray) -> Image.Image:
        return Image.fromarray(np.dstack([pal[idx], np.where(idx > 0, 255, 0).astype(np.uint8)]), "RGBA")

    def panel(lines: bool) -> Image.Image:
        im = Image.new("RGBA", (W, H), GROUND + (255,))
        d = ImageDraw.Draw(im)
        for y in range(200):
            for x in range(200):
                cx, cy = sb.hex_centre(x, y)
                if x0 - 16 <= cx <= x1 + 16 and y0 - 8 <= cy <= y1 + 8:
                    d.polygon(sb.hexagon(cx - x0, cy - y0), outline=GRID)
        def laid() -> None:
            for sq in squares:
                im.alpha_composite(tile_image(tile_for(tiles, block, *sq)), (origins[sq][0] - x0, origins[sq][1] - y0))

        if not roof:
            laid()                                       # the floor is drawn before every object
        if player:
            (px, py), pidx, pxo, pyo = player
            cx, cy = sb.hex_centre(px, py)
            sb.draw_frame(im, pal, pidx, cx, cy, pxo, pyo, -x0, -y0)
        if roof:
            laid()                                       # the roof after every object
        if lines:
            ov = np.zeros((H, W, 4), np.uint8)
            ys, xs = np.nonzero(edge)
            for sq in squares:
                ox, oy = origins[sq][0] - x0, origins[sq][1] - y0
                at_block = (-sq[0]) % block == 0 or sq[1] % block == 0
                colour = BLOCK_LINE if block > 1 and at_block else SQUARE_LINE
                ok = (xs + ox >= 0) & (xs + ox < W) & (ys + oy >= 0) & (ys + oy < H)
                ov[ys[ok] + oy, xs[ok] + ox] = colour + (255,)
            im.alpha_composite(Image.fromarray(ov, "RGBA"))
        return im

    sheet = Image.new("RGBA", (W, 2 * H + 26), (30, 30, 30, 255))
    sheet.alpha_composite(panel(False), (0, 0))
    sheet.alpha_composite(panel(True), (0, H + 26))
    label = "each tile's square (orange)" + (", the block's tiles together (blue edges)" if block > 1 else "")
    ImageDraw.Draw(sheet).text((6, H + 7), label, fill=(220, 220, 220))
    out.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(out)
    return {"preview": str(out)}


# ------------------------------------------------------------------------------ writing

def tile_name(prefix: str, i: int) -> str:
    return f"{prefix}{i:02d}.frm"


def name_problem(prefix: str, count: int, taken: set[str]) -> str | None:
    if not re.fullmatch(r"[a-z0-9_]{1,6}", prefix or ""):
        return "The name is 1 to 6 letters, digits or _ (the tiles add 2 digits, and the game keeps 8)."
    clash = [tile_name(prefix, i) for i in range(1, count + 1) if tile_name(prefix, i) in taken]
    if clash:
        return f"'{clash[0][:-4]}' is already a tile in the game. Pick another name."
    assert all(len(tile_name(prefix, i)) <= MAX_NAME for i in range(1, count + 1))
    return None


def stamp_path(stamp_name: str) -> Path:
    try:
        return sb.stamp_path(stamp_name)
    except sb.Refused as e:
        raise Refused(str(e))


def stamp_pattern(name: str, tile_ids: dict[tuple[int, int], int], block: int, roof: bool, note: str) -> dict:
    """A patch of whole blocks round the anchor's square, each square's tile by the map rule
    (tile_for); with tileRepeat gecko snaps the anchor's square to a multiple of the block."""
    n = -(-PATCH // block) * block
    lo = -(n // 2)
    entries = [{"dxTile": dx, "dyTile": dy, "tileId": tile_ids[((-dx) % block, dy % block)]}
               for dy in range(lo, lo + n) for dx in range(lo, lo + n)]
    variant = {"label": "default", "anchorHex": sb.START[1] * 200 + sb.START[0], "objects": [],
               "floor": [] if roof else entries, "roof": entries if roof else []}
    if block > 1:
        variant["tileRepeat"] = block
    return {"name": name, "version": 1, "variants": [variant],
            "source": {"made by": "tools/image_tiles.py", "picture": note}}


def write(gi: GameIndex, tiles: dict, block: int, roof: bool, prefix: str, stamp_name: str, note: str) -> dict:
    """Write the tiles into spare tile slots and the stamp; everything is checked first."""
    problem = name_problem(prefix, len(tiles), sb.tile_names_taken(gi))
    if problem:
        raise Refused(problem)
    stamp = stamp_path(stamp_name) if stamp_name else None
    if stamp is not None and stamp.exists():
        raise Refused(f"There is already a stamp called '{stamp.stem}'. Pick another name.")
    slots = ts.spare_slots()
    if len(tiles) > len(slots):
        raise Refused(f"The block needs {len(tiles)} tiles but only {len(slots)} spare tile slots are left. "
                      f"Make the block smaller.")
    ids, made = {}, []
    for k, key in enumerate(sorted(tiles, key=lambda c: (c[1], c[0])), 1):
        name = tile_name(prefix, k)
        tid = slots.pop(0)
        ts.TILE_DIR.mkdir(parents=True, exist_ok=True)
        (ts.TILE_DIR / name).write_bytes(sb.tile_frm_bytes(tiles[key]))
        ts.add_slot(tid, name)
        ids[key] = tid
        made.append({"name": name[:-4], "tile": tid, "column": key[0], "row": key[1]})
    if stamp is not None:
        stamp.parent.mkdir(parents=True, exist_ok=True)
        stamp.write_text(json.dumps(stamp_pattern(stamp.stem, ids, block, roof, note), indent=1), encoding="utf-8")
    return {"made": made, "stamp": str(stamp) if stamp else "", "spare_tile_slots": len(slots)}


# ------------------------------------------------------------------------------ main

def run(picture_path: Path, block: int, roof: bool, rotate: float, repeat: bool, own_colours: bool,
        even: bool = False):
    """(gi, pal, tiles): the block's tiles made from the picture."""
    gi = GameIndex(pristine=False)
    pal = load_8bit_palette(gi)
    try:
        picture = Image.open(picture_path)
        picture.load()
    except OSError as e:
        raise Refused(f"Couldn't read the picture: {e}")
    ref = None if own_colours else ref_lab(gi, np.array(pal, np.float32), ROOF_REFS if roof else FLOOR_REFS)
    return gi, pal, make_tiles(picture, block, pal, ref, rotate, repeat, even)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("picture", type=Path)
    ap.add_argument("prefix", help="tile names: PREFIX01.frm ... (1 to 6 letters)")
    ap.add_argument("--block", type=int, default=2, help=f"squares a side the picture covers, 1 to {MAX_BLOCK}")
    ap.add_argument("--roof", action="store_true", help="roof tiles: a roof colour reference and a roof stamp")
    ap.add_argument("--rotate", type=float, default=0.0,
                    help="degrees clockwise, any number (a minus number turns it left)")
    ap.add_argument("--repeat-edges", action="store_true", help="blend the edges so the picture repeats")
    ap.add_argument("--even-light", action="store_true", help="even out a photo's uneven light")
    ap.add_argument("--own-colours", action="store_true", help="keep the picture's brightness and colour")
    ap.add_argument("--stamp", default="", help="the stamp's name (default: the prefix)")
    ap.add_argument("--preview", type=Path)
    ap.add_argument("--write", action="store_true")
    args = ap.parse_args()
    try:
        gi, pal, tiles = run(args.picture, args.block, args.roof, args.rotate, args.repeat_edges, args.own_colours,
                             args.even_light)
        print(f"{len(tiles)} tiles, {len(ts.spare_slots())} spare tile slots")
        if args.preview:
            print("preview", preview(gi, pal, tiles, args.block, args.roof, args.preview)["preview"])
        if args.write:
            out = write(gi, tiles, args.block, args.roof, args.prefix.lower(), args.stamp or args.prefix,
                        f"from {args.picture.name}")
            print(f"wrote {', '.join(m['name'] for m in out['made'])} and the stamp {out['stamp']}")
            print("then: tools/gen_ids.py, deploy.py, validate.py (or Tools > Apply New Art in gecko)")
    except Refused as e:
        sys.exit(str(e))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
