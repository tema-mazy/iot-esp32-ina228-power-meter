# Pi 5 LiFePO4 Battery Monitor - Design Handoff

**Status:** Architecture decided, ready to build firmware. A few pack-specific parameters still open (see S9).
**Date:** 2026-08-13

---

## 1. Goal

Monitor the remaining capacity ("fuel gauge") of an **18 V LiFePO4** pack that powers a **Raspberry Pi 5** through a DC-DC converter. Needs a trustworthy state-of-charge readout, not just a voltage number.

## 2. Core constraint that shapes everything

- **The Pi 5's 40-pin header has no analog input.** Every GPIO is digital-only and 3.3 V-tolerant only. An analog voltmeter cannot connect directly; feeding pack voltage into a GPIO would destroy the pin. All sensing must go through a digital I2C/SPI monitor chip.
- **LiFePO4 has a flat discharge curve.** Resting voltage only indicates SoC near the two "knees" (~full and ~empty). Across ~80% of the usable range, voltage is nearly constant, so **voltage alone cannot measure capacity**. The real measurement must be **coulomb counting** (integrate current over time = Ah in/out).

These two facts drive every decision below.

## 3. Chosen architecture

A small microcontroller sits at the sensor and owns the coulomb count; the Pi just reads the result.

```
18V LiFePO4 (+) --[ INA228: VIN+ |shunt| VIN- ]--+--> DC-DC (18->5V)  --> Pi 5
                                                 +--> DC-DC (18->3V3) --> ESP32-C3
                     (INA228 measures ALL current)     |  VS (chip supply) from 3V3
                                                   I2C -+--> reads INA228
       battery (-) --------------- common ground --------------- Pi GND
   ESP32-C3 -- serial link (UART or data-only USB) --> Pi
```

- **INA228** on the **high side, before both DC-DCs**, so it captures total system draw (Pi + ESP32 + everything).
- **ESP32-C3** reads the INA228 over I2C, runs the fuel-gauge logic, persists the count to its own flash (NVS), and streams the result to the Pi as a serial feed.
- **ESP32 is powered from the battery** (its own small 18->3.3 V DC-DC), *not* from the Pi's USB - so it keeps counting when the Pi is off or rebooting.

### Why the ESP32 front-end (not INA228 straight to the Pi)

The INA228's CHARGE/ENERGY registers are **volatile** - they reset on power loss, and while the Pi is off nobody accumulates charge. An always-on, battery-powered ESP32 that persists the count to NVS solves this: the gauge survives Pi reboots/shutdowns and keeps counting during Pi downtime. Direct INA228->Pi I2C is simpler but only adequate if the Pi is genuinely always-on.

## 4. Component decisions (and the alternatives considered)

### Monitor chip - chose **INA228**

| Chip | ADC | Bus max | Shunt | HW charge counter | Notes |
|------|-----|---------|-------|-------------------|-------|
| INA219 (DFRobot SEN0291 Gravity board) | 16-bit | 26 V | onboard | no | Easiest/cheapest, but 26 V is tight margin over a 6S pack's ~21.9 V full; coarse resolution |
| INA260 | 16-bit | 36 V | 2 mOhm integrated (15 A) | no | Simplest wiring (no external shunt), but software coulomb-counting only, fixed 1.25 mA steps |
| **INA228** (chosen) | **20-bit** | **85 V** | external | **yes** | Hardware charge/energy accumulation, temp sensor, uA-capable resolution - the correct fuel-gauge chip for a flat LiFePO4 curve |

