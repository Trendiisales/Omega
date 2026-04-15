#!/usr/bin/env python3
# =============================================================================
# hmm_refit.py -- Hourly HMM regime classifier for Omega XAUUSD
#
# Runs on the Windows VPS via Task Scheduler (hourly).
# Reads H1/H4 bar close prices from OHLCBarEngine saved .dat files or
# falls back to the live L2 tick CSV logs to derive close prices.
#
# Fits a 3-state Gaussian HMM on log-returns:
#   State 0 = CHOPPY    (low volatility, mean-reverting, small returns)
#   State 1 = TRENDING  (directional, medium-high vol, signed drift)
#   State 2 = VOLATILE  (high volatility, large erratic returns)
#
# Outputs C:\Omega\hmm_state.json with:
#   {
#     "state":     0|1|2,
#     "label":     "CHOPPY"|"TRENDING"|"VOLATILE",
#     "p_stay":    0.0-1.0,   -- P(stay in current state next bar)
#     "p_flip":    0.0-1.0,   -- 1 - p_stay
#     "vol_regime":0.0-1.0,   -- normalised recent volatility (0=low, 1=high)
#     "ts_utc":    1234567890, -- Unix timestamp of refit
#     "bars_used": 90,
#     "fit_ok":    true|false
#   }
#
# The C++ HmmRegimeReader.hpp polls this file every 30s.
# If the file is >90 minutes stale, C++ treats HMM as UNKNOWN and skips gating.
#
# Usage:
#   python3 C:\Omega\scripts\hmm_refit.py
#
# Task Scheduler: run hourly, start on system startup.
# Working dir: C:\Omega
# =============================================================================

import json
import math
import os
import struct
import sys
import time
import glob
from datetime import datetime, timezone

import numpy as np
from hmmlearn import hmm

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------
OMEGA_DIR      = r"C:\Omega"
OUTPUT_FILE    = os.path.join(OMEGA_DIR, "hmm_state.json")
LOG_FILE       = os.path.join(OMEGA_DIR, "logs", "hmm_refit.log")

# Number of H1 log-return bars to use for fitting (90 = ~3.75 days)
H1_WINDOW      = 90
# Minimum bars required to fit -- below this, emit fit_ok=false
MIN_BARS       = 40
# HMM components
N_STATES       = 3
# Random seed for reproducibility across refits
RANDOM_SEED    = 42
# Max EM iterations
MAX_ITER       = 200

