# Donchian H1 Post-Regime Sweep — Decision Memo

**Run date:** 2026-04-28 (NZST)
**Script:** `phase2/donchian_postregime_sweep_v3.py`
**Validation:** canonical rule (period=20, cooldown=5, strict `>` on close, per-side cooldown with `>=` gap test) reproduces `phase1/signals/donchian_H1_long.parquet` byte-exact (509 / 509 signals, all idx + channel_upper + close match).

---

## Verdict: LOCK `(period=20, sl_atr=3.0, tp_r=5.0)` for Donchian H1 long

This **overrides** the script's auto-pick of rank 1 `(10, 3.0, 5.0)` for the reasoning below.

---

## ⚠️ DEFENSE GAP WARNING — READ BEFORE USING ANY NUMBER IN THIS MEMO

**The numbers in this memo come from `phase1/sim_lib.py`, which has zero defensive machinery.** Specifically, sim_lib has:

- **No spike-exit logic.** A volatility-adaptive exit on adverse spike bars does not exist in the simulator.
- **No trail logic.** Trades sit at fixed SL until either SL or TP fires, or `max_hold_bars` times out.
- **No vol-adaptive SL.** Stop is a fixed multiple of ATR at entry; it does not widen or tighten with regime.
- **No regime-aware exit.** Long trades held through a sustained BEAR regime continue to sit at fixed SL.

**Live Omega has all three.** HBG runs bracket + MFE-proportional trail (locks 80% of move), MacroCrash provides spike defense (currently disabled at S17 commit `9566bd6e`, re-enable pending audit), and the bracket logic adapts to regime.

**Concrete consequence — 2026-03-18 cluster:**
- Diagnosed in `cluster_postmortem_2026_03_18_v2.py` (this session).
- Long-only portfolio held longs through an 80hr BEAR regime with two `spike=True` bars.
- Trades sat at fixed SLs and ate full losses because sim_lib has no spike-exit, no trail, no vol-adaptive logic.
- Live HBG would not have taken those losses unmodified — the trail and (when re-enabled) MacroCrash would have intervened.

**What this means for every metric below:**

| Metric in this memo | What it actually represents |
|---|---|
| WR 45.5% | sim_lib WR with fixed SL/TP, no defense |
| PF 1.351 | sim_lib PF with no trail, no spike-exit |
| Stability 0.888 | quarterly stability under strawman exit logic |
| Expected ~57%/-7%/PF 1.25 (C1 baseline) | strawman portfolio metrics, NOT live Omega projection |
| Any C1 portfolio re-run with `(20, 3.0, 5.0)` | still strawman — re-run does not close the gap |

**Therefore:**

1. **The C1 portfolio re-run (next-session task) does NOT produce shippable numbers.** It produces *research-level* numbers under a defense-blind simulator. Even if those numbers look excellent, they cannot be used to greenlight shadow.
2. **Shadow start is BLOCKED on these numbers.** Do not initiate shadow paper-trading on the strength of any sim_lib output until Stage 2 (research stack defense parity) is complete.
3. **Stage 2 is the gate.** `sim_lib.py` must gain spike-exit + trail + vol-adaptive SL matching live HBG behaviour. After that, the C1 portfolio re-run becomes meaningful. The PF 1.351 / 45.5% WR figure either holds under realistic defense, or it collapses — both outcomes are useful, but only the post-Stage-2 number is decision-grade.
4. **The decision in this memo is still valid as a research-level lock** — `(period=20, sl_atr=3.0, tp_r=5.0)` is the right cell within the strawman simulator, and the cluster reasoning (rank 2 over rank 1) is sound. Locking the cell is fine. *Acting on the live deployment* is what's blocked.

**Provenance of this warning:** session 2026-04-28. Defense gap identified after cluster post-mortem confirmed sim_lib's exit logic is `hard_sl_atr=1.5`, `max_hold_bars=20`, BB midline exit only — no defensive overlay of any kind. Live Omega stack (HBG bracket+trail+pyramid, MacroCrash, MFE locking) is structurally different. Research metrics ≠ live projection.

---

## Top 5 by score (post-2026-01-28 only)

| rank | p | sl_atr | tp_r | n | wr% | pf | stab | score |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 10 | 3.0 | 5.0 | 66 | 47.0 | 1.452 | 0.852 | 1.2377 |
| **2** | **20** | **3.0** | **5.0** | **44** | **45.5** | **1.351** | **0.888** | **1.2003** ← chosen |
| 3 | 15 | 3.0 | 2.0 | 48 | 62.5 | 1.374 | 0.825 | 1.1339 |
| 4 | 20 | 1.0 | 2.0 | 44 | 40.9 | 1.692 | 0.647 | 1.0936 |
| 5 | 10 | 3.0 | 4.0 | 66 | 50.0 | 1.359 | 0.792 | 1.0760 |

## Canonical (20, 1.0, 2.5)

- Rank: **#26 of 101**
- n=44, wr=31.8%, pf=1.465, stab=0.492, score=0.7211
- High PF but low stability — concentrated returns over a few good quarters, not consistently winning

---

## Why rank 2 over rank 1

**Reason 1 — same period as canonical (20).** Rank 1 uses period=10 (10-hour breakout window), rank 2 uses period=20 (20 hours). The original phase1 walk-forward research selected period=20 for a reason: 10-hour windows pick up intraday noise, 20-hour windows capture meaningful pivots. Keeping the period stable means only the *exit geometry* (SL/TP) changes vs canonical — the signal-generation surface stays validated.

