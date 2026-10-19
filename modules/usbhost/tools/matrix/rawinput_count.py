"""Count raw mouse and keyboard reports from one USB device on Windows.

    python rawinput_count.py VID_303A SECONDS

Registers for raw input from every mouse and keyboard (a message-only
window with RIDEV_INPUTSINK, so it counts without focus) and counts the
WM_INPUT messages whose device name contains the given text. Prints a JSON
line: mouse reports, keyboard reports, and the summed mouse motion.
"""
import ctypes
import json
import sys
import time
from ctypes import wintypes as w

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32")

WM_INPUT = 0x00FF
RIDEV_INPUTSINK = 0x00000100
RID_INPUT = 0x10000003
RIDI_DEVICENAME = 0x20000007
RIM_TYPEMOUSE, RIM_TYPEKEYBOARD = 0, 1
HWND_MESSAGE = w.HWND(-3)
LRESULT = ctypes.c_ssize_t
WNDPROC = ctypes.WINFUNCTYPE(LRESULT, w.HWND, w.UINT, w.WPARAM, w.LPARAM)


class RAWINPUTDEVICE(ctypes.Structure):
    _fields_ = [("usUsagePage", w.USHORT), ("usUsage", w.USHORT), ("dwFlags", w.DWORD), ("hwndTarget", w.HWND)]


class RAWINPUTHEADER(ctypes.Structure):
    _fields_ = [("dwType", w.DWORD), ("dwSize", w.DWORD), ("hDevice", w.HANDLE), ("wParam", w.WPARAM)]


class RAWMOUSE(ctypes.Structure):
    _fields_ = [("usFlags", w.USHORT), ("_pad", w.USHORT), ("usButtonFlags", w.USHORT), ("usButtonData", w.USHORT),
                ("ulRawButtons", w.ULONG), ("lLastX", w.LONG), ("lLastY", w.LONG), ("ulExtraInformation", w.ULONG)]


class WNDCLASS(ctypes.Structure):
    _fields_ = [("style", w.UINT), ("lpfnWndProc", WNDPROC), ("cbClsExtra", ctypes.c_int), ("cbWndExtra", ctypes.c_int),
                ("hInstance", w.HINSTANCE), ("hIcon", w.HICON), ("hCursor", w.HANDLE), ("hbrBackground", w.HBRUSH),
                ("lpszMenuName", w.LPCWSTR), ("lpszClassName", w.LPCWSTR)]


kernel32.GetModuleHandleW.restype = w.HMODULE
user32.CreateWindowExW.restype = w.HWND
user32.CreateWindowExW.argtypes = [w.DWORD, w.LPCWSTR, w.LPCWSTR, w.DWORD, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                   ctypes.c_int, w.HWND, w.HMENU, w.HINSTANCE, w.LPVOID]
user32.DefWindowProcW.restype = LRESULT
user32.DefWindowProcW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
user32.GetRawInputData.argtypes = [w.HANDLE, w.UINT, ctypes.c_void_p, ctypes.POINTER(w.UINT), w.UINT]
user32.GetRawInputDeviceInfoW.argtypes = [w.HANDLE, w.UINT, ctypes.c_void_p, ctypes.POINTER(w.UINT)]

want = sys.argv[1].upper()
seconds = float(sys.argv[2])
names = {}
counts = {"mouse": 0, "keyboard": 0, "dx": 0, "dy": 0, "moves": 0}


def device_name(h):
    if h not in names:
        n = w.UINT(0)
        user32.GetRawInputDeviceInfoW(h, RIDI_DEVICENAME, None, ctypes.byref(n))
        buf = ctypes.create_unicode_buffer(n.value + 1)
        user32.GetRawInputDeviceInfoW(h, RIDI_DEVICENAME, buf, ctypes.byref(n))
        names[h] = buf.value.upper()
    return names[h]


def wndproc(hwnd, msg, wparam, lparam):
    if msg == WM_INPUT:
        size = w.UINT(0)
        hdr = ctypes.sizeof(RAWINPUTHEADER)
        user32.GetRawInputData(lparam, RID_INPUT, None, ctypes.byref(size), hdr)
        buf = ctypes.create_string_buffer(size.value)
        user32.GetRawInputData(lparam, RID_INPUT, buf, ctypes.byref(size), hdr)
        head = RAWINPUTHEADER.from_buffer_copy(buf)
        if want in device_name(head.hDevice):
            if head.dwType == RIM_TYPEMOUSE:
                m = RAWMOUSE.from_buffer_copy(buf, hdr)
                counts["mouse"] += 1
                counts["dx"] += m.lLastX
                counts["dy"] += m.lLastY
                if m.lLastX or m.lLastY:
                    counts["moves"] += 1
            elif head.dwType == RIM_TYPEKEYBOARD:
                counts["keyboard"] += 1
    return user32.DefWindowProcW(hwnd, msg, wparam, lparam)


proc = WNDPROC(wndproc)
wc = WNDCLASS(lpfnWndProc=proc, hInstance=kernel32.GetModuleHandleW(None), lpszClassName="rawcount")
user32.RegisterClassW(ctypes.byref(wc))
hwnd = user32.CreateWindowExW(0, "rawcount", "rawcount", 0, 0, 0, 0, 0, HWND_MESSAGE, None, wc.hInstance, None)
devs = (RAWINPUTDEVICE * 2)(RAWINPUTDEVICE(1, 2, RIDEV_INPUTSINK, hwnd), RAWINPUTDEVICE(1, 6, RIDEV_INPUTSINK, hwnd))
if not user32.RegisterRawInputDevices(devs, 2, ctypes.sizeof(RAWINPUTDEVICE)):
    sys.exit("RegisterRawInputDevices failed: %d" % ctypes.get_last_error())
print("listening", flush=True)
msg = w.MSG()
end = time.time() + seconds
while time.time() < end:
    while user32.PeekMessageW(ctypes.byref(msg), None, 0, 0, 1):
        user32.TranslateMessage(ctypes.byref(msg))
        user32.DispatchMessageW(ctypes.byref(msg))
    time.sleep(0.002)
print(json.dumps(counts))
