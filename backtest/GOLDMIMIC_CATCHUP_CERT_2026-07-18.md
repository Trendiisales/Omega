# GOLDMIMIC BOUNDED CATCH-UP CERT — restore-path `on_trend_restore` — 2026-07-18

**Task (operator-ordered, handoff 2026-07-18p):** port the crypto bounded catch-up to the Omega
companions with **their own cert** (crypto cert `Crypto/backtest/CATCHUP_OUTAGE_CERT_2026-07-18.md`
is never carried across).

**VERDICT: CERTIFIED — wired `catchup_max_age_secs=86400` (24h) on the three
restore-fire-receiving books: XauTf4h (H4), MgcTF4h (H4, rt5 venue), XauTfD1 (D1, bound = 1 bar).**

## What the port actually fixes here (different topology from crypto)

GoldTrendMimicLadder is parent-triggered (no own detector), so the crypto "lost detector
window" class does not exist. What DID exist: `XauTrendFollow4hEngine::persist_restore`
(and D1) re-fired `on_trend_open` **unconditionally on every restart while holding** —
post-arm (omega_main restore runs after init_engines' `arm()`), at the ORIGINAL entry px,
with NO age bound and NO dedup against the persisted legs. That spawned duplicate pending
legs at stale triggers whose BE-ENTER could book a level far from the market — the
`goldmimic_*_closed.csv` "time-travel fill" incident class (ledger survived only via its
entry_ts dedup key). The old refire was an UNBOUNDED catch-up; the port makes it BOUNDED:

- restore fires route through `on_trend_restore()` (new registry path);
- spawns IFF: never-seen trigger (persisted `last_trigger_ts_` watermark) AND flat book
  AND age <= 24h AND elapsed native bars < pend_bars AND confirmed-entry floored family
  (`be_entry_pct>0 && no_prebe_loss`) — default `0` = restore spawns NOTHING;
- spawned legs are `cu`-marked: on their FIRST managed bar, a BE level not traded inside
  the bar (long: bar low above it) CANCELS the leg — a cross that happened unseen during
  the outage is never back-filled (crypto SKIP rule). One-shot: later bars keep the
  certified level-fill convention (incl. weekend-gap bars).
- state-file persistence upgraded to full double precision (p15) — the 6-sig-fig default
  shifted a reloaded leg's entry by ~5e-3 on gold, so post-restart clips diverged from the
  always-on book (cert-caught; also applied to FxLadderPair state files).

## Harness
`backtest/goldmimic_catchup_bt.cpp` — drives the REAL `GoldTrendMimicBook` (no model),
engine_init config parity, each run in a fresh temp state dir. Data: certified
`/Users/jo/Tick/XAUUSD_2022_2026.h1.csv` bucketed to H4/D1. Triggers: |6-bar mom| >= 0.6%
both directions, isolated horizons (provenance-independent — the claim is per-trigger
restore mechanics, the books' edge certs are unchanged and separate).

Scenarios per trigger × k∈{1,2,4} restart offsets:
- **A always-on** (reference), **R normal restart** (state persists; restore fire must
  dedup), **L state loss** (files deleted; the only legitimate spawn case).

## Results (200 triggers H4 books / 67 D1; sorted-pairwise clip compare, rel 1e-8)

| book | R(dedup) fails | L exact n / fails | L conservative-cancel | L bounded-skip n / fails | L crossed-class env fails | worst clip |
|---|---|---|---|---|---|---|
| XauTf4h | **0** | 188 / **0** | 1 | 36 / **0** | **0** | −0.150% (=−rt, BE_FLOOR bound) |
| MgcTF4h | **0** | 188 / **0** | 1 | 36 / **0** | **0** | −0.050% (=−rt) |
| XauTfD1 | **0** | 8 / **0** | 0 | 147 / **0** | **0** | −0.150% (=−rt) |

- **R:** restarts with persisted legs re-book EXACTLY the always-on clips; the restore
  fire spawns nothing (watermark dedup). This is the dup-class removal proven.
- **L exact:** state-loss recoveries inside the bound book EXACTLY the always-on clips
  (legs re-enter at the same BE level via the live path).
- **Conservative-cancel (accepted, 1 window/book):** when the first bar a recovered leg
  sees is itself a gap bar over the BE level (weekend reopen), the book cannot distinguish
  outage-cross (forbidden backfill) from reopen gap-fill — it books NOTHING (never a
  worse/backdated fill). Harness verifies the gap is real on A's entry bar.
- **Bounded-skip:** restores older than the bound (all D1 k>=2 cases) spawn nothing, as
  designed.
- **L crossed class (BE crossed during the lost bars, state lost):** cannot be recovered
  faithfully; cu guard + floor keep every booked clip inside the floored envelope
  (worst == −rt_cost, the BE_FLOOR bound; env_fails 0 across 376+376+46 windows).

**Unit tests** `backtest/catchup_unit_test.cpp` (gold half): 12/12 — default-off,
age>bound, ebars>=pend, non-floored cfg, watermark dedup, in-flight skip, gap-through
cancel, honest-cross entry, one-shot guard, legacy old-format `.open` row parse
(no seam leg-drop).

## Scope notes
- Restore-fire sites converted: `XauTrendFollow4hEngine.hpp` (spot + MGC instances via
  mimic_tag) and `XauTrendFollowD1Engine.hpp`. All other trigger engines (turtles,
  Survivor, GoldBothWays, XauTf1h/2h) fire only on live opens — no restore path, no change.
- Books that never receive restore fires keep `catchup_max_age_secs=0` (restore path
  unreachable AND default-off).
- Pre-existing stale-PENDING-leg class (a pending leg persisted across a restart whose
  pbars freeze during downtime) is UNCHANGED by this port and remains a documented
  follow-up — it is not the catch-up mechanism and needs its own study before touching
  the certified pend semantics.
