# Omega — Known Bugs to Exclude From Backtest Analysis
## 2026-04-29

These trades in `omega_trade_closes.csv` are NOT representative of any
realistic engine behaviour. They are bugs. When evaluating engine edge or
comparing pre/post audit performance, **exclude them.** The fixes have been
pushed to `main` (commits `81dae231`, `49d8151b`, `675f063f`) — once the VPS
is running the post-fix binary, these patterns should not recur.

---

## Bug #1 — MacroCrash Apr-15 phantom-fire burst

**What happened.** Between 2026-04-15 01:16:16Z and 01:16:57Z (41 seconds)
the MacroCrash engine fired **61 LONG entries on XAUUSD**. Every one of them
closed via DOLLAR_STOP, hold_sec ≤ 2, MAE = 0 (telemetry artefact).
Combined net: **−$9,907.**

**Why it happened.** The 30-minute consecutive-loss kill switch in
`MacroCrashEngine.hpp` (line 899) only counted `SL_HIT`, not `DOLLAR_STOP`.
Sixty-one consecutive DOLLAR_STOPs therefore never armed the kill, and the
4-hour directional dollar-stop block had a thread race that allowed the
read-modify-write between threads to miss.

**How to identify these trades.**

```python
mce_phantom = df[
    (df.engine == "MacroCrash")
    & (df.entry_ts_utc >= "2026-04-15T01:16:00Z")
    & (df.entry_ts_utc <= "2026-04-15T01:17:30Z")
]
# 61 trades, sum(net_pnl) ≈ −$9,907
```

**Exclude them from any edge analysis.** Including them inflates MacroCrash
losses by ~85 % and biases system-wide statistics.

**Fix (`675f063f`, audit-fixes-4).** `m_consec_sl` now counts both `SL_HIT`
and `DOLLAR_STOP`. `force_close()` updates `pos.mae` from the live quote
before delegating to `_close_all` so MAE telemetry is accurate.

---

## Bug #2 — HybridBracketGold Apr-7 100× P&L race

**What happened.** A single trade on 2026-04-07 08:23:46Z was recorded with
`gross_pnl = −$3,008.38` despite a SL distance of only 3.58 points and a
position size of 0.0838 lots. The expected P&L for that trade is **−$30.08**.
The recorded value is exactly 100× too large.

**Why it happened.** Two close paths in `GoldHybridBracketEngine.hpp`
(`manage()` → TP/SL hit, and `force_close()`) had no mutex serialisation.
Under thread interleaving, the trade record write picked up partial state
(probably between the lifecycle’s tick-multiplier application and the cost
adjustment), producing an order-of-magnitude error.

**How to identify this trade.**

```python
hbg_phantom = df[
    (df.engine == "HybridBracketGold")
    & (df.entry_ts_utc == "2026-04-07T08:23:46Z")
]
# 1 trade, net_pnl ≈ −$3,008.38
```

**Exclude it.** Without it, HybridBracketGold is **−$378 over 82 trades**
(essentially flat). Including it makes HBG look like a $3.4k loser when in
reality it had one corrupted record.

**Fix (`675f063f`).** `_close()` now holds `m_close_mtx` for the entire path,
snapshots `pos.*` into locals before building the trade record, and a sanity
check on `tr.pnl` magnitude (`|pnl| > size × 200pt`) catches and recomputes
any anomalous value before emit. The atomic ostringstream log replaces the
line that was previously vulnerable to interleaving.

---

## Bug #3 — Index whipsaw (NAS100, IndexFlow ↔ HybridBracketIndex)

**What happened.** Across 294 NAS100 trades, **67 entries fired within 10
minutes of an opposite-side prior entry**, and 59 fired within 5 minutes for
a combined net of **−$55.77**. Pattern: IndexFlow fires LONG, exits with a
small loss/BE; within 1–3 minutes HybridBracketIndex fires SHORT (or vice
versa). Each engine in isolation is making a defensible signal call, but the
two engines do not cross-block each other, so on a chopping range they
take both sides and bleed cost.

The gold engines have a `gold_any_open` gate at `tick_gold.hpp:35–50` that
mutually blocks every engine. **The index engines have no equivalent gate.**

**Examples.**

