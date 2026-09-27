"""
Contrast sweep comparison on a photo: all 16 contrast levels in
  - gbcam PIXEL CAM style (C core, what the camera runs)
  - the original PIXEL CAM Python (PIXEL CAM/pixelboy/dither.py)
  - gbcam HARDWARE style (C core)

    python tools/host/sweep_compare.py <photo> [-o out.png] [--mirror]

Needs gbcam.dll (tools/host/build.ps1), numpy, Pillow, OpenCV.
"""
import argparse
import os
import sys

import cv2
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "PIXEL CAM"))
from pixelboy import dither as pb  # noqa: E402

import gbcam_live as g  # noqa: E402

GREY4 = np.array([255, 170, 85, 0], np.uint8)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("photo")
    ap.add_argument("-o", "--out", help="output PNG (default samples/out/<name>_sweep_compare.png)")
    ap.add_argument("--mirror", action="store_true", help="mirror the photo horizontally")
    args = ap.parse_args()

    frame = cv2.imread(args.photo, cv2.IMREAD_COLOR)
    if frame is None:
        sys.exit(f"cannot read {args.photo}")
    if args.mirror:
        frame = cv2.flip(frame, 1)
    h, w = frame.shape[:2]
    print(f"{args.photo}: {w}x{h}")

    # Same greyscale weights as PIXEL CAM (webgbcam's 0.3/0.59/0.11) for all three.
    rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
    grey = np.ascontiguousarray(np.clip(rgb @ np.array([0.30, 0.59, 0.11]), 0, 255).astype(np.uint8))

    lib = g.load_core()

    def ours(style, contrast, iters):
        c = lib.gbc_create()
        lib.gbc_set(c, 8, contrast, 1, 0, 0)
        lib.gbc_set_style(c, style)
        for _ in range(iters):  # let auto-exposure / smoothed auto-levels settle
            p = lib.gbc_process_grey(c, grey.ctypes.data, w, h, grey.strides[0], 0)
        sh = np.ctypeslib.as_array(p, shape=(112 * 128,)).reshape(112, 128).copy()
        lib.gbc_destroy(c)
        return sh

    # Original: centre crop to 8:7, LANCZOS to 128x112, auto levels, dither_gbcam.
    if w * 7 > h * 8:
        cw = h * 8 // 7
        crop = rgb[:, (w - cw) // 2:(w - cw) // 2 + cw]
    else:
        ch = w * 7 // 8
        crop = rgb[(h - ch) // 2:(h - ch) // 2 + ch, :]
    small = Image.fromarray(crop).resize((128, 112), Image.LANCZOS)
    pal = list(pb.PALETTES)[0]

    def original(contrast):
        f = 1 - contrast / 15
        fn = lambda p, c, gm: pb.dither_gbcam(p, contrast=c, gamma=gm, dither_factor=f,  # noqa: E731
                                             palette=pal, return_levels=True)[1]
        c, gm = pb.search_best_levels(small, fn)
        return 3 - pb.dither_gbcam(small, contrast=c, gamma=gm, dither_factor=f, palette=pal, return_levels=True)[1]

    def grid(title, fn):
        pad = 3
        cellw, cellh = 128 + pad, 112 + pad + 12
        out = np.full((30 + 4 * cellh, 4 * cellw + pad), 110, np.uint8)
        cv2.putText(out, title, (6, 21), cv2.FONT_HERSHEY_SIMPLEX, 0.6, 255, 1, cv2.LINE_AA)
        for k in range(16):
            sh = fn(k).astype(np.int64)
            x, y = pad + (k % 4) * cellw, 30 + (k // 4) * cellh
            out[y:y + 112, x:x + 128] = GREY4[sh]
            cv2.putText(out, f"contrast {k}" + (" *" if k == 7 else ""), (x + 2, y + 123),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.36, 255, 1, cv2.LINE_AA)
        return out

    grids = [grid("OURS: PIXEL CAM style", lambda k: ours(0, k, 25)),
             grid("ORIGINAL PIXEL CAM (Python)", original),
             grid("OURS: HARDWARE style", lambda k: ours(1, k, 40))]
    gap = np.full((grids[0].shape[0], 12), 60, np.uint8)
    sheet = np.hstack([grids[0], gap, grids[1], gap, grids[2]])

    out = args.out or os.path.join(ROOT, "samples", "out",
                                   os.path.splitext(os.path.basename(args.photo))[0] + "_sweep_compare.png")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    cv2.imwrite(out, sheet)
    print("wrote", out)


if __name__ == "__main__":
    main()
