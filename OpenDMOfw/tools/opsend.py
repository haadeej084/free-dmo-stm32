#!/usr/bin/env python3
"""OpenDMO-FW host-side sender - speaks the genuine Dymo LabelWriter 550/5XL
wire protocol (LW550_TECHREF.txt) directly over USB, no vendor driver.

The command layout byte-matches the decompiled stock driver
(StartPrintJob / Density / Quality / MediaType / LabelLength / LabelIndex /
PrintData / ShortFormFeed / FormFeed / EndPrintJob), so a real host and this
tool are interchangeable on the wire.

Install:  pip install pyusb pillow
          (pillow only for 'image'; on Windows use WinUSB/Zadig for the device)

Examples:
  python opsend.py status
  python opsend.py --model OP57 feed 30
  python opsend.py density 120
  python opsend.py testpattern
  python opsend.py image label.png
  python opsend.py config --count 500 --sku S0904980
"""
import argparse, sys, time

VID = 0x0922                       # genuine Dymo vendor ID
MODELS = {                          # name -> (PID, dots across head, bytes/line)
    "OP104": (0x002A, 1248, 156),   # LabelWriter 5XL  (101 mm head)
    "OP57":  (0x0028,  672,  84),   # LabelWriter 550  (57 mm head)
}
EP_OUT = 0x01
EP_IN  = 0x81

# ---- protocol encoders (LW550_TECHREF.txt p.11-20; decompiled driver) ------
def cmd_start_job(job_id=1):        return b"\x1b\x73" + job_id.to_bytes(4, "little")
def cmd_density_reset():            return b"\x1b\x65"              # ESC e -> 100 %
def cmd_density(duty):              return b"\x1b\x43" + bytes([max(0, min(200, duty))])
def cmd_graphics():                 return b"\x1b\x69"              # ESC i (graphics mode)
def cmd_text():                     return b"\x1b\x68"              # ESC h (text mode)
def cmd_media_type():               return b"\x1b\x4d" + bytes(8)   # ESC M + 8B (mtDefault)
def cmd_label_length(length):       return b"\x1b\x4c" + length.to_bytes(2, "little")
def cmd_label_index(idx):           return b"\x1b\x6e" + idx.to_bytes(2, "little")
def cmd_set_count(count):           return b"\x1b\x6f" + count.to_bytes(2, "little")  # ESC o
def cmd_raster(lines, dots, data):
    # ESC D: BPP=1, Align=0x80, Width(#lines) u32 LE, Height(#dots) u32 LE, data
    return (b"\x1b\x44" + bytes([1, 0x80])
            + lines.to_bytes(4, "little") + dots.to_bytes(4, "little") + data)
def cmd_short_feed():               return b"\x1b\x47"              # ESC G (between labels)
def cmd_form_feed():                return b"\x1b\x45"              # ESC E (to tear bar)
def cmd_end_job():                  return b"\x1b\x51"              # ESC Q
def cmd_status_query(lock=0):       return b"\x1b\x41" + bytes([lock])  # ESC A
def cmd_restart():                  return b"\x1b\x40"              # ESC @ (pipeline reset)
def cmd_factory_reset():            return b"\x1b\x24"              # ESC *
def cmd_version():                  return b"\x1b\x56"              # ESC V
def cmd_sku_info():                 return b"\x1b\x55"              # ESC U
# Backdoor (never sent by the stock host; config + driver-less bring-up):
def cmd_config(count, sku):
    s = sku.encode("ascii")[:23]
    return bytes([0x1d, 0x43, len(s), count & 0xFF, (count >> 8) & 0xFF]) + s   # GS C
def cmd_feed(n):                    return bytes([0x1b, 0x64, n & 0xFF])        # ESC d
def cmd_diag(sub, arg=None):
    b = bytes([0x1d, 0x44, sub & 0xFF])                       # GS D <sub>
    if arg is not None:
        b += bytes([arg & 0xFF])                              # count (sub 0x01/0x02)
    return b

