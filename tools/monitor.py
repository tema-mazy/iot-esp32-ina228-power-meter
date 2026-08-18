#!/usr/bin/env python3
"""Example host-side reader for the battery monitor.

    ./monitor.py /dev/ttyACM0                      # live table on stdout
    ./monitor.py '/dev/cu.usbmodem*' --json        # newline-delimited JSON
    ./monitor.py /dev/ttyACM0 --status-file /run/battery.json
    ./monitor.py /dev/ttyACM0 --http 8080          # GET / -> latest JSON

Import it instead of running it if you want the transport:

    from monitor import Monitor
    with Monitor('/dev/ttyACM0') as m:
        print(m.read())

Requires pyserial. Everything else is stdlib.
"""

import argparse
import glob
import json
import os
import sys
import tempfile
import threading
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial not installed:  pip install pyserial")


class ProtocolError(Exception):
    """Device answered, but not with what was asked for."""


class DeviceError(ProtocolError):
    """Device returned a numbered ERROR response."""

    def __init__(self, code, desc):
        super().__init__(f"ERROR {code} {desc}")
        self.code = code
        self.desc = desc


def resolve_port(pattern):
    """Accept a literal path or a glob.

    The port name is not stable across reboots on macOS, and the device
    reboots on ATZ and after every OTA. Passing a glob such as
    '/dev/cu.usbmodem*' lets the reader find it again by itself.
    """
    if any(c in pattern for c in "*?["):
        matches = sorted(glob.glob(pattern))
        if not matches:
            raise FileNotFoundError(f"no port matches {pattern!r}")
        return matches[0]
    return pattern


class Monitor:
    """Line-oriented client for the AT protocol.

    Reconnects on its own. The device disappears from the bus entirely
    during a reboot, so any operation may raise and be retried.
    """

    def __init__(self, port, baud=115200, timeout=2.0, retry_delay=2.0):
        self.pattern = port
        self.baud = baud
        self.timeout = timeout
        self.retry_delay = retry_delay
        self.ser = None

    # -- connection ----------------------------------------------------------

    def connect(self, retries=None):
        """Open the port, retrying while the device is absent."""
        attempt = 0
        while retries is None or attempt <= retries:
            try:
                path = resolve_port(self.pattern)
                self.ser = serial.Serial(path, self.baud, timeout=self.timeout)
                # The device may still be booting; discard any banner and
                # confirm it answers before declaring the link up.
                time.sleep(0.3)
                self.ser.reset_input_buffer()
                self.command("AT")
                return path
            except (serial.SerialException, OSError, FileNotFoundError,
                    ProtocolError):
                self.close()
                attempt += 1
                if retries is not None and attempt > retries:
                    raise
                time.sleep(self.retry_delay)

    def close(self):
        if self.ser is not None:
            try:
                self.ser.close()
            except Exception:
                pass
            self.ser = None

    def __enter__(self):
        self.connect()
        return self

    def __exit__(self, *exc):
        self.close()

    # -- protocol ------------------------------------------------------------

    def command(self, cmd):
        """Send one command, return its response line.

        Raises DeviceError for a numbered ERROR, ProtocolError for silence.
        """
        if self.ser is None:
            raise ProtocolError("not connected")
        self.ser.reset_input_buffer()
        self.ser.write((cmd + "\r\n").encode())
        self.ser.flush()
        line = self.ser.readline()
        if not line:
            raise ProtocolError(f"no response to {cmd}")
        text = line.decode(errors="replace").strip()
        if text.startswith("ERROR"):
            parts = text.split(None, 2)
            code = int(parts[1]) if len(parts) > 1 and parts[1].isdigit() else 0
            raise DeviceError(code, parts[2] if len(parts) > 2 else "")
        return text

    def info(self):
        """ATI -> dict. Fields: version, built, chip, mac."""
        fields = self.command("ATI").split(",")
        keys = ("version", "built", "chip", "mac")
        return dict(zip(keys, fields))

    def read(self):
        """ATA -> dict of live measurements.

        Keys present today: v, i, p, t, q_c, e_j, err. SoC fields (soc,
        mah_left, state, est) appear once the gauge lands in firmware phase 5,
        so treat them as optional rather than assuming they exist.
        """
        text = self.command("ATA")
        try:
            return json.loads(text)
        except json.JSONDecodeError as e:
            raise ProtocolError(f"ATA returned non-JSON: {text!r}") from e

    def log(self):
        """ATL -> list of buffered device log lines."""
        if self.ser is None:
            raise ProtocolError("not connected")
        self.ser.reset_input_buffer()
        self.ser.write(b"ATL\r\n")
        self.ser.flush()
        header = self.ser.readline().decode(errors="replace").strip()
        try:
            count = int(header.split()[0])
        except (ValueError, IndexError):
            raise ProtocolError(f"unexpected ATL header: {header!r}")
        lines = [self.ser.readline().decode(errors="replace").rstrip()
                 for _ in range(count)]
        self.ser.readline()  # trailing OK
        return lines

    def poll(self, interval=1.0):
        """Yield readings forever, reconnecting as needed.

        Never raises for transport problems: a monitor that dies when the
        device reboots is useless, and this device reboots on every update.
        """
        while True:
            started = time.monotonic()
            try:
                if self.ser is None:
                    self.connect()
                yield self.read()
            except DeviceError as e:
                # The device is talking, it just has nothing valid to report
                # (for example the INA228 is unreachable). Surface it and
                # keep the connection.
                yield {"err": e.code, "error_desc": e.desc}
            except (ProtocolError, serial.SerialException, OSError):
                self.close()
                time.sleep(self.retry_delay)
                continue
            delay = interval - (time.monotonic() - started)
            if delay > 0:
                time.sleep(delay)


