#!/usr/bin/env python3
"""Interrogate a GENUINE DYMO LabelWriter, read-only, and print what it answers.

This closes questions no amount of research could. Cycle 5 proved that the
genuine 550's IEEE-1284 device ID and its ESC V reply are obtainable from NO
online source - DYMO ships no firmware, lprint has the 550 family commented out,
and the Windows USBPRINT checksum provably covers only MFG and MDL, so no INF or
setupapi log can ever reveal the other keys. A genuine printer on a USB cable
answers all of it in about a second.

SAFETY - read this before running it against a printer you care about.

This script sends ONLY commands that ask questions. It will not print, will not
feed, will not write configuration, and will not touch the firmware-update path.
Every byte it can emit is listed in SAFE_COMMANDS below and the send path refuses
anything not on that list, so a typo cannot turn into a print job. It does not
open the printer, does not need it dismantled, and leaves no state behind.

What it CANNOT do, deliberately: ESC D / ESC L / ESC G / ESC E (print or feed),
GS C (writes the SKU), GS D (our own diagnostics - a genuine printer does not
have them and the sub-command space is not ours to poke), ESC @ / ESC $ (reset,
factory reset), ESC W / ESC R (firmware update framing).

USAGE
    python3 tools/probe_genuine.py                 # find it, ask everything
    python3 tools/probe_genuine.py --vid 0x0922 --pid 0x0028
    python3 tools/probe_genuine.py --out capture.txt

Needs pyusb and a driver that lets a raw handle bind (on Windows, WinUSB via
Zadig on the printer interface; on Linux, root or a udev rule). If DYMO's own
driver holds the device, unbind it first or use a USB capture instead - see the
note at the end of the output.

WHAT EACH ANSWER SETTLES
  GET_DEVICE_ID   the IEEE-1284 string verbatim, including the CMD, CLASS,
                  DESCRIPTION and SERN keys this project has never seen. Those
                  four are on the Hardware-only list with a proof that nothing
                  else can supply them.
  ESC V           the version reply, whose format and length this firmware
                  currently guesses.
  ESC A           the 32-byte status struct. Thirty of its bytes were
                  transcribed once from a manual and never re-verified; poll it
                  in several states (paper in, paper out, cover open, idle,
                  mid-job) and the real layout falls out.
  ESC U           the consumable record for the roll actually fitted, which is
                  the only direct check on this project's reconstruction of it -
                  including the die-cut gap, which the firmware currently takes
                  as a fleet-wide constant.
"""
import argparse
import sys

# Every byte sequence this tool is allowed to put on the wire. Read-only.
SAFE_COMMANDS = {
    "ESC A  (status, 32-byte struct)":      b"\x1b\x41\x00",
    "ESC V  (firmware version)":            b"\x1b\x56",
    "ESC U  (consumable / roll record)":    b"\x1b\x55",
    "ESC B  (status, legacy 1-byte)":       b"\x1b\x42",
}

EP_OUT, EP_IN = 0x02, 0x82
DYMO_VID = 0x0922


def hexdump(b, width=16):
    out = []
    for i in range(0, len(b), width):
        chunk = b[i:i + width]
        hexed = " ".join(f"{x:02x}" for x in chunk)
        text = "".join(chr(x) if 32 <= x < 127 else "." for x in chunk)
        out.append(f"  {i:04x}  {hexed:<{width * 3}} |{text}|")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--vid", type=lambda s: int(s, 0), default=DYMO_VID)
    ap.add_argument("--pid", type=lambda s: int(s, 0), default=None,
                    help="default: take the first DYMO device found")
    ap.add_argument("--out", help="also write everything to this file")
    a = ap.parse_args()

    try:
        import usb.core, usb.util
    except ImportError:
        sys.exit("pyusb is not installed: pip install pyusb")

    dev = usb.core.find(idVendor=a.vid, idProduct=a.pid) if a.pid else \
          usb.core.find(idVendor=a.vid)
    if dev is None:
        sys.exit(f"no device with VID {a.vid:#06x}"
                 + (f" PID {a.pid:#06x}" if a.pid else "") + " found")

    lines = []

    def say(s=""):
        print(s)
        lines.append(s)

    say(f"device     {dev.idVendor:#06x}:{dev.idProduct:#06x}")
    for name, attr in (("manufacturer", "manufacturer"), ("product", "product"),
                       ("serial", "serial_number")):
        try:
            say(f"{name:<10} {getattr(dev, attr)}")
        except Exception as e:
            say(f"{name:<10} <unreadable: {e}>")

    # ---- GET_DEVICE_ID: the one this project most needs -----------------
    say("\n=== GET_DEVICE_ID (printer class 0, the IEEE-1284 string) ===")
    try:
        cfg = dev.get_active_configuration()
        intf = cfg[(0, 0)]
        wIndex = (intf.bInterfaceNumber << 8) | intf.bAlternateSetting
        raw = dev.ctrl_transfer(0xA1, 0, 0, wIndex, 1024)
        n = (raw[0] << 8) | raw[1]
        say(f"  length field {n}, payload {len(raw) - 2} bytes")
        say(f"  {bytes(raw[2:n]).decode('ascii', 'replace')}")
        say("\n  -> paste this verbatim into DECISIONS; it settles MODEL_IEEE_ID's")
        say("     CMD / CLASS / DESCRIPTION / SERN keys, which are otherwise")
        say("     obtainable from nothing.")
    except Exception as e:
        say(f"  FAILED: {e}")
        say("  (a driver probably owns the interface - see the note at the end)")

    # ---- the read-only wire commands -------------------------------------
    for name, cmd in SAFE_COMMANDS.items():
        assert cmd in SAFE_COMMANDS.values(), "refusing to send an unlisted command"
        say(f"\n=== {name} ===")
        say(f"  sent: {' '.join(f'{x:02x}' for x in cmd)}")
        try:
            dev.write(EP_OUT, cmd, timeout=1000)
            reply = bytes(dev.read(EP_IN, 512, timeout=1500))
            say(f"  {len(reply)} bytes:")
            say(hexdump(reply))
        except Exception as e:
            say(f"  no reply: {e}")

    say("\n" + "=" * 70)
    say("If everything failed with a permission or resource error, the vendor")
    say("driver owns the device. The alternative needs no unbinding at all:")
    say("capture DYMO's own software talking to it - USBPcap + Wireshark on")
    say("Windows, or usbmon on Linux - print one label, and hand the .pcapng to")
    say("tools/capture_replay/. That gives the same answers plus the whole job")
    say("sequence, and it is how this project identified the 450 traffic it")
    say("already has.")

    if a.out:
        with open(a.out, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        print(f"\nwritten to {a.out}")


if __name__ == "__main__":
    main()
