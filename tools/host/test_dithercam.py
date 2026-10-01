"""
Checks the gbcam C core's Dither Cam against the original Python
implementation (dithercam.py's pure-Python _quantize_* reference versions),
on the same RGB images with the same palette/method/levels.

    python tools/host/test_dithercam.py

The error-diffusion methods run in float32 here and float64 in Python, so a
handful of pixels can land on a different (equally close) colour after the
error has travelled across the image; those are reported, and anything above
0.5% counts as a failure. Needs the Python reference implementation - see
ref_impl.py.
"""
import ctypes
import os
import random
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import ref_impl  # noqa: E402

ref_impl.add_to_path()
from pixelboy import dithercam as dc  # noqa: E402

METHODS = ["none", "bayer4", "bayer8", "floyd_steinberg", "atkinson", "sierra_lite"]
PY = {
    "none": dc._quantize_none,
    "bayer4": dc._quantize_bayer4,
    "bayer8": dc._quantize_bayer8,
    "floyd_steinberg": dc._quantize_floyd_steinberg_py,
    "atkinson": dc._quantize_atkinson_py,
    "sierra_lite": dc._quantize_sierra_lite_py,
}
PAL_DIR = os.path.join(ROOT, "assets", "palettes", "dithercam")


def main():
    lib = ctypes.CDLL(os.path.join(HERE, "gbcam.dll"))
    lib.gbc_dc_quantize.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                    ctypes.c_double, ctypes.c_double, ctypes.c_double, ctypes.c_void_p]
    names = sorted(f[:-4] for f in os.listdir(PAL_DIR) if f.endswith(".hex"))
    rnd = random.Random(3)
    w, h = 120, 90
    yy, xx = np.mgrid[0:h, 0:w]
    images = {
        "gradient": np.dstack([xx * 255 / (w - 1), yy * 255 / (h - 1), (xx + yy) * 255 / (w + h)]).astype(np.uint8),
        "random": np.array([[[rnd.randrange(256) for _ in range(3)] for _ in range(w)] for _ in range(h)], np.uint8),
    }
    cases = [("c64", 1.0, 1.0, 0.5), ("pico8", 1.3, 0.85, 0.75), ("gb", 1.0, 1.0, 1.0), ("db32", 0.8, 1.15, 0.25)]

    worst = 0.0
    failures = 0
    for img_name, img in images.items():
        flat = np.ascontiguousarray(img)
        for pal_name, contrast, gamma, amount in cases:
            pal_idx = names.index(pal_name)
            pal = np.array(dc.PALETTES[pal_name], dtype=np.float32)
            rgb = img.astype(np.float64)
            if contrast != 1.0 or gamma != 1.0:
                v = np.clip((rgb / 255.0 - 0.5) * contrast + 0.5, 0.0, 1.0)
                rgb = np.power(v, gamma) * 255.0
            for m_idx, m in enumerate(METHODS):
                want = PY[m](rgb, pal, amount)
                out = np.zeros((h, w, 3), np.uint8)
                lib.gbc_dc_quantize(flat.ctypes.data, w, h, pal_idx, m_idx, amount, contrast, gamma, out.ctypes.data)
                bad = float(np.any(out != want, axis=2).mean() * 100)
                worst = max(worst, bad)
                ok = bad <= 0.5
                failures += not ok
                print(f"[{'PASS' if ok else 'FAIL'}] {img_name:8s} {pal_name:6s} {m:15s} contrast {contrast} gamma {gamma} "
                      f"amount {amount}: {bad:.3f}% pixels differ")
    print(f"worst {worst:.3f}%  ->  {'ALL PASS' if failures == 0 else f'{failures} FAILED'}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
