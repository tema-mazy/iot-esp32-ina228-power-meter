# Session Handoff - 2026-08-13 - Universal battery monitor: design + Phase 0/1 firmware

## What was done

Turned a one-paragraph project sketch into a full design plus working Phase 0
and Phase 1 firmware, running on real hardware.

Documents written (all new except README):

- `DEVELOPMENT_PLAN.md` (560 lines) - firmware architecture, AT protocol spec,
  INA228 register/constant table, fuel gauge algorithms, LED/button UI,
  phased build 0-8.
- `hardware.md` (617 lines) - BOM, block diagram, complete wiring list (S2.1),
  assembly order (S2.2), pin map, bring-up checklist, power budget, safety.
- `README.md` - rewritten. Project overview, protocol summary, build/flash.
- `tools/fwupdate.py` (175 lines) - OTA client for the ATFW protocol.
  Syntax-checked only; never run against hardware (firmware does not exist yet).

Firmware:

- `partitions.csv` - replaced the leftover canspeed table. Dual OTA, no factory
  partition, nvs bumped 16K -> 24K for gauge write wear.
- `CMakeLists.txt` - project renamed canspeed -> power_meter.
- `sdkconfig.defaults` - 4MB flash, custom partitions, OTA rollback, 80 MHz CPU,
  radios off, console on USB Serial/JTAG.
- `main/Kconfig.projbuild` - every pin and hardware parameter is a config
  option. Nothing about the board or battery is hardcoded.
- `main/main.c` - boot identity report, LED helpers, INA228 poll loop.
- `main/ina228.h` / `main/ina228.c` - full INA228 driver.

Verified on hardware (ESP32-C3 SuperMini at /dev/cu.usbmodem1101):

```
found INA228 at 0x40, die 0x228 rev 1
cal: Imax=10.00A Rshunt=15000 uOhm -> CURRENT_LSB=19.0735 uA, SHUNT_CAL=3750, ADCRANGE=0
V=0.0004  I=-0.00013  P=0.0000  T=25.8C  Q=-0.001C
```

## What was successful

- **Reading the datasheet instead of trusting memory.** `WebFetch` on
  https://www.ti.com/lit/ds/symlink/ina228.pdf returned unparsed binary, BUT it
  saved the PDF locally and the `Read` tool with `pages: "22-31"` rendered it
  fine. That is the working recipe for TI datasheets. It caught a real error
  (see below).
- **Die temperature as the first bring-up measurement.** T=25.8C is the only
  meaningful reading available with no battery attached, and it validates the
  I2C path, the 16-bit signed read, and the 7.8125 mC/LSB constant in one shot.
- **Making everything runtime-configurable via ATS.** Chemistry, cell count,
  capacity and voltage limits are provisioning data, not build constants. This
  repeatedly de-blocked decisions (5S vs 6S stopped mattering to firmware).
- **Encoding all LED status in blink counts, never colour.** Survived the
  discovery that the board has only one controllable LED, and that two of the
  three LEDs are hardwired power indicators.
- **Capturing serial output non-interactively** with the IDF python env:
  `. $IDF_PATH/export.sh && python -c "import serial; ..."`. `idf.py monitor`
  is interactive and unusable here. System python3 lacks pyserial; the IDF env
  has it.

## What went wrong - do NOT repeat

- **Assumed LiFePO4 for ~8 exchanges.** The handoff doc said "18 V LiFePO4".
  LiFePO4 is 3.2 V/cell so it can only make 16.0 V (5S) or 19.2 V (6S) - never
  18.0 V. The pack (BL18120, 21700, 216 Wh) is **5S3P Li-ion NMC**, 21.0 V full.
  Lesson: check that nominal voltage is reachable with an integer cell count
  before accepting a stated chemistry. This invalidated all voltage thresholds
  and, more importantly, the premise that voltage cannot indicate SoC.
- **Said the Adafruit 5832 routes VBus internally from VIN+.** It does not -
  VBus is a separate unpopulated pad. Left floating, **current reads correctly
  and voltage reads zero**, which is a silent failure. Fix: solder-link VBus to
  VIN+. Recorded in `hardware.md:3.1.1`.
- **Over-sold Kelvin sensing.** Pushed a 2-fuse/2-lead separate VBus sense wire
  before doing the arithmetic. It buys ~2 mV against a ~25 mV floor set by
  electrochemical relaxation, because cell internal resistance (30-60 mOhm)
  dwarfs lead resistance (~10 mOhm) and no wiring reaches it. User pushed back
  correctly. Verdict and reasoning in `hardware.md:3.1.2`.
