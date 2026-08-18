# Session Handoff - 2026-08-18 - Firmware phases 2, 3, 6, 7; test suite; ASCII cleanup

Supersedes the 2026-08-13 handoff (design + phases 0/1), which is in git
history at commit a86c177.

## What was done

Commit `a86c177` (pushed to origin/main, 29 files, 3555 insertions):

- Transport layer: `main/link.h` plus `link_usb.c`, `link_uart.c`,
  `link_common.c`. Writes use a finite timeout and discard on expiry so a host
  that is not draining the FIFO cannot stall the gauge.
- `main/logbuf.c`: capture-only log ring buffer. Console is disabled entirely
  (`CONFIG_ESP_CONSOLE_NONE`); logs exit only via ATL.
- `main/at_cmd.c`: AT / ATI / ATA / ATL / ATZ / ATFW, numbered error codes.
- `main/ota.c`: ATFW receiver, streaming MD5, rollback gated on a health check
  (INA228 probe plus one served command), not merely on booting.
- `main/led.c`: blink-count vocabulary.
- `tools/test_at.py`: 39 tests. Found four real defects on first run.
- All files converted to pure ASCII (see below).

Phase 7 host reader, tested on hardware:

- `tools/monitor.py` - modes: table, `--json`, `--status-file` (atomic replace
  via `os.replace`), `--http PORT`. Importable as `Monitor`. Reconnects on its
  own; verified by sending ATZ mid-poll (one ERROR 5 during the reboot window,
  then it resumed unattended).
- `tools/battery-monitor.service` - systemd example.

## What was successful

- **The test suite paid for itself immediately.** Four real bugs on the first
  run, two of which manual spot-checks had "passed" for the wrong reason. Run
  it after every protocol change:
  `./tools/test_at.py /dev/cu.usbmodem1101 --ota build/power_meter.bin`
- **Reading the TI datasheet rather than trusting memory.** `WebFetch` on
  https://www.ti.com/lit/ds/symlink/ina228.pdf returns unparsed binary but
  saves the PDF locally; `Read` with `pages: "22-31"` renders it. Caught that
  SHUNT_CAL is a 15-bit field, not 16.
- **Building OTA early (phase 6 before 4 and 5).** Every later phase now
  flashes over the AT link instead of the cable.
- **Making ATA omit SoC fields rather than emit nulls.** Nothing can bind to
  values that do not mean anything yet.
- **Reporting unimplemented commands as "not implemented in this phase"**
  rather than "unknown command", so a host can tell wrong-firmware from typo.
- Capturing serial non-interactively with the IDF python env:
  `. $IDF_PATH/export.sh && python -c "import serial; ..."`. `idf.py monitor`
  is interactive; system python3 lacks pyserial.

## What went wrong - do NOT repeat

### The USB Serial/JTAG flow-control trap

**The driver silently discards data when its RX ring overflows.** No error, no
backpressure. Host writes at ~1 MB/s, device drains at flash-write speed
(~150 kB/s), so a free-running 278 KB OTA stream lost ~146 KB and then timed
out waiting for bytes that no longer existed.

Fixed with a per-chunk ACK, but the constraint cost an extra debug cycle:
**OTA_CHUNK must be smaller than rx_buffer_size** (now 2048 vs 4096). The ACK
paces the host BETWEEN chunks, not within one, so an oversized chunk overflows
the ring mid-chunk. Signature: a timeout reporting **exactly rx_buffer_size
bytes received**. Documented at `main/ota.h:20` and `main/link_usb.c:13`.

### Other bugs the suite caught

1. **ERROR sent after OK on oversize ATFW.** The host follows the spec, sees
   OK, starts streaming, and the device is not listening. Validation now
   precedes the ack (`ota_check_size`, called from `main/at_cmd.c:cmd_atfw`).
2. **The command line's LF became the first firmware byte.** The parser
   dispatched on CR, leaving LF queued; `ota_receive` read 0x0A as byte one and
   every image was shifted. `esp_ota_write` rejected the header with
   ESP_ERR_OTA_VALIDATE_FAILED, which reads like a corrupt image rather than a
   parser bug. Fix: dispatch on LF, discard CR (`main/at_cmd.c:at_task`).
3. **No RX flush after a failed transfer.** Leftover payload was parsed as AT
   commands; symptom was a bogus "ERROR 1 line too long" for a 46-char command.
   Fix: `link_flush_rx()`.

### Process mistakes

- **A manual test passed for the wrong reason.** "Corrupted image rejected" was
  caught by `esp_ota_end` because bug 2 had shifted the header, not by the MD5
  check being tested. Manual spot-checks gave false confidence; the scripted
  suite did not. Prefer a test that asserts WHICH layer rejected something.
- **Two `python3 - <<EOF` heredoc edits silently failed.** One had no assertion
  after `str.replace`; another contained a `b'''` bytes literal with non-ASCII
  and raised SyntaxError while the surrounding `&&` chain still printed "ok".
  **Always assert after replace and verify with grep, not the exit code.**
