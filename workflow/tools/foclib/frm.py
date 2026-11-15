"""
frm.py - Fallout's sprite files (.FRM, .FR0-.FR5) and palette, read and written in one place.

    from foclib.frm import load_palette, read_frm, write_frm, frame_image, critter_frames

    pal = load_palette(gi)                      # 256 (r, g, b), 8 bits each; index 0 is transparent
    frm = read_frm(gi.vfs.read(r"art\\critters\\hmcmbtaa.frm"))
    img = frame_image(frm.frames(2)[0], pal)    # PIL RGBA image of direction 2, frame 0
    data = write_frm(frm)                       # the same bytes back (directions that share frames stay shared)

    critter_frames(gi, "hmcmbt", "aa")          # {direction: [Frame, ...]} from the .frm or the .fr0-.fr5 files

The file, big-endian:
    u32 version, u16 fps, u16 action frame, u16 frames per direction,
    i16 shift x[6], i16 shift y[6], u32 frame data offset[6], u32 frame data size,
    then from 0x3E, per frame: u16 width, u16 height, u32 size, i16 x, i16 y, pixels (one palette index each).
A critter animation is either one .FRM with six directions or six files .FR0-.FR5 of one direction each.

New tools read sprites through this. Most of the older sprite tools carry their own reader
(extract_pc_anims, pc_mix, render.Frm ...): they work and are left as they are.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

HEADER = 0x3E
DIRECTIONS = 6

# the engine's colour-cycling tables (fallout2-ce cycle.cc), as 8-bit RGB. color.pal leaves these
# slots black and the engine animates them, so slime, screens, fire and shoreline need the first
# colour of each cycle to draw as anything but black.
ANIMATED_SLOTS = {
    229: [(0, 108, 0), (11, 115, 7), (27, 123, 15), (43, 131, 27)],                              # slime
    233: [(107, 107, 111), (99, 103, 127), (87, 107, 143), (0, 147, 163), (107, 187, 255)],      # monitors
    238: [(255, 0, 0), (215, 0, 0), (147, 43, 11), (255, 119, 0), (255, 59, 0)],                 # slow fire
    243: [(71, 0, 0), (123, 0, 0), (179, 0, 0), (123, 0, 0), (71, 0, 0)],                        # fast fire
    248: [(83, 63, 43), (75, 59, 43), (67, 55, 39), (63, 51, 39), (55, 47, 35), (51, 43, 35)],   # shoreline
    254: [(252, 0, 0)],                                                                          # alarm
}


def palette_from_bytes(raw: bytes) -> list[tuple[int, int, int]]:
    """color.pal: 256 RGB triples of 6-bit values (then lookup tables, ignored). Index 0 is transparent."""
    pal = []
    for i in range(256):
        r, g, b = raw[i * 3], raw[i * 3 + 1], raw[i * 3 + 2]
        # 6-bit -> 8-bit; *4 alone leaves the top end at 252
        pal.append(((r << 2) | (r >> 4), (g << 2) | (g >> 4), (b << 2) | (b >> 4)))
    for first, colours in ANIMATED_SLOTS.items():
        for k, rgb in enumerate(colours):
            pal[first + k] = rgb
    return pal


def load_palette(gi) -> list[tuple[int, int, int]]:
    """The game's palette (a GameIndex's color.pal)."""
    return palette_from_bytes(gi.vfs.read("color.pal"))


@dataclass
class Frame:
    width: int
    height: int
    x: int              # the frame's own offset from the one before it
    y: int
    pixels: bytes       # width * height palette indexes, row by row


@dataclass
class Frm:
    version: int = 4
    fps: int = 10
    action_frame: int = 0
    frames_per_dir: int = 1
    shift_x: list[int] = field(default_factory=lambda: [0] * DIRECTIONS)
    shift_y: list[int] = field(default_factory=lambda: [0] * DIRECTIONS)
    offsets: list[int] = field(default_factory=lambda: [0] * DIRECTIONS)   # which block each direction uses
    blocks: dict[int, list[Frame]] = field(default_factory=dict)           # offset -> that block's frames

    def frames(self, direction: int) -> list[Frame]:
        """The frames a direction shows (a one-direction file gives the same frames for all six)."""
        return self.blocks[self.offsets[direction]]

    @property
    def directions(self) -> int:
        """How many different directions the file holds (1 for scenery, items, tiles and .FRn files)."""
        return len(self.blocks)


def read_frm(data: bytes) -> Frm:
    version, fps, action, fpd = struct.unpack_from(">IHHH", data, 0)
    frm = Frm(version, fps, action, fpd,
              list(struct.unpack_from(">6h", data, 0x0A)), list(struct.unpack_from(">6h", data, 0x16)),
              list(struct.unpack_from(">6I", data, 0x22)))
    for off in sorted(set(frm.offsets)):
        pos, frames = HEADER + off, []
        for _ in range(fpd):
            w, h, size, x, y = struct.unpack_from(">HHIhh", data, pos)
            frames.append(Frame(w, h, x, y, bytes(data[pos + 12:pos + 12 + w * h])))
            pos += 12 + size
        frm.blocks[off] = frames
    return frm


def write_frm(frm: Frm) -> bytes:
    body, moved = bytearray(), {}
    for off in sorted(frm.blocks):
        moved[off] = len(body)
        for f in frm.blocks[off]:
            assert len(f.pixels) == f.width * f.height, "a frame's pixels must be width x height"
            body += struct.pack(">HHIhh", f.width, f.height, f.width * f.height, f.x, f.y) + f.pixels
    head = struct.pack(">IHHH", frm.version, frm.fps, frm.action_frame, frm.frames_per_dir)
    head += struct.pack(">6h", *frm.shift_x) + struct.pack(">6h", *frm.shift_y)
    head += struct.pack(">6I", *[moved[o] for o in frm.offsets]) + struct.pack(">I", len(body))
    return head + bytes(body)


def frame_image(frame: Frame, pal: list[tuple[int, int, int]]):
    """A frame as a PIL RGBA image (palette index 0 is see-through)."""
    from PIL import Image

    rgba = bytearray(frame.width * frame.height * 4)
    for i, p in enumerate(frame.pixels):
        if p:
            r, g, b = pal[p]
            rgba[i * 4:i * 4 + 4] = bytes((r, g, b, 255))
    return Image.frombytes("RGBA", (frame.width, frame.height), bytes(rgba))


def critter_frames(gi, family: str, anim: str = "aa") -> dict[int, list[Frame]]:
    """{direction: frames} of one critter animation: from <family><anim>.frm, or its .fr0-.fr5 files.
    An empty dict when the game has neither."""
    base = f"art\\critters\\{family}{anim}"
    try:
        frm = read_frm(gi.vfs.read(base + ".frm"))
        # a .frm with one block shows the same frames whichever way the critter faces
        return {d: frm.frames(d) for d in range(DIRECTIONS)}
    except Exception:
        pass
    out = {}
    for d in range(DIRECTIONS):
        try:
            out[d] = read_frm(gi.vfs.read(f"{base}.fr{d}")).frames(0)
        except Exception:
            continue
    return out
