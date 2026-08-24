#!/usr/bin/env python3
"""Summarise a discharge log produced by monitor.py --json.

    ./monitor.py PORT --json --interval 60 >> discharge.jsonl
    ./discharge_report.py discharge.jsonl

Cross-checks the INA228's hardware charge accumulator against the elapsed
time and mean current, and reports SoC from the resting-OCV table. Useful for
validating the gauge against a real discharge before trusting it.
"""

import json
import sys

# Li-ion NMC resting OCV, volts per cell -> percent. See DEVELOPMENT_PLAN S4.
OCV_LIION = [(4.20, 100), (4.10, 90), (4.00, 80), (3.90, 70), (3.82, 60),
             (3.72, 50), (3.65, 40), (3.58, 30), (3.50, 20), (3.40, 10),
             (3.20, 5), (3.00, 0)]


def soc_from_ocv(v_per_cell, table=OCV_LIION):
    if v_per_cell >= table[0][0]:
        return 100.0
    if v_per_cell <= table[-1][0]:
        return 0.0
    for (v_hi, s_hi), (v_lo, s_lo) in zip(table, table[1:]):
        if v_lo <= v_per_cell <= v_hi:
            f = (v_per_cell - v_lo) / (v_hi - v_lo)
            return s_lo + f * (s_hi - s_lo)
    return 0.0


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    series = int(sys.argv[2]) if len(sys.argv) > 2 else 5
    capacity_mah = float(sys.argv[3]) if len(sys.argv) > 3 else 2000.0

    rows = []
    with open(sys.argv[1]) as f:
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

    if len(rows) < 2:
        sys.exit(f"need at least 2 samples, got {len(rows)}")

    first, last = rows[0], rows[-1]
    dt_s = last["ts"] - first["ts"]

    # The INA228 CHARGE register is volatile: it resets to zero on every
    # device reboot (ATZ, OTA, or a battery disconnect). Summing per-sample
    # deltas and ignoring the negative jumps keeps the total correct across
    # a restart, where last-minus-first would report a large negative value.
    dq_c = 0.0
    resets = 0
    for a_, b_ in zip(rows, rows[1:]):
        step = b_["q_c"] - a_["q_c"]
        if step < -1.0:          # counter went backwards: a reset, not a charge
            resets += 1
            continue
        dq_c += step
    if resets:
        print(f"note        : {resets} CHARGE reset(s) detected and skipped "
              f"(device rebooted mid-log)")
    dmah = dq_c / 3.6
    currents = [r["i"] for r in rows]
    volts = [r["v"] for r in rows]

    print(f"samples     : {len(rows)} over {dt_s / 3600:.2f} h")
    print(f"V           : {first['v']:.4f} -> {last['v']:.4f}  "
          f"(drop {first['v'] - last['v']:.4f} V)")
    print(f"V/cell ({series}S) : {first['v'] / series:.3f} -> "
          f"{last['v'] / series:.3f}")
    print(f"I           : mean {sum(currents) / len(currents) * 1000:.1f} mA, "
          f"min {min(currents) * 1000:.1f}, max {max(currents) * 1000:.1f}")
    print(f"T           : {min(r['t'] for r in rows):.1f} - "
          f"{max(r['t'] for r in rows):.1f} C")
    print()
    print(f"charge used : {dmah:.2f} mAh  ({dq_c:.1f} C)")
    if dt_s > 0:
        # The accumulator is the truth; the sampled mean aliases a varying
        # load. A large gap between these two is expected, not an error.
        print(f"  accumulator average : {dq_c / dt_s * 1000:.2f} mA")
        print(f"  sampled mean        : "
              f"{sum(currents) / len(currents) * 1000:.2f} mA")
    print()

    soc_start = soc_from_ocv(first["v"] / series)
    soc_end = soc_from_ocv(last["v"] / series)
    print(f"SoC by OCV  : {soc_start:.1f}% -> {soc_end:.1f}%  "
          f"(delta {soc_start - soc_end:.1f}%)")
    print(f"  as mAh    : {(soc_start - soc_end) / 100 * capacity_mah:.1f} mAh")
    print(f"  coulombs  : {dmah:.1f} mAh")
    print()
    print("NOTE: OCV under load reads low by I x R_total. On the 5S1P 18650")
    print("pack R_total is ~1 ohm, so at 130 mA the voltage sags ~0.13 V,")
    print("about 1.5% SoC. Compare the two figures only at rest, or apply")
    print("IR compensation first (DEVELOPMENT_PLAN S4).")

    if volts[-1] < volts[0]:
        rate = (volts[0] - volts[-1]) / (dt_s / 3600) if dt_s else 0
        print(f"\ndischarge rate: {rate:.4f} V/h at this load")


if __name__ == "__main__":
    main()
