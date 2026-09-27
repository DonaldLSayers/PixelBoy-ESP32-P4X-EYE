"""
Live Game Boy Camera preview from a PC webcam, using the same C image core
as the ESP32-P4X-EYE firmware (gbcam.dll, built by build.ps1).

    python tools/host/gbcam_live.py [--camera 0] [--scale 4]

Keys (mirrors the camera's controls where it can):
    S            style: PIXEL CAM (auto levels) <-> HARDWARE (M64282FP model)
    Z            temporal noise reduction 0 (off) .. 3
    M            switch the arrow keys between brightness and contrast
    Up / Down    adjust brightness or contrast
    P            next palette           D  next dither pattern
    E            next edge mode         R  next edge ratio
    F            flip (mirror) on/off   H  hide/show the info bar
    [ / ]        webcam exposure -1 / +1 EV (webcam auto-exposure is locked off)
    A            webcam auto-exposure on/off (for comparison)
    Space        save a photo to samples/out
    Q / Esc      quit
"""
import argparse
import ctypes
import os
import sys
import time

import cv2
import numpy as np

from webcam import Webcam

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
OUT_DIR = os.path.join(ROOT, "samples", "out")

W, H = 128, 112
BRIGHTNESS_MAX, CONTRAST_MAX = 16, 15
DITHER_COUNT, PALETTE_COUNT, EDGE_RATIOS = 10, 4, 8
EDGE_MODES = ["auto", "none", "horizontal", "2d"]

# cv2.waitKeyEx codes for arrow keys on Windows
KEY_UP, KEY_DOWN = 2490368, 2621440


def load_core():
    path = os.path.join(HERE, "gbcam.dll")
    if not os.path.exists(path):
        sys.exit(f"{path} not found - run tools\\host\\build.ps1 first")
    lib = ctypes.CDLL(path)
    lib.gbc_create.restype = ctypes.c_void_p
    lib.gbc_destroy.argtypes = [ctypes.c_void_p]
    lib.gbc_set.argtypes = [ctypes.c_void_p] + [ctypes.c_int] * 5
    lib.gbc_process_grey.argtypes = [ctypes.c_void_p, ctypes.c_void_p] + [ctypes.c_int] * 4
    lib.gbc_process_grey.restype = ctypes.POINTER(ctypes.c_uint8)
    lib.gbc_gain.argtypes = [ctypes.c_void_p]
    lib.gbc_gain.restype = ctypes.c_double
    lib.gbc_tier.argtypes = [ctypes.c_void_p]
    lib.gbc_ae_error.argtypes = [ctypes.c_void_p]
    for f in (lib.gbc_dither_name, lib.gbc_palette_name):
        f.argtypes = [ctypes.c_int]
        f.restype = ctypes.c_char_p
    lib.gbc_edge_ratio_pct.argtypes = [ctypes.c_int]
    lib.gbc_palette_rgb.argtypes = [ctypes.c_int, ctypes.c_int]
    lib.gbc_set_style.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.gbc_set_denoise.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.gbc_style_name.argtypes = [ctypes.c_int]
    lib.gbc_style_name.restype = ctypes.c_char_p
    for f in (lib.gbc_levels_contrast, lib.gbc_levels_gamma):
        f.argtypes = [ctypes.c_void_p]
        f.restype = ctypes.c_double
    return lib


def palette_lut(lib, p):
    """4x3 uint8 array in BGR order for OpenCV."""
    lut = np.zeros((4, 3), np.uint8)
    for s in range(4):
        v = lib.gbc_palette_rgb(p, s)
        lut[s] = ((v & 0xFF), (v >> 8) & 0xFF, (v >> 16) & 0xFF)
    return lut


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--camera", type=int, default=0, help="webcam index (default 0)")
    ap.add_argument("--scale", type=int, default=4, help="window scale (default 4 = 512x448)")
    args = ap.parse_args()

    lib = load_core()
    cam = lib.gbc_create()

    webcam = Webcam(args.camera)
    if not webcam.ok:
        sys.exit(f"cannot open webcam {args.camera}")
    webcam.lock()
    try:
        run(lib, cam, webcam, args)
    finally:
        webcam.release()  # always hand auto-exposure back to the webcam
        cv2.destroyAllWindows()
        lib.gbc_destroy(cam)


