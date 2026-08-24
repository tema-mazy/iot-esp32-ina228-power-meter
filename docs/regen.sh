#!/bin/sh
# Regenerate the measured charts embedded in the docs.
# SVG, not PNG: scales cleanly, adapts to light/dark via a media query inside
# the file, and stays pure ASCII so it does not break the docs' encoding rule.
set -e
cd "$(dirname "$0")/.."

python3 tools/svg_chart.py docs/discharge-curve.svg \
  discharge-run4-nopred.jsonl discharge-run4-capacity.jsonl \
  --field v --per-cell 5 --ylabel "V/cell" --label "5S Li-ion" \
  --caption "Discharge at ~150 mA, 5S1P 18650, from 94.6 percent"

python3 tools/svg_chart.py docs/ir-compensation.svg \
  discharge-run4-capacity.jsonl \
  --field v --field2 v_ocv --per-cell 5 --ylabel "V/cell" \
  --label measured --label2 "IR-corrected" \
  --caption "IR compensation with the firmware-learned R = 0.92 ohm"

python3 tools/svg_chart.py docs/load-hdmi.svg discharge-run1.jsonl \
  --field i --scale 1000 --ylabel "mA" --label "with HDMI" \
  --caption "Pi load with HDMI attached - transients trigger undervoltage"

python3 tools/svg_chart.py docs/load-nohdmi.svg discharge-run3.jsonl \
  --field i --scale 1000 --ylabel "mA" --label "without HDMI" \
  --caption "Same Pi, HDMI removed - same mean, 12x less spread"
