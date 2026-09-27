"""
Imports frames from a framegroup_*.json pack (the zlib/tile-JSON format
extracted from the original Game Boy Camera cartridge ROM, also read by
PixelBoy's own apps) into assets/frames/*.png, ready for tools/gen_frames.py
to bake into the firmware.

    python tools/import_gb_frames.py path/to/framegroup_wi.json

These packs are copyrighted (Nintendo/Game Freak) - never commit one, and
never commit anything under assets/frames/ that was drawn by tracing one
without permission from its rights holder; this script only exists so
someone who already owns the ROM (or an equivalent legally-obtained pack)
can convert their own copy locally. See .gitignore's
assets/frames/framegroup_*.json entry.

Format and decode algorithm ported from PixelBoy's Android app
(engine/frames/FramePackParser.kt) - canvas is always 160px wide; each
frame's zlib payload (JSON string, Latin-1 bytes) inflates to an inner JSON
object with four sections of 16-byte (8x8, 2bpp planar) tile hex strings:
`upper`/`lower` (flat, CANVAS_W_TILES=20 wide) fill whatever vertical space
isn't the 16x14-tile photo window, `left`/`right` (2-wide tile pairs) are
the margins beside it. Only produces a PNG when the canvas comes out to one
of gen_frames.py's two supported sizes (160x144 "Standard", 160x224 "Wild") -
see that script's docstring; anything else is skipped with a warning, since
there'd be nowhere for it to fit in our own frame format.
"""
import json
import os
import re
import sys
import zlib

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "assets", "frames")

TILE_PX = 8
CANVAS_W_TILES = 20
CANVAS_W = CANVAS_W_TILES * TILE_PX  # 160
PHOTO_W_TILES, PHOTO_H_TILES = 16, 14
PHOTO_X = 2 * TILE_PX  # 16 - left margin is always 2 tiles
LAYOUTS = {144: 2, 224: 5}  # canvas height -> expected upper_rows; gen_frames.py's LAYOUTS - keep in sync

# hardware index (0=lightest..3=darkest) -> grayscale byte that round-trips
# losslessly through gen_frames.py's round(gray/64) quantizer.
LEVEL_GRAY = {0: 192, 1: 128, 2: 64, 3: 0}


def decode_tile(hex_bytes):
    """16 space-separated hex bytes (2 per row: low/high bitplane) -> 8x8 of
    hardware shade indices 0-3."""
    b = bytes.fromhex(hex_bytes.replace(" ", ""))
    rows = [[0] * 8 for _ in range(8)]
    for r in range(8):
        lo, hi = b[r * 2], b[r * 2 + 1]
        for c in range(8):
            bit = 7 - c
            rows[r][c] = (((hi >> bit) & 1) << 1) | ((lo >> bit) & 1)
    return rows


def paste_tile(canvas, tile, tx, ty):
    for r in range(8):
        dest = canvas[ty * 8 + r]
        src = tile[r]
        dest[tx * 8:tx * 8 + 8] = src