# -- output modes ------------------------------------------------------------

def write_status_file(path, reading):
    """Atomic replace, so a reader never sees a half-written file."""
    directory = os.path.dirname(os.path.abspath(path)) or "."
    fd, tmp = tempfile.mkstemp(dir=directory, suffix=".tmp")
    try:
        with os.fdopen(fd, "w") as f:
            json.dump(reading, f)
            f.write("\n")
        os.replace(tmp, path)
    except Exception:
        os.unlink(tmp)
        raise


def format_row(r):
    """One human-readable line. Tolerates missing gauge fields."""
    if r.get("error_desc"):
        return f"  ERROR {r.get('err')}  {r['error_desc']}"
    parts = [
        f"{r.get('v', 0):8.4f} V",
        f"{r.get('i', 0):9.5f} A",
        f"{r.get('p', 0):8.4f} W",
        f"{r.get('t', 0):5.1f} C",
    ]
    if "soc" in r:  # firmware phase 5 onward
        est = "?" if r.get("est") else " "
        parts.append(f"{r['soc']:3d}%{est}")
        if "mah_left" in r:
            parts.append(f"{r['mah_left']:6d} mAh")
        if r.get("state"):
            parts.append(f"{r['state']:<11}")
    else:
        parts.append(f"{r.get('q_c', 0):9.3f} C")
    if r.get("err"):
        parts.append(f"err={r['err']}")
    return "  ".join(parts)


class StatusServer:
    """Minimal HTTP endpoint serving the latest reading as JSON."""

    def __init__(self, port):
        from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
        self.latest = {}
        self.lock = threading.Lock()
        outer = self

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                with outer.lock:
                    body = json.dumps(outer.latest).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *a):
                pass  # do not spam stdout with request lines

        self.server = ThreadingHTTPServer(("", port), Handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def update(self, reading):
        with self.lock:
            self.latest = reading


def main():
    ap = argparse.ArgumentParser(
        description="Poll the battery monitor and report readings")
    ap.add_argument("port",
                    help="serial device, or a glob such as '/dev/cu.usbmodem*'")
    ap.add_argument("-i", "--interval", type=float, default=1.0,
                    help="seconds between readings (default 1.0)")
    ap.add_argument("-b", "--baud", type=int, default=115200)
    ap.add_argument("--json", action="store_true",
                    help="emit newline-delimited JSON instead of a table")
    ap.add_argument("--status-file", metavar="PATH",
                    help="atomically write the latest reading here")
    ap.add_argument("--http", type=int, metavar="PORT",
                    help="serve the latest reading as JSON on this port")
    ap.add_argument("-n", "--count", type=int,
                    help="stop after N readings (default: run forever)")
    args = ap.parse_args()

    mon = Monitor(args.port, args.baud)
    server = StatusServer(args.http) if args.http else None

    try:
        path = mon.connect()
        info = mon.info()
        if not args.json:
            print(f"connected: {path}")
            print(f"firmware : {info.get('version')} ({info.get('built')})")
            print(f"chip     : {info.get('chip')}  mac {info.get('mac')}")
            print()
    except KeyboardInterrupt:
        return 0

    seen = 0
    try:
        for reading in mon.poll(args.interval):
            reading["ts"] = time.time()
            if args.json:
                print(json.dumps(reading), flush=True)
            else:
                print(format_row(reading), flush=True)
            if args.status_file:
                write_status_file(args.status_file, reading)
            if server:
                server.update(reading)
            seen += 1
            if args.count and seen >= args.count:
                break
    except KeyboardInterrupt:
        pass
    finally:
        mon.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
