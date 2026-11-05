"""Rotate the connected Android display through all four orientations.

Run against an installed debug APK. Saves screenshots/logs, verifies the app
stays in the same process and rebuilds its render targets, and restores the
device's original rotation settings even on failure. Physical sensor testing
is still needed: the display override exercises the resize/presentation path.
"""
import argparse
from pathlib import Path
import re
import subprocess
import time


PACKAGE = "com.vulkan.advancedrendering"
ACTIVITY = PACKAGE + "/org.libsdl.app.SDLActivity"


def adb(*args):
    return subprocess.check_output(["adb", *args], text=True).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    adb("shell", "input", "keyevent", "KEYCODE_WAKEUP")
    adb("shell", "wm", "dismiss-keyguard")
    adb("shell", "am", "start", "-n", ACTIVITY)
    time.sleep(15)
    pid = adb("shell", "pidof", PACKAGE)
    assert pid, "Application did not start"
    activity = adb("shell", "dumpsys", "activity", "activities")
    records = activity.split("ActivityRecord{")
    assert any(PACKAGE in record and
               "requestedOrientation=SCREEN_ORIENTATION_FULL_SENSOR" in record
               for record in records), "Application is still orientation-locked"
    rotation = adb("shell", "wm", "user-rotation").split()
    fixed = adb("shell", "wm", "fixed-to-user-rotation")
    try:
        adb("shell", "wm", "fixed-to-user-rotation", "enabled")
        # Make the first checked orientation a real change even if startup was
        # already portrait (which correctly needs no swapchain recreation).
        adb("shell", "wm", "user-rotation", "lock", "3")
        time.sleep(5)
        # Repeat portrait/landscape after visiting both reverse orientations.
        for index, turn in enumerate([0, 1, 2, 3, 0, 1]):
            adb("shell", "input", "keyevent", "KEYCODE_WAKEUP")
            adb("shell", "wm", "user-rotation", "lock", str(turn))
            time.sleep(5)
            assert adb("shell", "pidof", PACKAGE) == pid, "Application crashed/restarted"
            stem = args.output / f"rotation-{index}-{turn}"
            png = subprocess.check_output(["adb", "exec-out", "screencap", "-p"])
            stem.with_suffix(".png").write_bytes(png)
            width = int.from_bytes(png[16:20], "big")
            height = int.from_bytes(png[20:24], "big")
            assert (width > height) == (turn % 2 == 1), (turn, width, height)
            log = adb("logcat", "-d", "--pid=" + pid, "-s", "Bistro", "SDL")
            stem.with_suffix(".log").write_text(log, encoding="utf-8")
            extents = re.findall(r"Swapchain resources recreated: (\d+)x(\d+)", log)
            assert extents, "Renderer did not report a resize"
            w, h = map(int, extents[-1])
            assert (w > h) == (turn % 2 == 1), (turn, w, h)
            for pass_name in ["Hi-Z", "SAO"]:
                sizes = re.findall(rf"{pass_name} resources created: (\d+)x(\d+)", log)
                assert sizes and tuple(map(int, sizes[-1])) == (w, h), pass_name
            volumes = re.findall(r"ScatteringVolume: (\d+)x(\d+)x64", log)
            assert volumes and tuple(map(int, volumes[-1])) == ((w + 7) // 8, (h + 7) // 8)
            assert not re.search(r"failed to (acquire|present|wait)|VK_ERROR|Fatal signal", log, re.I), "Render error; inspect log"
            print(f"rotation={turn}: display={width}x{height}, render={w}x{h}, PID={pid}", flush=True)
    finally:
        adb("shell", "wm", "user-rotation", *rotation)
        adb("shell", "wm", "fixed-to-user-rotation", fixed)


if __name__ == "__main__":
    main()
