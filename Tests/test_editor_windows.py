"""Windows acceptance test for the real renderer (run explicitly, not on headless CI).

python Tests/test_editor_windows.py --backend vulkan
python Tests/test_editor_windows.py --backend dx12
Only windows owned by the process started by this test are touched.
"""
import argparse
import ctypes
from ctypes import wintypes
from pathlib import Path
import subprocess
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--backend", choices=("vulkan", "dx12"), required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user32.EnumWindows.argtypes = (callback_type, wintypes.LPARAM)
    user32.GetWindowThreadProcessId.argtypes = (wintypes.HWND, ctypes.POINTER(wintypes.DWORD))
    user32.GetWindowTextW.argtypes = (wintypes.HWND, wintypes.LPWSTR, ctypes.c_int)
    user32.IsWindowVisible.argtypes = (wintypes.HWND,)
    user32.IsIconic.argtypes = (wintypes.HWND,)
    user32.ShowWindow.argtypes = (wintypes.HWND, ctypes.c_int)
    user32.PostMessageW.argtypes = (wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM)
    user32.SetWindowPos.argtypes = (wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int,
                                  ctypes.c_int, ctypes.c_int, wintypes.UINT)
    log_path = root / "build-msvc" / f"editor-{args.backend}-acceptance.log"
    with log_path.open("w", encoding="utf-8") as log:
        process = subprocess.Popen([str(root / "Bin" / "AdvancedVulkanRendering.exe"),
                                    f"--{args.backend}"], cwd=root, stdout=log,
                                   stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            def windows():
                found = {}

                @callback_type
                def visit(hwnd, _):
                    pid = wintypes.DWORD()
                    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
                    if pid.value == process.pid and user32.IsWindowVisible(hwnd):
                        title = ctypes.create_unicode_buffer(512)
                        user32.GetWindowTextW(hwnd, title, len(title))
                        found[title.value] = hwnd
                    return True

                user32.EnumWindows(visit, 0)
                return found

            def wait_for(predicate, label, timeout=30):
                deadline = time.monotonic() + timeout
                while time.monotonic() < deadline:
                    if process.poll() is not None:
                        raise AssertionError(f"renderer exited {process.returncode}: {label}; see {log_path}")
                    value = predicate()
                    if value:
                        return value
                    time.sleep(0.05)
                raise AssertionError(f"timeout: {label}; windows={windows()}; see {log_path}")

            def toggle(scene):
                # F1 virtual key with its scan code, sent only to our own scene window.
                user32.PostMessageW(scene, 0x100, 0x70, 0x003B0001)
                user32.PostMessageW(scene, 0x101, 0x70, 0xC03B0001)

            scene = wait_for(lambda: windows().get("AdvancedVulkanRendering"), "scene startup", 90)
            tool = wait_for(lambda: windows().get("Editor"), "Editor startup", 90)
            assert scene != tool, "Editor must be a separate native window"
            user32.SetWindowPos(tool, None, 1350, 120, 470, 620, 0x0014)
            time.sleep(0.4)
            user32.PostMessageW(tool, 0x10, 0, 0)  # WM_CLOSE
            wait_for(lambda: "Editor" not in windows(), "close Editor without closing scene")
            toggle(scene)
            tool = wait_for(lambda: windows().get("Editor"), "F1 restores closed Editor")
            user32.ShowWindow(scene, 6)  # SW_MINIMIZE
            time.sleep(0.4)
            assert not user32.IsIconic(tool), "scene minimization also minimized Editor"
            user32.PostMessageW(tool, 0x10, 0, 0)
            wait_for(lambda: "Editor" not in windows(), "Editor responds while scene minimized")
            user32.ShowWindow(scene, 9)  # SW_RESTORE
            toggle(scene)
            wait_for(lambda: windows().get("Editor"), "Editor restores after scene minimization")
            toggle(scene)
            wait_for(lambda: "Editor" not in windows(), "F1 hides Editor")
            toggle(scene)
            wait_for(lambda: windows().get("Editor"), "F1 shows Editor")
            user32.PostMessageW(scene, 0x10, 0, 0)
            code = process.wait(timeout=30)
            assert code == 0, f"unclean shutdown: {code}; see {log_path}"
            print(f"PASS {args.backend}: independent windows, resize, close/reopen, minimize, F1, clean exit")
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)


if __name__ == "__main__":
    main()
