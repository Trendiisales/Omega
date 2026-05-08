# S15 P0 — what to fix today

Date: 2026-05-08
Branch / HEAD: `main` @ `313b0aa06688169da84b77125bce06babfb64a2c`
Mode: `mode=SHADOW` — no real orders firing. P0s here are correctness defects that will bite the moment shadow flips to LIVE, or that have already produced ledger-level data corruption visible in the shadow tape.
Companion: full audit at [`AUDIT_S15_ENGINES.md`](computer:///Users/jo/omega_repo/AUDIT_S15_ENGINES.md).

Triage rule: do these in order. Each one is bounded (≤ 1 session of focused work).

---

## P0-1 — Dollar-stop close path: ms/seconds unit mismatch (still open on `CandleFlowEngine`, `HybridBracketGold`)

**What goes wrong.** Pre-fix, the dollar-stop branch in `include/on_tick.hpp` passed `ds_now` (unix seconds) where the engine's `force_close()` expected `now_ms` (unix milliseconds). Two observable defects fall out:

1. The cooldown register `m_cooldown_until = now_ms + COOLDOWN_MS` ends ~1 700 s in the past — re-entry is allowed on the very next tick. This is the failure mode of the **2026-04-15 phantom-fire incident: 61 entries / 41 sec / -$9 907** referenced at `omega_config.ini:27-29`.
2. The ledger record path computes `tr.exitTs = now_ms / 1000`. With seconds-as-ms, `exitTs ≈ entryTs / 1000`, so `hold_sec = exitTs − entryTs ≈ −entryTs`. The shadow tape shows this as values like `-1774312273` in `omega_shadow.csv`.

**What's already fixed.** `include/on_tick.hpp:929-932` (the `g_macro_crash` branch) now passes `ds_now_ms`:

> `// CRITICAL FIX: pass ds_now_ms (milliseconds) not ds_now (seconds).`
> `// ds_now in seconds caused cooldown to be set ~1700s in the past`
> `// (seconds treated as ms), allowing immediate re-entry loop.`
> `g_macro_crash.force_close(s_xau_bid, s_xau_ask, ds_now_ms);`

`MacroCrashEngine.hpp:1043-1044` is consistent: `tr.entryTs = pos.entry_ms / 1000; tr.exitTs = now_ms / 1000;`

**What's still broken.** The fix lives only inside the MacroCrash branch. The shadow tape contains 48 negative-`hold_sec` rows distributed across **three** engines:

| Engine | Negative-hold rows | Notes |
|---|---:|---|
| MacroCrash | 42 | All clustered 2026-04-08 14:32-33 UTC — pre-dates the Apr-21 fix. Confirms historical match. |
| **CandleFlowEngine** | 5 | Spread across early 2026. Pattern matches the unit mismatch. |
| **HybridBracketGold** | 1 | Single instance. Same pattern. |

Conclusion: the same defect lives in the close paths of `CandleFlowEngine` and `HybridBracketGold` (both gold-stack family). The 6 affected rows are circumstantial evidence; the trace still needs to be done.

**Where to look.** `include/on_tick.hpp:948-1000` is the GoldStack dollar-stop block. It has its own log throttle using `ds_now` (in seconds — that's correct, it's used for log rate-limiting). What needs to be confirmed: the path that ends up calling `force_close` on the active sub-engine. If any sub-engine of GoldStack — and specifically CandleFlowEngine and HybridBracketGold — receives the seconds-valued timestamp, it will reproduce the same bug.

**Proposed fix shape.**
1. Trace `tick_gold.hpp` and `on_tick.hpp:948-1000` for any `force_close(...)` call inside a GoldStack sub-engine path.
2. Replace any seconds-valued timestamp argument with the `_ms` variant.
3. Mirror the fix comment for future maintainers.
4. Add a runtime assertion in each sub-engine's `force_close`: `assert(now_ms >= 1e12 && now_ms < 4e12)` (post-2001, pre-2096 in ms). Catches future regressions where seconds slip back in.
5. Add a defensive sanitizer at the ledger entry point: `if (tr.exitTs < tr.entryTs) { tr.exitTs = tr.entryTs; tr.note += "[FIXED-EXIT-TS]"; }` so that any future occurrence is at least flagged in the row instead of silently corrupting the CSV.

**Why this is P0 even though the system is in SHADOW.** The tape shows the corrupted rows are getting written today. The phantom-fire branch is gated by `mode==LIVE` (per `on_tick.hpp:906-922`), so live re-entry storms are suppressed for now. But:
- The corrupted tape is what every downstream analytic ingests, so adaptive-risk / Kelly / Sharpe metrics fed by the ledger are seeing nonsense `hold_sec` values.
- The moment `mode=LIVE` is set on the affected engines, the cooldown side of the bug becomes a live-orders defect. Two of the three suspect engines (CandleFlowEngine, HybridBracketGold) are tier-2/sub-stack and could be promoted to LIVE before MacroCrash is.

Effort: ≤ 1 session. Reversible.

---

## P0-2 — `TrendPullback::DAILY_LOSS_CAP` gates after the cap is already broken

**What goes wrong.** Engine intends a hard daily-loss cap. `engine_init.hpp:510` sets the gold instance's cap to $150. Implementation at `CrossAssetEngines.hpp:2000-2009`:

```
if (DAILY_LOSS_CAP > 0.0) {
    if (daily_pnl_ <= -DAILY_LOSS_CAP) return {};
}
```

`daily_pnl_` is updated only by `record_daily_pnl()`, called from `_close()` at `CrossAssetEngines.hpp:1870` — i.e. AFTER the trade closes. So:

- Trade #1 of the day closes -$100. `daily_pnl_ = -100`.
- Trade #2 entry checks: -$100 > -$150, gate passes, trade enters.
- Trade #2 closes -$60. Now `daily_pnl_ = -160`.
- Cap fires AT THE END OF TRADE #2. It blocks new entries from trade #3 onward, but it has already let trade #2 in despite trade #2's existence guaranteeing the cap will be broken.

The cap is, in effect, "block new entries after we've already lost more than the cap" — not "block entries that would let us lose more than the cap".

**Why it's P0 anyway.** The XAUUSD instance is currently `enabled=false` (`engine_init.hpp:541`, "no edge on XAUUSD"). So the broken cap is dormant on gold. **But:** the indices instances (GER40, NQ, SP) have no cap configured at all, so the moment somebody adds a cap to an indices instance, it inherits the broken implementation. This is exactly the kind of latent defect that fires when configuration changes — high blast radius, low warning.

**Proposed fix shape.**

```
// at entry gate (CrossAssetEngines.hpp:2000):
if (DAILY_LOSS_CAP > 0.0) {
    const double open_max_loss = active_position_.has_value()
        ? active_position_->size * active_position_->sl_distance_pts * dollars_per_pt_
        : 0.0;
    if (daily_pnl_ - open_max_loss <= -DAILY_LOSS_CAP) return {};
}
```

i.e. include the worst-case loss of any currently-open position in the cap check.

Effort: 1 hour.

---

## P0-3 — `TrendRider` `max_lot_cap=0.50` is 8× peer engines, with no live-validation backing yet

**What goes wrong.** TrendRider is the highest-risk Tier-1 portfolio. INI sets `risk_pct=0.040, max_lot_cap=0.50` (vs. 0.005 / 0.02-0.05 for every other Tier-1). The engine's own header comment at `TrendRiderEngine.hpp:489-496` narrates a worst-case 6-cell simultaneous-SL of "6% portfolio drawdown". The arithmetic at the configured caps and the 1.5×ATR initial SL doesn't reach 6% — it's closer to the INI-comment's estimate of ~24% (`omega_config.ini:792-797`). The engine self-narration under-sells the risk to anyone reading the engine without the INI.

**Why it's P0 today.** Shadow tape pre-dates TrendRider ship — there is **zero** live data for these caps. Tsmom needed exactly this kind of cap-tightening (its `max_lot_cap` was halved 0.05 → 0.02 after a single 4.45-pt adverse move lost $23.65 — see `omega_config.ini:514-521`). Setting 8× exposure on the unproven engine is an asymmetric bet at this stage.

**Proposed fix shape.** One-line INI edit at `omega_config.ini:806`: `max_lot_cap=0.50` → `max_lot_cap=0.20`. Hot-reloadable. Reversible. After 50 closed shadow trades broadly match the +$19 633/yr backtest result, ratchet back up.

Effort: 5 minutes.

---

## P0-4 — `MinimalH4Breakout` and `MinimalH4US30Breakout` INI sections are dead config

**What goes wrong.** `engine_init.hpp:568-577` and `:774-786` construct both engines via their `make_*_params()` factories. There is no `apply_engine_config(MinimalH4Breakout&)` or `apply_engine_config(MinimalH4US30Breakout&)` overload in `engine_config.hpp`. Every key under `[minimal_h4]` and `[minimal_h4_us30]` (donchian_bars, sl_mult, tp_mult, max_spread, timeout, cooldown, weekend_close_gate, long_only, dollars_per_point) is read by no one.

**Why it's P0.** Both engines are post-cut survivors that the team uses regularly. Operators editing the INI to tune them — turning `long_only=true` for example — believe they are tuning the engine. They are not. This is a misconfiguration trap with silent failure: no warning, no log line, no compile error.

**Proposed fix shape.** Add the two `apply_engine_config` overloads in `engine_config.hpp`. Each is < 20 lines (read INI key, write to `engine.p`). Pattern already exists for SP / NQ / OIL etc.

Effort: 2 hours total.

---

## P0-5 — `OpeningRangeBreakout` per-symbol session UTC times are hardcoded wrong for all non-US instances

**What goes wrong.** `CrossAssetEngines.hpp:981-982` hardcodes `OPEN_HOUR=13, OPEN_MIN=30` (NY open). All four instantiated instances (US, GER40, UK100, ESTX50) inherit this:

| Instance | Hardcoded open | Actual cash open |
|---|---|---|
| US | 13:30 UTC | ✓ correct |
| GER40 | 13:30 UTC | ✗ Xetra opens 08:00 UTC |
| UK100 | 13:30 UTC | ✗ LSE opens 08:00 UTC |
| ESTX50 | 13:30 UTC | ✗ Euronext opens 09:00 UTC |

**Why it's P0 today.** The four instances are all currently `enabled=false` (`engine_init.hpp:1561-1564`), so the bug is dormant. **But:** the moment somebody flips `g_orb_ger30.enabled=true`, ORB will fire on GER40 at NY open — wrong market, no edge, undefined behaviour. This is a one-keystroke distance from a real defect.

There's also a second smaller P1 in the same engine: `opening_` flag is set false after the first fire and never reset on the next UTC day (no `last_opened_day_` update visible at `CrossAssetEngines.hpp:1029`). Symptom would be: ORB fires once ever per process restart, never again.

**Proposed fix shape.**
1. Move `OPEN_HOUR` / `OPEN_MIN` per-symbol into INI keys (e.g. `[orb_ger30] open_hour=8 open_min=0`).
2. At the top of `OpeningRangeEngine::on_tick`, add `if (today != last_opened_day_) { opening_ = true; last_opened_day_ = today; }`.

Effort: 1-2 hours.

---

# Action grid

| # | Title | Effort | Reversible | Blocks live? |
|---|---|---|---|---|
| 1 | Port ms-not-sec fix to CandleFlowEngine + HybridBracketGold | 1 session | yes | yes (data corruption) |
| 2 | Fix TrendPullback DAILY_LOSS_CAP gating order | 1h | yes | only if TrendPullback re-enables |
| 3 | TrendRider max_lot_cap 0.50 → 0.20 (INI) | 5 min | yes | no (mitigation) |
| 4 | Wire dead `[minimal_h4*]` INI sections | 2h | yes | no (operator trap) |
| 5 | Fix ORB per-symbol UTC open times + daily reset | 1-2h | yes | only if any ORB instance re-enables |

Pick in order. None of them require a backtest.

---

# What this doc deliberately does not contain

- Code patches. Per the user's standing preference, audit findings only.
- Backtest validation. Where a finding genuinely needs a backtest, it is deferred in the main audit.
- Anything about Tier-1 portfolio live performance — the supplied shadow tape pre-dates Tier-1 ship; nothing useful to say yet.
