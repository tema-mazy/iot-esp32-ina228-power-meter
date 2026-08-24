#!/usr/bin/env python3
"""Runtime prediction with cross-run calibration.

The gauge reports mAh remaining against a NOMINAL capacity (the ATS label).
Real usable capacity differs - cells are rarely what the label claims, and it
changes with age and temperature. This module:

  1. predicts time-to-empty at each sample and records it in the log,
  2. learns a capacity correction factor once a run reaches empty,
  3. applies that factor to the next run's predictions.

Predictions are stored AS MADE. Recomputing them later from a corrected model
would score the model against itself and always look good.

State lives in calibration.json next to the logs.
"""

import json
import os
import time

CALIB_DEFAULT = "calibration.json"


class Predictor:
    """Predicts remaining runtime, corrected by past measured capacity."""

    def __init__(self, path=CALIB_DEFAULT, window=10):
        self.path = path
        self.window = window          # samples averaged for the current estimate
        self._recent = []
        self.factor = 1.0             # measured capacity / nominal capacity
        self.runs = []
        self._load()

    def _load(self):
        if not os.path.exists(self.path):
            return
        try:
            with open(self.path) as f:
                data = json.load(f)
        except (OSError, json.JSONDecodeError):
            return
        self.factor = float(data.get("capacity_factor", 1.0))
        self.runs = data.get("runs", [])

    def save(self):
        with open(self.path, "w") as f:
            json.dump({"capacity_factor": self.factor, "runs": self.runs},
                      f, indent=2)
            f.write("\n")

    def predict(self, r):
        """Return the prediction block to embed in a log record."""
        i = r.get("i", 0.0)
        self._recent.append(i)
        if len(self._recent) > self.window:
            self._recent.pop(0)
        i_avg = sum(self._recent) / len(self._recent)

        mah_left = r.get("mah_left")
        if mah_left is None:
            return None

        # The correction is what makes this a closed loop: a factor below 1.0
        # means past runs delivered less than the label promised.
        mah_adj = mah_left * self.factor
        if i_avg > 0.001:
            hours = mah_adj / (i_avg * 1000.0)
        else:
            hours = None

        return {
            "mah_left_adj": round(mah_adj, 1),
            "i_avg": round(i_avg, 5),
            "h_to_empty": round(hours, 3) if hours is not None else None,
            "empty_ts": round(r["ts"] + hours * 3600, 1) if hours is not None else None,
            "factor": round(self.factor, 4),
        }

    def record_run(self, nominal_mah, measured_mah, note=""):
        """Fold a completed discharge into the correction factor.

        Averaged over runs rather than replacing outright: a single discharge
        that ended early (a BMS cutoff, a load change) should not overwrite
        everything learned so far.
        """
        if nominal_mah <= 0 or measured_mah <= 0:
            return self.factor
        f = measured_mah / nominal_mah
        self.runs.append({
            "ts": round(time.time()),
            "nominal_mah": round(nominal_mah, 1),
            "measured_mah": round(measured_mah, 1),
            "factor": round(f, 4),
            "note": note,
        })
        factors = [x["factor"] for x in self.runs][-5:]
        self.factor = sum(factors) / len(factors)
        self.save()
        return self.factor
