"""
Extracts the built-in frames from a real Game Boy Camera cartridge ROM (or a
folder/zip of them) into assets/frames/*.png, ready for tools/gen_frames.py
to bake into the firmware.

    python tools/extract_gb_frames.py "path/to/rom_or_folder_or.zip"

Accepts a single .gb/.gbc file, a .zip containing one or more of them, or a
folder containing any mix of those (non-recursive) - matching how a
"/ROMS" folder on the camera's own SD card is likely organized. Every
Standard frame (18 slots) and Wild frame (8 slots) the ROM has - including
ones that turn out blank/unused in that particular release - is extracted;
delete any you don't want from assets/frames/ afterwards.

ROM offsets and the tile-position layout are ported from cristofercruz's
gb-camera-frames (gbc-fr.py: https://github.com/cristofercruz/gb-camera-frames,
credited there to jkbenaim's gbcamextract for the addresses) - that project
INJECTS frame images into a ROM; this does the reverse (extracts them back
out). The Hello Kitty Pocket Camera release stores frames at a different,
non-contiguous set of offsets (HELLO_KITTY_*_OFFSETS below) - detected by
its distinct cartridge header title.

Also handles untoxa/gb-photo (https://github.com/untoxa/gb-photo), an
open-source GB Camera-alike - a completely different ROM, detected the same
way regardless of file size or title. Unlike the commercial ROM, gb-photo is
built with GBDK/SDCC, whose linker assigns bank/address per build - there's
no fixed offset to hardcode. Instead this locates its `print_frames[]` table
(src/print_frames.c upstream) by its own distinctive byte signature: a
`frame_desc_t` entry (see include/print_frames.h) with every pointer/bank
field zeroed (the "No Frame" placeholder, always first) - self-locating, so
it works across any build/version. gb-photo frames can use any canvas size
and photo position, unlike this pipeline's fixed Standard/Wild - ones that
don't match either are skipped with a warning (gb-photo's own "GB Camera"
frame currently doesn't fit, for exactly this reason).

These ROMs are copyrighted (Nintendo/Game Freak, or GPL for gb-photo) -
review anything you add to assets/frames/ from one before committing it,
and never commit a commercial ROM's own copy of a frame if that ROM itself
isn't yours to redistribute.
"""
import os
import re
import sys
import zipfile

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "assets", "frames")

TILE_PX = 8
CANVAS_W_TILES = 20
PHOTO_H_TILES = 14

ROM_TITLE_OFFSET = 0x134
ROM_TITLE_LENGTH = 0xF
HELLO_KITTY_TITLE = b"POCKETCAMERA_SN"

STANDARD_FRAME_OFFSET = 0xD0000
STANDARD_FRAME_LENGTH = 0x600     # 96 tiles, 16 bytes each
STANDARD_FRAME_MAP_LENGTH = 0x88  # 136 bytes, one tile-index (0-95) per border position
STANDARD_SLOT_LENGTH = STANDARD_FRAME_LENGTH + STANDARD_FRAME_MAP_LENGTH
STANDARD_SLOTS = 18

WILD_FRAME_OFFSET = 0xC4000
WILD_FRAME_LENGTH = 0x1800  # 384 tiles' worth; only the first 336 (below) are ever placed
WILD_SLOTS = 8

BANK_SHIFT = 0x4000
TILE_LENGTH = 0x10

MIN_ROM_SIZE = 0x100000  # every known GB Camera release is exactly 1MB

