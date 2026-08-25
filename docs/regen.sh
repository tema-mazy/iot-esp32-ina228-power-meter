#!/bin/sh
# Regenerate the measured charts embedded in the docs.
# SVG, not PNG: scales cleanly, adapts to light/dark via a media query inside
# the file, and stays pure ASCII so it does not break the docs' encoding rule.
set -e
cd "$(dirname "$0")/.."

python3 tools/svg_chart.py docs/discharge-curve.svg \
  discharge-run4-nopred.jsonl discharge-run5-dropout.jsonl \
  --field v --per-cell 5 --ylabel "V/cell" --label "5S Li-ion" \
  --min-bus-v 1.0 \
  --caption "Discharge at ~150 mA, 5S1P 18650, plateau through the knee"

python3 tools/svg_chart.py docs/discharge-knee.svg \
  discharge-run5-dropout.jsonl \
  --field v --per-cell 5 --ylabel "V/cell" --label "5S Li-ion" \
  --min-bus-v 1.0 \
  --caption "Full 9.6 h run to converter dropout - the knee below 3.4 V/cell"

python3 tools/svg_chart.py docs/discharge-soc.svg \
  discharge-run5-dropout.jsonl \
  --field soc --ylabel "percent" --label "coulomb SoC" --min-bus-v 1.0 \
  --caption "Coulomb-counted SoC over the same run, 80.4 to 3.7 percent"

python3 tools/svg_chart.py docs/ir-compensation.svg \
  discharge-run5-dropout.jsonl \
  --field v --field2 v_ocv --per-cell 5 --ylabel "V/cell" \
  --label measured --label2 "IR-corrected" --min-bus-v 1.0 \
  --caption "IR compensation with the firmware-learned R = 0.92 ohm"

python3 tools/svg_chart.py docs/load-hdmi.svg discharge-run1.jsonl \
  --field i --scale 1000 --ylabel "mA" --label "with HDMI" \
  --caption "Pi load with HDMI attached - transients trigger undervoltage"

python3 tools/svg_chart.py docs/load-nohdmi.svg discharge-run3.jsonl \
  --field i --scale 1000 --ylabel "mA" --label "without HDMI" \
  --caption "Same Pi, HDMI removed - same mean, 12x less spread"