def run(lib, cam, webcam, args):
    s = dict(brightness=8, contrast=7, dither=1, edge=0, ratio=0, palette=0, style=0, denoise=2)
    adjust = "brightness"
    mirror, show_info = True, True
    saved_msg, saved_until = "", 0.0
    fps, t_last = 0.0, time.time()
    win = "GB Camera (webcam)"
    cv2.namedWindow(win, cv2.WINDOW_AUTOSIZE)

    while True:
        grey = webcam.read_grey()
        if grey is None:
            print("webcam read failed")
            break
        grey = np.ascontiguousarray(grey)

        lib.gbc_set(cam, s["brightness"], s["contrast"], s["dither"], s["edge"], s["ratio"])
        lib.gbc_set_style(cam, s["style"])
        lib.gbc_set_denoise(cam, s["denoise"])
        ptr = lib.gbc_process_grey(cam, grey.ctypes.data, grey.shape[1], grey.shape[0], grey.strides[0], int(mirror))
        shades = np.ctypeslib.as_array(ptr, shape=(H * W,)).reshape(H, W).copy()

        lut = palette_lut(lib, s["palette"])
        img = lut[shades]
        big = cv2.resize(img, (W * args.scale, H * args.scale), interpolation=cv2.INTER_NEAREST)

        now = time.time()
        fps = 0.9 * fps + 0.1 * (1.0 / max(now - t_last, 1e-3))
        t_last = now

        if show_info:
            bar = np.zeros((62, big.shape[1], 3), np.uint8)
            mark_b = ">" if adjust == "brightness" else " "
            mark_c = ">" if adjust == "contrast" else " "
            line1 = (f"{mark_b}BRI {s['brightness']:2d}  {mark_c}CON {s['contrast']:2d}  "
                     f"{lib.gbc_palette_name(s['palette']).decode()}  {lib.gbc_dither_name(s['dither']).decode()}  "
                     f"denoise {s['denoise']} [Z]")
            style = lib.gbc_style_name(s["style"]).decode()
            if s["style"] == 0:
                line2 = (f"{style}: auto levels contrast {lib.gbc_levels_contrast(cam):.2f} "
                         f"gamma {lib.gbc_levels_gamma(cam):.2f}   {fps:4.1f} fps   [S] style")
            else:
                line2 = (f"{style}: edge {EDGE_MODES[s['edge']]} {lib.gbc_edge_ratio_pct(s['ratio'])}%  "
                         f"gain {lib.gbc_gain(cam):.2f}  tier {lib.gbc_tier(cam)}  "
                         f"ae {lib.gbc_ae_error(cam):+d}  {fps:4.1f} fps  [S]")
            if now < saved_until:
                line2 = saved_msg
            cv2.putText(bar, line1, (6, 17), cv2.FONT_HERSHEY_SIMPLEX, 0.45, (255, 255, 255), 1, cv2.LINE_AA)
            cv2.putText(bar, line2, (6, 36), cv2.FONT_HERSHEY_SIMPLEX, 0.45, (180, 180, 180), 1, cv2.LINE_AA)
            cv2.putText(bar, webcam.describe() + "   [ ] exposure  A webcam AE",
                        (6, 55), cv2.FONT_HERSHEY_SIMPLEX, 0.45, (140, 180, 140) if webcam.locked else (80, 120, 220),
                        1, cv2.LINE_AA)
            big = np.vstack([big, bar])

        cv2.imshow(win, big)
        key = cv2.waitKeyEx(1)
        if key == -1:
            if cv2.getWindowProperty(win, cv2.WND_PROP_VISIBLE) < 1:
                break
            continue
        ch = chr(key & 0xFF).lower() if key < 256 else ""

        if ch in ("q", "\x1b"):
            break
        elif ch == "m":
            adjust = "contrast" if adjust == "brightness" else "brightness"
        elif ch == "s":
            s["style"] = 1 - s["style"]
        elif ch == "z":
            s["denoise"] = (s["denoise"] + 1) % 4
        elif key in (KEY_UP, KEY_DOWN) or ch in ("+", "=", "-"):
            step = 1 if key == KEY_UP or ch in ("+", "=") else -1
            top = BRIGHTNESS_MAX if adjust == "brightness" else CONTRAST_MAX
            s[adjust] = min(top, max(0, s[adjust] + step))
        elif ch == "p":
            s["palette"] = (s["palette"] + 1) % PALETTE_COUNT
        elif ch == "d":
            s["dither"] = (s["dither"] + 1) % DITHER_COUNT
        elif ch == "e":
            s["edge"] = (s["edge"] + 1) % len(EDGE_MODES)
        elif ch == "r":
            s["ratio"] = (s["ratio"] + 1) % EDGE_RATIOS
        elif ch == "f":
            mirror = not mirror
        elif ch == "h":
            show_info = not show_info
        elif ch == "[":
            webcam.step_exposure(-1)
        elif ch == "]":
            webcam.step_exposure(+1)
        elif ch == "a":
            webcam.toggle_lock()
        elif ch == " ":
            os.makedirs(OUT_DIR, exist_ok=True)
            stamp = time.strftime("%Y%m%d_%H%M%S")
            gb_path = os.path.join(OUT_DIR, f"webcam_{stamp}_gb.png")
            x4_path = os.path.join(OUT_DIR, f"webcam_{stamp}_x4.png")
            cv2.imwrite(gb_path, img)
            cv2.imwrite(x4_path, cv2.resize(img, (W * 4, H * 4), interpolation=cv2.INTER_NEAREST))
            saved_msg, saved_until = f"saved {os.path.basename(x4_path)}", now + 2.0
            print(f"saved {gb_path} and {x4_path}")


if __name__ == "__main__":
    main()
