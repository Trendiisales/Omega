# COLD-LOSS CUT — live wire-in for `stall_accountant.py` (IBKRCrypto book)

**Context.** The live crypto/companion book (`stall_accountant.py`, IBKRCrypto
project — *not* in the Omega repo) shares its exit logic with the in-repo
faithful mirror `daymover_goldlogic_bt.py::simulate` and the live-ish port
`export_signals.py::_mark_and_exit`. All three had the same blind spot:

> STALL and REVERSAL clips are BOTH `AND armed` (`armed = fav >= GOLD_GATE`).
> A trade that goes adverse and never captures `GOLD_GATE` never arms → neither
> clip can fire → it rides to `MAX_HOLD` unprotected. Observed live: the SOL
> companion sitting at **−$6.40 / MFE 0% / stall 1 / pre-gate (rides wide)** while
> its parent leg was **+$6.80** (different fill: companion chased a new high that
> failed).

**Fix.** An independent adverse stop measured **from entry**, ungated by the arm.
It only bites never-armed trades (`not armed`), so runners — which arm by
definition — are exempt and still ride wide. It cannot scalp the runners that
carry PF; it only bounds the cold loser the AND-armed clips can never reach.

## Patch (apply in `stall_accountant.py`)

Add the param near the other gold-companion constants:

```python
# COLD-LOSS CUT: ungated adverse stop. 0.0 = OFF. Set the swept value from
# omega/tools/rdagent/daymover_goldlogic_bt.py --sweep-coldcut before enabling.
GOLD_COLDCUT = 0.0        # e.g. 0.015 once the sweep confirms net-positive
```

In the per-cycle mark/exit function, **after** `peak`/`since_high` are updated and
`fav` / `armed` are computed, **before** the STALL/REVERSAL/trail checks:

```python
fav   = peak / entry_px - 1.0
armed = fav >= GOLD_GATE

# COLD-LOSS CUT — ungated by the arm; only never-armed trades.
# LONG leg: adverse = px/entry - 1.  SHORT leg: adverse = entry/px - 1.
adverse = (px / entry_px - 1.0) if is_long else (entry_px / px - 1.0)
if GOLD_COLDCUT > 0.0 and not armed and adverse <= -GOLD_COLDCUT:
    return _exit(reason="COLD_CUT")   # tag it so it shows in the companion-bank breakdown
```

> ⚠️ Use the **direction-aware** `adverse` above — the crypto companion trades both
> LONG and SHORT legs. The in-repo `export_signals.py` / `daymover_goldlogic_bt.py`
> ports are LONG-only day-movers, so they use `c/entry - 1` directly.

## Reporting hook (optional but recommended)

The INTRADAY BOOK header already breaks the companion bank into
`REVERSAL_CLIP / ENGINE_EXIT / LOSS_CUT_CLIP`. Tag the cold-cut exits as
`COLD_CUT` (or fold into `LOSS_CUT_CLIP`) so the desk shows how much the cut is
saving vs the old ride-to-max_hold behaviour.

## DO NOT enable until backtested (CLAUDE.md build gate)

Per the Adverse-Protection mandate (the `NqMomentumEngine` −464pt lesson: a
%-scaled ratchet that never armed → unbounded cold loser):

1. `python omega/tools/rdagent/daymover_goldlogic_bt.py --sweep-coldcut`
2. Ship a non-zero `GOLD_COLDCUT` **only if** the sweep shows `tot%` flat-or-up
   with `worst%` / `maxDD%` improved. PF should hold (the cut never touches armed
   runners) — confirm it does, don't assume.
3. Record the chosen value + the sweep verdict in an `ADVERSE-PROTECTION:` note.

## SWEEP VERDICT (2026-07-01)

Two cut designs were tested. **v1 (arm-gated) REJECTED. v2 (MFE+time catastrophe
cut) VIABLE @20% on the equity day-mover book. Crypto gets NO cut.**

### v1 — arm-gated `not armed AND adverse<=-cut` — REJECTED

`--sweep-coldcut` over grid {off,1.0,1.5,2.0,3.0}% on largecap (27 names, 1778
rows, 63% bull). Every mode, every cut value, `tot%` goes DOWN and PF drops:

