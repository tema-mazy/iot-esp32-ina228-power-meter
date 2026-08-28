# Integration guide - host API

Complete specification of the interface between a host and the battery monitor.
Everything here is verified against `main/at_cmd.c`, `main/config.c` and
`main/ota.h` rather than restated from the design plan, which can drift.

For what the numbers mean physically, see DEVELOPMENT_PLAN.md S4 (gauge) and
S5.9 (measured behaviour). For wiring, hardware.md.

---

## 1. Transport

| | |
|---|---|
| Default | USB Serial/JTAG, appears as `/dev/ttyACM*` on Linux, `/dev/cu.usbmodem*` on macOS |
| Alternative | UART1, TX GPIO6, RX GPIO7, 115200 8N1, selected at build time |
| Baud | Irrelevant over USB CDC. Any value is accepted and ignored |
| Flow control | None, except during firmware transfer (S5) |

**The port name is not stable.** The device re-enumerates on `ATZ`, after every
OTA, and on any power cycle - which in the shipping topology means every battery
swap, since the monitor is powered from the pack it measures. Match a glob and
re-resolve it on every reconnect. `tools/monitor.py` does this.

**Nothing is emitted unsolicited.** The link is strictly request/response; the
device never speaks first. Device logs are retrieved with `ATL`, never pushed.
A host may therefore assume that any bytes arriving outside a command response
are a protocol error on its own side.

---

## 2. Framing

- One command per line, terminated `\r\n`. A bare `\n` is accepted.
- Maximum line length **256 bytes**. Longer lines get `ERROR 1`.
- Leading and trailing whitespace is stripped. A blank line is **ignored with
  no response at all** - do not wait for one.
- Commands are upper-cased **only up to the first `=`**. Everything after it is
  data: `pack_id` is case-sensitive, chemistry names are compared
  case-insensitively regardless.
- Exactly one response line per command, `\r\n` terminated. `ATL` is the sole
  exception (S4.6).

Success is `OK`. Failure is:

```
ERROR <code> <description>
```

| Code | Constant | Meaning |
|---|---|---|
| 1 | `AT_ERR_SYNTAX` | Malformed line, missing `AT` prefix, or too long |
| 2 | `AT_ERR_UNKNOWN` | Unrecognised command |
| 3 | `AT_ERR_PARAM` | Parameter out of range or unparseable |
| 4 | `AT_ERR_NOCONFIG` | No pack provisioned - send `ATS=` first |
| 5 | `AT_ERR_INA` | INA228 fault: I2C failure, ID mismatch, or no valid reading |
| 6 | `AT_ERR_NVS` | NVS read or write failure |
| 7 | `AT_ERR_OTA` | OTA partition, write, or verify failure |
| 8 | `AT_ERR_TIMEOUT` | Timeout during binary transfer |
| 9 | `AT_ERR_CHECKSUM` | MD5 mismatch, image rejected |

Parse the **code**, not the description. Descriptions are for humans and are
not part of the contract.

---

## 3. Command summary

| Command | Response |
|---|---|
| `AT` | `OK` |
| `ATI` | `<version>,<date> <time>,esp32c3,<mac>` |
| `ATA` | one JSON object |
| `ATS=<chem>,<xSyP>,<mAh>,<Vmin>,<Vmax>,<Imax>[,<pack_id>]` | `OK` \| `ERROR` |
| `ATS?` | current config, same field order |
| `ATR` | `OK` \| `ERROR 3` |
| `ATC=<mAh>` | `OK` \| `ERROR 3` |
| `ATL` | `<n> lines follow`, n lines, `OK` |
| `ATFW=<bytes>,<md5hex>` | `OK <chunk>` then binary phase |
| `ATZ` | `OK`, then reboots after 100 ms |

---

## 4. Commands in detail

### 4.1 `AT` - keepalive

Answers `OK` in under 5 ms. Use it to confirm the link after opening the port;
the device may still be booting and a port that opens is not a device that
answers.

### 4.2 `ATI` - identity

```
ATI
c82f6c1,Aug 26 2026 09:03:15,esp32c3,94A99071387C
```

Fields: firmware version, build date and time, chip, MAC (12 hex, no
separators).

**Version is `git describe` output**, and releases are tagged `vYY.MM-<short
hash>` - the month says roughly when, the hash says exactly what. An untagged
build reports a bare commit SHA; an uncommitted build reports `<sha>-dirty` and
**two different dirty builds are indistinguishable**. Treat a `-dirty` suffix
as "unknown build" for any rollback or inventory decision. `release.sh` refuses
to package a dirty tree for this reason, and verifies the version string is
actually present in the built image before shipping it.

### 4.3 `ATA` - live measurement

Returns one JSON object. Provisioned:

```json
{"v":19.8400,"i":0.15300,"p":3.0355,"t":23.8,"soc":48.3,"mah_left":965,
 "mah_used":1035,"wh":19.15,"v_ocv":19.9832,"r":0.955,"v_full":4.121,
 "state":"discharging","est":false,"q_c":5527.129,"e_j":100102.30,"err":0}
```