- **Assigned ESP32 UART pins GPIO14/15.** Those are the *Raspberry Pi header*
  numbers from the handoff doc; on the ESP32-C3, GPIO14/15 are SPI flash pins
  and unusable. Correct pins are GPIO6/7. See `hardware.md:4.2`.
- **Over-weighted the GPIO9 BOOT-button strapping risk.** Wrote several
  paragraphs about the chip entering download mode if the button is held during
  power-up. User: "noone will hold this button." Correct - it is a two-handed
  connector insertion. Trimmed to one line.
- **Reversed the power topology twice.** Went self-powered -> host-powered ->
  self-powered as the scope changed from "Pi accessory" to "universal monitor".
  Wasted effort. Lesson: pin down whether the device must function with no host
  attached BEFORE specifying any power path.
- **`git add -A` staged the entire `build/` directory** - there was no
  `.gitignore`. Added one. Always check for it before staging in a fresh repo.
- **Wrote `SHUNT_CAL` as a 16-bit field.** It is 15-bit (bits 14-0); bit 15 is
  reserved. Driver now bounds-checks against 0x7FFF, `main/ina228.c:99`.

## Session 2 addendum - 2026-08-18 - Phases 2, 3, 6 plus a test suite

Built the transport layer, AT core and OTA, then wrote `tools/test_at.py`
(39 tests). The suite found four real defects on its first run. All 39 pass now.

New firmware files, all in `main/`: `link.h`, `link_usb.c`, `link_uart.c`,
`link_common.c`, `logbuf.c/h`, `led.c/h`, `at_cmd.c/h`, `ota.c/h`.

### Bugs the test suite caught - the valuable part

1. **ERROR sent after OK on oversize ATFW.** The host follows the spec, sees
   OK, starts streaming binary, and the device is not listening. Size checks
   now run before the acknowledgement (`ota_check_size`, `main/at_cmd.c`).
2. **The command line's LF became the first firmware byte.** The parser
   dispatched on CR, leaving LF queued; `ota_receive` then read 0x0A as byte
   one and the whole image was shifted. `esp_ota_write` rejected the header
   with ESP_ERR_OTA_VALIDATE_FAILED, which reads like a corrupt image rather
   than a parser bug. Fix: dispatch on LF, discard CR (`main/at_cmd.c:at_task`).
3. **No RX flush after a failed transfer.** Leftover payload was parsed as AT
   commands. Symptom was a bogus "ERROR 1 line too long" for a 46-char command.
   Fix: `link_flush_rx()`.
4. **No flow control at all** - the important one, see below.

### The USB Serial/JTAG flow-control trap - do NOT forget this

**The driver silently discards data when its RX ring overflows.** No error, no
backpressure. Host writes at ~1 MB/s, device drains at flash speed (~150 kB/s),
so a free-running 278 KB stream lost ~146 KB and then timed out waiting for
bytes that no longer existed.

Fix is a per-chunk ACK, with a constraint that cost an extra debug cycle:
**OTA_CHUNK must be smaller than rx_buffer_size** (now 2048 vs 4096). The ACK
paces the host BETWEEN chunks, not within one, so an oversized chunk overflows
the ring mid-chunk. Signature: a timeout reporting **exactly rx_buffer_size
bytes received**. Documented at `main/ota.h:20` and `main/link_usb.c:13`.

### Also worth not repeating

- An earlier manual test, "corrupted image rejected", **passed for the wrong
  reason** - it was caught by `esp_ota_end` because bug 2 had shifted the
  header, not by the MD5 check being tested. Manual spot-checks gave false
  confidence; the scripted suite did not.
- Two `python3 - <<EOF` heredoc edits silently failed: one had no assertion
  after `str.replace`, another had a `b'''` bytes literal containing non-ASCII
  and raised SyntaxError while the surrounding `&&` chain still reported "ok".
  **Always assert after replace, and check the grep output, not the exit code.**
- `sdkconfig.defaults` now sets `CONFIG_ESP_CONSOLE_NONE`. Logs reach the ring
  buffer only and come out via ATL. This works well but means a boot panic is
  invisible; use the GPIO21/20 debug header if something dies early.

### Verified working on hardware

- INA228 at 0x40, die 0x228 rev 1. With a 5 V source and ~1.3 mA load:
  V=5.2764, I=0.00130, P matches V*I, dQ/dt matches I, dE/dt matches P.
  Current sign confirmed positive = discharge (VIN+ to VIN-).