| mode   | off tot% / PF | best cut tot% / PF | note |
|--------|---------------|--------------------|------|
| GOLD   | 838 / 1.76    | 302 / 1.24 @3%     | collapses (PF 1.76→1.24, tot −64%) |
| WIDE   | 2852 / 2.91   | 2807 / 3.15 @2%    | tot down every value |
| SWITCH | 2727 / 3.44   | 2382 / 3.05 @3%    | tot + PF both down |

Root cause: `not armed` = fav < GATE (2%) is TOO BROAD — it catches trades that
ran up, dipped, and recovered. WR% drops 67→38-49: the cut converts dip-then-
recover WINNERS into losers, not just the rare never-worked cold loser.

### v2 — MFE+time catastrophe cut — VIABLE @0.20 (equity)

The operator's actual target: a position that opened, went red, and **NEVER went
green** (the live SOL case MFE 0% / stall 1) or a bad re-open. v2 fires ONLY when
(a) peak fav <= 0.3% (never green), (b) held >= 3 bars (not a wick), (c) adverse
<= −cut. Any strength first → EXEMPT, so runners are untouched. `--sweep-smartcut`,
PAIRED standalone (fixed exogenous entries, only the exit varies — no re-entry
confound), GOLD companion book, all regimes:

| cut  | n   | PF   | WR% | tot%  | worst% | maxDD% | H1/H2     |
|------|-----|------|-----|-------|--------|--------|-----------|
| off  | 529 | 1.76 | 67  | 838.2 | −45.4  | −281.6 | 1.20/2.67 |
| 16%  | 529 | 1.65 | 66  | 739.4 | −41.3  | −230.7 | 1.13/2.48 |
| 18%  | 529 | 1.69 | 67  | 772.2 | −41.3  | −253.4 | 1.19/2.46 |
| **20%** | 529 | **1.85** | 67 | **882.2** | **−41.3** | **−234.7** | **1.30/2.70** |
| 25%  | 529 | 1.83 | 67  | 880.2 | −41.3  | −261.2 | 1.23/2.85 |
| 30%  | 529 | 1.74 | 67  | 821.7 | −41.3  | −278.5 | 1.14/2.78 |

@20% PASSES the Adverse-Protection gate: tot UP (838→882), PF UP (1.76→1.85),
worst improved (−45→−41), maxDD improved (−282→−235), **both** WF halves up
(1.20→1.30, 2.67→2.70), fires only ~15×/7yr. Below ~18% the cut starts scalping
dip-then-recover winners (tot + PF fall); above 25% it fires too late to help.
Caveat: the BEAR sleeve's own maxDD gets slightly worse at 20% (−54→−91, realized
losses concentrate) though its tot and PF both improve (145→178, 1.46→1.63) — the
whole-book maxDD still improves. **Shipped: `export_signals.py GOLD_COLDCUT=0.20`,
`GOLD_COLDCUT_MFE_EPS=0.003`, `GOLD_COLDCUT_MINHOLD=3`; harness `CUT_ADV=0.20`.**

### Crypto (`stall_accountant.py`) — NO cut helps, keep OFF

`crypto_companion_coldcut_sweep.py` (imports `crypto_companion_intraday_bt`, byte-
identical signal/cost/regime port; KELT/EMAX × BTC/ETH/SOL × 1h/4h, two-sided) over
grid {off,5,8,12,20,30}%. The crypto companion rides to a **TREND FLIP**, which
naturally bounds the worst trade at ≈ −31% — so there is no fat unbounded tail for a
catastrophe cut to bound. Every cut value lowers tot; a 20-30% cut fires ≈1/10871.
The mechanism differs from equity: the day-mover rides to a 60-day TIME stop (fat
−45% left tail the cut can bound); crypto rides to a flip (already bounded). **Do
NOT transfer the equity 20% cut to crypto.** Note the LIVE `COLD_LOSS_CRYPTO=-15`
flat-dollar cut on ~$377 SOL ≈ −4% sits in the proven-HARMFUL tight zone (it scalps
recoverable dips). Recommend to the operator: relax it or convert to a %-based
never-green catastrophe floor (~25-30%) — but that is a LIVE behavior change and
needs operator sign-off; not auto-applied.
