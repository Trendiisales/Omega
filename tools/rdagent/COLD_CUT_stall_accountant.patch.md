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

## SWEEP VERDICT (2026-07-01) — REJECTED, keep `GOLD_COLDCUT = 0.0`

`--sweep-coldcut` over grid {off,1.0,1.5,2.0,3.0}% on largecap (27 names, 1778
rows, 63% bull). Every mode, every cut value, `tot%` goes DOWN and PF drops:

| mode   | off tot% / PF | best cut tot% / PF | note |
|--------|---------------|--------------------|------|
| GOLD   | 838 / 1.76    | 302 / 1.24 @3%     | collapses (PF 1.76→1.24, tot −64%) |
| WIDE   | 2852 / 2.91   | 2807 / 3.15 @2%    | tot down every value |
| SWITCH | 2727 / 3.44   | 2382 / 3.05 @3%    | tot + PF both down |

The cut improves `worst%` (−45→−23) and `maxDD%`, **but** WR% drops sharply
(GOLD 67→38-49): it converts eventual-winners that dip-then-recover into small
losers, not just the rare never-arms cold loser. Fails the mandate gate
(`tot%` flat-or-up). **The cold-loss cut is NOT viable on the day-mover book —
stays OFF.** The knob + harness are kept so the negative result is reproducible;
re-open only with a materially different companion book (e.g. the live crypto
`stall_accountant.py`, which is both-direction and higher-cadence — run its own
sweep before trusting this verdict there).