# State label assignment heuristic:
#   After fitting, sort states by emission variance.
#   Lowest variance  = CHOPPY
#   Middle variance  = TRENDING
#   Highest variance = VOLATILE
STATE_LABELS   = ["CHOPPY", "TRENDING", "VOLATILE"]

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------
def log(msg: str):
    ts = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")
    line = f"[{ts}] {msg}"
    print(line, flush=True)
    try:
        os.makedirs(os.path.dirname(LOG_FILE), exist_ok=True)
        with open(LOG_FILE, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except Exception:
        pass

# ---------------------------------------------------------------------------
# Data loading: parse Omega .dat indicator files (binary) or fall back to CSV
# ---------------------------------------------------------------------------

def load_closes_from_dat(dat_path: str) -> list[float]:
    """
    Omega saves BarIndicators state to a binary .dat file via save_indicators().
    The .dat format is not documented here -- it only stores indicator scalars,
    not raw bar history. We cannot reconstruct H1 closes from it.
    Return empty list to fall through to CSV loader.
    """
    return []


def load_closes_from_l2_csv(timeframe_minutes: int, n_bars: int) -> list[float]:
    """
    Derive pseudo-OHLC close prices from the L2 tick CSV logs.
    Reads C:/Omega/logs/l2_ticks_YYYY-MM-DD.csv files (newest first),
    buckets ticks into timeframe_minutes bars, returns last n_bars closes.

    CSV format (from CTraderDepthClient):
      timestamp_ms, mid, bid, ask, imbalance, depth_events_total
    """
    log_dir = os.path.join(OMEGA_DIR, "logs")
    pattern = os.path.join(log_dir, "l2_ticks_*.csv")
    files = sorted(glob.glob(pattern), reverse=True)  # newest first

    ticks: list[tuple[int, float]] = []  # (ts_ms, mid)
    for fpath in files[:5]:  # look back up to 5 days
        try:
            with open(fpath, "r", encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if not line or line.startswith("#") or line.startswith("timestamp"):
                        continue
                    parts = line.split(",")
                    if len(parts) < 3:
                        continue
                    ts_ms = int(parts[0])
                    mid   = float(parts[1])
                    if mid > 0:
                        ticks.append((ts_ms, mid))
        except Exception as e:
            log(f"  CSV read error {fpath}: {e}")
            continue
        if len(ticks) > n_bars * timeframe_minutes * 60 * 5:
            break  # enough data

    if len(ticks) < 2:
        return []

    # Sort ascending by time
    ticks.sort(key=lambda x: x[0])

    tf_ms = timeframe_minutes * 60 * 1000
    # Bucket into bars
    bar_start = (ticks[0][0] // tf_ms) * tf_ms
    bars: list[float] = []
    bar_close = ticks[0][1]
    for ts_ms, mid in ticks:
        if ts_ms >= bar_start + tf_ms:
            bars.append(bar_close)
            bar_start += tf_ms
            # Skip empty bars
            while ts_ms >= bar_start + tf_ms:
                bars.append(bar_close)  # carry close forward for gaps
                bar_start += tf_ms
        bar_close = mid
    bars.append(bar_close)  # final partial bar

    # Return last n_bars
    return bars[-n_bars:] if len(bars) >= 2 else []


def load_h1_closes() -> list[float]:
    """Try .dat first, then L2 CSV, then GoldFlow tick CSV fallback."""
    # 1. Try dat (returns empty -- format not suitable)
    dat = os.path.join(OMEGA_DIR, "indicators", "bars_gold_h1.dat")
    closes = load_closes_from_dat(dat)
    if closes:
        return closes

    # 2. L2 CSV bucketed into H1 bars
    closes = load_closes_from_l2_csv(timeframe_minutes=60, n_bars=H1_WINDOW + 10)
    if len(closes) >= MIN_BARS:
        log(f"  Loaded {len(closes)} H1 pseudo-closes from L2 tick CSV")
        return closes

    # 3. Try GoldFlow tick log (older format)
    pattern = os.path.join(OMEGA_DIR, "logs", "goldflow_*.csv")
    files = sorted(glob.glob(pattern), reverse=True)
    ticks: list[tuple[int, float]] = []
    for fpath in files[:3]:
        try:
            with open(fpath, "r", encoding="utf-8") as f:
                for line in f:
                    parts = line.strip().split(",")
                    if len(parts) < 2:
                        continue
                    try:
                        ts_ms = int(parts[0])
                        mid   = float(parts[1])
                        if mid > 0:
                            ticks.append((ts_ms, mid))
                    except ValueError:
                        continue
        except Exception:
            continue
        if len(ticks) > H1_WINDOW * 60 * 10:
            break

    if len(ticks) < 2:
        log("  No usable price data found")
        return []

    ticks.sort(key=lambda x: x[0])
    tf_ms = 60 * 60 * 1000
    bars: list[float] = []
    bar_start = (ticks[0][0] // tf_ms) * tf_ms
    bar_close = ticks[0][1]
    for ts_ms, mid in ticks:
        if ts_ms >= bar_start + tf_ms:
            bars.append(bar_close)
            bar_start += tf_ms
            while ts_ms >= bar_start + tf_ms:
                bars.append(bar_close)
                bar_start += tf_ms
        bar_close = mid
    bars.append(bar_close)
    closes = bars[-(H1_WINDOW + 10):]
    log(f"  Loaded {len(closes)} H1 pseudo-closes from GoldFlow tick CSV")
    return closes


# ---------------------------------------------------------------------------
# HMM fit
# ---------------------------------------------------------------------------

def compute_log_returns(closes: list[float]) -> np.ndarray:
    arr = np.array(closes, dtype=np.float64)
    # log-returns: r_t = log(c_t / c_{t-1})
    returns = np.log(arr[1:] / arr[:-1])
    # Replace inf/nan with 0
    returns = np.where(np.isfinite(returns), returns, 0.0)
    return returns


def assign_state_labels(model: hmm.GaussianHMM) -> dict[int, str]:
    """
    Sort HMM states by emission variance ascending.
    Lowest  variance -> CHOPPY   (state index 0 in sorted order)
    Middle  variance -> TRENDING
    Highest variance -> VOLATILE
    Returns mapping: {hmm_state_id -> label_string}
    """
    variances = model.covars_.flatten()  # shape (n_states,) for diagonal
    sorted_states = np.argsort(variances)  # ascending variance
    mapping = {}
    for rank, state_id in enumerate(sorted_states):
        mapping[state_id] = STATE_LABELS[rank]
    return mapping


def fit_hmm(returns: np.ndarray) -> dict:
    """
    Fit GaussianHMM and return state dict.
    """
    X = returns.reshape(-1, 1)
    n = len(X)

    model = hmm.GaussianHMM(
        n_components=N_STATES,
        covariance_type="diag",
        n_iter=MAX_ITER,
        random_state=RANDOM_SEED,
        tol=1e-4,
    )
    model.fit(X)

    # Decode most likely state sequence
    _, state_seq = model.decode(X, algorithm="viterbi")
    current_state = int(state_seq[-1])

    # State label mapping
    label_map = assign_state_labels(model)
    current_label = label_map[current_state]

    # Transition probability: P(stay in current state)
    trans_row = model.transmat_[current_state]
    p_stay = float(trans_row[current_state])
    p_flip = 1.0 - p_stay

    # Normalised volatility: recent 5-bar std vs full window std
    recent_vol = float(np.std(returns[-5:])) if len(returns) >= 5 else float(np.std(returns))
    full_vol   = float(np.std(returns))
    vol_regime = float(np.clip(recent_vol / (full_vol + 1e-10), 0.0, 3.0) / 3.0)

    return {
        "state":      current_state,
        "label":      current_label,
        "p_stay":     round(p_stay, 4),
        "p_flip":     round(p_flip, 4),
        "vol_regime": round(vol_regime, 4),
        "ts_utc":     int(time.time()),
        "bars_used":  n,
        "fit_ok":     True,
        # Debug: emission means per state (signed drift proxy)
        "state_means": {
            label_map[i]: round(float(model.means_[i][0]), 8)
            for i in range(N_STATES)
        },
        "state_vars": {
            label_map[i]: round(float(model.covars_[i][0][0]), 10)
            for i in range(N_STATES)
        },
    }


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def write_fallback(reason: str):
    """Write a safe fallback JSON so C++ gets fit_ok=false, not a stale file."""
    result = {
        "state":      1,          # default TRENDING (least restrictive)
        "label":      "UNKNOWN",
        "p_stay":     0.5,
        "p_flip":     0.5,
        "vol_regime": 0.5,
        "ts_utc":     int(time.time()),
        "bars_used":  0,
        "fit_ok":     False,
        "error":      reason,
    }
    tmp = OUTPUT_FILE + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(result, f, indent=2)
    os.replace(tmp, OUTPUT_FILE)  # atomic replace
    log(f"  Wrote fallback: {reason}")


def main():
    log("=== hmm_refit.py starting ===")

    closes = load_h1_closes()
    if len(closes) < MIN_BARS:
        write_fallback(f"insufficient data: {len(closes)} bars < {MIN_BARS} required")
        log(f"DONE (fallback: {len(closes)} bars)")
        return

    returns = compute_log_returns(closes)
    if len(returns) < MIN_BARS - 1:
        write_fallback("insufficient returns after diff")
        return

    try:
        result = fit_hmm(returns)
    except Exception as e:
        write_fallback(f"HMM fit failed: {e}")
        log(f"  Exception: {e}")
        return

    # Atomic write
    tmp = OUTPUT_FILE + ".tmp"
    os.makedirs(os.path.dirname(OUTPUT_FILE) if os.path.dirname(OUTPUT_FILE) else ".", exist_ok=True)
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(result, f, indent=2)
    os.replace(tmp, OUTPUT_FILE)

    log(f"  state={result['label']}  p_flip={result['p_flip']:.3f}"
        f"  vol_regime={result['vol_regime']:.3f}  bars={result['bars_used']}"
        f"  fit_ok={result['fit_ok']}")
    log(f"  Written to: {OUTPUT_FILE}")
    log("=== hmm_refit.py done ===")


if __name__ == "__main__":
    main()