# ---- USB -------------------------------------------------------------------
def open_dev(vid, pid):
    try:
        import usb.core, usb.util
    except ImportError:
        sys.exit("pyusb missing: pip install pyusb")
    dev = usb.core.find(idVendor=vid, idProduct=pid)
    if dev is None:
        sys.exit(f"device {vid:04x}:{pid:04x} not found (plugged in? right --model?)")
    try:
        if dev.is_kernel_driver_active(0):
            dev.detach_kernel_driver(0)
    except Exception:
        pass                                  # Windows / not applicable
    dev.set_configuration()
    return dev

def send(dev, data):
    dev.write(EP_OUT, data, timeout=3000)

def read_bulk(dev, n, tries=5, timeout=1000):
    last = b""
    for _ in range(tries):
        last = bytes(dev.read(EP_IN, n, timeout=timeout))
        if len(last) >= 8:
            return last
        time.sleep(0.03)
    return last

def parse_status(r):
    if len(r) < 32:
        return {"raw": r.hex()}
    st = {
        "print_status": r[0],                 # 0 idle 1 printing 2 error 4 busy
        "job_id":     int.from_bytes(r[1:5], "little"),
        "label_index":int.from_bytes(r[5:7], "little"),
        "density_%":  r[9],                   # 0-200
        "bay":        r[10],                  # 8 ok / 10 counterfeit / 2 no media
        "sku":        r[11:23].split(b"\x00", 1)[0].decode("ascii", "replace"),
        "label_count":int.from_bytes(r[27:29], "little"),
    }
    return st

def read_status(dev, lock=0):
    send(dev, cmd_status_query(lock))
    r = read_bulk(dev, 32)
    return parse_status(r)

# ---- GS D self-test / diagnostic backdoor ----------------------------------
def read_diag(dev):
    """Read a 'D'-prefixed diagnostic reply (short packet, 3-24 bytes)."""
    r = b""
    for _ in range(5):
        try:
            r = bytes(dev.read(EP_IN, 24, timeout=500))
        except Exception:
            r = b""
        if len(r) >= 3 and r[0] == ord("D"):
            return r
        time.sleep(0.03)
    return r

def parse_diag(r):
    out = {"raw": r.hex()}
    if len(r) < 3 or r[0] != ord("D"):
        return out
    sub = r[1]
    out["sub"] = sub
    if sub == 0x04:                       # diagnostic snapshot
        out["model"] = {0x2a: "5XL/OP104", 0x28: "550/OP57"}.get(r[2], hex(r[2]))
        out["thermistor_raw"] = (r[3] << 8) | r[4]
        out["thermal_ok"] = bool(r[5])
        out["paper_present"] = bool(r[6] & 1)
        out["button_pressed"] = bool(r[6] & 2)
        out["density_%"] = r[7]
        out["flags"] = r[8]
        out["sku"] = r[9:21].split(b"\x00", 1)[0].decode("ascii", "replace")
        out["label_count"] = int.from_bytes(r[21:23], "little")
        out["head_bytes"] = r[23]
    elif sub == 0x01:                     # head strobe
        out["lines"] = r[2]; out["thermal_ok"] = bool(r[3])
    elif sub == 0x02:                     # motor step
        out["lines"] = r[2]
    elif sub == 0x03:                     # EEPROM self-test
        out["eeprom_match"] = bool(r[2])
    return out

# ---- raster-conversion -----------------------------------------------------
def image_to_raster(path, dots):
    try:
        from PIL import Image
    except ImportError:
        sys.exit("pillow missing (for 'image'): pip install pillow")
    im = Image.open(path).convert("L")
    w, h = im.size
    nh = max(1, round(h * dots / w))          # scale width to the head, keep ratio
    im = im.resize((dots, nh)).convert("1")   # 1-bit, 0 = black
    px = im.load()
    bpl = (dots + 7) // 8
    data = bytearray()
    for y in range(nh):
        for xb in range(bpl):
            b = 0
            for bit in range(8):              # MSB = leftmost dot
                x = xb * 8 + bit
                if x < dots and px[x, y] == 0:
                    b |= 0x80 >> bit
            data.append(b)
    return nh, dots, bytes(data)              # (lines, dots, data)

