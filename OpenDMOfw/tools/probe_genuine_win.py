#!/usr/bin/env python3
"""probe_genuine.py for Windows WITHOUT Zadig: talk to a genuine LabelWriter
through Microsoft's own usbprint.sys.

probe_genuine.py needs pyusb and a WinUSB binding, which on Windows means
replacing DYMO's driver with Zadig. This variant needs neither: usbprint.sys
(the in-box USB printer class driver that DYMO's own driver stack sits on)
exposes the printer as a device interface that any process may CreateFile(),
answers the IEEE-1284 device ID over an IOCTL, and passes raw bytes through
WriteFile/ReadFile - which is all the protocol probes need.

SAFETY: same contract as probe_genuine.py. The send path refuses any byte
sequence that is not in SAFE_COMMANDS - status, version, roll record. It
cannot print, feed, write the SKU or touch the update path. Read-only.

USAGE
    py -3 tools/probe_genuine_win.py                  # find it, ask everything
    py -3 tools/probe_genuine_win.py --id-only        # only the 1284 device ID
    py -3 tools/probe_genuine_win.py --out 550_devid.txt --label "3a idle, roll fitted"

If CreateFile fails with "sharing violation" or "access denied", something
(DYMO's PnP service, a job in the spooler) has the port open. Wait for it, or
pause DYMO's services for the duration; nothing here needs them.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import sys
import time
import uuid

# ---- what this tool may send: read-only, same list as probe_genuine.py -----
SAFE_COMMANDS = {
    "ESC A  (status, 32-byte struct)":      b"\x1b\x41\x00",
    "ESC V  (firmware version)":            b"\x1b\x56",
    "ESC U  (consumable / roll record)":    b"\x1b\x55",
    "ESC B  (status, legacy 1-byte)":       b"\x1b\x42",
}
SAFE_BYTES = set(SAFE_COMMANDS.values())

# ---- Win32 -----------------------------------------------------------------
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
setupapi = ctypes.WinDLL("setupapi", use_last_error=True)

GENERIC_READ, GENERIC_WRITE = 0x80000000, 0x40000000
FILE_SHARE_READ, FILE_SHARE_WRITE = 1, 2
OPEN_EXISTING = 3
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
DIGCF_PRESENT, DIGCF_DEVICEINTERFACE = 0x02, 0x10

# usbprint.sys: GUID_DEVINTERFACE_USBPRINT and IOCTL_USBPRINT_GET_1284_ID
# (FILE_DEVICE_UNKNOWN 0x22, function 13, METHOD_BUFFERED, FILE_ANY_ACCESS).
GUID_USBPRINT = uuid.UUID("28d78fad-5a12-11d1-ae5b-0000f803a8c2")
IOCTL_USBPRINT_GET_1284_ID = 0x220034


class GUID(ctypes.Structure):
    _fields_ = [("Data1", wt.DWORD), ("Data2", wt.WORD), ("Data3", wt.WORD),
                ("Data4", ctypes.c_ubyte * 8)]

    @classmethod
    def from_uuid(cls, u):
        b = u.bytes_le
        return cls(int.from_bytes(b[0:4], "little"), int.from_bytes(b[4:6], "little"),
                   int.from_bytes(b[6:8], "little"), (ctypes.c_ubyte * 8)(*b[8:16]))


class SP_DEVICE_INTERFACE_DATA(ctypes.Structure):
    _fields_ = [("cbSize", wt.DWORD), ("InterfaceClassGuid", GUID),
                ("Flags", wt.DWORD), ("Reserved", ctypes.c_void_p)]


# 64-bit handles: without these prototypes ctypes truncates HDEVINFO / HANDLE
# to a C int and every call after the first fails silently.
HANDLE = ctypes.c_void_p
setupapi.SetupDiGetClassDevsW.restype = HANDLE
setupapi.SetupDiGetClassDevsW.argtypes = [ctypes.c_void_p, wt.LPCWSTR, wt.HWND, wt.DWORD]
setupapi.SetupDiEnumDeviceInterfaces.restype = wt.BOOL
setupapi.SetupDiEnumDeviceInterfaces.argtypes = [HANDLE, ctypes.c_void_p, ctypes.c_void_p, wt.DWORD, ctypes.c_void_p]
setupapi.SetupDiGetDeviceInterfaceDetailW.restype = wt.BOOL
setupapi.SetupDiGetDeviceInterfaceDetailW.argtypes = [HANDLE, ctypes.c_void_p, ctypes.c_void_p, wt.DWORD, ctypes.c_void_p, ctypes.c_void_p]
setupapi.SetupDiDestroyDeviceInfoList.restype = wt.BOOL
setupapi.SetupDiDestroyDeviceInfoList.argtypes = [HANDLE]
k32.CreateFileW.restype = HANDLE
k32.CreateFileW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, ctypes.c_void_p, wt.DWORD, wt.DWORD, HANDLE]
k32.DeviceIoControl.restype = wt.BOOL
k32.DeviceIoControl.argtypes = [HANDLE, wt.DWORD, ctypes.c_void_p, wt.DWORD, ctypes.c_void_p, wt.DWORD, ctypes.c_void_p, ctypes.c_void_p]
k32.WriteFile.restype = wt.BOOL
k32.WriteFile.argtypes = [HANDLE, ctypes.c_void_p, wt.DWORD, ctypes.c_void_p, ctypes.c_void_p]
k32.ReadFile.restype = wt.BOOL
k32.ReadFile.argtypes = [HANDLE, ctypes.c_void_p, wt.DWORD, ctypes.c_void_p, ctypes.c_void_p]
k32.CloseHandle.restype = wt.BOOL
k32.CloseHandle.argtypes = [HANDLE]


class OVERLAPPED(ctypes.Structure):
    _fields_ = [("Internal", ctypes.c_void_p), ("InternalHigh", ctypes.c_void_p),
                ("Offset", wt.DWORD), ("OffsetHigh", wt.DWORD), ("hEvent", HANDLE)]


FILE_FLAG_OVERLAPPED = 0x40000000
ERROR_IO_PENDING = 997
WAIT_OBJECT_0 = 0
k32.CreateEventW.restype = HANDLE
k32.CreateEventW.argtypes = [ctypes.c_void_p, wt.BOOL, wt.BOOL, wt.LPCWSTR]
k32.WaitForSingleObject.restype = wt.DWORD
k32.WaitForSingleObject.argtypes = [HANDLE, wt.DWORD]
k32.GetOverlappedResult.restype = wt.BOOL
k32.GetOverlappedResult.argtypes = [HANDLE, ctypes.c_void_p, ctypes.c_void_p, wt.BOOL]
k32.CancelIoEx.restype = wt.BOOL
k32.CancelIoEx.argtypes = [HANDLE, ctypes.c_void_p]



def _err(what):
    e = ctypes.get_last_error()
    return f"{what}: WinError {e} ({ctypes.FormatError(e).strip()})"


def enumerate_usbprint_paths():
    """Every present device that exposes the usbprint interface."""
    guid = GUID.from_uuid(GUID_USBPRINT)
    hdev = setupapi.SetupDiGetClassDevsW(ctypes.byref(guid), None, None,
                                         DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)
    if hdev == INVALID_HANDLE_VALUE:
        raise OSError(_err("SetupDiGetClassDevs"))
    paths = []
    try:
        i = 0
        while True:
            did = SP_DEVICE_INTERFACE_DATA()
            did.cbSize = ctypes.sizeof(did)
            if not setupapi.SetupDiEnumDeviceInterfaces(hdev, None, ctypes.byref(guid), i, ctypes.byref(did)):
                break
            need = wt.DWORD(0)
            setupapi.SetupDiGetDeviceInterfaceDetailW(hdev, ctypes.byref(did), None, 0, ctypes.byref(need), None)
            buf = ctypes.create_string_buffer(need.value)
            # cbSize of SP_DEVICE_INTERFACE_DETAIL_DATA_W: 6 on 32-bit, 8 on 64-bit
            ctypes.cast(buf, ctypes.POINTER(wt.DWORD))[0] = 8 if ctypes.sizeof(ctypes.c_void_p) == 8 else 6
            if not setupapi.SetupDiGetDeviceInterfaceDetailW(hdev, ctypes.byref(did), buf, need, None, None):
                raise OSError(_err("SetupDiGetDeviceInterfaceDetail"))
            paths.append(ctypes.wstring_at(ctypes.addressof(buf) + 4))
            i += 1
    finally:
        setupapi.SetupDiDestroyDeviceInfoList(hdev)
    return paths


def open_port(path):
    h = k32.CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        None, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, None)
    if h == INVALID_HANDLE_VALUE:
        raise OSError(_err("CreateFile"))
    return h


def get_1284_id(h):
    out = ctypes.create_string_buffer(1024)
    ret = wt.DWORD(0)
    ov = OVERLAPPED()
    ov.hEvent = k32.CreateEventW(None, True, False, None)
    try:
        ok = k32.DeviceIoControl(h, IOCTL_USBPRINT_GET_1284_ID, None, 0, out, 1024, ctypes.byref(ret), ctypes.byref(ov))
        if not ok:
            if ctypes.get_last_error() != ERROR_IO_PENDING:
                raise OSError(_err("IOCTL_USBPRINT_GET_1284_ID"))
            if k32.WaitForSingleObject(ov.hEvent, 2000) != WAIT_OBJECT_0:
                k32.CancelIoEx(h, ctypes.byref(ov))
                raise OSError("IOCTL_USBPRINT_GET_1284_ID: timed out")
            k32.GetOverlappedResult(h, ctypes.byref(ov), ctypes.byref(ret), True)
    finally:
        k32.CloseHandle(ov.hEvent)
    raw = out.raw[:ret.value]
    # Big-endian 2-byte length prefix, then the string (IEEE 1284 / USB printer class 1.0).
    if len(raw) >= 2:
        n = (raw[0] << 8) | raw[1]
        body = raw[2:2 + max(0, n - 2)] if n >= 2 else raw[2:]
    else:
        body = raw
    return raw, body.decode("ascii", "replace")


def send(h, data):
    if data not in SAFE_BYTES:
        raise ValueError(f"refusing to send {data!r}: not in SAFE_COMMANDS")
    n = wt.DWORD(0)
    ov = OVERLAPPED()
    ov.hEvent = k32.CreateEventW(None, True, False, None)
    try:
        ok = k32.WriteFile(h, data, len(data), ctypes.byref(n), ctypes.byref(ov))
        if not ok:
            if ctypes.get_last_error() != ERROR_IO_PENDING:
                raise OSError(_err("WriteFile"))
            if k32.WaitForSingleObject(ov.hEvent, 2000) != WAIT_OBJECT_0:
                k32.CancelIoEx(h, ctypes.byref(ov))
                raise OSError("WriteFile: timed out (printer not accepting data)")
            k32.GetOverlappedResult(h, ctypes.byref(ov), ctypes.byref(n), True)
        return n.value
    finally:
        k32.CloseHandle(ov.hEvent)


def send_unsafe(h, data):
    """The one path around SAFE_COMMANDS: only for the two documented,
    non-printing state commands behind --set-count / --restart."""
    assert data[:2] in (b"\x1b\x6f", b"\x1b\x40"), "send_unsafe is for ESC o / ESC @ only"
    n = wt.DWORD(0)
    ov = OVERLAPPED()
    ov.hEvent = k32.CreateEventW(None, True, False, None)
    try:
        ok = k32.WriteFile(h, data, len(data), ctypes.byref(n), ctypes.byref(ov))
        if not ok:
            if ctypes.get_last_error() != ERROR_IO_PENDING:
                raise OSError(_err("WriteFile"))
            if k32.WaitForSingleObject(ov.hEvent, 2000) != WAIT_OBJECT_0:
                k32.CancelIoEx(h, ctypes.byref(ov))
                raise OSError("WriteFile: timed out")
            k32.GetOverlappedResult(h, ctypes.byref(ov), ctypes.byref(n), True)
        return n.value
    finally:
        k32.CloseHandle(ov.hEvent)


def recv(h, want, timeout_s=2.0):
    """Read up to `want` bytes with a real timeout. A plain ReadFile on usbprint
    blocks for as long as the printer stays silent, so the read is overlapped
    and cancelled when the clock runs out; whatever arrived is returned."""
    buf = ctypes.create_string_buffer(want)
    ov = OVERLAPPED()
    ov.hEvent = k32.CreateEventW(None, True, False, None)
    n = wt.DWORD(0)
    try:
        ok = k32.ReadFile(h, buf, want, ctypes.byref(n), ctypes.byref(ov))
        if not ok:
            e = ctypes.get_last_error()
            if e != ERROR_IO_PENDING:
                raise OSError(_err("ReadFile"))
            if k32.WaitForSingleObject(ov.hEvent, int(timeout_s * 1000)) != WAIT_OBJECT_0:
                k32.CancelIoEx(h, ctypes.byref(ov))
                k32.GetOverlappedResult(h, ctypes.byref(ov), ctypes.byref(n), True)
                return buf.raw[:n.value]
            k32.GetOverlappedResult(h, ctypes.byref(ov), ctypes.byref(n), True)
        return buf.raw[:n.value]
    finally:
        k32.CloseHandle(ov.hEvent)


def hexdump(b):
    return " ".join(f"{x:02x}" for x in b)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", help="append the answers to this file")
    ap.add_argument("--label", default="", help="state label for the status dump, e.g. '3b out of paper'")
    ap.add_argument("--id-only", action="store_true", help="only the IEEE-1284 device ID")
    ap.add_argument("--path", help="usbprint device path (default: the first VID_0922)")
    # NOT read-only, deliberately outside SAFE_COMMANDS and off by default:
    # both are in DYMO's own 550 manual (p.20) and neither prints, feeds or
    # touches the update path, but they change printer state.
    ap.add_argument("--set-count", type=int, metavar="N",
                    help="send ESC o N (DYMO 'set label count', 0-255) BEFORE the probes, then read the status back")
    ap.add_argument("--u16", action="store_true", help="with --set-count: send the count as u16 LE (ESC o lo hi)")
    ap.add_argument("--restart", action="store_true",
                    help="send ESC @ ('restart print engine') BEFORE the probes, then read the status back")
    a = ap.parse_args()

    lines = []
    def say(s=""):
        print(s, flush=True); lines.append(s)

    paths = enumerate_usbprint_paths()
    dymo = [p for p in paths if "vid_0922" in p.lower()]
    say(f"usbprint interfaces present: {len(paths)}; DYMO: {len(dymo)}")
    for p in paths:
        say(f"  {p}")
    path = a.path or (dymo[0] if dymo else None)
    if not path:
        say("no DYMO (VID 0922) printer on usbprint - is it plugged in and enumerated?")
        return 1

    say(f"\nopening {path}")
    h = open_port(path)
    try:
        if a.set_count is not None or a.restart:
            send(h, SAFE_COMMANDS["ESC A  (status, 32-byte struct)"])
            before = recv(h, 32)
            cnt = (before[27] | (before[28] << 8)) if len(before) == 32 else "?"
            say(f"\n[pre] ESC A before: {hexdump(before)}  (count = {cnt})")
            if a.set_count is not None:
                cmd = bytes([0x1B, 0x6F, a.set_count & 0xFF] + ([(a.set_count >> 8) & 0xFF] if a.u16 else []))
                say(f"[state-changing] ESC o {a.set_count} -> {hexdump(cmd)}")
                send_unsafe(h, cmd)
            if a.restart:
                cmd = b"\x1b\x40"
                say(f"[state-changing] ESC @ -> {hexdump(cmd)}")
                send_unsafe(h, cmd)
                time.sleep(1.0)
        raw, s = get_1284_id(h)
        say(f"\n[1] IEEE-1284 device ID ({len(raw)} bytes raw, length prefix {hexdump(raw[:2])}):")
        say(f"    {s}")
        say(f"    raw: {hexdump(raw)}")
        if a.id_only:
            return 0

        for name, cmd in SAFE_COMMANDS.items():
            if name.startswith("ESC B"):
                continue      # legacy; ESC A covers it
            say(f"\n[{name}]  -> {hexdump(cmd)}")
            send(h, cmd)
            # Ask for EXACTLY the genuine reply length. usbprint hands a read
            # whatever is queued on bulk-IN, and DYMO Connect polls ESC A on the
            # same pipe: a 128-byte request once came back as the 64-byte record
            # followed by DYMO's own 32-byte status - a reply stolen from its
            # poller. Exact sizes keep the two conversations apart.
            want = {"ESC A": 32, "ESC V": 34, "ESC U": 64}[name[:5]]
            reply = recv(h, want)
            tag = f" ({a.label})" if a.label and name.startswith("ESC A") else ""
            say(f"    <- {len(reply)} bytes{tag}: {hexdump(reply)}")
            if reply and not name.startswith("ESC A"):
                say(f"       ascii: {reply.decode('ascii', 'replace')!r}")
    finally:
        k32.CloseHandle(h)

    if a.out:
        with open(a.out, "a", encoding="utf-8") as f:
            f.write(time.strftime("# %Y-%m-%d %H:%M:%S") + (f"  {a.label}" if a.label else "") + "\n")
            f.write("\n".join(lines) + "\n\n")
        say(f"\nappended to {a.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