Specific board: **Adafruit INA228 breakout (PN 5832)** - onboard 15 mOhm shunt (~+/-10 A, plenty for the Pi's ~2-3 A battery-side draw), 85 V bus, STEMMA QT + header pads, default I2C address **0x40**, ALERT pin available. VBUS sense is routed internally; only land wires on **VIN+ / VIN-**.

> Note on INA228 pins: **VBUS** = the 0-85 V voltage-sense input (why no divider is needed). **IN+/IN-** = the shunt-sense pins for current. **VS** = the chip's own 2.7-5.5 V supply. VBUS is a measurement input, not a power input. (Unrelated to *USB* VBUS.)

### MCU - chose **ESP32-C3 using its USB Serial/JTAG peripheral**

- C3's USB Serial/JTAG is a **fixed hardware CDC** device: enumerates on the Pi as `/dev/ttyACM0` with zero USB-stack code - just `Serial.print`. Stays alive for flashing/debug even if firmware hangs. Low power, cheap, single-core RISC-V - ample for this job.
- **ESP32-S3 USB-OTG** (full TinyUSB stack: HID/MSC/host/composite) was considered but adds capability this task doesn't use, plus more power/cost. Only worth it to present as a non-serial USB device or act as USB host. Not needed here.

## 5. The one electrical gotcha - USB VBUS contention

A standard USB-A cable carries **+5 V on VBUS**. Because the ESP32 is self-powered from the battery-side DC-DC, connecting VBUS would tie two 5 V sources together (Pi USB vs. our DC-DC rail) -> back-power / current fight. **Resolutions, best first:**

1. **UART link (recommended for a fixed install):** 3 wires - ESP32 TX->Pi RX (GPIO15), ESP32 RX->Pi TX (GPIO14), GND<->GND. No VBUS, no enumeration quirks. Pi reads `/dev/serial0`.
2. **Data-only USB cable (VBUS cut):** keep USB convenience, remove the +5 V wire. Caveat: a self-powered C3 may need VBUS as a *sense* line to enumerate - if so, feed Pi-VBUS through a Schottky diode into a sense pin only, never into the 3.3 V rail.
3. **Power ESP32 from Pi USB (normal cable):** simplest, but ESP32 dies when the Pi is off -> defeats the whole point. Only if Pi is always-on.

No galvanic isolation is used or needed (shared ground is fine because the INA228 is high-side sensing).

## 6. Firmware design (ESP32-C3)

Continuous loop: read INA228 -> accumulate charge -> persist -> decide SoC -> stream to Pi.

**State-of-charge logic:**
- **Primary:** coulomb counting. Read the INA228 charge (coulombs); `mAh = coulombs / 3.6`. Track net Ah vs. full-capacity reference.
- **Full-charge anchor (self-heal):** whenever pack voltage ~ full **and** charge current has tapered to ~0 (charge-termination), snap SoC to **100%**. This is what keeps the gauge honest long-term despite flat-curve uncertainty.
- **Persistence:** write running count to **NVS every few seconds** so a power cut loses almost nothing. Optional hold-up (supercap on 3.3 V, or Pi-USB back-feed while Pi is on) to save cleanly on brown-out.

**Battery-replacement / re-seed state machine (see S7).**

**Output:** JSON line over serial, e.g.
`{"v":19.84,"i":1.42,"p":28.2,"soc":73,"mah_used":1180,"wh":22.4,"state":"discharging","est":false}`

## 7. Battery replacement handling

Swapping the pack invalidates the accumulated count (new pack = unknown SoC). Because LiFePO4 voltage is flat, you often *can't* recover SoC from voltage alone. Strategy:

1. **Detect the swap** - cold boot after power loss is the primary signal; also an unexplained resting-voltage jump; best is an explicit button or connector-detect pin.
2. **Re-seed on connect** - let the pack rest, read voltage:
   - In the **full knee** (>~3.42 V/cell) -> seed 100%.
   - In the **empty knee** (<~3.0 V/cell) -> seed low.
   - **Flat middle** -> SoC unknown; use a conservative estimate, flag `est:true`, self-correct at next full charge.
3. **Full-charge anchor** re-syncs to 100% on the next complete charge regardless.
4. **Multiple interchangeable packs** -> optional **1-Wire ID EEPROM (e.g. DS2431) on each pack**, so the monitor restores that pack's stored count and rated Ah automatically. Without it, treat every swap as "unknown pack."
5. **Update capacity reference per pack** - rated Ah differs between/across aging packs; make "100% = X Ah" settable (config, ID chip, or learned over a full discharge).

## 8. LiFePO4 voltage reference (per cell)

| Point | V/cell | 5S pack | 6S pack |
|-------|--------|---------|---------|
| Full (charging) | 3.65 | 18.25 | 21.9 |
| Full (resting) | ~3.40 | ~17.0 | ~20.4 |
| Nominal | 3.20 | 16.0 | 19.2 |
| Knee / near-empty | ~2.8-3.0 | ~14-15 | ~17-18 |
| Cutoff | 2.5 | 12.5 | 15.0 |

## 9. Open decisions needed before writing firmware

1. **Pack configuration: 5S or 6S?** (Sets all voltage thresholds. Also: on a 6S pack the DFRobot/INA219 26 V option is borderline - moot now that INA228 is chosen.)
2. **Pack rated capacity (Ah)?** (Sets the 100% reference for the percentage math.)
3. **Link to Pi: UART or data-only USB?** (Recommendation: UART for a fixed install.)
4. **Single pack or multiple rotating packs?** (Determines whether to add per-pack ID chips.)

## 10. What's left to build

- [ ] ESP32-C3 firmware: INA228 read, coulomb count, NVS persistence, full-charge anchor, swap re-seed state machine, JSON serial output. (Arduino or ESP-IDF - TBD.)
- [ ] Pi-side reader: parse serial feed, expose battery %; optional systemd service + small dashboard.
- [ ] Wiring diagram with exact pin assignments once link type is fixed.
- [ ] Add a BMS if the pack doesn't already have one - this design is **pack-level** monitoring and does **not** do per-cell balancing/protection.

## 11. Safety note

This is a capacity gauge, not a protection circuit. LiFePO4 packs still need a **BMS** for per-cell balancing and over-discharge/over-charge/short protection. The INA228 gauge complements a BMS; it does not replace one.