def decode_frame(payload):
    raw = payload.encode("latin1")
    inner = json.loads(zlib.decompress(raw))
    upper, lower, left, right = inner["upper"], inner["lower"], inner["left"], inner["right"]

    upper_rows = len(upper) // CANVAS_W_TILES
    lower_rows = len(lower) // CANVAS_W_TILES
    canvas_h_tiles = upper_rows + PHOTO_H_TILES + lower_rows
    canvas_h = canvas_h_tiles * TILE_PX

    # gen_frames.py assumes a fixed photo_y per canvas height (LAYOUTS) - if
    # this pack's own upper_rows doesn't match, the border art would land
    # shifted relative to where the firmware pastes the real photo.
    if LAYOUTS.get(canvas_h) != upper_rows:
        raise ValueError(f"160x{canvas_h} canvas but upper_rows={upper_rows} "
                          f"(expected {LAYOUTS.get(canvas_h, '?')}) - unsupported layout")

    canvas = [[0] * CANVAS_W for _ in range(canvas_h)]

    for i, hexs in enumerate(upper):
        paste_tile(canvas, decode_tile(hexs), i % CANVAS_W_TILES, i // CANVAS_W_TILES)
    for i, hexs in enumerate(lower):
        tx, ty = i % CANVAS_W_TILES, upper_rows + PHOTO_H_TILES + i // CANVAS_W_TILES
        paste_tile(canvas, decode_tile(hexs), tx, ty)
    for row_i, pair in enumerate(left):
        for col_i, hexs in enumerate(pair):
            paste_tile(canvas, decode_tile(hexs), col_i, upper_rows + row_i)
    right_col0 = (PHOTO_X // TILE_PX) + PHOTO_W_TILES
    for row_i, pair in enumerate(right):
        for col_i, hexs in enumerate(pair):
            paste_tile(canvas, decode_tile(hexs), right_col0 + col_i, upper_rows + row_i)

    return canvas


def sanitize(name):
    return re.sub(r"[^A-Za-z0-9_-]+", "_", name).strip("_") or "frame"


# Friendly names for these packs' own ids, ported from PixelBoy's Android app
# (engine/frames/FrameNaming.kt's NAME_OVERRIDES, itself ported from
# pixelboy/frames.py) - preferred over the pack's own "name" field (often
# just "International 01") wherever there's an entry for it.
NAME_OVERRIDES = {
    "int01": "GameBoy", "int02": "Dashes", "int03": "Marbled", "int04": "Film Strip",
    "int05": "Picture Frame", "int06": "Squiggles", "int07": "Diamonds", "int08": "X-Mas",
    "int09": "Caution", "int10": "Bricks", "int11": "Meandering Line", "int12": "Television",
    "int13": "White", "int14": "Black", "int15": "Postage Stamp", "int16": "Kitty and flowers",
    "int17": "Plaid", "int18": "Pattern",
    "jp01": "Pocket Camera", "jp02": "Round Pocket Camera", "jp07": "Nintendo Pocket Camera",
    "wi01": "Mario and Luigi", "wi02": "Super Mario World", "wi03": "Game Boy Camera",
    "wi04": "Yoshi", "wi05": "Legend of Zelda", "wi06": "Wario", "wi07": "Mario Kart 64",
    "wi09": "Pokemon Trainer", "wi10": "Pocket Camera", "wi12": "Blastoise",
    "wi13": "Pikachu and Clefairy", "wi14": "Gakkyu-oh Yamazaki", "wi15": "Bakusou Kyoudai",
    "wi16": "Hello Kitty Pattern", "wi17": "Hello Kitty Comic", "wi18": "Hello Kitty Memo",
    "wi19": "Kitty's Family", "wi20": "Sanrio Friends", "wi21": "Hello Kitty House",
    "wi22": "GameBoy",
}


def main():
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: python {os.path.basename(__file__)} path/to/framegroup_xx.json")
    path = sys.argv[1]
    root = json.load(open(path, encoding="utf-8"))
    state = root.get("state", {})
    frame_list = state.get("frames", [])
    if not frame_list:
        raise SystemExit("no 'state.frames' list found - not a recognized frame-pack file")

    os.makedirs(OUT_DIR, exist_ok=True)
    written, skipped = 0, 0
    for meta in frame_list:
        fid, name, hash_ = meta.get("id", ""), meta.get("name", ""), meta.get("hash", "")
        key = f"frame-{hash_}"
        if not fid or key not in root:
            skipped += 1
            continue
        try:
            canvas = decode_frame(root[key])
        except Exception as e:
            print(f"skip {fid} ({name}): couldn't decode - {e}")
            skipped += 1
            continue
        h = len(canvas)
        display_name = NAME_OVERRIDES.get(fid, name)

        img = Image.new("L", (CANVAS_W, h))
        img.putdata([LEVEL_GRAY[v] for row in canvas for v in row])
        out_path = os.path.join(OUT_DIR, f"{sanitize(fid)}_{sanitize(display_name)}.png")
        img.convert("RGB").save(out_path)
        written += 1
        print(f"{fid} ({display_name}) -> {out_path}")

    print(f"\n{written} frame(s) written, {skipped} skipped. "
          f"Run tools/gen_frames.py next to bake them into the firmware.")


if __name__ == "__main__":
    main()