# Border tile positions (1-indexed, row-major in a CANVAS_W_TILES-wide grid) -
# ported verbatim from gbc-fr.py's standardTopBottomTilePositions /
# standardSidesTilePositions / wildTopBottomTilePositions /
# wildLeftRightTilePositions. Order matters: it's the order tiles are packed
# into the ROM.
STANDARD_TOP_BOTTOM = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 321, 322, 323, 324, 325, 326, 327, 328, 329, 330, 331, 332, 333, 334, 335, 336, 337, 338, 339, 340, 341, 342, 343, 344, 345, 346, 347, 348, 349, 350, 351, 352, 353, 354, 355, 356, 357, 358, 359, 360]
STANDARD_SIDES = [41, 42, 61, 62, 81, 82, 101, 102, 121, 122, 141, 142, 161, 162, 181, 182, 201, 202, 221, 222, 241, 242, 261, 262, 281, 282, 301, 302, 59, 60, 79, 80, 99, 100, 119, 120, 139, 140, 159, 160, 179, 180, 199, 200, 219, 220, 239, 240, 259, 260, 279, 280, 299, 300, 319, 320]
WILD_TOP_BOTTOM = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 381, 382, 383, 384, 385, 386, 387, 388, 389, 390, 391, 392, 393, 394, 395, 396, 397, 398, 399, 400, 401, 402, 403, 404, 405, 406, 407, 408, 409, 410, 411, 412, 413, 414, 415, 416, 417, 418, 419, 420, 421, 422, 423, 424, 425, 426, 427, 428, 429, 430, 431, 432, 433, 434, 435, 436, 437, 438, 439, 440, 441, 442, 443, 444, 445, 446, 447, 448, 449, 450, 451, 452, 453, 454, 455, 456, 457, 458, 459, 460, 461, 462, 463, 464, 465, 466, 467, 468, 469, 470, 471, 472, 473, 474, 475, 476, 477, 478, 479, 480, 481, 482, 483, 484, 485, 486, 487, 488, 489, 490, 491, 492, 493, 494, 495, 496, 497, 498, 499, 500, 501, 502, 503, 504, 505, 506, 507, 508, 509, 510, 511, 512, 513, 514, 515, 516, 517, 518, 519, 520, 521, 522, 523, 524, 525, 526, 527, 528, 529, 530, 531, 532, 533, 534, 535, 536, 537, 538, 539, 540, 541, 542, 543, 544, 545, 546, 547, 548, 549, 550, 551, 552, 553, 554, 555, 556, 557, 558, 559, 560]
WILD_SIDES = [101, 102, 121, 122, 141, 142, 161, 162, 181, 182, 201, 202, 221, 222, 241, 242, 261, 262, 281, 282, 301, 302, 321, 322, 341, 342, 361, 362, 119, 120, 139, 140, 159, 160, 179, 180, 199, 200, 219, 220, 239, 240, 259, 260, 279, 280, 299, 300, 319, 320, 339, 340, 359, 360, 379, 380]

HELLO_KITTY_STANDARD_OFFSETS = [
    [0xC6C70, 0xCF5D0], [0xC3B80, 0xCF548], [0xCBEC0, 0xCF4C0], [0xC5F10, 0xCF658],
    [0xCF210, 0xCF7F0], [0xC73A0, 0xCF768], [0xB7420, 0xCF6E0], [0xBE3E0, 0xCF438],
    [0xB3CD0, 0xC7EF0], [0xB2B80, 0xCF3B0], [0x8FD50, 0xC7F78], [0xC3800, 0xD7800],
    [0xBDC00, 0xD3F70], [0xD7F70, 0xD7888], [0xC5C00, 0xD7998], [0xB7C20, 0xD7910],
    [0xC3ED0, 0xD3D50], [0x33F80, 0xD3CC8], [0xDB800, 0xD3DD8], [0xB2200, 0xD3EE8],
    [0xB34D0, 0xD3E60], [0xB3030, 0xD7A20], [0x93E00, 0xD7D50], [0x77FE0, 0xCFCB8],
    [0x77FF0, 0xCFDC4],
]
HELLO_KITTY_WILD_OFFSETS = [0x6C000, 0x60000, 0x64000, 0x65800, 0x69800, 0x68000]

LEVEL_GRAY = {0: 192, 1: 128, 2: 64, 3: 0}  # hardware index -> gray, see import_gb_frames.py


def decode_tile(b16):
    """16 raw bytes (2 per row: low/high bitplane) -> 8x8 of hardware shade
    indices 0-3 - same GB 2bpp format as our own tiles."""
    rows = [[0] * 8 for _ in range(8)]
    for r in range(8):
        lo, hi = b16[r * 2], b16[r * 2 + 1]
        for c in range(8):
            bit = 7 - c
            rows[r][c] = (((hi >> bit) & 1) << 1) | ((lo >> bit) & 1)
    return rows


def paste_tile(canvas, tile, tx, ty):
    for r in range(8):
        canvas[ty * 8 + r][tx * 8:tx * 8 + 8] = tile[r]


def canvas_for(positions):
    max_pos = max(positions)
    rows = (max_pos + CANVAS_W_TILES - 1) // CANVAS_W_TILES
    return rows


def build_canvas(rom, tile_offset, map_offset, top_bottom_positions, sides_positions,
                  n_unique_tiles, has_map):
    # The *_SIDES lists above are grouped by column for readability (every
    # left-margin position, then every right-margin position), but the ROM
    # itself was written by a scan over ascending tile position (1, 2, 3...)
    # - for the sides that means left/right pairs interleaved row by row,
    # not grouped. sorted() reconstructs that true write order; top/bottom's
    # own list is already in it (nothing else falls between position 40 and
    # 321 to interleave with).
    total_positions = top_bottom_positions + sorted(sides_positions)
    canvas_h_tiles = canvas_for(total_positions)
    canvas = [[0] * (CANVAS_W_TILES * TILE_PX) for _ in range(canvas_h_tiles * TILE_PX)]

    def pos_to_xy(pos):
        p = pos - 1
        return p % CANVAS_W_TILES, p // CANVAS_W_TILES

    if has_map:
        tiles = [decode_tile(rom[tile_offset + i * TILE_LENGTH: tile_offset + (i + 1) * TILE_LENGTH])
                 for i in range(n_unique_tiles)]
        tile_map = list(rom[map_offset:map_offset + len(total_positions)])
        for i, pos in enumerate(total_positions):
            tx, ty = pos_to_xy(pos)
            paste_tile(canvas, tiles[tile_map[i]], tx, ty)
    else:
        for i, pos in enumerate(total_positions):
            off = tile_offset + i * TILE_LENGTH
            tile = decode_tile(rom[off:off + TILE_LENGTH])
            tx, ty = pos_to_xy(pos)
            paste_tile(canvas, tile, tx, ty)

    return canvas, canvas_h_tiles


