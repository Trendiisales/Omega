# SESSION HANDOFF — 2026-07-20p — honest re-cert DONE (372/380 FAIL, pause stays) + increment-cascade proposal KILLED (1/10080); OPERATOR WANTS A WORKING MIMIC ALTERNATIVE — single-leg cert queued, NOT run

Predecessor: `outputs/SESSION_HANDOFF_2026-07-20o.md`. Manual "handoff" hard-stop mid-experiment.

## ⚡ OPERATOR ORDER PENDING — DO THIS FIRST NEXT SESSION
**"if be cascade is not working find me an alternative that works"** — find a mimic design that
passes at the HONEST basis. Work queued but NOT run (hard stop; a zsh heredoc parse error meant
NOTHING executed — `cascade_increment_bt.cpp` is still at its committed `3a161f8` state, no
`single_leg_2026-07-20/` outputs exist):
1. **Single-leg mimic cert (next cheap decisive experiment).** Rationale: every honest survivor
   is wide-g tail-carried; cascade multiplies churn ×8 legs/window. Engine's own design
   comment (MimicLadderCompanion.hpp L979-982): plain `mimic_floor` cell (mimic_stagger=false)
   = ONE managed T1 leg per event. Patch `Crypto/backtest/cascade_increment_bt.cpp`: add env
   `UM_SINGLE=1` → `c.mimic_stagger=false` (single T1; keep anchor=0 own-fill so booking stays
   honest-by-construction, lc sweep {30,60} intact). Run fleet: 35 coins × thr{0.005,0.010,
   0.015,0.020} × W{2,4,8,12} × g{0.2,0.5,0.75,0.9} (add g0.9 — survivors trend wide) × base/2×,
   OHLC-path drive (already in harness). PASS cells = candidate replacement design.
   USE A SCRIPT FILE for the runner (inline xargs quoting broke twice; pattern:
   `cascade_inc_2026-07-20/run_all.sh`).
2. If single-leg insufficient: next candidates in order — (a) engine-side real-fill-px record +
   book shadow from it + revert intrabar confirm → re-cert e0 close-fill exactly (e0-honest
   was 747/840 but UPPER BOUND); (b) wide-g-only cascade (g0.75/0.9) with legs 2-3 max;
   (c) DOGE/RUNE-only allowlist of the 8 certified survivors (needs allowlist mechanism in
   LiveMimicMirror — substr pause can't express it).
3. Findings doc + vault + operator verdict for whatever passes. Mirror stays PAUSED until then.

## ✅ DONE THIS SESSION (do not redo)
1. **Honest entry-basis re-cert (operator-ordered) COMPLETE** — Crypto `c944c79`.
   `honest_entry_basis_bt.cpp`: S-20j harness + exact per-clip re-base to entry=`le*(1+confirm60)`
   (validated 60.2bp/clip vs S-20j raw). Full S-20j matrix + live-thr supplement; all **380 live
   BECASC cells mapped 1:1, 0 missing. RESULT: e1 (intrabar=LIVE) 8/840 matrix PASS; 372/380
   live cells FAIL** — S-20j's 840/840 was an entry-basis artifact. Survivors: 7 DOGE + 1 RUNE
   wide-g tail lanes (figures in `HONEST_ENTRY_BASIS_RECERT_2026-07-20.md`). **e0 close-confirm
   survives honest basis 747/840** = intrabar confirm is the churn source — but e0-honest is an
   UPPER bound (real close fill ≥ confirm level; engine records `entry_px=le`, header L30 — no
   true fill px exists). Mirror pause (josgp1 `7bb43cb`) STAYS; nothing touched on the box.
2. **Increment-gated cascade (operator proposal) CERTIFIED DEAD** — Crypto `3a161f8`.
   "Arm next mimic at +38bp attained": exact spec on the real engine (escalating per-tier
   confirm 60+k·inc, anchor=0 own-fill honest booking, PREBE cut at fill−lc). 10,080 cells
   (35 coins × 4 thr × lc{30,60} × inc{38,60,100} × W{2,4,8,12} × g3 × base/2×): **1 PASS**
   (DOGE thr1.5 lc30 inc38 W8 g0.5, H2 +17% marginal). Kill mechanics (explained to operator):
   failed increment costs lc+RT=60–90bp not 38; attaining +inc banks nothing while one reversal
   takes g×run from EVERY trailed leg + full cut on newest (1:N against); oscillation re-fires
   arms (PREBE=55-70% of clips). `CASCADE_INCREMENT_FINDINGS_2026-07-20.md`.
   **HARNESS TRAP found+fixed:** the anchored certs' worse-of drive (fill@high then low) is a
   MECHANISM-KILLING bias under own-fill basis (first run: 96% PREBE at exactly −90 = artifact).
   Fair drive = canonical OHLC tick path (up bar O→L→H→C, down O→H→L→C, ≤15bp ticks) — already
   in `cascade_increment_bt.cpp`. Never judge an own-fill design under the anchored drive.
3. Handoff 20o committed `7f077c75` (Omega). Vault: Memory-Chimera `MimicShadowEntryBasisError`
   (status=re-cert complete) + `LiveMimicMirror` + index + log [16.21][16.40]. Auto-memory
   `feedback-shadow-entry-basis-honesty` + MEMORY.md updated with verdict.
4. **Gross-exposure item (20o) VERIFIED:** $500 pilot cap deliberately superseded —
   `[LIVE-FULL] pilot restriction LIFTED (live_full=true)`, gross bounded by portfolio hardcap
   $1100 = LEDGER cash enforce=1. 13-leg ≈$1300 valuation = post-fill drift, not a breach.

## 🔎 OPEN
- Alternative-design search (the pending order above).
- Shadow BECASC ledgers still book from epx — `emit_clip_` fix vs retire decided by what passes.
- Crypto shadow-first REBUILD (project memory) still standing; Omega audit owed.
- Engine has NO real-fill-px field (`entry_px=le` by design) — needed for exact e0 re-cert.

## GOTCHAS
- Deploy_to_box.sh guard expects Mac edits UNCOMMITTED; NEVER --hard on josgp1 (20o item).
- Harness family: `-I/Users/jo/ChimeraCrypto/include`, data `/Users/jo/Crypto/backtest/data/
  <COIN>USDT_1h.csv`, 35-coin roster in `cascade_inc_2026-07-20/run_all.sh`.
- Gate everywhere: omit-2022, net>0, PF≥1.3, both WF halves>0, base AND 2× cost (RT=30/60).
- clangd shows false include errors on these harnesses (no -I in compile_commands) — ignore;
  clang++ build is the truth.
- Inline xargs/heredoc in the Bash tool broke twice (zsh parse) — always write runner scripts
  to a file first.
- Live cell inventory extractor (380 BECASC cells → /tmp/becasc_cells.json) rebuilds from
  main.cpp BcCell/BcCell2 regexes if needed.
