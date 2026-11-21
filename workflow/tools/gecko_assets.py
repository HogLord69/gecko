#!/usr/bin/env python3
"""
gecko_assets.py - what gecko's Tools menu runs to make new art: scenery from a
picture, mirrored and resized copies, names and examine text, and Apply New Art
(close gecko, deploy, reopen it on the same map).

    python tools/gecko_assets.py info
    python tools/gecko_assets.py preview PICTURE --name NAME --height PX [options] --out PREVIEW.png
    python tools/gecko_assets.py create  PICTURE --name NAME --height PX [options] --title NAME --text TEXT
    python tools/gecko_assets.py mirror  PID --name NAME
    python tools/gecko_assets.py resize  PID --name NAME --scale 2 [--pixel]
    python tools/gecko_assets.py text    PID [--title NAME --text TEXT]    (without --title: show it)
    python tools/gecko_assets.py suggest-name [WORD] [--pid PID] [--suffix S]
    python tools/gecko_assets.py pending
    python tools/gecko_assets.py building-preview PICTURE --height PX [--line X,Y X,Y ...] --out PREVIEW.png
    python tools/gecko_assets.py building-create  PICTURE --height PX [--line ...] --prefix NAME --stamp STAMP
    python tools/gecko_assets.py apply --wait-pid N --gecko GECKO.EXE [--map MAP] [--dry-run]
    python tools/gecko_assets.py sources                 (the games gecko's object palette can show)
    python tools/gecko_assets.py source NAME [--new]     (one game's data paths, as gecko mounts them;
                                                          --new: and the pieces Fallout 2 doesn't have)
    python tools/gecko_assets.py port SOURCE PID         (bring one of its pieces into the mod)
    python tools/gecko_assets.py stamp-games             (which of gecko's stamp folders is which game's)
    python tools/gecko_assets.py stamp-convert SOURCE STAMP.json
                                                         (a copy in Custom/From <game>, converted)

Every command but apply prints one JSON line last, {"ok": true, ...} or
{"ok": false, "error": "..."}: that line is what gecko reads. apply runs in its
own console window, because gecko has closed by then.

The art is made by image_scenery.py, mirror_scenery.py and scale_scenery.py, so
it is the same art those make from the command line; this file only adds what
gecko needs around them:

  * --kind picks the proto the new piece copies, which decides how it behaves:
      prop     the Pep Gas sign (GASSIGN1, 0x020004DE): stands on its hex and blocks it
      vehicle  CAR1 (0x020000E0): blocks its hex, stands on its middle
      flat     the Pep Gas sign's shadow (GASSIGN2, 0x020004DF): flat on the ground,
               walked and shot over, drawn under everything standing
  * a name check stronger than the tools' own: 8 letters, lower case, and not
    the name of ANY scenery art the game can see. A mod file named like a
    vanilla one (car1.frm) would replace the vanilla picture everywhere.
  * the preview: the piece standing on its hex, on a hex grid, beside the
    vanilla player (HMJMPSAA) for scale, drawn the way the engine places a
    sprite (object.cc objectGetRect: left = hex x + 16 - w/2 + x offset,
    top = hex y + 8 - h + 1 + y offset).
  * names and examine text go into mod/ids/proto_text.tsv (or the row the proto
    already has in scenery_text.tsv), applied by scenery_text.py. Pieces with the
    same name share one message, so one examine text (port.allocate is keyed by
    name): a different text for a name already in use is refused here, before
    scenery_text.py would refuse the whole table at Apply.
  * build/gecko_assets/pending.json lists what was made since the last Apply.

Apply runs, after gecko has exited:
    gen_ids.py, deploy.py, scenery_text.py --no-deploy, deploy.py, repack_art.py, validate.py
deploy comes first because scenery_text.py finds a proto through the deployed
lists, which a brand-new proto is not in yet; repack_art.py moves the new art
below the engine's 4096 cap (image_scenery.py lists it in keep_protos.txt).
It refuses while the game runs, while gecko is still open, and while gecko is
open on another game (tools/gecko_browse), and it reopens gecko either way, on
the map gecko had open.

The window shows the steps ("Step 5 of 6"), then FINISHED and closes itself after
5 seconds, or FAILED and stays open to be read. The steps themselves run in a
worker with no window (apply-worker), followed through build/gecko_assets/apply.log
and apply_status.json, so closing the window early stops nothing: the worker
finishes and opens gecko again. (A window closed half way through repack_art.py
once cut it off between swapping the registry and rewriting a stamp.)
"""

from __future__ import annotations

import argparse
import ctypes
import json
import os
import re
import string
import subprocess
import sys
import time
import traceback
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from foclib import GameIndex, parse_lst, parse_msg  # noqa: E402
from ids import ENGINE_ART_NAME_MAX  # noqa: E402

# The art modules (OpenCV, SciPy) take over a second to load: only the commands
# that draw import them, so a name lookup or Apply stays quick.


def art_modules():
    import numpy as np
    import image_scenery as isc
    from replace_pc_from_ref import decode_frm, load_8bit_palette
    from scale_critter import Quant
    return np, isc, decode_frm, load_8bit_palette, Quant


def games_running() -> list[str]:
    from deploy import games_running as running
    return running()

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
IDS = ROOT / "mod" / "ids"
ART = ROOT / "mod" / "data" / "art" / "scenery"
from foclib import config as _config  # noqa: E402
DEPLOYED = _config.base_dir() / "mods" / "foc"
WORK = Path(os.environ.get("FOC_GECKO_ASSETS_WORK") or ROOT / "build" / "gecko_assets")   # the self-test points it away
PENDING = WORK / "pending.json"
APPLY_LOG = WORK / "apply.log"
STATUS = WORK / "apply_status.json"
TEXT_TABLES = [IDS / "scenery_text.tsv", IDS / "proto_text.tsv"]
NEW_TEXT_TABLE = IDS / "proto_text.tsv"
GECKO_SETTINGS = _config.gecko_settings_dir()
BROWSE_MARKER = GECKO_SETTINGS / "settings.json.foc-live"   # tools/gecko_browse/_swap_and_launch.ps1

KINDS = {
    "prop": {"proto": 0x020004DE, "stand": "foot", "ao": 0.30,
             "label": "Sign or prop: stands on its hex and blocks it"},
    "vehicle": {"proto": 0x020000E0, "stand": "centre", "ao": 0.30,
                "label": "Vehicle: blocks its hex, stands on its middle"},
    "flat": {"proto": 0x020004DF, "stand": "centre", "ao": 0.0,
             "label": "Flat on the ground: walked over, drawn under everything"},
}
SHADOW_PROTO = 0x020004DF
COLOUR_REFS = {
    "signs": ["GASSIGN1.FRM", "GASSIGN3.FRM"],
    "vehicles": ["CAR1.FRM", "CAR2.FRM", "CAR4.FRM", "SCAR401.FRM"],
}
PLAYER_FRM = "art\\critters\\hmjmpsaa.frm"
PLAYER_DIR = 2                       # facing south-east, toward the viewer
GROUND = (60, 52, 40)
GRID = (88, 78, 60)
STAND_HEX = (225, 200, 90)
PLAYER_HEX = (150, 150, 150)
TYPE_DIR = {0: "items", 1: "critters", 2: "scenery"}
MSG_FILE = {"items": "pro_item.msg", "critters": "pro_crit.msg", "scenery": "pro_scen.msg"}
GAME_STEPS = [
    ("gen_ids.py", []),
    ("deploy.py", []),
    ("scenery_text.py", ["--no-deploy"]),
    ("deploy.py", []),
    ("repack_art.py", []),
    ("validate.py", []),
]
STEP_NOTES = {"repack_art.py": "the slow one: it reads every map, about a minute"}


class Refused(Exception):
    """A request gecko should show as a plain message, not a crash."""


# ------------------------------------------------------------------------------ names

def scenery_names_taken(gi: GameIndex) -> set[str]:
    """Every scenery art file name the game sees, plus the mod's registered and written ones."""
    taken = {n.lower() for n in parse_lst(gi.vfs.read("art\\scenery\\scenery.lst"))}
    reg = IDS / "art_scenery.txt"                    # a new mod has none yet
    taken |= {l.strip().lower() for l in (reg.read_text() if reg.is_file() else "").splitlines()
              if l.strip() and not l.startswith("#")}
    if ART.is_dir():
        taken |= {p.name.lower() for p in ART.iterdir()}
    return taken


