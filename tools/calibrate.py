#!/usr/bin/env python3
"""Close the prediction loop: measure real capacity, correct the next run.

    ./calibrate.py discharge.jsonl                 # report only
    ./calibrate.py discharge.jsonl --commit        # update calibration.json

A run is usable for calibration only if it actually reached empty. Scoring a
partial discharge would teach the model that the battery is smaller than it is.
"""

import argparse
import json
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from predict import Predictor          # noqa: E402


def load(path):
    rows = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                r = json.loads(line)
            except json.JSONDecodeError:
                continue
            if "v" in r and "ts" in r:
                rows.append(r)
    return rows


def consumed_mah(rows):
    """Sum per-sample deltas, skipping CHARGE-register resets on reboot."""
    total = 0.0
    resets = 0
    for a, b in zip(rows, rows[1:]):
        step = b["q_c"] - a["q_c"]
        if step < -1.0:
            resets += 1
            continue
        total += step
    return total / 3.6, resets


def main():
    ap = argparse.ArgumentParser(description="Calibrate capacity from a run")
    ap.add_argument("log")
    ap.add_argument("--commit", action="store_true",
                    help="write the new factor to calibration.json")
    ap.add_argument("--calibration", default="calibration.json")
    ap.add_argument("--series", type=int, default=5)
    ap.add_argument("--empty-v-per-cell", type=float, default=3.05,
                    help="below this the run counts as having reached empty")
    args = ap.parse_args()

    rows = load(args.log)
    if len(rows) < 10:
        sys.exit(f"only {len(rows)} samples, need at least 10")

    first, last = rows[0], rows[-1]
    used, resets = consumed_mah(rows)
    hours = (last["ts"] - first["ts"]) / 3600.0

    soc0 = first.get("soc")
    soc1 = last.get("soc")
    vpc_end = last["v"] / args.series
    reached_empty = vpc_end <= args.empty_v_per_cell or (soc1 is not None and soc1 <= 2)

    print(f"samples      : {len(rows)} over {hours:.2f} h"
          + (f"  ({resets} CHARGE reset(s) skipped)" if resets else ""))
    print(f"SoC          : {soc0:.1f}% -> {soc1:.1f}%" if soc0 is not None else "")
    print(f"V/cell       : {first['v'] / args.series:.3f} -> {vpc_end:.3f}")
    print(f"consumed     : {used:.1f} mAh")
    print()

    # Score the predictions that were recorded at the time, not recomputed.
    preds = [r for r in rows if r.get("pred", {}).get("empty_ts")]
    if preds:
        actual_end = last["ts"]
        errs = [(p["pred"]["empty_ts"] - actual_end) / 3600.0 for p in preds]
        early, late = errs[0], errs[-1]
        print(f"predictions  : {len(preds)} recorded")
        print(f"  first      : off by {early:+.2f} h")
        print(f"  last       : off by {late:+.2f} h")
        if not reached_empty:
            print("  (run has not reached empty - these are provisional)")
        print()

    if not reached_empty:
        print(f"NOT CALIBRATING: run ended at {vpc_end:.3f} V/cell, above the "
              f"{args.empty_v_per_cell} threshold.")
        print("A partial discharge would teach the model the pack is smaller "
              "than it is.")
        return 0

    # Usable capacity is what was delivered, scaled up for whatever was left
    # at the start. Requires a trustworthy starting SoC.
    if soc0 is None or soc0 <= 0:
        sys.exit("no starting SoC in the log, cannot infer full capacity")
    measured_full = used / (soc0 / 100.0)
    nominal = used / ((soc0 - (soc1 or 0)) / 100.0) if soc0 != soc1 else None

    print(f"measured full capacity : {measured_full:.0f} mAh "
          f"(delivered {used:.0f} mAh from {soc0:.1f}%)")
    if nominal:
        print(f"nominal implied        : {nominal:.0f} mAh")

    p = Predictor(args.calibration)
    old = p.factor
    if args.commit:
        # nominal_mah is what the gauge assumed; measured is what it delivered.
        assumed = used / ((soc0 - (soc1 or 0)) / 100.0)
        new = p.record_run(assumed, measured_full, note=args.log)
        print(f"\ncapacity factor: {old:.4f} -> {new:.4f}  "
              f"(averaged over {len(p.runs)} run(s), written to {args.calibration})")
    else:
        print(f"\ncapacity factor would change from {old:.4f} "
              f"(run with --commit to apply)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
