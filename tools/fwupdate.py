#!/usr/bin/env python3
"""OTA client for the INA228 power meter.

    ./fwupdate.py /dev/ttyACM0 build/power_meter.bin

Speaks the ATFW protocol from DEVELOPMENT_PLAN.md S2.5:

    host -> ATFW=<bytes>,<md5hex>
    dev  <- OK                      device enters raw binary mode
    host -> <exactly N raw bytes>
    dev  <- OK                      MD5 verified, boot partition set, rebooting
         |  ERROR <code> <desc>     nothing was changed, current image kept

Requires pyserial.
"""

import argparse
import hashlib
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial not installed:  pip install pyserial")

# Device-side reboot after a successful update, plus USB re-enumeration.
REBOOT_WAIT_S = 5.0

# Fallback if the device does not announce a chunk size. The transfer is
# windowed: the device emits one ACK byte per chunk consumed and we wait for
# it before sending the next. Without this the host outruns the device's flash
# writes and the USB driver silently discards the overflow.
DEFAULT_CHUNK = 4096
CHUNK_ACK = b"."


class ProtocolError(Exception):
    pass


class Link:
    """Line-oriented AT transport with a raw-bytes escape hatch."""

    def __init__(self, port, baud, timeout):
        self.ser = serial.Serial(port, baud, timeout=timeout)
        # The C3's USB-CDC has no real baud rate; UART builds do. Harmless either way.
        self.ser.reset_input_buffer()
        self.ser.reset_output_buffer()

    def command(self, cmd, timeout=None):
        """Send one AT command, return its single response line."""
        if timeout is not None:
            self.ser.timeout = timeout
        self.ser.write((cmd + "\r\n").encode())
        self.ser.flush()
        return self._read_line()

    def _read_line(self):
        line = self.ser.readline()
        if not line:
            raise ProtocolError("timeout waiting for response")
        return line.decode(errors="replace").strip()

    def close(self):
        self.ser.close()


def expect_ok(resp, what):
    """Accept bare 'OK' or 'OK <chunk>'; returns the chunk size."""
    if resp == "OK":
        return DEFAULT_CHUNK
    if resp.startswith("OK "):
        try:
            return int(resp.split()[1])
        except (IndexError, ValueError):
            return DEFAULT_CHUNK
    if resp.startswith("ERROR"):
        raise ProtocolError(f"{what}: device returned {resp}")
    raise ProtocolError(f"{what}: unexpected response {resp!r}")


def progress(sent, total, started):
    pct = 100.0 * sent / total
    elapsed = time.monotonic() - started
    rate = sent / elapsed / 1024 if elapsed > 0 else 0
    bar = "#" * int(pct / 2.5)
    sys.stdout.write(f"\r  [{bar:<40}] {pct:5.1f}%  {sent}/{total} B  {rate:.0f} kB/s")
    sys.stdout.flush()


def main():
    ap = argparse.ArgumentParser(description="OTA update over the AT serial link")
    ap.add_argument("port", help="serial device, e.g. /dev/ttyACM0 or /dev/serial0")
    ap.add_argument("firmware", help="path to the .bin image")
    ap.add_argument("-b", "--baud", type=int, default=115200,
                    help="baud rate (ignored on USB-CDC builds; default 115200)")
    ap.add_argument("-t", "--timeout", type=float, default=10.0,
                    help="response timeout in seconds (default 10)")
    ap.add_argument("--no-verify", action="store_true",
                    help="skip the post-reboot ATI check")
    args = ap.parse_args()

    with open(args.firmware, "rb") as f:
        image = f.read()
    if not image:
        sys.exit("firmware image is empty")
    # ESP-IDF app images start with the 0xE9 magic byte. Catches the classic
    # mistake of flashing bootloader.bin or a .elf.
    if image[0] != 0xE9:
        sys.exit(f"{args.firmware}: not an ESP-IDF app image "
                 f"(first byte 0x{image[0]:02X}, expected 0xE9)")

    md5 = hashlib.md5(image).hexdigest()
    print(f"image   : {args.firmware}")
    print(f"size    : {len(image)} bytes")
    print(f"md5     : {md5}")

    link = Link(args.port, args.baud, args.timeout)
    try:
        # Keepalive first: proves the link is alive and the device is in line
        # mode before we commit to a transfer.
        expect_ok(link.command("AT"), "keepalive")

        before = link.command("ATI")
        print(f"running : {before}")

        chunk = expect_ok(link.command(f"ATFW={len(image)},{md5}"),
                          "ATFW handshake")
        print(f"transfer: (chunk {chunk} B, ACK-paced)")

        # Device is now in raw mode and counting bytes. Anything we write is
        # firmware payload -- do not send a newline, do not send anything else.
        started = time.monotonic()
        sent = 0
        link.ser.timeout = 10.0
        while sent < len(image):
            n = link.ser.write(image[sent:sent + chunk])
            link.ser.flush()
            sent += n
            # Wait for the device to confirm it has written this chunk before
            # sending more. This is the flow control, not a nicety.
            ack = link.ser.read(1)
            if ack != CHUNK_ACK:
                if not ack:
                    raise ProtocolError(
                        f"no chunk ACK after {sent}/{len(image)} bytes")
                # An early error line means the device rejected the transfer.
                rest = link.ser.read(link.ser.in_waiting or 0)
                raise ProtocolError(
                    (ack + rest).decode(errors="replace").strip())
            progress(sent, len(image), started)
        print()

        # esp_ota_end() + MD5 verify can take a few seconds on a large image.
        link.ser.timeout = max(args.timeout, 30.0)
        expect_ok(link._read_line(), "image verification")
        print("device  : OK, rebooting")

    except ProtocolError as e:
        print(f"\nFAILED: {e}", file=sys.stderr)
        print("The device kept its current image.", file=sys.stderr)
        link.close()
        return 1
    finally:
        if link.ser.is_open:
            link.close()

    if args.no_verify:
        return 0

    # USB-CDC disappears and re-enumerates across the reboot; the port may not
    # exist for a moment. Retry rather than racing it.
    print(f"waiting : {REBOOT_WAIT_S:.0f}s for reboot")
    time.sleep(REBOOT_WAIT_S)
    for attempt in range(5):
        try:
            link = Link(args.port, args.baud, args.timeout)
            after = link.command("ATI")
            link.close()
            print(f"running : {after}")
            if after == before:
                print("\nWARNING: version string unchanged. Either the build "
                      "was identical, or the image failed its health check "
                      "and the bootloader rolled back. Check ATL.",
                      file=sys.stderr)
                return 2
            print("\nUpdate complete.")
            return 0
        except (serial.SerialException, ProtocolError):
            time.sleep(2.0)

    print("\nWARNING: device did not respond after reboot. If it fails its "
          "health check the bootloader will roll back to the previous image "
          "automatically; power-cycle and retry.", file=sys.stderr)
    return 3


if __name__ == "__main__":
    sys.exit(main())
