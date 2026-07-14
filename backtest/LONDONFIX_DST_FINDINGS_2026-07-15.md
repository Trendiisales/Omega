# LondonFixMomentum — DST audit + faithful re-backtest (2026-07-15)

**Trigger:** first forward trade 2026-07-14 15:00:01Z LONG 4086.02 → SL_HIT 4081.02,
net −$5.45 (77s hold). Operator asked why + fix.

## Findings

1. **The SL itself worked as designed** (5.0pt stop, TP 10.8pt, plain SL_HIT). No
   execution fault.
2. **Thesis/timing misalignment (real):** engine hard-codes the LBMA PM fix at
   **15:00 UTC** (`GoldEngineStack.hpp` LondonFixMomentumEngine, prefix bar
   14:30–15:00 UTC). The LBMA PM auction is 15:00 **London local** — during BST
   (late Mar–late Oct) the real fix is **14:00 UTC**, so the engine fires 1h after
   the event it claims to trade, ~7 months of the year. The 2026-07-14 trade was
   one of these: fired 16:00 London, an hour post-fix, into COMPRESSION.
3. **BUT retiming does not rescue it.** Faithful replication on certified
   `xau_1m_spliced_2024_2026.csv` (837k bars, gate CERTIFIED CLEAN; entry rule,
   |move|≥1.0 conviction, live-observed SL 5.0/TP 10.8, 0.45pt RT cost from the
   live net-gross, conservative SL-first, 2h timeout, 1/day):

   | Variant | n | net | PF | WR | maxDD |
   |---|---|---|---|---|---|
   | A 15:00 UTC (LIVE today) | 530 | −60.9pt | 0.96 | 38.5% | 247pt |
   | B 15:00 London (DST-correct) | 539 | **−276.5pt** | 0.85 | 34.9% | 365pt |
   | C 14:00 UTC fixed | 541 | −180.7pt | 0.90 | 35.5% | 330pt |

   Sensitivity on the live timing: SL6.0/conv1.0 PF 0.96; conv2.0 PF 0.99;
   conv3.0 PF 0.99 — flat-to-negative everywhere. The header's claimed
   WR 58% / Sharpe 2.60 ("Sim approximated from London Fix research") does not
   replicate at ANY timing: there is **no fix-momentum edge** in 2024–2026 gold,
   and the DST-correct variant (the real fix flow) is the WORST of the three.

## Verdict + action

- **Do NOT retime** — the "correct" timing is backtested worse.
- **DISABLE** LondonFixMomentum via `set_subengine_audit_disabled` (AsianRange
  S99d precedent: live bleed + thin/unreplicable BT ⇒ audit-disable). Auto-demote
  gate would need n≥30 forward losers to catch it — no reason to donate 29 more
  spreads to confirm PF 0.96.
- Re-enable bar: fresh sweep with real edge at 1m truth, both WF halves.

## Caveats

- BT models raw TP/SL exits (matches the one live trade exactly); any
  GoldPositionManager in-flight management is not modeled. Spread gate
  (MAX_SPREAD 2.0) not modeled — cost side only.
- Script: `backtest/londonfix_dst_bt.py`.

## Same-sweep verdicts on the other 2026-07-14 negatives (no action)

- **GoldDon15mMimicT/W −$22.58 ×2 LOSS_CUT:** entry 4105.1010 → 4084.5755 =
  exactly −0.50% = the deployed lc0.5 drawdown-cancel (f09518fc, documented
  "lc=0.5 ACTIVE and BEST", BT worst −0.55%). Both legs cut on first 15m bar
  before trail divergence — designed protection, in-envelope. Minor telemetry
  note: mimic ledger rows carry mfe/mae = 0 (close-path TradeRecord doesn't
  populate them).
- **INJ-UJ55W24-SWEET T1/T2 −242.1bp:** already explained in
  SESSION_HANDOFF_2026-07-15a — protected reversal clips, in-envelope
  (worst BT clip ≈ −770bp, retire@−2300bp), shadow.
