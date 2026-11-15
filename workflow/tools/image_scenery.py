#!/usr/bin/env python3
"""
image_scenery.py - turn a picture of an object on a white background (a
render or a photo) into a new FOC scenery piece shaded like Fallout 2's own
pre-rendered art, with its own art and proto, and optionally a ground shadow
object like the Pep Gas sign's (GASSIGN2.FRM).

    python tools/image_scenery.py sign.webp cldrvsgn.frm --height 300 --preview build/s.png
    python tools/image_scenery.py sign.webp cldrvsgn.frm --height 300 --shadow cldrvshd.frm --write
    python tools/image_scenery.py sign.webp cldrvsgn.frm --height 300 --shadow cldrvshd.frm --match-colour --redo

The drive-in sign (cldrvsgn, cldrvshd) is made with --match-colour --grain 2.5
--rim 0.35: the owner liked the vanilla-matched colour best. The finned car
(clcarsw, clcarse, clcarne, clcarnw: one piece per floor-axis view, rendered
by blender_fallout_dirs.py ... floor) is matched to the vanilla cars with
--ref car1.frm,car2.frm,car4.frm,scar401.frm --match-amount 0.8 --chroma 0.85
--centre, all four at one scale (0.205 of the renders), which makes the sw14
view as wide as CAR1.FRM.

Owner, 2026-10-27: "make this an object, It is a large drive in sign, match
the shading to other fallout 2 assets", then "the color is okay just give it
correct lighting and shadows", then of the --match-colour look: "I actually
like this color change for the sign".

1. Cut-out: background is the near-white, low-saturation pixels connected
   to the image border (flood fill), which takes the soft grey floor shadow
   of a studio render too; enclosed white (a lightbox, white stars) stays.
   A picture with a transparent background keeps its own alpha instead.
2. Scale: premultiplied Lanczos down to --height pixels, alpha cut hard at
   50% like the game's sprites.
3. Lighting, the picture's own colours kept: Fallout 2's scenery is lit
   from the front, high and a little from the left. As New Reno places the
   Pep Gas sign (newrcs.map: GASSIGN1 at (148,118), its shadow GASSIGN2 at
   (142,114)), the shadow falls behind it, up the screen and a little to
   the right. The object's silhouette is blown up into a soft height field
   whose normals light it Lambert-style from that side, so posts and box
   edges turn round with their left and top sides lit and their right sides
   in shade; a broad gradient darkens the whole piece toward the lower right
   (--falloff); the foot darkens where it meets the ground (--ao). A light
   grain (--grain, Lab L sigma) stands in for the noise of Fallout's
   renders, and the outline pixels are darkened (--rim) the way the
   pre-renders fringe against black. --match-colour also maps the lightness
   and chroma onto the --ref FRMs' (dark and dusty, like the vanilla pieces).
4. Palette: nearest of the fixed colours 1-228 (scale_critter.Quant), no
   dithering, as the vanilla scenery is.
5. Anchor: the bottom centre of the lowest opaque row (where the object
   stands) is put on the hex, through the header offsets.
6. Shadow (--shadow NAME): the silhouette thrown onto the ground behind
   the object, in GASSIGN2's direction (0.08 px right and 0.42 px up the
   screen for each pixel of height), one near-black colour like GASSIGN2,
   on its own proto copied from --shadow-proto. It belongs on the hex just
   north of the object's (tile - 200, the same x and one row up): the
   engine draws that hex first, so the object covers the part of the
   shadow behind it, as the pole hides the Pep Gas sign's.

The protos are byte copies of --proto / --shadow-proto with PID and FID
repointed (as mirror_scenery.py does), appended to the ID registry and to
mod/ids/keep_protos.txt. Afterwards give them names in
mod/ids/scenery_text.tsv and run scenery_text.py, gen_ids.py, deploy.py,
repack_art.py and validate.py.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

import cv2
import numpy as np
from PIL import Image
from scipy import ndimage

sys.path.insert(0, str(Path(__file__).resolve().parent))

from foclib import GameIndex, parse_lst  # noqa: E402
from ids import FOC_START, check_art_name  # noqa: E402
from mirror_scenery import ART, IDS, PROTO, next_index  # noqa: E402
from replace_pc_from_ref import decode_frm, encode_frm, load_8bit_palette  # noqa: E402
from scale_critter import Quant  # noqa: E402

SHADOW_DX, SHADOW_DY = 0.08, 0.42       # GASSIGN2 against GASSIGN1 as placed: right, and up the screen, per px of height
SHADOW_HEX = (-16, -12)                 # the shadow's hex (tile - 200) on screen, from the object's
LIGHT = np.array([-0.30, -0.45, 0.84])  # screen x right, y down, z toward the viewer: front, high, a little left
LIGHT = LIGHT / np.linalg.norm(LIGHT)


def to_lab(rgb: np.ndarray) -> np.ndarray:
    return cv2.cvtColor((rgb / 255.0).astype(np.float32).reshape(-1, 1, 3),
                        cv2.COLOR_RGB2LAB).reshape(-1, 3)


def to_rgb(lab: np.ndarray) -> np.ndarray:
    rgb = cv2.cvtColor(lab.astype(np.float32).reshape(-1, 1, 3), cv2.COLOR_LAB2RGB).reshape(-1, 3)
    return np.clip(np.round(rgb * 255), 0, 255).astype(np.uint8)


def cut_out(rgb: np.ndarray, white: int, sat: int) -> np.ndarray:
    """Alpha (0/255): everything but the light, grey background touching the border."""
    hi, lo = rgb.max(axis=2).astype(int), rgb.min(axis=2).astype(int)
    bg_like = (lo >= white) & (hi - lo <= sat)
    lab, _ = ndimage.label(bg_like)
    border = set(np.unique(np.concatenate([lab[0], lab[-1], lab[:, 0], lab[:, -1]]))) - {0}
    bg = np.isin(lab, list(border))
    fg = ~bg
    fg = ndimage.binary_opening(fg, iterations=1)
    # drop specks: keep the largest piece and anything big near it
    lab, n = ndimage.label(fg)
    if n > 1:
        sizes = ndimage.sum(fg, lab, range(1, n + 1))
        keep = [i + 1 for i, s in enumerate(sizes) if s >= 0.02 * sizes.max()]
        fg = np.isin(lab, keep)
    return np.where(fg, 255, 0).astype(np.uint8)


def ref_lab(gi, pal, names) -> np.ndarray:
    art = parse_lst(gi.vfs.read("art\\scenery\\scenery.lst"))
    low = [a.lower() for a in art]
    out = []
    for n in names:
        m = decode_frm(gi.vfs.read("art\\scenery\\" + art[low.index(n.lower())]))
        for f in m["frames"][0][:1]:
            px = np.frombuffer(f.px, np.uint8)
            out.append(pal[px[px > 0]])
    return to_lab(np.concatenate(out).astype(np.float32))


def shade(lab: np.ndarray, ref: np.ndarray, amount: float = 1.0, chroma: float | None = None) -> np.ndarray:
    q = np.linspace(0, 100, 101)
    have, want = np.percentile(lab[:, 0], q), np.percentile(ref[:, 0], q)
    have = np.maximum.accumulate(have + np.arange(101) * 1e-6)
    L = lab[:, 0] * (1 - amount) + np.interp(lab[:, 0], have, want) * amount
    if chroma is None:
        c_have = np.percentile(np.hypot(lab[:, 1], lab[:, 2]), 90)
        c_want = np.percentile(np.hypot(ref[:, 1], ref[:, 2]), 90)
        chroma = min(1.0, c_want / max(c_have, 1e-3))
    return np.stack([L, lab[:, 1] * chroma, lab[:, 2] * chroma], axis=1)


def light(opaque: np.ndarray, radius: float, falloff: float, ao: float) -> np.ndarray:
    """A brightness factor per pixel (1 = as drawn) for light from the upper right."""
    h, w = opaque.shape
    dist = ndimage.distance_transform_edt(opaque)
    field = ndimage.gaussian_filter(np.minimum(dist, radius * 2), radius)
    gy, gx = np.gradient(field)
    n = np.dstack([-gx, -gy, np.full_like(gx, 0.8)])
    n /= np.linalg.norm(n, axis=2, keepdims=True)
    amb, kd = 0.35, 0.65
    diffuse = np.clip(n @ LIGHT, 0, 1)
    f = (amb + kd * diffuse) / (amb + kd * LIGHT[2] / np.linalg.norm([0, 0, 1]))
    # the whole piece a little darker toward the lower left, away from the light
    ys, xs = np.nonzero(opaque)
    u = ((np.arange(w)[None, :] - xs.mean()) / max(1, np.ptp(xs)) * LIGHT[0]
         + (np.arange(h)[:, None] - ys.mean()) / max(1, np.ptp(ys)) * LIGHT[1])
    f *= 1 + falloff * np.clip(u / (abs(LIGHT[0]) + abs(LIGHT[1])) * 2, -1, 1)
    # the foot, where it meets the ground
    base = ys.max()
    rise = (base - np.arange(h)[:, None]) / max(1, 0.12 * np.ptp(ys))
    f *= 1 - ao * np.clip(1 - rise, 0, 1)
    return f


CROP = {}                                   # the last build's crop box and scale, for --stand


def build(img: Image.Image, height: int, ref, pal, quant, white, sat, grain, rim, seed,
          radius, falloff, ao, amount=1.0, chroma=None, scale=None):
    rgb = np.asarray(img.convert("RGB"))
    # a picture already cut out (transparent background) keeps its own alpha
    own = np.asarray(img.convert("RGBA"))[..., 3] if "A" in img.getbands() else None
    if own is not None and own.min() < 255:
        alpha = np.where(own >= 128, 255, 0).astype(np.uint8)
    else:
        alpha = cut_out(rgb, white, sat)
    ys, xs = np.nonzero(alpha)
    y0, y1, x0, x1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
    rgb, alpha = rgb[y0:y1, x0:x1], alpha[y0:y1, x0:x1]
    if scale:
        height = max(1, round(rgb.shape[0] * scale))
    s = height / rgb.shape[0]
    CROP.update(x0=x0, y0=y0, s=s)
    w = max(1, round(rgb.shape[1] * s))
    a = alpha.astype(np.float32) / 255.0
    pm = np.dstack([rgb * a[..., None], a * 255]).astype(np.float32)
    small = cv2.resize(pm, (w, height), interpolation=cv2.INTER_AREA)
    sa = small[..., 3] / 255.0
    opaque = sa >= 0.5
    col = small[..., :3] / np.maximum(sa, 1e-3)[..., None]
    lab = to_lab(col[opaque])
    if ref is not None:
        lab = shade(lab, ref, amount, chroma)
    f = light(opaque, radius, falloff, ao)[opaque]
    lab[:, 0] = np.clip(lab[:, 0] * f, 0, 100)
    lab[:, 1:] *= np.sqrt(np.clip(f, 0, 1.5))[:, None]
    rng = np.random.default_rng(seed)
    lab[:, 0] = np.clip(lab[:, 0] + rng.normal(0, grain, len(lab)), 0, 100)
    # outline pixels go darker, as the pre-renders fringe against black
    edge = opaque & ~ndimage.binary_erosion(opaque)
    lab[edge[opaque], 0] *= (1.0 - rim)
    out = np.zeros(opaque.shape, np.uint8)
    out[opaque] = [quant(tuple(int(v) for v in c)) for c in to_rgb(lab)]
    return out


def anchor(idx: np.ndarray) -> tuple[int, int]:
    """Header offsets that put the bottom centre of the lowest opaque row on the hex."""
    h, w = idx.shape
    rows = np.nonzero(idx.any(axis=1))[0]
    base = rows.max()
    xs = np.nonzero(idx[base])[0]
    foot = (xs.min() + xs.max()) / 2.0
    return int(round(w / 2.0 - foot)), int(h - 1 - base)


def anchor_centre(idx: np.ndarray) -> tuple[int, int]:
    """Header offsets that put the middle of the piece on the hex, as the
    vanilla cars stand (CAR1.FRM's hex is near its centre)."""
    h, w = idx.shape
    ys, xs = np.nonzero(idx)
    return int(round(w / 2.0 - xs.mean())), int(round(h - 1 - ys.mean()))


def anchor_at(idx: np.ndarray, x: float, y: float, dx: float = 0.0, dy: float = 0.0) -> tuple[int, int]:
    """Header offsets that put source-image pixel (x, y) on the hex, moved (dx, dy) screen
    pixels from it (--stand): a rail car's ground point under its middle, put on the
    centre line between track03's rails, which runs (-2, +4.5) from the hex."""
    h, w = idx.shape
    px, py = (x - CROP["x0"]) * CROP["s"], (y - CROP["y0"]) * CROP["s"]
    return int(round(w / 2.0 - px + dx)), int(round(h - 1 - py + dy))


def shadow(idx: np.ndarray, colour: int) -> tuple[np.ndarray, int, int]:
    """The silhouette thrown onto the ground behind the object, and its header
    offsets for the hex north of the object's (SHADOW_HEX)."""
    h, w = idx.shape
    rows = np.nonzero(idx.any(axis=1))[0]
    base = rows.max()
    xs = np.nonzero(idx[base])[0]
    foot_x = (xs.min() + xs.max()) / 2.0
    ys, xs = np.nonzero(idx)
    hgt = base - ys
    sx = xs - foot_x + SHADOW_DX * hgt
    sy = -SHADOW_DY * hgt                       # up the screen: away from the viewer
    minx, maxx = int(np.floor(sx.min())), int(np.ceil(sx.max()))
    miny = int(np.floor(sy.min()))
    W, H = maxx - minx + 1, -miny + 2
    m = np.zeros((H, W), bool)
    m[np.round(sy).astype(int) - miny, np.round(sx).astype(int) - minx] = True
    m = ndimage.binary_closing(m, iterations=2) | m
    m = ndimage.binary_fill_holes(m)
    out = np.where(m, colour, 0).astype(np.uint8)
    # the object's foot is at (fx, fy) in the frame; it must land 16, 8 px
    # from the object's hex, which is -SHADOW_HEX from the shadow's
    fx, fy = -minx, -miny
    xo = 16 - SHADOW_HEX[0] + W // 2 - fx - 16
    yo = 8 - SHADOW_HEX[1] + (H - 1) - fy - 8
    return out, xo, yo


def preview(path: Path, pal, idx, xo, yo, sh=None):
    pal = np.array(pal, np.uint8)
    pad = 40
    parts = [(idx, xo, yo)] + ([sh] if sh else [])
    W, H = 900, 700
    canvas = np.zeros((H, W, 3), np.uint8) + np.array([60, 52, 40], np.uint8)
    hx, hy = W // 2, H - 220
    for k, (im, ox, oy) in reversed(list(enumerate(parts))):
        h, w = im.shape
        dx, dy = SHADOW_HEX if k == 1 else (0, 0)      # the shadow stands on its own hex
        left = hx + dx + ox - w // 2
        top = hy + dy + oy - (h - 1)
        sub = canvas[top:top + h, left:left + w]
        m = im > 0
        sub[m] = pal[im[m]]
    Image.fromarray(canvas).resize((W * 2, H * 2), Image.NEAREST).save(path)
    print(f"preview {path}")


def frm_bytes(idx, xo, yo) -> bytes:
    h, w = idx.shape
    meta = dict(version=4, fps=10, action=0, fpd=1, xo=[xo] * 6, yo=[yo] * 6)
    return encode_frm(meta, {0: [(w, h, 0, 0, idx.tobytes())]}, split=True)


def write_piece(name, idx, xo, yo, src_proto: int, gi, note):
    art_idx = next_index(IDS / "art_scenery.txt", FOC_START["art_scenery"] - 1)   # art is 0-based
    pid_idx = next_index(IDS / "proto_scenery.txt", FOC_START["proto_scenery"])
    data = frm_bytes(idx, xo, yo)
    proto = bytearray(gi.vfs.read(gi.proto_path(src_proto)))
    struct.pack_into(">I", proto, 0, 0x02000000 | pid_idx)
    struct.pack_into(">I", proto, 8, 0x02000000 | art_idx)
    proto_file = f"{pid_idx:08d}.pro"
    if (PROTO / proto_file).exists() or (ART / name).exists():
        sys.exit(f"{proto_file} or {name} already exists")
    (ART / name).write_bytes(data)
    (PROTO / proto_file).write_bytes(bytes(proto))
    with open(IDS / "art_scenery.txt", "a", newline="\r\n") as fh:     # the registry is CRLF
        fh.write(name + "\n")
    with open(IDS / "proto_scenery.txt", "a", newline="\r\n") as fh:
        fh.write(proto_file + "\n")
    with open(IDS / "keep_protos.txt", "a", newline="\r\n") as fh:
        fh.write(f"0x{0x02000000 | pid_idx:08X}  # {name}, {note}\n")
    print(f"{name}: art {art_idx}, proto 0x{0x02000000 | pid_idx:08X} ({proto_file}) from 0x{src_proto:08X}")
    return 0x02000000 | pid_idx


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("image", type=Path)
    ap.add_argument("art_name")
    ap.add_argument("--height", type=int, help="height of the object in game pixels")
    ap.add_argument("--scale", type=float, help="instead of --height: game pixels per source pixel")
    ap.add_argument("--stand", help="X,Y[,DX,DY]: the source-image pixel that stands on the hex, moved DX,DY screen "
                                    "pixels from it (rail cars: the ground under the middle, on track03's centre line)")
    ap.add_argument("--ref", default="GASSIGN1.FRM,GASSIGN3.FRM", help="reference scenery FRMs for the shading")
    ap.add_argument("--proto", type=lambda s: int(s, 0), default=0x020004DE, help="proto to copy (Pep Gas sign)")
    ap.add_argument("--shadow", help="also make a ground shadow piece with this art name")
    ap.add_argument("--shadow-proto", type=lambda s: int(s, 0), default=0x020004DF)
    ap.add_argument("--white", type=int, default=150, help="background: darkest channel at least this")
    ap.add_argument("--sat", type=int, default=28, help="background: channel spread at most this")
    ap.add_argument("--match-colour", action="store_true", help="also map lightness and chroma onto --ref")
    ap.add_argument("--match-amount", type=float, default=1.0, help="how far the lightness moves onto --ref (0..1)")
    ap.add_argument("--chroma", type=float, help="scale the colour by this instead of capping it at --ref's")
    ap.add_argument("--centre", action="store_true", help="stand the piece on its middle, not its foot (vehicles)")
    ap.add_argument("--radius", type=float, default=3.0, help="how far in from an edge the surfaces turn (px)")
    ap.add_argument("--falloff", type=float, default=0.15)
    ap.add_argument("--ao", type=float, default=0.30)
    ap.add_argument("--grain", type=float, default=1.5)
    ap.add_argument("--rim", type=float, default=0.30)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--preview", type=Path)
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--redo", action="store_true", help="rewrite the art of pieces made before, keeping their ids")
    args = ap.parse_args()

    name = args.art_name.lower()
    check_art_name("scenery", name)
    gi = GameIndex(pristine=False)
    pal = load_8bit_palette(gi)
    quant = Quant(pal)
    ref = ref_lab(gi, np.array(pal, np.float32), args.ref.split(",")) if args.match_colour else None
    idx = build(Image.open(args.image), args.height, ref, pal, quant,
                args.white, args.sat, args.grain, args.rim, args.seed,
                args.radius, args.falloff, args.ao, args.match_amount, args.chroma, args.scale)
    if args.stand:
        xo, yo = anchor_at(idx, *map(float, args.stand.split(",")))
    else:
        xo, yo = anchor_centre(idx) if args.centre else anchor(idx)
    print(f"{name}: {idx.shape[1]}x{idx.shape[0]}, offsets ({xo}, {yo})")
    sh = None
    if args.shadow:
        check_art_name("scenery", args.shadow.lower())
        dark = min(range(1, 229), key=lambda j: sum(pal[j]))
        sh = shadow(idx, dark)
        print(f"{args.shadow.lower()}: {sh[0].shape[1]}x{sh[0].shape[0]}, offsets ({sh[1]}, {sh[2]})")
    if args.preview:
        preview(args.preview, pal, idx, xo, yo, sh)
    if args.redo:
        for n, (im, ox, oy) in [(name, (idx, xo, yo))] + ([(args.shadow.lower(), sh)] if sh else []):
            if not (ART / n).exists():
                sys.exit(f"{n} was never written; use --write")
            (ART / n).write_bytes(frm_bytes(im, ox, oy))
            print(f"rewrote {n}")
    elif args.write:
        write_piece(name, idx, xo, yo, args.proto, gi, f"from {args.image.name}")
        if sh:
            write_piece(args.shadow.lower(), sh[0], sh[1], sh[2], args.shadow_proto, gi, f"shadow of {name}")
        print("written; name them in mod/ids/scenery_text.tsv, then scenery_text.py, gen_ids.py, deploy.py, repack_art.py, validate.py")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