| Field | Unit | Notes |
|---|---|---|
| `v` | V | Bus voltage, pack level |
| `i` | A | **Positive = discharge**, current leaving the pack |
| `p` | W | Instantaneous power |
| `t` | deg C | INA228 die temperature, not the pack's |
| `soc` | % | State of charge |
| `mah_left` | mAh | Remaining charge |
| `mah_used` | mAh | Since the last full |
| `wh` | Wh | Remaining energy, `mah_left x v_ocv` |
| `v_ocv` | V | IR-compensated, `v + i x r`. Equals `v` until `r` is learned |
| `r` | ohm | Learned `R_total`. `0` until a load step has been seen |
| `v_full` | V/cell | Learned resting-full voltage. `0.000` until observed |
| `state` | | `charging` \| `discharging` \| `idle` \| `full` \| `unknown` |
| `est` | bool | `true` when SoC came from OCV in a flat stretch, worth +/-10 % |
| `q_c` | C | INA228 **hardware** charge accumulator |
| `e_j` | J | INA228 **hardware** energy accumulator |
| `err` | bitmask | Latched faults; `ATL` explains them in words |

**Unprovisioned, the SoC fields are absent entirely** rather than null:

```json
{"v":19.8400,"i":0.15300,"p":3.0355,"t":23.8,"q_c":5527.129,"e_j":100102.30,"err":0}
```

An unprovisioned device is still a working voltmeter and ammeter. Test for key
presence; do not assume `soc` exists.

> **`q_c` and `e_j` come from silicon, not from sampling.** The INA228
> integrates continuously, so polling faster does not make charge more
> accurate - it only samples `v`, `i` and `p` more often. Measured: on a load
> swinging 97-201 mA the accumulator read 0.1118 A while ten 0.8 s point
> samples averaged 0.1340 A, a 17 % aliasing error. **For anything
> charge-related, difference `q_c` between two readings. Never integrate `i`
> yourself.**

Poll as often as you like; 1 Hz is typical and 60 s is plenty for logging.

### 4.4 `ATS=` - provision the pack

```
ATS=LiIon,5S1P,2000,15.5,20.6,2.5,bp18650
OK
```

| Field | Range | Notes |
|---|---|---|
| `chem` | `LifePo` \| `LiIon` \| `AGM` \| `Acid` | Case-insensitive |
| `xSyP` | `^[0-9]{1,2}S[0-9]{1,2}P$` | e.g. `5S3P` |
| `mAh` | 1 .. 1000000 | Nominal capacity |
| `Vmin` | 0.1 .. 100.0 | Pack level, discharge floor |
| `Vmax` | > `Vmin`, <= 100.0 | Pack level, **charge ceiling** |
| `Imax` | 0.001 .. `0.16384 / Rshunt` | 10.9 A with the stock 15 mOhm shunt |
| `pack_id` | 1-15 of `[A-Za-z0-9_-]` | Optional, defaults to `default` |

Setting `Imax` also sets `CURRENT_LSB = Imax / 2^19` and selects `ADCRANGE`, so
**size it to the load, not to the pack**. `Imax=10` on a 150 mA load makes
resolution 4x coarser and puts 1.5 W into the shunt.

**`Vmax` is the charge ceiling, not the resting-full voltage.** They are close
for lithium and differ by ~1.8 V for lead-acid, which charges at 2.40 V/cell
and rests at 2.14. The resting figure comes from the OCV table, and each pack's
own is learned into `v_full`.

**Set `Vmin` to the voltage at which your system actually stops working**, not
the cell chemistry's floor. Measured on this bench, a Pi's DC-DC drops out at
15.46 V while the cells would go to 13.79 V - so `Vmin=15.0` promises charge no
load can reach, and every time-to-empty projection overruns by the whole tail.

> ! **Re-provisioning discards learned per-pack state** - `v_full`, learned
> capacity - even when the config is byte-identical, because `ATS` re-runs
> `gauge_init` from a zeroed struct. `tools/test_at.py` re-provisions many
> times, so running the test suite against a live pack wipes that pack's
> calibration. Re-issue `ATR` after a full charge to restore it.

`ATS?` returns the same field order, always including `pack_id`:

```
ATS?
LiIon,5S1P,2000,15.50,20.60,2.50,bp18650
```

### 4.5 `ATR` and `ATC=` - correcting the gauge

They are not duplicates.

- **`ATR`** - "new pack, assume full". Sets remaining to capacity and records
  the present IR-compensated voltage as this pack's `v_full`. **Refused with
  `ERROR 3` when the raw OCV table says the pack is below 80 %**, because a
  confident wrong gauge is worse than one that admits ignorance.
- **`ATC=<mAh>`** - "it is exactly this much". Range `0 .. capacity`. For a
  bench-charged pack of known content, or a drift correction that should not
  wait for the next full charge.

