# Session Handoff - 2026-08-25 - Full discharge run 5 to converter dropout, docs updated

## What was done

- Monitored a 9.58 h discharge of the 5S1P 18650 Li-ion pack (`bp18650`) to the
  point the load died. 578 samples, 80.4 to 3.7 percent, 1532.6 mAh delivered.
  Archived as `discharge-run5-dropout.jsonl` (was `discharge.jsonl` during the run).
- Diagnosed the run termination: the Pi's DC-DC fell out of regulation at
  **15.456 V (3.091 V/cell) under 193 mA**. Not the BMS, not the cells. Evidence:
  current dropped 193 mA to 2.2 mA in one 60 s interval, then pack voltage
  *rebounded* +47 mV/cell over 13 min. A pack that recovers is not empty.
- Ran `tools/calibrate.py discharge.jsonl` (dry run). It **refused**, correctly:
  run started at 80.4 percent and ended at 3.138 V/cell, above its 3.05 threshold.
  No `capacity_factor` was written. `calibration.json` is untouched.
- Stopped everything on request: cron job `a308aa1e` (5-min status reports)
  cancelled via CronDelete; `monitor.py` and `plot_discharge.py` killed.
- Docs updated and committed as `7c8e497`, pushed to `origin/main`:
  - `DEVELOPMENT_PLAN.md` S5.9.1 - run 5, slope table across the knee, R rising
    0.92 to 1.02 ohm with depth of discharge.
  - `DEVELOPMENT_PLAN.md` S5.9.2 - why the run does NOT measure capacity.
  - `hardware.md` S7.3.1 - the converter sets the floor, not the chemistry.
  - `hardware.md` S5.2.1 - gauge zeroing when the pack is pulled while powered.
  - New charts `docs/discharge-knee.svg`, `docs/discharge-soc.svg`.
- Fixed three pre-existing defects found while doing the above (details below).

## What was successful

- **Rasterizing SVGs and actually looking at them.** `rsvg-convert -w 1200 -b white
  docs/X.svg -o scratch/X.png` then Read the PNG. This is what caught the flattened
  y-axis; no amount of reading the SVG source would have. `/opt/local/bin/rsvg-convert`
  is installed via macports. Note `qlmanage -t` silently produced nothing - do not
  use it. Also note rsvg-convert does NOT create the output directory; `mkdir -p` first.
- **Three independent corroborations of the gauge** from the pack's own 4-LED
  indicator: 2 bars at 34.3 percent (band 25-50), the 2-to-1 transition at 27.0
  percent against a predicted 25, 1 bar blinking at 3.7 percent. This is the only
  external reference available for SoC and it is worth continuing to record.
- **Trusting `calibrate.py`'s refusal rather than overriding it.** Its two guards
  (must start full, must end below 3.05 V/cell) both fired for good reason.
- Keeping the raw log intact and filtering at plot time (`--min-bus-v`) rather than
  deleting rows from the jsonl.

## What went wrong - do NOT repeat

- **`docs/regen.sh` had been silently broken.** It referenced
  `discharge-run4-capacity.jsonl`, a file renamed to `discharge.jsonl` mid-run and
  never present under that name, so the very first `svg_chart.py` call raised
  FileNotFoundError. Worse, `sh docs/regen.sh` reported `exit=0` because the
  `echo "exit=$?"` measured the exit of the pipeline's `tail`, not the script.
  Lesson: when checking a script's exit status, do not pipe it. Fixed in `7c8e497`.
- **Charts were committed but their source data was gitignored** (`*.jsonl`), so
  `regen.sh` was never runnable from a clone and every measured claim in S5.9 was
  unverifiable. Added `!discharge-run*.jsonl` to `.gitignore` and tracked the five
  archived runs (~310 KB). The live `discharge.jsonl` stays ignored.
- **First `discharge-knee.svg` was unreadable** - a flat line with a vertical spike
  to zero. Cause: the last two rows of the log were taken after the pack was
  physically disconnected (bus 0.039 V then 0.011 V, `err:0`), which dragged the
  auto-ranged y-axis down to 0 and compressed the real 3.1-4.0 V range into nothing.
  Fixed with `svg_chart.py --min-bus-v`. Do not assume a log ends with valid data.
- **First attempt at the filter was wrong in a way that violated the dataviz rules.**
  I made it filter on `--field`, then wired the SoC chart as `--field v --field2 soc`
  to reuse it - producing a dual-axis chart mixing V/cell and percent on one scale.
  Rewrote the option to key off bus voltage regardless of what is plotted. Never
  reach for a second scale to make a filter work.
