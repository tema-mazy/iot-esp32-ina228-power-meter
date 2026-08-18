# Universal Battery Monitor - ESP32-C3 + INA228

A self-contained battery fuel gauge that mounts **at the battery** and reports over USB-C to any host.

It is **battery-agnostic and host-agnostic**: chemistry, cell count, capacity and voltage limits are set at runtime with a single `ATS` command and stored in NVS - nothing about the battery is compiled in. It powers itself from the battery it measures, so it keeps counting whether or not anything is plugged into it.

```
battery --> [INA228] --+--> the load (Raspberry Pi, or anything else)
                       +--> buck 9-36V->5V --> ESP32-C3 --USB-C--> host
```

**Why coulomb counting.** Resting voltage alone cannot measure capacity on a flat discharge curve. The INA228 accumulates charge **in hardware** (20-bit, 85 V bus, +/-10.9 A), the ESP32 integrates it, persists to NVS, and re-syncs from open-circuit voltage on every power-up.

## Documentation

| Document | Contents |
|---|---|
| **[hardware.md](hardware.md)** | **Wiring, BOM, pinout, bring-up.** Start here to build one - S2.1 is the complete wiring list |
| [DEVELOPMENT_PLAN.md](DEVELOPMENT_PLAN.md) | Firmware architecture, AT protocol spec, gauge algorithms, phased build |
| [battery-monitor-handoff.md](battery-monitor-handoff.md) | Original design rationale (Pi-specific; superseded where the two disagree) |

## Hardware at a glance

See **[hardware.md](hardware.md)** for the authoritative detail.

| | |
|---|---|
| Sensor | Adafruit **INA228** breakout (PN 5832), 15 mOhm shunt, I2C `0x40` |
| MCU | **ESP32-C3 SuperMini** - native USB-Serial/JTAG, 4 MB flash |
| Supply | 9-36 V buck -> SuperMini `5V` pin (self-powered from the battery) |
| Host link | One USB-C cable, **VBUS wire cut** -> `/dev/ttyACM0` |
| Draw | ~7 mA while connected; nothing in storage |

**Pinout:** SDA `GPIO4` . SCL `GPIO3` . ALERT `GPIO5` . LED `GPIO8` . button `GPIO9` (dev) / `GPIO10` (production) . debug UART0 `GPIO21/20`

Two things that fail silently if you skip them:
- **Bridge `VBus` to `VIN+`** on the breakout - otherwise current reads correctly and voltage reads zero ([hardware.md S3.1.1](hardware.md))
- **All load current must pass through `VIN+ -> VIN-`** - any second connection to battery positive is invisible to the gauge

## Serial protocol

115200 8N1 (or USB-CDC), one command per line, `\r\n`. Strictly request/response - the device never speaks first.

| Command | Response | Purpose |
|---|---|---|
| `AT` | `OK` | Keepalive |
| `ATI` | version, git sha, build, chip, MAC | Identity |
| `ATS=<chem>,<xSyP>,<mAh>,<Vmin>,<Vmax>,<Imax>` | `OK` | Provision the battery |
| `ATS?` | current config | Read back |
| `ATA` | JSON | Live V, I, P, SoC, mAh |
| `ATL` | log lines | Recent log ring buffer |
| `ATR` | `OK` | Declare a battery swap |
| `ATC=<mAh>` | `OK` | Force remaining capacity |
| `ATZ` | `OK`, reboot | Restart |
| `ATFW=<bytes>,<md5>` | `OK <chunk>`, binary, `OK` | OTA update (ACK-paced) |

`chem` ? `LifePo` `LiIon` `AGM` `Acid` - selects the open-circuit-voltage table used for SoC seeding.

```
ATS=LiIon,5S3P,12000,15.0,21.0,10.0     # 18 V 12 Ah 21700 pack
ATA
{"v":19.84,"i":1.42,"p":28.2,"t":31.4,"soc":73,"mah_left":8760,
 "mah_used":3240,"wh":22.4,"state":"discharging","est":false,"err":0}
```

Errors are `ERROR <code> <description>`; codes are listed in [DEVELOPMENT_PLAN.md S2](DEVELOPMENT_PLAN.md).

## User interface

One controllable LED (blue, GPIO8) and one button. All feedback is **blink counts**, never colour - the LED is monochrome and varies by board.

