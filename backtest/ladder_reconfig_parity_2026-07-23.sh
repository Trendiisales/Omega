#!/usr/bin/env bash
# Runner for backtest/ladder_reconfig_parity_2026-07-23.cpp — the 4 ladder-reconfig
# C++ parity certs of BULLGATE_PROTECTION_SWEEPS_2026-07-23.md (build queue 1-4).
# Each cert runs a *_parity variant (python-sweep architecture: no weekend layers,
# python entry arch) and a *_task/live variant (the config as it would ship on the
# live cell: weekend Layer2+3, live BE-ENTRY where applicable).
# gb mapping: engine wide_gb_frac = giveback = 1 - python_keep_g (sweep_ladder_h1
# "g70" -> 0.30; "g50" -> 0.50; nas_ladder_sweep2 g50 -> 0.50 directly).
set -euo pipefail
cd "$(dirname "$0")/.."
BIN=/tmp/ladder_parity
g++ -std=c++17 -O2 -Iinclude -o "$BIN" backtest/ladder_reconfig_parity_2026-07-23.cpp
WD=$(mktemp -d /tmp/ladder_parity_runs.XXXXXX)
T=/Users/jo/Tick
run() { "$BIN" "$@" | grep '^RESULT'; }

echo "== CERT 1: NAS100 replacement (W48/thr2.0 rt3, confirm 1.0%=0.5thr, g50, cap0->engine cap1) =="
run $T/NSXUSD_2022_2026.h1.csv 48 2.0 3.0 0.50 -1 1.0 48 1 daily200 $T/SPX_daily_2016_2026.csv 1 0.0 $WD c1_spx_task
run $T/NSXUSD_2022_2026.h1.csv 48 2.0 3.0 0.50 -1 1.0 48 1 daily200 $T/SPX_daily_2016_2026.csv 0 1.0 $WD c1_spx_parity
run $T/NSXUSD_2022_2026.h1.csv 48 2.0 3.0 0.50 -1 1.0 48 1 daily200 $T/NDX_daily_2016_2026.csv 1 0.0 $WD c1_ndx_task
run $T/NSXUSD_2022_2026.h1.csv 48 2.0 3.0 0.50 -1 1.0 48 1 daily200 $T/NDX_daily_2016_2026.csv 0 1.0 $WD c1_ndx_parity

echo "== CERT 2: US500 upgrade (W24/thr2.0 rt4, dma200 own-H1 gate, keep50 -> gb0.50, cap5) =="
run $T/SPXUSD_2022_2026.h1.csv 24 2.0 4.0 0.50 -1 0.08 4 5 dma200h1 - 1 0.0 $WD c2_live
run $T/SPXUSD_2022_2026.h1.csv 24 2.0 4.0 0.50 -1 0.0  4 5 dma200h1 - 0 1.0 $WD c2_parity
run $T/SPXUSD_2022_2026.h1.csv 24 2.0 4.0 0.30 -1 0.0  4 5 dma200h1 - 0 1.0 $WD c2_keep70

echo "== CERT 3: GBPUSD migrate (W48/thr1.0 rt2, keep70 -> gb0.30, cap5, NO gate) =="
run $T/GBPUSD_befloor_h1.csv 48 1.0 2.0 0.30 -1 0.0  4 5 none - 0 1.0 $WD c3_parity
run $T/GBPUSD_befloor_h1.csv 48 1.0 2.0 0.30 -1 0.08 4 5 none - 1 0.0 $WD c3_livearch

echo "== CERT 4: GER40 in-bull upgrade (W12/thr1.5 rt2, volcalm 24/240, keep50 -> gb0.50, cap5) =="
run $T/GRXEUR_merged.h1.csv 12 1.5 2.0 0.50 -1 0.0 4 5 volcalm - 0 1.0 $WD c4_parity
run $T/GRXEUR_merged.h1.csv 12 1.5 2.0 0.50 -1 0.0 4 5 volcalm - 1 0.0 $WD c4_wknd

echo "workdir: $WD"
