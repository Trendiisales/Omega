#!/bin/bash
# Build bundled warmup CSVs for SurvivorPortfolio cells from /Users/jo/Tick/
# data. Output to phase1/signal_discovery/.
# Format: ts(sec),o,h,l,c -- matches SurvivorPortfolio Bar loader.
set -e
OUT=phase1/signal_discovery

resample() {
  local sym="$1" src="$2" tf_sec="$3" tf_tag="$4"
  local out_path="$OUT/warmup_${sym}_${tf_tag}.csv"
  echo "[build] $sym $tf_tag -> $out_path"
  awk -F, -v tf="$tf_sec" '
    NR>1 {
      ts = int($1/1000)
      mid = ($2 + $3) / 2
      b = int(ts / tf) * tf
      if (b != cur) {
        if (cur > 0) printf "%d,%.5f,%.5f,%.5f,%.5f\n", cur, o, hi, lo, c
        cur = b; o = mid; hi = mid; lo = mid; c = mid
      } else {
        if (mid > hi) hi = mid
        if (mid < lo) lo = mid
        c = mid
      }
    }
    END { if (cur > 0) printf "%d,%.5f,%.5f,%.5f,%.5f\n", cur, o, hi, lo, c }
  ' "$src" > "$out_path".tmp
  echo "ts,o,h,l,c" > "$out_path"
  cat "$out_path".tmp >> "$out_path"
  rm "$out_path".tmp
  wc -l "$out_path"
}

# GER40 multi-TF
resample GER40 /Users/jo/Tick/GER40_merged.csv 300  M5
resample GER40 /Users/jo/Tick/GER40_merged.csv 1800 M30
resample GER40 /Users/jo/Tick/GER40_merged.csv 900  M15

# USTEC = NSXUSD historical proxy
resample USTEC.F /Users/jo/Tick/NSXUSD_merged.csv 14400 H4

# USDJPY H4
resample USDJPY /Users/jo/Tick/USDJPY_merged.csv 14400 H4

# SPXUSD H4 (for SPX overlay)
resample SPXUSD /Users/jo/Tick/SPXUSD_merged.csv 14400 H4

echo "[done]"
ls -la $OUT/warmup_*.csv | tail -10