def canvas_to_image(canvas):
    h = len(canvas)
    w = len(canvas[0])
    img = Image.new("L", (w, h))
    img.putdata([LEVEL_GRAY[v] for row in canvas for v in row])
    return img.convert("RGB")


def sanitize(name):
    return re.sub(r"[^A-Za-z0-9_-]+", "_", name).strip("_") or "rom"


# Different ROM releases (regions, revisions, the .zip vs. loose .gb copies)
# routinely share identical frames - dedupe by actual pixel content within
# one run, not by filename, so pointing this at a whole /ROMS folder doesn't
# produce a pile of pixel-identical PNGs under different names.
SEEN_CANVASES = set()


def save_if_new(canvas, path):
    key = (len(canvas[0]), len(canvas), bytes(v for row in canvas for v in row))
    if key in SEEN_CANVASES:
        print(f"  (skipped {os.path.basename(path)}: duplicate of an already-extracted frame)")
        return False
    SEEN_CANVASES.add(key)
    canvas_to_image(canvas).save(path)
    return True


# ---------------------------------------------------------- untoxa/gb-photo
# See the module docstring for why this locates its data by signature
# instead of a fixed offset. GBPHOTO_ANCHOR is a frame_desc_t (include/
# print_frames.h upstream) with every pointer/bank field zeroed - the
# always-first "No Frame" placeholder entry.
GBPHOTO_ANCHOR = bytes([0x0E, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x02])
GBPHOTO_MAP_WIDTH = 20  # PRN_TILE_WIDTH
GBPHOTO_LAYOUTS = {(18, 2, 2), (28, 5, 2)}  # (height, image_y, image_x) tiles this pipeline supports


def resolve_ptr(bank, ptr):
    return None if ptr < 0x4000 else bank * 0x4000 + (ptr - 0x4000)