**Reason 2 — higher stability (0.888 vs 0.852).** Across the 4 chronological quarters of post-regime data, rank 2 wins more evenly. Rank 1 has slightly more variance, suggesting one or two quarters carry the score.

**Reason 3 — score gap is tiny (~3%).** 1.2003 vs 1.2377. Within sweep noise, especially given small n.

**Reason 4 — robust cluster, not a curve-fit peak.** The `(*, 3.0, 5.0)` row dominates: period 10/15/20 all rank in the top 9 with this SL/TP combo. The edge is in the wide-stop, runner-TP geometry — not in any specific period choice. Picking period=20 puts us in the cluster while staying anchored to the original research.

## Why this is opposite of the previous (v1) sweep

v1 used a reverse-engineered generator (no validation, 465 signals vs canonical 509, ~82% overlap). That run identified `sl=1.0, tp=4.0` as the winner. With the correct generator (v3, byte-exact validation passed), `sl=1.0, tp=4.0` collapses to PF 0.807 (loser cluster). The canonical signal set is 9% larger and structured differently than v1 assumed; the previous decision was based on biased data.

---

## What this means for Variant C1 (NOT YET LOCKED)

C1 is the locked portfolio (Donchian H1 + Bollinger H2/H4/H6 long, 0.5% risk, max 4 concurrent, $10k base, expected ~57%/-7%/PF 1.25). The Donchian H1 cell was the blocker that prevented C1 from shipping. **This sweep resolves the blocker but introduces new variables that require validation.**

Changes from canonical to new params:
- SL widens 1.0 → 3.0 ATR (3× larger position drawdown per loser)
- TP widens 2.5 → 5.0 R (trades hold longer)
- WR rises 31.8% → 45.5% (asymmetry shifts from low-WR/big-runners to mid-WR/balanced)
- PF drops 1.465 → 1.351 (slightly less profit per trade, but distributed more evenly)

Net: per-trade $ risk is the same (0.5% of equity by design), but trade duration is longer, which interacts with the max-4 concurrent-position cap. A wider SL also means each losing trade chews more equity before the stop fires — relevant for the -7% drawdown estimate.

**Required next:** re-run `phase2/portfolio_C1_C2.py` with Donchian H1 params updated to `(20, 3.0, 5.0)`. Compare to baseline metrics. **However, see DEFENSE GAP WARNING above — the re-run is research-grade, not shadow-grade.** C1 is not actually shippable until Stage 2 (sim_lib defense parity) closes the gap between simulator and live Omega.

---

## Files

- `phase2/donchian_postregime/sweep_results.csv` — all 101 combos
- `phase2/donchian_postregime/sweep_results.json` — full metrics + verdict
- `phase2/donchian_postregime/run.log` — tee'd console output
- `phase2/donchian_postregime/CHOSEN.md` — this memo
- `phase2/donchian_postregime_sweep_v3.py` — the validated sweep script
- `cluster_postmortem_2026_03_18_v2.py` — diagnosed the 2026-03-18 cluster, source of defense-gap finding

---

## Status

- [x] Canonical rule reverse-engineered and validated byte-exact
- [x] Sweep run on correct rule
- [x] Decision: `(p=20, sl_atr=3.0, tp_r=5.0)` locked for Donchian H1 long (**research level only — see DEFENSE GAP WARNING**)
- [ ] Portfolio C1 re-run with new params — produces research-grade numbers, NOT shadow-grade
- [ ] **Stage 2 — sim_lib.py defense parity (spike-exit + trail + vol-adaptive SL matching live HBG)** ← BLOCKER for shadow
- [ ] Re-run portfolio C1 *after* Stage 2 — these are the shadow-grade numbers
- [ ] **🛑 SHADOW PAPER-TRADING BLOCKED until Stage 2 complete and post-Stage-2 portfolio re-run reviewed**
- [ ] Lock or re-evaluate C1 based on post-Stage-2 portfolio re-run
- [ ] If C1 holds under realistic defense: live shadow paper-trading 4–8 weeks

---

## Next-session opener

> **State for next session — Donchian H1 retune validation in C1 portfolio (research-grade only)**
>
> Locked: Donchian H1 long params changed from canonical `(20, 1.0, 2.5)` to `(20, 3.0, 5.0)` based on v3 sweep. See `phase2/donchian_postregime/CHOSEN.md`.
>
> **⚠️ Defense gap acknowledged.** The C1 portfolio re-run produces research-grade numbers under sim_lib (no spike-exit, no trail, no vol-adaptive SL). These numbers cannot be used to greenlight shadow. Stage 2 (sim_lib defense parity with live HBG) is the gate before any shadow decision.
>
> Next: re-run `phase2/portfolio_C1_C2.py` with Donchian H1 long params updated to `(20, 3.0, 5.0)`. The Bollinger cells (H2/H4/H6 long) are unchanged. Cap=4, 0.5% risk, $10k base, $1k margin floor. Compare new portfolio metrics to baseline (~57% return / -7% DD / PF 1.25). Result is a research-level checkpoint, not a ship decision.
>
> After portfolio re-run: proceed to Stage 1 (MacroCrash re-enable audit) and Stage 2 (sim_lib defense parity) per session 2026-04-28 staged plan.
>
> Entry signal generator validated byte-exact in v3 sweep. Bar files at `phase0/bars_H1_final.parquet`. Trade ledgers at `phase1/trades_net/`. Portfolio script at `phase2/portfolio_C1_C2.py`.
