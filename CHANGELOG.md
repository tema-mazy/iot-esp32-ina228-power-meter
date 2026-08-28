# Changelog

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versions are git tags; `release.sh` reads the section matching the tag it is
building and copies it into that release's notes, so keep the headings exactly
as `## [x.y.z] - YYYY-MM-DD`.

Measured figures cite the run they came from. Runs are the archived
`discharge-run*.jsonl` logs, and the analysis behind them is in
DEVELOPMENT_PLAN.md S5.9.

## [Unreleased]

Nothing released yet. Everything below shipped to `main` but carries no tag,
so builds identify themselves over `ATI` by commit SHA rather than by version.

### Added

- **Per-pack learned full voltage** (`gauge.c`). The OCV table's 100 % is
  4.20 V/cell, which tool packs never reach - they commonly terminate near
  4.10 to buy cycle life. The measured 5S pack settles at 4.085-4.121 V/cell
  with the charger showing green and refusing to restart, which reads as 92 %
  on the raw table. The gauge now learns each pack's own resting-full voltage
  and rescales the curve to it, so a full pack reads 100 %.
  - Learned from `ATR`, the button long press, or the charge anchor firing.
  - Also learned **hands-off**, as the highest settled resting voltage the
    record has seen, after 20 minutes continuously at rest. Nobody in the
    field sends `ATR`, and the settle requirement is what rejects surface
    charge: straight off the charger a cell reads 4.15-4.18 and decays toward
    its true value over tens of minutes.
  - Only ratchets up, which is the safe direction - a low `v_full`
    under-reports rather than claiming charge that is not there.
- **`v_full` in the `ATA` response.** On a fleet of packs it is the field that
  says what each charger actually does.
- **`svg_chart.py --bucket-min`**, `--min-bus-v`, `--scale2`, `--ylabel2`.
- **Right-hand second axis** on the dashboard's load panel and in
  `docs/load-current-power.svg`.
- **External shunt support** for currents above the onboard 15 mOhm shunt's
  10.9 A ceiling (hardware.md S3.4).

### Changed

- **Assume-full is judged on table percent, not a fixed voltage.** The old
  test worked out to 4.12 V/cell for Li-ion and refused exactly the tool packs
  it is meant to serve; the measured pack cleared it by 1 mV. Now `raw_soc >=
  80 %`, which carries the same intent without hardcoding one chemistry's
  ceiling.
- `GAUGE_VERSION` 1 -> 2. Version and size mismatches were already rejected by
  `storage_load_gauge`, so the gauge re-seeds from OCV once after the upgrade
  and nothing needs migrating.
- OCV seeding is now IR-compensated. Seeding from a loaded voltage charges the
  whole IR drop against state of charge: on a 0.94 ohm pack at 300 mA that is
  56 mV/cell, worth 5.6 % SoC.

### Fixed

- **A disconnected pack no longer zeroes the stored count.** The empty clamp
  tested `bus_v <= vmin` with no lower bound, so an absent battery reading
  0.077 V was trivially "at or below Vmin" and the record was rewritten to
  0 mAh with `mah_used` at full capacity. Observed twice. Now requires the pack
  to be present at all, `0.5 V/cell x series`. Cannot arise in the shipping
  topology, where the monitor is powered by the pack and dies with it; it is
  specific to the split-powered bench rig, which is where the firmware gets
  developed.
- `docs/regen.sh` referenced `discharge-run4-capacity.jsonl`, a file renamed
  mid-run and never present under that name, so the script had been failing on
  its first command.
- Archived run logs are tracked, so `regen.sh` runs from a fresh clone and the
  measured claims in the docs can be checked.

### Measured

- **Capacity 1885 mAh**, 94 % of the 2000 mAh label, charger-full to BMS
  cutoff over 12.17 h (run 7). Not `calibrate.py`'s 1968: that divides by
  `soc0 = 95.8 %`, which came from an OCV seed against a ceiling this pack's
  charger never reaches.
- **Three floors, not one.** Surge headroom fails at ~10 % SoC; converter
  dropout at 15.46 V (confirmed twice: 15.456 V run 5, 15.4641 V run 7); BMS
  protection at 13.79 V under 192 mA. `R_total` rises 0.965 -> 1.114 ohm across
  a discharge, so a 1.5 A boot surge exhausts the converter headroom long
  before steady-state draw would.
- **The load draws constant power**, so current rises as the pack sags: -12.1 %
  voltage against +18.6 % current, power moving only +4.2 %.
- **Time-to-empty error is 19.7 % / 11.6 %** with the present `mah_left / I`,
  and 3.4 % / 3.0 % using energy at mean remaining voltage over power,
  validated on two independent runs.

## Pre-release history

Phases as built, from the git log. No tags, so these are not versions.

- **2026-08-13** Initial commit.
- **2026-08-18** Universal battery monitor: design docs, firmware phases 0-3
  and 6, test suite. AT transport over USB Serial/JTAG, ring-buffer logging,
  INA228 driver, LED and button.
- **2026-08-18** Phase 7: host-side reader (`tools/monitor.py`), systemd
  example, docs.
- **2026-08-24** Phase 4: `ATS` battery provisioning with per-pack NVS records.
- **2026-08-24** Phase 5: fuel gauge, prediction loop, live dashboard,
  measured charts.

## Planned

- **Method C for time-to-empty** - energy at mean remaining voltage over mean
  power, `tools/predict.py:69`. Single site; there is no firmware estimate.
  Validated at ~3 % against ~20 %.
- **Provision `Vmin` to the real system floor** rather than the chemistry's.
  15.5 V for this pack and converter, against the 15.0 currently documented.
- **Make `Vmax` define 100 % for the charge anchor** on tool packs: with
  `Vmax=21.0` the anchor needs 20.9 V and can never fire on a pack that tops
  out at 20.6.
