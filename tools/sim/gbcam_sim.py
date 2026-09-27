"""
GB Camera simulator: runs the real firmware app code (gbcam_sim.dll) on the PC,
with the webcam as the camera. The screen shows exactly what the 240x240 LCD on
the ESP32-P4X-EYE would show.

    python tools/sim/gbcam_sim.py [--camera 0] [--scale 3] [--no-sd] [--test-pattern]

Controls
    Space / Enter / E / left-click knob   Encoder press = SHUTTER (take photo)
    Left/Right, Up/Down, mouse wheel      Turn the encoder (adjust / menu navigate)
    1 / click MENU          Menu button: open/close the menu
    L / right-click MENU    Menu long press: gallery (GB Camera photos only)
    M / 2                   Mode button: switch what the encoder adjusts
    N / right-click MODE    Mode long press: style, PIXEL CAM <-> HARDWARE (GB Camera only)
    3 / click CAM MODE      Cam mode button: GB CAMERA -> DITHER CAM -> NORMAL CAM
    F   mirror webcam      O   open the simulated SD card folder
    [ / ]   webcam exposure -1 / +1 EV      A   webcam auto-exposure on/off
    (the webcam's own auto-exposure is locked off, so only the Game Boy Camera
     auto-exposure in the firmware runs)
    P / click ACTUAL SIZE (debug)  toggle a second window at the real device's
                                    true physical screen size (see --physical-ppi)
Q / Esc quits. The on-screen buttons can also be clicked with the mouse.

GB Camera's viewfinder scale (2x cropped vs 1:1 full sensor image) is a real
menu setting now, not a sim-only toggle: Menu -> SCALE, cycles with shutter
like any other row.
"""
import argparse
import ctypes
import math
import os
import sys
import time

import cv2
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools", "host"))
from webcam import Webcam  # noqa: E402
DATA_DIR = os.path.join(ROOT, "sim_data")

LCD = 240
PANEL_DIAGONAL_IN = 1.54  # PLAN.md: 1.54" 240x240 ST7789
PANEL_SIDE_IN = PANEL_DIAGONAL_IN / math.sqrt(2)  # ~1.09" - it's square, not the full diagonal
PRESS, CLICK, LONG, ROTATE = 0, 1, 2, 3
MENU, MODE, CAMMODE, SHUTTER = 0, 1, 2, 3   # SHUTTER = encoder push
ENCODER = SHUTTER

# cv2.waitKeyEx codes on Windows
KEY_LEFT, KEY_UP, KEY_RIGHT, KEY_DOWN = 2424832, 2490368, 2555904, 2621440

# Sim-only debug tool - not a real device control, so it's kept out of
# button/MENU/MODE/CAMMODE/SHUTTER's id space (those get sent to the firmware
# via sim_input; this never does) and drawn in a visually distinct colour.
# (GB Camera's viewfinder scale - 2x cropped vs 1:1 - is a REAL menu setting
# now, ROW_VF_SCALE in app.c: Menu -> SCALE, not a sim-only toggle.)
PHYS_TOGGLE = "PHYS_TOGGLE"


def load_sim():
    path = os.path.join(HERE, "gbcam_sim.dll")
    if not os.path.exists(path):
        sys.exit(f"{path} not found - run tools\\sim\\build.ps1 first")
    lib = ctypes.CDLL(path)
    lib.sim_init.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_int]
    lib.sim_push_frame.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int]
    lib.sim_push_frame_rgb.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int]
    lib.sim_input.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int]
    lib.sim_framebuffer.restype = ctypes.POINTER(ctypes.c_uint16)
    lib.sim_sd_root.restype = ctypes.c_char_p
    return lib


def lcd_to_bgr(lib):
    """Read the LCD framebuffer (big-endian RGB565) as a BGR image."""
    raw = np.ctypeslib.as_array(lib.sim_framebuffer(), shape=(LCD * LCD,)).copy()
    v = raw.byteswap().reshape(LCD, LCD).astype(np.uint32)
    r = ((v >> 11) & 0x1F) * 255 // 31
    g = ((v >> 5) & 0x3F) * 255 // 63
    b = (v & 0x1F) * 255 // 31
    return np.dstack([b, g, r]).astype(np.uint8)