- **Do not quote 1998 mAh as a capacity measurement.** 1532.6 mAh over 76.7 percent
  of SoC implies it, and it matches the 2000 mAh label almost exactly, which makes
  it very tempting. It is circular: `soc` is the coulomb count divided by the
  2000 mAh handed to `ATS`, so the arithmetic can only ever return the label.
- Earlier in the session (pre-compaction) two wrong guesses about the overnight
  130 mAh drain (Pi standby, then INA228 ESD with VS dead) were both refuted by the
  user - the Pi and the ESP were both physically disconnected. Still unattributed.
  Do not guess a third time; run the differential test in Next steps.

## Current state

- Branch `main`, clean, in sync with `origin/main` at `7c8e497`.
- **Battery is at empty and should be on charge.** Last reading 15.69 V resting
  (3.138 V/cell). Li-ion should not sit here.
- Pack was physically disconnected from the INA228 at the end of the run. The
  gauge latched `soc:0.0`, `mah_left:0`, `mah_used:2000` from the 0 V reading.
  This self-corrects on reconnect (a charged pack at ~21 V is far outside
  `GAUGE_SAME_PACK_MV_PER_CELL` of the stored `last_v`, so it re-seeds from OCV).
  No action needed, but expect the first post-reconnect `ATA` to look odd briefly.
- No logger, no dashboard, no cron running.
- `calibration.json` still has no `capacity_factor` - the prediction loop the user
  asked for ("adjust prediction next run") is still open, pending a qualifying run.

## Next steps

1. **Charge the pack fully.** Confirm ~21 V at the terminals before the next run.
2. **Decide what the next run should measure** (see Open questions). This changes
   the rig, so decide before starting.
3. Start a fresh single-file log for the whole cycle. Do NOT rename or rotate the
   file mid-run - a previous session fragmented a capacity run that way and the
   user's explicit ask was "i though i will get full stat from full charge to 0":
   ```
   python3 tools/monitor.py '/dev/cu.usbmodem*' -i 60 --json > discharge.jsonl
   ```
4. When it reaches cutoff:
   ```
   python3 tools/calibrate.py discharge.jsonl --commit
   ```
   It will refuse unless the run started full and ended below 3.05 V/cell.
5. Archive as `discharge-run6-*.jsonl`, add it to `docs/regen.sh`, rerun
   `sh docs/regen.sh`, and rasterize the output to check it before committing.
6. **Differential test for the unattributed overnight drain**, still owed: Pi off,
   DC-DC connected to the battery, monitor powered, log 30 min. ~0.5 mA acquits the
   converter; ~13 mA convicts it.
7. Still owed from Phase 1: verify V and I against a DMM within 1 percent.
8. Still owed: S1.2 blocking-write trap - leave the host disconnected 10 minutes
   and confirm the firmware does not stall.

## Open questions / risks

- **What should the next discharge measure?** Two different questions, two rigs:
  - Through the Pi again: measures usable system runtime, bounded by converter
    dropout at ~15.46 V. Cell capacity stays unmeasured.
  - Through a bench load to BMS cutoff: measures actual pack health.
  Only the second can ever satisfy `calibrate.py`.
- **Should `ATS` `Vmin` change from 15.0 to 15.5 V?** Proposed in `hardware.md`
  S7.3.1 but NOT applied. At 15.0 the gauge reports charge no load can reach and
  time-to-empty overruns by the whole tail. Counter-argument: 15.5 is specific to
  this pack-plus-converter pair, and re-provisioning changes the stored NVS record.
  Needs the user's call. Command would be:
  `ATS=LiIon,5S1P,2000,15.5,21.0,2.5,bp18650`
- **Firmware hardening not implemented:** the gauge will seed SoC from a ~0 V
  reading when the pack is absent. Suggested guard is to refuse OCV seeding below
  ~0.5 V/cell and log the disconnect instead (`main/gauge.c:seed_from_ocv`). Present
  behaviour is harmless only because the same-pack check catches it afterwards.
- The overnight ~13 mA drain remains unexplained after ruling out the Pi, the ESP,
  pack self-discharge, and the DC-DC (measured 0.37 mA idle, 25x too small).
- User feedback worth keeping permanently (belongs in memory, not just here): the
  ASCII-only rule for all written files, and "verify before recommending" - do the
  arithmetic and read the datasheet before advising. Both are already in
  `~/.claude/projects/.../memory/`.
