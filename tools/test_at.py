#!/usr/bin/env python3
"""AT protocol test suite for the battery monitor.

    ./test_at.py /dev/ttyACM0                 # protocol tests only
    ./test_at.py /dev/ttyACM0 --ota FW.bin    # also exercise ATFW (slow)

Every test leaves the device in a known state: the suite hard-resets before
starting and drains RX between cases, because a desynchronised link produces
cascading failures that hide the real one.

Exit code 0 = all passed.
"""

import argparse
import hashlib
import json
import re
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial not installed:  pip install pyserial")

CHUNK_ACK = b"."
PASS, FAIL = "PASS", "FAIL"


def send_image(d, image, chunk):
    """Send an image with the device's per-chunk ACK pacing."""
    d.ser.timeout = 10.0
    sent = 0
    while sent < len(image):
        d.ser.write(image[sent:sent + chunk])
        d.ser.flush()
        sent += chunk
        ack = d.ser.read(1)
        if ack != CHUNK_ACK:
            rest = d.ser.read(d.ser.in_waiting or 0)
            return (ack + rest).decode(errors="replace").strip()
    return None


def handshake(d, size, md5):
    d.drain()
    d.ser.write(f"ATFW={size},{md5}\r\n".encode())
    d.ser.flush()
    time.sleep(0.5)
    resp = d.ser.read(d.ser.in_waiting or 1).decode(errors="replace").strip()
    if resp == "OK":
        return 4096
    if resp.startswith("OK "):
        return int(resp.split()[1])
    return None
results = []


class Device:
    def __init__(self, port, baud=115200, timeout=2.0):
        self.ser = serial.Serial(port, baud, timeout=timeout)
        self.reset()

    def reset(self):
        """Hard reset via RTS, then wait for boot and drain the banner."""
        self.ser.setDTR(False)
        self.ser.setRTS(True)
        time.sleep(0.1)
        self.ser.setRTS(False)
        time.sleep(2.0)
        self.drain()

    def drain(self):
        """Discard anything pending, both directions."""
        self.ser.reset_input_buffer()
        self.ser.reset_output_buffer()
        end = time.monotonic() + 0.3
        while time.monotonic() < end:
            if self.ser.in_waiting:
                self.ser.read(self.ser.in_waiting)
                end = time.monotonic() + 0.3
            else:
                time.sleep(0.05)

    def cmd(self, text, wait=0.4):
        """Send one command, return everything it replied, stripped."""
        self.drain()
        self.ser.write((text + "\r\n").encode())
        self.ser.flush()
        time.sleep(wait)
        out = b""
        while self.ser.in_waiting:
            out += self.ser.read(self.ser.in_waiting)
            time.sleep(0.05)
        return out.decode(errors="replace").strip()

    def close(self):
        self.ser.close()


def check(name, condition, detail=""):
    results.append((name, PASS if condition else FAIL, detail))
    mark = "ok  " if condition else "FAIL"
    print(f"  [{mark}] {name}" + (f"  <- {detail}" if not condition else ""))
    return condition


# -- Protocol tests -----------------------------------------------------------

def test_keepalive(d):
    print("\nkeepalive")
    check("AT returns OK", d.cmd("AT") == "OK", repr(d.cmd("AT")))
    check("at (lowercase) returns OK", d.cmd("at") == "OK")
    check("  AT  (whitespace) returns OK", d.cmd("  AT  ") == "OK")
    # A bare newline must not produce a response: the protocol is strictly
    # one-response-per-command, and a spurious OK would desynchronise a host
    # that is counting replies.
    d.drain()
    d.ser.write(b"\r\n")
    time.sleep(0.3)
    check("bare newline is silent", d.ser.in_waiting == 0,
          f"{d.ser.in_waiting} bytes")


def test_errors(d):
    print("\nerror handling")
    check("unknown command -> ERROR 2", d.cmd("ATXYZ").startswith("ERROR 2"))
    check("missing AT prefix -> ERROR 1", d.cmd("HELLO").startswith("ERROR 1"))
    r = d.cmd("AT" + "X" * 300)
    check("overlong line -> ERROR 1", "ERROR 1" in r, repr(r[:60]))
    check("device alive after errors", d.cmd("AT") == "OK")


def test_identity(d):
    print("\nidentity")
    r = d.cmd("ATI")
    parts = r.split(",")
    check("ATI has 4 fields", len(parts) == 4, repr(r))
    check("ATI reports esp32c3", "esp32c3" in r, repr(r))
    check("ATI MAC is 12 hex chars",
          bool(re.fullmatch(r"[0-9A-F]{12}", parts[-1])) if parts else False,
          repr(parts[-1] if parts else None))


def test_measurement(d):
    print("\nmeasurement")
    r = d.cmd("ATA")
    if not check("ATA returns JSON", r.startswith("{"), repr(r[:60])):
        return
    try:
        obj = json.loads(r)
    except json.JSONDecodeError as e:
        check("ATA JSON parses", False, str(e))
        return
    check("ATA JSON parses", True)
    for key in ("v", "i", "p", "t", "err"):
        check(f"ATA has '{key}'", key in obj, repr(sorted(obj)))
    if "t" in obj:
        # Die temperature is the one field with a knowable plausible range,
        # and a wrong DIETEMP scaling would show up here immediately.
        check("die temp plausible (0-60 C)", 0 < obj["t"] < 60, f"{obj.get('t')} C")
    if "err" in obj:
        check("no latched INA228 fault", obj["err"] == 0, f"err={obj['err']}")

    # Two reads a second apart must both succeed: catches a gauge task that
    # dies after one poll.
    time.sleep(1.2)
    check("ATA repeatable", d.cmd("ATA").startswith("{"))


