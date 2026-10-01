"""
Checks the gbcam C core's PIXEL CAM style against the original Python
implementation (dither.py's dither_gbcam), on the same 128x112 greyscale
images with fixed levels, pattern and light table.

    python tools/host/test_pixelcam.py

Needs gbcam.dll (tools/host/build.ps1), numpy, Pillow, and the Python
reference implementation - see ref_impl.py.
"""
import ctypes
import os
import random
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ref_impl  # noqa: E402

ref_impl.add_to_path()
from pixelboy import dither as pb  # noqa: E402

W, H = 128, 112


def main():
    lib = ctypes.CDLL(os.path.join(HERE, "gbcam.dll"))
    lib.gbc_create.restype = ctypes.c_void_p
    lib.gbc_destroy.argtypes = [ctypes.c_void_p]
    lib.gbc_set.argtypes = [ctypes.c_void_p] + [ctypes.c_int] * 5
    lib.gbc_set_style.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.gbc_set_levels.argtypes = [ctypes.c_void_p, ctypes.c_double, ctypes.c_double]
    lib.gbc_force_table.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.gbc_process_grey.argtypes = [ctypes.c_void_p, ctypes.c_void_p] + [ctypes.c_int] * 4
    lib.gbc_process_grey.restype = ctypes.POINTER(ctypes.c_uint8)

    pal = list(pb.PALETTES)[0]
    rnd = random.Random(7)
    images = {
        "random": np.array([[rnd.randrange(256) for _ in range(W)] for _ in range(H)], np.uint8),
        "gradient": np.array([[(x * 2 + y) % 256 for x in range(W)] for y in range(H)], np.uint8),
        "dark": np.array([[rnd.randrange(60) for _ in range(W)] for _ in range(H)], np.uint8),
    }
    # (contrast, gamma, pattern, high_light)
    cases = [(1.15, 1.0, 6, True), (1.0, 1.3, 7, True), (2.0, 0.7, 0, False),
             (0.8, 0.85, 15, True), (1.5, 1.15, 10, False)]

    failures = 0
    for name, img in images.items():
        rgb = Image.fromarray(np.dstack([img] * 3))
        flat = np.ascontiguousarray(img)
        for c, g, pattern, high in cases:
            _, levels = pb.dither_gbcam(rgb, contrast=c, gamma=g, dither_factor=1 - pattern / 15,
                                        light="high" if high else "low", palette=pal, return_levels=True)
            want = 3 - levels  # PIXEL CAM level 0 = darkest; gbcam shade 3 = darkest

            cam = lib.gbc_create()
            lib.gbc_set_style(cam, 0)
            lib.gbc_set(cam, 8, pattern, 1, 0, 0)  # brightness 8 = no gamma bias
            lib.gbc_set_levels(cam, c, g)
            lib.gbc_force_table(cam, 1 if high else 0)
            ptr = lib.gbc_process_grey(cam, flat.ctypes.data, W, H, W, 0)
            got = np.ctypeslib.as_array(ptr, shape=(H * W,)).reshape(H, W).copy()
            lib.gbc_destroy(cam)

            bad = int((got != want).sum())
            failures += bad != 0
            print(f"[{'PASS' if bad == 0 else 'FAIL'}] {name:8s} contrast {c:4.2f} gamma {g:4.2f} "
                  f"pattern {pattern:2d} {'high' if high else 'low '}  mismatched pixels: {bad}")

    print("ALL PASS" if failures == 0 else f"{failures} FAILED")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
