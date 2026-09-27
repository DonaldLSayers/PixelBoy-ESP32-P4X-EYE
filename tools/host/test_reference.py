"""
Checks the gbcam C core against an independent Python port of the Game Boy
Camera reference emulation in Pan Docs (Antonio Nino Diaz / GiiBiiAdvance),
and checks the generated dither registers against real Game Boy Camera
register dumps (Raphael Boichot, Mitsubishi-M64282FP-dashcam config.h).

    python tools/host/test_reference.py

Needs gbcam.dll (tools/host/build.ps1).
"""
import ctypes
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
W, H = 128, 112

# Dither registers written by the stock Game Boy Camera ROM (A006-A035).
REAL_HIGH = [0x89, 0x92, 0xA2, 0x8F, 0x9E, 0xC6, 0x8A, 0x95, 0xAB, 0x91, 0xA1, 0xCF, 0x8D, 0x9A, 0xBA, 0x8B,
             0x96, 0xAE, 0x8F, 0x9D, 0xC3, 0x8C, 0x99, 0xB7, 0x8A, 0x94, 0xA8, 0x90, 0xA0, 0xCC, 0x89, 0x93,
             0xA5, 0x90, 0x9F, 0xC9, 0x8E, 0x9C, 0xC0, 0x8C, 0x98, 0xB4, 0x8E, 0x9B, 0xBD, 0x8B, 0x97, 0xB1]
REAL_LOW = [0x8C, 0x98, 0xAC, 0x95, 0xA7, 0xDB, 0x8E, 0x9B, 0xB7, 0x97, 0xAA, 0xE7, 0x92, 0xA2, 0xCB, 0x8F,
            0x9D, 0xBB, 0x94, 0xA5, 0xD7, 0x91, 0xA0, 0xC7, 0x8D, 0x9A, 0xB3, 0x96, 0xA9, 0xE3, 0x8C, 0x99,
            0xAF, 0x95, 0xA8, 0xDF, 0x93, 0xA4, 0xD3, 0x90, 0x9F, 0xC3, 0x92, 0xA3, 0xCF, 0x8F, 0x9E, 0xBF]

EDGE_AUTO, EDGE_NONE, EDGE_H, EDGE_2D = 0, 1, 2, 3
EDGE_RATIOS = [0.50, 0.75, 1.00, 1.25, 2.00, 3.00, 4.00, 5.00]


def cdiv(a, b):
    """C integer division (truncates toward zero)."""
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b > 0) else -q


def clamp(lo, v, hi):
    return lo if v < lo else hi if v > hi else v


def reference(luma, exposure, edge, ratio_idx, matrix):
    """Pan Docs GB_CameraTakePicture(), sensor rows already cropped to 128x112.
    exposure is the A002/A003 value; the reference scales by exposure / 0x0300."""
    alpha = EDGE_RATIOS[ratio_idx]
    buf = [[0] * H for _ in range(W)]
    for i in range(W):
        for j in range(H):
            v = (luma[j][i] * exposure) // 0x0300
            v = 128 + cdiv(v - 128, 8)
            buf[i][j] = clamp(0, v, 255) - 128           # make signed
    if edge in (EDGE_2D, EDGE_H):
        tmp = [[0] * H for _ in range(W)]
        for i in range(W):
            for j in range(H):
                px = buf[i][j]
                mw, me = buf[max(0, i - 1)][j], buf[min(i + 1, W - 1)][j]
                if edge == EDGE_2D:
                    mn, ms = buf[i][max(0, j - 1)], buf[i][min(j + 1, H - 1)]
                    val = px + (4 * px - mw - me - mn - ms) * alpha
                else:
                    val = px + (2 * px - mw - me) * alpha
                tmp[i][j] = clamp(-128, int(val), 127)   # int() truncates like C
        buf = tmp
    out = [[0] * W for _ in range(H)]
    for i in range(W):
        for j in range(H):
            value = buf[i][j] + 128                      # make unsigned
            base = ((j & 3) * 4 + (i & 3)) * 3
            r0, r1, r2 = matrix[base:base + 3]
            c = 0x00 if value < r0 else 0x40 if value < r1 else 0x80 if value < r2 else 0xC0
            out[j][i] = 3 - (c >> 6)
    return out