def test_log(d):
    print("\nlog buffer")
    r = d.cmd("ATL", wait=1.2)
    lines = r.splitlines()
    check("ATL header names a count", bool(re.match(r"\d+ lines follow", r)),
          repr(lines[0] if lines else None))
    check("ATL terminates with OK", lines and lines[-1] == "OK",
          repr(lines[-1] if lines else None))
    if lines and re.match(r"\d+ lines follow", lines[0]):
        n = int(lines[0].split()[0])
        check("ATL line count matches header", len(lines) == n + 2,
              f"header says {n}, got {len(lines) - 2}")
    check("ATL captured the INA228 probe", "ina228" in r, "no ina228 line")
    # Non-destructive: a dropped response must not lose the log.
    check("ATL is idempotent", d.cmd("ATL", wait=1.2) == r)


def test_ota_rejects(d, img_size):
    print("\nATFW input validation (image must survive all of these)")
    good_md5 = "0" * 32
    cases = [
        ("ATFW=999",                       "ERROR 1", "missing md5"),
        ("ATFW=100,abc",                   "ERROR 3", "short md5"),
        ("ATFW=100," + "z" * 32,           "ERROR 3", "non-hex md5"),
        ("ATFW=0," + good_md5,             "ERROR 3", "zero size"),
        ("ATFW=99999999," + good_md5,      "ERROR 3", "oversize"),
    ]
    for cmd, expect, why in cases:
        r = d.cmd(cmd)
        # Critically: the rejection must come INSTEAD of OK, not after it. An
        # OK commits the host to streaming binary immediately.
        check(f"{why} -> {expect}, no OK first",
              r.startswith(expect), repr(r[:70]))
    check("device alive after rejects", d.cmd("AT") == "OK")


def test_ota_md5_mismatch(d, image):
    print("\nATFW md5 mismatch (transfers a full image, slow)")
    chunk = handshake(d, len(image), "0" * 32)
    if not check("handshake acknowledged", chunk is not None):
        return
    early = send_image(d, image, chunk)
    if early:
        r = early
    else:
        out = b""
        end = time.monotonic() + 30
        while time.monotonic() < end and b"\n" not in out:
            out += d.ser.read(d.ser.in_waiting or 1)
        r = out.decode(errors="replace").strip()
    check("wrong md5 -> ERROR 9", r.startswith("ERROR 9"), repr(r[:70]))
    # The RX flush after a failed transfer is what makes this possible; without
    # it the leftover payload is parsed as commands.
    time.sleep(0.5)
    check("link resynchronised after failure", d.cmd("AT") == "OK")


def test_ota_success(d, image):
    print("\nATFW successful update (reboots the device)")
    md5 = hashlib.md5(image).hexdigest()
    before = d.cmd("ATI")
    chunk = handshake(d, len(image), md5)
    if not check("handshake acknowledged", chunk is not None):
        return
    t0 = time.monotonic()
    early = send_image(d, image, chunk)
    if early:
        r = early
    else:
        out = b""
        end = time.monotonic() + 60
        while time.monotonic() < end and b"\n" not in out:
            out += d.ser.read(d.ser.in_waiting or 1)
        r = out.decode(errors="replace").strip()
    if not check("valid image -> OK", r.startswith("OK"), repr(r[:70])):
        return
    print(f"       transferred {len(image)} B in {time.monotonic() - t0:.1f} s")

    time.sleep(6)  # reboot + USB re-enumeration
    d.drain()
    check("device responds after OTA reboot", d.cmd("AT", wait=1.0) == "OK")
    check("ATI still answers", d.cmd("ATI") == before,
          "version changed unexpectedly")


def main():
    ap = argparse.ArgumentParser(description="AT protocol test suite")
    ap.add_argument("port")
    ap.add_argument("--ota", metavar="FW.bin",
                    help="also run ATFW transfer tests using this image")
    args = ap.parse_args()

    print(f"Resetting {args.port} ...")
    d = Device(args.port)

    image = None
    if args.ota:
        with open(args.ota, "rb") as f:
            image = f.read()
        if image[0] != 0xE9:
            sys.exit(f"{args.ota}: not an ESP-IDF app image (first byte "
                     f"0x{image[0]:02X}, expected 0xE9)")

    try:
        test_keepalive(d)
        test_errors(d)
        test_identity(d)
        test_measurement(d)
        test_log(d)
        test_ota_rejects(d, len(image) if image else 0)
        if image:
            test_ota_md5_mismatch(d, image)
            test_ota_success(d, image)
    finally:
        d.close()

    failed = [r for r in results if r[1] == FAIL]
    print(f"\n{len(results) - len(failed)}/{len(results)} passed")
    if failed:
        print("\nfailures:")
        for name, _, detail in failed:
            print(f"  {name}" + (f"  <- {detail}" if detail else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
