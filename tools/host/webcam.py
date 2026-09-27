"""
Webcam wrapper for the PC tools: locks the webcam's own auto-exposure so the
Game Boy Camera auto-exposure in the gbcam core is the only one running.

Tested with a Logitech C270 on Windows (DirectShow):
  - setting CAP_PROP_EXPOSURE switches the camera to manual exposure
    (values are log2 seconds: -4 = 1/16 s, -9 = 1/512 s, one step = 1 EV)
  - setting CAP_PROP_GAIN fixes the analog gain
  - CAP_PROP_AUTO_EXPOSURE = 0.75 hands exposure back to the camera
UVC cameras keep these settings until unplugged, so auto-exposure is always
restored in release().
"""
import cv2

EXPOSURE_MIN, EXPOSURE_MAX = -13, -1
AUTO_ON = 0.75
# Mains-powered LED/fluorescent lights pulse at 100/120 Hz. A short exposure
# catches a different part of each pulse every frame, which shows up as rolling
# horizontal bands (measured ~5.6 levels at 1/512 s) and makes dithered pixels
# dance. At >= 1/32 s several pulses average out, so brightness is set with the
# webcam's gain first and exposure is only shortened if gain alone can't do it.
EXPOSURE_FLICKER_SAFE = -5   # 1/32 s
GAIN_MIN, GAIN_MAX = 0, 255


class Webcam:
    def __init__(self, index=0, width=640, height=480):
        self.cap = cv2.VideoCapture(index, cv2.CAP_DSHOW)
        if not self.cap.isOpened():
            self.cap = cv2.VideoCapture(index)
        self.ok = self.cap.isOpened()
        self.locked = False
        self.exposure = None
        self.gain = None
        if self.ok:
            self.cap.set(cv2.CAP_PROP_FRAME_WIDTH, width)
            self.cap.set(cv2.CAP_PROP_FRAME_HEIGHT, height)

    def read_grey(self):
        ok, frame = self.cap.read()
        return cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY) if ok else None

    def read_rgb(self):
        """As read_grey(), but keeps colour (RGB order, matching gbcam_frame_t)."""
        ok, frame = self.cap.read()
        return cv2.cvtColor(frame, cv2.COLOR_BGR2RGB) if ok else None

    def lock(self, warmup_frames=20, meter=True):
        """Let the webcam auto-expose briefly, then freeze exposure and gain there.

        With meter=True the exposure is then nudged once, in whole EV steps, until
        the raw frame sits mid-scale (no clipped highlights before the Game Boy
        processing). After that it stays fixed; only the gbcam auto-exposure runs."""
        self.cap.set(cv2.CAP_PROP_AUTO_EXPOSURE, AUTO_ON)
        for _ in range(warmup_frames):
            self.cap.read()
        exp = self.cap.get(cv2.CAP_PROP_EXPOSURE)
        self.exposure = int(round(exp)) if EXPOSURE_MIN <= exp <= EXPOSURE_MAX else -5
        self.gain = self.cap.get(cv2.CAP_PROP_GAIN)
        self.cap.set(cv2.CAP_PROP_EXPOSURE, self.exposure)
        if self.gain >= 0:
            self.cap.set(cv2.CAP_PROP_GAIN, self.gain)
        self.locked = True
        if meter:
            self._meter()

    def _mean(self, frames=8):
        grey = None
        for _ in range(frames):  # let the new exposure take effect
            grey = self.read_grey()
        return float(grey.mean()) if grey is not None else 128.0

    def _meter(self, low=80, high=140, max_steps=14):
        """One-off metering: flicker-safe exposure, brightness via gain first."""
        if self.exposure < EXPOSURE_FLICKER_SAFE:
            self.exposure = EXPOSURE_FLICKER_SAFE
            self.cap.set(cv2.CAP_PROP_EXPOSURE, self.exposure)
        if self.gain is None or self.gain < 0:
            self.gain = 128.0
        for _ in range(max_steps):
            m = self._mean()
            if m > high:
                if self.gain > GAIN_MIN:
                    self.gain = max(GAIN_MIN, self.gain * 0.6 - 4)
                    self.cap.set(cv2.CAP_PROP_GAIN, self.gain)
                elif self.exposure > EXPOSURE_MIN:
                    self.exposure -= 1  # very bright: flicker-safe exposure isn't possible
                    self.cap.set(cv2.CAP_PROP_EXPOSURE, self.exposure)
                else:
                    break
            elif m < low:
                if self.gain < GAIN_MAX:
                    self.gain = min(GAIN_MAX, self.gain * 1.5 + 8)
                    self.cap.set(cv2.CAP_PROP_GAIN, self.gain)
                elif self.exposure < EXPOSURE_MAX:
                    self.exposure += 1
                    self.cap.set(cv2.CAP_PROP_EXPOSURE, self.exposure)
                else:
                    break
            else:
                break

    def unlock(self):
        """Give exposure back to the webcam."""
        if self.ok:
            self.cap.set(cv2.CAP_PROP_AUTO_EXPOSURE, AUTO_ON)
        self.locked = False

    def toggle_lock(self):
        if self.locked:
            self.unlock()
        else:
            self.lock(warmup_frames=15)

    def step_exposure(self, steps):
        """Change the locked exposure by whole EV steps (locks if needed)."""
        if not self.locked:
            self.lock(warmup_frames=15, meter=False)
        self.exposure = max(EXPOSURE_MIN, min(EXPOSURE_MAX, self.exposure + steps))
        self.cap.set(cv2.CAP_PROP_EXPOSURE, self.exposure)

    def describe(self):
        if not self.locked:
            return "webcam AE on"
        return f"webcam exp {self.exposure} (1/{2 ** -self.exposure}s) gain {self.gain:.0f} locked"

    def release(self):
        if self.ok:
            self.unlock()
            self.cap.release()
            self.ok = False