def normalise_name(raw: str) -> str:
    name = raw.strip().lower()
    return name if name.endswith(".frm") else name + ".frm"


def name_problem(name: str, taken: set[str]) -> str | None:
    stem = name[:-4]
    if not stem:
        return "Give the piece a file name."
    if len(name) > ENGINE_ART_NAME_MAX or not re.fullmatch(r"[a-z0-9_]{1,8}", stem):
        return (f"'{stem}' can't be used: a file name is 1 to 8 letters, digits or _ "
                "(the game cuts longer names and then can't find the picture).")
    if name in taken:
        return f"'{stem}' is already the name of a picture in the game. Pick another."
    return None


def free_name(word: str, taken: set[str], suffix: str = "") -> str:
    stem = re.sub(r"[^a-z0-9_]", "", word.lower()) or "piece"
    room = 8 - len(suffix)
    cand = f"{stem[:room]}{suffix}.frm"
    if cand not in taken:
        return cand
    digits = string.digits + string.ascii_lowercase
    for width in (1, 2, 3):
        for k in range(len(digits) ** width):
            tag, n = "", k
            for _ in range(width):
                n, r = divmod(n, len(digits))
                tag = digits[r] + tag
            cand = f"{stem[:room - width]}{tag}{suffix}.frm"
            if cand not in taken:
                return cand
    raise Refused(f"no free name starting with '{stem}'")


# ------------------------------------------------------------------------------ text

def read_text_rows() -> list[tuple[Path, int, int, str, str]]:
    """(table, line number, pid, name, text) for every row of the text tables."""
    rows = []
    for table in TEXT_TABLES:
        if not table.is_file():
            continue
        for n, line in enumerate(table.read_text(encoding="utf-8").splitlines()):
            if not line.strip() or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) >= 3:
                rows.append((table, n, int(parts[0], 0), parts[1].strip(), parts[2].strip()))
    return rows


def known_text_for(type_dir: str, name: str, rows) -> str | None:
    """The examine text a name already has within a type: from the tables, else from
    the mod's description registry (port.msg_desc_path)."""
    want = name.strip().lower()
    for _, _, pid, n, t in rows:
        if TYPE_DIR.get(pid >> 24) == type_dir and n.lower() == want:
            return t
    p = IDS / f"msg_{type_dir}_desc.txt"
    if p.is_file():
        for line in p.read_text(encoding="utf-8").splitlines():
            if "\t" in line and not line.startswith("#"):
                n, t = line.split("\t", 1)
                if n.strip().lower() == want:
                    return t.strip()
    return None


def clean_text(s: str, what: str) -> str:
    s = " ".join(s.split())
    if not s:
        raise Refused(f"The {what} is empty.")
    if any(c in s for c in "{}\t"):
        raise Refused(f"The {what} can't contain braces or tabs.")
    return s


def set_text(pid: int, title: str, text: str, art_name: str = "", note: str = "gecko") -> None:
    type_dir = TYPE_DIR.get(pid >> 24)
    if type_dir is None:
        raise Refused(f"0x{pid:08X} is not an item, critter or scenery proto.")
    title, text = clean_text(title, "name"), clean_text(text, "examine text")
    rows = read_text_rows()
    others = [r for r in rows if r[2] != pid]
    have = known_text_for(type_dir, title, others)
    if have is not None and have != text:
        raise Refused(f"Other pieces are already called '{title}', and pieces with the same name share "
                      f"one examine text:\n\n{have}\n\nUse that text, or a different name.")
    row = f"0x{pid:08X}\t{title}\t{text}\t{art_name}\t{note}"
    mine = [r for r in rows if r[2] == pid]
    if mine:
        table, n = mine[0][0], mine[0][1]
        lines = table.read_text(encoding="utf-8").splitlines()
        old = lines[n].split("\t")
        if len(old) > 3:                 # keep the row's own trailing notes
            row = "\t".join([f"0x{pid:08X}", title, text] + old[3:])
        lines[n] = row
        table.write_text("\n".join(lines) + "\n", encoding="utf-8")
    else:
        body = NEW_TEXT_TABLE.read_text(encoding="utf-8") if NEW_TEXT_TABLE.is_file() else ""
        if body and not body.endswith("\n"):
            body += "\n"
        NEW_TEXT_TABLE.write_text(body + row + "\n", encoding="utf-8")


def current_text(pid: int) -> dict:
    """The name and examine text a proto has: its table row, else what the game shows."""
    for _, _, p, n, t in read_text_rows():
        if p == pid:
            return {"title": n, "text": t, "source": "table"}
    type_dir = TYPE_DIR.get(pid >> 24)
    if type_dir is None:
        raise Refused(f"0x{pid:08X} is not an item, critter or scenery proto.")
    gi = GameIndex(pristine=False)
    pr = gi.proto(pid)
    if pr is None:
        raise Refused(f"0x{pid:08X} isn't in the game yet. Apply New Art first.")
    try:
        msg = parse_msg(gi.vfs.read(f"text\\english\\game\\{MSG_FILE[type_dir]}"))
    except FileNotFoundError:
        msg = {}
    return {"title": msg.get(pr.message_id, ""), "text": msg.get(pr.message_id + 1, ""), "source": "game"}


# ------------------------------------------------------------------------------ pending