- OTA: 278768 bytes in 2.9 s, device reboots and answers.

## Current state

Branch `main`, one commit (`5afead2 Initial commit`). **Everything is still
uncommitted** - all source, all docs, `.gitignore`, both tools. The user has
not asked for a commit.

Phases 0, 1, 2, 3 and 6 are done and running on hardware. Phase 4 (ATS
provisioning + NVS) is next; Phase 5 (gauge) after it. `ATA` currently reports
live V/I/P/temp/charge/energy with no SoC fields - deliberately omitted rather
than emitted as nulls.

Hardware is soldered and on the bench, powered from USB only (no buck, no
battery). Phase 1 firmware is flashed and running.

Phase 1 is functionally complete but its checkpoint is NOT met: V and I have
not been checked against a DMM, because that needs battery and load.

Known temporary setting: `sdkconfig.defaults` has
`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` so logs arrive over the flashing cable.
**This must change at Phase 2**, when the AT link claims USB. Switch to
`CONFIG_ESP_CONSOLE_UART_DEFAULT` (GPIO21/20 debug header) or
`CONFIG_ESP_CONSOLE_NONE` for shipping. Flagged in the file and in README.

## Next steps

0. Run `./tools/test_at.py /dev/cu.usbmodem1101 --ota build/power_meter.bin`
   before and after any protocol change. 39/39 is the baseline.
1. Finish the Phase 1 checkpoint: attach buck + battery + a known resistive
   load, confirm V and I match a DMM within 1 percent, and **confirm the current
   sign** (positive = discharge, `DEVELOPMENT_PLAN.md:2.3`). If inverted, VIN+
   and VIN- are swapped.
2. Confirm the VBus-to-VIN+ solder link exists. A zero-volt reading with correct
   current is the signature of it missing.
3. Determine whether the DD4012SA or STL6118A buck modules clear ~30 V input
   (read the silkscreen). If one does, no purchase needed. mini360 (23 V abs
   max), mini560 (20 V max) and XL6009E1 (boost, wrong topology) are all ruled
   out for a 21 V pack.
4. Phase 4: `ATS` / `ATS?`, parameter validation, NVS persistence, and
   recomputing SHUNT_CAL from the real Imax. Note `SHUNT_CAL` is a 15-bit
   field - `ina228_set_calibration` already bounds-checks it. A realistic Imax
   also improves the +/-4% current noise seen at mA-level loads, and below
   2.7 A selects ADCRANGE=1 for 4x finer resolution.
5. Phase 5: gauge. Chemistry-selected OCV tables, coulomb counting, anchors,
   IR compensation. Phase 7: host-side reader. Phase 8 (sleep) is documented
   but deliberately not planned - the monitor is never stored on a battery.
6. Still untested: the S1.2 blocking-write trap. Leave the host disconnected
   for 10 minutes and confirm the gauge keeps running.

Build and flash:

```
. $IDF_PATH/export.sh          # IDF 5.5.1 at ~/esp/5.5.1
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash
```

## Open questions / risks

- **Verify the pack is Li-ion, not LiFePO4.** Charge fully and measure: ~21 V
  confirms 5S Li-ion; ~18.2 V would mean LiFePO4 and every threshold in
  `hardware.md:7` is wrong. This is inference from the naming and cell format,
  not a datasheet.
- **SuperMini pad order is assumed** to run 5V, GND, 3V3, GPIO4, GPIO3 down the
  left edge. The GPIO3/GPIO4 I2C assignment exists purely so the QT cable's four
  wires land on consecutive pads. Clones vary; if yours differs, keep the
  principle (SCL adjacent to SDA), not the pin numbers.
- **Multiple rotating packs?** A Makita-format battery implies an ecosystem. If
  several will be used, `ATS` needs a `pack_id` field NOW - adding it later is a
  breaking protocol change. The per-pack ID chip can come later.
- **The OCV tables are typical values, not measured.** All four chemistries in
  `DEVELOPMENT_PLAN.md:4` need trimming against the real pack in Phase 5 by
  resting it at known coulomb-counted states.
- **`tools/fwupdate.py` has never been executed.** Needs pyserial and Phase 6
  firmware.

Worth remembering permanently (user preferences observed this session):
prefers minimal component count and pushes back on complexity that is not
justified by numbers; wants the arithmetic before the recommendation.
