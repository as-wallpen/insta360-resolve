#!/usr/bin/env python3
"""Check the plugin's CPU stitch (Windows port) against the Python reference, like compare_metal.py.

Runs both on synthetic textured lens frames with the built-in X5 calibration, for the stitched
360 view and several reframed views, in both buffer orientations, with the same limits.

usage: compare_kernel.py [test_plugin binary]   (default: plugin/build-native/test_plugin)
Needs numpy and opencv-python-headless.
"""
import subprocess, sys, tempfile
from pathlib import Path
import cv2, numpy as np
TOOLS = Path(__file__).resolve().parent
BIN = sys.argv[1] if len(sys.argv) > 1 else str(TOOLS.parent / "plugin/build-native/test_plugin")
sys.path.insert(0, str(TOOLS))
from insv_trailer import SENSOR_W
from stitch_poc import camera_rays, rot_x, rot_y, rot_z, stitch
raw = "".join(l.split('"')[1] for l in (TOOLS.parent / "plugin/src/Calibration.cpp").read_text().split("kNominalX5Calibration =")[1].split(";")[0].splitlines() if '"' in l)
vals = [float(x) for x in raw.split("_")]
lenses = []
for i in range(int(vals[0])):
    s = 1 + i * 27
    lenses.append({"xi": vals[s], "fx": vals[s + 1], "fy": vals[s + 2], "cx": vals[s + 3] - i * SENSOR_W, "cy": vals[s + 4],
                   "yaw": vals[s + 5], "pitch": vals[s + 6], "roll": vals[s + 7], "t": vals[s + 8:s + 11], "d": vals[s + 11:s + 24]})
lw = 704
rng = np.random.default_rng(3)
def texture(seed):
    r = np.random.default_rng(seed); img = np.zeros((lw, lw, 3), np.float32)
    for size in (4, 16, 64):
        small = r.random((lw // size + 2, lw // size + 2, 3)).astype(np.float32)
        img += cv2.resize(small, (lw + 2 * size, lw + 2 * size), interpolation=cv2.INTER_CUBIC)[size:size + lw, size:size + lw] * (60 if size > 4 else 40)
    return np.clip(img + 20, 0, 255).astype(np.uint8)
frames = [texture(1), texture(2)]
w, h = 2 * lw, lw
cases = [("stitched 360", [], None)]
for pan, tilt, roll, fov, dist, aspect in ((0, 0, 0, 100, 0.0, 16 / 9), (135, -20, 10, 80, 0.0, 16 / 9), (-90, 35, -25, 120, 0.0, 9 / 16),
                                           (60, 10, 0, 150, 0.5, 16 / 9), (40, -90, 0, 300, 1.0, 1.0)):
    cases.append((f"reframe {pan},{tilt},{roll} fov {fov} d {dist}", [str(x) for x in (pan, tilt, roll, fov, dist, aspect)],
                  camera_rays(w, h, pan, tilt, roll, fov, dist, aspect)))
stab = rot_y(25.0) @ rot_x(-12.0) @ rot_z(8.0)
view = camera_rays(w, h, 30, 5, 0, 100, 0.25, 16 / 9)
cases.append(("reframe, stabilised", [str(x) for x in (30, 5, 0, 100, 0.25, 16 / 9)] + [",".join(f"{v:.9f}" for v in stab.ravel())],
              (stab @ view.reshape(3, -1)).reshape(view.shape)))
rgba = np.concatenate([np.concatenate(frames, axis=1)[..., ::-1].astype(np.float32) / 255.0, np.ones((lw, 2 * lw, 1), np.float32)], axis=2)
failed = False
with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    for label, extra, rays in cases:
        reference, _ = stitch(frames, lenses, w, rays=rays)
        for flip in (0, 1):
            np.ascontiguousarray(rgba[::-1] if flip else rgba).tofile(tmp / "in.raw")
            subprocess.run([BIN, "--stitch", str(tmp / "in.raw"), str(w), str(h), "-", str(tmp / "out.raw"), str(flip)] + extra, check=True)
            out = np.fromfile(tmp / "out.raw", np.float32).reshape(h, w, 4)
            out = out[::-1] if flip else out
            cpu = np.clip(out[..., 2::-1] * 255.0, 0, 255)
            diff = np.abs(cpu - reference.astype(np.float32))
            mean, p999 = float(diff.mean()), float(np.percentile(diff, 99.9))
            ok = mean < 0.5 and p999 < 6.0
            failed |= not ok
            print(f"{'ok  ' if ok else 'FAIL'}  {label}, flipY={flip}: mean {mean:.3f}, p99.9 {p999:.2f}, max {diff.max():.1f}")
sys.exit(1 if failed else 0)
