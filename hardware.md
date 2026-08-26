# Hardware - Universal Battery Monitor (ESP32-C3 + INA228)

**Date:** 2026-08-13
Companion to `battery-monitor-handoff.md` (origin/rationale) and `DEVELOPMENT_PLAN.md` (firmware).

**Scope.** A self-contained monitor module that mounts **at the battery** and reports over USB-C to any host - Raspberry Pi, PC, laptop. It is **host-agnostic and battery-agnostic**: chemistry, cell count, capacity and voltage limits are all runtime configuration via `ATS` (plan S2.2), never compiled in. The handoff document describes the Pi-specific installation that prompted this; the design has since generalised.

---

## 1. Bill of materials

| # | Part | Spec | Notes |
|---|---|---|---|
| 1 | **INA228 breakout** | Adafruit PN **5832** | 20-bit, 85 V bus, onboard **15 mOhm** shunt (+/-10.9 A), I2C addr **0x40**, STEMMA QT + header pads. **VBus is a separate pad** - bridge it to VIN+ with a solder link (S3.1.1) |
| 2 | **ESP32-C3 SuperMini** | ESP32-C3FH4, 4 MB flash, native USB-C | Reference board - see S6. Native USB-Serial/JTAG on GPIO18/19, plain active-low LED on GPIO8, BOOT button on GPIO9. **Not DevKitM-1** (WS2812 LED). Power via the **`5V`** pin, using its onboard regulator |
| 3 | **Buck converter** | **9-36 V in**, 5 V out, >=200 mA, **Iq < 1 mA** | Powers the monitor from the battery it watches. Feeds the SuperMini's `5V` pin. See S11 |
| 4 | *(host's own supply)* | - | **Out of scope.** Whatever the battery powers sits downstream of the INA228 and is simply measured |
| 5 | Fuse | Sized to the load, inline on battery (+) | Upstream of everything, including the INA228 |
| 6 | Battery *(any)* | Configured at runtime via `ATS` | Currently **BL18120**, 18 V 12 Ah 216 Wh, 5S3P Li-ion (S7). Must have its own BMS - this board is not protection |
| 7 | **USB-C cable, VBUS cut** | host USB-A -> SuperMini USB-C | Data + ground only. The module is self-powered. See S5 |
| 8 | **STEMMA QT -> male header cable** | JST-SH 4-pin, ~100-150 mm | Adafruit 4209 or Qwiic equivalent. Connects INA228 to the SuperMini (S3.2) |
| 9 | Wire | 16-18 AWG power, 24-26 AWG signal | Power path sized for ~3 A continuous |
| 10 | Pin header (optional) | 3-pin, GPIO21/GND/3V3 | Debug UART0 - strongly recommended, see S4.4 |

---

## 2. Block diagram

```
  +---- FUSE
  |
battery (+) --+--> [ INA228 ]  IN+ -+- 15mOhm shunt -+- IN- --+--> whatever the battery powers
                   VBUS sense ------+              |        |    (Pi, or any other load
                        |                          |        |     measured, not managed)
                        |                          |        +--> buck 9-36V->5V --> SuperMini `5V`
                        |                          |                                  |
                        +-- I2C -------------------+----------------------------------+
                                                   |                          onboard VR -> 3V3
                                                   |                                  |
                                                   |                          ESP32-C3 + INA228 VS
                                                   |
   battery (-) -------------------- COMMON GROUND -+----------------------------------------

       ESP32-C3 === USB-C, VBUS CUT (D+/D-/GND) ===> any host  ->  /dev/ttyACM0
```

The INA228 sits **high side, upstream of everything**, so it measures the battery's total draw whatever the load happens to be. The monitor **powers itself from the battery it watches**, so it counts continuously regardless of whether any host is attached - which is what makes coulomb counting, NVS persistence and the INA228's hardware charge accumulator worth having. USB is purely a console.


### 2.1 Complete wiring list

Everything, in one place. Detail and rationale in the sections referenced.

**Power path - thick wire (16-18 AWG), fused**

| From | To | Note |
|---|---|---|
| Battery **(+)** | Fuse -> INA228 **VIN+** | Fuse ahead of everything (S10) |
| INA228 **VBus** | **VIN+**, solder link on the board | ! **Mandatory** - no voltage reading without it (S3.1.1) |
| INA228 **VIN-** | The load (host PSU, etc.) | Whatever the battery powers |
| INA228 **VIN-** | Buck **IN+** | Monitor's own supply |
| Battery **(-)** | Buck **IN-**, own dedicated wire | Kelvin ground - not tapped off the load return (S3.1.4) |
| Battery **(-)** | Load ground | Separate conductor |

**Monitor supply**

| From | To | Note |
|---|---|---|
| Buck **OUT+** (5.0 V) | SuperMini **`5V`** | Set/verify 5 V *before* connecting (S11.6) |
| Buck **OUT-** | SuperMini **`GND`** | |
| - | SuperMini `3V3` | ! **Leave unconnected as an input.** It is the onboard regulator's *output* |

**INA228 ? SuperMini - STEMMA QT cable, connector end at the INA228 (S3.2, S3.3)**

| Wire | Signal | SuperMini pad |
|---|---|---|
|  Black | GND | **GND** |
|  Red | 3.3 V | **3V3** |
|  Blue | SDA | **GPIO4** |
|  Yellow | SCL | **GPIO3** |

Those four are consecutive pads on the left edge, in the cable's own colour order - no wire crosses the board.

**Optional / build-dependent**

| Signal | Pin | Note |
|---|---|---|
| INA228 **ALERT** | GPIO5 | Optional, unused by the planned firmware. Wire it anyway - it costs one wire and enables limit alerts later |
| Button - dev | GPIO9 | Onboard BOOT button, no wiring |
| Button - production | **GPIO10** | Case-mounted, to GND (S4.3) |
| LED - dev | GPIO8 | Onboard, active low, no wiring |
| LED - production | any free pin | Panel LED, ~2 mA series resistor (S8) |
| Debug UART0 | GPIO21 / GPIO20 / GND | 3-pin header, strongly recommended (S4.4) |

**Host link**

| From | To | Note |
|---|---|---|
| SuperMini **USB-C** | Host USB-A | **VBUS wire CUT.** D+/D-/GND only (S5) |

**Ground - the INA228's voltage reference**

| From | To | Note |
|---|---|---|
| Battery **(-)** | **INA228 breakout GND** | ! Short dedicated wire. This is the reference VBus is measured against (S3.1.5) |
| Battery **(-)** | Buck **IN-** | Own conductor, not tapped off the load return |
| Battery **(-)** | Load ground | Separate conductor |

Without a ground reference the INA228 measures VBus against nothing: **voltage reads garbage while current reads correctly** - the same silent failure as a floating VBus pad.

### 2.2 Assembly order

1. Bench-set the buck to 5.0 V at both battery voltage extremes, load disconnected (S11.6)
2. Buck -> SuperMini `5V`/`GND`; confirm `3V3` reads ~3.3 V
3. QT cable to the INA228; verify colours with a meter *before* plugging in (S3.2)
4. USB cable with VBUS cut; confirm `/dev/ttyACM0` enumerates
5. Only then the power path: fuse, VIN+/VIN-, load, battery

---

---

## 3. INA228 wiring

### 3.1 Power path - the battery connection

The INA228 sits **in series with the battery's positive lead**: all load current flows `VIN+ -> 15 mOhm shunt -> VIN-`.

```
                  +------------- INA228 breakout -------------+
                  |        +-- solder link --+                 |
                  |        v                 v                 |
  battery (+) --[FUSE]--> VIN+ --[ 15 mOhm shunt ]--> VIN- ------+--+--> load (+)
                  |       VBus                                 |  |
                  |    (sense pad)                             |  +--> buck IN+
                  |                                            |
                  |    GND <--- logic ground ------------------+
                  +----------------------+---------------------+
                                         |
  battery (-) ---------------------------+--+--> load (-)
                                            +--> buck IN- (own wire, S3.1.3)
```

#### 3.1.1 ! VBus must be connected - bridge it to VIN+

On this board **VBus is a separate pad, not bonded to VIN+**. Left floating, the INA228 measures **no battery voltage at all**: no SoC, no OCV seeding (plan S4), no full-charge anchor, no empty clamp. Current still reads correctly, which makes the failure easy to miss on the bench.

**Solder a link from VBus to VIN+ on the breakout.** A solder blob or a short wire between the pads - no extra battery wire, no extra fuse, nothing to route.

Confirm during bring-up (S9 step 5) that reported voltage tracks a DMM.

#### 3.1.2 Why not a separate Kelvin sense wire

Running VBus to the battery terminal on its own fused wire would be *slightly* more accurate: the sense wire carries no current, so the reading excludes the drop across the main lead and fuse. Realistically ~10 mOhm, so **20 mV at 2 A, 50 mV at 5 A - 0.1-0.25 % on a 21 V pack.**

It is not worth the extra wire and fuse, for two reasons:

1. **Voltage is only trusted at rest.** OCV seeding and the full-charge anchor both operate at or near zero current, where the IR drop is zero either way. The gain applies only under load, where the design deliberately relies on coulomb counting instead.
2. **Cell internal resistance dwarfs it.** A 5S3P Li-ion pack has ~30-60 mOhm internal resistance - **60-120 mV of sag at 2 A**, three to six times any wiring drop, and unfixable by any wiring topology. Eliminating 20 mV of lead drop while 100 mV of cell sag remains is optimising the wrong term.

**The decisive point:** IR compensation (plan S4) is needed *regardless*, because cell internal resistance is inside the battery and no wiring reaches it. Kelvin sensing only takes the ~10 mOhm of lead and fuse out of `R_total`, cutting it from ~50 mOhm to ~40 mOhm. After compensation the residual is +/-10 mV vs +/-8 mV - against a **+/-25 mV floor set by electrochemical relaxation**, which no wiring can touch. Net improvement ~ 2 mV, i.e. unmeasurable.

It is also marginally *worse* in one respect: a second thin conductor on battery positive is an extra chafe/pinch/short path on a pack that can deliver hundreds of amps. Small, but real, and bought for nothing.

If a future build genuinely needs accurate under-load *terminal* voltage - a different requirement from SoC - run VBus separately with its own **250 mA inline fuse**. It is a thin wire on battery positive and must be protected.

#### 3.1.3 The rules that matter

**Every amp leaving the battery's positive terminal must pass through VIN+ -> VIN-.** A second connection to battery positive bypasses the shunt, and that current is invisible to the gauge - silently, with no error indication. Check that exactly one wire leaves the battery's positive terminal.

**Charging in place must connect on the VIN- side.** Wired to the battery terminals directly, charge current bypasses the shunt and is never counted.

#### 3.1.5 Ground - VBus has no meaning without it

The INA228 measures VBus **relative to its own GND pin**. With that pin unreferenced to battery negative there is no measurement: **voltage reads garbage while current reads correctly**, because current is differential across the shunt and needs no ground. Same silent-failure signature as a floating VBus pad (S3.1.1).

Two paths reach battery negative, and you want both:

1. **Implicit, via the buck:** `INA228 GND -> QT black -> SuperMini GND -> buck OUT- -> buck IN- -> battery (-)`. This works **only because a standard buck module is non-isolated** - input and output grounds are one net.
   > ! **Fit a non-isolated buck.** An isolated DC-DC breaks voltage sensing entirely, and does so silently.
2. **Explicit, recommended:** a short dedicated wire from the **INA228 breakout's GND pad to battery negative**. Shorter reference path, and it removes the dependency on the buck's internal ground bond. The resulting loop with path 1 is harmless - only milliamps flow.

   **Landing it on the SuperMini's GND instead is equivalent** - the QT black wire makes them one net. The INA228 end is marginally better (it is the measurement reference, so you want the shortest path to it) but the difference is the drop across a 28 AWG wire carrying 0.6 mA: **tens of microvolts.** Use whichever is physically easier; the INA228 usually is, since the battery wires already land there.

> ### ! `GND` is not `VIN-`
>
> They are entirely different nets, and the names invite the mistake.
>
> - **`GND`** - logic ground, on the header/QT side. This is what battery negative connects to.
> - **`VIN-`** - a *power* terminal on the load side of the shunt, sitting at roughly **battery-positive** potential, because this is high-side sensing.
>
> **Wiring battery negative to `VIN-` shorts the battery** through the load lead. Check this before applying power.

Route both to battery negative as **their own conductors**, not tapped off the load's ground return: a shared return puts the load's IR drop straight into the VBus reference, a larger error than anything in S3.1.2.

> **Bench testing without the buck:** on USB power the module's ground is the *host's*. Connecting VIN+/VBus to a battery with no ground tie gives a meaningless reading, and if the two grounds sit far apart you can exceed the 85 V common-mode limit. **Always tie battery negative to the module GND**, even for a quick test.

#### 3.1.4 Terminals and wire

| Terminal | Connects to | Wire |
|---|---|---|
| **VIN+** | Battery (+), through the fuse | 16-18 AWG |
| **VBus** | **VIN+**, via a solder link on the board | - |
| **VIN-** | Load (+) **and** buck IN+ | 16-18 AWG |

Size for full load current, not average - 16 AWG to ~10 A, 18 AWG to ~5 A. Screw terminals must be genuinely tight: a loose joint carries the entire load current.

#### 3.1.6 Current sign

Discharge current flows VIN+ -> VIN-, which the INA228 reports as **positive**, matching plan S2.3. Charging reads negative. **Verify during bring-up** (S9 step 6); if inverted, VIN+ and VIN- are swapped.

> **Pin naming, because it trips people up:** INA228 **VBus** is the 0-85 V *voltage-sense input*. **VIN+/VIN-** carry load current through the shunt. **VS** is the chip's own 2.7-5.5 V supply, from the SuperMini's 3V3. VBus is a measurement input, **not** a power input, and is unrelated to *USB* VBUS.

### 3.2 Signal side - STEMMA QT cable

The Adafruit 5832 has two STEMMA QT (JST-SH 4-pin) sockets. The SuperMini has none, so use a **STEMMA QT -> male header jumper cable** (Adafruit 4209 or any Qwiic equivalent) and land the four flying leads on the ESP32 header.

**STEMMA QT / Qwiic colour code - fixed across both standards:**

| Wire | JST-SH pin | Signal | -> ESP32-C3 SuperMini |
|---|---|---|---|
|  **Black** | 1 | GND | **GND** |
|  **Red** | 2 | 3.3 V | **3V3** |
|  **Blue** | 3 | SDA | **GPIO4** |
|  **Yellow** | 4 | SCL | **GPIO3** |

**These four are consecutive pads on the SuperMini's left edge**, in exactly the cable's own colour order - so a cut-and-solder job needs no wire crossing the board:

```
   5V   -
   GND  ---  black
   3V3  ---  red
   GPIO4---  blue   (SDA)
   GPIO3---  yellow (SCL)
   GPIO2 -
   GPIO1 -
   GPIO0 -
```

This is why SCL is on GPIO3 rather than GPIO5: GPIO5 is on the *opposite* edge of the board and would force the yellow wire across it. Verify the pad order against your board's silkscreen - SuperMini clones vary.

Mnemonic: the two power wires are the conventional black/red pair; of the signals, **bl**ue is the one that starts like **SDA's** partner - simply remember *blue = data, yellow = clock*.

The red wire supplies the INA228's **VS** pin from the SuperMini's `3V3` pin, i.e. from the board's onboard regulator. That is correct and intended - the sensor draws under 1 mA, and sharing the rail keeps I2C levels matched.

> ! Verify before trusting the colours. Cheap unbranded Qwiic cables occasionally deviate. Beep out black?GND and red?3V3 with a multimeter against the breakout's silkscreen before plugging in - a swapped red/black will destroy the INA228.

Remaining connections, on the breakout's header pads (not on the QT connector):

| INA228 | ESP32-C3 | Note |
|---|---|---|
| ALERT | **GPIO5** | Open-drain, needs a pull-up. Optional; unused in Phase 1-5 |
| A0/A1 | unconnected | Leaves address at **0x40** |

I2C pull-ups (4.7 kOhm) are **already on the Adafruit board - do not add a second set.** Run I2C at **400 kHz**; keep the cable under ~20 cm, and drop to 100 kHz if it must be longer.

### 3.3 Cutting and soldering the STEMMA QT cable

Keep the **connector end** for the INA228 and cut the other. A QT-to-QT cable cut in half yields two usable pigtails; the wiring is straight-through, not crossed.

- **Verify colours after cutting.** Once the header end is gone you've lost the labelling. Beep each wire back to its connector pin before soldering - especially black and red, since reversing them destroys the INA228.
- These are **~28 AWG stranded with thin PVC insulation**. Strip ~2 mm, work quickly at ~300 degC: the insulation recedes fast and the strands are easy to nick.
- **Pre-tin** each wire and each pad, then join. Don't try to melt solder into an untinned bundle.
- **Strain relief is the real failure mode.** Fine stranded wire soldered to a pad with nothing holding it will fatigue and crack at the joint under vibration - and this is a mobile/vehicle install. Anchor the cable to the board with a blob of hot glue or epoxy just behind the joints, or thread it through a nearby mounting hole first.
- Heatshrink each conductor individually if there's any chance of the four ends shifting against each other.

### 3.4 Shunt sensing

Nothing to wire - the 15 mOhm shunt and its Kelvin connections are on the breakout. Dissipation at 3 A is `I^2R = 9 x 0.015 = 0.135 W`, trivial for the 2512 part. At the 10.9 A ceiling it is 1.8 W, which would need airflow. Size `Imax` in `ATS` to the actual load, not the shunt's ceiling.

### Measuring above 10 A - external shunt

The onboard shunt caps the module at **+/-10.9 A** (163.84 mV full scale / 15 mOhm). For more, remove it and fit an external one.

**Example: FL-2 DC 75 mV / 100 A.** These are rated by the voltage they develop at rated current, so 75 mV at 100 A is **750 uOhm**.

| | Onboard | FL-2 75 mV / 100 A |
|---|---|---|
| Resistance | 15 mOhm | **0.75 mOhm** |
| Ceiling (163.84 mV / R) | 10.9 A | **218 A** |
| `CURRENT_LSB` at that Imax | 20.8 uA | 190.7 uA at Imax 100 A |
| Dissipation at rated current | 1.8 W at 10.9 A | **7.5 W at 100 A** |

That last row is why a 100 A shunt is a large finned block rather than a chip resistor. Mount it where it can shed heat, and expect its resistance to drift with temperature - a few hundred ppm/degC on a decent one, worse on a cheap one.

**Steps:**

1. **Remove the onboard shunt.** It is the large 2512 resistor between the VIN+ and VIN- terminals on the Adafruit 5832. Hot air is easiest; two irons also work. Leaving it in place puts 15 mOhm in parallel with the external shunt and silently scales every reading.
2. **Wire the load current through the external shunt**, using conductors sized for the real current - at 100 A that is welding cable, not hookup wire.
3. **Sense with a Kelvin (4-wire) pair.** Run IN+ / IN- from the shunt's **inner sense screws**, never the power lugs. This matters far more than it did for VBus (S3.1.2): at 100 A, even 0.1 mOhm of lug and joint resistance is 10 mV against a 75 mV signal - a 13 percent error. Twist the sense pair and keep it away from the power path.
4. **Set the value** in `idf.py menuconfig` under Battery Monitor -> Hardware parameters:
   `CONFIG_PM_RSHUNT_MICROOHM = 750`
5. **Provision a matching Imax** with `ATS`. The firmware derives the allowed range from the shunt, so `ATS` will now accept up to ~218 A and reject beyond it:

```
ATS=LiIon,5S3P,12000,15.0,21.0,100.0,bigpack
```

> **VBus is unaffected.** Only the current path changes; the 0-85 V bus measurement and its solder link to VIN+ stay as they are.

> **Resolution is the trade.** `CURRENT_LSB = Imax / 2^19`, so a 100 A ceiling gives 191 uA steps against 20.8 uA on the stock shunt. Still far finer than any voltage-based estimate, but set `Imax` to the current you actually expect rather than the shunt's ceiling - and below 54 A on this shunt the driver selects ADCRANGE=1 for 4x finer resolution automatically.

If you fit some other shunt, `R = rated_mV / rated_A`. A 50 mV / 50 A part is 1 mOhm; a 75 mV / 200 A part is 375 uOhm.

---

## 4. ESP32-C3 pin map

### 4.1 Assignments

| Function | GPIO | Direction | Note |
|---|---|---|---|
| I2C SDA | **4** | I/O | To INA228 (blue) |
| I2C SCL | **3** | I/O | To INA228 (yellow). Chosen so GND/3V3/SDA/SCL are four consecutive pads - S3.2 |
| INA228 ALERT | **5** | in | Optional, unused in Phase 1-5 |
| **User LED (blue)** | **8** | out | Onboard, active low. The only controllable LED - see S4.3 |
| **Button - dev** | **9** | in | Onboard BOOT button, active low. **Dev-board expedient only** |
| **Button - production** | **10** | in | Case-mounted button. No strapping function. See S4.3 |
| USB D- | **18** | - | **Fixed by hardware.** AT link |
| USB D+ | **19** | - | **Fixed by hardware.** AT link |
| Debug UART0 TX | **21** | out | Bootloader + panic output |
| Debug UART0 RX | **20** | in | |
| UART1 TX *(alt link)* | **6** | out | Only if built for UART instead of USB |
| UART1 RX *(alt link)* | **7** | in | |

Everything the user interface needs is already on the devboard - **no additional components**.

### 4.2 Pins you must not use

| GPIO | Why |
|---|---|
| **12-17** | SPI flash. Not available, full stop |
| **11** | VDD_SPI |
| **2** | Strapping pin, must be high at reset |
| **18, 19** | USB Serial/JTAG, fixed function |

GPIO8 and GPIO9 are also strapping pins, but are used here for the onboard LED and (on dev boards only) the BOOT button - the standard devkit arrangement. Their constraints are in S4.3.

Free and unassigned: **GPIO0, 1, 2**, plus **GPIO10** until the production button claims it. GPIO0-2 are the RTC/wake-capable spares.

### 4.3 LED and button - strapping-pin constraints

Both onboard controls sit on strapping pins. Neither is a problem with stock devkit wiring, but both have a rule.

**Three LEDs, only one controllable:**

| LED | Where | Driven by | Meaning |
|---|---|---|---|
|  **Red** | SuperMini | hardwired to 3V3 | ESP32 has power |
|  **Green** | INA228 breakout | hardwired to VS | INA228 has power |
|  **Blue** | SuperMini, **GPIO8** | firmware | All status signalling (S4.3 table) |

Red and green are power indicators - always on when powered, not addressable. Only the blue GPIO8 LED carries information, which is why the whole vocabulary is encoded in blink counts on a single LED.

#### Bring-up diagnostic value

The two power LEDs localise a fault instantly, before any serial connection exists:

| Red | Green | Blue | Diagnosis |
|---|---|---|---|
| off | off | off | No 5 V - buck output, wiring, or battery |
| on | **off** | on | **QT red wire (3V3) not reaching the INA228** - the single most likely assembly error |
| on | on | off | ESP powered but firmware not running - check flash, or a boot loop |
| on | on | on | Board healthy; any remaining fault is USB, I2C, or VBus |

Note green proves only that the INA228 has **supply**. It says nothing about VBus - a floating VBus pad gives a lit green LED, correct current readings, and zero volts (S3.1.1).

#### Power cost

The two power LEDs draw roughly 1-3 mA each at 3.3 V, so ~1.2 mA at the battery once regulator and buck losses are counted - about **17 % of the monitor's ~7 mA total** (S8). Irrelevant here, since the monitor is not stored on a battery. If a future build ever needs minimum standby, desoldering both power LEDs is the cheapest saving available and costs only the diagnostic table above.

**Signalling LED - plain monochrome, no RGB.** All feedback is encoded in blink counts and timing (plan S2.6), so colour would add nothing. The driver is a GPIO toggle plus a timer; no `led_strip` component, no RMT peripheral.

! **Dev board must have a *plain* LED on GPIO8.** Seeed XIAO ESP32-C3 and the C3 "super mini" boards qualify. **ESP32-C3-DevKitM-1 does not** - its GPIO8 LED is an addressable WS2812 that ignores plain high/low levels and will never light. Check this before ordering.

GPIO8 is a strapping pin and must read **high at reset**. Stock active-low wiring (3V3 -> LED -> resistor -> GPIO8) weakly pulls it up and is fine. ! **Never fit an external pull-down or an active-high LED with a stiff pull-down on GPIO8** - the board will boot-loop or fail to start.

In production the LED moves to the enclosure panel alongside the button, on any free pin (`CONFIG_PM_LED_GPIO`), at which point the GPIO8 strapping constraint no longer applies. Size the series resistor for ~2-3 mA, not 20 mA: it only has to be visible, and the fault pattern blinks continuously (S8).

| Event | LED response |
|---|---|
| Serial command OK | ~20 ms flicker |
| Serial command ERROR | 3 fast blinks over 1 s |
| Double press | 2 fast blinks, then N slow blinks, N = SoC/10 |
| Long press >=5 s | 5 medium blinks = "new pack, assume full" accepted |
| Released early | 1 long blink = cancelled |
| Refused (pack not full) | 10 rapid flutter blinks |
| Idle | **off** - saves standby current (S8) |
| **INA228 fault** | **continuous blink, 0.5 s on / 0.5 s off**, until cleared |

Full timing and the collision analysis are in plan S2.6. Note the activity indication is a **20 ms flicker, not a blink** - the Pi polls at 1 Hz, so anything longer would be visually identical to the fault pattern.

Set `CONFIG_PM_LED_ACTIVE_LOW` to match the board.

**Button - the pin changes between dev and production**, so firmware never hardcodes it:

```
CONFIG_PM_BUTTON_GPIO        default 9  (dev) -> 10 (production)
CONFIG_PM_BUTTON_ACTIVE_LOW  default y
```

**Development: onboard BOOT button, GPIO9.** Zero extra parts, external pull-up already fitted. Held low at reset it selects serial download mode - that's the normal flashing path, and not a concern in use since the pack is inserted with both hands and the button pressed afterwards. Power-cycle recovers it.

**Production: case-mounted button on GPIO10.** GPIO10 is free, has **no strapping function**, and no ADC or peripheral conflict - so the download-mode interaction disappears entirely in the shipping build. That is the main reason to prefer it over any other spare pin.

> **GPIO10 is settled.** Only GPIO0-5 are RTC/wake-capable on the C3, so GPIO10 cannot wake the chip from deep sleep. That would have mattered for the low-power storage mode (plan S6) - but **the monitor is never stored connected to a battery**, so it draws nothing in storage and the mode is unnecessary. GPIO10's lack of a strapping function is the deciding factor. If storage mode is ever revived, the button must move to GPIO1.

Wiring for a panel button on a flying lead - a metre of wire into a high-impedance CMOS input is an antenna, and the internal ~45 kOhm pull-up is too weak for it:

```
3V3 --[10k]--+--[100R]-- GPIO10
             |
           [100n]        button -- GND
             |
            GND
```

External 10 kOhm pull-up, 100 nF to ground at the pin for debounce and ESD, 100 Ohm series to limit injected current. Use shielded or twisted pair if the run is long, and keep it away from the DC-DC switching nodes.

Firmware requires the button to be **released before arming** gesture detection, so a press held across a reset can't instantly register as a 5 s hold. This matters only on the GPIO9 dev build, but is harmless to keep.

> **! Correction to prior docs.** `battery-monitor-handoff.md` S5 lists the UART option as "ESP32 TX->Pi RX (GPIO15), ESP32 RX->Pi TX (GPIO14)". Those are the **Raspberry Pi header** pin numbers. On the ESP32-C3, GPIO14 and GPIO15 are **SPI flash pins and cannot be used**. The ESP32 side of that link must be GPIO6/GPIO7 (or any free pin - the C3's GPIO matrix routes UART1 anywhere). `DEVELOPMENT_PLAN.md` S1.3 repeated the mistake and has been corrected to match this table.

### 4.4 Debug header - build it in

With `CONFIG_ESP_CONSOLE_NONE=y` and logs going to the RAM ring buffer (plan S1.4), **a boot-time panic produces no output on the AT link.** The ROM bootloader still prints on UART0 TX at 115200. Bring GPIO21 / GPIO20 / GND out to a 3-pin header so an FTDI can be clipped on when something is genuinely broken. Omitting this makes a bricked board very hard to diagnose.

---

## 5. USB wiring - cut VBUS

The module is **self-powered from the battery** (S11), so a host's USB +5 V must not be connected: two independent 5 V sources on one net means back-powering and current contention. On the SuperMini this is worse than usual, because **the `5V` pin and USB VBUS are the same net** - the buck's output would appear on the USB connector and feed backwards into the host's port through its current-limit switch.

**Cut the VBUS (red) wire. Keep D+, D-, GND.**

This needs no workaround on the ESP32-C3: the USB Serial/JTAG peripheral **has no VBUS sense line**. It enumerates purely on the D+ pull-up, which asserts whenever the chip is powered. So:

- Host on, module on -> enumerates as `/dev/ttyACM0`
- **Host absent -> module keeps counting**, presenting a pull-up to a dead port. Harmless, and the entire point of the design
- Battery removed -> module off; the host's port sees nothing

The "feed VBUS through a Schottky into a sense pin" workaround in handoff S5 applies to parts that need VBUS detection. **The C3 does not.**

### 5.1 Which wire to cut - USB Type-A pinout

| Pin | Signal | Wire | Keep? |
|---|---|---|---|
| 1 | **VBUS** +5 V |  red | [no] **cut this one** |
| 2 | D- | white | keep |
| 3 | D+ | green | keep |
| 4 | **GND** | black | keep |

Orientation-independent rule: **power on the outside, data on the inside** - pins 1 and 4 are the outer contacts, 2 and 3 the inner pair. If you need the physical order, looking *into* a Type-A receptacle with the plastic tongue at the top, pin 1 is on the **left**; same for a plug held contacts-down.

> ! **Meter it, don't trust the colours.** Unbranded cables deviate. Cutting a data line instead of VBUS gives a dead link; leaving VBUS connected back-feeds the host's port. Plug the A end into a powered port, measure 5 V between the outer contacts to confirm pins 1 and 4, then beep each wire back to its pin.

**Use a USB 2.0 cable, not 3.0.** A blue USB 3 Type-A has 9 pins including five SuperSpeed contacts to identify and ignore. The C3's USB Serial/JTAG is full-speed, so USB 3 adds nothing but confusion.

### 5.2 ! Power-sequencing on the bench - battery OFF first

**Disconnect the battery before removing power from the INA228. Reconnect in the opposite order.**

The failure this prevents: with `VS` at 0 V and `VIN+`/`VBus` still held at pack voltage, the INA228's internal ESD and protection structures sit forward-biased into a dead supply rail. The datasheet's 10 nA leakage figure covers *shutdown mode with VS present* and says nothing about this state. It is not a documented operating condition, and current in the milliamp range is plausible.

Measured consequence on this bench: a pack left overnight with the monitor unpowered but still wired lost about **130 mAh in 10 hours - roughly 13 mA**, against a DC-DC idle draw measured at 0.37 mA and a pack self-discharge budget under 1.5 mAh. Most of that drain is still unattributed, and this sequencing is the cheapest way to remove it as a variable.

```
Power up  :  monitor supply ON   ->  battery ON
Power down:  battery OFF         ->  monitor supply OFF
```

**This cannot happen in the shipping design**, because the monitor is powered from the battery it measures (S11): `VS` and the bus inputs rise and fall together, so the dead-rail state does not exist. It is purely an artifact of the bench rig, which splits the two across separate sources - the monitor on host USB, the sensing side on the pack.

> That asymmetry is itself an argument for wiring the bench the way the product works: fit the 9-36 V buck, power the monitor from the pack, and use the VBUS-cut USB cable for data only. Then the sequencing rule stops mattering because there is only one power domain.

If you must run split-powered for firmware work, the practical rule is: **unplug the battery before the notebook sleeps.**

#### 5.2.1 Side effect: pulling the pack while the monitor runs zeroed the gauge

Observed at the end of run 5, and again after it. With the ESP32 still USB-powered, disconnecting the pack made the INA228 read the bus at **0.077 V** with `err:0` - a correct measurement of an absent battery. The stored record was then rewritten to `soc:0.0`, `mah_left:0`, `mah_used:2000`.

**The cause was the empty clamp, not OCV seeding.** Both seeding paths were already guarded (`gauge_init` defers below 0.5 V, and the deferred seed waits for `bus_v > 0.5`). The unguarded test was the clamp:

```c
if (r->bus_v <= cfg->vmin && s_st.mah_remaining > 0)   // 0.077 <= 15.0
```

A disconnected pack is trivially "at or below Vmin", so the clamp fired on an absent battery and persisted the result. **Fixed** by requiring the pack to be present at all - `bus_v > 0.5 V/cell x series` - since no functioning pack of any chemistry sits that low. In the shipping topology the case cannot arise anyway, because the monitor is powered from the pack it measures (S11) and dies with it; it is specific to the split-powered bench rig.

One analysis consequence remains regardless: **the last few samples of any log that ends with a pack disconnect are not battery data** and must be excluded. `tools/svg_chart.py --min-bus-v 1.0` does this; without it the zero readings pin the y-axis and flatten the whole curve.

### 5.3 Bench development without a battery

Keep a **second, unmodified USB-C cable**. With the buck disconnected, a normal cable powers the SuperMini from the PC and everything except battery measurement works - fine for protocol, OTA and UI development. Two cables, no board modifications, no diode-OR circuitry.

If you later want one cable to do both, Schottky-OR the buck output and VBUS - but that requires cutting the SuperMini's VBUS-to-`5V` trace, and is not worth it for a bench convenience.

### 5.4 Ground reference - the one thing to get right

The INA228 measures **VBUS (battery voltage) relative to its own GND pin**, so that ground must sit at true battery negative for the reading to be accurate. Its *current* reading is differential across IN+/IN- and does not care.

**Run the module's ground directly to battery negative**, near the shunt - not via the USB cable's ground back through the host. Any IR drop along a shared load-return path would otherwise appear as voltage-measurement error: at 1.4 A through ~20 mOhm that is ~28 mV, about 0.13 % on a 21 V battery. Small, but systematic and load-dependent.

The USB ground forms a second, parallel path to the host. That is expected and harmless at these currents. No galvanic isolation is used or needed, because the INA228 is high-side sensing (handoff S5).

---

## 6. Devboard - ESP32-C3 SuperMini

The **ESP32-C3 SuperMini** (~22 x 18 mm, ESP32-C3FH4) is the reference board for this project. It satisfies every constraint this design imposes:

| Requirement | SuperMini |
|---|---|
| Native USB Serial/JTAG | [done] GPIO18/19 -> USB-C directly, no bridge chip -> `/dev/ttyACM0` |
| Plain (non-addressable) LED on GPIO8 | [done] blue LED, **active low** -> `CONFIG_PM_LED_ACTIVE_LOW=y` |
| BOOT button on GPIO9 | [done] plus a separate RESET button |
| All assigned pins broken out | [done] see below |
| 4 MB flash for dual OTA | [done] C3FH4 - **verify with `esptool.py flash_id`**, variants exist |
| Small enough to enclose | keep |

**Pin availability.** The SuperMini exposes GPIO0-10 and GPIO20/21. GPIO11-19 are not broken out, which costs nothing here: 11 is VDD_SPI, 12-17 are flash, and 18/19 are the USB pins we want on the connector anyway. Every pin in S4.1 - 3/4 (I2C), 5 (ALERT), 6/7 (alt UART), 8 (LED), 9 (button, dev), 10 (button, production), 20/21 (debug UART) - is available.

> The silkscreen labels GPIO8/GPIO9 as SDA/SCL. Ignore it - that's just the Arduino default. The C3's GPIO matrix routes I2C to any pin, and here it goes to GPIO4/3, leaving GPIO8 for the LED.

**Power it through the `5V` pin, using the onboard regulator.** Feed the Pi's 5 V rail to `5V` + `GND`; the board's regulator produces 3.3 V for the C3, and the `3V3` pin then supplies the INA228's VS via the QT cable's red wire. **No external converter is required.**

The regulator only has to carry ~25 mA, and 5 V -> 3.3 V leaves ample dropout headroom. Linear loss is ~40 mW, which is irrelevant - and zero whenever the system is off.

! **The `5V` pin is the same net as USB VBUS**, so the USB cable's VBUS wire *must* be cut (S5). Feeding both would back-feed the Pi's USB port. Do not also feed `3V3` - one supply pin only.

**The known SuperMini weakness is irrelevant here.** These boards are widely criticised for poor WiFi range - the ceramic antenna sits on an inadequate ground plane. This design uses no WiFi whatsoever (that's why the canspeed AP/HTTP OTA was dropped in favour of `ATFW`), so the defect never surfaces.

### 6.1 If you substitute another board

Two independent traps:

1. **USB-UART bridge instead of native USB.** Many C3 boards populate a CP2102/CH340 rather than wiring GPIO18/19 to the connector. The bridge is powered from VBUS, so cutting VBUS kills the port outright, and it enumerates as `/dev/ttyUSB0` rather than `/dev/ttyACM0`. Fall back to the UART link (S4.1, GPIO6/7) on such a board.
2. **Addressable LED on GPIO8.** ESP32-C3-DevKitM-1 fits a WS2812 there. With RGB support dropped (S4.3) it ignores plain high/low levels and simply never lights - everything else works, so it presents as a firmware bug.

Seeed XIAO ESP32-C3 is the closest alternative: native USB, plain GPIO8 LED, but fewer pins broken out.

---

## 7. Battery parameters

**Pack currently in use: BL18120 - 18 V, 12 Ah, 216 Wh, 21700 cells.**

> ! **None of this section is compiled into the firmware.** Chemistry, cell count, capacity and voltage limits are all runtime configuration, supplied by `ATS` and stored in NVS (plan S2.2). The values below describe the pack on hand today - swap in a different chemistry or cell count and only the provisioning string changes, not the build.

### 7.1 Chemistry - Li-ion NMC, not LiFePO4

This is **5S3P Li-ion (NMC)**, 15 x ~4000 mAh 21700 cells. The determining fact: **LiFePO4 cannot produce an 18 V nominal pack.** At 3.2 V/cell it lands on 16.0 V (5S) or 19.2 V (6S) - never 18.0 V. Li-ion at 3.6 V/cell gives exactly 5 x 3.6 = 18.0 V. The 21700 format, the 12 Ah = 3 x 4 Ah parallel arrangement, and the Makita-LXT-style `BL18...` designation all agree.

| Point | V/cell | **5S pack** |
|---|---|---|
| Full (charged) | 4.20 | **21.0 V** |
| 80 % | 3.95 | 19.75 V |
| 50 % | 3.65 | 18.25 V |
| Nominal | 3.60 | 18.0 V |
| 20 % | 3.50 | 17.5 V |
| Knee | 3.40 | 17.0 V |
| Cutoff | 3.00 | **15.0 V** |

> **Verify before provisioning:** charge the pack fully and measure. **~21 V confirms 5S Li-ion.** ~18.2 V would mean 5S LiFePO4 and every threshold here is wrong.

Provisioning string:
```
ATS=LiIon,5S3P,12000,15.0,21.0,10.0
```

### 7.2 What this changes versus the LiFePO4 assumption

The handoff's core argument (S2) was that a **flat** LiFePO4 curve makes voltage useless for SoC, forcing pure coulomb counting. **That largely dissolves for NMC**, which slopes usably from 4.2 V to 3.0 V per cell. Resting OCV maps to SoC within roughly +/-5-10 %.

Consequences, all favourable:

- **Swap re-seeding is now genuinely accurate.** The LiFePO4 fallback was "flat middle -> assume 50 %, flag `est:true`" - effectively +/-30 %. A rested NMC pack's voltage gives a real number.
- **The button is no longer load-bearing.** Voltage alone can re-sync the gauge after a swap.
- Coulomb counting stays primary - it is far better under load, where IR sag corrupts voltage - but it now has an independent cross-check instead of a blind spot.

### 7.3 Voltage ceiling

Max pack voltage is **21.0 V**, up from 18.25 V for a 5S LiFePO4 pack. Confirm the Pi's DC-DC accepts it. (Had the ESP32 needed its own pack-side converter, the mini360's 23 V absolute maximum would leave only 2 V of margin - another reason the S11 topology is the right one.)

### 7.3.1 ! The converter sets the floor, not the cells

**Measured, run 5:** the load died at **15.456 V (3.091 V/cell) drawing 193 mA**. That was the Pi's DC-DC falling out of regulation - not the BMS, and not the cells. Confirmation is unambiguous: current dropped 193 mA -> 2.2 mA in one 60 s interval, and pack voltage then *rose* 47 mV/cell over 13 minutes as the cells relaxed. The pack still held charge the converter could no longer reach.

Two consequences:

- **`Vmin` in `ATS` should describe the system, not the chemistry.** The table above puts cutoff at 3.00 V/cell = 15.0 V, but the Pi is already dead at 15.46 V. A gauge provisioned with `Vmin=15.0` reports charge remaining that no load can use, and its time-to-empty prediction overruns by the whole tail. For this pack plus this converter, **`Vmin=15.5` is the honest number.**
- **Measuring true cell capacity needs a different load.** Any capacity run through this converter is bounded by converter dropout, so it measures the system, not the pack. Use a bench load taken to the BMS cutoff if the cells' actual capacity is the question.

The general rule: on a battery-plus-converter system there are two distinct "empty" points, and the higher one wins. Provision for the one that matters to the user, which is almost always the point where their load stops working.

### 7.3.2 Tool packs do not charge to 4.20 V/cell

**Measured:** the 5S bench pack's charger goes to steady green and then **refuses to restart** on a re-insert after an overnight rest. Terminal voltage settles at **4.085-4.121 V/cell** (20.4-20.6 V), not the 4.20 V/cell (21.0 V) the chemistry table calls 100 percent. That is deliberate on tool packs - giving up ~8 percent of capacity buys a large gain in cycle life - and it means **the charger's green light, not the datasheet, defines full.**

Consequences, all of which are handled but worth knowing:

- **Provision `Vmax` to the charger's actual termination voltage.** With `Vmax=21.0` the charge-termination anchor needs `bus_v >= 20.9` (`gauge.c`, `vmax - 0.1`) and **can never fire on such a pack** - `mah_full` is never relearned and `full_charges` stays at 0 forever. `Vmax=20.6` fixes it.
- **The gauge learns the resting-full voltage by itself** and rescales the OCV curve to it, so SoC reads a true 100 at the top instead of ~92. See DEVELOPMENT_PLAN.md S4.4.1. It is reported as `v_full` in `ATA`.
- **Declare full once per pack.** Charging happens off-rig, so the INA228 never sees the charge current and the anchor never gets the chance. Issue `ATR` (or the button long press) after the charger goes green, with the pack reconnected and near rest. That is what teaches it `v_full`.
- Do this **promptly after charging.** The assume-full check refuses below 80 percent on the raw table, and a pack left on a load walks down past that.

Combining with S7.3.1, provisioning for a 5S3P tool pack becomes:

```
ATS=LiIon,5S3P,12000,15.5,20.6,<Imax>,<pack_id>
```

versus the `15.0,21.0` given in S7.1 - `Vmin` raised to the converter's dropout, `Vmax` lowered to the charger's termination.

### 7.3.3 Sizing `Imax` for a 12 Ah pack

The pack can source far more than the load draws, and `Imax` should follow the **load**, not the pack. Two reasons, both quantified:

| `Imax` | `CURRENT_LSB` (`Imax / 2^19`) | Shunt dissipation at that current |
|---|---|---|
| 2.5 A | 4.77 uA | 94 mW |
| 10 A | 19.1 uA | **1.5 W** |

The 15 mOhm shunt's ceiling is 10.9 A and S8 already flags 1.8 W as needing airflow, so `Imax=10` sits close to both limits while making resolution **4x coarser** on a load that actually draws ~150 mA. Keep `Imax` at 2.5-3 A for a Pi-class load. Raise it only with the load, and past ~10 A move to an external shunt (S3.4).

### 7.4 Multiple packs

A Makita-format pack implies an ecosystem of interchangeable packs. If you will rotate several, add a `pack_id` field to `ATS` **now** - retrofitting it is a breaking protocol change, whereas the per-pack ID chip (handoff S7.4) can come later without touching the protocol.

---

## 8. Power budget

The monitor's own draw - the only power this project is responsible for. Everything else on the battery is simply measured.

| Item | Draw |
|---|---|
| ESP32-C3, radios **disabled**, 160 MHz | ~22 mA @ 3.3 V ~ 73 mW |
| INA228, continuous conversion | ~0.6 mA @ 3.3 V ~ 2 mW |
| SuperMini onboard regulator (linear, 5->3.3 V) | ~40 mW |
| Buck loss + quiescent (S11.4) | ~20 mW |
| **Total from the battery** | **~135 mW ~ 7 mA @ 19 V** |

**This runs continuously while a battery is connected, with or without a host** - that is the design (S11.2). On the 216 Wh BL18120 it is ~1.5 %/day.

**It does not run in storage.** The monitor is not stored attached to a battery, so a disconnected battery loses nothing to it. The drain therefore only applies while the system is assembled and in use, where 1.5 %/day is negligible against the actual load. This is why the low-power sleep mode (plan S6) is documented but not planned.

The draw is itself measured by the INA228, so it is accounted for in the gauge rather than hidden.

WiFi and Bluetooth stay off. An active radio averages 80-100 mA and would multiply this roughly tenfold - which is also why OTA runs over `ATFW` on the serial link rather than a WiFi AP and HTTP upload page.

**Measured idle drain, bench rig.** A pack left connected overnight with the
monitor unpowered lost ~130 mAh in 10 h (**~13 mA**). That is 25x the DC-DC's
own idle draw measured at 0.37 mA, and ~15 %/day on a 2 Ah pack. Cause not yet
isolated - candidates are the converter idling higher than a single spot
reading suggested, and conduction through the INA228's protection structures
while `VS` was dead (S5.2). Until it is measured properly, treat the `Iq < 1 mA`
requirement in S11.4 as an **acceptance test to run on the actual module**, not
a datasheet figure to trust: at 13 mA the monitor's supply would dominate the
entire power budget.

> Superseded figures: earlier revisions gave ~20 mA/350 mW (too high - assumed a heavier CPU/radio load than this firmware has) and later ~0 mA standby (correct only for the abandoned host-powered topology).

---

## 9. Bring-up checklist

1. **Bench first, no pack.** 18 V lab supply, current-limited to 500 mA.
2. Confirm the Pi's 5 V rail is 5.0 V +/-5 % before wiring it to the SuperMini's `5V` pin. The onboard regulator then makes 3.3 V - check the `3V3` pin reads ~3.3 V before connecting the INA228.
2a. Confirm 4 MB flash: `esptool.py --port /dev/ttyACM0 flash_id`. The partition table in plan S6 assumes it.
3. ESP32 alone: `i2cdetect`-equivalent scan finds **0x40**.
4. Read `MANUFACTURER_ID` (0x3E) = `0x5449` ("TI") and `DEVICE_ID` (0x3F) = `0x228x`. If these fail, stop - nothing downstream is meaningful.
5. Bus voltage vs DMM at the battery terminals, no load: within 1 %. **A reading of 0 V means VBus is not wired** (S3.1.1) - current will still look correct, so check this explicitly.
6. Known resistive load (e.g. 18 Ohm / 20 W -> ~1 A): current vs DMM within 1%. **Check the sign** - plan S2.3 defines positive as discharge.
7. Only then attach the real pack, fuse installed, Pi's own DC-DC connected, Pi booting.
8. Unplug the Pi's USB for 10 minutes and confirm the ESP32 neither stalls nor reboots - this is the blocking-write trap in plan S1.2 and it must be tested deliberately.
9. **Power sequencing (bench only):** battery OFF before monitor power OFF;
   monitor power ON before battery ON. See S5.2 - leaving the INA228's inputs
   energised with `VS` dead is not a documented operating state.
10. Button/LED: double press -> SoC blinks match `ATA`. Long press on a full pack -> 5 blinks, `ATA` shows 100%. Long press on a half-empty pack -> flutter refusal, SoC **unchanged**.

---

## 10. Safety

- **Fuse the pack (+) before anything else**, including the INA228. A shorted shunt-sense wire is otherwise fed by the full pack.
- The pack needs its **own BMS** for per-cell balancing and over-discharge / over-charge / short protection. This board is a **capacity gauge, not a protection circuit** - it measures, it does not intervene (handoff S11).
- Never hot-plug the shunt sense wires while the power path is live.
- 18 V is not a shock hazard, but a 216 Wh Li-ion pack will deliver hundreds of amps into a short. **Li-ion NMC is markedly less tolerant of abuse than LiFePO4** - a shorted or crushed cell can go into thermal runaway rather than merely venting. Fuse it, don't puncture it, and don't charge it unattended.

---

## 11. Monitor power - self-powered from the battery

### 11.1 The topology

```
battery --> [INA228] --+--> the load (host, or anything else)
                       |
                       +--> buck 9-36V -> 5V --> SuperMini `5V` --> onboard VR --+--> ESP32-C3
                                                                                +--> INA228 VS
```

The monitor draws its own power from the battery it measures, through a wide-input buck into the SuperMini's `5V` pin. The board's onboard regulator makes 3.3 V for the C3, and the `3V3` pin supplies the INA228's VS via the QT cable's red wire. **No 3.3 V converter is needed** - the SuperMini already has one.

### 11.2 Why self-powered rather than USB-powered

The module mounts **at the battery**, with heavy wires to VIN+/VIN- already present, so a power tap costs one small buck and no extra wiring runs. Against that:

- **It is what makes coulomb counting meaningful.** Continuous accumulation, NVS persistence and the INA228's hardware charge register only earn their place if the device never stops counting. A USB-powered monitor could only read voltage and current at the moment you plug in - at which point the design collapses to an OCV estimate and most of the gauge logic is dead weight.
- **The host may be transient.** A Pi permanently powered from the same battery is the common case, but a laptop connecting for five minutes is equally valid for a universal monitor. Assuming the host is always present fails silently, as under-counting.
- **It works with no host at all.** The button and LED (S4.3) give a standalone SoC readout, so the module is useful on its own.

### 11.3 What it costs

~5 mA continuous, ~95 mW, or about **1 %/day of a 216 Wh battery**. That is the price of a gauge that never stops counting, and it is itself measured by the INA228, so it is accounted for rather than hidden.

**In practice it never runs unattended**, because the monitor is not stored connected to a battery - so this only applies while the system is in use, where it is negligible beside the actual load. A sleep mode could cut it to ~0.7 mA (plan S6) but is not planned.

### 11.4 Converter spec

| Parameter | Requirement |
|---|---|
| Input | **9-36 V** - covers 12 V AGM/lead-acid through 8S Li-ion |
| Output | 5.0 V (feeds the SuperMini's `5V` pin and its onboard regulator) |
| Current | >=200 mA (actual load ~30 mA) |
| **Quiescent current** | **< 1 mA - the spec that matters** |

At a ~30 mA load, efficiency is nearly irrelevant and **quiescent current dominates**. A converter idling at 5 mA burns more than the entire load, continuously. This rules out the otherwise-convenient **LM2596** (40 V-rated, but ~5-10 mA Iq and poor light-load efficiency).

Fit a bulk electrolytic (>=100 uF, >=50 V) and a TVS across the converter's battery input.

> **Voltage range vs. `ATS`.** The firmware accepts battery configurations well beyond what a 9-36 V buck can run from. If a battery outside that range is ever provisioned, the converter - not the firmware - is the limit. A 60 V-input part extends coverage to 48 V systems without any firmware change.

### 11.5 Modules assessed

| Module | Verdict |
|---|---|
| **mini360** (MP2307, 23 V abs max) | [no] Too low. Your own 21 V pack leaves 2 V under an *absolute maximum* |
| **mini560** (20 V max) | [no] Below the 21 V pack's full-charge voltage |
| **XL6009E1** | [no] Boost/buck-boost - wrong topology, and larger than the ESP32 and INA228 combined |
| **DD4012SA**, **STL6118A** | ! 5 V-output parts. Usable **only** if the printed input range clears ~30 V - read the silkscreen. Their 5 V output is exactly what the SuperMini's `5V` pin wants, so either would work directly if the rating holds |

**Check the DD4012SA and STL6118A input ratings first** - if one clears 30 V, you already have the part. Otherwise buy a 9-36 V -> 5 V module with low quiescent current.

### 11.6 Bench check before connecting the ESP32

1. Set/verify the buck output at **5.0 V +/-5 %**, at **both** the battery's minimum and maximum voltage - some modules' output shifts with input.
2. Wire it to the SuperMini's `5V` + `GND` only. **Do not also feed `3V3`**, and confirm the USB cable's VBUS wire is cut (S5) - `5V` and VBUS are the same net.
3. Check the `3V3` pin reads ~3.3 V before connecting the INA228's red wire.
4. Measure standby draw at the battery terminals with no host attached. Expect **~5 mA**; substantially more means the buck's quiescent current is the problem, not the firmware.
