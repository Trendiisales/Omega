# HANDOFF 2026-07-14p — latent-class P1+P2 fix batch SHIPPED + DEPLOYED

## State
- main == origin/main == `bb3753d1`. VPS (omega-new) tree `bb3753d1`, **running binary `f3cc7803`**
  (correct: bb3753d1 is tooling-only, no C++). Two deploys verified via tools/omega_deploy.sh.
- Working tree clean except untracked research files. Canary green throughout.

## VAULT UPDATE OWED — DO FIRST (deploy mandate violation until done)
Operator invoked hard-stop handoff before vault ingest. Memory-Omega needs:
- entities: GoldExecSpreadBasis (NEW page — 0.10 COMEX-tick spread basis, 4 call-sites, commit
  `51a6a9be`), SeedFreshnessSafeguard (SURV_ADD parser-blind guard `8a97427c`+`bb3753d1`),
  MondayRiskOn (persist wire `f3cc7803`), AdverseProtectionMandate page (glob widened, legacy
  3→8), persistence-audit page (dynamic register_source resolvers), OmegaMacroRegime task
  (NEW — feeds vix_term_ratio + index_regime, `abcd6380`).
- index.md pointers + log.md entries (NZ time). Then advance pin.

## What shipped (6 commits; details in messages, don't re-derive)
- `51a6a9be` ak: XAUUSD cost-gate spread basis — GoldExecSpreadBasis.hpp (single def 0.10),
  4 live sites: tick_gold 675/1516, GoldEngineStack ~4248, GoldVolBreakoutM30 spot instance.
- `0eddc8a1` al: lot gate extended to omega_main.hpp + 5 LOT-GATE-OK annotations (negative-tested).
- `abcd6380` am: OmegaMacroRegime VPS task (daily 21:30 UTC + AtStartup) runs
  fetch_macro_regime.py → data/index_regime.txt + data/vix_term_ratio.txt. Registered + seeded
  LIVE, files verified, feeds_selftest GREEN (2 manifest rows + CRITICAL_FEED_TASKS entry).
  yfinance 1.5.1 installed in C:\Omega\bracket-bot\.venv. Fail-open kept BY DESIGN.
- `8a97427c` an: **PARTIAL — see trap below.** Captured only refresh_warmup_seeds.py deletion.
- `f3cc7803` ao: MondayRiskOn persist wire (MondayRiskOnEngine persist_save/restore +
  wire_cross "MondayRiskOn_NAS100" + kPersistTags) + persistence_audit.sh dynamic-shape
  resolvers (eng->engine_name / reg_straddle / _ps_src; unknown shape = FAIL; zero-parse = FAIL).
  FxCrossRevEURGBP dead allowlist entry removed. DEPLOYED binary.
- `bb3753d1` ap: the 5 files the an-batch add dropped (VERIFY_STARTUP hint → seed_refresh.py
  --only ibkr; OMEGA.ps1 comment; adverse glob *Engines.hpp/*Stack.hpp + legacy 8 entries +
  honest header; SURV_ADD [P1-PARSER-BLIND] exit-3 guard).

## TRAP (new, this session)
`git add <list> 2>/dev/null; git commit` SILENTLY committed only pre-staged content when the
add errored — stderr suppressed = no symptom; caught only via post-deploy git status. Never
suppress git add stderr; verify `git status --short` empty of M before declaring a batch committed.

## NEW FINDING — rdagent stock-basket GUI P&L cross-wired (unfixed)
Operator screenshot 07-13: ADBE row shows −$835 but 17sh@220.36 vs price 230.61 = +$174;
−$835 back-solves to CRM's price 171.25 → ADBE P&L computed against row-1 price
(row-index/symbol-key bug in basket panel writer). CRM/NOW rows correct (−$2 each).
Headline −$843 is fiction; real ≈ +$170. Same class as display_truth_selftest
(feedback-content-parity-not-just-plumbing). NOT yet traced — find the panel P&L join
(rdagent basket writer, likely tools/rdagent/* or the GUI html generator), fix, extend
display_truth_selftest to cover per-row P&L parity.

## WATCH
- Tonight 21:30 UTC: OmegaMacroRegime first scheduled run — content ts should advance past
  the seeded 07-10 bar (last common row; some of the 5 yahoo tickers lag). If still 07-10
  after 2 runs, investigate dropna() row-intersection in fetch_macro_regime.py.
- Tonight 23:30 UTC: OmegaSeedRefresh first run of USTEC.F_H4 / GBPUSD_H1 / mgc_h4_hist
  recipes (inherited from 2026-07-14o handoff).
- Inherited: CLIP-JF PJ opens, bigcap ladder windows, IBKR-EXEC XAUUSD.M paper fills.

## Remaining queue (P3, from outputs/LATENT_CLASS_SWEEP_2026-07-14.md items 9-14)
feeds_selftest vs data_health_monitor threshold registry unify; ungated-audit ENTRY_RE widen
(3 opener idioms); fail_verdict_guard sleeve `e.enabled=true` shape; engine_state_audit thr()
import OVERRIDES; omega-vps residual refs ×3; hand-mirrored rosters (STOCKS/BIGCAP_LAD,
INDEX_FUTURES ×3). Plus adverse-protection legacy backfill now 8 owed (burn down when touching
each engine). Plus the rdagent P&L bug above (arguably P1-display, do early).

## Standing traps (carry-forward)
outputs/ gitignored → `git add -f` handoff docs. ssh form literally `ssh omega-new "..."`;
zsh eats bare `=`-prefixed words; Windows has no tail — use PowerShell or `2>&1 | tail` Mac-side
only. VPS dirty seeds: checkout-then-pull only when incoming has equal-or-fresher copies.
No bulk pulls via omega-new:4002. Survivor seed ts = SECONDS.