- **Non-ASCII crept into every doc and source file.** The user had already
  stated ASCII-only via the handoff skill; I applied it only to handoff.md and
  left emoji, em dashes, arrows, section signs and box drawing everywhere else.
  Now enforced globally in `~/.claude/CLAUDE.md` and in memory as
  `ascii-only-output`. Check with `grep -rlP '[^\x00-\x7F]'` before finishing.
  Note U+2212 (minus) is not a hyphen and became `?` in pin names such as
  `VIN-` during transliteration; those were repaired by hand.
- **Inferred a resistor value from Ohm's law without asking what the load was.**
  Computed "5.2764 V / 1.30 mA implies ~4.06 kOhm, so a 3.9k or 4.3k part".
  The load was actually a **device with its own battery being charged from a
  USB port**, so 1.3 mA was charge-termination trickle current, not a resistive
  load. Ask what is connected before deriving component values from it.

## Current state

Branch `main`, tracking `origin/main`. Commit `a86c177` pushed. `tools/`
additions from phase 7 committed on top (see git log).

Phases 0, 1, 2, 3, 6, 7 done and running on hardware. Phases 4 and 5 not
started.

Hardware on the bench: ESP32-C3 SuperMini plus INA228, powered from USB only,
no buck and no battery fitted. `/dev/cu.usbmodem1101`.

Bench observations, consistent and repeatable across three connect/disconnect
cycles and a reboot:

- 5 V source present: V=5.2771, I=1.28 mA, P matches V*I, dQ/dt matches I.
  The "load" was a USB-charged device with its own battery, so 1.28 mA was
  charge-termination current, not a resistor.
- Source removed: exponential decay, tau ~9 s, settling to exactly 0.0000 V
  (14 of 30 samples read 0.0000, trend -0.08 mV over 30 s). Not backflow - a
  current source would hold a floor and never reach zero. The load device
  evidently has reverse blocking in its charge path.
- **Current channel keeps a fixed -0.13 mA offset with the input open.** That
  is ~2 uV referred to the shunt, ordinary INA228 input offset, but a fuel
  gauge integrates it: -3.1 mAh/day, about -0.8 %/month on a 12 Ah pack.
  Voltage settles to true zero; current does not. Different zero behaviour
  because VBUS is single-ended and current is differential across the shunt.

## Next steps

1. Mark phase 7 done in README.md and DEVELOPMENT_PLAN.md S8 (the phase table
   in README still lists it as not started).
2. Add to the phase 5 plan, both discovered on the bench this session:
   - **Zero-current offset calibration.** Capture current with the load known
     off and subtract thereafter. There is no INA228 offset register, so it is
     a software correction persisted to NVS. Trigger explicitly at provisioning
     time, or automatically after a long idle stretch at flat voltage.
   - **Load-capacitance backflow on disconnect.** The load's bulk caps drain
     backwards through `VIN- -> shunt -> VIN+` and register as charging. Small,
     but one-directional, and this workflow disconnects the pack regularly.
3. Phase 4: `ATS` / `ATS?`, validation, NVS persistence, recompute SHUNT_CAL
   from the real Imax. A realistic Imax also improves the +/-4 % current noise
   seen at mA loads; below 2.7 A it selects ADCRANGE=1 for 4x resolution.
4. Phase 5: gauge. Chemistry-selected OCV tables, coulomb counting, anchors,
   IR compensation.
5. Finish the phase 1 checkpoint: V and I against a DMM within 1 %, and confirm
   the current sign with a real load (positive = discharge).
6. Still untested: the S1.2 blocking-write trap. Leave the host disconnected
   for 10 minutes and confirm the gauge keeps running.

Build and flash:

```
. $IDF_PATH/export.sh                              # IDF 5.5.1 at ~/esp/5.5.1
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash               # first time
./tools/fwupdate.py /dev/cu.usbmodem1101 build/power_meter.bin   # thereafter
./tools/test_at.py  /dev/cu.usbmodem1101 --ota build/power_meter.bin
./tools/monitor.py  '/dev/cu.usbmodem*'            # live readings
```

## Open questions / risks

- **Pack chemistry is still inference, not measurement.** BL18120, 18 V, 12 Ah,
  21700 is read as 5S3P Li-ion NMC because LiFePO4 cannot make 18 V nominal
  with an integer cell count. Charge it fully and measure: ~21 V confirms;
  ~18.2 V would mean LiFePO4 and every threshold in `hardware.md` S7 is wrong.
- **Multiple rotating packs?** A Makita-format battery implies an ecosystem. If
  several will be used, `ATS` needs a `pack_id` field NOW - adding it later is
  a breaking protocol change.
- **Buck converter still unresolved.** Read the printed input range on the
  DD4012SA and STL6118A; if either clears ~30 V it can be the ESP32 supply.
  mini360 (23 V abs max), mini560 (20 V max) and XL6009E1 (boost, wrong
  topology) are all ruled out for a 21 V pack.
- **OCV tables are typical values, not measured.** All four chemistries in
  `DEVELOPMENT_PLAN.md` S4 need trimming against the real pack in phase 5.
- **VBus-to-VIN+ solder link** must be confirmed present on the breakout. A
  floating VBus gives correct current and zero volts - a silent failure. It
  also looks like the decaying reading seen on the bench this session, so do
  not confuse the two.