class Device:
    """Draws the camera body and maps mouse clicks to controls."""

    def __init__(self, scale):
        self.scale = scale
        self.lcd_size = LCD * scale
        m = 40
        self.lcd_x, self.lcd_y = m + 20, m + 20
        panel_x = self.lcd_x + self.lcd_size + 60
        self.w = panel_x + 230
        self.h = self.lcd_y + self.lcd_size + m + 40
        bw, bh = 190, 56
        self.buttons = {
            MENU: ((panel_x, self.lcd_y + 10, bw, bh), "MENU", "1  hold: L"),
            MODE: ((panel_x, self.lcd_y + 90, bw, bh), "MODE", "M  hold: N"),
            CAMMODE: ((panel_x, self.lcd_y + 170, bw, bh), "CAM MODE", "3"),
        }
        self.knob = (panel_x + bw // 2, self.lcd_y + 330, 62)
        self.knob_angle = 0.0
        self.flash = {}  # control -> time until highlighted

        # Sim-only debug tool, in the gap between CAM MODE and the knob -
        # visually separate (different colour, own header) from the real
        # device controls above them.
        dby = self.lcd_y + 170 + bh + 22
        self.debug_buttons = {
            PHYS_TOGGLE: (panel_x, dby, bw, 40),
        }

    def hit(self, x, y):
        for bid, ((bx, by, bw, bh), _, _) in self.buttons.items():
            if bx <= x < bx + bw and by <= y < by + bh:
                return bid
        for bid, (bx, by, bw, bh) in self.debug_buttons.items():
            if bx <= x < bx + bw and by <= y < by + bh:
                return bid
        kx, ky, kr = self.knob
        if (x - kx) ** 2 + (y - ky) ** 2 <= kr * kr:
            return ENCODER
        return None

    def highlight(self, control):
        self.flash[control] = time.time() + 0.18

    def draw(self, lcd_bgr, status, phys_visible=False):
        img = np.full((self.h, self.w, 3), (38, 38, 42), np.uint8)
        cv2.rectangle(img, (12, 12), (self.w - 12, self.h - 12), (70, 70, 76), 2, cv2.LINE_AA)
        x0, y0 = self.lcd_x, self.lcd_y
        cv2.rectangle(img, (x0 - 14, y0 - 14), (x0 + self.lcd_size + 14, y0 + self.lcd_size + 14), (15, 15, 15), -1)
        big = cv2.resize(lcd_bgr, (self.lcd_size, self.lcd_size), interpolation=cv2.INTER_NEAREST)
        img[y0:y0 + self.lcd_size, x0:x0 + self.lcd_size] = big

        now = time.time()
        for bid, ((bx, by, bw, bh), label, key) in self.buttons.items():
            lit = self.flash.get(bid, 0) > now
            cv2.rectangle(img, (bx, by), (bx + bw, by + bh), (90, 160, 90) if lit else (62, 62, 68), -1, cv2.LINE_AA)
            cv2.rectangle(img, (bx, by), (bx + bw, by + bh), (120, 120, 128), 1, cv2.LINE_AA)
            cv2.putText(img, label, (bx + 14, by + 26), cv2.FONT_HERSHEY_SIMPLEX, 0.62, (235, 235, 235), 1, cv2.LINE_AA)
            cv2.putText(img, f"[{key}]", (bx + 14, by + 46), cv2.FONT_HERSHEY_SIMPLEX, 0.45, (170, 170, 170), 1, cv2.LINE_AA)

        dbx0, dby0 = next(iter(self.debug_buttons.values()))[:2]
        cv2.putText(img, "DEBUG (sim only)", (dbx0, dby0 - 8), cv2.FONT_HERSHEY_SIMPLEX, 0.4, (140, 160, 200), 1, cv2.LINE_AA)
        debug_labels = {
            PHYS_TOGGLE: ("ACTUAL SIZE: ON" if phys_visible else "ACTUAL SIZE: OFF", "P"),
        }
        for bid, (bx, by, bw, bh) in self.debug_buttons.items():
            lit = self.flash.get(bid, 0) > now
            label, key = debug_labels[bid]
            cv2.rectangle(img, (bx, by), (bx + bw, by + bh), (110, 150, 200) if lit else (55, 70, 95), -1, cv2.LINE_AA)
            cv2.rectangle(img, (bx, by), (bx + bw, by + bh), (130, 150, 180), 1, cv2.LINE_AA)
            cv2.putText(img, label, (bx + 6, by + 17), cv2.FONT_HERSHEY_SIMPLEX, 0.4, (230, 235, 245), 1, cv2.LINE_AA)
            cv2.putText(img, f"[{key}]", (bx + 6, by + 33), cv2.FONT_HERSHEY_SIMPLEX, 0.38, (170, 180, 200), 1, cv2.LINE_AA)

        kx, ky, kr = self.knob
        lit = self.flash.get(ENCODER, 0) > now
        cv2.circle(img, (kx, ky), kr, (90, 160, 90) if lit else (62, 62, 68), -1, cv2.LINE_AA)
        cv2.circle(img, (kx, ky), kr, (120, 120, 128), 2, cv2.LINE_AA)
        for i in range(24):
            a = self.knob_angle + i * math.pi / 12
            cv2.line(img, (int(kx + (kr - 8) * math.cos(a)), int(ky + (kr - 8) * math.sin(a))),
                     (int(kx + kr * math.cos(a)), int(ky + kr * math.sin(a))), (110, 110, 118), 2, cv2.LINE_AA)
        a = self.knob_angle
        cv2.line(img, (kx, ky), (int(kx + (kr - 14) * math.cos(a)), int(ky + (kr - 14) * math.sin(a))),
                 (220, 220, 220), 3, cv2.LINE_AA)
        hints = ["ENCODER = SHUTTER", "wheel / arrows: turn", "click / Space: take photo"]
        for i, t in enumerate(hints):
            cv2.putText(img, t, (kx - 95, ky + kr + 26 + i * 20), cv2.FONT_HERSHEY_SIMPLEX,
                        0.5 if i == 0 else 0.42, (220, 220, 220) if i == 0 else (160, 160, 160), 1, cv2.LINE_AA)

        cv2.putText(img, status, (x0 - 10, self.h - 22), cv2.FONT_HERSHEY_SIMPLEX, 0.45, (150, 150, 150), 1, cv2.LINE_AA)
        cv2.putText(img, "ESP32-P4X-EYE  (simulated)", (x0 - 10, 34), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (150, 150, 150), 1, cv2.LINE_AA)
        return img


def test_pattern(t):
    """Moving pattern for running without a webcam."""
    h, w = 480, 640
    y, x = np.mgrid[0:h, 0:w]
    v = (x / w * 255).astype(np.float32)
    cx, cy = w * (0.5 + 0.3 * math.sin(t)), h * 0.5
    v[(x - cx) ** 2 + (y - cy) ** 2 < 90 ** 2] = 240
    v[y > h * 0.8] = np.where(((x // 16 + y // 16) & 1) == 1, 230, 25)[y > h * 0.8]
    return v.astype(np.uint8)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--camera", type=int, default=0, help="webcam index (default 0)")
    ap.add_argument("--scale", type=int, default=3, help="LCD scale (default 3 = 720x720)")
    ap.add_argument("--no-sd", action="store_true", help="simulate a missing SD card")
    ap.add_argument("--camera-error", action="store_true", help="simulate a camera init failure")
    ap.add_argument("--test-pattern", action="store_true", help="use a moving test pattern instead of the webcam")
    ap.add_argument("--physical-ppi", type=float, default=109.0,
                    help="pixels-per-inch of YOUR monitor, for the ACTUAL SIZE debug button/[P] key: it "
                         "opens a second window sized to the real device's actual physical screen (1.54in "
                         "ST7789, PLAN.md). Find yours: sqrt(width_px^2 + height_px^2) / diagonal_inches - "
                         "default 109 is a 27in 1440p monitor. Warning: that comes out tiny (~120px at 109 "
                         "PPI) since the real screen is only ~1.1in square - use --scale for actual "
                         "debugging, this is just a gut check on real-world size.")
    args = ap.parse_args()

    physical_px = max(1, round(args.physical_ppi * PANEL_SIDE_IN))
    print(f"[P]/ACTUAL SIZE button -> {physical_px}x{physical_px}px window "
          f"(device screen is ~{PANEL_SIDE_IN:.2f}in square at ~{LCD / PANEL_SIDE_IN:.0f} PPI, "
          f"assuming your monitor is {args.physical_ppi:.0f} PPI)")

    lib = load_sim()
    os.makedirs(DATA_DIR, exist_ok=True)
    err = lib.sim_init(DATA_DIR.replace("\\", "/").encode(), 0 if args.no_sd else 1, 1 if args.camera_error else 0)
    print(f"sim_init -> {err}   SD card folder: {lib.sim_sd_root().decode()}")

    webcam = None
    if not args.test_pattern:
        webcam = Webcam(args.camera)
        if webcam.ok:
            webcam.lock()
        else:
            print(f"cannot open webcam {args.camera}; using the test pattern")
            webcam = None

    dev = Device(args.scale)
    win = "GB Camera simulator"
    cv2.namedWindow(win, cv2.WINDOW_AUTOSIZE)
    physical_win = "Actual size"
    state = {"phys_visible": False}
    mirror = True
    pending_rot = [0]

    def button(bid, long_press=False):
        dev.highlight(bid)
        lib.sim_input(PRESS, bid, 0)
        lib.sim_input(LONG if long_press else CLICK, bid, 0)

    def toggle_phys_window():
        dev.highlight(PHYS_TOGGLE)
        state["phys_visible"] = not state["phys_visible"]
        if state["phys_visible"]:
            cv2.namedWindow(physical_win, cv2.WINDOW_AUTOSIZE)
        else:
            cv2.destroyWindow(physical_win)

    def rotate(n):
        n = -n  # matches the real encoder's direction - see app_input.c's detents negation
        dev.knob_angle += n * math.pi / 12
        dev.highlight(ENCODER)
        lib.sim_input(ROTATE, 4, n)

    def on_mouse(event, x, y, flags, _):
        if event == cv2.EVENT_MOUSEWHEEL:
            # flags' high 16 bits hold the signed wheel delta; cv2.getMouseWheelDelta()
            # is missing from some OpenCV python builds (e.g. 5.0.0), so decode it directly.
            delta = (flags >> 16) & 0xFFFF
            if delta >= 0x8000:
                delta -= 0x10000
            pending_rot[0] += 1 if delta > 0 else -1
        elif event == cv2.EVENT_LBUTTONDOWN:
            hit = dev.hit(x, y)
            if hit == PHYS_TOGGLE:
                toggle_phys_window()
            elif hit is not None:
                button(hit)
        elif event == cv2.EVENT_RBUTTONDOWN and dev.hit(x, y) in (MENU, MODE):
            button(dev.hit(x, y), long_press=True)

    cv2.setMouseCallback(win, on_mouse)
    fps, t_last, t0 = 0.0, time.time(), time.time()

    try:
        while True:
            if webcam is not None:
                rgb = webcam.read_rgb()
                if rgb is None:
                    print("webcam read failed")
                    break
            else:
                rgb = np.dstack([test_pattern(time.time() - t0)] * 3)  # grey, but in RGB888 form
                time.sleep(1 / 30)
            if mirror:
                rgb = cv2.flip(rgb, 1)
            rgb = np.ascontiguousarray(rgb)
            # Pushed as RGB888 so all three camera modes (including Dither Cam
            # and Normal Cam's colour palettes) see a real image, matching what
            # the ESP32-P4X-EYE's own camera format negotiation prefers.
            lib.sim_push_frame_rgb(rgb.ctypes.data, rgb.shape[1], rgb.shape[0], rgb.strides[0])

            if pending_rot[0]:
                rotate(pending_rot[0])
                pending_rot[0] = 0

            lib.sim_step()

            now = time.time()
            fps = 0.9 * fps + 0.1 / max(now - t_last, 1e-3)
            t_last = now
            cam_state = webcam.describe() if webcam else "test pattern"
            status = (f"{fps:4.1f} fps   {cam_state}   {'mirrored' if mirror else 'normal'}   "
                      f"SD: {'none' if args.no_sd else 'sim_data/sdcard'}   [ ] exposure  A webcam AE  F mirror  O SD  Q quit")
            lcd_bgr = lcd_to_bgr(lib)
            cv2.imshow(win, dev.draw(lcd_bgr, status, state["phys_visible"]))
            if state["phys_visible"]:
                cv2.imshow(physical_win, cv2.resize(lcd_bgr, (physical_px, physical_px), interpolation=cv2.INTER_NEAREST))

            key = cv2.waitKeyEx(1)
            if key == -1:
                if cv2.getWindowProperty(win, cv2.WND_PROP_VISIBLE) < 1:
                    break
                continue
            ch = chr(key & 0xFF).lower() if key < 256 else ""
            if ch in ("q", "\x1b"):
                break
            elif ch in (" ", "\r", "e"):
                button(SHUTTER)
            elif ch == "1":
                button(MENU)
            elif ch == "l":
                button(MENU, long_press=True)
            elif ch in ("m", "2"):
                button(MODE)
            elif ch == "n":
                button(MODE, long_press=True)
            elif ch in ("3", "c"):
                button(CAMMODE)
            elif key in (KEY_RIGHT, KEY_UP) or ch in (".", "+", "="):
                rotate(1)
            elif key in (KEY_LEFT, KEY_DOWN) or ch in (",", "-"):
                rotate(-1)
            elif ch == "f":
                mirror = not mirror
            elif ch == "o":
                os.startfile(lib.sim_sd_root().decode().replace("/", "\\"))
            elif ch == "p":
                toggle_phys_window()
            elif webcam is not None and ch == "[":
                webcam.step_exposure(-1)
            elif webcam is not None and ch == "]":
                webcam.step_exposure(+1)
            elif webcam is not None and ch == "a":
                webcam.toggle_lock()

    finally:
        if webcam is not None:
            webcam.release()  # always hand auto-exposure back to the webcam
        cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
