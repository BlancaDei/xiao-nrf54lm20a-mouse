"""Minimal Windows raw-input mouse counter for CPI calibration.

Logs cumulative raw dx/dy. Press SPACE at each ruler endpoint; each SPACE
writes an ``M,sum_x,sum_y,time`` line, so the counts between two markers are
the counts for that physical move. Works while another window has focus
(RIDEV_INPUTSINK).
"""

import argparse
import ctypes
import ctypes.wintypes as w
import time

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

WM_INPUT = 0x00FF
RID_INPUT = 0x10000003
RIM_TYPEMOUSE = 0
RIDEV_INPUTSINK = 0x00000100
PM_REMOVE = 0x0001
VK_SPACE = 0x20
HWND_MESSAGE = -3


class RAWINPUTDEVICE(ctypes.Structure):
    _fields_ = [
        ("usUsagePage", w.USHORT),
        ("usUsage", w.USHORT),
        ("dwFlags", w.DWORD),
        ("hwndTarget", w.HWND),
    ]


class RAWINPUTHEADER(ctypes.Structure):
    _fields_ = [
        ("dwType", w.DWORD),
        ("dwSize", w.DWORD),
        ("hDevice", w.HANDLE),
        ("wParam", w.WPARAM),
    ]


class RAWMOUSE(ctypes.Structure):
    _fields_ = [
        ("usFlags", w.USHORT),
        ("usButtonFlags", w.USHORT),
        ("usButtonData", w.USHORT),
        ("ulRawButtons", w.ULONG),
        ("lLastX", w.LONG),
        ("lLastY", w.LONG),
        ("ulExtraInformation", w.ULONG),
    ]


class RAWINPUT(ctypes.Structure):
    _fields_ = [("header", RAWINPUTHEADER), ("mouse", RAWMOUSE)]


class WNDCLASSW(ctypes.Structure):
    _fields_ = [
        ("style", w.UINT),
        ("lpfnWndProc", ctypes.c_void_p),
        ("cbClsExtra", ctypes.c_int),
        ("cbWndExtra", ctypes.c_int),
        ("hInstance", w.HINSTANCE),
        ("hIcon", w.HANDLE),
        ("hCursor", w.HANDLE),
        ("hbrBackground", w.HANDLE),
        ("lpszMenuName", w.LPCWSTR),
        ("lpszClassName", w.LPCWSTR),
    ]


WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_ssize_t, w.HWND, w.UINT, w.WPARAM, w.LPARAM)
user32.DefWindowProcW.restype = ctypes.c_ssize_t
user32.DefWindowProcW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
user32.GetRawInputData.restype = w.UINT
user32.GetRawInputData.argtypes = [w.HANDLE, w.UINT, ctypes.c_void_p, ctypes.POINTER(w.DWORD), w.UINT]
user32.RegisterRawInputDevices.restype = w.BOOL
user32.RegisterRawInputDevices.argtypes = [ctypes.POINTER(RAWINPUTDEVICE), w.UINT, w.UINT]
user32.CreateWindowExW.restype = w.HWND
user32.CreateWindowExW.argtypes = [w.DWORD, w.LPCWSTR, w.LPCWSTR, w.DWORD, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, w.HWND, w.HMENU, w.HINSTANCE, w.LPVOID]
user32.RegisterClassW.restype = w.ATOM
user32.RegisterClassW.argtypes = [ctypes.POINTER(WNDCLASSW)]
user32.PeekMessageW.restype = w.BOOL
user32.PeekMessageW.argtypes = [ctypes.POINTER(w.MSG), w.HWND, w.UINT, w.UINT, w.UINT]
user32.TranslateMessage.restype = w.BOOL
user32.TranslateMessage.argtypes = [ctypes.POINTER(w.MSG)]
user32.DispatchMessageW.restype = ctypes.c_ssize_t
user32.DispatchMessageW.argtypes = [ctypes.POINTER(w.MSG)]
user32.GetAsyncKeyState.restype = ctypes.c_short
user32.GetAsyncKeyState.argtypes = [ctypes.c_int]
kernel32.GetModuleHandleW.restype = w.HINSTANCE
kernel32.GetModuleHandleW.argtypes = [w.LPCWSTR]

sum_x = 0
sum_y = 0
out_file = None


def wndproc(hwnd, msg, wparam, lparam):
    global sum_x, sum_y
    if msg == WM_INPUT:
        size = w.DWORD(0)
        user32.GetRawInputData(lparam, RID_INPUT, None, ctypes.byref(size), ctypes.sizeof(RAWINPUTHEADER))
        if size.value:
            buf = (ctypes.c_byte * size.value)()
            if user32.GetRawInputData(lparam, RID_INPUT, buf, ctypes.byref(size), ctypes.sizeof(RAWINPUTHEADER)) == size.value:
                ri = ctypes.cast(buf, ctypes.POINTER(RAWINPUT)).contents
                if ri.header.dwType == RIM_TYPEMOUSE:
                    dx, dy = ri.mouse.lLastX, ri.mouse.lLastY
                    if dx or dy:
                        sum_x += dx
                        sum_y += dy
                        if out_file is not None:
                            out_file.write(f"D,{dx},{dy},{time.time():.3f}\n")
        return 0
    return user32.DefWindowProcW(hwnd, msg, wparam, lparam)


def main():
    global out_file
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=180.0)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    class_name = "TraeRawInputCPI"
    wc = WNDCLASSW()
    wc.style = 0
    wc.lpfnWndProc = ctypes.cast(WNDPROC(wndproc), ctypes.c_void_p)
    wc.cbClsExtra = 0
    wc.cbWndExtra = 0
    wc.hInstance = kernel32.GetModuleHandleW(None)
    wc.hIcon = None
    wc.hCursor = None
    wc.hbrBackground = None
    wc.lpszMenuName = None
    wc.lpszClassName = class_name
    if not user32.RegisterClassW(ctypes.byref(wc)):
        raise ctypes.WinError(ctypes.get_last_error())

    hwnd = user32.CreateWindowExW(0, class_name, "raw", 0, 0, 0, 0, 0, w.HWND(HWND_MESSAGE), None, wc.hInstance, None)
    if not hwnd:
        raise ctypes.WinError(ctypes.get_last_error())

    rid = RAWINPUTDEVICE(1, 2, RIDEV_INPUTSINK, hwnd)
    if not user32.RegisterRawInputDevices(ctypes.byref(rid), 1, ctypes.sizeof(RAWINPUTDEVICE)):
        raise ctypes.WinError(ctypes.get_last_error())

    msg = w.MSG()
    end = time.time() + args.seconds
    prev_space = False
    with open(args.out, "w", encoding="utf-8") as f:
        out_file = f
        f.write("# raw input mouse counts; M rows = SPACE markers\n")
        f.flush()
        while time.time() < end:
            while user32.PeekMessageW(ctypes.byref(msg), hwnd, 0, 0, PM_REMOVE):
                user32.TranslateMessage(ctypes.byref(msg))
                user32.DispatchMessageW(ctypes.byref(msg))
            space = bool(user32.GetAsyncKeyState(VK_SPACE) & 0x8000)
            if space and not prev_space:
                f.write(f"M,{sum_x},{sum_y},{time.time():.3f}\n")
                f.flush()
            prev_space = space
            time.sleep(0.001)
        out_file = None


if __name__ == "__main__":
    main()