**Send `ATR` after the charger finishes, with the pack reconnected and near
rest - but not immediately.** Straight off the charger a cell carries surface
charge and reads 4.15-4.18 V rather than its true ~4.10; anchoring there sets
`v_full` too high and makes SoC read low for the pack's life. Twenty minutes of
rest is enough, and is exactly what the automatic path waits for. If you do
nothing at all, `v_full` is learned by itself the first time the pack sits idle
that long.

### 4.6 `ATL` - device log

The only multi-line response:

```
ATL
3 lines follow
I (787) ina228: found INA228 at 0x40, die 0x228 rev 1
W (977) gauge: no valid voltage at init (0.000 V), deferring seed
I (1007) gauge: seeded 95.9% (1918 mAh) from 4.159 V/cell [deferred seed]
OK
```

Read the count from the header, then exactly that many lines, then the
trailing `OK`. The buffer is **not** drained on read, so `ATL` is idempotent
and a dropped response loses nothing.

### 4.7 `ATZ` - reboot

Replies `OK`, waits 100 ms so the response drains, then resets. The port
disappears and re-enumerates. Expect to reconnect.

---

## 5. Firmware update over the link

```
host -> ATFW=311248,7b817b5f666ea81bc4df31fbad4581f6
dev  -> OK 2048                  <- chunk size for THIS build
host -> <2048 raw bytes>
dev  -> .                        <- one ASCII dot, chunk written, send next
host -> <2048 raw bytes>
dev  -> .
        ... final chunk may be short ...
dev  -> OK                       then reboots into the new image
```

Rules that matter:

1. **Read the chunk size from the `OK` line.** It is announced per build so it
   can change; hardcoding it is how this breaks silently later.
2. **Wait for the `.` before sending the next chunk.** This is real flow
   control, not politeness. The USB Serial/JTAG driver **silently discards** on
   RX-ring overflow - no error, no signal - and an unpaced 278 KB stream lost
   about 146 KB.
3. **Everything rejectable is rejected before the `OK`.** Once `OK` is sent the
   device is in raw binary mode and any error response would be read as an
   answer to a transfer that never started. So a size that does not fit the
   partition fails immediately, not after streaming.
4. **On any failure the device flushes its RX buffer** and answers with a code
   from S2. Leftover payload would otherwise be parsed as commands - the
   symptom was `ERROR 1 line too long` for a 46-character command.
5. **Success reboots the device.** The reply arrives, then the port drops. The
   new image must serve one command and find the INA228 before the rollback
   gate is satisfied; otherwise it rolls back on the next boot.

`release.sh` prints the exact `ATFW=` line for each build, and
`release_notes.md` in the tarball repeats it.

---

## 6. Worked host loop

```python
import glob, json, serial, time

port = sorted(glob.glob('/dev/cu.usbmodem*'))[0]   # re-resolve after reboots
ser  = serial.Serial(port, 115200, timeout=2.0)
time.sleep(0.3)                                     # may still be booting
ser.reset_input_buffer()

def cmd(s):
    ser.reset_input_buffer()
    ser.write((s + '\r\n').encode())
    ser.flush()
    line = ser.readline().decode(errors='replace').strip()
    if not line:
        raise TimeoutError(f'no response to {s}')
    if line.startswith('ERROR'):
        raise RuntimeError(line)
    return line

cmd('AT')
r = json.loads(cmd('ATA'))
if 'soc' in r:                      # absent when unprovisioned
    print(f"{r['soc']:.1f}%  {r['mah_left']} mAh  {r['v']:.3f} V  {r['i']*1000:.0f} mA")
else:
    print(f"unprovisioned: {r['v']:.3f} V  {r['i']*1000:.0f} mA")
```

`tools/monitor.py` is a fuller version: reconnects on its own, tolerates the
device vanishing mid-poll, and can emit JSON, a table, a status file or HTTP.
Import it rather than reimplementing:

```python
from monitor import Monitor
with Monitor('/dev/cu.usbmodem*') as m:
    print(m.read())
```

---

## 7. Behaviour a host must tolerate

- **The device disappears.** `ATZ`, OTA, and every battery swap re-enumerate
  it. Treat any I/O error as "retry after re-resolving the port", not fatal.
- **`v_full` and `r` read `0` until learned.** Neither is an error. `r` needs a
  load step of at least 30 mA; `v_full` needs the pack declared full or 20
  minutes at rest.
- **`soc` can jump.** On reconnect the gauge compares against the stored
  voltage and, if the pack moved more than 50 mV/cell while it was off,
  re-seeds from OCV rather than trusting the stored count. That is correct
  behaviour, not drift.
- **`est:true` means +/-10 %.** Do not present those readings with the same
  confidence as counted ones.
- **A pack at 100 % SoC may still refuse to start your load.** Internal
  resistance rises as the pack empties, so surge headroom disappears before
  steady-state headroom does: measured, a Pi could no longer boot at ~10 % SoC
  while steady draw continued happily to 3 %. If your load has an inrush,
  reserve accordingly and do not size purely on `mah_left`.
- **Time-to-empty is a host concern.** The device never emits a projection;
  `ATA` carries measurements and state only. The `pred` object in this repo's
  logs is added host-side by `tools/predict.py`.