def test_pattern(dots, lines=120):
    """Diagonal stripes + border so width/alignment are visible."""
    bpl = (dots + 7) // 8
    data = bytearray()
    for y in range(lines):
        row = bytearray(bpl)
        for xb in range(bpl):
            b = 0
            for bit in range(8):
                x = xb * 8 + bit
                if x >= dots:
                    continue
                edge = (x < 4 or x >= dots - 4 or y < 4 or y >= lines - 4)
                diag = ((x + y) % 32) < 2
                if edge or diag:
                    b |= 0x80 >> bit
            row[xb] = b
        data += row
    return lines, dots, bytes(data)

# ---- a full print job (matches the stock driver's command sequence) --------
def send_job(dev, lines, dots, data, job_id=1, length=0):
    send(dev, cmd_start_job(job_id))
    send(dev, cmd_density_reset())
    send(dev, cmd_graphics())
    send(dev, cmd_media_type())
    send(dev, cmd_label_length(length))       # 0 = die-cut (tag sets pitch)
    send(dev, cmd_label_index(job_id))
    send(dev, cmd_raster(lines, dots, data))
    send(dev, cmd_short_feed())
    send(dev, cmd_form_feed())
    send(dev, cmd_end_job())

# ---- CLI -------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description="OpenDMO-FW Dymo wire-protocol sender")
    ap.add_argument("--model", choices=MODELS, default="OP104")
    ap.add_argument("--vid", type=lambda s: int(s, 0))
    ap.add_argument("--pid", type=lambda s: int(s, 0))
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("status")
    p = sub.add_parser("feed");      p.add_argument("lines", type=int)
    p = sub.add_parser("density");   p.add_argument("value", type=int, help="0-200 %%")
    sub.add_parser("testpattern")
    p = sub.add_parser("image");     p.add_argument("path")
    p = sub.add_parser("config");    p.add_argument("--count", type=int, required=True); p.add_argument("--sku", required=True)
    p = sub.add_parser("version")
    p = sub.add_parser("diag");      p.add_argument("sub", type=int, help="0x01 strobe head / 0x02 step motor / 0x03 EEPROM test / 0x04 snapshot")
    p.add_argument("arg", type=int, nargs="?", default=None, help="count for 0x01/0x02")
    a = ap.parse_args()

    pid, dots, bpl = MODELS[a.model]
    if a.pid: pid = a.pid
    vid = VID if not a.vid else a.vid

    dev = open_dev(vid, pid)

    if a.cmd == "status":
        print(read_status(dev))
    elif a.cmd == "feed":
        send(dev, cmd_feed(a.lines)); print(f"feed {a.lines} lines (backdoor ESC d)")
    elif a.cmd == "density":
        send(dev, cmd_density(a.value)); print(f"density set to {a.value} %")
    elif a.cmd == "config":
        send(dev, cmd_config(a.count, a.sku)); print("config set:", read_status(dev))
    elif a.cmd == "version":
        send(dev, cmd_version()); print(read_bulk(dev, 34).hex())
    elif a.cmd == "diag":
        send(dev, cmd_diag(a.sub, a.arg))
        time.sleep(0.2)
        print("diag:", parse_diag(read_diag(dev)))
    elif a.cmd in ("testpattern", "image"):
        if a.cmd == "image":
            lines, dots2, data = image_to_raster(a.path, dots)
        else:
            lines, dots2, data = test_pattern(dots)
        send_job(dev, lines, dots2, data)
        print(f"job sent: {lines} lines x {dots2} dots ({a.model}, head {bpl} B/line)")
        time.sleep(0.3)
        print("status:", read_status(dev))

if __name__ == "__main__":
    main()