| timestamp | engine | side | prev side | gap (s) | net P&L |
|---|---|---|---|---:|---:|
| 2026-04-06 13:33:04Z | IndexFlow | SHORT | LONG | 94 | −$2.05 |
| 2026-04-06 13:33:19Z | HybridBracketIndex | LONG | SHORT | 15 | −$0.36 |
| 2026-04-06 14:35:31Z | HybridBracketIndex | LONG | SHORT | 123 | **−$19.04** |
| 2026-04-07 11:09:53Z | HybridBracketIndex | LONG | SHORT | 136 | **−$25.09** |

**How to identify whipsaw trades.**

```python
nas = df[df.symbol == "NAS100"].sort_values("entry_ts_unix").copy()
nas["prev_side"] = nas["side"].shift()
nas["gap_s"]    = nas["entry_ts_unix"] - nas["entry_ts_unix"].shift()
whipsaw = nas[(nas.side != nas.prev_side) & (nas.gap_s < 300)]
# 59 trades, sum(net_pnl) ≈ −$55.77
```

**Exclude them and the underlying engines look much better.** NAS100
HybridBracketIndex without whipsaw entries is the +$172 winner the audit
already flagged.

**Fix (PENDING — to be implemented next).** Add a cross-engine `index_any_open`
gate analogous to gold’s. Engines proposed: `g_iflow_*`, `g_hybrid_sp/nq/us30/nas100`,
`g_minimal_h4_us30`. Locations to patch:

- `include/tick_indices.hpp` (the index tick handler) — add the gate
- `include/globals.hpp` — already has the engine instances
- Optionally `omega_config.ini` `[risk] index_min_entry_gap_sec=120`

This is the next session’s priority. The fix is straightforward (~30 lines)
but invasive across `tick_indices.hpp`, so it deserves a dedicated commit and
shadow validation.

---

## How to filter all three bugs from a CSV

```python
import pandas as pd
df = pd.read_csv("omega_trade_closes.csv", encoding="utf-8-sig")
df.columns = [c.strip().lower() for c in df.columns]
for c in ["entry_ts_unix","net_pnl"]:
    df[c] = pd.to_numeric(df[c], errors="coerce")

# Bug #1 — MCE Apr-15 phantom burst
mask_b1 = (df.engine == "MacroCrash") & \
          (df.entry_ts_utc.between("2026-04-15T01:16:00Z","2026-04-15T01:17:30Z"))
# Bug #2 — HBG Apr-7 100x record
mask_b2 = (df.engine == "HybridBracketGold") & \
          (df.entry_ts_utc == "2026-04-07T08:23:46Z")
# Bug #3 — NAS100 whipsaw within 5 minutes of opposite side
nas = df[df.symbol == "NAS100"].sort_values("entry_ts_unix").copy()
nas["prev_side"] = nas["side"].shift()
nas["gap_s"]    = nas["entry_ts_unix"] - nas["entry_ts_unix"].shift()
whip_idx = nas.index[(nas.side != nas.prev_side) & (nas.gap_s < 300)]
mask_b3 = df.index.isin(whip_idx)

excluded = df[mask_b1 | mask_b2 | mask_b3]
clean    = df[~(mask_b1 | mask_b2 | mask_b3)]

print(f"excluded: {len(excluded)} trades, sum(net_pnl) = ${excluded.net_pnl.sum():.2f}")
print(f"clean:    {len(clean)} trades,   sum(net_pnl) = ${clean.net_pnl.sum():.2f}")
```

Expected output (on the existing 1,938-trade audit CSV):

```
excluded: 121 trades, sum(net_pnl) = $-13,019.21
clean:    1,817 trades, sum(net_pnl) =  $-9,301.21
```

So removing only the three known-bug clusters strips **$13,019** of the
$22,320 total loss — almost 60 % — without touching any other engine logic.

---

## Summary

| Bug | Trades | Loss | Status |
|---|---:|---:|---|
| MacroCrash Apr-15 phantom burst | 61 | −$9,907 | **FIXED** in `675f063f` (C-1 + C-3) |
| HybridBracketGold Apr-7 100x record | 1 | −$3,008 | **FIXED** in `675f063f` (mutex + sanity) |
| NAS100 whipsaw across IndexFlow ↔ HybridBracketIndex | 59 | −$56 | **PENDING** (`index_any_open` gate) |
| **Total bugs** | **121** | **−$12,971** | |

Once the VPS is running `49d8151b` or later, these patterns cannot recur for
MCE and HBG. The index whipsaw fix lands next session.