def read_pending() -> list[dict]:
    try:
        return json.loads(PENDING.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return []


def add_pending(entry: dict) -> None:
    WORK.mkdir(parents=True, exist_ok=True)
    items = read_pending()
    items.append(entry)
    PENDING.write_text(json.dumps(items, indent=1), encoding="utf-8")


# ------------------------------------------------------------------------------ picture -> piece

def make_piece(args, gi: GameIndex, pal) -> dict:
    """Build the art (and shadow) for a picture the way image_scenery.py does."""
    from PIL import Image
    np, isc, _, _, Quant = art_modules()
    kind = KINDS[args.kind]
    picture = Image.open(args.picture)
    ref = None
    if args.colour != "none":
        names = set(n.lower() for n in parse_lst(gi.vfs.read("art\\scenery\\scenery.lst")))
        have = [n for n in COLOUR_REFS[args.colour] if n.lower() in names]
        if not have:
            raise Refused(f"none of the {args.colour} reference pictures are in the game")
        ref = isc.ref_lab(gi, np.array(pal, np.float32), have)
    if args.scale is None and not args.height:
        raise Refused("Give a height in game pixels, or a scale.")
    idx = isc.build(picture, args.height, ref, pal, Quant(pal), args.white, args.sat, args.grain,
                    args.rim, args.seed, args.radius, args.falloff, kind["ao"] if args.ao is None else args.ao,
                    args.match_amount, None, args.scale)
    stand = args.stand or kind["stand"]
    if stand == "foot":
        xo, yo = isc.anchor(idx)
    elif stand == "centre":
        xo, yo = isc.anchor_centre(idx)
    else:
        x, y = (float(v) for v in stand.split(","))
        if not (0 <= x < picture.width and 0 <= y < picture.height):
            raise Refused("The stand point is outside the picture.")
        xo, yo = isc.anchor_at(idx, x, y)
    sh = None
    if args.shadow:
        dark = min(range(1, 229), key=lambda j: sum(pal[j]))
        sh = isc.shadow(idx, dark)
    return {"idx": idx, "xo": xo, "yo": yo, "shadow": sh, "crop": dict(isc.CROP)}


def to_rgba(idx, pal):
    import numpy as np
    from PIL import Image
    rgb = pal[idx]
    alpha = np.where(idx > 0, 255, 0).astype(np.uint8)
    return Image.fromarray(np.dstack([rgb, alpha]), "RGBA")


def hexagon(cx: int, cy: int) -> list[tuple[int, int]]:
    return [(cx, cy - 8), (cx + 16, cy - 4), (cx + 16, cy + 4), (cx, cy + 8), (cx - 16, cy + 4), (cx - 16, cy - 4)]


def render_preview(gi: GameIndex, pal, piece: dict, out: Path) -> dict:
    from PIL import Image, ImageDraw
    np, isc, decode_frm, _, _ = art_modules()
    pal = np.array(pal, np.uint8)
    sprites = []                                  # (image, left, top) relative to the stand hex at (0, 0)
    idx, xo, yo = piece["idx"], piece["xo"], piece["yo"]
    h, w = idx.shape
    piece_left, piece_top = xo - w // 2, yo - (h - 1)
    if piece["shadow"]:
        sidx, sxo, syo = piece["shadow"]
        sh_, sw_ = sidx.shape
        dx, dy = isc.SHADOW_HEX
        sprites.append((to_rgba(sidx, pal), dx + sxo - sw_ // 2, dy + syo - (sh_ - 1)))
    sprites.append((to_rgba(idx, pal), piece_left, piece_top))

    player_hex = None
    try:
        meta = decode_frm(gi.vfs.read(PLAYER_FRM))
        f = meta["frames"][PLAYER_DIR][0]
        pidx = np.frombuffer(f.px, np.uint8).reshape(f.h, f.w)
        pxo, pyo = meta["xo"][PLAYER_DIR], meta["yo"][PLAYER_DIR]
        k = -1                                    # whole hexes to the west until the player clears the piece
        while k * 32 + pxo - f.w // 2 + f.w + 10 > piece_left:
            k -= 1
        player_hex = (k * 32, 0)
        sprites.append((to_rgba(pidx, pal), k * 32 + pxo - f.w // 2, pyo - (f.h - 1)))
    except (FileNotFoundError, KeyError, IndexError):
        pass

    left = min(s[1] for s in sprites) - 40
    top = min(s[2] for s in sprites) - 30
    right = max(s[1] + s[0].width for s in sprites) + 40
    bottom = max(max(s[2] + s[0].height for s in sprites), 8) + 50
    W, H = right - left, bottom - top
    canvas = Image.new("RGBA", (W, H), GROUND + (255,))
    draw = ImageDraw.Draw(canvas)
    ox, oy = -left, -top                          # the stand hex's centre on the canvas
    for j in range((-oy) // 12 - 2, (H - oy) // 12 + 3):
        for i in range((-ox - 16 * j) // 32 - 2, (W - ox - 16 * j) // 32 + 3):
            cx, cy = ox + 32 * i + 16 * j, oy + 12 * j
            draw.polygon(hexagon(cx, cy), outline=GRID)
    if player_hex:
        draw.polygon(hexagon(ox + player_hex[0], oy + player_hex[1]), outline=PLAYER_HEX)
    draw.polygon(hexagon(ox, oy), outline=STAND_HEX)
    for im, l, t in sprites:
        canvas.alpha_composite(im, (l + ox, t + oy))
    # the hex's centre again on top: a piece standing on its middle hides its hex
    draw = ImageDraw.Draw(canvas)
    draw.line([(ox - 3, oy), (ox + 3, oy)], fill=STAND_HEX)
    draw.line([(ox, oy - 3), (ox, oy + 3)], fill=STAND_HEX)
    out.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(out)
    return {"preview": str(out), "stand_hex": [ox, oy]}


# ------------------------------------------------------------------------------ commands

# What the tools need besides Python itself (setup.py installs them). An answer that lists them under
# "missing", with "python_path", lets gecko offer to install them with that same Python.
PACKAGES = {"PIL": "pillow", "numpy": "numpy", "scipy": "scipy", "cv2": "opencv-python-headless"}


def missing_packages() -> list[str]:
    import importlib.util
    return [pkg for mod, pkg in PACKAGES.items() if importlib.util.find_spec(mod) is None]


# The working copy of Fallout 2 the tools read the game from and deploy into (the deployed layer's
# game folder). The map workflow kit makes it with setup.py --build-base; without it most commands
# can't run, and the answer says so (needs_base) so gecko can offer to make it.
NO_BASE_NEEDED = {"info", "sources", "stamp-games", "pending"}


def working_copy() -> Path:
    return DEPLOYED.parent.parent


def configured_fallout2() -> str:
    """The Fallout 2 folder the map workflow kit was given (its config.json), if it holds a game."""
    try:
        from foclib import config   # the kit's: where this computer keeps its games
    except ImportError:
        return ""
    p = config.fallout2_dir()
    return str(p) if p and (p / "master.dat").exists() else ""


def missing_working_copy() -> dict | None:
    base = working_copy()
    if (base / "master.dat").exists():
        return None
    setup = ROOT / "setup.py"
    return {"ok": False, "needs_base": True, "base": str(base), "fallout2": configured_fallout2(),
            "setup": str(setup) if setup.is_file() else "",
            "error": f"The map tools have no working copy of Fallout 2 yet ({base}): they read the game from it "
                     f"and put new pieces into it. Make it once with: python setup.py --build-base (in "
                     f"{ROOT}), giving it your Fallout 2 folder with the Restoration Project Unofficial in it."}


def cmd_info(_args) -> dict:
    return {
        "root": str(ROOT),
        "deployed": str(DEPLOYED),
        "python": sys.version.split()[0],
        "python_path": sys.executable,
        "missing": missing_packages(),
        "base_ready": missing_working_copy() is None,
        "games_running": games_running(),
        "browse_session": BROWSE_MARKER.exists(),
        "pending": len(read_pending()),
        "kinds": {k: v["label"] for k, v in KINDS.items()},
        "colours": list(COLOUR_REFS),
    }


def cmd_suggest_name(args) -> dict:
    gi = GameIndex(pristine=False)
    word = args.word
    if args.pid is not None:                     # start from the proto's own art name
        pr = gi.proto(args.pid)
        if pr is not None and args.pid >> 24 == 2:
            lst = parse_lst(gi.vfs.read("art\\scenery\\scenery.lst"))
            if (pr.fid & 0xFFFFFF) < len(lst):
                word = lst[pr.fid & 0xFFFFFF].rsplit(".", 1)[0]
    return {"name": free_name(word or "piece", scenery_names_taken(gi), args.suffix)[:-4]}


def cmd_preview(args) -> dict:
    gi = GameIndex(pristine=False)
    pal = art_modules()[3](gi)
    piece = make_piece(args, gi, pal)
    result = render_preview(gi, pal, piece, args.out)
    h, w = piece["idx"].shape
    name = normalise_name(args.name) if args.name else ""
    result.update({
        "width": w, "height": h, "offsets": [piece["xo"], piece["yo"]],
        "hexes_wide": round(w / 32, 1),
        "name_problem": name_problem(name, scenery_names_taken(gi)) if name else None,
    })
    return result


def cmd_create(args) -> dict:
    gi = GameIndex(pristine=False)
    taken = scenery_names_taken(gi)
    name = normalise_name(args.name)
    problem = name_problem(name, taken)
    if problem:
        raise Refused(problem)
    shadow_name = None
    if args.shadow:
        shadow_name = normalise_name(args.shadow_name) if args.shadow_name else free_name(name[:-4], taken | {name}, "s")
        problem = name_problem(shadow_name, taken | {name})
        if problem:
            raise Refused("Shadow: " + problem)
    title = clean_text(args.title, "name")
    text = clean_text(args.text, "examine text")
    have = known_text_for("scenery", title, read_text_rows())
    if have is not None and have != text:
        raise Refused(f"Other pieces are already called '{title}', and pieces with the same name share "
                      f"one examine text:\n\n{have}\n\nUse that text, or a different name.")
    _, isc, _, load_8bit_palette, _ = art_modules()
    pal = load_8bit_palette(gi)
    piece = make_piece(args, gi, pal)
    note = f"from {args.picture.name} (gecko)"
    pid = isc.write_piece(name, piece["idx"], piece["xo"], piece["yo"], KINDS[args.kind]["proto"], gi, note)
    set_text(pid, title, text, name[:-4])
    add_pending({"pid": f"0x{pid:08X}", "name": name, "what": f"new {args.kind} from {args.picture.name}"})
    out = {"pid": f"0x{pid:08X}", "name": name[:-4]}
    if piece["shadow"]:
        sidx, sxo, syo = piece["shadow"]
        spid = isc.write_piece(shadow_name, sidx, sxo, syo, SHADOW_PROTO, gi, f"shadow of {name}")
        add_pending({"pid": f"0x{spid:08X}", "name": shadow_name, "what": f"shadow of {name}"})
        out["shadow"] = {"pid": f"0x{spid:08X}", "name": shadow_name[:-4]}
    return out


def run_copy_tool(tool: str, pid: int, name: str, extra: list[str]) -> int:
    r = subprocess.run([sys.executable, str(TOOLS / tool), f"0x{pid:08X}", name, "--write", *extra],
                       cwd=ROOT, capture_output=True, text=True)
    if r.returncode:
        raise Refused((r.stderr or r.stdout).strip().splitlines()[-1] if (r.stderr or r.stdout).strip()
                      else f"{tool} failed")
    m = re.search(r"-> 0x([0-9A-Fa-f]{8}) ", r.stdout)
    if not m:
        raise Refused(f"{tool} wrote nothing I could read:\n{r.stdout}")
    return int(m.group(1), 16)


def check_copy_source(pid: int, gi: GameIndex) -> None:
    if pid >> 24 != 2:
        raise Refused("Only scenery can be copied this way.")
    if gi.proto(pid) is None:
        raise Refused(f"0x{pid:08X} isn't in the game yet. Apply New Art first, then copy it.")


def cmd_mirror(args) -> dict:
    gi = GameIndex(pristine=False)
    check_copy_source(args.pid, gi)
    name = normalise_name(args.name)
    problem = name_problem(name, scenery_names_taken(gi))
    if problem:
        raise Refused(problem)
    new = run_copy_tool("mirror_scenery.py", args.pid, name, [])
    add_pending({"pid": f"0x{new:08X}", "name": name, "what": f"mirrored copy of 0x{args.pid:08X}"})
    return {"pid": f"0x{new:08X}", "name": name[:-4]}


def cmd_resize(args) -> dict:
    gi = GameIndex(pristine=False)
    check_copy_source(args.pid, gi)
    if not 0.1 <= args.scale <= 8:
        raise Refused("The size has to be between 10% and 800%.")
    name = normalise_name(args.name)
    problem = name_problem(name, scenery_names_taken(gi))
    if problem:
        raise Refused(problem)
    new = run_copy_tool("scale_scenery.py", args.pid, name,
                        ["--scale", f"{args.scale:g}"] + (["--pixel"] if args.pixel else []))
    add_pending({"pid": f"0x{new:08X}", "name": name, "what": f"x{args.scale:g} copy of 0x{args.pid:08X}"})
    return {"pid": f"0x{new:08X}", "name": name[:-4]}


def cmd_text(args) -> dict:
    if args.title is None:
        return current_text(args.pid)
    set_text(args.pid, args.title, args.text or "")
    add_pending({"pid": f"0x{args.pid:08X}", "name": "", "what": f"name '{args.title}'"})
    return {"pid": f"0x{args.pid:08X}"}


def cmd_pending(_args) -> dict:
    return {"items": read_pending()}


# ------------------------------------------------------------------------------ other games

# gecko's object palette can show another game's pieces (its Game picker). Each game is loaded in
# gecko on its own, from the layers `source` lists, and never mixed into the mod's data: a piece is
# a position in its game's lists, so two games' lists cannot be mounted together. Placing one runs
# `port`, which brings it into the mod with port.py (new numbers, recorded in mod/assets.csv) and
# writes the overlay: the lists deploy.py would write, plus the new files, for gecko to mount on top
# of the mod for the rest of the session, so the piece shows at once. Apply New Art deploys it for
# real and clears the overlay.

OVERLAY = WORK / "overlay"
NO_SCRIPT = 0xFFFFFFFF
IMPORTABLE_TYPES = {0: "items", 2: "scenery", 3: "walls", 5: "misc"}
IMPORTABLE_ITEM_TYPES = {1: "container", 5: "misc item", 6: "key"}
ITEM_TYPE_NAMES = {0: "armor", 1: "container", 2: "drug", 3: "weapon", 4: "ammo", 5: "misc item", 6: "key"}
PROTO_SID_OFFSET = 28        # pid, message id, fid, light distance, light intensity, flags, extended flags
ITEM_TYPE_OFFSET = 32        # (fallout2-ce proto.cc protoRead)
ITEM_INVENTORY_FID_OFFSET = 52


def source_label(entry: dict, name: str) -> str:
    if entry.get("label"):
        return entry["label"]
    project = entry.get("project") or name
    return re.split(r" \(|,| \+ ", project)[0].strip() or name


def source_path(entry: dict) -> Path:
    path = Path(entry["path"])
    return path if path.is_absolute() else ROOT / path


def left_out(entry: dict) -> str | None:
    """Why a source is not offered in gecko's Game picker, or None when it is."""
    if entry.get("palette") is False:
        return entry.get("palette_note") or "left out of gecko's Game picker in tools/sources.json"
    path = source_path(entry)
    if not path.is_dir():
        return f"not on this computer ({path})"
    if not ((path / "master.dat").is_file() or any(path.glob("*.dat")) or (path / "data").is_dir()
            or (path / "mods").is_dir()):
        return "not a game: no .dat archives and no data folder"
    return None


def game_layers(gi: GameIndex) -> list[str]:
    """A game's layers in gecko's data-path order: lowest priority first (GameVFS lists highest first)."""
    out = []
    for prov in reversed(gi.vfs.providers):
        out.append(str(prov.root if prov.kind == "dir" else prov.dat.path))
    return out


def cmd_sources(_args) -> dict:
    from port import load_sources
    games, skipped = [], []
    for name, entry in load_sources().items():
        why = left_out(entry)
        if why:
            skipped.append({"id": name, "label": source_label(entry, name), "why": why})
        else:
            games.append({"id": name, "label": source_label(entry, name)})
    games.sort(key=lambda g: g["label"].lower())
    out = {"games": games, "left_out": skipped}
    if not games:
        try:
            from foclib import config  # noqa: F401 - only the gecko repo's workflow kit has it
            out["note"] = ("No other games are set up yet: run python setup.py in the workflow folder "
                           "and give it the folders of the other games you have")
        except ImportError:
            out["note"] = "None of the games in tools/sources.json is on this computer"
    return out


def cmd_source(args) -> dict:
    from port import load_sources
    entry = load_sources().get(args.name)
    if entry is None:
        raise Refused(f"No game called '{args.name}' in tools/sources.json.")
    why = left_out(entry)
    if why:
        raise Refused(f"{source_label(entry, args.name)}: {why}")
    paths = game_layers(GameIndex(source_path(entry), pristine=False))
    if entry.get("overlay") == "foc":           # a derived tree laid over the mod (fonline2)
        paths = game_layers(GameIndex(pristine=False)) + paths
    out = {"id": args.name, "label": source_label(entry, args.name), "paths": paths}
    if args.new:
        out["new"] = new_pieces(args.name, entry)
    return out


# Which of a game's pieces Fallout 2 doesn't have, so the Game list can leave out the vanilla pieces
# every total conversion carries. A piece is judged by its picture: the first frame's fingerprint
# from art_index.py (size and colours, so a renamed or repacked copy still counts as the same), or
# the md5 of the standing animation for a critter. "Fallout 2" is the base the mod is built on
# (Fallout 2 + RPU, GameIndex() pristine), which does not change with what the mod adds. Both sides
# are cached in build/gecko_assets/new_pieces/ and redone when an archive or data folder changes.

PIECE_DIRS = {0: "items", 1: "critters", 2: "scenery", 3: "walls", 5: "misc"}
NEW_PIECES = WORK / "new_pieces"


def art_file_name(entry: str, art_dir: str) -> str:
    return f"{entry.split(',', 1)[0].strip()}aa.frm" if art_dir == "critters" else entry


def fingerprint(data: bytes, art_dir: str, pal6) -> str | None:
    import hashlib
    import art_index
    if art_dir == "critters":
        return hashlib.md5(data).hexdigest()
    try:
        for _d, _f, w, h, idx in art_index.frm_frames(data):
            return art_index.frame_hash(w, h, idx, pal6).hex()
    except Exception:                                   # noqa: BLE001 - a broken FRM is just not matched
        return None
    return None


def cached(name: str, sig) -> dict | None:
    f = NEW_PIECES / f"{name}.json"
    try:
        got = json.loads(f.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    return got["data"] if got.get("sig") == json.loads(json.dumps(sig)) else None


def store(name: str, sig, data) -> None:
    NEW_PIECES.mkdir(parents=True, exist_ok=True)
    (NEW_PIECES / f"{name}.json").write_text(json.dumps({"sig": sig, "data": data}), encoding="utf-8")


def base_signature() -> list:
    import art_index
    base = _config.base_dir()
    return [art_index.VERSION, art_index._dat_sig(base), art_index._dat_sig(base / "mods"),
            art_index._walk_sig(base / "data")]


def base_fingerprints() -> dict[str, set[str]]:
    import art_index
    sig = base_signature()
    data = cached("_fallout2", sig)
    if data is None:
        gi = GameIndex()                                # pristine: Fallout 2 + RPU, not the mod
        pal6 = art_index.palette6(gi.vfs.read("color.pal"))
        data = {}
        for art_dir in PIECE_DIRS.values():
            prints = set()
            try:
                names = parse_lst(gi.vfs.read(f"art\\{art_dir}\\{art_dir}.lst"))
            except FileNotFoundError:
                names = []
            for entry in dict.fromkeys(n for n in names if n):
                try:
                    fp = fingerprint(gi.vfs.read(f"art\\{art_dir}\\{art_file_name(entry, art_dir)}"), art_dir, pal6)
                except OSError:                         # missing, or a file Windows won't let us read
                    continue
                if fp:
                    prints.add(fp)
            data[art_dir] = sorted(prints)
        store("_fallout2", sig, data)
    return {k: set(v) for k, v in data.items()}


def new_pieces(name: str, entry: dict) -> dict[str, list[int]]:
    """{proto folder: [1-based .lst lines]} of the pieces whose picture Fallout 2 doesn't have."""
    import art_index
    path = source_path(entry)
    sig = [art_index.source_signature(name, path), base_signature()]
    data = cached(name, sig)
    if data is not None:
        return data
    base = base_fingerprints()
    gi = GameIndex(path, pristine=False)
    try:
        pal6 = art_index.palette6(gi.vfs.read("color.pal"))
    except FileNotFoundError:                           # an overlay ships no palette of its own
        pal6 = art_index.palette6(GameIndex().vfs.read("color.pal"))
    data = {}
    for t, art_dir in PIECE_DIRS.items():
        keep = []
        try:
            protos = parse_lst(gi.vfs.read(f"proto\\{art_dir}\\{art_dir}.lst"))
            art = parse_lst(gi.vfs.read(f"art\\{art_dir}\\{art_dir}.lst"))
        except FileNotFoundError:
            data[art_dir] = keep
            continue
        mask = 0xFFF if art_dir == "critters" else 0xFFFFFF
        for line, pro_name in enumerate(protos, 1):
            try:
                pro = gi.vfs.read(f"proto\\{art_dir}\\{pro_name}")
                index = int.from_bytes(pro[8:12], "big") & mask
                if index >= len(art) or not art[index]:
                    continue
                picture = gi.vfs.read(f"art\\{art_dir}\\{art_file_name(art[index], art_dir)}")
            except (OSError, ValueError):               # missing, or a file Windows won't let us read
                continue
            fp = fingerprint(picture, art_dir, pal6)
            if fp and fp not in base[art_dir]:
                keep.append(line)
        data[art_dir] = keep
    store(name, sig, data)
    return data


def ported_row(source: str, pid: int) -> dict | None:
    from port import read_ledger
    return next((r for r in read_ledger()
                 if r["source"] == source and r["kind"] == "proto"
                 and r["source_id"].lower() == f"0x{pid:08x}"), None)


def importable(pid: int, data: bytes, label: str) -> None:
    """Refuse what other games' numbers reach further into than the proto and its picture."""
    t = pid >> 24
    if t == 1:
        raise Refused(f"Critters can't be brought in from {label} this way yet: a critter carries that "
                      f"game's script, talking head and AI packet numbers, which mean something else here.")
    if t not in IMPORTABLE_TYPES:
        raise Refused(f"Only scenery, walls, misc pieces and some items can be brought in (this is type {t}).")
    if t == 0:
        item_type = int.from_bytes(data[ITEM_TYPE_OFFSET:ITEM_TYPE_OFFSET + 4], "big")
        if item_type not in IMPORTABLE_ITEM_TYPES:
            raise Refused(f"This is a {ITEM_TYPE_NAMES.get(item_type, 'special item')}: its game rules point at "
                          f"{label}'s other items and art. Only containers, misc items and keys come across.")
        inventory_fid = int.from_bytes(data[ITEM_INVENTORY_FID_OFFSET:ITEM_INVENTORY_FID_OFFSET + 4], "big")
        if inventory_fid != NO_SCRIPT:
            raise Refused("This item has an inventory picture, and the mod's lists have no room set aside for "
                          "inventory pictures. Items that can't be picked up (desks, lockers) come across.")


def deployed(pid: int) -> bool:
    """Whether the game's deployed layer already holds this piece's proto, as the mod has it."""
    from foclib import TYPE_NAME
    from port import MOD
    folder = TYPE_NAME.get(pid >> 24)
    if not folder:
        return False
    name = f"{pid & 0xFFFFFF:08d}.pro"
    live, mine = DEPLOYED / "proto" / folder / name, MOD / "data" / "proto" / folder / name
    return live.is_file() and mine.is_file() and live.read_bytes() == mine.read_bytes()


def merged_names(fname: str) -> bytes | None:
    """The deployed name file plus any names the mod has allocated since, or None when nothing is new."""
    from port import MOD
    mine = MOD / "data" / "text" / "english" / "game" / fname
    if not mine.is_file():
        return None
    live = DEPLOYED / "text" / "english" / "game" / fname
    try:
        live_bytes = live.read_bytes() if live.is_file() else GameIndex(pristine=False).vfs.read(f"text\\english\\game\\{fname}")
    except FileNotFoundError:
        live_bytes = b""
    have = parse_msg(live_bytes)
    extra = [f"{{{mid}}}{{}}{{{text}}}" for mid, text in sorted(parse_msg(mine.read_bytes()).items()) if mid not in have]
    if not extra:
        return None
    return live_bytes.rstrip(b"\r\n") + b"\n" + "\n".join(extra).encode("cp1252", "replace") + b"\n"


def build_overlay() -> list[str]:
    """Write the overlay for the pieces brought in since the last Apply; returns its game paths."""
    import shutil
    import lists
    from foclib import TYPE_NAME
    from gen_ids import read_registry
    from ids import ART_KEY, PROTO_KEY
    from port import MOD, MSG_FILE_FOR_TYPE

    files = [f for item in read_pending() for f in item.get("files", [])]
    # gecko has this folder mounted while it is rebuilt: a file it holds open stays, to be
    # overwritten below, rather than failing a piece that is already in the registry.
    shutil.rmtree(OVERLAY, ignore_errors=True)
    if not files:
        return []
    written: list[str] = []

    def put(game_path: str, data: bytes) -> None:
        dst = OVERLAY / game_path
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(data)
        written.append(game_path)

    from port import base_index
    gi = base_index()                    # pristine, as deploy.py builds the lists
    for t, key in PROTO_KEY.items():
        if read_registry(lists.IDS / f"{key}.txt"):
            name = TYPE_NAME[t]
            put(f"proto/{name}/{name}.lst", lists.render_lst(lists.proto_lst(gi, t)))
    for art_dir, art_key in ART_KEY.items():
        if read_registry(lists.IDS / f"{art_key}.txt") or (art_dir == "tiles" and lists.tile_slots()):
            put(f"art/{art_dir}/{art_dir}.lst", lists.render_lst(lists.art_lst(gi, art_dir)))
    for game_path in dict.fromkeys(files):
        src = MOD / "data" / game_path
        if src.is_file():
            put(game_path, src.read_bytes())
    for folder in sorted({f.split("/")[1] for f in files if f.startswith("proto/")}):
        fname = MSG_FILE_FOR_TYPE.get(folder)
        names = merged_names(fname) if fname else None
        if names:
            put(f"text/english/game/{fname}", names)
    return written


# Stamps that come with gecko (its stamps folder, "Came with gecko" in the Pattern Library). A stamp
# holds its game's numbers, so the folders cut from another game draw the wrong pieces in this one.
# gecko draws those folders with the game's own data (stamp-games says which game) and, when one is
# picked, stamp-convert copies it into the user's own stamps (Custom/From <game>/...) and converts the
# copy with port_stamp.py: pieces and tiles matched to the mod's or brought in. The files that brought
# in go into the overlay, as for port, so the converted stamp draws right at once.

# gecko's stamps folder, one top folder per game (tools/organize_stamps.py)
STAMP_FOLDERS = {"Fallout 1": "fo1", "Nevada": "nevada", "Olympus": "olympus", "Resurrection": "resurrection",
                 "Sonora": "sonora"}
NATIVE_STAMP_FOLDERS = ["Fallout 2", "HogLord Props"]           # in Fallout 2's own numbers already


def cmd_stamp_games(_args) -> dict:
    from port import load_sources
    srcs = load_sources()
    folders = {}
    for folder, sid in STAMP_FOLDERS.items():
        entry = srcs.get(sid)
        label = source_label(entry, sid) if entry else folder
        why = left_out(entry) if entry else ("not set up here: run python setup.py in the workflow folder and "
                                             "give it this game's folder")
        folders[folder] = {"id": sid, "label": label, "available": why is None, "why": why or ""}
    return {"folders": folders, "native": NATIVE_STAMP_FOLDERS}


def mod_data_files() -> dict[str, tuple[int, int]]:
    """game path -> (size, time) of every file in mod/data, from the directory listings alone."""
    from port import MOD
    out = {}
    folders = [(str(MOD / "data"), "")]
    while folders:
        folder, rel = folders.pop()
        try:
            with os.scandir(folder) as it:
                for e in it:
                    if e.is_dir():
                        folders.append((e.path, f"{rel}{e.name}/"))
                    elif e.is_file():
                        st = e.stat()
                        out[rel + e.name] = (st.st_size, st.st_mtime_ns)
        except FileNotFoundError:
            continue
    return out


def cmd_stamp_convert(args) -> dict:
    import contextlib
    import shutil
    import port as portmod
    import port_stamp

    entry = portmod.load_sources().get(args.source)
    if entry is None:
        raise Refused(f"{args.source} isn't set up here: run python setup.py in the workflow folder and give it "
                      f"that game's folder.")
    label = source_label(entry, args.source)
    why = left_out(entry)
    if why:
        raise Refused(f"{label}: {why}")
    src = Path(args.stamp)
    if not src.is_file():
        raise Refused(f"No stamp at {src}.")
    # Custom/From <game>/<the folders below the game's own folder>/<name>.json
    parts = src.parts
    below = [i for i, p in enumerate(parts) if p in STAMP_FOLDERS or p.removesuffix(" Stamps") in STAMP_FOLDERS]
    rel = Path(*parts[below[-1] + 1:]) if below else Path(src.name)
    folder = "From " + re.sub(r'[<>:"/\\|?*]', "", label).strip()
    dest = port_stamp.CUSTOM / folder / rel
    notes: list[str] = []
    fresh = not dest.is_file()
    if fresh:
        before = mod_data_files()
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dest)
        old_argv = sys.argv
        sys.argv = ["port_stamp.py", str(dest), "--source", args.source]
        try:
            with contextlib.redirect_stdout(sys.stderr):      # its report; the JSON line stays last
                port_stamp.main()
        except BaseException:
            dest.unlink(missing_ok=True)                      # no half-made stamp left behind
            raise
        finally:
            sys.argv = old_argv
        after = mod_data_files()
        files = sorted(f for f, sig in after.items() if before.get(f) != sig)
        if files:
            add_pending({"pid": "stamp", "name": dest.stem, "what": f"stamp from {label}", "files": files})
    done = json.loads(dest.read_text(encoding="utf-8")).get("focConverted") or {}
    if fresh and not done:
        notes.append("It already matched your game, so nothing needed bringing in.")
    if done.get("tilesSwapped"):
        notes.append(f"{done['tilesSwapped']} floor tile(s) your game didn't have became the closest-looking ones.")
    if done.get("objectsDropped"):
        notes.append(f"{done['objectsDropped']} piece(s) couldn't be brought in and were left out.")
    overlay = build_overlay()
    return {"stamp": str(dest), "new": fresh, "label": label,
            "overlay": str(OVERLAY) if overlay else "", "changed": overlay, "notes": notes}


def cmd_port(args) -> dict:
    import contextlib
    import port as portmod

    entry = portmod.load_sources().get(args.source)
    if entry is None:
        raise Refused(f"No game called '{args.source}' in tools/sources.json.")
    label = source_label(entry, args.source)
    why = left_out(entry)
    if why:
        raise Refused(f"{label}: {why}")
    pid = args.pid
    notes: list[str] = []
    row = ported_row(args.source, pid)
    fresh = row is None
    if fresh:
        src = portmod.open_source(args.source)
        game_path = src.proto_path(pid)
        if game_path is None or not src.vfs.exists(game_path):
            raise Refused(f"{label} has no proto 0x{pid:08X}.")
        importable(pid, src.vfs.read(game_path), label)
        with contextlib.redirect_stdout(sys.stderr):      # port.py's report; the JSON line stays last
            portmod.cmd_proto(argparse.Namespace(source=args.source, pids=[f"0x{pid:08X}"],
                                                 notes="brought in from gecko"))
        row = ported_row(args.source, pid)
        if row is None:
            raise Refused(f"port.py did not bring 0x{pid:08X} in from {label}; its report is in gecko's log.")
        proto_file = portmod.ROOT / row["dest_path"]
        data = bytearray(proto_file.read_bytes())
        if pid >> 24 in (0, 2, 3):
            sid = int.from_bytes(data[PROTO_SID_OFFSET:PROTO_SID_OFFSET + 4], "big")
            if sid != NO_SCRIPT:
                data[PROTO_SID_OFFSET:PROTO_SID_OFFSET + 4] = NO_SCRIPT.to_bytes(4, "big")
                proto_file.write_bytes(bytes(data))
                notes.append(f"Its script ({sid & 0xFFFFFF} in {label}'s scripts.lst) was left behind: "
                             f"that number means another script here.")
        mod_data = (portmod.MOD / "data").resolve()
        art = [r for r in portmod.read_ledger()
               if r["source"] == args.source and r["kind"] == "art"
               and r["notes"] == f"pulled in by proto 0x{pid:08X}"]
        files = [proto_file.resolve().relative_to(mod_data).as_posix()]
        files += [(portmod.ROOT / r["dest_path"]).resolve().relative_to(mod_data).as_posix() for r in art]
        new_pid = int(row["dest_id"], 16)
        add_pending({"pid": f"0x{new_pid:08X}", "name": Path(files[-1]).stem,
                     "what": f"from {label} (0x{pid:08X})", "files": files})
    new_pid = int(row["dest_id"], 16)
    overlay = [] if deployed(new_pid) else build_overlay()
    return {"pid": f"0x{new_pid:08X}", "source_pid": f"0x{pid:08X}", "new": fresh, "label": label,
            "overlay": str(OVERLAY) if overlay else "", "changed": overlay, "notes": notes}


def building(args):
    """slice_building.run with its refusals turned into ours."""
    import slice_building as sb
    try:
        return sb, *sb.run(args.picture, args.height, args.scale, args.line, args.colour, args.rim, args.wall_top)
    except sb.Refused as e:
        raise Refused(str(e))


def cmd_building_preview(args) -> dict:
    sb, gi, pal, pieces, roof, summary = building(args)
    result = {**summary, **sb.preview(gi, pal, pieces, roof, args.out)}
    prefix = args.prefix.strip().lower()
    result["name_problem"] = sb.prefix_problem(prefix, len(pieces), sb.wall_names_taken(gi), len(roof),
                                               sb.tile_names_taken(gi) if roof else set()) if prefix else None
    result["roof_problem"] = None
    if roof:
        spare = len(sb.ts.spare_slots())
        result["spare_tile_slots"] = spare
        if len(roof) > spare:
            result["roof_problem"] = (f"The roof needs {len(roof)} roof tiles but only {spare} spare tile slots are left: "
                                      f"make the building smaller, or keep the roof in the walls.")
    result["stamp_problem"] = None
    if args.stamp.strip():
        try:
            if sb.stamp_path(args.stamp).exists():
                result["stamp_problem"] = f"There is already a stamp called '{args.stamp.strip()}'."
        except sb.Refused as e:
            result["stamp_problem"] = str(e)
    return result


def cmd_building_create(args) -> dict:
    sb, gi, pal, pieces, roof, summary = building(args)
    try:
        out = sb.write(gi, pieces, roof, args.prefix.strip().lower(), args.stamp.strip() or args.prefix.strip(),
                       f"from {args.picture.name} (gecko)")
    except sb.Refused as e:
        raise Refused(str(e))
    roof_text = f" and {len(out['roof_made'])} roof tiles" if out["roof_made"] else ""
    add_pending({"pid": out["made"][0]["pid"], "name": f"{out['made'][0]['name']}..{out['made'][-1]['name']}",
                 "what": f"building '{Path(out['stamp']).stem}': {len(out['made'])} wall pieces{roof_text} "
                         f"from {args.picture.name}"})
    return {**summary, **out}


# ------------------------------------------------------------------------------ apply

def process_alive(pid: int) -> bool:
    if os.name != "nt":
        try:
            os.kill(pid, 0)
            return True
        except OSError:
            return False
    k32 = ctypes.windll.kernel32
    k32.OpenProcess.restype = ctypes.c_void_p
    k32.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
    k32.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    k32.WaitForSingleObject.restype = ctypes.c_uint32
    k32.CloseHandle.argtypes = [ctypes.c_void_p]
    h = k32.OpenProcess(0x00100000, False, pid)          # SYNCHRONIZE
    if not h:
        return False
    try:
        return k32.WaitForSingleObject(h, 0) == 0x102    # WAIT_TIMEOUT: still running
    finally:
        k32.CloseHandle(h)


def geckos_running() -> int:
    try:
        out = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout.lower()
    except OSError:
        return 0
    return out.count('"gecko.exe"')


class Tee:
    """Lines to the log and, unless quiet, to the window."""

    def __init__(self, path: Path, mode: str = "w", quiet: bool = False):
        path.parent.mkdir(parents=True, exist_ok=True)
        self.f = path.open(mode, encoding="utf-8")
        self.quiet = quiet

    def __call__(self, text: str = "") -> None:
        if not self.quiet:
            print(text, flush=True)
        self.f.write(text + "\n")
        self.f.flush()

    def close(self) -> None:
        self.f.close()


def set_title(text: str) -> None:
    if os.name == "nt":
        try:
            ctypes.windll.kernel32.SetConsoleTitleW(text)
        except (AttributeError, OSError):
            pass


def read_status() -> dict:
    try:
        return json.loads(STATUS.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def write_status(**fields) -> None:
    WORK.mkdir(parents=True, exist_ok=True)
    tmp = STATUS.with_suffix(".tmp")
    tmp.write_text(json.dumps(fields), encoding="utf-8")
    os.replace(tmp, STATUS)


def wait_until(check, say, what: str, ask: bool) -> bool:
    """Ask the person to clear a blocker (close the game, close gecko) until check() is true."""
    while not check():
        say(f"\n{what}")
        if not ask:
            return False
        a = input("Close it, then press Enter to go on (or type q and Enter to stop): ").strip().lower()
        if a == "q":
            return False
    return True


def relaunch(gecko: str, map_path: str, say) -> None:
    cmd = [gecko] + (["-m", map_path] if map_path and Path(map_path).is_file() else [])
    say("\nOpening gecko again" + (f" on {Path(map_path).name}" if len(cmd) > 1 else "") + ".")
    flags = 0x00000008 | 0x00000200 if os.name == "nt" else 0       # DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP
    subprocess.Popen(cmd, cwd=str(Path(gecko).parent), creationflags=flags, close_fds=True)


def step_label(i: int, tool: str, extra: list[str]) -> str:
    note = STEP_NOTES.get(tool, "")
    return f"--- Step {i} of {len(GAME_STEPS)}: {tool} {' '.join(extra)}".rstrip() + (f"   ({note})" if note else "")


def banner(say, word: str, line2: str = "") -> None:
    say("")
    say("=" * 44)
    say(f"  {word}")
    if line2:
        say(f"  {line2}")
    say("=" * 44)


def cmd_apply(args) -> int:
    """The window gecko opens. It checks that the deploy may run, then hands the steps to a
    worker with no window of its own and shows the worker's log until it is done: FINISHED and
    closes itself, or FAILED and waits. The worker does not depend on this window, so closing
    it early stops nothing (an Apply cut off half way through repack_art.py, 2026-11-12, left
    the registry swapped and a stamp unrewritten)."""
    set_title("Apply New Art - closing this window does not stop it")
    status = read_status()
    if status.get("state") == "running" and status.get("pid") and process_alive(status["pid"]):
        print("An Apply New Art is already running; showing it.")
        return follow(args)

    say = Tee(APPLY_LOG)
    say("Apply New Art")
    say("=" * 44)
    pending = read_pending()
    if pending:
        say("Waiting to go in:")
        for p in pending:
            say(f"  {p['pid']}  {p.get('name') or ''}  {p.get('what', '')}")
    if args.wait_pid:
        say(f"\nWaiting for gecko (process {args.wait_pid}) to close...")
        t0 = time.time()
        while process_alive(args.wait_pid):
            if time.time() - t0 > 120:
                banner(say, "NOT DONE", "gecko didn't close within two minutes. Nothing was deployed.")
                say.close()
                input("Press Enter to close this window.")
                return 1
            time.sleep(0.3)
    ask = not args.dry_run
    blocked = None
    if BROWSE_MARKER.exists():
        blocked = ("gecko is set up for another game (tools/gecko_browse). Close that gecko so your mod's settings "
                   "come back, then use Apply New Art from the gecko for your mod.")
    elif not wait_until(lambda: geckos_running() == 0, say, "Another gecko window is still open.", ask):
        blocked = "A deploy never runs while gecko is open."
    elif not wait_until(lambda: not games_running(), say, f"The game is running ({', '.join(games_running())}).", ask):
        blocked = "A deploy never runs under the game."
    if blocked:
        banner(say, "NOT DONE", blocked + " Nothing was deployed.")
        if args.gecko and not args.dry_run:
            relaunch(args.gecko, args.map, say)
        say.close()
        if not args.dry_run:
            input("\nPress Enter to close this window.")
        return 1

    if args.dry_run:
        for i, (tool, extra) in enumerate(GAME_STEPS, 1):
            say("\n" + step_label(i, tool, extra))
        if args.gecko:
            say(f"\n(dry run) would open: {args.gecko}" + (f" -m {args.map}" if args.map else ""))
        banner(say, "FINISHED (dry run)", "nothing was run")
        say.close()
        return 0

    say("\nThe work runs on its own: closing this window does not stop it, and gecko opens again at the end.")
    say.close()
    write_status(state="starting", step=0, steps=len(GAME_STEPS), started=time.time())
    cmd = [sys.executable, str(Path(__file__).resolve()), "apply-worker", "--gecko", args.gecko, "--map", args.map]
    if args.fake_steps:
        cmd.append("--fake-steps")
    flags = 0x08000000 | 0x00000200 if os.name == "nt" else 0       # CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP
    worker = subprocess.Popen(cmd, cwd=str(ROOT), stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL, creationflags=flags, close_fds=True)
    write_status(state="running", pid=worker.pid, step=0, steps=len(GAME_STEPS), started=time.time())
    return follow(args)


def follow(args) -> int:
    """Print the log as the worker writes it, until it is done."""
    pos = APPLY_LOG.stat().st_size if APPLY_LOG.exists() else 0
    try:
        while True:
            if APPLY_LOG.exists():
                with APPLY_LOG.open("r", encoding="utf-8", errors="replace") as f:
                    f.seek(pos)
                    chunk = f.read()
                    pos = f.tell()
                if chunk:
                    print(chunk, end="", flush=True)
            st = read_status()
            state = st.get("state")
            if state in ("finished", "failed"):
                break
            if state == "running" and st.get("pid") and not process_alive(st["pid"]):
                time.sleep(0.5)                       # it may have written its last status just now
                if read_status().get("state") == "running":
                    write_status(**{**st, "state": "failed", "error": "the worker stopped without finishing"})
                continue
            time.sleep(0.4)
    except KeyboardInterrupt:
        print("\nStopped watching. The work goes on; its log is", APPLY_LOG)
        return 0
    st = read_status()
    if st.get("state") == "finished":
        print("\nThis window closes in 5 seconds.", flush=True)
        if not args.no_close_wait:
            time.sleep(5)
        return 0
    if st.get("error") and "worker stopped" in st["error"]:
        print(f"\nFAILED: {st['error']}. Its log is {APPLY_LOG}")
    if not args.no_close_wait:
        input("\nPress Enter to close this window.")
    return 1


def cmd_apply_worker(args) -> int:
    """The steps, with no window: the log and the status file are how it is followed."""
    say = Tee(APPLY_LOG, mode="a", quiet=True)
    ok, failed = True, None
    try:
        for i, (tool, extra) in enumerate(GAME_STEPS, 1):
            write_status(state="running", pid=os.getpid(), step=i, steps=len(GAME_STEPS), tool=tool)
            say("\n" + step_label(i, tool, extra))
            if args.fake_steps:                        # the self-test's stand-in for each tool
                cmd = [sys.executable, "-c", f"import time; print('stand-in for {tool}'); time.sleep(0.4)"]
            else:
                cmd = [sys.executable, str(TOOLS / tool), *extra]
            flags = 0x08000000 if os.name == "nt" else 0               # CREATE_NO_WINDOW
            proc = subprocess.Popen(cmd, cwd=ROOT, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace",
                                    creationflags=flags, env={**os.environ, "PYTHONUNBUFFERED": "1"})
            for line in proc.stdout:
                say(line.rstrip("\n"))
            if proc.wait():
                say(f"\n{tool} stopped with an error (exit {proc.returncode}). The steps after it did not run.")
                ok, failed = False, f"step {i}, {tool} (exit {proc.returncode})"
                break
    except Exception:                                  # noqa: BLE001 - written down, not lost
        say(traceback.format_exc())
        ok, failed = False, "the worker itself (see the log)"
    if ok and not args.fake_steps:
        PENDING.unlink(missing_ok=True)
        import shutil
        shutil.rmtree(OVERLAY, ignore_errors=True)      # deployed now: gecko reads it from the mod
    if args.gecko:
        try:
            relaunch(args.gecko, args.map, say)
        except OSError as e:
            say(f"\nCould not open gecko again: {e}")
    if ok:
        banner(say, "FINISHED", "The new pieces are in the palette (Scenery tab, or search for their names).")
    else:
        banner(say, "FAILED", f"at {failed}. Read the lines above; nothing after that step ran.")
    say(f"Log: {APPLY_LOG}")
    say.close()
    write_status(state="finished" if ok else "failed", pid=os.getpid(), step=len(GAME_STEPS), steps=len(GAME_STEPS),
                 error=failed)
    return 0 if ok else 1


# ------------------------------------------------------------------------------ main

def piece_args(p: argparse.ArgumentParser) -> None:
    p.add_argument("picture", type=Path)
    p.add_argument("--name", default="", help="art file name, up to 8 letters")
    p.add_argument("--height", type=int, help="height in game pixels")
    p.add_argument("--scale", type=float, help="instead of --height: game pixels per picture pixel")
    p.add_argument("--kind", choices=list(KINDS), default="prop")
    p.add_argument("--stand", help="foot | centre | X,Y (picture pixel that stands on the hex); "
                                   "default from --kind")
    p.add_argument("--colour", choices=["none", *COLOUR_REFS], default="none",
                   help="move the colours toward these vanilla pieces")
    p.add_argument("--match-amount", type=float, default=1.0)
    p.add_argument("--shadow", action="store_true", help="also make a ground shadow piece")
    p.add_argument("--shadow-name", default="")
    p.add_argument("--white", type=int, default=150)
    p.add_argument("--sat", type=int, default=28)
    p.add_argument("--grain", type=float, default=1.5)
    p.add_argument("--rim", type=float, default=0.30)
    p.add_argument("--radius", type=float, default=3.0)
    p.add_argument("--falloff", type=float, default=0.15)
    p.add_argument("--ao", type=float, help="foot darkening; default from --kind")
    p.add_argument("--seed", type=int, default=7)


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except AttributeError:
        pass
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("info")
    p = sub.add_parser("suggest-name")
    p.add_argument("word", nargs="?", default="")
    p.add_argument("--pid", type=lambda s: int(s, 0), help="start from this scenery proto's art name")
    p.add_argument("--suffix", default="")
    p = sub.add_parser("preview")
    piece_args(p)
    p.add_argument("--out", type=Path, default=WORK / "preview.png")
    p = sub.add_parser("create")
    piece_args(p)
    p.add_argument("--title", required=True, help="the name the game shows")
    p.add_argument("--text", required=True, help="the examine text")
    for name in ("mirror", "resize"):
        p = sub.add_parser(name)
        p.add_argument("pid", type=lambda s: int(s, 0))
        p.add_argument("--name", required=True)
        if name == "resize":
            p.add_argument("--scale", type=float, required=True)
            p.add_argument("--pixel", action="store_true")
    p = sub.add_parser("text")
    p.add_argument("pid", type=lambda s: int(s, 0))
    p.add_argument("--title")
    p.add_argument("--text")
    sub.add_parser("pending")
    sub.add_parser("sources")
    p = sub.add_parser("source")
    p.add_argument("name")
    p.add_argument("--new", action="store_true", help="also list the pieces Fallout 2 doesn't have")
    p = sub.add_parser("port")
    p.add_argument("source")
    p.add_argument("pid", type=lambda s: int(s, 0))
    sub.add_parser("stamp-games")
    p = sub.add_parser("stamp-convert")
    p.add_argument("source")
    p.add_argument("stamp")
    for name in ("building-preview", "building-create"):
        p = sub.add_parser(name)
        p.add_argument("picture", type=Path)
        p.add_argument("--prefix", default="", required=name == "building-create",
                       help="piece names PREFIX001.frm ..., 1 to 5 letters")
        p.add_argument("--stamp", default="", help="the stamp's name in gecko's patterns\\Custom")
        p.add_argument("--height", type=int)
        p.add_argument("--scale", type=float)
        p.add_argument("--line", nargs="+", default=["auto"], help="the foot, left to right: X,Y X,Y ... or auto")
        p.add_argument("--wall-top", default="auto",
                       help="the walls' height above the foot in picture pixels, auto, or none (roof stays in the walls)")
        p.add_argument("--colour", choices=["none", "walls"], default="none")
        p.add_argument("--rim", type=float, default=0.25)
        if name == "building-preview":
            p.add_argument("--out", type=Path, default=WORK / "building.png")
    p = sub.add_parser("apply")
    p.add_argument("--wait-pid", type=int, default=0)
    p.add_argument("--gecko", default="")
    p.add_argument("--map", default="")
    p.add_argument("--dry-run", action="store_true")
    p.add_argument("--fake-steps", action="store_true", help=argparse.SUPPRESS)      # the self-test's
    p.add_argument("--no-close-wait", action="store_true", help=argparse.SUPPRESS)   # the self-test's
    p = sub.add_parser("apply-worker")                 # started by apply, with no window
    p.add_argument("--gecko", default="")
    p.add_argument("--map", default="")
    p.add_argument("--fake-steps", action="store_true", help=argparse.SUPPRESS)
    args = ap.parse_args()

    if args.cmd == "apply-worker":
        return cmd_apply_worker(args)
    if args.cmd == "apply":
        try:
            return cmd_apply(args)
        except Exception:                                       # keep the window open to read it
            traceback.print_exc()
            input("\nApply New Art failed. Press Enter to close this window.")
            return 1

    handlers = {"info": cmd_info, "suggest-name": cmd_suggest_name, "preview": cmd_preview,
                "create": cmd_create, "mirror": cmd_mirror, "resize": cmd_resize,
                "text": cmd_text, "pending": cmd_pending,
                "sources": cmd_sources, "source": cmd_source, "port": cmd_port,
                "stamp-games": cmd_stamp_games, "stamp-convert": cmd_stamp_convert,
                "building-preview": cmd_building_preview, "building-create": cmd_building_create}
    missing = missing_packages() if args.cmd != "info" else []
    if missing:
        result = {"ok": False, "missing": missing, "python_path": sys.executable,
                  "error": f"The Python running the map tools ({sys.executable}) doesn't have {', '.join(missing)}, "
                           f"which they need. Install them with: \"{sys.executable}\" -m pip install {' '.join(missing)}"}
        print(json.dumps(result))
        return 1
    if args.cmd not in NO_BASE_NEEDED and (result := missing_working_copy()):
        print(json.dumps(result))
        return 1
    try:
        result = {"ok": True, **handlers[args.cmd](args)}
    except Refused as e:
        result = {"ok": False, "error": str(e)}
    except SystemExit as e:                                     # a tool's own sys.exit("message")
        result = {"ok": False, "error": str(e.code) if e.code not in (None, 0) else "stopped"}
    except Exception as e:                                      # noqa: BLE001 - gecko shows it
        result = {"ok": False, "error": f"{type(e).__name__}: {e}", "trace": traceback.format_exc()}
        if isinstance(e, ModuleNotFoundError) and e.name in PACKAGES:
            result.update(missing=[PACKAGES[e.name]], python_path=sys.executable)
    print(json.dumps(result))
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