def decode_gbphoto_frame(rom, map_off, tiles_off, height):
    map_len = GBPHOTO_MAP_WIDTH * height
    tile_indices = rom[map_off:map_off + map_len]
    if len(tile_indices) < map_len:
        return None
    canvas = [[0] * (GBPHOTO_MAP_WIDTH * TILE_PX) for _ in range(height * TILE_PX)]
    tile_cache = {}
    for i, idx in enumerate(tile_indices):
        if idx not in tile_cache:
            off = tiles_off + idx * TILE_LENGTH
            tile_cache[idx] = decode_tile(rom[off:off + TILE_LENGTH])
        paste_tile(canvas, tile_cache[idx], i % GBPHOTO_MAP_WIDTH, i // GBPHOTO_MAP_WIDTH)
    return canvas


def extract_gbphoto_rom(data, label):
    """Returns None if this doesn't look like a gb-photo ROM at all (caller
    should try the commercial-ROM path instead); otherwise the number of
    frames written (which can be 0)."""
    search_from = 0
    while True:
        anchor = data.find(GBPHOTO_ANCHOR, search_from)
        if anchor < 0:
            return None
        bank = anchor // 0x4000
        cap_ptr = data[anchor + 12] | (data[anchor + 13] << 8)
        cap_off = resolve_ptr(bank, cap_ptr)
        if cap_off is not None and data[cap_off:cap_off + 8] == b"No Frame":
            break
        search_from = anchor + 1

    num_banks = len(data) // 0x4000
    stem = sanitize(label)
    written = 0
    print(f"{label}: gb-photo frame table at ROM offset {hex(anchor)} (bank {bank})")

    i = 1  # entry 0 is the "No Frame" anchor itself - no graphics to extract
    while True:
        off = anchor + i * 16
        if off + 16 > len(data):
            break
        e = data[off:off + 16]
        height = e[0]
        map_ptr, map_bank = e[1] | (e[2] << 8), e[3]
        tiles_ptr, tiles_bank = e[4] | (e[5] << 8), e[6]
        image_y, image_x = e[10], e[11]
        cap_ptr = e[12] | (e[13] << 8)

        if not (0 < map_bank < num_banks and 0 < tiles_bank < num_banks and
                map_ptr >= 0x4000 and tiles_ptr >= 0x4000):
            break  # past the end of the real table

        cap_off = resolve_ptr(bank, cap_ptr)
        caption = (data[cap_off:cap_off + 20].split(b"\x00")[0].decode("ascii", "replace")
                  if cap_off is not None else f"frame{i}")

        if (height, image_y, image_x) not in GBPHOTO_LAYOUTS:
            print(f"  skip \"{caption}\": {height*8}px tall, photo at ({image_x*8},{image_y*8})px "
                  f"- doesn't match a supported layout")
            i += 1
            continue

        map_off = resolve_ptr(map_bank, map_ptr)
        tiles_off = resolve_ptr(tiles_bank, tiles_ptr)
        canvas = decode_gbphoto_frame(data, map_off, tiles_off, height)
        if canvas and save_if_new(canvas, os.path.join(OUT_DIR, f"{stem}_{sanitize(caption)}.png")):
            written += 1
        i += 1

    return written


def extract_rom(data, label):
    gbphoto = extract_gbphoto_rom(data, label)
    if gbphoto is not None:
        return gbphoto

    if len(data) < MIN_ROM_SIZE:
        print(f"skip {label}: {len(data)} bytes, too small to be a GB Camera ROM "
              f"(expected {MIN_ROM_SIZE})")
        return 0
    title = data[ROM_TITLE_OFFSET:ROM_TITLE_OFFSET + ROM_TITLE_LENGTH]
    is_hk = title == HELLO_KITTY_TITLE
    print(f"{label}: title={title!r}{' (Hello Kitty)' if is_hk else ''}")

    stem = sanitize(label)
    written = 0

    if is_hk:
        for i, (tile_off, map_off) in enumerate(HELLO_KITTY_STANDARD_OFFSETS):
            canvas, h = build_canvas(data, tile_off, map_off, STANDARD_TOP_BOTTOM,
                                     STANDARD_SIDES, 96, has_map=True)
            if save_if_new(canvas, os.path.join(OUT_DIR, f"{stem}_std{i+1}.png")):
                written += 1
        for i, off in enumerate(HELLO_KITTY_WILD_OFFSETS):
            canvas, h = build_canvas(data, off, None, WILD_TOP_BOTTOM, WILD_SIDES,
                                     len(WILD_TOP_BOTTOM) + len(WILD_SIDES), has_map=False)
            if save_if_new(canvas, os.path.join(OUT_DIR, f"{stem}_wild{i+1}.png")):
                written += 1
        return written

    for slot in range(STANDARD_SLOTS):
        bank = BANK_SHIFT if slot >= 9 else 0
        idx = slot if slot < 9 else slot - 9
        base = STANDARD_FRAME_OFFSET + bank + STANDARD_SLOT_LENGTH * idx
        canvas, h = build_canvas(data, base, base + STANDARD_FRAME_LENGTH,
                                 STANDARD_TOP_BOTTOM, STANDARD_SIDES, 96, has_map=True)
        if save_if_new(canvas, os.path.join(OUT_DIR, f"{stem}_std{slot+1}.png")):
            written += 1

    for slot in range(WILD_SLOTS):
        base = WILD_FRAME_OFFSET + WILD_FRAME_LENGTH * slot  # always slot < 9, no bank shift
        canvas, h = build_canvas(data, base, None, WILD_TOP_BOTTOM, WILD_SIDES,
                                 len(WILD_TOP_BOTTOM) + len(WILD_SIDES), has_map=False)
        if save_if_new(canvas, os.path.join(OUT_DIR, f"{stem}_wild{slot+1}.png")):
            written += 1

    return written


def process_path(path, total):
    lower = path.lower()
    if lower.endswith(".gb") or lower.endswith(".gbc"):
        label = os.path.splitext(os.path.basename(path))[0]
        total[0] += extract_rom(open(path, "rb").read(), label)
    elif lower.endswith(".zip"):
        with zipfile.ZipFile(path) as zf:
            for name in zf.namelist():
                nl = name.lower()
                if nl.endswith(".gb") or nl.endswith(".gbc"):
                    label = os.path.splitext(os.path.basename(name))[0]
                    total[0] += extract_rom(zf.read(name), label)


def main():
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: python {os.path.basename(__file__)} path/to/rom_or_folder_or.zip")
    target = sys.argv[1]
    os.makedirs(OUT_DIR, exist_ok=True)

    total = [0]
    if os.path.isdir(target):
        for name in sorted(os.listdir(target)):
            process_path(os.path.join(target, name), total)
    else:
        process_path(target, total)

    print(f"\n{total[0]} frame(s) written to {OUT_DIR}. "
          f"Delete any you don't want, then run tools/gen_frames.py.")


if __name__ == "__main__":
    main()