def main():
    path = os.path.join(HERE, "gbcam.dll")
    if not os.path.exists(path):
        sys.exit("gbcam.dll not found - run tools\\host\\build.ps1")
    lib = ctypes.CDLL(path)
    lib.gbc_create.restype = ctypes.c_void_p
    lib.gbc_set.argtypes = [ctypes.c_void_p] + [ctypes.c_int] * 5
    lib.gbc_set_manual_gain.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.gbc_process_grey.argtypes = [ctypes.c_void_p, ctypes.c_void_p] + [ctypes.c_int] * 4
    lib.gbc_process_grey.restype = ctypes.POINTER(ctypes.c_uint8)
    lib.gbc_tier.argtypes = [ctypes.c_void_p]
    lib.gbc_destroy.argtypes = [ctypes.c_void_p]
    lib.gbc_force_table.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.gbc_set_style.argtypes = [ctypes.c_void_p, ctypes.c_int]

    failures = 0

    # 1. Generated dither registers vs. real stock camera dumps.
    lib.gbc_matrix.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_uint8)]
    for name, high, real in (("high-light", 1, REAL_HIGH), ("low-light", 0, REAL_LOW)):
        m = (ctypes.c_uint8 * 48)()
        lib.gbc_matrix(1, high, 7, m)
        ok = list(m) == real
        failures += not ok
        print(f"[{'PASS' if ok else 'FAIL'}] contrast 7 {name} matrix == stock Game Boy Camera registers")

    # 2. Full pipeline vs. the Pan Docs reference, on random and gradient images.
    rnd = random.Random(1234)
    images = {
        "random": [[rnd.randrange(256) for _ in range(W)] for _ in range(H)],
        "gradient": [[(x * 2 + y) % 256 for x in range(W)] for y in range(H)],
        "dark": [[rnd.randrange(40) for _ in range(W)] for _ in range(H)],
    }
    # (edge mode, ratio index, contrast, high_light, gain_q8)
    cases = [
        (EDGE_2D, 0, 7, 1, 896),
        (EDGE_2D, 3, 7, 1, 896),
        (EDGE_H, 0, 7, 1, 512),
        (EDGE_NONE, 0, 7, 0, 3072),
        (EDGE_2D, 0, 0, 1, 1280),
        (EDGE_2D, 0, 15, 1, 1024),
    ]
    for img_name, img in images.items():
        flat = bytes(v for row in img for v in row)
        buf = ctypes.create_string_buffer(flat, len(flat))
        for edge, ratio, contrast, high, gain in cases:
            cam = lib.gbc_create()
            lib.gbc_set_style(cam, 1)  # hardware style
            # gbc_set's dither index 1 = DEFAULT (stock Bayer)
            lib.gbc_set(cam, 8, contrast, 1, edge, ratio)
            lib.gbc_set_manual_gain(cam, gain)
            lib.gbc_force_table(cam, high)
            ptr = lib.gbc_process_grey(cam, ctypes.addressof(buf), W, H, W, 0)
            got = [[ptr[y * W + x] for x in range(W)] for y in range(H)]
            m = (ctypes.c_uint8 * 48)()
            lib.gbc_matrix(1, high, contrast, m)
            exposure = gain * 0x0300 // 256
            want = reference(img, exposure, edge, ratio, list(m))
            bad = sum(g != w for gr, wr in zip(got, want) for g, w in zip(gr, wr))
            ok = bad == 0
            failures += not ok
            print(f"[{'PASS' if ok else 'FAIL'}] {img_name:8s} edge={['auto','none','horiz','2d'][edge]:5s} "
                  f"ratio={int(EDGE_RATIOS[ratio]*100)}% contrast={contrast:2d} "
                  f"table={'high' if high else 'low '} gain={gain/256:.2f}  mismatched pixels: {bad}")
            lib.gbc_destroy(cam)

    print("ALL PASS" if failures == 0 else f"{failures} FAILED")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