| Action | Response |
|---|---|
| Double press | 2 fast blinks, then N slow blinks (N = SoC/10) |
| Long press >=5 s | 5 blinks - "new battery, assume full" |
| Refused | 10 rapid blinks - battery is not actually full |
| INA228 fault | continuous 0.5 s blink |

Red and green LEDs on the two boards are hardwired power indicators, useful for bring-up diagnosis ([hardware.md S4.3](hardware.md)).

## Build

Requires ESP-IDF >= 5.0.

```sh
. $IDF_PATH/export.sh
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

Configure pins and hardware parameters under `Battery Monitor` in `idf.py menuconfig`.

### Flashing vs. the AT link - both use the same USB port

The USB Serial/JTAG peripheral is implemented in **ROM**, so on every reset the bootloader owns the port before the application runs. Flashing therefore works no matter what the firmware does with USB, and survives a hung image - one of the reasons this peripheral was chosen over a USB-UART bridge.

| Situation | Action |
|---|---|
| Normal | `idf.py -p PORT flash` - auto-reset over USB-SJ |
| Firmware wedged, auto-reset fails | Hold **BOOT** (GPIO9), tap **RESET**, release BOOT, then flash |
| Routine updates | Skip flashing - use `fwupdate.py` over the AT link |

! **The console currently shares USB with flashing** (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`), which is convenient for Phase 0-1. **From Phase 2 the AT link claims USB**, so the console must move to `CONFIG_ESP_CONSOLE_UART_DEFAULT` (GPIO21/20 debug header) or `CONFIG_ESP_CONSOLE_NONE` for shipping builds, where logs reach the ring buffer only and surface via `ATL`. Flashing is unaffected by either.

### OTA updates

Subsequent updates go over the serial link, no cable swap:

```sh
./tools/fwupdate.py /dev/ttyACM0 build/power_meter.bin
```

Roughly 3 seconds for a 280 KB image. The transfer is **ACK-paced**: the device replies `OK <chunk>` to the handshake and emits one `.` per chunk written, and the host waits for it. This is not optional - the USB Serial/JTAG driver silently discards data on RX-ring overflow, so a free-running stream loses about half the image with no error reported. See [DEVELOPMENT_PLAN.md S2.5](DEVELOPMENT_PLAN.md).

Images boot as `PENDING_VERIFY` and must pass a health check (INA228 probe + one served command) before the bootloader keeps them - a bad update rolls back rather than bricking the unit.

## Reading it from a host

```sh
./tools/monitor.py /dev/ttyACM0                      # live table
./tools/monitor.py '/dev/cu.usbmodem*' --json        # newline-delimited JSON
./tools/monitor.py /dev/ttyACM0 --status-file /run/battery.json
./tools/monitor.py /dev/ttyACM0 --http 8080          # GET / -> latest JSON
```

Or import it:

```python
from monitor import Monitor
with Monitor('/dev/ttyACM0') as m:
    print(m.read())
```

It reconnects on its own. The device disappears from the bus during a reboot
(ATZ, and after every OTA), so a reader that dies on a dropped port is
useless. Pass a glob rather than a fixed path: the port name is not stable
across reboots on macOS.

`tools/battery-monitor.service` is a systemd example.

## Tests

```sh
./tools/test_at.py /dev/ttyACM0                             # protocol only
./tools/test_at.py /dev/ttyACM0 --ota build/power_meter.bin # + full OTA
```

39 tests covering keepalive, error handling, identity, measurement (including a sanity check on die temperature, which catches a wrong DIETEMP scaling), the log ring buffer, `ATFW` input validation, MD5 rejection and a real update. Run it after any protocol change.

## Status

| Phase | State |
|---|---|
| 0 Scaffold | done |
| 1 INA228 driver | done - bench checkpoint (V/I vs DMM) pending |
| 2 Transport + log buffer | done |
| 3 AT core | done |
| 4 `ATS` provisioning + NVS | next |
| 5 Fuel gauge | not started |
| 6 OTA | done (built early) |
| 7 Host-side reader | done |

`ATA` currently reports live V, I, P, temperature, charge and energy. SoC fields arrive with the gauge in Phase 5 - they are omitted rather than emitted as nulls, so nothing can bind to values that do not yet mean anything.

See [DEVELOPMENT_PLAN.md S8](DEVELOPMENT_PLAN.md) for detail.

## Safety

This is a **capacity gauge, not a protection circuit**. It measures; it does not intervene. The battery still needs its own BMS for cell balancing and over-charge / over-discharge / short protection. Fuse the positive lead.

## License

See [LICENSE](LICENSE).
