#pragma once
// engine_init.hpp -- Engine configuration, wiring, and startup.
// All engine config, callback wiring, state loading, and startup checks
// that were previously embedded in main() have been extracted here.
// Called once from main() as: init_engines(cfg_path);
//
// SINGLE-TRANSLATION-UNIT include -- only include from main.cpp

#include "SeedGuard.hpp"
#include "PortfolioGuard.hpp"
#include "IbkrExec.hpp"   // thin TWS-free IBKR execution interface
#include "BigCapMomoIbkr.hpp"   // thin TWS-free interface to the in-process BigCapMomo engine
#include "NqMomoIbkr.hpp"       // thin TWS-free interface to the in-process NQ/MNQ futures momentum engine
#include "QndxSqfIbkr.hpp"      // thin TWS-free interface to the in-process QNDX (Nasdaq-100 SQF) book
#include "CryptoLedgerInbound.hpp"  // route IBKRCrypto shadow closes into the Omega ledger (OMEGA_CRYPTO_INBOUND=1)
#include "GoldBeFloorCompanion.hpp" // AUPOS/AUNEG gold BE-floor companion (native C++, /api/gold_companion)
#include "XagBeFloorCompanion.hpp"  // XAGPos/XAGNeg SILVER BE-floor companion (native C++, /api/xag_companion)
#include "UsoilBeFloorCompanion.hpp"// USOILPos/USOILNeg WTI CRUDE BE-floor companion (native C++, /api/usoil_companion)
#include "FxBeFloorCompanion.hpp"   // per-pair FX BE-floor companion (EUR/GBP/JPY/AUD/NZD) -> /api/fx_companion
#include "FxUpJumpLadderCompanion.hpp" // per-pair FX jump LADDER companion (long EUR/GBP/NZD/AUD + short USDCAD) -> /api/fxladder_companion
                                       //   + index_upjump_ladder_book() (US500/NAS100/GER40) -> /api/idxladder_companion
#include "IndexRiskGate.hpp"           // omega::index_risk_off() (GER40 ladder bull-gate)
#include "IndexBeFloorCompanion.hpp"// per-symbol index BE-floor companion (US500/NAS100/DJ30/GER40) -> /api/index_companion
// (JumpRiderEngine.hpp include REMOVED — engine culled/tombstoned S-2026-07-10)
#include "StockDayMoverBeFloorCompanion.hpp"// per-name BIGCAP day-mover BE-floor companion (RETIRED S-2026-07-07e) -> /api/stockmover_companion
#include "StockDayMoverLadderCompanion.hpp" // per-name BIGCAP day-mover UP-JUMP LADDER companion (39 stocks) -> /api/stockladder_companion
#include "StockDipTurtleEngine.hpp" // per-name US-stock StockDip (ConnorsRSI2 archetype) + StockTurtle (Donchian 20/10) daily-close books (S-2026-07-08c)
#include "BigCap2pctImpulseCompanion.hpp"  // per-name BIGCAP +2%-impulse / 20d-breakout LONG-only LOOSE-RIDE book (S-2026-07-09) -> /api/bigcap2pct_companion
#include "StallCompanion.hpp"       // 25 gold/index giveback-clip books (native C++ port of stall_accountant.py) -> /api/companion

static void init_engines(const std::string& cfg_path)
{
    // Stamp process boot time for the central phantom-trade net (trade_lifecycle.hpp).
    // Any closed trade whose entryTs predates this = opened on a warm-seed historical
    // bar or carried from a prior session -> dropped at the ledger, never recorded.
    g_process_boot_sec = static_cast<int64_t>(std::time(nullptr));
    printf("[OMEGA-INIT] process boot stamp = %lld (phantom-trade net armed)\n",
           (long long)g_process_boot_sec);

    // ── AuroraGate engine hook: routes omega::aurora_allow() (called by the
    // trend/breakout gold engines -- GoldVolBreakoutM30, SessionMomentum,
    // GoldOrbRetrace) through g_aurora_gate. Fail-open on stale/missing tape;
    // kill instantly via g_aurora_gate.enabled_=false. enter_directional uses
    // g_aurora_gate directly. Mean-rev + multi-cell engines intentionally NOT
    // gated (room-to-wall logic suits trend/breakout only).
    omega::aurora_gate_hook() = [](const char* s, bool is_long, int64_t now_ms) {
        return g_aurora_gate.allow(std::string(s), is_long, now_ms);
    };
    printf("[OMEGA-INIT] AuroraGate hook installed (enabled=%d path=%s)\n",
           (int)g_aurora_gate.enabled_, g_aurora_gate.path_.c_str());

    // ── MacroGoldGate (2026-06-17): macro-hostile de-risk tightening for the gold
    // book, layered ON TOP of the price-based gold_regime() core. Fed each gold
    // tick in tick_gold.hpp; all 8 long-only gold engines inherit it via
    // long_blocked(). Fail-safe (false on missing/stale feed). Producer:
    // tools/macro_gold_gate.py (daily). Kill via g_macro_gold_gate.enabled_=false.
    printf("[OMEGA-INIT] MacroGoldGate installed (enabled=%d path=%s, fail-safe=false-on-stale)\n",
           (int)g_macro_gold_gate.enabled_, g_macro_gold_gate.path_.c_str());

    // ── S51 2026-05-27: PortfolioGuard config ──────────────────────────────
    // After S49/S50 real-class audit confirmed 16 XAU D1/H4 engines have real
    // edge, the concurrency cap must protect against correlated stacking:
    // multiple D1 engines fire LONG on the same trend day → 16 simultaneous
    // longs amplifies the worst-case drawdown linearly. Cap at 2 leaves
    // headroom for two-engine confirmation patterns (Turtle + Pullback) but
    // blocks the full-zoo-stack adverse case.
    //   Why 2 not 4: post-audit MDD-to-gross ratios on TrendFollowD1 (95%)
    //   and TrendFollow2h (75%) show drawdown stacking risk dominates. Two
    //   uncorrelated entries can absorb the same trend; four-plus invites
    //   one bad day wiping a week of gains. Kill-file + vol-scaled lot
    //   remain available as opt-in per-engine if needed.
    omega::pg::g_pg_cfg.max_concurrent_positions = 2;
    omega::pg::g_pg_cfg.kill_file_enabled        = true;
    omega::pg::g_pg_cfg.kill_file_path           = "C:/Omega/KILL_SWITCH.lock";
    omega::pg::g_pg_cfg.kill_file_recheck_sec    = 30;
    // Vol-scaled lot + HTF scalar remain opt-in (per-engine wiring not
    // applied portfolio-wide -- S44 lesson: hard HTF gate destroyed edge).
    omega::pg::g_pg_cfg.vol_scale_enabled   = false;
    omega::pg::g_pg_cfg.htf_scalar_enabled  = false;
    printf("[OMEGA-INIT] PortfolioGuard: cap=%d kill_file=%s recheck=%ds\n",
           omega::pg::g_pg_cfg.max_concurrent_positions,
           omega::pg::g_pg_cfg.kill_file_path,
           omega::pg::g_pg_cfg.kill_file_recheck_sec);

    // ── Seed-path anchor (2026-05-22 incident fix) ──────────────────────
    // Windows services inherit CWD = C:\Windows\System32, which breaks
    // every "phase1/...", "data/...", "logs/..." relative path the rest
    // of init_engines uses. Pin CWD to the exe directory before anything
    // touches disk. See CLAUDE.md > Engine Warm-Seed Mandate.
    omega::anchor_cwd_to_exe_dir();

    // ── SHADOW/LIVE policy (ISSUE-5) ──────────────────────────────────────
    // Global default for newly-stamped engines (#4 Class A + C).
    // Engines in the hardcoded-lock cohort (MCE, RSIReversal, PDHL, H1Swing,
    // H4Regime, ISwingSP/NQ) override this below
    // with `shadow_mode = true` AFTER this assignment -- those locks are
    // load-bearing safety interlocks and are NOT affected by g_cfg.mode.
    // 2026-05-08 DEEPSTRIKE: hard-pinned to true regardless of g_cfg.mode.
    //   Single-engine live deploy policy: ONLY engines with an explicit
    //   `shadow_mode = false` pin go live. Everything else (whether using
    //   kShadowDefault or otherwise) stays shadow regardless of the global
    //   mode flag. This is the defensive whitelist the user asked for --
    //   "max protection". Today the only authorised live engine is
    //   GoldMicroScalper (see its explicit `= false` pin below). To re-arm
    //   mode-following behaviour for any other engine, add an explicit
    //   `engine.shadow_mode = false;` line elsewhere in this function;
    //   reverting this line to `(g_cfg.mode != "LIVE")` would re-arm ALL
    //   engines that use kShadowDefault and is the wrong unit of change.
    const bool kShadowDefault = true;

    // ── ISSUE-5 newly-stamped engines: follow kShadowDefault ─────────────
    // Engines with no prior explicit shadow_mode wiring in engine_init.hpp.
    // All flip to LIVE atomically when g_cfg.mode = "LIVE" in omega_config.ini.
    // Class A (stamped 2026-04-21):
    // 41-cell walk-forward sweep showed no edge on 9 days of XAUUSD tick data.
    // See globals.hpp tombstone comment for full details.
    g_ema_cross.shadow_mode    = kShadowDefault;  // EMACrossEngine
    // ── S37d 2026-05-26: EMERGENCY ENGINE CULL based on 14d shadow audit ──
    // 73 bug-polluted rows removed (held<0 or >7d, |pnl|>$1000) before audit.
    // 43 FORCE_CLOSE rows also excluded (shutdown phantoms, not real edge).
    // Audit results (n>=5 trades, PF<1.0 = DISABLE):
    //   CandleFlowEngine     411n WR 47.0%  PF 0.69  -$1077  -> DISABLE
    //   DomPersistEngine      59n WR 28.8%  PF 0.29  -$ 702  -> DISABLE
    //   XAUUSD_BRACKET        86n WR 12.8%  PF 0.26  -$ 324  -> DISABLE
    //   GoldFlowEngine        26n WR 23.1%  PF 0.13  -$ 256  -> DISABLE
    //   BBMeanRev             45n WR 11.1%  PF 0.51  -$ 137  -> DISABLE
    //   IndexFlow (x4)        34n WR 23.5%  PF 0.15  -$ 112  -> DISABLE
    //   EMACross              83n WR 31.3%  PF 0.85  -$  74  -> DISABLE (borderline)
    // (MacroCrash already disabled L626 since S99b 2026-05-18.)
    // SURVIVORS kept enabled: HybridBracketGold/Index, AsianRange,
    // Us303BarMomH1, Us30Ensemble (new InsBrkH1).
    // S37d cull REVERTED 2026-05-26 (build-fix): these engine classes lack
    // an `.enabled` field. They are already shadow_mode=kShadowDefault above
    // (and 3 of the original 8 -- DomPersist, GoldFlow, BBMeanRev -- no
    // longer exist as globals; that audit data was historical from removed
    // engines). Live impact: nil (shadow_mode keeps them paper-only).
    // To truly cull these classes, would need to either add an `.enabled`
    // member to each engine OR strip their on_tick dispatch sites. Out of
    // scope for the build-recovery commit; revisit if a surviving engine
    // shows real-live (not historical-shadow) losses.
    // TOMBSTONE 2026-05-01 (S52 trade-quality follow-up):
    //   153-combo parameter sweep on data/l2_ticks_2026-04-16.csv: 0/153
    //   produced positive PnL. Best combo -$4.30, worst -$225, mean -$81.
    //   Original 12-trade / 2-day backtest was a tiny lucky window.
    //   Disabling rather than deleting -- file remains for reference.
    //
    // ***  S37 AUDIT REVERSAL (2026-05-27): BASELINE WAS CONTAMINATED.  ***
    // The Apr-16 capture was inside the cTrader-off / FIX-not-wired window
    // when gold_l2_imbalance was permanently stuck at 0.5 (default-init).
    // Every one of the 153 sweep combos saw a constant signal at col index
    // 4 (l2_imb=0.5 forever) and col 15 (micro_edge=0.0 forever). The
    // "0/153 produced positive PnL" finding is an artifact of constant-
    // signal data, NOT a no-edge verdict on the engine. RSIExtremeTurn
    // should be re-tested against a post-S8 (2026-05-06+) L2 capture
    // before the disable is treated as final. data/l2_ticks_2026-04-16.csv
    // is retained for reproducibility of the original (contaminated)
    // sweep but DO NOT use it as a research baseline -- any sweep run
    // against it is re-fitting the same dead signal.
    // 11-day/3.4M tick sweep showed no edge. See globals.hpp tombstone.
    // 96-cell walk-forward sweep. See globals.hpp tombstone comment.
    g_candle_flow.shadow_mode  = kShadowDefault;  // CandleFlowEngine -- restored 2026-04-29 with audit-tightened gates, shadow only
    // S11 P3b (2026-05-07): IndexHybridBracket (4 instances) culled.
    //   Original S8 shadow_mode=true pin (RR-asymmetric bleed analysis from
    //   2026-05-06 NAS100 tape) preserved in commit ba5f0e9 (S10 P3a) and
    //   NEXT_SESSION_S11.md. Engines were dispatch-removed in P3a and have
    //   now been fully removed: globals decls, init blocks, register_engine
    //   calls, heartbeat registrations, heartbeat pulses, and gate-reads
    //   across tick_indices.hpp / on_tick.hpp / globals.hpp::index_any_open.
    // S12 P3c (2026-05-07): IndexHybridBracketEngine.hpp header file DELETED
    //   + all #include refs removed (production + backtest). Engine family
    //   fully retired.
    // IndexFlowEngine (4 instances, uniform):
    // shadow_mode lives on private IdxOpenPosition pos_; use set_shadow_mode() proxy.
    //
    // 2026-05-14 (part W): S63 VWR-pattern in-flight protection — explicit
    //   re-affirm of the class default (LOSS_CUT only flavor — IndexFlow
    //   uses a staircase ATR trail that already covers giveback well, so
    //   only the cold-loss phase is added — see IndexFlowEngine.hpp:547-552
    //   for the design comment). The override matches the class default
    //   (IndexFlowEngine.hpp:553 declares LOSS_CUT_PCT = 0.07) but documents
    //   intent and makes activation discoverable from engine_init.hpp alone.
    //   Mirrors g_vwap_rev_ger40 precedent at engine_init.hpp:649 and
    //   g_ustec_tf_5m at engine_init.hpp:976. See
    //   outputs/S63_STATE_CLASSIFICATION_2026-05-14.md §3.1 for the audit
    //   that confirmed STATE A via class-default route (mgmt-path at
    //   IndexFlowEngine.hpp:651-654 invokes pos_.manage(..., LOSS_CUT_PCT)
    //   every tick; check at L419 of IdxOpenPosition::manage).
    //
    //   LOSS_CUT_PCT = 0.07 -> US500@7400 : ~5.18pt cold-loss cut.
    //                       -> USTEC@28000: ~19.6pt.
    //                       -> NAS100@22500: ~15.75pt.
    //                       -> DJ30@45000 : ~31.5pt.
    g_iflow_sp.LOSS_CUT_PCT   = 0.07;
    g_iflow_nq.LOSS_CUT_PCT   = 0.07;
    g_iflow_nas.LOSS_CUT_PCT  = 0.07;
    g_iflow_us30.LOSS_CUT_PCT = 0.07;
    g_iflow_sp.set_shadow_mode(kShadowDefault);
    g_iflow_nq.set_shadow_mode(kShadowDefault);
    g_iflow_nas.set_shadow_mode(kShadowDefault);
    g_iflow_us30.set_shadow_mode(kShadowDefault);

    // -- IndexIntradayDriftEngine (S37-Z 2026-05-28) -------------------------
    // BUY at first H1 of UTC day / SELL at last H1. 3 viable instances.
    // Audit (see include/IndexIntradayDriftEngine.hpp header for full table):
    //   SPXUSD Sharpe +0.77 net, WF1 +1.28, WF2 +0.48 -- VIABLE
    //   USA30  Sharpe +1.31 net, WF1 +1.04, WF2 +1.53 -- VIABLE (strongest)
    //   UK100  Sharpe +1.12 net, WF1 +1.01, WF2 +1.46 -- VIABLE (strongest WF)
    // NSXUSD + GER40 NOT wired -- both flagged MARGINAL (one walk-fwd half
    // negative). Re-audit after 6mo additional data before wiring.
    // All 3 start in shadow_mode per kShadowDefault until 30+ live trades
    // confirm post-deploy expectancy. ENTRY_SIZE matches existing index
    // bracket defaults (0.01) and scales via AdaptiveRiskManager.
    // S11 P3b (2026-05-07): GoldHybridBracketEngine culled.
    //   Original S8 shadow_mode=true pin (2026-05-06 RR-asymmetric bleed:
    //   7 SL hits in 13:00-14:07 UTC window for -$50 net, ~0.4:1 RR needing
    //   65%+ WR to break even) preserved in commit ba5f0e9 (S10 P3a) and
    //   NEXT_SESSION_S11.md. Engine was dispatch-removed in P3a and has now
    //   been fully removed: globals decl, init block, register_engine call,
    //   heartbeat registration, heartbeat pulse, g_open_positions source, and
    //   gate-reads across tick_gold.hpp / globals.hpp / quote_loop.hpp.
    // S12 P3c (2026-05-07): GoldHybridBracketEngine.hpp header file DELETED
    //   + all #include refs removed (production + backtest). Engine family
    //   fully retired.
    // 2026-05-01 SESSION_h: GoldMidScalperEngine -- pinned shadow-only on
    //   first deployment regardless of g_cfg.mode. New engine, untested in
    //   live conditions, $20-40 capture zone. Promote to kShadowDefault
    //   (i.e. follow g_cfg.mode) after a 2-week paper validation showing
    //   positive expectancy. Until then this line stays as `true` not
    //   `kShadowDefault`.
    // 2026-05-08 USER REQUEST: only g_gold_microscalper trades on gold;
    //   force every other gold engine to shadow regardless of g_cfg.mode.
    // GoldMidScalperEngine has no `enabled` member; its register_engine was REMOVED
    // 2026-06-02 so it cannot fire. shadow_mode pinned. RETIRED (scalp family).
    g_gold_midscalper.shadow_mode = true;
    // 2026-05-08 DEEPSTRIKE LIVE PROMOTION (authorised by user in chat):
    //   GoldMicroScalper goes live on account 8077780 at 0.03 lot. This
    //   is the SINGLE engine authorised for live trading under the
    //   single-engine deploy policy. All other gold + non-gold engines
    //   stay shadow via the kShadowDefault hard-pin at the top of this
    //   function plus their existing explicit shadow pins.
    //
    //   Live-promotion configuration:
    //     - shadow_mode = false        (this line)
    //     - LIVE_LOT = 0.03            (GoldMicroScalperEngine.hpp)
    //     - MAX_SPREAD = 0.5pt         (tightened from 1.0 for max protection)
    //     - max_lot_gold = 0.03        (omega_config.ini, matches LIVE_LOT)
    //     - mode = SHADOW              (omega_config.ini stays SHADOW;
    //                                   per-engine pin is the live override)
    //     - RiskMonitor logging_only = false (auto-pin on trip)
    //     - g_nbm_gold_london pinned shadow (was the unpinned default)
    //
    //   Risk profile: real-cost BE_WR ~ 82.1% at 0.03 lot. Backtest WR was
    //   92.5%, leaving ~10pp of headroom. RiskMonitor TRIP_WR = 82.16% will
    //   auto-shadow this engine if rolling 50+ trade WR drops to BE.
    //
    //   To revert to shadow without rebuilding: there is no runtime toggle
    //   in v1; either flip this line back to `= true` and redeploy, or
    //   the RiskMonitor will auto-pin on trip.
    //
    //   2026-05-08 AMENDMENT (S21, authorised by user in chat):
    //     LIVE_LOT raised 0.03 -> 0.20 -> 0.30 (current) in
    //     GoldMicroScalperEngine.hpp and max_lot_gold raised 0.03 -> 0.30
    //     (current) in omega_config.ini. The "0.03 lot" / "max_lot_gold =
    //     0.03" values in the comment block above are SUPERSEDED -- kept
    //     for the original authorization paper trail. Authoritative
    //     current values:
    //       LIVE_LOT       = 0.30  (GoldMicroScalperEngine.hpp)
    //       max_lot_gold   = 0.30  (omega_config.ini:166)
    //       MAX_SPREAD     = 0.5pt (unchanged from original live promotion)
    //       account        = 8077780 (unchanged)
    //       shadow_mode    = false (unchanged -- this line below)
    //
    //   2026-05-08 LIVE-MODE FLIP (S21 audit, same authorisation):
    //     omega_config.ini: mode=SHADOW -> mode=LIVE. The original
    //     DEEPSTRIKE comment block at the top of init_engines() claimed
    //     `shadow_mode = false` alone made an engine live regardless of
    //     g_cfg.mode. That was wrong. order_exec.hpp:72 hard-gates
    //     send_live_order on g_cfg.mode == "LIVE", so under mode=SHADOW
    //     every microscalper close was being silently dropped at the
    //     broker submit boundary -- account 8077780 saw zero trades for
    //     21 minutes despite the engine firing ~30 paper trades during
    //     that window. With mode=LIVE the broker submit boundary opens
    //     and microscalper actually places real orders. Single-engine
    //     deploy semantics preserved by hard-pinning bracket engines'
    //     shadow_mode=true regardless of g_cfg.mode (see the wire_bracket
    //     lambda comment further below). Other engines either have
    //     explicit shadow_mode=true pins or use kShadowDefault=true; only
    //     g_gold_microscalper has shadow_mode=false in production source.
    //
    //   Risk delta vs original 0.03 promotion: per-trade $ outcomes 10x
    //   larger (TP win ~+$20.40 net, SL hit ~-$93.30 net at 0.30 lot).
    //   RiskMonitor TRIP_WR=0.8216 anchored to backtest expectancy, not
    //   $ threshold; auto-pin on trip remains the in-process safety
    //   circuit. OMEGA.ps1 stop on the VPS is the manual kill switch
    //   (note: shutdown does NOT currently force-close positions -- the
    //   proper fix is queued for next deploy; today the stop+manual-
    //   cTrader-close workaround applies if a position is open at stop).
    // ===== SCALP/MICRO FAMILY — HARD RETIREMENT (operator directive S-2026-06-19) =====
    // The sub-minute/microstructure scalp class is proven unviable across audits
    // (GoldBracket PF0.36, XauusdFvg 0.50, VWAPRev 0.54/0.95, TrendPB 0.24/0.27,
    // GoldScalpPyramid S45, BBandScalp 27-cfg/154M-tick PF0.07-0.09, 78 HF gold
    // configs all neg). Full retirement = enabled=false (no dispatch/fire/GUI), NOT
    // shadow-pin. Tombstone: include/retired_micro_engines.hpp.
    // CONFIG-AUTHORITY FIX: FxScalpPyramidEngine struct DEFAULTS enabled=true, and
    // these 5 had NO explicit disable -> they were live despite "S45-disabled" docs.
    // GoldMicroScalperEngine has no `enabled` member — it is disabled via the
    // global g_disable_microscalper (globals.hpp:424, =true since S-2026-06-02 cull).
    // register_engine uses !g_disable_microscalper, so it is already OFF. shadow_mode below kept.
    g_gold_microscalper.shadow_mode = true;   // S37-Z 2026-05-28: pinned shadow. Operator decision: scalp family proven unviable across audits this session (GoldBracket PF=0.36, XauusdFvg PF=0.50, VWAPRev PF=0.54/0.95, TrendPB PF=0.24/0.27, GoldScalpPyramid S45-disabled, 5 FxScalpPyramid S45-disabled, BBandScalp disabled). Microscalper was the last live scalp -- pin shadow to stop bleed. Re-promote only after a documented edge (audit + walk-forward + 30-trade shadow verification).

    // 2026-05-08 S20+: RiskMonitor wiring -------------------------------------
    // Logging-only per-engine surveillance. Watches WR break-even, fire-rate
    // over/under, and spread-at-entry drift. Loads its per-engine thresholds
    // from data/risk_monitor_thresholds.csv (generated by the standalone
    // backtest/calibrate_risk_thresholds tool). v1 emits WOULD-TRIP log lines
    // to logs/risk_monitor_<DATE>.log; never touches any engine.shadow_mode.
    //
    // To extend monitoring to another engine:
    //   (1) add a row to ENGINE_TABLE in calibrate_risk_thresholds.cpp,
    //       recompile + re-run that tool.
    //   (2) bind the engine's fire hook below (parallel to the microscalper
    //       binding). Engines without an on_fire_hook member can still be
    //       monitored on the close side; only the fire-rate check needs
    //       the fire-side wiring.
    g_risk_monitor.load_thresholds("data/risk_monitor_thresholds.csv");

    g_gold_microscalper.on_fire_hook = [](int64_t now_s) {
        g_risk_monitor.on_fire("MicroScalperGold", now_s);
    };

    // 2026-05-08 DEEPSTRIKE auto-pin callback: fired by RiskMonitor when any
    //   of the three checks (WR / fire-rate / spread) hits its trip
    //   threshold and `logging_only = false`. Idempotent: only flips the
    //   engine's shadow_mode the first time, subsequent calls are no-ops.
    //   This is the actual safety circuit-breaker for the live deploy.
    g_risk_monitor.register_shadow_pin_cb("MicroScalperGold",
        [](const std::string& reason) {
            if (!g_gold_microscalper.shadow_mode) {
                g_gold_microscalper.shadow_mode = true;
                printf("[RISK-MON] AUTO-PIN MicroScalperGold to SHADOW: %s\n",
                       reason.c_str());
                fflush(stdout);
            }
        });

    // 2026-05-08 DEEPSTRIKE belt-and-braces (BUILD-FIX 2026-05-08, S21):
    //   Originally tried `g_nbm_gold_london.shadow_mode = true;` here as an
    //   explicit shadow pin to back up the kShadowDefault policy. That line
    //   broke the VPS build with C2039: NoiseBandMomentumEngine
    //   (CrossAssetEngines.hpp:2379) has NO public `shadow_mode` member.
    //   The cross-asset cohort stores shadow_mode on the private
    //   CrossPosition pos_ field (CrossAssetEngines.hpp:156), and only
    //   IndexFlowEngine exposes a `set_shadow_mode(bool)` proxy
    //   (IndexFlowEngine.hpp:548). NBM does not have an analogous proxy,
    //   so there is no compile-clean way to flip shadow on it from here.
    //
    //   Removing this pin is SAFE in the current build: NBM's entry-
    //   dispatch code was removed in the engine-cull audit (96-cell
    //   walk-forward + 11-day/3.4M tick sweep both showed zero profitable
    //   configs; production cell WR=22%, net -$47/14d -- see the
    //   tombstone comment block at tick_gold.hpp:2302-2318). The only
    //   remaining call site -- tick_gold.hpp:2298-2300 -- runs NBM.on_tick
    //   strictly inside `if (has_open_position())`, i.e. position-management
    //   only. There is no path by which NBM can open a fresh position, so
    //   live-order risk is zero regardless of shadow_mode.
    //
    //   If a future session re-arms NBM entry dispatch and a runtime
    //   shadow gate becomes needed, add a proxy to NoiseBandMomentumEngine
    //   parallel to IndexFlowEngine.hpp:548:
    //     void set_shadow_mode(bool b) noexcept { pos_.shadow_mode = b; }
    //   and call `g_nbm_gold_london.set_shadow_mode(true);` here.
    // g_nbm_gold_london.shadow_mode = true;  // intentionally NOT compiled --
    //                                          field does not exist on NBM.
    // ========================================================================
    // [S99-DISABLED -- STALE WIRING, 2026-06-23 audit] The 5 session-open
    // compression-bracket engines wired below (Eurusd/Gbpusd/UsdjpyAsian/
    // AudusdSydney/NzdusdAsian Open) were KILLED by S99 (2026-05-23) for
    // negative expectancy (cost-to-target 5-15% on 8-20pip TPs vs 0.5-1.4pip
    // spread; live edge collapsed) and REPLACED by FxTurtleH4Engine — see
    // FxTurtleH4Engine.hpp PROVENANCE. The S99 kill is enforced at the DISPATCH
    // level: tick_fx.hpp on_tick_{eurusd L140 / gbpusd L272 / usdjpy L404 /
    // audusd L490 / nzdusd L576} early-`return` BEFORE the engine's `.on_tick()`,
    // so all 5 open ZERO positions. The shadow_mode/cancel_fn lines below are
    // therefore INERT (the engines are configured + heartbeat-registered but
    // never ticked). Kept as no-ops; not flipped to enabled=false because these
    // classes carry no `enabled` field (the dispatch early-return IS the off
    // switch). DO NOT mistake these for active shadow engines — they are dead.
    // The 2026-06-23 census subagent mislabeled them "SHADOW active" by reading
    // shadow_mode without checking the dispatch kill; this banner prevents recurrence.
    // ========================================================================
    // 2026-05-02: EurusdLondonOpenEngine -- pinned shadow-only on first
    //   deployment regardless of g_cfg.mode. First FX engine since the
    //   2026-04-06 global FX disable; new engine model (compression-breakout
    //   with BE-lock + news-blackout + 06-09 UTC session gate, NOT inheriting
    //   the disabled MacroCrash signal model). Promote to kShadowDefault
    //   after a 2-week paper validation showing >=30 trades with WR >=35%
    //   net positive after costs. Until then this line stays as `true`.
    g_eurusd_london_open.shadow_mode = true;
    // Cancel callback: matches g_gold_midscalper pattern. Used
    //   when PENDING TIMEOUT or one side fills (cancel the loser).
    g_eurusd_london_open.cancel_fn   = [](const std::string& id) { send_cancel_order(id); };
    // 2026-05-02: UsdjpyAsianOpenEngine -- pinned shadow-only on first
    //   deployment regardless of g_cfg.mode. Asian-session compression bracket
    //   on USDJPY, 00:00-04:00 UTC (Tokyo open). Pre-sweep S56-inherited
    //   defaults; USDJPY-specific Kelly analysis from the parallel sweep
    //   harness should retune before live promotion. Promote to kShadowDefault
    //   after a 2-week paper validation showing >=30 trades with WR >= 60%
    //   net positive after costs. Until then this line stays as `true`.
    g_usdjpy_asian_open.shadow_mode = true;
    g_usdjpy_asian_open.cancel_fn   = [](const std::string& id) { send_cancel_order(id); };
    // 2026-05-04 (audit-fixes-36 + S57): GbpusdLondonOpenEngine -- pinned
    //   shadow-only on first deployment regardless of g_cfg.mode. Cable
    //   sister to EurusdLondonOpen; targets the 07:00-10:00 UTC London
    //   compression window with GBP volatility (~50% wider than EUR --
    //   MIN/MAX_RANGE 12-75 pips vs EUR 8-50). News-blackout gated for
    //   BoE/UK CPI/UK GDP plus NFP/CPI/FOMC. Promote to kShadowDefault
    //   after a 2-week paper validation showing >=30 trades with WR >=35%
    //   net positive after costs (matches EURUSD S56 promotion gate).
    //   Until then this line stays as `true`.
    g_gbpusd_london_open.shadow_mode = true;
    g_gbpusd_london_open.cancel_fn   = [](const std::string& id) { send_cancel_order(id); };
    // 2026-05-04 (audit-fixes-36 + S57): AudusdSydneyOpenEngine -- pinned
    //   shadow-only on first deployment regardless of g_cfg.mode. Aussie
    //   sister to UsdjpyAsianOpen; targets the 22:00-02:00 UTC Sydney
    //   open + Tokyo handoff window with AUD pip math (1 pip = 0.0001 --
    //   identical scale to EUR/GBP, USD-quote major). News-blackout gated
    //   for RBA/AU CPI/AU jobs plus NFP/CPI/FOMC. Promote to kShadowDefault
    //   after a 2-week paper validation showing >=30 trades with WR >= 60%
    //   net positive after costs (matches USDJPY S56 promotion gate).
    //   Until then this line stays as `true`.
    g_audusd_sydney_open.shadow_mode = true;
    g_audusd_sydney_open.cancel_fn   = [](const std::string& id) { send_cancel_order(id); };
    // 2026-05-04 (audit-fixes-36 + S57): NzdusdAsianOpenEngine -- pinned
    //   shadow-only on first deployment regardless of g_cfg.mode. Kiwi
    //   sister to AudusdSydneyOpen; targets the 22:00-04:00 UTC Wellington
    //   open + Tokyo handoff window with NZD pip math (1 pip = 0.0001 --
    //   identical scale to EUR/GBP/AUD, USD-quote major). News-blackout
    //   gated for RBNZ/NZ CPI/NZ jobs plus NFP/CPI/FOMC. Promote to
    //   kShadowDefault after a 2-week paper validation showing >=30 trades
    //   with WR >= 60% net positive after costs (matches USDJPY S56 gate).
    //   Until then this line stays as `true`. Retires the last
    //   [FX-NO-ENGINE] diag stub from tick_fx.hpp::on_tick_audusd.
    g_nzdusd_asian_open.shadow_mode = true;
    g_nzdusd_asian_open.cancel_fn   = [](const std::string& id) { send_cancel_order(id); };
    // 2026-05-02: XauusdFvgEngine -- pinned shadow-only on first deployment
    //   regardless of g_cfg.mode. FVG-on-15m engine for XAUUSD per
    //   docs/DESIGN_XAUUSD_FVG_ENGINE.md  §7.1 / HANDOFF_FVG_BACKTEST.md.
    //   Backtest expects PF 1.5-1.8, ~50% WR, ~25 trades/month. Promote to
    //   kShadowDefault after a 3-month shadow run that clears the four-gate
    //   quarterly re-validation (n>=50, PF>=1.2, PF>All, cost-stress 2x
    //   PF>=1.0). Until then this line stays as `true`. cancel_fn is wired
    //   for API parity with the cohort -- the FVG engine does not place
    //   pending stop orders in v1, so the callback is unused.
    //   on_close_cb routes closed trades through (a) the standard ledger
    //   path via handle_closed_trade(tr) AND (b) a side-channel
    //   live_xauusd_fvg.csv writer for the quarterly v3 re-feed
    //   (omega::xauusd_fvg::log_xauusd_fvg_csv pulls score_at_entry /
    //   atr_at_entry / session / fvg_age_bars / bars_held from the engine's
    //   last_extras() snapshot, which is set BEFORE on_close fires inside
    //   _close()).
    // 2026-05-14 (part W): S63 VWR-pattern in-flight protection — explicit
    //   re-affirm of the class defaults (XAU-scaled) for grep visibility.
    //   Mirrors g_vwap_rev_ger40 precedent at engine_init.hpp:649-651 and
    //   g_ustec_tf_5m at engine_init.hpp:976-978: the override matches the
    //   class default but documents intent and makes activation
    //   discoverable from engine_init.hpp alone. See
    //   outputs/S63_STATE_CLASSIFICATION_2026-05-14.md §3.2 for the audit
    //   that confirmed STATE A via class-default route (fields declared
    //   at XauusdFvgEngine.hpp:142-144; mgmt-path at L1063-1083 fires
    //   every tick).
    //
    //   LOSS_CUT_PCT  = 0.05  -> XAU@3700: ~$1.85 cold-loss cut.
    //   BE_ARM_PCT    = 0.03  -> XAU@3700: ~$1.11 mfe arms ratchet.
    //   BE_BUFFER_PCT = 0.012 -> XAU@3700: ~$0.44 buffer (typical XAU spread).
    g_xauusd_fvg.LOSS_CUT_PCT  = 0.05;
    g_xauusd_fvg.BE_ARM_PCT    = 0.03;
    g_xauusd_fvg.BE_BUFFER_PCT = 0.012;
    // 2026-05-15 (part-L sweep): FVG backtest on 154M-tick fresh tape
    //   (Mar 2024 – Apr 2026) confirms independent edge:
    //   IS PF=1.57, OOS PF=2.12, 665 trades, WR=48.4%.
    //   OOS *improved* vs IS (negative decay). Both directions profitable.
    //   S63 in-flight protection already wired (LOSS_CUT=0.05, BE_ARM=0.03,
    //   BE_BUFFER=0.012 — confirmed STATE A at part-W audit).
    //   Promoting from shadow to live based on this evidence.
    g_xauusd_fvg.shadow_mode = true;  // 2026-05-29: forced shadow during live-feed cutover (demo->live FIX). Restore false after validation.
    // ⛔ TOMBSTONE note S-2026-06-15b (operator cull): XauusdFvg died on full NAS M1 (slice artifact;
    //   same finding that culled g_fvgcont_nas). Already gated OFF via g_disable_xauusd_fvg=true
    //   (globals.hpp:412) -- the engine has NO `enabled` field; the disable flag IS the off-switch.
    //   Leaving it; do NOT flip g_disable_xauusd_fvg to false without cross-regime proof.
    g_xauusd_fvg.cancel_fn   = [](const std::string& id) { send_cancel_order(id); };
    g_xauusd_fvg.on_close_cb = [](const omega::TradeRecord& tr) {
        handle_closed_trade(tr);                              // standard ledger path
        omega::xauusd_fvg::log_xauusd_fvg_csv(tr, g_xauusd_fvg);
    };
    g_xauusd_fvg.warmup_csv_path = "phase1/signal_discovery/warmup_XAUUSD_M15.csv";
    omega::warmup_or_die(g_xauusd_fvg, "XauusdFvg");

    // ---- GoldScalpPyramid (2026-05-18) ----------------------------------------
    // M5 Donchian breakout + EMA filter + aggressive 4-phase trail.
    //
    // SWEEP v2 RESULTS (154M ticks, 152K M5 bars, Mar 2024 - Apr 2026, 162 configs):
    //   Harness v2: chronological intra-bar ordering (best_ts vs worst_ts)
    //   fixes pyramid never-firing artifact from v1.
    //
    //   Best: LB=8 SL=1.5 TP=3.0 Trail=0.12 Pyr=Y
    //   n=5436  PnL=$+15083.97  WR=71.0%  PF=1.45  DD=$-453.91  Lyrs=1.1
    //
    //   Runner-up (same params, Pyr=N):
    //   n=5436  PnL=$+14082.84  WR=71.0%  PF=1.42  DD=$-450.27
    //   Pyramid adds +$1001 (+7.1%) on best config.
    //
    //   SL sensitivity: wider SL=1.5 dominates (WR=71%, trail limits losses).
    //   Tight SL=0.8 sees biggest pyramid uplift (+33% PnL, Lyrs=1.4) but
    //   lower absolute PnL. Trail=0.12 universally best across all configs.
    //
    // REGIME WARNING: 18-month chop period (Mar24-Sep25) then 7-month trend
    //   run (Oct25-Apr26). The edge is real but regime-dependent.
    //   Shadow-observe through one full chop cycle before promoting to live.
    //
    // L2 integration (same session): entry confirmation, wall gate, adaptive
    //   trail, pyramid gating, lot sizing. Live-only enhancement — degrades
    //   gracefully when gold_l2_real=false.
    //
    // shadow_mode=true. Do NOT promote until observed through a chop regime.
    // S63 in-flight protection: same VWR pattern as XauusdFvg.
    // 2026-05-18 S100c: DISABLED. The "validated GSP $15K / PF 1.45" edge
    // was a bar-level-harness artifact. The audit (backtest/gsp_s63_audit_
    // bt.cpp) drove the GSP CLASS tick-level with S63=OFF (matching the
    // harness shape exactly) and produced -$16,935 / PF 0.62 over the
    // same 154M tick / 26-month tape. The harness reported 72 TP_HITs
    // (+$3,998); tick-level execution produced 0. AvgWin collapsed from
    // $12.58 (harness) to $5.54 (class) because tick-level exits the
    // trail before the bar's intra-bar peak is reached. Disabling
    // pending tick-level-validated exit philosophy (see SESSION_HANDOFF_
    // 2026-05-18b.md). Do NOT re-enable without harness-class agreement
    // to within 10% on PnL.
    // S38a 2026-05-26: re-enable in SHADOW with chop filter + cost-aware BE.
    // Speed advantage now available (LD4 colo sub-ms confirmed) -- the original
    // shadow pin (pending tick-level harness agreement) no longer the bottleneck.
    // 14-day shadow validation required before any live promotion.
    //
    // S45 2026-05-27: DISABLED. Today's 21-trade chop-day session (-$130 net,
    // 13 LOSS_CUTs) prompted re-audit. Built + ran gsp_s63_audit_bt.cpp
    // (existing dormant harness that drives the REAL engine class on the
    // 26mo XAUUSD tape, vs the standalone harness which re-implements logic
    // inline). Result:
    //   Standalone harness (no S63):  $+33,551 PF=2.64 (5074 trades)
    //   Real class S63-OFF:           $-12,380 PF=0.63 (5360 trades)
    //   Real class S63-ON (live cfg): $-152    PF=0.59 (134 trades)
    // The "validated +$33.5k" was a phantom from the re-impl. Real class
    // LOSES on 26mo of historical data. S63-ON only loses less because cuts
    // reduce trade count 40x via cooldowns -- not because S63 adds edge.
    // Engine has no validated edge; shadow-mode "validation" period was
    // meaningless. Per the safety rule above ("Do NOT re-enable without
    // harness-class agreement to within 10% on PnL") this MUST be disabled
    // pending re-tune against the real class. The harness divergence is
    // the bug -- engine.enabled re-enable was based on incomplete evidence.
    // S38a tunables (new fields on engine):
    // S38c: ADX 10 chop filter -- free DD safety. Backtest: 0% PnL cost on
    // 2yr but -24% DD ($443 -> $338). Filters bars where Wilder ADX(14) < 10
    // (no sustained directional pressure). Operator chose adx10 as default
    // after 12-config x 3-period sweep (May, Jan-Apr, 2yr).
    // S38d: Range-expansion filter -- off by default. Enable when scaling
    // beyond 0.01 lot (DD reduction matters more). Recommended 1.2x.
    // Backtest: 1.2x -> -15% PnL but -32% DD + 8% PF on 2yr.

    // ---- FxScalpPyramid REMOVED (S-2026-06-29) -- dead FX config: the
    //   config_fx_scalp lambda was never invoked ((void)-cast) and all FX is
    //   force-disabled ("we have no FX", S-2026-06-23). Deleted with the FX book.

    // ---- GoldRegimeDaily (2026-05-19 S110) ----------------------------------
    // H4 EMA-cross trend-follow engine. FIRST gold engine to clear the full
    // success criterion (PnL > $5K AND PF > 1.20) on the 154M-tick Dukascopy
    // XAUUSD 2025/6 subset:
    //   Best: H4/tr=99/tp=12  PnL $5,854  PF 2.35  WR 92.6%  N=54  DD $2,303
    //   All 6 sweep cfgs profitable; all PF > 1.81; WR uniform 92.6-92.7%.
    //
    // MECHANISM (matches operator's stated trading mechanism):
    //   1. ENTRY: EMA9 crosses EMA21 on H4 bars. Long on bull cross,
    //      short on bear cross. The "signal" IS the EMA crossover event.
    //   2. COST-COVER BE: at MFE >= 5.0pt (10x retail-cost basis), SL
    //      ratchets to entry + 1.0pt buffer (cost-covered breakeven).
    //   3. TIGHT TRAIL: TRAIL_DIST=99 (intentionally large) -- ratchet
    //      never beats BE level, so SL stays at entry+1.0pt after arm.
    //      Effectively: BE-lock + no aggressive trail. Lets H4 trends run.
    //   4. EXIT on signal/trend reverse: EMA9 crosses back against
    //      position direction -> TREND_FLIP_EXIT immediate close.
    //      Hard SL backstop at 2.0xATR(H4). Hard TP at 12xSL. Time-stop
    //      at 100 H4 bars (~16 days).
    //
    // SIZING: RISK_DOLLARS=$1,200 / LOT_MAX=2.50 (24x scaled vs default).
    //   At avg ATR(H4)=20pt and SL=2.0xATR=40pt:
    //     size = $1200 / (40 * $100/pt) = 0.30 lot (clamped to 0.10-0.30 typ).
    //   Per-trade dollar risk: ~$1200. Worst observed DD: $2,303.
    //
    // FREQUENCY: 54 trades / 16 months = 0.17/day. Low by design --
    //   high-frequency signals on gold M5-M15 do NOT carry edge after
    //   retail costs (empirically established across S102-S108: 78 high-
    //   freq configs all negative). H4 regime trades rarely but each
    //   captures a multi-bar trend.
    //
    // SHADOW-OBSERVE PERIOD: minimum 30 days of live shadow on VPS before
    //   any enabled=true switch. Watch for harness-vs-class divergence
    //   (the S100 GSP lesson). Verify Git hash + tick-level reproduction
    //   per CLAUDE.md "Deploy Hygiene" before flipping enabled.
    //
    // shadow_mode=true. enabled=false. Operator-approval required.
    // S112: PYRAMID_ON = true. Pyramid sweep result:
    //   PYR=Y tr=99/tp=12: PnL $12,725 / PF 2.17 / WR 75.9% / DD $8,507
    //   PYR=N tr=99/tp=12: PnL $5,854 / PF 2.35 / WR 92.6% / DD $2,303
    // 2.17x PnL uplift, PF slightly degrades, DD ~3.7x. Net PnL/DD ratio
    // remains favorable (~1.5x). Operator can flip back to false if DD
    // is undesirable; pyramid layers compound size during trend continuation.

    // ?? BBandScalp config (2026-05-18 part B) ????????????????????????????????
    // M1 Bollinger + RSI mean-reversion scalper. Structural-signal entry,
    // BE-lock asymmetric exit. Indicators come from g_bars_gold.m1.ind
    // atomics (BB_P=20, K=2.0 in production OHLCBarEngine) -- no internal
    // bar accumulator, no warmup. Promote to live only after the sweep
    // (backtest/bband_scalp_bt.cpp) confirms positive edge on fresh tape.
    //
    // Default parameters are the "medium" point of the sweep grid -- 35/65
    // RSI thresholds, BE arm at 0.30pt (covers cost), 0.40pt structural SL.
    // If sweep best differs, update these values in the same commit that
    // promotes from shadow.
    //
    // 2026-05-18 part C: DISABLED. The 27-config / 154M-tick sweep showed
    // PF 0.07-0.09 and WR 7-8% across every BB period / stdev / RSI
    // threshold combination -- BB extreme touches on gold M1 are
    // continuation signals, not reversal signals. Engine code retained
    // for reference + future redesign; enabled=false so no shadow trades
    // pollute the ledger. See backtest/bband_scalp_results.txt for
    // evidence. Re-enable only after entry-filter redesign + new sweep.

    // (LatencyEdgeStack startup-flag block removed S13 Finding B 2026-04-24 — engine culled)
    // OLD COMMENT PRESERVED BELOW FOR CONTEXT (can be deleted in a later sweep):
    //   LatencyEdgeStack: was DISABLED (VPS RTT ~68ms, needs <1ms). No positions
    // possible, so shadow_mode wiring is moot. When the stack is re-enabled, add:
    //   g_le_stack.set_shadow_mode(kShadowDefault);  // obsolete — stack culled S13
    // 2026-05-08 USER REQUEST: was kShadowDefault, now hard-pinned to shadow.
    g_gold_stack.set_shadow_mode(true);  // GoldEngineStack / GoldPositionManager via proxy
    // TrendPullbackEngine (4 instances, uniform per Q1 decision):
    // BreakoutEngine non-index instances (FX) -- 2026-05-06 USER INSTRUCTION:
    //   "switch off the fx pairs until we can get them validated".
    //   Pinned to shadow_mode=true regardless of g_cfg.mode. Gold engines
    //   continue trading live. Promote back to kShadowDefault only after a
    //   2-week paper validation showing >=30 trades with WR >=35% net positive
    //   after costs, per the same gate used for the *_london_open / *_asian_open
    //   / *_sydney_open cohort above (lines 56-109).
    //   Trigger trade that prompted this: 2026-05-06 09:19:24 GBPUSD LONG
    //   GbpusdLondonOpenSL net -$45.50 ($28.10 SL + $17.40 commission).
    g_eng_eurusd.shadow_mode  = true;
    g_eng_gbpusd.shadow_mode  = true;
    g_eng_audusd.shadow_mode  = true;
    g_eng_nzdusd.shadow_mode  = true;
    g_eng_usdjpy.shadow_mode  = true;
    // S37 audit (2026-05-27): these five BreakoutEngine FX instances have
    // NO on_tick/on_bar/process dispatch in tick_fx.hpp -- they are
    // signal-dead. Per tick_fx.hpp:3-13 header comment, the FX symbols
    // remained "subscribed for macro context" but live trade firing was
    // replaced by the *_london_open / *_asian_open / *_sydney_open cohort.
    //
    // S37-X fixup (2026-05-28): the original audit commit set `.enabled =
    // false` here, but BreakoutEngine has no `enabled` field (only
    // shadow_mode + the underlying CRTP base, which does not expose enabled
    // either). MSVC caught this on the VPS build; Mac OmegaBacktest target
    // doesn't compile this path so the Mac canary went green. Since these
    // instances already have shadow_mode = true (lines 681-685) AND there
    // is no dispatch site that would ever set pos.active = true, they are
    // already fully inert. Removing the broken-MSVC lines leaves runtime
    // behaviour unchanged. To formally retire them, delete the static
    // omega::BreakoutEngine g_eng_<sym> declarations in globals.hpp:46-51
    // in a follow-up cleanup commit.

    apply_engine_config(g_eng_sp);   // [sp] section: tp=0.60%, sl=0.35%, vol=0.04%, regime-gated
    apply_engine_config(g_eng_nq);   // [nq] section: tp=0.70%, sl=0.40%, vol=0.05%, regime-gated
    apply_engine_config(g_eng_cl);   // [oil] section: tp=1.20%, sl=0.60%, vol=0.08%, inventory-blocked
    apply_engine_config(g_eng_us30); // typed Us30Engine: macro-gated like SP/NQ
    apply_engine_config(g_eng_nas100); // typed Nas100Engine: macro-gated, independent from USTEC.F
    apply_generic_index_config(g_eng_ger30);
    apply_generic_index_config(g_eng_uk100);
    apply_generic_index_config(g_eng_estx50);
    // Bracket engines -- configure() with tuned production params.
    // buffer, lookback, RR, cooldown_ms, MIN_RANGE, CONFIRM_MOVE, confirm_timeout_ms, min_hold_ms
    g_bracket_gold.configure(
        0.8,    // buffer: place orders 0.8pts outside the range
        600,    // FEED-CALIBRATED lookback: 600 ticks = 185s = 3min at 195/min XAUUSD.
                //   300 ticks (90s) was borderline: gold moves 0.5-1.0pt in 90s of compression.
                //   MIN_RANGE=1.0pt -> bracket_high reset to 0 every tick (oscillates around threshold).
                //   600 ticks (3min) shows 1-4pt range in compression -> arms reliably.
                //   MAX_RANGE=12pt still prevents bracketing full trending session moves.
        4.0,    // DATA-CALIBRATED RR: 4.0x SL. Best on 2yr tick data ($38k profit).
                //   TP = 4x the structure range. Median range $2.67 ? TP ~$10.68
        90000,  // cooldown_ms: 90s
        1.5,    // DATA-CALIBRATED MIN_RANGE: $1.5. Allows small but real compressions.
                //   Brute-force showed $1.5-$12 range captures $38k vs $5 min misses most signals.
        0.05,   // CONFIRM_MOVE static fallback
        4000,   // confirm_timeout_ms
        12000,  // min_hold_ms
        0.0,    // VWAP_MIN_DIST: removed.
        30000,  // MIN_STRUCTURE_MS
        25000,  // FAILURE_WINDOW_MS
        20,     // ATR_PERIOD
        0.15,   // ATR_CONFIRM_K
        2.0,    // ATR_RANGE_K
        0.8,    // SLIPPAGE_BUFFER
        1.5     // EDGE_MULTIPLIER
    );
    // Wire shadow fill simulation -- price-triggered in PENDING, not immediate at arm
    // 2026-05-08 USER REQUEST: hard-pin to shadow regardless of cfg.mode.
    g_bracket_gold.shadow_mode = true;

    // ?? MacroCrashEngine config ??????????????????????????????????????????????????
    // Always-on macro event engine. Shadow mode = log only, no live orders.
    // Enable live once shadow logs confirm it fires correctly on real expansion events.
    //
    // S17 DEMOTION (2026-04-24): enabled = false.
    //   Regression disaster 2026-04-15 01:16 UTC: 61 LONG trades in 41s into
    //   a $20 gold drop, -$9,600 on XAUUSD. Post-disaster guards (4hr
    //   DOLLAR_STOP block, 3-SL kill switch) proved insufficient. Engine
    //   structure has no position-open check, no signal cooldown at entry
    //   gate, and no consecutive-SL block -- so a single misaligned drift
    //   sign re-fires the same direction until bankroll hits stop.
    //   Shadow-mode 8-day audit: 108 trades, -$11,742 net. Demoted pending
    //   re-fire guard rework; code preserved (not culled) for autopsy and
    //   future revival. Do not flip back to true without new guards in
    //   place and a fresh 2-year backtest.
    // S44 SPIKE-ONLY RETUNE (2026-04-29 LATE) ----------------------------------
    //
    // Re-enabled after S17 demote (2026-04-24, kept=false until guards added).
    //
    // EVIDENCE (post-Apr-2025 mce_duka_bt_trades.csv, 345 trades):
    //   25 MAX_HOLD winners,  net +$429.70, mean +$17.19/trade, mean MFE 33.10pt
    //  320 SL_HIT losers,     net -$688.66, mean -$ 2.15/trade, mean MFE 16.15pt
    //   ALL 25 winners: UTC hour in {22,23,0,1,2,3,4} (Asia session)
    //   ALL 25 winners: hold_sec >= 7200 (rode through to MAX_HOLD)
    //   Long bias winners: 16/25 (64%); losers: 130/320 (40%)
    //
    // The exceptional trades the user wants preserved are the Asia 2-hour
    // grinds with low MAE and large MFE.  Calibration:
    //
    //   - Lift base ATR/DRIFT/VOL thresholds significantly so London/NY
    //     small-wobble fires are filtered.  These thresholds historically
    //     produce the bleed; the actual macro events that matter (Apr 2
    //     tariff crash, FOMC days) cleared even the higher bar.
    //   - Tighten Asia thresholds upward to match the empirical winner
    //     profile (ATR ~6, drift ~5 at the moment 25 winners triggered).
    //   - Trim Asia SL_ATR_MULT slightly (1.5 -> 1.3) since the higher-
    //     quality entries justify a tighter stop.  Winners' worst MAE was
    //     -11.91pt; even at 1.3x ATR (~9pt) the SL preserves them.
    //   - SpreadRegimeGate v2 (already wired) provides spread-spike rejection
    //     at the entry path, so we don't need a hard "block on wide spread"
    //     constant here.
    //   - COOLDOWN unchanged at 300s -- existing 3-SL kill-switch guard
    //     prevents the 2026-04-15 over-fire scenario; no need to over-
    //     restrict spike captures.
    //
    // Existing safeguards still in force (do NOT remove):
    //   - 4hr DOLLAR_STOP block, 3-SL kill switch (m_consec_sl)
    //   - Weekend gap force-close window
    //   - Asia session-aware override (already in code -- this retune
    //     just lifts the ASIA thresholds inside the override branch)
    //   - shadow_mode=true: no live orders, telemetry only
    //
    // Validation gate: 2-week paper run with new thresholds.  Promote to
    // live only if (a) trade count drops by >=60%, (b) WR rises above 30%,
    // (c) no UTC-hour outside 22:00-04:00 produces a winner.  Else revert.
    // --------------------------------------------------------------------------
    // S57 2026-05-04 (audit-fixes-36): RE-ENABLED per Jo.
    //   The 2026-04-30 disable cited 84 trades / 4.8% WR / -10,849pts. Per
    //   Jo (2026-05-04): those large losses were PHANTOM TRADES (bookkeeping
    //   artifacts -- positions reported as closed at large loss were not
    //   real broker fills, identified at the time as such). The S44 retune
    //   thresholds (ATR>=12, vol>=3.5x, drift>=10) below are the correct
    //   spike-only profile for capturing genuine macro impulses. Engine
    //   stays shadow_mode=true (default) until paper validation shows the
    //   spike-only thresholds produce positive expectancy on real moves.
    //   Re-enable now so the engine fires on the next macro spike and
    //   produces visible shadow ledger entries / PnL.
                                              // S42 revert: original $200 step matches Apr 2 crash-size moves (continued from L139)
    // Wire trade record callback -- fires in BOTH shadow and live.
    // This is what makes MCE trades appear in GUI, ledger, and CSV with correct costs.

    // RSI Reversal Engine startup config
    // RSIReversalEngine -- tuned for high-frequency XAUUSD mean-reversion scalping.
    // Based on documented backtest results (TradingView, QuantifiedStrategies):
    //   RSI 30/70 extremes on tick data = genuine exhaustion, not chop noise.
    //   EMA200 trend filter in tick_gold gates direction (LONG only above, SHORT only below).
    //   Exit at RSI 50 = full mean reversion captured.
    //   2:1 R:R: SL = 0.5x ATR, TP via RSI_EXIT at 50.
    //   Cooldown 15s = high frequency, gold RSI cycles every 30-60s at extremes.
    //   MAX_HOLD 90s = scalp timeframe, don't overstay.
    // Win rate target: 55-70%, profit factor >1.4 (per backtested research).
    // TOMBSTONE 2026-05-01 (S52 trade-quality follow-up):
    //   Real-tick backtest 4320 trades / 2yr -> -$3,800 (negative EV).
    //   See `// Real-tick backtest:` comment ~20 lines below for source.
    //   Disabling rather than deleting -- file remains for reference.
    // 2026-05-14 (part W): S63 VWR-pattern in-flight protection — explicit
    //   re-affirm of the class default (XAU-scaled, LOSS_CUT only flavor) for
    //   grep visibility. Mirrors g_vwap_rev_ger40 precedent at
    //   engine_init.hpp:649 and g_ustec_tf_5m at engine_init.hpp:976: the
    //   override matches the class default but documents intent and makes
    //   activation discoverable from engine_init.hpp alone. See
    //   outputs/S63_STATE_CLASSIFICATION_2026-05-14.md §3.4 for the audit
    //   that confirmed STATE A via class-default route (field declared at
    //   RSIReversalEngine.hpp:98; mgmt-path at L571-582 — header comment
    //   L87-94 explicitly notes 'LOSS_CUT only' — no BE_RATCHET on this
    //   engine's design).
    //
    //   IMPORTANT: this engine is currently DISABLED (enabled=false below
    //   per 2026-05-01 negative-EV finding). The S63 re-affirm here is for
    //   grep-visibility only — if the engine is ever re-enabled, S63
    //   protection will already be wired correctly without further config.
    //
    //   LOSS_CUT_PCT = 0.05 -> XAU@3700: ~$1.85 adverse cut.
    fflush(stdout);

    //  Real-tick backtest: 4320 trades / 2yr, -$3.8k. Momentum = negative EV.

    // [BUG-5 NOTE] MCE is shadow_mode=true by design -- it logs [MCE-SHADOW] but sends
    // no FIX orders. Entry/exit logic is fully functional via on_close callback wired above.
    // To enable live MCE trades: set g_macro_crash.shadow_mode = false (requires authorisation).
    fflush(stdout);
    fflush(stdout);
    // PENDING_TIMEOUT_SEC: gold compresses for minutes before breaking -- 60s was expiring before the move
    g_bracket_gold.PENDING_TIMEOUT_SEC = 600;  // 10 min: gold compression can last well beyond 5 min
    // MAX_HOLD_SEC (S20 2026-04-25): absolute cap on filled-position hold.
    // Driver: 2026-04-24 session 15:45:17 LONG held 234 minutes for -$18.74.
    // 3600s (60min) chosen as conservative first cap -- analytics review after
    // 5 trading days will tune. 0 = disabled.
    g_bracket_gold.MAX_HOLD_SEC = 3600;
    // ?? Post-fill confirmation gate (S21 2026-04-25) ??????????????????????????
    // Driver: 2026-04-24 session fakeouts. Price pierced a bracket level by
    // 0.5-1pt, filled the stop order, then reversed before ever moving 3pt
    // in our favour. Existing BREAKOUT_FAIL needs mid-crossing (half range back)
    // — CONFIRM triggers earlier at a fixed pt threshold.
    //
    // Backtest (logs/shadow/omega_shadow.csv, 86 XAUUSD_BRACKET trades over ~2wks):
    //   CONFIRM_PTS=3.0, CONFIRM_SECS=30 aborts 31 trades at ~$0.28 cost each
    //   instead of letting them run to full SL. Saved $267.11. Zero winning
    //   trades harmed (winners=0 in the abort bucket).
    //
    // Mechanics per BracketEngine.hpp: on fill, enter CONFIRM phase. If MFE
    // reaches 3.0pt inside 30s -> promote to LIVE, normal management. If 30s
    // elapses without reaching 3.0pt -> abort at market (BREAKOUT_FAIL_CONFIRM).
    // Hard-SL still fires inside the window (safety).
    //
    // 0.0 / 0 disables the gate. Set per-symbol -- other bracket engines keep
    // defaults (gate inert) unless explicitly enabled.
    g_bracket_gold.CONFIRM_PTS  = 3.0;
    g_bracket_gold.CONFIRM_SECS = 30;
    // MIN_BREAK_TICKS: sweep guard -- price must stay inside the bracket for N consecutive
    // ticks before orders are sent. Catches London open liquidity sweeps (07:00:34 SHORT
    // -$7.97): bracket range $7.80 was exactly one sweep wide, SHORT filled in 1 tick
    // then price snapped back $7.80 to SL in 16s. 3 ticks ~= 0.3-0.6s -- long enough to
    // distinguish a single-tick spike from genuine compression holding at the boundary.
    g_bracket_gold.MIN_BREAK_TICKS = 5;  // raised 3->5: extra sweep protection
    // ATR-based dynamic minimum range: eff_min_range = max(recent_noise * ATR_RANGE_K, MIN_RANGE)
    // Prevents brackets arming when the market noise floor exceeds the bracket width --
    // the SL then sits inside normal noise and gets swept without a real move.
    //
    // ATR_PERIOD=20 ticks: at London open ~5-15 ticks/sec = 1-4s of recent price action.
    // This captures the current noise floor, not historical vol. Updates every tick.
    //
    // Gold: London noise $8-20. MIN_RANGE=6. ATR_RANGE_K=1.5 ? when noise=$12, eff_min=18.
    //   A $10 bracket in $12 noise is invalid -- SL would be swept immediately.
    //   A $20 bracket in $12 noise is valid -- genuine compression above noise.
    // FX (EURUSD etc.): noise ~0.0003-0.0008. ATR_RANGE_K=1.8 (tighter price, more sensitive).
    // Gold bracket: ATR_RANGE_K=0 disables ATR floor -- use MIN_RANGE=1.0pt directly.
    // With VIX at 24 gold compresses to 1-3pt ranges which ATR_RANGE_K=1.5 rejects (needs 15pt).
    // The bracket window itself defines the range -- 1pt minimum just filters single-tick noise.
    g_bracket_gold.ATR_PERIOD  = 20;  g_bracket_gold.ATR_RANGE_K  = 0.0;
    g_bracket_gold.MIN_RANGE   = 2.5;  // raised 1.0->2.5: filter small noisy compressions
    g_bracket_eurusd.ATR_PERIOD = 20; g_bracket_eurusd.ATR_RANGE_K = 1.8;
    g_bracket_gbpusd.ATR_PERIOD = 20; g_bracket_gbpusd.ATR_RANGE_K = 1.8;
    g_bracket_audusd.ATR_PERIOD = 20; g_bracket_audusd.ATR_RANGE_K = 1.8;
    g_bracket_nzdusd.ATR_PERIOD = 20; g_bracket_nzdusd.ATR_RANGE_K = 1.8;
    g_bracket_usdjpy.ATR_PERIOD = 20; g_bracket_usdjpy.ATR_RANGE_K = 1.8;
    // Indices: leave ATR disabled -- noise floor more stable, fixed MIN_RANGE sufficient
    // MAX_RANGE: prevents bracketing full trending session moves instead of real compression
    // Gold at $4400: 0.4% = $17.6 max range. Tight compression is $8-16. Day range is $40-120.
    // 2026-06-12 re-opt: DONE the "derive band as % of price" fix the old comment kept
    //   asking for. MAX_RANGE_PCT auto-tracks price (gold $2400->$4700->$4213 forced
    //   two manual abs bumps; this ends that). 0.40% = the index-symbol bracketing
    //   proportion. 2yr A/B (bracket_gold_2yr_audit, PCT_MODE): PF 0.700 vs abs-19's
    //   0.705 -- statistically identical, so the switch is safe + future-proof. The
    //   abs values stay as a fallback for the (never-hit) px_ref==0 case.
    g_bracket_gold.MAX_RANGE_PCT   = 0.40;   // %-of-price; overrides MAX_RANGE below
    g_bracket_gold.MAX_RANGE        = 19.0;  // fallback only (used iff px_ref==0)
    // ?? S22c 2026-04-25: empirical SL-dist gate (gold only) ?????????????????????
    // 62 live XAUUSD_BRACKET trades 2026-04-13..24: every trade with bracket
    // dist > 6pt was a loser (0 wins / 39 losses / -$388 combined). Trades at
    // dist <= 6pt won 6/26 = 23% WR at +$73 net. The 6pt band is where the
    // bracket represents genuine compression vs late-breakout chasing.
    // ?? S37-Z 2026-05-29: raised 6.0 -> 12.0. Diagnosis from 2026-05-28 log:
    // EVERY London arm (07:00-12:00 UTC, 29 events) hit BLOCKED sl_dist_too_wide
    // with dist=6.3..8.6pt vs cap=6.00. Cause: gold price moved $2400 -> $4400
    // since the empirical band was calibrated. The 6pt cap was 0.25% of
    // price-at-calibration; same proportion at $4400 = $11. Round to 12
    // matching MAX_RANGE. Re-audit when 60+ new live trades accumulate to
    // confirm the % equivalent holds at the new price level.
    g_bracket_gold.MAX_SL_DIST_PCT = 0.40;  // 2026-06-12: %-of-price, auto-tracks (see MAX_RANGE_PCT). overrides the abs below
    g_bracket_gold.MAX_SL_DIST_PTS = 19.0;  // fallback only (used iff px_ref==0)
    // ?? Regime-flip exit (Session 13, 2026-04-23) -- gold only ?????????????????
    // Thresholds wired for XAUUSD_BRACKET only. See BracketEngine.hpp config block.
    // Trigger: |ewm_drift| >= 2.5 against position for 5 consecutive ticks.
    //   - 2.5 is well above noise floor (typical quiet drift is +/- 0.5-1.0)
    //   - 5 ticks at ~5-10 ticks/s = 0.5-1.0s of persistent counter-drift
    // Rationale: 2026-04-23 Session 12 losses showed drift flipping 30+ seconds
    // before bracket's SL hit. 5 ticks confirm catches confirmed reversals while
    // ignoring momentary bid-ask flicker. drift arg passed from tick_gold.hpp
    // via g_gold_stack.ewm_drift().
    // All other bracket engines keep defaults (0.0 / 0) -- exit branch inert.
    g_bracket_gold.REGIME_FLIP_MIN_DRIFT     = 2.5;
    g_bracket_gold.REGIME_FLIP_CONFIRM_TICKS = 5;
    // Configure opening range engines
    // New ORB instruments: LSE and Euronext with tighter 15-min range windows
    // VWAPReversionEngine params -- per-instrument tuning
    // Indices: raised 0.20->0.40% -- USTEC at $24000: 0.20%=$48 fires on noise.
    // 0.40%=$96 requires a genuine VWAP dislocation not a 2-tick wiggle.
    // MAX_EXTENSION raised 0.80->1.20%: prevents blocking real dislocations.
    // MAX_HOLD raised 900->600s: exit stalled trades faster (was holding 15min losers).
    // S95 2026-05-15: disabled. SPX/US500 UltimateBacktest v1 (4118 trades, PF=0.92)
    //   and v2 (117 trades, PF=1.13 overall but OOS PF=0.88 — fails >=1.20 criterion).
    //   SPX lacks persistent trend-following momentum edge. ATR 8-15 band (120 trades,
    //   PF=1.19) was the only positive segment — too thin for production.
    // 2026-05-13 (S37-H-followup): in-flight cut + BE ratchet (index defaults).
    //   LOSS_CUT_PCT=0.08 -> US500@7400: ~5.9pt cold-loss cut.
    //   BE_ARM_PCT  =0.05 -> US500@7400: ~3.7pt mfe arms the ratchet.
    //   BE_BUFFER_PCT=0.02 -> US500@7400: ~1.5pt buffer (~typical spread).
    // 2026-05-13 (part K): REVERTED to baseline (all zero). VWAPReversionBacktest
    //   P1 sweep (16 cells of BE_ARM x BE_BUFFER on 830 days of HistData ticks)
    //   showed baseline gross +3.7085 dominates every threshold combination
    //   tested. Best wider cell (a=0.10, b=0.06) was -0.33. US500's baseline
    //   p95 worst loss (-0.13) is already tight, so in-flight cuts amputate
    //   winners without meaningful tail protection. USTEC and GER40 keep the
    //   cuts because their fatter baseline tails justify the mechanism. See
    //   outputs/SESSION_HANDOFF_2026-05-13f.md.
    // 2026-05-13 (S37-H-followup): USTEC@28000: ~22pt LOSS_CUT, ~14pt ARM, ~5.6pt buffer.
    // 2026-05-13 (part L): REVERTED to baseline (all zero). The part-L smoke
    //   test against VWAPReversionBacktest (NSXUSD 4943 trades) showed the
    //   same winner-amputation pattern part-K caught on US500/EURUSD:
    //     TP_HIT      129 -> 58    (-55%, winners cut at BE ratchet)
    //     gross_pnl   -7.28 -> -8.05  (cuts make a losing strategy worse)
    //     worst_trade -6.10 -> -8.40  (tail also worsens, not just gross)
    //     p95_loss    -0.56 -> -0.21  (only p95 improves; abs worst worsens)
    //   BE_ARM=0.05% on USTEC@28000 = ~14pt; the typical TP is 0.40% (~112pt)
    //   so the ratchet arms when the trade is only 12% of the way to target
    //   and any 5.6pt noise retrace triggers BE_CUT. Same shape as US500.
    //   USTEC.F baseline itself is marginally net-negative (-0.00147/trade)
    //   -- a separate parameter retune session is warranted, but revert
    //   stops the active bleed first.
    // 2026-05-14 (part P / S69): parameter retune EXECUTED and FAILED. Phase 1
    //   univariate sweep (21 cells on the 4.4GB 404-day NSXUSD tape) found
    //   one cell above the +0.001/trade decision threshold: ext=0.80 at
    //   +0.00282. Phase 2 robustness check (6-cell ext fine sweep around
    //   0.80) showed 2/6 positive cells with the positives sandwiched
    //   between negatives -- classic single-cell-artifact pattern, not a
    //   stable parameter surface. Phase 2B 2D grid did not improve over
    //   Phase 2A's best. TP rates were 1-3% across the entire sweep; the
    //   strategy is not closing trades at profit targets, it's avoiding
    //   bigger losses via timeout/MAE_EXIT -- risk shaping, not edge.
    //   Conclusion: parameter tuning cannot rescue USTEC.F VWR. The engine
    //   stays disabled (g_vwap_rev_nq.enabled = false, L608) until a future
    //   signal-side rework session (VIX/L2 confluence, session filters etc)
    //   produces positive baseline expectancy. See outputs/VWR_USTEC_PHASE1_RESULTS_2026-05-14e.md
    //   and outputs/VWR_USTEC_PHASE2_RESULTS_2026-05-14e.md for full evidence.
    //   Retune plan at outputs/VWR_USTEC_RETUNE_PLAN_2026-05-14a.md is
    //   closed -- parameter surface explored, no edge found.
    // 2026-05-13 (S37-H-followup): GER40 index defaults same as US500/USTEC.
    // EURUSD: 0.12% extension threshold (FX moves more precisely, smaller range)
    // S18 explicit tune (was: MAX_EXTENSION_PCT and MAX_HOLD_SEC fell back to
    // class defaults 0.80 / 900s). Class defaults were calibrated for indices
    // pre-tune; for an FX pair the threshold-to-max ratio of indices (avg
    // ~3.25x: SP=3.43, NQ=3.00, GER40=3.33) maps to EURUSD as 0.12 * ~3.3
    // ≈ 0.40. At EURUSD ~1.10 that's ~44 pips, which is the upper end of a
    // typical daily range and a reasonable cap for "VWAP dislocation beyond
    // which mean reversion is unreliable". MAX_HOLD aligned to 600s like the
    // indices for consistency -- "exit stalled trades faster" rationale at
    // L446 applies equally to FX. Re-tune from fresh shadow tape once
    // VWAPReversion has been firing live-shadow for 2-4 weeks.
    // 2026-05-13 (S37-H-followup): FX moves smaller than indices -- tighter cut.
    //   LOSS_CUT_PCT=0.05 -> EURUSD@1.10: ~5.5pip cold-loss cut.
    //   BE_ARM_PCT  =0.03 -> EURUSD@1.10: ~3.3pip mfe arms ratchet.
    //   BE_BUFFER_PCT=0.015 -> EURUSD@1.10: ~1.6pip buffer (~typical spread).
    // 2026-05-13 (part K): REVERTED to baseline (all zero). The part-K
    //   VWAPReversionBacktest precision fix (%.4f -> %.6f) revealed a hidden
    //   91% regression -- baseline gross +0.000854 vs tuned +0.000078 (part-J
    //   numbers had truncated both to 0.0009 vs 0.0001, masking the gap). P1b
    //   sweep (12 cells of BE_ARM x BE_BUFFER on 404 days of HistData ticks)
    //   found no cell beats baseline -- best (a=0.06, b=0.02) reached
    //   +0.000761, still 11% short. EURUSD profile matches US500 (tight p95
    //   -0.000010), so same revert call applies. See part-K handoff.
    // ?? NBM London session engines (07:00-13:30 UTC) ????????????????????????????
    // Covers the gap before NY open. Gold and oil are liquid from London open.
    // Uses same ATR/band logic as NY engines but anchored to London open price.


    // TrendPullbackEngine params -- per-instrument tuning
    //
    // GOLD (M15 bar EMAs -- seeded from g_bars_gold.m15):
    //   EMAs come from OHLCBarEngine on 50 M15 bars:
    //     EMA9  half-life = 3.1 bars = 47 min
    //     EMA21 half-life = 7.3 bars = 109 min
    //     EMA50 half-life = 17.3 bars = 260 min = 4.3 hours
    //
    //   PULLBACK_BAND_PCT: the critical setting.
    //   Old value (0.08%) = ?3.7pts at $4700. With M15 EMA50 representing
    //   4.3h weighted average, price is typically 10-30pts from EMA50 during
    //   an active trend. 0.08% = never fires. Must be wide enough to detect
    //   genuine pullbacks TO the M15 EMA50 level.
    //   0.50% = ?23.5pts at $4700. Fires when price is within ~24pts of
    //   the M15 EMA50 -- correct for pullback-to-slow-average strategy.
    //   Upper bound: 1.0% = ?47pts -- too loose, fires mid-trend constantly.
    //
    //   COOLDOWN_SEC: M15 trade = swing trade, minimum 1 M15 bar (15min)
    //   between signals. 900s (15min) = 1 full M15 bar -- prevents rapid
    //   re-entry on the same pullback level.
    //
    //   TRAIL_ARM/DIST: ATR from M15 bars = 4-8pts typical. 2x arm = 8-16pts
    //   before trailing. 1x dist = 4-8pt trail. Correct for swing scale.
    //   Leave at class defaults (2.0x arm, 1.0x dist, 1.0x BE).
    //
    //   BE_ATR_MULT: lock BE at 1x M15 ATR (~5pts). Unchanged -- good.
    // Improvement 1: vol regime sizing
    // Improvement 2: daily loss cap -- stop gold TrendPB after $150 loss in a day
    // Improvement 4: time-of-day weighting
    // Improvement 5: CVD gate
    // Improvement 7: news SL widening
    // Widen pullback band: 0.15% -> 0.50% (?23pts at 4620)
    // Default 0.15% = ?6.9pts. On a $20 trending move price is 20pts from EMA50
    // and never enters the band -- engine silent on all clean trends.
    // 0.50% allows entry when price is trending away from EMA50 but still directional.
    // Improvement 8: pyramid on second pullback
    // Trail/BE params: class defaults are correct for M15 ATR scale (4-8pts)
    // TRAIL_ARM_ATR_MULT=2.0, TRAIL_DIST_ATR_MULT=1.0, BE_ATR_MULT=1.0 -- no change needed

    // DISABLED: TrendPullback gold has no edge on XAUUSD.
    // S44 v6 backtest verdict (2024-03 -> 2026-03, 148M ticks, 3933 TPB trades):
    //   - Net/T = -$0.84 baseline, -$0.70 best-gate (G3: atr>=3.0 AND spread<=0.85)
    //   - 0 of 24 net-positive hours under G3. No whitelist exists.
    //   - 2025 net/T = -$0.69, 2026 net/T = -$0.71. No 2026-edge.
    //   - Tightest cohort (mae<1pt, 99.6% WR, gross +$0.037/T) still net -$0.78/T.
    //     => spread cost > strategy alpha across every cohort, gate, session, year.
    // Original disable rationale (counter-trend Asia firing, H4 gate cold-start) was
    // a real bug but not the root cause; v6 ran with H4 gate cold-start fixed and
    // still finds no edge. Tombstone: do not re-enable without fundamentally new logic.
    // See: bt_trades.csv (S44 v6, HEAD aa6624b0 on s44-bt-validation).

    // HTF swing engines v2 -- per-instrument params, partial TP, weekend close gate.
    // shadow_mode=true always. To go live: validate shadow signals then set false.
    {
        g_h1_swing_gold.p           = omega::make_h1_gold_params();
        g_h1_swing_gold.symbol      = "XAUUSD";
        g_h1_swing_gold.shadow_mode = true;
        // 2026-05-08 USER REQUEST: was true, only microscalper trades on gold.
        g_h1_swing_gold.enabled     = false;
        printf("[INIT] H1SwingEngine  XAUUSD: shadow=true adx_min=%.0f sl=%.1fx"
               " tp1=%.1fx trail_arm=%.1fx trail_dist=%.1fx daily_cap=$%.0f\n",
               g_h1_swing_gold.p.adx_min,    g_h1_swing_gold.p.sl_mult,
               g_h1_swing_gold.p.tp1_mult,   g_h1_swing_gold.p.tp2_trail_arm_mult,
               g_h1_swing_gold.p.tp2_trail_dist_mult, g_h1_swing_gold.p.daily_cap);

        // MinimalH4Breakout -- pure Donchian, no filters. Runs PARALLEL to H4Regime.
        // OOS-validated config: D=10 SL=1.5x TP=4.0x. See header for evidence.

        // C1RetunedPortfolio -- Phase 2 winner from CHOSEN.md, ported from
        // Python sim. Donchian H1 long retuned (period=20, sl=3.0 ATR,
        // tp=5.0 ATR) + Bollinger H2/H4/H6 long, max_concurrent=4, 0.5% risk.
        // Long-only, XAUUSD only, shadow_mode=true. Self-contained engine
        // (does not interfere with other engines or borrow their state).
        // S-2026-06-11 TOMBSTONED (operator order). Backtest provenance is real
        // (CHOSEN.md, WF pass) but the corpus was 2024-03..2026-04 = gold BULL
        // only. Engine is a LONG-ONLY dip-buyer (BBand lower-band touch H2/H4/H6)
        // with no trend/regime gate — in the current confirmed downtrend it
        // catches falling knives (lifetime live record: n=2, 0 wins, −118.14,
        // both bollinger_H4 SLs). Same BBand-long-XAU family the 2026-06-01
        // mean-rev audit culled elsewhere. Do NOT re-enable without a bear-tape
        // backtest + trend gate.
        // S-2026-07-08c: that bear-tape backtest + trend gate WAS RUN
        // (backtest/c1_retuned_gated_bt.cpp, REAL class, 2022-2026 XAU H1 certified,
        // gate = live gold_regime price core + macro-hostile overlay): FAILS every
        // variant — naked PF0.59 −$811, price-gated PF0.57, full-gated PF0.54; the
        // −7.5% DD halt trips inside the 2022 bear in all three. Dip-buys fire on the
        // way INTO bear classification, exactly the GoldOversoldBounce
        // "gate-incompatible" pattern. Tombstone question CLOSED: permanently dead —
        // no gate variant left to try on spot data.
        // S99f 2026-05-18: prime_from_shared_h1_bars call REVERTED.
        // I mistakenly added the method to CellEngine.hpp (omega::cell::CellPortfolio)
        // but g_c1_retuned is type omega::C1RetunedPortfolio (separate class in
        // C1RetunedPortfolio.hpp). The wrong class lookup caused VPS build error
        // C2039 on this line. C1Retuned priming gap remains as a known issue;
        // proper fix requires adding the prime method to C1RetunedPortfolio.hpp
        // directly (uses C1DonchianH1LongCell + C1BollingerLongCell + C1BarSynth,
        // different architecture than CellPortfolio). Queued for follow-up.
        // Impact: shadow_mode=true engine, no real PnL, runs cold until first live
        // H1 bar closes (same state as every prior session before S99e).
        fflush(stdout);

        // ?? TsmomPortfolio -- Tier-1 ship 2026-04-30 ?????????????????????????
        // 5 long tsmom cells (H1/H2/H4/H6/D1). Verdict source:
        //   phase1/signal_discovery/POST_CUT_FULL_REPORT.md
        //   phase1/signal_discovery/post_cut_revalidate_all.py
        // Post-cut sim_c (1 unit, 365 days, costs included):
        //   H1 long: 3,484 trades, 53.2% WR, +$17,482, pf 1.39
        //   H2 long: 1,826 trades, 55.1% WR, +$12,952, pf 1.35
        //   H4 long:   933 trades, 61.4% WR, +$15,885, pf 1.66
        //   H6 long:   661 trades, 57.8% WR, +$13,380, pf 1.65
        //   D1 long:   216 trades, 56.5% WR,  +$9,109, pf 1.65
        // 82% of master_summary 27-cell survivor set total simulated edge.
        // Self-contained header (TsmomEngine.hpp). Long-only Tier-1 (Tier-3
        // adds the 5 tsmom shorts once shadow ledger validates the longs).
        // shadow_mode = kShadowDefault: flips to LIVE atomically with g_cfg.mode.
        // block_on_risk_off=true: skip new entries when g_macroDetector is
        // RISK_OFF (existing positions still managed). Existing safeguards
        // (margin_call, max_lot_cap, cooldown=hold_bars=12 anti-stacking) are
        // load-bearing; do NOT remove without a fresh post-cut backtest.
        //
        // warmup_csv_path -> CSV of 6,156 post-cut H1 bars (2025-04-01 ->
        // 2026-04-01) generated by phase1/signal_discovery/export_tsmom_warmup.py.
        // Fed through synthesisers/ATRs/closes_ deques BEFORE live ticks
        // arrive so every cell (H1/H2/H4/H6/D1) is READY when the first
        // live H1 bar fires. Empty path -> cold start fallback.
        // 2026-05-04 (post-handoff risk-budget fix): max_lot_cap 0.05 -> 0.02.
        //   Live shadow tape had a Tsmom_H1_long position lose $23.65 on a
        //   4.45pt adverse move (~5x bracket-cohort exposure). Capping at
        //   0.02 brings the same move to $8.90 (~2x bracket cohort) while
        //   preserving Sharpe (risk_pct unchanged at 0.005). g_tsmom_v2
        //   below inherits this cap via `= g_tsmom.max_lot_cap`.
        //   See omega_config.ini [tsmom] section for the parity comment.
        fflush(stdout);

        // ?? TsmomPortfolioV2 -- Phase 2a CellEngine-refactor live shadow ??????
        //
        // Runs alongside g_tsmom over the same H1 bar stream and same XAUUSD
        // ticks. Trades go into logs/shadow/tsmom_v2.csv via
        // omega::cell::shadow::tsmom_writer -- they DO NOT touch g_omegaLedger.
        // This is non-negotiable: routing V2 trades into the master ledger
        // would double-count every position in daily PnL / drawdown / engine-
        // cull / param-gate / fast-loss-streak state.
        //
        // shadow_mode = true ALWAYS (refactor-validation engine, not a
        // trader). Independent of g_cfg.mode -- this engine never goes LIVE
        // until Phase 2b validation completes and the cutover replaces
        // g_tsmom outright (see docs/CELL_ENGINE_REFACTOR_PLAN.md §4 Phase 4).
        //
        // max_positions_per_cell = 1 (Phase 2a refactor-correctness). Backtest
        // parity over the 1-year tsmom_warmup_H1 corpus already passed; live
        // shadow exists to confirm the same parity under real bar gaps,
        // weekend spreads, and FORCE_CLOSE-under-reconnect timing. Flip to
        // max=10 (Phase 2b) only after the agreed shadow-window holds.
        //
        // All other config matches g_tsmom 1:1 so the V1 vs V2 comparison
        // isolates the refactor only.
        omega::cell::shadow::tsmom_writer().open("logs/shadow/tsmom_v2.csv");
        fflush(stdout);

        // ?? DonchianPortfolio -- Tier-2 ship 2026-04-30 ???????????????????????
        // 7 donchian cells: H2 long, H4 long+short, H6 long+short, D1 long+short.
        // (H1 long is NOT here -- it's the retuned cell in C1RetunedPortfolio.)
        // Verdict: phase1/signal_discovery/POST_CUT_FULL_REPORT.md
        // Combined: 328 trades/yr, +$5,620 = 47% of unshipped post-cut edge.
        // Bidirectional: would have profited during 2026-03-18 BEAR cluster.
        // Reuses tsmom warmup CSV (same H1 stream input).
        fflush(stdout);

        // ── XauTrendFollow4hEngine (S33d 2026-05-11) ──────────────────────────
        // 3-cell trend-follow ensemble for XAU on 4h bars. Built from
        // edge_hunt.cpp results showing convergent positive PnL across
        // Donchian N=20, InsideBar, and ER0.20 cells in all 3 Dukascopy
        // years + L2 sample. Shadow-only by default; 0.01 lot per cell;
        // max 3 concurrent positions. Drives off the s_cur_h4 bar already
        // aggregated in tick_gold.hpp.
        // 2026-05-15 (S96): 154M-tick fresh-tape backtest v2 (pruned losers):
        //   Pruned 3 cells: InsideBar(1) PF=0.98, ER20(2) PF=0.95, ADX_Mom(4) PF=0.85
        //   Kept: Donch20(0) PF=1.44, Keltner(3) PF=1.16, RangeExpand(5) PF=1.24
        //   v2 ALL: IS PF=1.31, OOS PF=1.31 (zero decay), 502 OOS trades.
        //   cell_enable_mask = bits 0,3 = 0x09 (Donchian PF=1.15 + Keltner PF=1.03)
        //   S-NEXT 2026-05-17: RangeExpand (bit 5) removed — PF=0.82 on fresh tape,
        //   negative all 3 WF folds. Was 0x29, now 0x09.
        g_xau_tf_4h.shadow_mode = false;  // S-2026-07-01: LIVE on IBKR 4002 paper (FIX-cutover shadow reason moot on IBKR)
        g_xau_tf_4h.enabled     = true;
        // S-2026-06-17: in-flight cold-loss protection (was 0.0/OFF by default).
        // Faithful per-TF backtest (backtest/losscut_xau_faithful.py, 2yr XAU
        // M30->H4): LC=1.5% holds net (+20) while cutting maxDD -38% (-449->-278)
        // and worst trade -163->-83. TF-scaled (H4 wider than H1/H2). Shadow ->
        // live ledger is the final gate. Protects the big adverse swing without
        // killing the trend ride; see [[omega-runner-profit-protect-regime]].
        g_xau_tf_4h.LOSS_CUT_PCT = 1.5;
        // S116 2026-05-19: bit 6 (0x40) added for EmaCross8_21 cell from the
        // S114 long-trend ensemble research (Python +$30,966 / Sharpe +1.96
        // / 25mo; C++ +$32,025 / 95 trades).
        // S117 2026-05-19: activated.  Service-level mode=SHADOW provides
        // synthetic-fills safety net; engine.shadow_mode=false keeps the
        // ledger emission identical to other live cells so we can validate
        // EmaCross_8_21 fill rate against the S115 backtest CSV.  Expect
        // ~4 trades/month per the C++ harness (95 trades / 25mo).
        g_xau_tf_4h.cell_enable_mask = 0xC9;  // S41: + KeltnerEMA50 (bit7); Donchian + Keltner20 + EmaCross_8_21 + KeltnerEMA50
        g_xau_tf_4h.lot         = 0.01;
        g_xau_tf_4h.max_spread  = 1.0;
        // S88-followup post-sweep 2026-05-27: 4h per-cell breakdown shows
        // the engine-wide vol_band over-filtered Donch20 (baseline PF 1.44
        // already best; vol_band kept PF flat but cut net 50%). Keltner
        // is the only enabled cell that benefits (+PF, -57% DD). EmaCross
        // not yet tested -- mask unset there. Engine cell ids:
        //   0 = Donch20 (NO gate -- baseline wins)
        //   3 = Keltner (VolBand wins: PF 1.16 -> 1.25, DD -57%)
        //   6 = EmaCross_8_21 (untested)
        // Mask 0x8 = bit 3 only (Keltner). RangeExpand bit 5 also lifts
        // with VolBand if operator ever enables it; bit pre-set in mask
        // for forward compat -> 0x28.
        g_xau_tf_4h.use_vol_band_gate = true;
        g_xau_tf_4h.vol_band_low_pct  = 0.30;
        g_xau_tf_4h.vol_band_high_pct = 0.85;
        // CONSERVATIVE mask: only Keltner (bit 3) -- proven on harness.
        // RangeExpand (bit 5) would also benefit but isn't in production
        // cell_enable_mask (0x49). EmaCross_8_21 (bit 6) is in production
        // but untested in this harness; leaves it ungated (safe).
        g_xau_tf_4h.cell_vol_band_mask = 0x8;
        // For D1: all 3 cells benefit from vol_band -- mask stays 0xFFFFFFFF
        // (default all enabled). Bits 0,1,2 = Momentum20, Keltner, ADX_Mom.
        // S-2026-07-02 RETUNE 1.0->0.5. The prior 1.0 (S-2026-06-29) was set on a
        // faithful 4h2h BT run BEFORE the S-2026-07-02 direction-aware thrust fix --
        // i.e. with the long-biased bug that vetoed every SHORT, so the "1.0 sweet
        // spot bull PF1.25->1.48" was a bull-only artifact (shorts weren't firing).
        // Re-ran the FAITHFUL real-engine harness (XauTrendFollow4h2hBacktest, prod
        // cfg mask=0xC9 + ADX15 + vol-band, CORRECT IBKR cost) WITH the fix across
        // IMP {0,0.5,1.0}: IMP=1.0 is WORST both files (2yr net +4346/PF1.64,
        // 2022-23 +186/PF1.07). IMP=0.5 wins the robust WF-passing 2yr sample
        // (net +5523/PF1.74/maxDD 832 vs IMP0 +5322/1.70/951 -- higher PF+net,
        // LOWER DD, both-WF-halves+ both regimes). On bear-heavy 2022-23 IMP0 edges
        // 0.5 by ~$130 but that sample fails WF both-halves either way. 0.5 also
        // matches the 1h + D1 fleet (same faithful basis). Direction-aware code kept.
        //
        // S-2026-07-10 RETUNE 0.5 -> 0.3 (faithful 4h2h BT, prod cfg mask 0xC9 +
        // ADX15 + vol-band, correct IBKR cost, IMP-plateau sweep 0.1..1.0):
        //   FINDING 1 -- impulse is NOT a cadence lever. n_trades is FLAT at 313 on
        //   the bull file across IMP 0.1..0.5 (blocks 0/62/165 at 0.0/0.25/0.5) --
        //   a blocked breakout bar simply re-enters the same cell a bar later, so the
        //   live "[XTF4H-BLOCK] 133x" is per-cell/-bar log spam, NOT throttling. The
        //   low gold-trend cadence is validated/structural (D1 EMA200 gate + setup
        //   scarcity), not the impulse gate. Loosening buys ZERO extra trades.
        //   FINDING 2 -- 0.5 sits just PAST a broad robust bull plateau (IMP 0.2..0.35
        //   PF 1.77..1.82); 0.3 is the plateau centre. 0.3 vs 0.5 (2yr bull fresh):
        //   PF 1.82 vs 1.74, net +5960 vs +5523 (+8%), maxDD 816 vs 832, both WF
        //   halves STRONGER (H1 1.88 vs 1.62 / H2 1.80 vs 1.79), cadence unchanged
        //   (n=313). Spread-robust @0.40 (net +5954, PF 1.82 -- unchanged). 2022-23
        //   BEAR neutral (PF 1.13 net ~+$420 either way; WF both-halves FAIL at EVERY
        //   IMP -- the documented bull-positive/bear-flat shadow profile, not caused
        //   by this change). So 0.3 = free entry-QUALITY gain, bear-neutral, robust
        //   shoulder (0.25/0.3/0.35 flat top, not an in-sample spike). NOT a cadence
        //   change. Siblings (1h/2h/D1) left at 0.5 -- own harnesses, out of scope.
        g_xau_tf_4h.min_impulse_atr = 0.3;
        // S-2026-06-30 ADX CHOP-GATE: block ALL cell entries when Wilder ADX14 < 15
        // (= ranging). Kills the EMA-cross/breakout whipsaw-into-a-range losers (the
        // operator-flagged 4k-chop trade). Faithful 4h2h BT, production cfg
        // (mask 0xC9 + vol-band + IMP=1.0), CORRECT IBKR cost: bull net FLAT
        // (+4041->+4021), PF 1.69->1.79, maxDD -38% ($1806->$1119), both-WF-halves+
        // stronger; bear (2022-23) net flat, no new bleed. 15 = robust plateau
        // SHOULDER (16 is a 1-point in-sample PEAK; 17 cliffs to PF 1.70) -> 15 keeps
        // margin vs forward ADX drift. Spread-robust @0.40. Harness xau_tf_4h2h_bt ADX env.
        g_xau_tf_4h.min_adx_entry = 15.0;
        g_xau_tf_4h.warmup_csv_path = "phase1/signal_discovery/warmup_XAUUSD_H4.csv";
        g_xau_tf_4h.init();
        omega::warmup_or_die(g_xau_tf_4h, "XauTrendFollow4h");
        // S-2026-07-02 KILL-THE-4H-WAIT (operator): the bundled warm-seed CSV ends
        // ~60d stale, so after a restart bars_ held only stale-price bars and the
        // engine could not evaluate a new entry until the NEXT live H4 close (up to
        // 4h idle). Append the fresh live H4 dump (gold_d1_trend_h4.csv, written on
        // every H4 close) so indicators/price are current, and arm a guarded
        // fire-on-boot (evaluated on the first live tick in tick_gold.hpp). Reuses
        // the H4 dump gold_d1_trend already maintains ("taken care of by our logs").
        g_xau_tf_4h.append_fresh_h4(log_root_dir() + "/gold_d1_trend_h4.csv");
        printf("[OMEGA-INIT] XauTrendFollow4hEngine initialised: shadow=%d enabled=%d lot=%.2f cells=7 mask=0x%X"
               " (Donchian,InsideBar_RR4to1,ER0.20_RR4to1,Keltner,ADX_Mom,RangeExpand,EmaCross8_21_S116)\n",
               (int)g_xau_tf_4h.shadow_mode, (int)g_xau_tf_4h.enabled, g_xau_tf_4h.lot,
               (unsigned)g_xau_tf_4h.cell_enable_mask);
        fflush(stdout);

        // ── XauTrendFollow1hEngine (S118 2026-05-19) ──────────────────────────
        // H1 long-only ensemble, 2 cells:
        //   bit 0: EmaCross_20_80 (long-only EMA(20,80) + slow-rising filter,
        //                          ATR(14)x4 stop, tp_mult=20 ~= no-TP)
        //   bit 1: Donchian_N40   (long-only break of 40-bar prior high,
        //                          exit on close < 40-bar prior low,
        //                          ATR(14)x5 stop)
        // S114/S115 evidence (combined two cells, ~25mo):
        //   A (EmaCross): +$26,024 / Sharpe +1.97 / 173 trades
        //   C (Donchian): +$27,969 / Sharpe +2.11 / 70 trades
        // Both shadow-emit through ledger via the bracket_on_close path so
        // the new cells are auditable against the S115 backtest CSV from
        // day one.  Service-level mode=SHADOW (config.ini) is the outer
        // safety net while we validate fill rates.
        g_xau_tf_1h.shadow_mode = false;  // S-2026-07-01: LIVE on IBKR 4002 paper (FIX-cutover shadow reason moot on IBKR)
        g_xau_tf_1h.enabled     = true;
        // S-2026-06-17 cold-loss protection. Faithful M30->H1 backtest: LC=0.5%
        // -> net +337, maxDD -44% (-602->-339), worst -191->-27. Tighter than H4
        // (faster TF). See backtest/losscut_xau_faithful.py.
        g_xau_tf_1h.LOSS_CUT_PCT = 0.5;
        g_xau_tf_1h.cell_enable_mask = 0x0F;  // S40: all 4 ensemble cells (EmaCross+Donchian+Pullback+Keltner)
        g_xau_tf_1h.lot         = 0.01;
        g_xau_tf_1h.max_spread  = 1.0;
        g_xau_tf_1h.min_impulse_atr = 0.5;    // S-2026-06-24q RETUNE 1.0->0.5. The 1.0 came from an MGC-FUTURES bar-replay (memory omega-impulse-filter-entry-quality "PF1.96->2.45"); FAITHFUL re-confirm on the REAL XauTrendFollow1h engine WITH the live er_gate_min=0.40 (XauTrendFollow1hBacktest.cpp, XAU H1 bull 2yr + 2022-bear, cross-spread + SL-first) found ER-gate + impulse REDUNDANT and IMP=1.0 OVER-filters: bull PF 2.20->1.91 / total -18% / maxDD +30%, bear PF 1.19->1.13 / total -24%. IMP=0.5 ~= IMP=0 on bull (PF 2.18, DD same) + marginally BETTER on bear (PF 1.19->1.21). Net of 1.0->0.5: bull total +22% / DD -23%, bear total +40%. Basis FAITHFUL.
        // S-2026-06-21 ER trend/chop gate (shadow A/B vs prior ungated record). Harness
        // er_gate_trend_bt: gold H1 trend PF 1.05->1.13, edge density 2.4x, maxDD down at
        // ER~0.40 in trend tape (skip-only filter, never adds trades). Bear bleed handled
        // by existing macro/vol gates, not this. breakout cells only (Pullback exempt).
        //
        // S-2026-06-22 INVESTIGATE STATUS = UNRESOLVED (offline). The fleet-audit asked to
        // drop EITHER the ER gate OR the impulse (they are redundant trend/chop filters).
        // An offline real-class A/B (backtest/XauTrendFollow1hBacktest.cpp) CANNOT pick:
        // the harness drives the engine WITHOUT the live stack (use_vol_target / pyramidK2 /
        // LOSS_CUT) AND without the live gates that define regime behaviour -- gold_wt()
        // WaveTrend (L589), reg_slope_size (L184), D1-EMA200/macro -- which all fail-open
        // standalone. Baseline sweep showed bear ALL-POSITIVE (PF 1.20-1.36) while the LIVE
        // shadow record is bear-NEG (0.66-0.87): different engine, conclusion does not
        // transfer; bear per-trade edge (~$2-3/t, n~210) is within noise. Resolution must
        // come from a LIVE-SHADOW A/B on the VPS ledger (the faithful instrument for a
        // deployed engine), or regime-scope each filter once that data exists. Until then
        // KEEP BOTH (live status quo). Do NOT drop either off an offline run.
        g_xau_tf_1h.er_gate_min = 0.40;
        g_xau_tf_1h.er_gate_n   = 20;
        // ── S39 vol-target + pyramiding on the Donchian40 cell (OFF by default).
        // Validated edge (gold_regime_edges.cpp, 2yr WF + 6-block + 3x-cost):
        // vol-target N40 Donchian PF~3 robust; pyramid K2 lifts avg-win ~3x at
        // higher DD/lower PF. To enable in SHADOW, uncomment + watch the ledger:
        // S42 2026-05-30: ACTIVATED in shadow (validated edge; watch ledger fill
        // rates before any live promotion). vol-target sizes risk across vol
        // regimes; pyramid K2 lifts avg-win ~3x at higher DD (engine-driven BT
        // XauTrendFollow1hBacktest: K2 +$26357 exp $70.85 PF2.86).
        g_xau_tf_1h.use_vol_target  = true;   // size = unit/ATR (clamp 0.01-0.08)
        g_xau_tf_1h.vol_target_unit = 0.10;
        g_xau_tf_1h.pyramid_max_adds = 2;     // start K2; K3/step0.75 = best exp (harness)
        g_xau_tf_1h.pyramid_step_atr = 1.0;
        g_xau_tf_1h.pyramid_sl_atr   = 3.0;
        g_xau_tf_1h.warmup_csv_path = "phase1/signal_discovery/warmup_XAUUSD_H1.csv";
        g_xau_tf_1h.init();
        omega::warmup_or_die(g_xau_tf_1h, "XauTrendFollow1h");
        printf("[OMEGA-INIT] XauTrendFollow1hEngine initialised: shadow=%d enabled=%d lot=%.2f cells=4 mask=0x%X"
               " (EmaCross_20_80_S118, Donchian_N40_S118, Pullback_EMA20_S40, Keltner_EMA50_S40; vol-target+pyramidK2 ON)\n",
               (int)g_xau_tf_1h.shadow_mode, (int)g_xau_tf_1h.enabled, g_xau_tf_1h.lot,
               (unsigned)g_xau_tf_1h.cell_enable_mask);
        fflush(stdout);

        // ── XauTrendFollow M15 Donchian-40 (S-2026-06-02) ────────────────────
        // Same engine type as g_xau_tf_1h, but fed M15 bars in tick_gold.hpp so
        // it runs the Donchian-40 breakout on M15. Motivation: the IBKR gold
        // cost cut (0.60->0.37 RT pts) makes higher-frequency gold trend viable
        // -- M15 Donchian-40 was marginal at BlackBull cost, clearly profitable
        // at IBKR cost. OOS-validated (last 30%, gold_cost_unlock_sweep.cpp,
        // IBKR cost): +$13.6k PF2.18 Sharpe3.11 win45%, daily-MTM corr 0.70 to
        // the live H1/H4 book (partial diversification, not a duplicate).
        // Donchian40 cell ONLY (mask=0x02). A chop-suppression gate was tested
        // (ADX/ER/cooldown/breakout-buffer) and REJECTED: all subtractive or
        // overfit noise on the gold trend book -- the Donchian breakout IS the
        // chop filter. So no extra gate here. Fixed lot, vol-target + pyramid
        // OFF (clean entry/exit edge validation first; pyramiding fails
        // cross-regime per the S45 tombstone). HARD shadow until ledger gate.
        // S-2026-06-17 cold-loss protection. EXTRAPOLATED tighter than H1's 0.5%
        // (m15 = faster TF, smaller bars) -- NOT yet backtested at m15; shadow
        // ledger validates. Conservative first value; retune on its own M30->m15 test.
        fflush(stdout);

        // ── UstecTrendFollow5mEngine (S33d 2026-05-11) ───────────────────────
        // Donchian N=20 at 5m bars on USTEC. Convergent edge across 4
        // unrelated signal families on the 15-day L2 sample (n=111,
        // WR=45%, BE cost $10.1, 170x margin over $0.06).
        // CAVEAT: only 2 months of data. KEEP shadow until 6+ months
        // L2 capture confirm the finding.
        // 2026-05-14 (part L): S63 VWR-pattern in-flight protection — explicit
        //   re-affirm of the class defaults (USTEC-scaled) for grep visibility.
        //   Mirrors g_vwap_rev_ger40 precedent at engine_init.hpp:632-634
        //   where the override matches the class default but documents intent
        //   and makes the activation discoverable from engine_init.hpp alone.
        //   See UstecTrendFollow5mEngine.hpp §S63 for per-cell semantics.
        //
        //   LOSS_CUT_PCT  = 0.08 -> USTEC@28000: ~22pt cold-loss cut.
        //   BE_ARM_PCT    = 0.05 -> USTEC@28000: ~14pt mfe arms ratchet.
        //   BE_BUFFER_PCT = 0.02 -> USTEC@28000: ~5.6pt buffer (~typical spread).
        //
        //   Promotion gate (RESOLVED 2026-05-14 -- gate fails):
        //     - Phase 1 (S72 P1, UTF5M_PHASE1_RESULTS_2026-05-14.md):
        //       S63 + S37 widened SL/TP is empirically adverse on USTEC.
        //       Baseline (S63 zeroed) gross=+929; every S63-active cell
        //       net negative. Phase 2 skipped (signal decisive).
        //     - Phase 3 (S73, UTF5M_PHASE3_RESULTS_2026-05-14.md):
        //       4-window WF on baseline fails PF gate (1.1154 vs 1.20).
        //       3 of 4 windows pass; w1 (2024-H2) anomalous.
        //     Engine remains disabled. Re-enable requires either a Tier 4
        //     redesign (vol-regime gate is the leading candidate) OR a
        //     deliberate operator decision to soften the decision rule
        //     (NOT recommended -- see Phase 3 memo §7 item 4).
        fflush(stdout);

        // ── S37-P2 RiskMonitor wiring for g_ustec_tf_5m ──────────────────────
        // 2026-05-12 (part C): mirror the g_gold_microscalper pattern at
        // lines 162-181 above. Two parts:
        //
        //   (a) fire-side hook: every successful _fire_entry inside the
        //       engine calls g_risk_monitor.on_fire("UstecTrendFollow5m",
        //       now_s). The umbrella engine name (no cell suffix) is used
        //       because the threshold model treats the Donchian+Keltner
        //       ensemble as one signal source for the fire-rate evaluator.
        //
        //   (b) auto-pin callback: flips g_ustec_tf_5m.shadow_mode = true
        //       on any tripped condition. Idempotent: only flips the
        //       first time. Engine is ALREADY in hard shadow at startup
        //       (line 880 above), so the auto-pin is currently belt-and-
        //       braces for the post-promotion path. When operator
        //       authorises step 5 (flip shadow_mode = false), this
        //       callback becomes the real circuit-breaker.
        //
        // NOTE on calibration: the on_fire path inside RiskMonitor early-
        // returns if "UstecTrendFollow5m" has no row in
        // data/risk_monitor_thresholds.csv. The wiring below registers the
        // infrastructure; producing the calibrated threshold row requires
        // adding {engine="UstecTrendFollow5m", symbol="USTEC.F", ...} to
        // ENGINE_TABLE in backtest/calibrate_risk_thresholds.cpp and
        // re-running. That is a separate task; until it lands the fire-
        // rate evaluator is a no-op but the auto-pin callback is
        // available via g_risk_monitor.trip_engine_to_shadow() and other
        // external trip paths.
        // S37-P3: three auto-pin callbacks. Each flips the same engine
        //   instance to shadow on any trip. The umbrella name handles
        //   fire-rate trips; the per-cell names handle close-side WR /
        //   spread trips because tr.engine carries the cell suffix
        //   (UstecTrendFollow5mEngine.hpp S34 BUG #3). All three target
        //   the same g_ustec_tf_5m.shadow_mode -- on a trip from any of
        //   the three evaluators, the engine pins regardless of which
        //   cell or which check tripped.
        auto pin_ustec_tf_5m = [](const std::string& reason) {
        };
        g_risk_monitor.register_shadow_pin_cb("UstecTrendFollow5m",          pin_ustec_tf_5m);
        g_risk_monitor.register_shadow_pin_cb("UstecTrendFollow5m_Donchian", pin_ustec_tf_5m);
        g_risk_monitor.register_shadow_pin_cb("UstecTrendFollow5m_Keltner",  pin_ustec_tf_5m);
        printf("[OMEGA-INIT] UstecTrendFollow5m RiskMonitor wiring installed"
               " (fire-hook + 3 auto-pin callbacks: umbrella + Donchian + Keltner cells)\n");
        fflush(stdout);

        // ── XauTrendFollowD1Engine (S33e 2026-05-11) ──────────────────────────
        // 3-cell daily trend-follow ensemble (Momentum lb=20, Keltner K=2.0,
        // ADX_Mom adx>25). D1 bars synthesised internally from the same
        // s_cur_h4 stream the 4h engine uses. Shadow-default. 0.01 lot/cell.
        // Lower cadence than 4h ensemble (~2 trades/month) but biggest
        // per-trade edges in the project ($36-60). 2/3 Duka years +ve per cell.
        // 2026-05-15 (S96): 154M-tick fresh-tape backtest (all 3 D1 cells profitable):
        //   Momentum20 PF=1.38, Keltner PF=1.51, ADX_Mom PF=1.45.
        //   D1 aggregate: 79 trades, PF=1.43. All cells enabled.
        // 2026-05-27 S52: DISABLED -- real-class audit (xau_trendfollow_audit)
        // confirmed Sharpe +2.40 over 95 trades BUT mdd_to_gross=95%. Equity
        // goes deeply underwater before recovery -- high capitulation risk.
        //
        // S88-followup 2026-05-27: REVIVED to HARD-SHADOW with vol-band gate.
        // The S52 DD profile (mdd_to_gross 95%) was on UNGATED entries; full
        // Duka 2yr backtest with vol_band on:
        //   Baseline (ungated): n=79 PF=1.43 net=$2583 MaxDD=$1470
        //   +vol_band:           n=50 PF=2.31 net=$3220 MaxDD=$ 908 (-38% DD)
        // Per-cell PF lift: Mom20 1.38->2.32 (+68%), Keltner 1.51->2.46 (+63%),
        // ADX_Mom 1.45->2.18 (+50%). MaxDD cut nearly in half.
        // The DD profile that drove the S52 disable may improve with the
        // gate; 60+ days HARD shadow before considering enabled=live.
        g_xau_tf_d1.shadow_mode = false;  // S-2026-07-01: LIVE on IBKR 4002 paper (operator all-engines cutover)
        g_xau_tf_d1.enabled     = true;   // S88: revived w/ vol-band gate, HARD shadow
        g_xau_tf_d1.min_impulse_atr = 0.5;  // UPGRADE S-2026-06-22 (fleet-audit, real engine reproduced in main tree, backtest/XauTrendFollowD1Backtest.cpp IMP=0.5): signal-day |close-open|>=0.5*ATR14 gate. Ensemble bull PF1.60, bear PF1.52, both-WF-halves+ both regimes, maxDD -16%. Per-cell + IMP=1.0-rejected detail in manifest XauTfD1 row + fleet-audit log.
        // S-2026-06-17 cold-loss protection. Daily backtest (losscut_batch_b.py):
        // LC=1.0% -> net flat, maxDD -68% (-341->-110), worst -174->-53.
        g_xau_tf_d1.LOSS_CUT_PCT = 1.0;
        // S-2026-06-19 giveback protection (Phase-3 sweep, the planned S63 D1
        // evaluation — see XauTrendFollowD1Engine.hpp BE_ARM comment block).
        // BE_ARM_PCT=3 / BE_BUFFER_PCT=0: arms a break-even ratchet once mfe
        // reaches 3% of entry, cuts only on a FULL round-trip back to entry
        // (buf=0) -> protects the giveback case WITHOUT truncating live runners
        // (the designated Donchian_N5 no-TP runner cell is left untouched in BT).
        // Wide-arm is mandatory: a tight 1% arm GUTS the trend edge (PF<1, 2-3x
        // trades) -- confirms the swing-protection-sweep prior.
        // FAITHFUL BT (xau_tf_d1_bearm_sweep, engine-driven, bull+bear, 3x-cost):
        //   net-positive BOTH regimes + BOTH halves + 3x-cost-robust + broad
        //   cell attribution + plateau arm3-5, on the reproducible (raw-cell and
        //   full-config) baselines. CAVEAT: the production vol-band gate is NOT
        //   standalone-reproducible (needs live rolling-percentile state), so the
        //   production-faithful number comes from the SHADOW LEDGER -- arbiter.
        //   Complementary to g_rider_d1 (BE-arm fires on round-trips; the rider
        //   has already banked its +N*ATR legs by then). small-n: D1 ~2 trades/mo.
        // S-2026-06-25 RE-SWEEP (xau_tf_d1_bearm_sweep, real engine, 2yr H4, bull+bear, WF):
        //   arm5/buf1.0 BEAT arm3/buf0 on BOTH net AND maxDD -- PF1.58 +$4160 maxDD$1553
        //   (vs arm3: PF1.53 +$3792 maxDD$1872), both-WF-halves+ (1.49/1.62). Within the
        //   documented plateau arm3-5. arm1/2 still GUTS it (PF<1) -- early-arm tight-lock
        //   refuted again. Buffer 1.0 = bank a small profit above entry, not a full round-trip.
        g_xau_tf_d1.BE_ARM_PCT    = 5.0;
        g_xau_tf_d1.BE_BUFFER_PCT = 1.0;
        // S88-followup post-sweep 2026-05-27: widen D1 band [0.30,0.85] ->
        // [0.20,0.90]. Sweep showed D1 entry-vol distribution sits inside the
        // band already; widening picks up 2 extra cell-Keltner trades (PF
        // 3.27 standalone) and 1 Momentum20 trade. Modest lift: $3220 -> $3369
        // over 2yr (+$149). Marginal but free; tighter bands gave identical
        // result so wider is dominated upside.
        g_xau_tf_d1.use_vol_band_gate = true;
        g_xau_tf_d1.vol_band_low_pct  = 0.20;
        g_xau_tf_d1.vol_band_high_pct = 0.90;
        // (min_impulse_atr=0.5 set above at the enabled= line — single source of truth.)
        g_xau_tf_d1.lot         = 0.01;
        g_xau_tf_d1.max_spread  = 1.0;
        g_xau_tf_d1.warmup_csv_path = "phase1/signal_discovery/warmup_XAUUSD_H4.csv";
        g_xau_tf_d1.init();
        omega::warmup_or_die(g_xau_tf_d1, "XauTrendFollowD1");
        printf("[OMEGA-INIT] XauTrendFollowD1Engine initialised: shadow=%d enabled=%d lot=%.2f cells=3"
               " (Momentum,Keltner,ADX_Mom)\n",
               (int)g_xau_tf_d1.shadow_mode, (int)g_xau_tf_d1.enabled, g_xau_tf_d1.lot);
        fflush(stdout);

        // S-2026-06-19: TrendRider bank-and-reload companions on the 4h + D1 hosts.
        // SHADOW. Banks +N*ATR per host cell + reloads while the cell stays open
        // (validated D1+4h both-halves both regimes; 2h marginal/1h hurts -> not wired).
        g_rider_4h.enabled = true; g_rider_4h.shadow_mode = false;  // S-2026-07-01: LIVE on IBKR 4002 paper (operator all-engines cutover)
        g_rider_4h.N = 2.5; g_rider_4h.lot = 0.01; g_rider_4h.tag = "XauTrendRider4h";
        g_rider_4h.init(omega::kXauTfNumCells);
        // S-2026-07-12 DISABLED (operator cull order). GOLD_PHASE1B: RiderD1 standalone
        // -$593 at true cost (best-effort). Rider4H (+$3,127) stays live — this is D1 only.
        g_rider_d1.enabled = false; g_rider_d1.shadow_mode = false;
        g_rider_d1.N = 2.5; g_rider_d1.lot = 0.01; g_rider_d1.tag = "XauTrendRiderD1";
        g_rider_d1.init(omega::kXauTfD1NumCells);
        printf("[OMEGA-INIT] TrendRider companions: 4h(cells=%d) + D1(cells=%d) N=2.5 shadow=1\n",
               omega::kXauTfNumCells, omega::kXauTfD1NumCells);
        fflush(stdout);

        // ── Gold WaveTrend momentum-confirm gate (S-2026-06-03) ──────────────
        // Gold-validated X1 filter now gates XauTrendFollow 1h/4h/D1 entries:
        // skip a trend entry unless a confirming WaveTrend momentum tag fired in
        // the last `lookback` M1 bars. Edge: confirmed-trend winners 71.9% vs
        // losers 51.5% (+12pp, ~2 SE). Fed gold M1 closes in tick_gold.hpp;
        // fails open during ~55-bar warmup. ACTIVE in shadow for live validation.
        omega::gold_wt().enabled      = true;
        omega::gold_wt().gate_enabled = true;
        omega::gold_wt().lookback     = 10;
        printf("[OMEGA-INIT] GoldWaveTrend momentum gate: enabled=%d gate=%d lookback=%d "
               "warmup=%d -> gates XauTrendFollow 1h/4h/D1\n",
               (int)omega::gold_wt().enabled, (int)omega::gold_wt().gate_enabled,
               omega::gold_wt().lookback, omega::gold_wt().warmup_bars);
        fflush(stdout);

        // ── XauTsmomFastD1Engine (2026-05-20) -- short-lookback momentum sister
        //   Backtest 2yr daily XAU (long_only, cost 1bps):
        //     IS Sh=6.69 / OOS Sh=7.65 / FUL Sh=7.57. n=48. PnL=78.1%.
        //   Cost stress holds: Sh 6.69 at 20bps. Distinct cell from D1 ensemble
        //   (lb=5 vs lb=20, sl=1.0 vs 2.0, tp=5.0 vs 4.0, hold=20).
        // 2026-05-27 S57: DISABLED -- regime split MID Sharpe -0.86 (negative
        // in normal-vol regime, 24/59 trades). Operator policy: must pass ALL
        // rigour tests; one regime negative = fail.
        fflush(stdout);

        // ── XauTurtleD1Engine (2026-05-20) -- 40d Donchian break (long-only)
        //   Resurrection of S50 X1 retired TurtleTick. Re-tested 2yr daily XAU:
        //     FUL Sh=13.01 at 10bps (IS=7.32 OOS=18.42), n=20, WR=70%.
        //     Cost-robust to 50bps (FUL Sh=10.51).
        //   CAVEAT: sparse (~10 trades/year). High Sharpe variance from low n.
        // CULL S-2026-06-17 (retest campaign): xau_d1_zoo_audit real-class
        // Sharpe=+0.33 (n=28) vs this comment's claimed FUL Sh=13.01 = ~40x
        // inline inflation (BACKTEST_TRUTH disease). Positive-but-sub-0.5 =
        // marginal noise, not edge. Disabled. Reversible: shadow, lot 0.01.
        fflush(stdout);

        // ── Shared price-based regime brain (RegimeState / gold_regime()) ─────
        //   2026-06-12: warm-seed the gold bull/bear classifier so it is queryable
        //   on the first live tick (else ~13 days cold-warm). Fed per-tick in
        //   tick_gold.hpp; queried by long-only gold engines to skip longs in a
        //   sustained bear. Validated: gold_regime_gate_bt.cpp (XAU H1 2020-23) --
        //   H1-sustained-bear gate beat D1-slope (which HURT) and D1-sustained-bear.
        // 2026-06-23 stale-seed fix: restore the LIVE-accurate regime state across restarts
        // first (saved every 60s by quote_loop). Only fall back to the warm-seed CSV when no
        // fresh persisted state exists (true cold start). The CSV alone was 83 days stale
        // (April-1, gold 4692 vs live 4120) and reset every restart -> regime stuck NEUTRAL ->
        // long-only gold engines bought into the downtrend unprotected.
        {
            const std::string gr_dump = log_root_dir() + "/gold_regime_h1.csv";
            omega::gold_regime().set_live_dump(gr_dump);   // record live H1 from now on
            if (!omega::gold_regime().load_state(log_root_dir() + "/gold_regime_state.dat")) {
                // No fresh .dat (cold start / >12h down). Prefer the self-recorded live H1 once it's
                // warm-capable (>=300 bars ~13d); else fall back to the static tsmom CSV (stale-prone).
                size_t n = omega::gold_regime().seed_from_h1_csv(gr_dump);
                if (n < 300) { omega::gold_regime().reset();
                               omega::gold_regime().seed_from_h1_csv("phase1/signal_discovery/tsmom_warmup_H1.csv"); }
            }
        }
        printf("[OMEGA-INIT] gold_regime() brain: regime=%s warm=%d (long-only gold engines gate on long_blocked())\n",
               omega::gold_regime().regime_name(), (int)omega::gold_regime().warm());
        fflush(stdout);

        // ── AUPOS/AUNEG gold BE-floor companion (S-2026-07-06; RETIRED S-2026-07-07e) ──
        //   REAL-FILL VERDICT (backtest/index_befloor_intrabar_bt.cpp + bull-gate variant, certified
        //   histdata ticks 2022-23 + m5-synth 2024-26): the "neg=0, WF both halves" research
        //   (gold_befloor_ls.py) is the same max(0,.) clamp as the index/FX books (registry §5).
        //   Real fills at the live 1.0%/be6: 2022-23 -$68.5k, 2024-26 +$68k but H1 half AND Pos
        //   flavor negative. NO cell in thr 0.3-3.0% x be 6/10/20 x exec A/buf10/buf25 x
        //   {ungated, sustained-bull-gated} is positive in BOTH eras with both halves/flavors + —
        //   each era's best config loses the other era. The bull-gate rule (feedback-companion-
        //   bull-gate-not-reject) was TESTED, not skipped: 1.0/6/buf25+BG flips 2022-23 to +$85k
        //   but loses -$61.5k on 2024-26 (H1 -$79k). Nothing to reconfigure to -> RETIRED.
        //   Evidence outputs/BEFLOOR_FAMILY_REALFILL_2026-07-07.txt. Ledger history rows stand.
        //   enabled=false + one state write keeps /api/gold_companion serving the honest real
        //   forward history with no new arms; the jump-rider H1 feed (separate honest book) stays.
        {
            auto& gc = omega::gold_befloor_companion();
            gc.enabled = false;            // on_h1_bar() short-circuits; no seeds, no exec wiring
            gc.clear_open_arms();          // drop persisted shadow arm-state (no frozen "open" legs)
            gc.recompute_and_write();      // publish once: persisted REAL forward history, no open arms
            // (JumpRider XAUUSD h1 sink REMOVED — engine culled S-2026-07-10)
        }
        printf("[OMEGA-INIT][SEED] gold BE-floor companion (AUPOS/AUNEG) RETIRED S-2026-07-07e (real-fill: no config survives both eras; bull-gate tested, fails 2024-26 WF) -- state serves real history, no new arms\n");
        fflush(stdout);

        // ── GoldTrendMimicLadder: INDEPENDENT SHADOW mimic ladders off gold trend engines
        //    (S-2026-07-09, operator). A trend engine opens -> one-way on_trend_open -> this
        //    engine spawns its OWN legs at that entry, managed on the XAU H1 close (deploy-
        //    forward: armed AFTER seeding, at the end of init_engines). Validated standalone
        //    (backtest/clip_path_*.cpp real-engine entries, independent window exit, cost-debited):
        //    XauTf4h 4-leg +110/leg, MgcFastDon 2-leg +34/leg, XauTfD1 2-leg +24/leg -- all WF
        //    both halves +, both regimes +. SHADOW (send_live_order no-op until flip), judged
        //    STANDALONE (feedback-companion-independent-engine).
        {
            auto& gm = omega::gold_trend_mimic();
            {   omega::GoldTrendMimicBook::Config c; c.trigger_tag="XauTf4h"; c.live_sym="XAUUSD";
                c.legs={{"T1",0.08},{"T2",0.10},{"W1",0.20},{"W2",0.25}};
                c.arm_pct=0.25; c.lc_pct=1.5; c.cap_bars=12; c.rt_cost_bp=15.0; c.be_entry_pct=0.15; c.pend_bars=6; gm.add(std::move(c)); }
            {   omega::GoldTrendMimicBook::Config c; c.trigger_tag="MgcFastDon"; c.live_sym="XAUUSD";
                c.legs={{"T",0.08},{"W",0.20}};
                c.arm_pct=0.15; c.lc_pct=1.0; c.cap_bars=24; c.rt_cost_bp=15.0; c.be_entry_pct=0.10; c.pend_bars=12; gm.add(std::move(c)); }
            {   omega::GoldTrendMimicBook::Config c; c.trigger_tag="XauTfD1"; c.live_sym="XAUUSD";
                c.legs={{"T",0.08},{"W",0.20}};
                c.arm_pct=0.25; c.lc_pct=2.0; c.cap_bars=8; c.rt_cost_bp=15.0; c.be_entry_pct=0.15; c.pend_bars=4; gm.add(std::move(c)); }
            // XauTf2h (S-2026-07-10, mimic-extension sweep): 2 legs T gb8 + W gb30, fed on the
            // native 2h close. Validated STANDALONE over the FULL grid incl REAL 2022 bear
            // (backtest/clip_path_xau_tf.cpp 2h + mimic_ladder_overlay.cpp): TIGHT +76.9% PF1.39
            // (bull+51.6/bear+25.2), WIDE +88.5% PF1.44 (bull+55.2/bear+33.3), both WF halves + ,
            // both regimes + ; 8 tight / 12 wide passers across the sweep (robust plateau). The
            // strongest NEW mimic candidate of the sweep (2h is a diverse addition to 4h/D1).
            {   omega::GoldTrendMimicBook::Config c; c.trigger_tag="XauTf2h"; c.live_sym="XAUUSD";
                c.legs={{"T",0.08},{"W",0.30}};
                c.arm_pct=0.25; c.lc_pct=1.0; c.cap_bars=24; c.rt_cost_bp=15.0; c.be_entry_pct=0.15; c.pend_bars=6; gm.add(std::move(c)); }
            // Index D1 turtle mimics (S-2026-07-09b, operator "all symbols"): 2 legs each (tight+wide),
            // fed on the D1 bar (turtle cadence). Validated STANDALONE (clip_path_idx_turtle real
            // entries, independent D1 window exit): NAS100 T+80.9/W+79.5 80%win; US500 +45.8/+47.2
            // 75%win; DJ30 +62.3/+57.9 81%win -- all WF both halves +, both regimes +.
            for (const char* isym : { "NAS100", "US500", "DJ30" }) {
                omega::GoldTrendMimicBook::Config c;
                c.trigger_tag = std::string(isym) + "Turtle"; c.live_sym = isym;
                c.legs = {{"T",0.08},{"W",0.20}};
                c.arm_pct = 0.5; c.lc_pct = 3.0; c.cap_bars = 10; c.rt_cost_bp = 4.0; c.be_entry_pct = 0.10; c.pend_bars = 5;
                gm.add(std::move(c));
            }
            gm.set_exec(
                [](const std::string& sym, bool is_long, double lots, double px)->std::string { return send_live_order(sym, is_long, lots, px); },
                [](const std::string& sym, bool orig_is_long, double lots, double px, const std::string& token){ send_live_order(sym, !orig_is_long, lots, px, token); },
                [](const std::string& sym, double tp_dist_pts, double lots)->bool { return ExecutionCostGuard::is_viable(sym.c_str(), 0.30, tp_dist_pts, lots, 2.0); },
                [](const std::string& engine, const std::string& sym, bool is_long, double entry_px, double exit_px, double lots, int64_t entry_ts, int64_t exit_ts, const char* reason){
                    omega::TradeRecord tr; tr.engine=engine; tr.symbol=sym; tr.side=is_long?"LONG":"SHORT";
                    tr.entryPrice=entry_px; tr.exitPrice=exit_px; tr.size=lots; tr.entryTs=entry_ts; tr.exitTs=exit_ts;
                    tr.exitReason=reason; tr.pnl=(is_long?(exit_px-entry_px):(entry_px-exit_px))*lots; handle_closed_trade(tr); });
            printf("[OMEGA-INIT][SEED] GoldTrendMimicLadder wired: 7 trigger books (XauTf4h 4-leg, XauTf2h 2-leg, MgcFastDon 2-leg, XauTfD1 2-leg, NAS100/US500/DJ30 Turtle 2-leg), specific native feeds, SHADOW, deploy-forward\n");
            fflush(stdout);
        }

        // ── XAGPos/XAGNeg SILVER BE-floor companion (S-2026-07-06; RETIRED S-2026-07-07e) ──
        //   REAL-FILL VERDICT (backtest/index_befloor_intrabar_bt.cpp, XAGUSD H1-OHLC synth
        //   2024-01..2026-07 incl the silver squeeze 22->119->62): the "neg=0, both WF halves +"
        //   calibration was the max(0,.) clamp (registry §5). Real fills: EVERY grid cell
        //   (thr 0.3-1.5% x be 6/10/20 x exec A/buf10/buf25) NEGATIVE, both halves negative
        //   everywhere — best cell 1.5/6/A -$486k real vs +$5.8M model per 1-lot book. The
        //   squeeze half is WORSE, so no bull-gate salvage exists. Nothing to reconfigure to ->
        //   RETIRED. Evidence outputs/BEFLOOR_FAMILY_REALFILL_2026-07-07.txt. Ledger rows stand.
        //   Silver DIRECTIONAL remains a graveyard (SilverTurtle/GoldSilverLeadLag DEAD) — and the
        //   befloor JUMP mechanism is now real-fill-dead on XAG too; do not resurrect either.
        //   The on_tick.hpp XAG H1 aggregator still calls on_h1_bar(); enabled=false short-circuits.
        {
            auto& xc = omega::xag_befloor_companion();
            xc.enabled = false;            // feed short-circuits; no seeds, no exec wiring
            xc.clear_open_arms();          // drop persisted shadow arm-state (no frozen "open" legs)
            xc.recompute_and_write();      // publish once: real forward history, no new arms
        }
        printf("[OMEGA-INIT][SEED] XAG BE-floor companion (XAGPos/XAGNeg) RETIRED S-2026-07-07e (real-fill: every cell negative incl squeeze era) -- state serves real history, no new arms\n");
        fflush(stdout);

        // ── USOILPos/USOILNeg WTI CRUDE BE-floor companion (S-2026-07-06) ─────
        //   RETIRED S-2026-07-07e. REAL-FILL VERDICT (backtest/index_befloor_intrabar_bt.cpp):
        //   the "neg=0, both WF halves strongly +" calibration was the max(0,.) clamp (registry
        //   §5). Real fills: USOIL 2026-only H1 = sea of red at every thr/be/exec; the single
        //   positive cell (0.70/20/buf25 +$41k) does NOT replicate on 16mo of certified Brent
        //   (BCOUSD) real ticks 2025-01..2026-04 — same cell -$138k, ALL Brent cells negative.
        //   Nothing to reconfigure to -> RETIRED. Evidence outputs/BEFLOOR_FAMILY_REALFILL_
        //   2026-07-07.txt. Ledger rows stand. The on_tick.hpp USOIL H1 aggregator still calls
        //   on_h1_bar(); enabled=false short-circuits.
        {
            auto& uc = omega::usoil_befloor_companion();
            uc.enabled = false;            // feed short-circuits; no seeds, no exec wiring
            uc.clear_open_arms();          // drop persisted shadow arm-state (no frozen "open" legs)
            uc.recompute_and_write();      // publish once: real forward history, no new arms
        }
        printf("[OMEGA-INIT][SEED] USOIL BE-floor companion (USOILPos/USOILNeg) RETIRED S-2026-07-07e (real-fill: 2026 H1 sea of red; lone +cell refuted by 16mo Brent ticks) -- state serves real history, no new arms\n");
        fflush(stdout);

        // ── FX per-pair Pos/Neg BE-floor companion (S-2026-07-06; RETIRED S-2026-07-07) ──
        //   REAL-FILL VERDICT (S-2026-07-07, backtest/index_befloor_intrabar_bt.cpp with bes=2,
        //   certified histdata+duka ticks): the "VALIDATED neg=0" research (fx_befloor_ls.py)
        //   is the same max(0,.) clamp as the index book. Real fills at the live 0.30%/be2:
        //   EURUSD 13mo -$59k real (vs +$112k model), GBPUSD 16mo -$86k (vs +$125k) per 1-lot
        //   book, and the salvage grid (thr 0.3-1.5% x be 2/6/10/20 x exec A/buf10/buf25) has
        //   ZERO positive cells on either pair -- nothing to reconfigure to. ALL 5 PAIRS
        //   RETIRED (the 2026-07-07 EURUSDNeg -$80x2 ledger rows were this mechanism).
        //   Registry §5 MODEL-FILL TRAP. Empty pair list keeps the aggregate publishing an
        //   empty state (panel honest); persisted book/closed CSVs on VPS remain as history.
        {
            // NO pairs added (a zero-size init-list array is ill-formed; the add/seed/set_exec
            // wiring was deleted with the retirement — see git history for the 5-pair block).
            // finalize_all() still runs so the aggregate publishes an EMPTY pairs[] state and
            // the FX COMPANIONS panel reads honest-empty instead of a stale -$160 book.
            auto& fxb = omega::fx_befloor_book();
            fxb.finalize_all();
            printf("[OMEGA-INIT][SEED] FX BE-floor companion RETIRED S-2026-07-07 (real-fill negative, no surviving config; EURUSD -$59k/13mo GBPUSD -$86k/16mo per-lot vs + model) -- aggregate publishes empty state\n");
            fflush(stdout);
        }

        // ── Index per-symbol Pos/Neg BE-floor companion (S-2026-07-06; REAL-FILL re-validation
        //    S-2026-07-07) ───────
        //   The original "VALIDATED" research (backtest/index_befloor_ls.py) books every clip
        //   max(0, floor-fill) with 1.2-1.5bp cost — the "neg=0 by construction" claim is a code
        //   CLAMP, not an execution property. Real fills (worse-of(floor, H1 close), real rt_bp)
        //   on certified tick data (backtest/index_befloor_intrabar_bt.cpp) showed the live
        //   0.30%/be6 config STRUCTURALLY NEGATIVE: US500 3.4yr -$482k real vs +$2.34M model;
        //   DJ30 7mo -$100k real vs +$341k model (90% of exits pierced the floor at close —
        //   the 2026-07-06 US500Pos -$273 x5 ledger rows were this mechanism, not a fluke).
        //   REAL-FILL salvage grid (thr x be x exec, 45 cells/symbol):
        //     US500  KEEP, reconfigured thr=1.5% be=10 + intrabar cap 25bp: +$216k/3.4yr,
        //            BOTH WF halves + (+$45k/+$171k), BOTH flavors + (Pos +$160k/Neg +$56k),
        //            all 5 tiers >= +, worst clip -21.6pt (vs -112.5pt close-only).
        //     NAS100/DJ30/GER40 NOT WIRED: no config passed both WF halves on real fills
        //     (DJ30 best +$32.6k had H2 -$5.5k; GER40 7mo survivors are +$377-noise-thin).
        //   SEPARATE INDEPENDENT observe-only shadow book (feedback-companion-independent-engine),
        //   judged STANDALONE on pts_real. Own aggregate index_companion_state.json ->
        //   /api/index_companion -> desk INDEX COMPANIONS. PRICE POINTS -> USD via sizing.hpp.
        {
            // CULLED S-2026-07-09 (operator: "befloors must all be culled"). US500 was the LAST
            // live-wired BE-floor (the one real-fill-passing cell, thr=1.5%/be=10/cap=25). It is
            // now removed: NO symbols added, NO set_exec -> the book arms/trades nothing.
            // finalize_all() still runs so the aggregate publishes an EMPTY pairs[] state (INDEX
            // COMPANIONS panel reads honest-empty). Persisted book/closed CSVs on the VPS remain
            // as history. NB: the wiring block is kept (not deleted) on purpose -- the singleton
            // defaults enabled=true and the on_tick feed still calls on_h1_bar(); an empty book
            // simply has no symbols to arm. Deleting the block would reconstruct a default
            // enabled=true book and RESURRECT it. Do NOT re-add symbols here.
            auto& ib = omega::index_befloor_book();
            ib.finalize_all();
            printf("[OMEGA-INIT][SEED] index BE-floor companion CULLED S-2026-07-09 (US500 removed -- all befloors culled per operator) -- aggregate publishes empty state\n");
            fflush(stdout);
        }

        // ── JumpRider ENGINE CULLED / TOMBSTONED (S-2026-07-10, operator) — removed completely.
        //    Reason: shorted gold (JumpRiderXAUUSD SYM_FLIP -$5,215) into the safe-haven squeeze;
        //    exhaustive gold-short backtest found NO viable gold short at any TF/regime. Whole
        //    UpJump-on-non-crypto rider retired (header + wiring + feeds + /api/jumprider gone).
        //    Memory-Omega GoldShort tombstone. Do NOT resurrect without a new all-6 basis.

        // ── FX JUMP LADDER companion (S-2026-07-07x wire; S-2026-07-08c both-ways update) ──
        //   The FX member of the no-floor ladder family (BIGCAP daily sibling above). Native
        //   C++ port of backtest/omega_upjump_ladder_bt.py, PARITY-EXACT vs python (backtest/
        //   fx_upjump_parity.cpp: EURUSD +39.7 vs +39.7, GBPUSD +37.4 vs +37.4, NZDUSD +41.2
        //   vs +41.2 — registry §6). Detector: close >= thr% off the W-bar min low -> W-bar
        //   window; legs TIGHT a0.17thr/trail0.67thr + WIDE a2.7thr/g50 + STACKED
        //   {0.67,1.33,2.0}thr g50 + reclip WIDE on +1.67thr (cap 5); LOSS_CUT 5thr pre-arm
        //   (ADVERSE-PROTECTION verdict: free insurance — never binds at the locked cells;
        //   backtested in the expanded sweep). Costs: per-clip RT bp debited, single honest
        //   column, 2x-cost PASS on all wired cells.
        //   Cells — ALL RE-VALIDATED by the 2026-07-08 both-directions sweep
        //   (outputs/FX_BOTHWAYS_SWEEP_2026-07-08.md, anchors reproduced EXACT to the bp,
        //   data gate-certified; raw grid outputs/FX_BOTHWAYS_SWEEP_2026-07-08.txt):
        //     EURUSD W48 thr0.5  L +3972bp PF1.47 n507 (all gates, plateau ok; short side
        //            WEED: all 28 DNJUMP cells negative)
        //     GBPUSD W48 thr1.0  L +3738bp PF2.20 n240 (the ONLY cross-regime PASS:
        //            2022H2 dollar-rally+Truss +1368bp PF1.17, 2x-cost +980)
        //     NZDUSD W24 thr1.5  L +3686bp PF4.02 n90 — RE-PROVEN on gate-clean REBUILT
        //            2025 H1 (Tick/fx_bothways_deriv/NZDUSD_2025_h1.csv from monthly ticks;
        //            the old Tick/NZDUSD_befloor_h1.csv is integrity-REJECTED, 90d hole
        //            Jan-Mar 2026. Live warmup warmup_NZDUSD_H1.csv is a separate continuous
        //            capture — gap-checked clean this session, NOT the rejected file.)
        //     AUDUSD W72 thr0.75 L +3729bp PF1.48 n335 — S-2026-07-08c CELL UPGRADE from
        //            the thin W96/1.0 anchor (+3092, still passes): sweep found W72/0.75
        //            passes STRONGER (WF +2639/+1091, ex-best +3285, 2x-cost +3059,
        //            over-random +3712, plateau ok).
        //     USDCAD W96 thr0.5  S — NEW S-2026-07-08c: first genuine FX short-side ladder
        //            pass. DOWN-JUMP mirror (short_downjump=true): +2241bp PF1.58 n230,
        //            WF +1493/+748, ex-best +2007, 2x-cost +1781, over-random +2137,
        //            broad W-plateau. SINGLE-REGIME caveat (2025 = CAD-strength year, no
        //            2022 data) -> HALF notional ($5k vs $10k standard) + auto-retirement
        //            retire_usd -$580 = 2x worst BT drawdown (maxDD -581bp @$5k = -$291).
        //            USDCAD LONG stays WEED (all 30 cells negative — perfect mirror).
        //   USDJPY WATCH only (nothing cross-regime clean) — not wired. EURGBP/USDCHF WEED/
        //   no-data. XAU bull-beta / GER40 bull-only — index axis, separate wire.
        //   SEPARATE INDEPENDENT observe-only SHADOW book (feedback-companion-independent-
        //   engine), judged STANDALONE, deploy-forward.
        //   FEED: tick_fx.hpp H1 roll (h/l/c — manage is intrabar adverse-first, in-calibration).
        {
            auto& fl = omega::fx_upjump_ladder_book();
            // {pair, W, thr%, rt_cost_bp, short_downjump, notional$, retire_usd, warmup CSV (ts,o,h,l,c H1)}
            struct FLCfg { const char* pair; int W; double thr; double rt;
                           bool short_dj; double notional; double retire; const char* csv; };
            // S-2026-07-09c IBKR-FEED REVALIDATION (operator: pull the REAL IBKR
            //   IDEALPRO feed and re-validate on it — the prior cells were validated on
            //   histdata merged/befloor files that covered short FAVORABLE slices).
            //   Faithful sweep drove THIS engine class over 3Y IBKR H1 MIDPOINT (2023-07..
            //   2026-07, 18,618 bars/pair, all 5 CERTIFIED CLEAN by data_integrity_gate).
            //   Puller backtest/ibkr_fx_h1_pull.py ; sweep backtest/fx_ibkr_ladder_sweep.cpp
            //   + fx_ibkr_ladder_orchestrate.py (each cell in a fresh mktemp cwd -> det.).
            //   All-6 STANDALONE verdict (net>0, PF>=1.3, WF both halves>0, bull>0, bear>0;
            //   bull/bear = pair trend at entry vs SMA480 H1). Results (current exits):
            //     GBPUSD W48/thr0.75  PASS  net +40.5% PF1.44 n526 WF +21.0/+19.5
            //                         bull+18.2 bear+22.3; 2x-cost +30.0% PF1.31 (holds),
            //                         robust across the whole wide_arm x be_entry exit grid
            //                         (every be>=0.08 cell PASSES). The ONE genuine edge on
            //                         the real feed. LIVE cell was W48/thr1.0 (+5.9% FAIL);
            //                         edge lives at thr0.75 -> retuned here.
            //     EURUSD/NZDUSD/AUDUSD/USDCAD  FAIL all-6 on IBKR data (edgeless on the real
            //                         feed — matches the 2026-06-29 "FX edgeless" caution).
            //                         Best IBKR cells: EURUSD none (live W48/0.5 = -4.6%);
            //                         NZDUSD live W24/1.5 = +16.9% but WF-H1 -2.2 & bear +0.5
            //                         (fails); AUDUSD live W72/0.75 = -12.5%; USDCAD only
            //                         "pass" W24/1.5 is a 20-clip fluke (PF21.85, all other
            //                         cells negative). DISABLED pending a genuine cell — do
            //                         NOT force a pass (operator rule; standalone-negative).
            //   Evidence: the 3 research artifacts above (ibkr_fx_h1_pull.py +
            //   fx_ibkr_ladder_sweep.cpp + fx_ibkr_ladder_orchestrate.py) reproduce these
            //   numbers; parity: the same driver reproduces the histdata research exactly
            //   (EURUSD_merged W48/0.5 = +39.7% == research). Vault: FxUpJumpLadder page.
            static const FLCfg FL[] = {
                {"GBPUSD", 48, 0.75, 2.0, false, 10000.0,    0.0, "phase1/signal_discovery/warmup_GBPUSD_H1.csv"},
                // -- DISABLED S-2026-07-09c: FAIL all-6 STANDALONE on 3Y IBKR feed (above). --
                // {"EURUSD", 48, 0.5,  2.0, false, 10000.0,    0.0, "phase1/signal_discovery/warmup_EURUSD_H1.csv"},
                // {"NZDUSD", 24, 1.5,  2.5, false, 10000.0,    0.0, "phase1/signal_discovery/warmup_NZDUSD_H1.csv"},
                // {"AUDUSD", 72, 0.75, 2.0, false, 10000.0,    0.0, "phase1/signal_discovery/warmup_AUDUSD_H1.csv"},
                // {"USDCAD", 96, 0.5,  2.0, true,   5000.0, -580.0, "phase1/signal_discovery/warmup_USDCAD_H1.csv"},
            };
            for (const auto& fc : FL) {
                omega::FxLadderPair::Config c;
                c.pair = fc.pair; c.live_sym = fc.pair;
                c.W = fc.W; c.thr = fc.thr; c.rt_cost_bp = fc.rt;
                c.short_downjump = fc.short_dj; c.notional = fc.notional; c.retire_usd = fc.retire;
                // S-2026-07-09 WIDE peak-profit trail: keep ~90% of the peak gain, engage at +1% MFE
                // (LADDER_WIDE_TRAIL_TIGHTEN_2026-07-09.md — NZD +5.6% / GBP +5.4%, arm0 hurts FX).
                c.wide_gb_frac = 0.10; c.wide_arm_pct = 1.0; c.be_entry_pct = 0.08; c.pend_bars = 4;
                // ── WEEKEND-GAP RISK GATE (S-2026-07-11, go-live) — WEEKEND_RISK_LAYERS_FINDINGS.md,
                //   faithful port of backtest/weekend_risk_layers_bt.py (parity-checked). Flag-gated.
                //   LAYER 2 (block_weekend_arms) = strict free improvement (GBPUSD +40.5%->+43.8%,
                //     all-6 preserved): never arm a NEW window whose trigger closes in the weekend.
                //   LAYER 3 (weekend_carry_frac=0.0, OPERATOR DECISION S-2026-07-11) = carry ZERO
                //     size across the weekend gap -> ~$0 weekend-gap loss. Free on GBPUSD (net +40.5
                //     ->+44.0 at f=0, all-6). GBPUSD empirical worst DOWN-gap = -0.67% (a $67 tail at
                //     $10k) so the de-risk is essentially costless; f=0 chosen for a uniform zero-carry
                //     book across all cells. SHADOW; nothing here is a real forward trade yet.
                c.block_weekend_arms = true;
                c.weekend_carry_frac = 0.0;
                fl.add(std::move(c));
            }
            size_t flseeded = 0;
            for (const auto& fc : FL) flseeded += fl.seed_pair(fc.pair, fc.csv);
            size_t flrestored = fl.seed_dumps_all();   // own persisted forward h/l/c bars
            fl.set_exec(
                /* open   */ [](const std::string& sym, bool is_long, double lots, double px) -> std::string {
                    return send_live_order(sym, is_long, lots, px);
                },
                /* close  */ [](const std::string& sym, bool orig_is_long, double lots, double px, const std::string& token) {
                    send_live_order(sym, !orig_is_long, lots, px, token);
                },
                /* gate   */ [](const std::string& sym, double tp_dist_pts, double lots) -> bool {
                    return ExecutionCostGuard::is_viable(sym.c_str(), 0.00015, tp_dist_pts, lots, 1.5); // FX ~1.5 pip
                },
                /* ledger */ [](const std::string& engine, const std::string& sym, bool is_long,
                                double entry_px, double exit_px, double lots,
                                int64_t entry_ts, int64_t exit_ts, const char* reason) {
                    omega::TradeRecord tr;
                    tr.engine = engine; tr.symbol = sym; tr.side = is_long ? "LONG" : "SHORT";
                    tr.entryPrice = entry_px; tr.exitPrice = exit_px; tr.size = lots;
                    tr.entryTs = entry_ts; tr.exitTs = exit_ts; tr.exitReason = reason;
                    tr.pnl = (is_long ? (exit_px - entry_px) : (entry_px - exit_px)) * lots;
                    handle_closed_trade(tr);
                });
            fl.finalize_all();
            printf("[OMEGA-INIT][SEED] FX jump LADDER wired: GBPUSD(W48/0.75 L) only -- S-2026-07-09c IBKR-feed revalidation: GBPUSD PASS all-6 (+40.5%% PF1.44 n526, 2x-cost holds); EURUSD/NZDUSD/AUDUSD/USDCAD DISABLED (fail all-6 standalone on 3Y IBKR data). %zu H1 bars seeded, %zu forward bars restored, LC5thr+trail+window-flush, SHADOW, deploy-forward\n",
                   flseeded, flrestored);
            fflush(stdout);
        }

        // ── INDEX up-jump LADDER companion (S-2026-07-07x resume, operator: "more NAS100/
        //    most-lucrative-index companion/upjump trades with protections") ──
        //   Same validated ladder class/mechanism as the FX book above (parity-exact port).
        //   Research: backtest/index_upjump_ladder_sweep.py over freshly-built H1 from the
        //   /Users/jo/Tick tick corpus (backtest/histdata_tick_to_h1.cpp), evidence
        //   outputs/INDEX_UPJUMP_LADDER_2026-07-07.txt:
        //     US500  W24 thr2.0: +123.2% PF1.34 n854 WF+ 2x+89 RANDOM -24 (pure detector
        //            edge; SPXUSD_2022_2026.h1.csv CERTIFIED CLEAN; thr2 plateau W12/24/96 WF+)
        //     NAS100 W24 thr1.5: +242.9% PF1.23 n2129 WF+ 2x+179 RANDOM -5 (the most
        //            lucrative index; W24 thr1.5-3.0 pocket beats random massively while the
        //            rest of the NAS grid is bull-beta. DATA CAVEAT: source missing 7 months
        //            (2022-04, 2024-04/07/08/09/11/12) — sweep gap-masked, live guard blocks
        //            windows across gaps; forward record decides promotion)
        //     GER40  W12 thr1.5 BULL-GATED: bull file +72.4% PF3.51 WF+ vs random -6.5, but
        //            bear file 24/24 cells NEGATIVE -> new windows ONLY when
        //            !omega::index_risk_off() (feedback-companion-bull-gate-not-reject)
        //   ADVERSE-PROTECTION: in-mechanism LOSS_CUT 5*thr pre-arm + trail + window flush
        //   (same backtested verdict as the FX book). SEPARATE INDEPENDENT observe-only
        //   SHADOW book, judged STANDALONE, deploy-forward, per-clip RT cost debited.
        {
            auto& il = omega::index_upjump_ladder_book();
            struct ILCfg { const char* tag; const char* live; int W; double thr; double rt;
                           bool bull_gate; const char* csv; };
            static const ILCfg IL[] = {
                // csv != nullptr -> warm-seed from the phase-2 FUTURES-scale warmup CSV.
                // ONLY correct when the LIVE feed for that symbol is ALSO IBKR futures.
                // S-2026-07-09: US500 (ES) + GER40 (DAX) live feeds MOVED to IBKR
                // futures too -> their futures warmup seed is now the CONSISTENT scale,
                // RESTORED alongside NAS100 (all three seed on futures = live feed).
                {"US500",  "US500.F", 24, 2.0, 4.0, false, "phase1/signal_discovery/warmup_US500_H1.csv"},
                {"NAS100", "NAS100",  24, 1.5, 3.0, false, "phase1/signal_discovery/warmup_NAS100_H1.csv"},
                {"GER40",  "GER40",   12, 1.5, 2.0, true,  "phase1/signal_discovery/warmup_GER40_H1.csv"},
                // S-2026-07-09: M2K micro E-mini Russell 2000 (CME), IBKR-only L1 feed.
                // rt=4.0 conservative (micro Russell < ES/NQ liquidity). BULL-GATED: the 2yr
                // IBKR continuous sample has NO 2022 bear and Russell is high-beta -> gate new
                // windows in risk-off (same treatment as GER40; flip to false for the ungated
                // backtested config if bear-regime evidence lands). SHADOW, deploy-forward, cost-debited.
                // S-2026-07-10 CADENCE/EDGE RETUNE (operator: "why aren't the ladders firing"):
                //   the W24/thr1.5 cell was NOT rare (61.6 windows/yr on the 2yr M2K_h1 sample)
                //   but it was the WEAKEST M2K cell. Full W{12,24,48} x thr{0.75..2.0} sweep on
                //   the REAL FxLadderPair over Tick/M2K_h1.csv (= the live warmup_M2K_H1 data,
                //   backtest/index_upjump_ladder_cadence_sweep.cpp, LIVE params arm0.5/be0.08/
                //   gb0.10): W24/thr1.0 STRICTLY DOMINATES W24/thr1.5 on EVERY axis --
                //     thr1.0: fires 70.6/yr n675 net+186.3% PF1.54 H1+93.4 H2+92.9 bull+71.9
                //             bear+64.3 ; 2x-cost(rt8) net+159.3 PF1.45 all-6 HOLD.
                //     thr1.5: fires 61.6/yr n530 net +76.5% PF1.24 H1+44.8 H2+31.6 bull+33.5
                //             bear+26.5 (= the S-07-09 wired figure; harness parity confirmed).
                //   MORE fires + 2.4x net + higher PF + balanced halves/regimes, all-6 + 2x PASS
                //   -> thr lowered 1.5->1.0. CAVEAT (registry ENGINE_BACKTEST_REGISTRY §9 index
                //   trap): a net-over-random-window control (the python research gate) is OWED
                //   before treating any lift as pure detector edge vs Russell bull-beta -- this
                //   retune is all-6+2x-validated but the random control was not re-run (C++-only
                //   session). W12/thr1.5 was rejected (bear -1.8, all-6 FAIL); W48 impossible
                //   (RTH-only H1 + the 6-day contiguity guard blocks every 48-bar window -> 0 fires).
                {"M2K",    "M2K",     24, 1.0, 4.0, true,  "phase1/signal_discovery/warmup_M2K_H1.csv"},
            };
            for (const auto& ic : IL) {
                omega::FxLadderPair::Config c;
                c.pair = ic.tag; c.live_sym = ic.live;
                c.W = ic.W; c.thr = ic.thr; c.rt_cost_bp = ic.rt;
                c.file_prefix = "idxladder_companion_";
                if (ic.bull_gate) c.block_new_windows_fn = [] { return omega::index_risk_off(); };
                // S-2026-07-09 WIDE peak-profit trail: keep ~90% of the peak gain, engage at +1% MFE
                // (LADDER_WIDE_TRAIL_TIGHTEN_2026-07-09.md — US500 +24%, GER40 +4.9%, NAS100 +7.4% at
                // arm1; NAS100 alone is +34.4% at arm0/engage-from-entry -> per-symbol opt-in below).
                c.wide_gb_frac = 0.10;
                // S-2026-07-09 SURGICAL FIX (operator: NAS100 gave back a +0.6% pop because the
                // WIDE runner armed at +1.0% and never engaged). LOWER the wide arm to +0.5% so it
                // catches sub-1% pops, but KEEP it riding (10% giveback) so trends still run. Index
                // H1 sweep (SPX/GER40/NASbear, real engine): arm1.0 SPX+161/GER+107, arm0.5
                // SPX+155/GER+131 -> catches small pops, keeps ~all trend capture. US500's smooth
                // trends slightly prefer a higher arm; per-symbol tuning available if wanted.
                c.wide_arm_pct = 0.5;
                c.be_entry_pct = 0.08; c.pend_bars = 4;   // BE-ENTRY: open only once cost covered (WF+ both halves, both regimes; no open-into-loss)
                // ── WEEKEND-GAP RISK GATE (S-2026-07-11, go-live) — WEEKEND_RISK_LAYERS_FINDINGS.md,
                //   faithful port of backtest/weekend_risk_layers_bt.py (parity-checked). Flag-gated.
                //   LAYER 2 (block_weekend_arms) = strict free improvement on every index cell
                //     (US500 +0.2%, NAS100 +6.0%, GER40 +0.0%, M2K +8.7%; all-6 preserved): never
                //     arm a NEW window whose trigger bar closes inside the weekend window.
                //   LAYER 3 (weekend_carry_frac=0.0, OPERATOR DECISION S-2026-07-11) = carry ZERO
                //     size across the weekend gap -> the index cells' correlated macro-Monday tail
                //     (Apr-2025 tariff crash gapped GER40 -9.46% / NAS100 -4.85% / US500 -2.91% /
                //     M2K -3.55% the SAME weekend, ~$2,077 portfolio at full $10k carry) is capped
                //     at ~$0. f=0 passes all-6 on every cell: FREE-or-better on US500 (+204.9->+207.2),
                //     NAS100 (+90.1->+108.7), GER40 (+123.7->+129.2); a small accepted cost on M2K
                //     (+186.3->+163.8, WF-H2 +93.7->+63.5, STILL all-6). Empirical worst DOWN-gaps:
                //     US500 -2.91%, NAS100 -4.85%, GER40 -9.46%, M2K -5.03%. SHADOW; deploy-forward.
                c.block_weekend_arms = true;
                c.weekend_carry_frac = 0.0;
                // NAS100 upside opt-in: arm0 (engage the 10% trail FROM ENTRY) = NAS100 +34.4%
                // (robust: WF+ both halves, bear -12.4->+0.4). Flip below for the max NAS catch.
                // if (std::string(ic.tag) == "NAS100") c.wide_arm_pct = 0.0;
                il.add(std::move(c));
            }
            // S-2026-07-09 COMPLETE FEED-MIGRATION — FULL revert of the c1a83306 seam fix.
            //   Background: c1a83306 dropped the phase-2 warmup seed for ALL 3 index-ladder
            //   symbols because the seed is FUTURES-scale (seed_refresh.py phase-2 pulls
            //   NQ/ES/DAX via IB Gateway, ~29433 NAS100) while the LIVE feed was the
            //   BlackBull CFD (~29214) -- a two-feed scale SEAM in one price deque.
            //   Now (S-2026-07-09) ALL THREE live feeds move to IBKR futures: NAS100->NQ
            //   (depth), US500->ES + GER40->DAX (L1) -- fix_dispatch is_ibkr_primary_index
            //   gates BlackBull out and omega_main on_book posts the bridge futures tick
            //   under each symbol. So the futures warmup seed is once again the CORRECT,
            //   CONSISTENT scale for ALL THREE -> RESTORED: seed + live + disp_mid + the
            //   desk tiles all agree on the IBKR-futures scale, no seam. (US500 basis is
            //   ~0.3-0.5%, GER40 ~0.5-1% over their CFDs -- cosmetic on this SHADOW book.)
            //   Cutover note: pre-existing idxladder_companion_{nas100,us500,ger40}_h1.csv
            //   dumps hold OLD CFD-scale forward bars; on cutover the operator should
            //   remove all three so the ladder warms cleanly on futures scale (else a
            //   one-window transient until W H1 futures bars dominate ~12-24h).
            size_t ilseeded = 0;
            for (const auto& ic : IL) if (ic.csv) ilseeded += il.seed_pair(ic.tag, ic.csv);
            size_t ilrestored = il.seed_dumps_all();   // own persisted forward bars (per-symbol scale)
            il.set_exec(
                /* open   */ [](const std::string& sym, bool is_long, double lots, double px) -> std::string {
                    return send_live_order(sym, is_long, lots, px);
                },
                /* close  */ [](const std::string& sym, bool orig_is_long, double lots, double px, const std::string& token) {
                    send_live_order(sym, !orig_is_long, lots, px, token);
                },
                /* gate   */ [](const std::string& sym, double tp_dist_pts, double lots) -> bool {
                    return ExecutionCostGuard::is_viable(sym.c_str(), 0.5, tp_dist_pts, lots, 1.5); // index CFD ~0.5pt
                },
                /* ledger */ [](const std::string& engine, const std::string& sym, bool is_long,
                                double entry_px, double exit_px, double lots,
                                int64_t entry_ts, int64_t exit_ts, const char* reason) {
                    omega::TradeRecord tr;
                    tr.engine = engine; tr.symbol = sym; tr.side = is_long ? "LONG" : "SHORT";
                    tr.entryPrice = entry_px; tr.exitPrice = exit_px; tr.size = lots;
                    tr.entryTs = entry_ts; tr.exitTs = exit_ts; tr.exitReason = reason;
                    tr.pnl = (is_long ? (exit_px - entry_px) : (entry_px - exit_px)) * lots;
                    handle_closed_trade(tr);
                });
            il.finalize_all();
            printf("[OMEGA-INIT][SEED] INDEX upjump LADDER wired: US500(W24/2.0) NAS100(W24/1.5) GER40(W12/1.5 BULL-GATED) M2K(W24/1.0 BULL-GATED, feed via bridge --symbols M2K), ALL IBKR-futures seed+live (S-2026-07-09 complete migration), %zu H1 warmup bars seeded, %zu forward bars restored, LC5thr+trail+window-flush, SHADOW, deploy-forward\n",
                   ilseeded, ilrestored);
            fflush(stdout);
        }

        // ── Stock day-mover BE-floor companion (S-2026-07-06; RETIRED S-2026-07-07e) ──
        //   REAL-FILL VERDICT (faithful daily-close port over data/rdagent/sp500_long_close.csv
        //   2019-06..2026-06, rt=8bp, $10k notional): the "every BIGCAP name net>0, gross-neg=0,
        //   both WF halves +" research (daymover_befloor_x3_v2.py / daymover_pername_screen.py)
        //   was the max(0,.) clamp (registry §5). Real fills = worse-of(floor, DAILY close):
        //   39-name book -$110.7k real vs +$1.57M model; Neg (down-mover) flavor -$325k at every
        //   thr 3/4/5%; Pos-only +$214k total but its FIRST half (2019-2022, which CONTAINS the
        //   2020-21 bull) is negative at every thr — daily granularity + overnight gaps eat the
        //   trail. No robust config -> RETIRED (no names added; empty aggregate publishes so the
        //   STOCK MOVERS panel reads honest-empty). Evidence outputs/BEFLOOR_FAMILY_REALFILL_
        //   2026-07-07.txt. No poller started (nothing to feed). Ledger rows stand.
        {
            auto& sb = omega::stockmover_befloor_book();
            sb.finalize_all();   // EMPTY book -> publishes empty aggregate state (panel honest)
            printf("[OMEGA-INIT][SEED] stock day-mover BE-floor companion RETIRED S-2026-07-07e (real-fill -$110.7k vs +$1.57M model over 7yr; Pos-only fails 2019-22 half) -- aggregate publishes empty state\n");
            fflush(stdout);
        }

        // ── Stock day-mover UP-JUMP LADDER companion (S-2026-07-07w, operator item 4) ──
        //   The NO-FLOOR successor to the retired BE-floor above. Native C++ port of the
        //   VALIDATED backtest/bigcap_upjump_ladder_bt.py (evidence outputs/BIGCAP_UPJUMP_
        //   LADDER_2026-07-07.md, vault BigCapUpJumpLadder): parent +3% day -> enter NEXT
        //   close, exit -3% day (flush next close); legs TIGHT a0.5/s2 (stall banker) +
        //   WIDE a8/g50 (giveback runner) + self-funding ladder cap5, reclip 5%, LOSS_CUT 15
        //   (ADVERSE-PROTECTION verdict: FREE, worst clip -32.6% -> -28.1%), RT 8bp/clip.
        //   39-name pooled book: n=4,981 net +7,044% of clip notional PF1.58, all-6 PASS,
        //   2x-cost PASS, ex-semis PASS, full-565-universe PASS. LONG-ONLY (Neg flavor died
        //   with befloor). SEPARATE INDEPENDENT observe-only SHADOW book (feedback-companion-
        //   independent-engine), judged STANDALONE, deploy-forward ($0 until first live clip).
        //   COST GATE: harness-validated 8bp RT debited per clip (ExecutionCostGuard has no
        //   single-name equity rows — the befloor lesson); real cost row owed before LIVE sizing.
        //   FEED: same wide daily-close CSV as befloor (RDAgent refresh_close_ibkr.py, IBKR 4002).
        //   NOTE: sp500_long_close.csv stale since 2026-06-29 (IBKR sub lapse — operator item);
        //   engine seeds+arms now, books resume the day the feed does.
        {
            auto& sl = omega::stockmover_ladder_book();
            // the RDAgent BIGCAP universe (tools/rdagent/refresh_close_ibkr.py) — the exact
            // 39-name list the PASS was measured on (bigcap_upjump_ladder_bt.py BIGCAP).
            static const char* BIGCAP_LAD[] = {
                "NVDA","AMD","AVGO","MU","MRVL","SMCI","ARM","PLTR","TSLA","META","NFLX","CRWD",
                "SHOP","COIN","MSTR","SNOW","NOW","PANW","UBER","ABNB","DELL","ORCL","QCOM","INTC",
                "AMZN","GOOGL","MSFT","AAPL","CRM","ADBE","IONQ","RGTI","QBTS","ASTS","RKLB","NBIS",
                "CRWV","ALAB","CRDO",
                // S-2026-07-08c FULL-UNIVERSE ADDS (operator go; outputs/FULL_UNIVERSE_RANK_2026-07-08.txt,
                // all-6 pass + net-over-random >= +17k% on 533-name IBKR-refreshed data):
                "WDC","STX","DD","TPR","BMY","SWKS"
            };
            // S-2026-07-08c AGGRESSIVE MULTIPLIER (operator: "wire the aggressive
            // multiplier"). Per-name validated-cell ranking 2019-2026 (evidence
            // outputs/BIGCAP_AGGRESSIVE_RANKING_2026-07-08.md; bar = n>=30, PF>=1.3,
            // both-WF-halves+, ex-best+):
            //   x2 notional ELITE  : MU 2.32 / NVDA 2.33 / AVGO 2.70 / DELL 2.72 /
            //                        CRDO 3.27 / PANW 6.23
            //   x1 baseline        : every other net-positive name (incl the 10
            //                        passers AMD/INTC/AAPL/CRM/ALAB/MRVL/MSFT/ASTS/
            //                        SMCI/ARM and the positive-but-fragile middles)
            //   RANKED-OUT (no new windows): per-name NET-NEGATIVE at the validated
            //                        cell -- TSLA PF0.56, COIN, PLTR, MSTR, UBER,
            //                        CRWV, SHOP, META, IONQ, QBTS. Detector history
            //                        still maintained; re-rank can re-enable warm.
            // Auto-retirement (-$600 scaled with notional) stays the safety net on
            // everything left on. Shadow book; re-rank when the 744-name refresh
            // extends the universe.
            // (MSVC note, cutover-#8 build fail C2760/C3536: the original lambda with
            // static local arrays breaks MSVC in this nested init scope; plain arrays
            // + inline loops compile everywhere.)
            const char* AGG_ELITE[] = {"MU","NVDA","AVGO","DELL","CRDO","PANW",
                                       // S-2026-07-08c full-universe re-rank promotions:
                                       // WDC over-random +78.9k%/PF2.57, STX +57.8k/2.67,
                                       // DD +73.9k/4.36, INTC +32.6k/1.78 (was baseline)
                                       "WDC","STX","DD","INTC"};
            const char* AGG_OUT[]   = {"TSLA","COIN","PLTR","MSTR","UBER",
                                       "CRWV","SHOP","META","IONQ","QBTS"};
            int n_elite = 0, n_out = 0;
            for (const char* nm : BIGCAP_LAD) {
                omega::StockLadderSym::Config c;
                c.sym = nm; c.live_sym = nm;   // equities: live order symbol == ticker
                // S-2026-07-09 WIDE peak-profit trail: replace the arm8/gb50 runner with arm1/gb10
                // ("keep ~90% of the peak gain", exit on the turn). backtest/stockladder_wide_trail_
                // tighten.py 39-name pooled: round-trip 12.0%->7.3%, all-6 gate still PASS, net
                // -13.4% (operator accepts the protection cost; the stock ladder is where the real
                // 12% "gave it all back" problem lives). LOSS_CUT 15% unchanged.
                c.w_arm = 1.0; c.w_gb = 0.10;
                // S-2026-07-11 operator "extra mimic x4": 4x the clip notional across the WHOLE
                // stock book ($10k->$40k base). This is a MIMIC -> it never touches the real trade
                // (CompanionDominanceError), so scaling it is purely additive and its own drawdown-
                // cancel (loss_cut 15%, DRAWDOWN-CANCEL gate S-2026-07-11) is the free safety lever.
                // Retirement bar scales with size so protection stays proportional (-$600->-$2400).
                // Elite x2 tilt still applies ON TOP (elite -> $80k / -$4800).
                c.notional   *= 4.0;
                c.retire_usd *= 4.0;
                bool is_elite = false, is_out = false;
                for (const char* e : AGG_ELITE) if (c.sym == e) { is_elite = true; break; }
                for (const char* o : AGG_OUT)   if (c.sym == o) { is_out   = true; break; }
                c.cap = 6;   // S-2026-07-08c: +1 for the MIRROR base leg (ladder capacity unchanged)
                if (is_out) { c.ranked_out = true; ++n_out; }
                else if (is_elite) {
                    c.notional   *= 2.0;       // elite x2 clip notional
                    c.retire_usd *= 2.0;       // retirement bar scales with size
                    ++n_elite;
                }
                sl.add(std::move(c));
            }
            printf("[OMEGA-INIT] BIGCAP ladder aggressive ranking: %d elite x2, %d ranked-out, %d baseline\n",
                   n_elite, n_out, (int)(sizeof(BIGCAP_LAD)/sizeof(BIGCAP_LAD[0])) - n_elite - n_out);
            const std::string wide_csv = "data/rdagent/sp500_long_close.csv";
            // S-2026-07-08c ORDER FIX (cutover-#9 no-op catch-up): set_exec MUST run
            // BEFORE seeding -- live_step_ hard-returns when open_fn_ is unset, so a
            // boot catch-up row dispatched pre-set_exec was consumed as a no-op (the
            // 07-07 row replay converted nothing). Seed rows are live=false and fire
            // no orders regardless; catch-up rows suppress broker calls via
            // g_aulad_catchup -- wiring exec first is side-effect-free for both.
            sl.set_exec(
                /* open   */ [](const std::string& sym, bool is_long, double lots, double px) -> std::string {
                    return send_live_order(sym, is_long, lots, px);
                },
                /* close  */ [](const std::string& sym, bool orig_is_long, double lots, double px, const std::string& token) {
                    send_live_order(sym, !orig_is_long, lots, px, token);
                },
                /* gate   */ [](const std::string& /*sym*/, double /*tp_dist_pts*/, double /*lots*/) -> bool {
                    // Equity cost gate = the harness's OWN 8bp RT debit (PASS is net-of-8bp and
                    // survives 2x=16bp; single-name IBKR RT ~3-8bp). ExecutionCostGuard has NO
                    // single-name cost table -> unknown tickers default to CFD-scaled values that
                    // mis-price stocks (befloor lesson). TODO: real equity cost row before LIVE sizing.
                    return true;
                },
                /* ledger */ [](const std::string& engine, const std::string& sym, bool is_long,
                                double entry_px, double exit_px, double lots,
                                int64_t entry_ts, int64_t exit_ts, const char* reason) {
                    omega::TradeRecord tr;
                    tr.engine = engine; tr.symbol = sym; tr.side = is_long ? "LONG" : "SHORT";
                    tr.entryPrice = entry_px; tr.exitPrice = exit_px; tr.size = lots;
                    tr.entryTs = entry_ts; tr.exitTs = exit_ts; tr.exitReason = reason;
                    tr.pnl = (is_long ? (exit_px - entry_px) : (entry_px - exit_px)) * lots;
                    handle_closed_trade(tr);
                });
            // S-2026-07-10 LIVE-CONFIRMATION GATE (operator: stop opening "fake" paper legs on
            // stale daily-close signals). Armed BEFORE seeding so the boot catch-up replay also
            // defers to a live tick instead of blind-opening. A pending +thr window now opens ONLY
            // when a live L1 tick (fed via on_book -> stockmover_ladder_book().on_live_tick) confirms
            // US-session-open + fresh(<60s) + rising(px > arming close). Requires the bigcap tickers
            // on the IBKR bridge --symbols (STK/SMART/USD L1). Bridge down / no tick -> stays PENDING
            // (safe: no blind open). US-equity L1 entitlement VERIFIED live 2026-07-10 (NVDA/AVGO/
            // SMCI/DELL real-time bid/ask, mdType=1, no err 354). See ENGINE_BACKTEST_REGISTRY §6.
            sl.set_live_confirm(true);
            size_t lseeded = sl.seed_from_wide_csv(wide_csv);   // primes detector history (deploy-forward) + live catch-up
            size_t lrestored = sl.seed_dumps_all();             // replay persisted forward daily bars
            sl.finalize_all();
            // STEP 3: one-shot flush of the pre-gate blind-open legs (the 18 fake NVDA/AVGO/SMCI/
            // DELL/NBIS/CRDO opens). Sentinel-guarded -> runs once, then future live-confirmed legs
            // persist across restarts untouched.
            size_t lflushed = sl.flush_all_unconfirmed_once("live-confirm gate S-2026-07-10");
            // S-2026-07-10 IN-BINARY DAILY-CLOSE WRITER (operator: "why yfinance when we have live
            // IBKR data writing the CSV we hot-seed from"). The bigcap names are on the IBKR bridge
            // as STK/SMART/USD L1 (on_book -> on_live_tick stores each name's live mid). The poll
            // loop now snapshots those mids once/day at the 20:00 UTC US cash close and APPENDS a
            // WIDE-format row to the same daily-close CSV — one IBKR source, replacing the yfinance
            // producer (tools/rdagent/vps_stockmover_feed.py, task OmegaStockMoverFeed, to be DISABLED
            // on the VPS once this ships). Idempotent (persisted YMD + last-row-date guard); min 10
            // fresh names to write (bridge-down/thin day -> skip, last-good CSV untouched).
            sl.enable_daily_close_writer(true, 10);
            sl.start_poller(wide_csv, 900000);   // 15-min poll of the wide daily-close CSV
            printf("[OMEGA-INIT][SEED] BIGCAP upjump LADDER companion wired: 45 names, %zu seed rows, %zu forward bars restored, %zu unconfirmed legs flushed, LIVE-CONFIRM GATE ON (session+fresh<60s+rising), TIGHT a0.5/s2 + WIDE a1/g10 + MIRROR a2/g75 + ladder cap6 reclip5%%, LOSS_CUT 15, rt 8bp, LONG-only, SHADOW, deploy-forward, daily-CSV-polled\n",
                   lseeded, lrestored, lflushed);
            printf("[OMEGA-INIT][SEED] BIGCAP in-binary DAILY-CLOSE WRITER active: target=%s, fires once/day at 20:00 UTC (US cash close, weekday) appending a WIDE-format row from live IBKR L1 mids (min 10 fresh names), idempotent -- REPLACES yfinance OmegaStockMoverFeed\n",
                   wide_csv.c_str());
            fflush(stdout);
        }

        // ── BigCap2pctImpulse per-name +2%-impulse LOOSE-RIDE book (S-2026-07-09) ──
        //   A SEPARATE INDEPENDENT engine from the up-jump ladder above. ONE LONG
        //   position per name: entered when the daily close is >= +2% close-to-close
        //   AND a NEW 20-day closing high (impulse + continuation), UNGATED (fires
        //   every regime, no bull/bear/200d gate). Ridden with a DELIBERATELY LOOSE
        //   3-layer exit: gb90 peak-profit give-back trail (keep 10% of the peak) +
        //   60-day max-hold cap + -15% catastrophe hard floor. NO tight give-back leg,
        //   NO tight (3-8%)+BE loss-cut — those AMPUTATE this impulse signal.
        //   FAITHFUL C++ VALIDATION (backtest/clip_path_bigcap_impulse.cpp over
        //   data/rdagent/sp500_long_close.csv 2019-07..2026-07, 45 names, 20bp RT):
        //   n=932 clips, PF 2.18, net +2797.5% of clip notional, WF H1 +553% / H2
        //   +2245%, worst clip -42.5% (daily-close gap through the floor). Regime by the
        //   PRINCIPLED market label (equal-weight basket vs its own 200DMA): bull(>200DMA)
        //   +2075% n=701 / bear(<200DMA) +722% n=231 — BOTH POSITIVE => ALL-6 PASS.
        //   (A stricter calendar-2022-entry slice is -273%, a harsh stress cut that lumps
        //   the sharpest 2022-decline entries into catastrophe floors — informational, not
        //   the regime gate.) LONG-only. SEPARATE INDEPENDENT observe-only SHADOW book
        //   (feedback-companion-independent-engine), judged STANDALONE, deploy-forward
        //   ($0 until first live clip). COST GATE: 20bp RT debited per clip (the
        //   validated gate; ExecutionCostGuard has no single-name equity rows — the
        //   befloor lesson; real cost row owed before LIVE sizing). FEED: same wide
        //   daily-close CSV as the ladder (stale since 2026-06-29, IBKR sub lapse).
        {
            auto& bi = omega::bigcap_impulse_book();
            static const char* BC2_UNIV[] = {
                "NVDA","AMD","AVGO","MU","MRVL","SMCI","ARM","PLTR","TSLA","META","NFLX","CRWD",
                "SHOP","COIN","MSTR","SNOW","NOW","PANW","UBER","ABNB","DELL","ORCL","QCOM","INTC",
                "AMZN","GOOGL","MSFT","AAPL","CRM","ADBE","IONQ","RGTI","QBTS","ASTS","RKLB","NBIS",
                "CRWV","ALAB","CRDO","WDC","STX","DD","TPR","BMY","SWKS"
            };
            for (const char* nm : BC2_UNIV) {
                omega::BigCapImpulseSym::Config c;
                c.sym = nm; c.live_sym = nm;   // equities: live order symbol == ticker
                // loose-ride cfg (validated): thr 2% / new-20d-high / gb90 / 60d cap /
                // -15% catastrophe / 20bp RT / $10k notional. UNGATED, LONG-only.
                c.thr = 0.02; c.hi_window = 20; c.gb = 0.90; c.max_hold = 60;
                c.catastrophe = 15.0; c.rt_cost_bp = 20.0; c.notional = 10000.0;
                bi.add(std::move(c));
            }
            const std::string bc2_csv = "data/rdagent/sp500_long_close.csv";
            // set_exec BEFORE seeding (cutover-#9 lesson: a boot catch-up row dispatched
            // pre-set_exec would fire no live order; seed rows are live=false regardless,
            // catch-up rows suppress broker calls via g_bc2_catchup). Routes through the
            // SAME send_live_order path (hard-gated on mode!=LIVE) as every companion —
            // SHADOW today, LIVE on flip with ZERO code change.
            bi.set_exec(
                /* open   */ [](const std::string& sym, bool is_long, double lots, double px) -> std::string {
                    return send_live_order(sym, is_long, lots, px);
                },
                /* close  */ [](const std::string& sym, bool orig_is_long, double lots, double px, const std::string& token) {
                    send_live_order(sym, !orig_is_long, lots, px, token);
                },
                /* gate   */ [](const std::string& /*sym*/, double /*tp_dist_pts*/, double /*lots*/) -> bool {
                    // Equity cost gate = the book's OWN 20bp RT debit (PASS is net-of-20bp).
                    // ExecutionCostGuard has NO single-name cost table -> unknown tickers
                    // default to CFD-scaled values that mis-price stocks (befloor lesson).
                    // TODO: real equity cost row before LIVE sizing.
                    return true;
                },
                /* ledger */ [](const std::string& engine, const std::string& sym, bool is_long,
                                double entry_px, double exit_px, double lots,
                                int64_t entry_ts, int64_t exit_ts, const char* reason) {
                    omega::TradeRecord tr;
                    tr.engine = engine; tr.symbol = sym; tr.side = is_long ? "LONG" : "SHORT";
                    tr.entryPrice = entry_px; tr.exitPrice = exit_px; tr.size = lots;
                    tr.entryTs = entry_ts; tr.exitTs = exit_ts; tr.exitReason = reason;
                    tr.pnl = (is_long ? (exit_px - entry_px) : (entry_px - exit_px)) * lots;
                    tr.shadow = true;   // SHADOW book: audit-only ledger row
                    handle_closed_trade(tr);
                });
            size_t bi_seeded   = bi.seed_from_wide_csv(bc2_csv);   // primes detector history (deploy-forward) + live catch-up
            size_t bi_restored = bi.seed_dumps_all();              // replay persisted forward daily bars
            bi.finalize_all();
            bi.start_poller(bc2_csv, 900000);   // own 15-min poll of the wide daily-close CSV
            printf("[OMEGA-INIT][SEED] BIGCAP 2%%-impulse LOOSE-RIDE book wired: %d names, %zu seed rows, %zu forward bars restored, thr2%%/new-20d-high/gb90/60d-cap/-15%%-catastrophe, rt 20bp, LONG-only, UNGATED, SHADOW, deploy-forward, daily-CSV-polled\n",
                   (int)(sizeof(BC2_UNIV)/sizeof(BC2_UNIV[0])), bi_seeded, bi_restored);
            fflush(stdout);
        }

        // ── StockDip + StockTurtle per-name daily-close books (S-2026-07-08c) ──
        //   TWO validated LONG-only single-stock families in ONE registry
        //   (include/StockDipTurtleEngine.hpp), AUDITED_CONFIGS rows
        //   StockConnorsDip_RESEARCH PF1.60 + StockTurtleD1_RESEARCH PF2.13
        //   (evidence outputs/STOCK_OTHER_ENGINES_2026-07-08.txt):
        //   * StockDip (ConnorsRSI2 archetype): close>SMA200 & RSI2<10 at the daily
        //     close -> LONG; exit first close>SMA5 or 10 trading days. Wired = the
        //     11 individual all-6 passers of the audited per-name split.
        //   * StockTurtle (Donchian 20/10): close > max(prior 20 closes) -> LONG;
        //     exit close < min(prior 10 closes). Wired = the 11 all-6 passers
        //     (n>=30, net>0, PF>=1.3, both time-halves>0, ex-best>0) of the
        //     S-2026-07-08c per-name rerun outputs/STOCK_TURTLE_PERNAME_2026-07-08.txt
        //     (rule-faithful rerun: PF 2.13 / bear22 -93.3 / best-trade +167% match
        //     the audited pooled row exactly; MU/WDC/INTC fail on a negative H1,
        //     CRM on PF, DELL/CRDO/PANW on n).
        //   ADVERSE-PROTECTION verdicts + auto-retirement basis live in the engine
        //   header. $10k notional/position, 8bp RT debit (the validated gate) +
        //   ExecutionCostGuard::is_viable US-equity row on entry. SHADOW, judged
        //   STANDALONE, deploy-forward ($0 until the first live close), boot
        //   catch-up watermark, own 15-min poller on the same wide daily CSV.
        {
            auto& sdt = omega::stock_dipturtle_book();
            // (MSVC C2760 lesson: plain arrays + loops, no lambdas w/ static locals)
            const char* DIP_NAMES[] = { "MU","NVDA","AVGO","DELL","CRDO","STX",
                                        "INTC","AMD","AAPL","TPR","MSFT" };
            const char* TUR_NAMES[] = { "NVDA","AVGO","STX","DD","AMD","AAPL",
                                        "TPR","BMY","SWKS","MSFT","QCOM" };
            int n_dip = 0, n_tur = 0;
            for (const char* nm : DIP_NAMES) {
                omega::StockDipTurtleSym::Config c;
                c.sym = nm; c.live_sym = nm; c.family = omega::StockDipTurtleSym::DIP;
                // AUTO-RETIREMENT: -$9,500 = ~2x the worst BT per-name banked-curve
                // DD episode (MU -47.6% of $10k = -$4,756; engine header).
                c.retire_usd = -9500.0;
                sdt.add(std::move(c)); ++n_dip;
            }
            for (const char* nm : TUR_NAMES) {
                omega::StockDipTurtleSym::Config c;
                c.sym = nm; c.live_sym = nm; c.family = omega::StockDipTurtleSym::TURTLE;
                // AUTO-RETIREMENT: -$8,500 = ~2x the worst BT per-name banked-curve
                // DD episode (TPR -41.3% of $10k = -$4,130; engine header).
                c.retire_usd = -8500.0;
                sdt.add(std::move(c)); ++n_tur;
            }
            // exec wiring BEFORE seeding (cutover-#9: live logic hard-returns when
            // open_fn unset -- a catch-up row dispatched pre-set_exec is a no-op).
            sdt.set_exec(
                /* open   */ [](const std::string& sym, bool is_long, double lots, double px) -> std::string {
                    return send_live_order(sym, is_long, lots, px);
                },
                /* close  */ [](const std::string& sym, bool orig_is_long, double lots, double px, const std::string& token) {
                    send_live_order(sym, !orig_is_long, lots, px, token);
                },
                /* gate   */ [](const std::string& sym, double tp_dist_pts, double lots) -> bool {
                    // S-2026-07-08c US-equity cost row (lots = SHARES; spread ~2c bigcap).
                    // The validated book-level gate stays the 8bp RT debit in ret_real.
                    return ExecutionCostGuard::is_viable(sym.c_str(), 0.02, tp_dist_pts, lots);  // global-scope struct (MSVC C3083 fix)
                },
                /* ledger */ [](const std::string& engine, const std::string& sym, bool is_long,
                                double entry_px, double exit_px, double lots,
                                int64_t entry_ts, int64_t exit_ts, const char* reason) {
                    omega::TradeRecord tr;
                    tr.engine = engine; tr.symbol = sym; tr.side = is_long ? "LONG" : "SHORT";
                    tr.entryPrice = entry_px; tr.exitPrice = exit_px; tr.size = lots;
                    tr.entryTs = entry_ts; tr.exitTs = exit_ts; tr.exitReason = reason;
                    tr.pnl = (is_long ? (exit_px - entry_px) : (entry_px - exit_px)) * lots;
                    tr.shadow = true;   // SHADOW book: audit-only ledger row
                    handle_closed_trade(tr);
                });
            const std::string sdt_csv = "data/rdagent/sp500_long_close.csv";
            size_t sdt_seeded   = sdt.seed_from_wide_csv(sdt_csv);   // indicators + boot catch-up
            size_t sdt_restored = sdt.seed_dumps_all();              // persisted forward daily bars
            sdt.finalize_all();
            sdt.start_poller(sdt_csv, 900000);   // own 15-min poller
            printf("[OMEGA-INIT][SEED] StockDip/StockTurtle books wired: %d dip + %d turtle names, %zu seed rows, %zu forward bars restored, $10k notional, rt 8bp, retire dip -$9.5k / turtle -$8.5k, LONG-only, SHADOW, deploy-forward, daily-CSV-polled\n",
                   n_dip, n_tur, sdt_seeded, sdt_restored);
            fflush(stdout);
        }

        // ── Stall/giveback-clip companion zoo (S-2026-07-06) ─────────────────
        //   Native C++ port of the retired Mac-cron stall_accountant.py (25 gold/index
        //   PROTECTION books) + companion_aggregate.py. PARENT-MIRROR streaming books:
        //   each mirrors the real engine legs in the in-memory live_trades[] snapshot,
        //   banks its OWN giveback-clip book, never touches a real position
        //   (feedback-companion-independent-engine). Driven every 60s from on_tick off
        //   the live snapshot; the merged aggregate is written to companion_state.json
        //   -> /api/companion (the existing desk COMPANION panel, schema unchanged).
        //   State lives under C:\Omega\stall\<book>\ ; the historical companion_closed.csv
        //   ledgers are migrated from the Mac at cutover so each book's bank CONTINUES.
        //   Configs are a 1:1 transcription of the retired cron lines (params in comment).
        {
            using SC = omega::StallBook::Config;
            const std::vector<std::string> EXG =   // the shared "main" book's default engine exclude list
                {"IBS","Mean-Rev","MeanRev","RSIrev","RSIRev","Regime","Connors","Seasonal",
                 "Monday","Turnaround","TurnOfMonth","Turtle","TSMom50"};
            auto& reg = omega::stall_companions();
            auto B = [&](SC c){ c.dir = "stall/" + c.name; reg.add(std::move(c)); };
            // helpers: PCT book / USD book (mirrors the two cron gauges)
            // --- shared main book (default EXCLUDE, default gauge) ---
            { SC c; c.name="main"; c.exclude=EXG; B(c); }
            // --- index turtle D1 clips (PCT gauge, arm2, giveback variants) ---
            { SC c; c.name="spx_turtle_clip";      c.include={"US500"}; c.gate_pct=2; c.rev_gb=0.50; c.stall_bars=9999; c.retrig_pct=0.02; c.tf_sec=24*3600; B(c); }
            { SC c; c.name="dj30_turtle_clip";     c.include={"DJ30"};  c.gate_pct=2; c.rev_gb=0.50; c.stall_bars=9999; c.retrig_pct=0.02; c.tf_sec=24*3600; B(c); }
            { SC c; c.name="spx_turtle_clip_gv40"; c.include={"US500"}; c.gate_pct=2; c.rev_gb=0.40; c.stall_bars=9999; c.retrig_pct=0;    c.tf_sec=24*3600; B(c); }
            { SC c; c.name="spx_turtle_clip_gv60"; c.include={"US500"}; c.gate_pct=2; c.rev_gb=0.60; c.stall_bars=9999; c.retrig_pct=0;    c.tf_sec=24*3600; B(c); }
            { SC c; c.name="spx_turtle_clip_gv80"; c.include={"US500"}; c.gate_pct=2; c.rev_gb=0.80; c.stall_bars=9999; c.retrig_pct=0;    c.tf_sec=24*3600; B(c); }
            // --- gold vol/panic/mgc %-clips ---
            { SC c; c.name="mgc_fastdon_clip";        c.include={"MgcFastDonchian30m"}; c.gate_pct=1; c.rev_gb=0.50; c.stall_bars=9999; c.retrig_pct=0; c.tf_sec=1800;   B(c); }
            { SC c; c.name="gold_panic_bounce_clip";  c.include={"GoldPanicBounce"};    c.gate_pct=1; c.rev_gb=0.50; c.stall_bars=9999; c.retrig_pct=0; c.tf_sec=3600;   B(c); }
            // --- XAU TrendFollow zoo reclip companions (PCT gauge) ---
            { SC c; c.name="xau_tf4h_clip"; c.include={"XauTrendFollow4h"}; c.gate_pct=2; c.rev_gb=0.50; c.stall_bars=9999; c.retrig_pct=0.02; c.tf_sec=4*3600; B(c); }
            { SC c; c.name="xau_tf1h_clip"; c.include={"XauTrendFollow1h"}; c.gate_pct=2; c.rev_gb=0.50; c.stall_bars=9999; c.retrig_pct=0.02; c.tf_sec=1*3600; B(c); }
            { SC c; c.name="xau_tfd1_clip"; c.include={"XauTrendFollowD1"}; c.gate_pct=1; c.rev_gb=0.50; c.stall_bars=9999; c.retrig_pct=0;    c.tf_sec=24*3600; B(c); }
            // --- XAU TrendFollow $-gauge clips (USD mode: arm_usd/trail_usd/retrig_usd) ---
            //   cold_loss_bear=-35 on 4h+1h (S-2026-07-08 loss-bound study, operator ask after the
            //   2x -$51 tf4h LOSS_CUTs in the 60pt/30min drop): tighter cold cut ONLY while gold is
            //   price-bear (gold_regime().is_bear() core; NOT long_blocked -- macro-hostile is the
            //   trend engines' best cohort per GOLD_DEEP_DIVE_2026-07-08). Evidence
            //   backtest/companion_lossbound_sweep.py on certified XAUUSD_2022_2026 H1 (intrabar
            //   adverse-first, econ per-cycle accounting): 4h keeps 96% econ / 98.5% ledger net,
            //   worst leg -71->-52; 1h keeps 96%; all-6 PASS both; smooth plateau -35/-25/-20/-15.
            //   FLAT tightening REJECTED (4h: -35 keeps 66%, -15 fails all-6). Velocity-freeze
            //   REJECTED (no-op on 4h/1h). 2h/d1/gvb NOT wired (2h: no benefit; d1/gvb: untested).
            { SC c; c.name="xau_tfd1_usd";   c.include={"XauTrendFollowD1"}; c.bull_only=true;  c.arm_usd=40; c.trail_usd=20; c.retrig_usd=20; c.stall_bars=9999; c.tf_sec=24*3600; B(c); }
            { SC c; c.name="xau_tf4h_usd_a"; c.include={"XauTrendFollow4h"};                    c.arm_usd=30; c.trail_usd=15; c.retrig_usd=15; c.stall_bars=9999; c.tf_sec=4*3600;  c.cold_loss_bear=-35; B(c); }
            { SC c; c.name="xau_tf4h_usd_b"; c.include={"XauTrendFollow4h"};                    c.arm_usd=30; c.trail_usd=15; c.retrig_usd=15; c.stall_bars=9999; c.tf_sec=4*3600;  c.cold_loss_bear=-35; B(c); }
            { SC c; c.name="xau_tf1h_usd_a"; c.include={"XauTrendFollow1h"};                    c.arm_usd=15; c.trail_usd=30; c.retrig_usd=30; c.stall_bars=9999; c.tf_sec=1*3600;  c.cold_loss_bear=-35; B(c); }
            { SC c; c.name="xau_tf1h_usd_b"; c.include={"XauTrendFollow1h"};                    c.arm_usd=15; c.trail_usd=30; c.retrig_usd=30; c.stall_bars=9999; c.tf_sec=1*3600;  c.cold_loss_bear=-35; B(c); }
            { SC c; c.name="xau_tf4h_aggr";  c.include={"XauTrendFollow4h"};                    c.arm_usd=15; c.trail_usd=5;  c.retrig_usd=15; c.stall_bars=8;    c.tf_sec=1*3600;  B(c); }
            { SC c; c.name="xau_tf1h_aggr";  c.include={"XauTrendFollow1h"};                    c.arm_usd=15; c.trail_usd=10; c.retrig_usd=15; c.stall_bars=12;   c.tf_sec=1*3600;  B(c); }
            { SC c; c.name="xau_tf4h_aggr_b";c.include={"XauTrendFollow4h"};                    c.arm_usd=15; c.trail_usd=5;  c.retrig_usd=15; c.stall_bars=8;    c.tf_sec=1*3600;  B(c); }
            { SC c; c.name="xau_tf1h_aggr_b";c.include={"XauTrendFollow1h"};                    c.arm_usd=15; c.trail_usd=10; c.retrig_usd=15; c.stall_bars=12;   c.tf_sec=1*3600;  B(c); }
            // S-2026-07-08d DISABLED (loss-bound study flag): the 2h gold USD companion books
            // are econ-marginal (PF1.15 at live config, below the PF>=1.3 companion standard).
            // Their parent XauTF2h stack is unaffected; the 4h/1h/D1 USD books stay live.
            // { SC c; c.name="xau_tf2h_usd_a"; c.include={"XauTrendFollow2h"}; c.bull_only=true;  c.arm_usd=40; c.trail_usd=10; c.retrig_usd=10; c.stall_bars=9999; c.tf_sec=2*3600;  B(c); }
            // { SC c; c.name="xau_tf2h_usd_b"; c.include={"XauTrendFollow2h"}; c.bull_only=true;  c.arm_usd=40; c.trail_usd=10; c.retrig_usd=10; c.stall_bars=9999; c.tf_sec=2*3600;  B(c); }
            { SC c; c.name="xau_tfd1_usd_b"; c.include={"XauTrendFollowD1"}; c.bull_only=true;  c.arm_usd=40; c.trail_usd=20; c.retrig_usd=20; c.stall_bars=9999; c.tf_sec=24*3600; B(c); }
            // --- GoldVolBreakout M30 $-gauge clips (bull-gated) ---
            // S-2026-07-11 GOLD PHASE 1: the MGC instance got its own tag
            // (MgcVolBreakoutM30_*, venue-identity fix); include BOTH tags so the
            // clip trigger population is unchanged by the retag.
            { SC c; c.name="gvb_m30_usd_a"; c.include={"GoldVolBreakoutM30","MgcVolBreakoutM30"}; c.bull_only=true; c.arm_usd=20; c.trail_usd=30; c.retrig_usd=30; c.stall_bars=9999; c.tf_sec=1800; B(c); }
            { SC c; c.name="gvb_m30_usd_b"; c.include={"GoldVolBreakoutM30","MgcVolBreakoutM30"}; c.bull_only=true; c.arm_usd=20; c.trail_usd=30; c.retrig_usd=30; c.stall_bars=9999; c.tf_sec=1800; B(c); }
            // ── ConnorsMirror x2 add-on mirror (S-2026-07-07t, SHADOW) ────────
            //   VALIDATED-thin (AUDITED ConnorsMirror_NAS100, commit 059918cd): REAL
            //   ConnorsRSI2 NAS100 parents, real-fill H1 close-eval sim — arm at parent
            //   gain >=2.0%, x2 size, trail gb 0.75%, retrig 2%, flat-on-parent-close:
            //   n15/4.3yr +897pt PF2.66 WR67 both-halves+ bear22 +410 2x-cost +809
            //   worst -155 ex-best +496. arm<=1.0% ALL NEG (snapback spent). ~3.5 t/yr
            //   THIN -> SHADOW, promote at n>=30 banked mirrors. DIFFERENT mechanism
            //   from the giveback-clip books above (add-on mirror from arm price); the
            //   main book's Connors EXCLUDE stays — no overlap. Separate independent
            //   book per operator rule; judged STANDALONE. NAS100 rt_bp=3.
            {
                omega::MirrorBook::Config m;
                m.name = "connors_mirror_x2";
                m.legs = {"ConnorsRSI2|NAS100"};
                m.arm_pct = 2.0; m.gb_pct = 0.75; m.retrig_pct = 2.0;
                m.size_mult = 2.0; m.rt_bp = 3.0; m.tf_sec = 3600;
                m.dir = "stall/" + m.name;
                auto& mb = reg.add_mirror(std::move(m));
                printf("[OMEGA-INIT][SEED] ConnorsMirror x2 wired (SHADOW, arm2.0/gb0.75/retrig2/x2 H1-close): restored %zu watch, %d open mirror(s) from stall/connors_mirror_x2\n",
                       mb.watching(), mb.open_mirrors());
            }
            // ── Mirror sweep x2 add-ons (S-2026-07-07v, SHADOW) ──────────────
            //   Operator order 2026-07-07: wire the 3 PASSes from the all-books
            //   mirror sweep ([[MirrorSweep20260707]], AUDITED_CONFIGS rows
            //   SpxTurtleD1Mirror/XauTF2hMirror_LONG/XauTF4hMirror_LONG, evidence
            //   outputs/MIRROR_SWEEP_2026-07-07.txt). Same validated cell as
            //   connors (arm2.0/gb0.75/retrig2/x2, H1-close, worse-of fills):
            //     SpxTurtleD1  n12  +94.0 PF1.73 bothH YES bear22 YES (thin -> SHADOW)
            //     XauTF2h LONG n59 +283.6 PF1.53 bothH YES bear22 YES
            //     XauTF4h LONG n119 +519.2 PF1.40 bothH YES bear22 YES
            //   XauTF fams LONG-ONLY (cfg.long_only): the sweep's SHORT sim is
            //   invalid (negation breaks % thresholds) -> shorts unvalidated.
            //   rt_bp: US500 4.0, XAU 3.4 (faithful sweep costs). Promote n>=30.
            //   Legs match the telemetry live_trades tags VERIFIED this session
            //   (plain "XauTrendFollow2h/4h|XAUUSD"; "SpxTurtleD1|US500.F" via the
            //   new dashboard source below at ~L6370). Judged STANDALONE
            //   (feedback-companion-independent-engine).
            {
                omega::MirrorBook::Config m;
                m.name = "spx_turtle_mirror_x2";
                m.legs = {"SpxTurtleD1|US500.F"};
                m.arm_pct = 2.0; m.gb_pct = 0.75; m.retrig_pct = 2.0;
                m.size_mult = 2.0; m.rt_bp = 4.0; m.tf_sec = 3600;
                m.dir = "stall/" + m.name;
                auto& mb = reg.add_mirror(std::move(m));
                printf("[OMEGA-INIT][SEED] SpxTurtleMirror x2 wired (SHADOW, arm2.0/gb0.75/retrig2/x2 H1-close rt4bp): restored %zu watch, %d open mirror(s)\n",
                       mb.watching(), mb.open_mirrors());
            }
            {
                omega::MirrorBook::Config m;
                m.name = "xautf2h_mirror_x2";
                m.legs = {"XauTrendFollow2h|XAUUSD"};
                m.arm_pct = 2.0; m.gb_pct = 0.75; m.retrig_pct = 2.0;
                m.size_mult = 2.0; m.rt_bp = 3.4; m.tf_sec = 3600;
                m.long_only = true;
                m.dir = "stall/" + m.name;
                auto& mb = reg.add_mirror(std::move(m));
                printf("[OMEGA-INIT][SEED] XauTF2hMirror x2 wired (SHADOW LONG-only, arm2.0/gb0.75/retrig2/x2 H1-close rt3.4bp): restored %zu watch, %d open mirror(s)\n",
                       mb.watching(), mb.open_mirrors());
            }
            {
                omega::MirrorBook::Config m;
                m.name = "xautf4h_mirror_x2";
                m.legs = {"XauTrendFollow4h|XAUUSD"};
                m.arm_pct = 2.0; m.gb_pct = 0.75; m.retrig_pct = 2.0;
                m.size_mult = 2.0; m.rt_bp = 3.4; m.tf_sec = 3600;
                m.long_only = true;
                m.dir = "stall/" + m.name;
                auto& mb = reg.add_mirror(std::move(m));
                printf("[OMEGA-INIT][SEED] XauTF4hMirror x2 wired (SHADOW LONG-only, arm2.0/gb0.75/retrig2/x2 H1-close rt3.4bp): restored %zu watch, %d open mirror(s)\n",
                       mb.watching(), mb.open_mirrors());
            }
            printf("[OMEGA-INIT] stall-companion zoo wired: %zu books + %zu mirror (native C++, /api/companion; python cron retired)\n", reg.size(), reg.mirror_count());
            fflush(stdout);
        }

        // Market-bear PROXY (NAS bellwether) -- IndexRiskGate's price-based dead-feed
        //   fallback so index longs stay protected in a bear even with no macro feed.
        // 2026-06-23 stale-seed fix (same class as gold_regime): persist + self-fresh so the
        // index risk-off gate doesn't reset to a stale seed every restart.
        {
            omega::index_market_regime().set_live_dump(log_root_dir() + "/index_mkt_regime_h1.csv");
            if (!omega::index_market_regime().load_state(log_root_dir() + "/index_mkt_regime_state.dat"))
                omega::index_market_regime().seed_from_h1_csv("phase1/signal_discovery/warmup_NAS100_H1.csv");
        }
        printf("[OMEGA-INIT] index_market_regime() proxy: regime=%s warm=%d (IndexRiskGate dead-feed fallback)\n",
               omega::index_market_regime().regime_name(), (int)omega::index_market_regime().warm());
        fflush(stdout);

        // S-2026-07-10: restore the DISPLAY VIX across restarts (spot VIX only streams US RTH;
        // an off-hours restart otherwise shows VIX 0 until 13:30 UTC). regime() still treats a
        // restored-but-stale VIX as NEUTRAL via m_vix_ts, so this is display-only, never a signal.
        g_macroDetector.load_vix(log_root_dir() + "/macro_vix_state.dat");
        printf("[OMEGA-INIT] macro VIX restored: vix=%.2f (display-only; regime gates on freshness)\n",
               g_macroDetector.vixLevel());
        fflush(stdout);

        // ── XauStopRunD1Engine (2026-05-20) -- 5d stop-run reversal long
        //   Resurrection of S50 X2 retired StopRunReversal. Re-tested 2yr daily:
        //     FUL Sh=6.34 at 10bps (IS=7.06 OOS=6.14), n=29, WR=65.5%.
        //     Cost-robust to 50bps (FUL Sh=4.12).
        // 2026-05-27 S57: DISABLED -- regime split LOW Sharpe -3.68 (clear
        // negative in low-vol regime, 9/28 trades). Operator strict policy.
        fflush(stdout);

        // ── XauPullbackContH4Engine (2026-05-20) -- EMA10>EMA50 pullback long
        //   PullbackCont archetype (S49 X5 retirement). H4 2yr XAU:
        //   FUL Sh=3.96, IS=3.97, OOS=4.06, n=97 (highest density of D-class).
        fflush(stdout);

        // ── XauNbmD1Engine (2026-05-20) -- Noise Band Momentum D1
        //   Signal from disabled g_nbm_* family. D1 XAU 2yr:
        //   FUL Sh=8.01, IS=9.60, OOS=7.30, n=25.
        // 2026-05-27 S53: DISABLED -- DD/gross=111% (+6.34 / -7.01).
        // Sharpe +1.91 positive but equity buries deeper than recovers.
        // Same failure mode as TF2h/D1 (S52).
        fflush(stdout);

        // ── XauEmaCrossH4Engine (2026-05-20) -- 20/100 golden cross H4
        //   H4 XAU 2yr: FUL Sh=7.15, IS=4.45, OOS=9.19 (OOS > IS).
        //   Sparse n=20 but cleanly OOS-validated.
        // 2026-05-27 S57: DISABLED -- regime split LOW Sharpe -8.48 (worst
        // catastrophic single-regime failure in entire zoo). Overall +6.38
        // misleading -- engine bleeds heavily when low-vol regime persists.
        fflush(stdout);

        // ── 2026-05-20 mega-sweep batch (4 new engines) ─────────────────────
        // 2026-05-27 S54: DISABLED -- walk-forward OOS sign-flip.
        // IS Sharpe +4.27 / 22 trades / +$6.07 gross
        // OOS Sharpe -1.12 / 5 trades / -$1.19 gross  <-- sign flipped
        // Edge does not survive out-of-sample. PullbackH4 robust as backup.

        // 2026-05-27 S53: DISABLED -- DD/gross=240% (+3.68 / -8.85). Worst
        // DD ratio in the entire zoo. Sharpe +1.75 positive but extremely
        // unstable equity curve. Same failure mode as TF2h/D1 (S52).

        g_xau_swing_break_d1.p           = omega::make_xau_swing_break_d1_params();
        // 2026-05-27 S53: DISABLED -- DD/gross=101% (+4.41 / -4.47). Borderline
        // but same failure mode as TF2h/D1 (S52). Thin Sharpe +1.39.
        g_xau_swing_break_d1.shadow_mode = true;
        g_xau_swing_break_d1.enabled     = false;  // S53: DD ratio fail
        g_xau_swing_break_d1.symbol      = "XAUUSD";
        printf("[OMEGA-INIT] XauSwingBreakD1Engine: shadow=%d lb=%d sl=%.1fx tp=%.1fx hold=%d\n",
               (int)g_xau_swing_break_d1.shadow_mode,
               g_xau_swing_break_d1.p.lookback_days,
               g_xau_swing_break_d1.p.sl_atr_mult, g_xau_swing_break_d1.p.tp_atr_mult,
               g_xau_swing_break_d1.p.hold_max_days);

        // ── S136 2026-05-24: XauDonchian55GatedM30Engine ───────────────────────
        // XAU M30 Donchian-55 symmetric + EMA50/200 regime gate.
        // MFE-lock trail: arm at +0.7R, lock 80% of extreme.
        // L2-forward-validated (2026-04-09 → 2026-05-19): PF 3.03, +$773 / 48 trades.
        // 2026-05-27 S50: DISABLED -- real-class audit (xau_donchian55_m30_audit)
        // showed real Sharpe -1.47 / -$14 / 585 trades / 22:1 SL:TP ratio. The
        // L2-forward "PF 3.03" was inflated by inline-reimpl harness — same
        // pattern as GoldScalpPyramid. Class-driven test on full M30 history
        // contradicts the claim. Keep wired but off.

        // ── XauStraddleM30Engine (S-2026-06-02) ────────────────────────────────
        // OCO breakout straddle (Quantum Dark Gold entry, minus the M5 grid).
        // M30 boxN15, stop 3*ATR, TP=1R, symmetric. Research
        // (straddle_breakout_sweep.cpp, 2yr XAUUSD tick): OOS PF 1.64-1.90,
        // Sharpe 4-6, WR ~62%, both legs net-positive, survives 3x cost.
        // The EA's "thousand cuts" was the M5 TF (the grid masked it); M30 + 1R
        // TP removes the bleed -> NO GRID. HARD shadow until the auto-demote gate
        // judges it on >=30 live trades. Reuses the existing 24.7k-bar M30 warmup.
        // S-2026-06-17 CULLED: ledger_analytics (n=8) NEG-EXPECTANCY, net -$123,
        // expR -0.51 -- the bar-replay overstatement (validated PF 1.6-1.9) caught
        // live. Straddle's wrong-leg bleed isn't covered by the right-leg edge at
        // IBKR cost. Disabled; revisit only via engine-faithful tick BT.
        fflush(stdout);

        // ── XauStraddleM15 (S-2026-06-02): M15 sibling, fed M15 bars in tick_gold.
        // Same engine, same params; the 1R TP makes the higher frequency pay.
        // Validated BETTER than M30 (OOS PF 1.72-1.78 Sharpe 6.6-7.3, lower MDD,
        // 3x-cost-robust). Reuses the 49.4k-bar warmup_XAUUSD_M15.csv. HARD shadow,
        // gate-watched -- the live ledger sorts out its overlap with M30.
        fflush(stdout);

        // ── OrbBreakoutEngine / ESTX50 (S-2026-06-02) ──────────────────────────
        // Faithful long-only Opening-Range-Breakout. ESTX50 was the ONE OOS-robust
        // survivor of the multi-symbol ORB sweep (orb_multi_sweep.cpp): OR 07:00-
        // 08:00 UTC, enter on OR_high+0.05*ATR break, range-SL (opposite OR edge),
        // TP=2.0R, flat 15:30 UTC, one shot/day. OOS@cost2.0 PF 1.28 Sh 1.41 win 48%
        // MDD 150. Cost-sensitive (PF 1.35@c1 -> 1.09@c3) -- watch live ESTX50 cost.
        // Short leg dead -> long_only. Distinct from the disabled %-based
        // g_orb_estx50 (OpeningRangeEngine). HARD shadow, registered in the gate.
        // CULL S-2026-06-17 (retest campaign): faithful real-class BT on REAL
        // ESTX50 (=EUSIDXEUR) Dukascopy ticks, 6mo (orb_estx50_revalidate.cpp):
        // PF0.97 NEGATIVE at ZERO cost, both halves neg (0.75/0.98), -251pt @2pt
        // cost, n=109. Prior PF1.28 was the optimistic orb_multi_sweep (m5
        // bar-replay); faithful tick = dead. Reversible: shadow, lot 0.01.
        fflush(stdout);

        // ── INDEX STRADDLE cells (S-2026-06-02) ────────────────────────────────
        // Gold straddle archetype on indices: rolling box (boxN15) + 3*ATR stop +
        // 1R TP, SYMMETRIC. straddle_breakout_sweep.cpp (OOS 0.33, up to 3x cost):
        //   GER40  M30 PF1.49 Sh4.3 WR62% | M15 PF1.31 Sh4.0 WR58%
        //   NAS100 M15 PF1.42 Sh4.9 WR59% | M30 PF1.45 Sh4.1 WR59%
        //   UK100  M30 PF1.40 Sh4.2 WR60% | M240 PF1.85 Sh3.4 WR64% (lowest DD)
        // Both legs net+ on all. Self-aggregating (tf_min>0). HARD shadow, gate-watched.
        {
            struct IdxCell { omega::XauStraddleM30Engine* e; const char* sym; const char* name;
                             int tf; int hold; double maxsp; const char* warm; };
            const IdxCell cells[] = {
                { &g_idx_straddle_ger40_m30,  "GER40",  "IdxStraddleGER40_M30",  30,  48, 6.0, "phase1/signal_discovery/warmup_GER40_M30.csv"  },
                { &g_idx_straddle_ger40_m15,  "GER40",  "IdxStraddleGER40_M15",  15,  96, 6.0, "phase1/signal_discovery/warmup_GER40_M15.csv"  },
                { &g_idx_straddle_nas_m15,    "NAS100", "IdxStraddleNAS100_M15", 15,  96, 6.0, "phase1/signal_discovery/warmup_NAS100_M15.csv" },
                { &g_idx_straddle_nas_m30,    "NAS100", "IdxStraddleNAS100_M30", 30,  48, 6.0, "phase1/signal_discovery/warmup_NAS100_M30.csv" },
                { &g_idx_straddle_uk100_m30,  "UK100",  "IdxStraddleUK100_M30",  30,  48, 4.0, "phase1/signal_discovery/warmup_UK100_M30.csv"  },
                { &g_idx_straddle_uk100_m240, "UK100",  "IdxStraddleUK100_M240", 240, 24, 4.0, "phase1/signal_discovery/warmup_UK100_M240.csv" },
            };
            for (const auto& c : cells) {
                c.e->shadow_mode   = true;
                // 2026-06-04: DISABLED. Index-straddle cells make pennies (~$0-2)
                // on 250-740min holds -> no real edge (distinct from the validated
                // GOLD XauStraddle). Net ~0 across NAS/GER/UK. Culled; index book =
                // FVGcont + OvernightDrift.
                c.e->enabled       = false;
                c.e->symbol        = c.sym;
                c.e->engine_name   = c.name;
                c.e->box_n         = 15;
                c.e->stop_atr      = 3.0;
                c.e->tp_r          = 1.0;
                c.e->lot           = 0.01;
                c.e->tf_min        = c.tf;
                c.e->hold_max_bars = c.hold;
                c.e->max_spread    = c.maxsp;
                c.e->obi_tilt      = false;   // OBI signal is XAU-only
                c.e->seed_from_csv(c.warm);
                printf("[OMEGA-INIT] %s: shadow=%d tf=%dm boxN=%d stop=%.1fx TP=%.1fR\n",
                       c.name, (int)c.e->shadow_mode, c.tf, c.e->box_n, c.e->stop_atr, c.e->tp_r);
            }
            fflush(stdout);
        }

        // ── S136 2026-05-24: Xau3BarMomGatedH4Engine ───────────────────────────
        // XAU H4 three-bar momentum, symmetric long+short.
        // MFE-lock trail: arm at +1.0R, lock 90% of extreme.
        // L2 forward: PF 1.53, +$365 / 34 trades. WF agg OOS +$2931 / 199 trades.
        // 2026-05-27 S50: DISABLED -- real-class audit (xau_d1_zoo_audit)
        // showed real Sharpe -1.81 / -$30 / 384 trades / SL=185 TP=15. The
        // claimed PF 1.53 was harness-class divergence. Class-driven test on
        // full H4 tape says the engine bleeds. Keep wired but off.

        // ── S136 2026-05-24: Us303BarMomH1Engine ───────────────────────────────
        // US30 H1 three-bar momentum, symmetric long+short.
        // MFE-lock trail: arm at +1.0R, lock 90% of extreme.
        // WF 4 folds all positive, agg OOS +$10,943 / 160 trades.
        // No US30 H1 warmup CSV in repo yet; engine cold-warms over ~15 H1 bars
        // before first signal possible. TODO: generate warmup_US30_H1.csv.
        // 2026-05-26 S37j: REVERTED to shadow per operator directive --
        // "NOTHING should be live all shadow". Earlier flip (LIVE) at
        // S37 was operator-approved to capture DJ30 trend gains but now
        // pulled back. ALL engines now shadow_mode=true.

        // ── S37 2026-05-26: Us30EnsembleEngine ─────────────────────────────────
        // DJ30.F 4-cell ensemble (M15 base; synthesizes M30/H1/H4 internally).
        // Cells: atr_exp H1, inside_brk H1, atr_exp M30, ema_pullback H4. All
        // LONG-only. BE/trail OFF by default (sim showed they clip atr_exp
        // winners; ENGINE_SIM.PY bare +$1411 vs trail -$676 over 2yr at 0.01 lot).
        // Cells INDEPENDENT (up to 4 concurrent positions per operator choice).
        // Validation: 3-period intersection + 4/4 walk-forward folds positive
        // + engine-sim integrated backtest +$1411 / 1711 trades / WR 50%.
        // max_spread bumped 5.0 -> 10.0 (2026-05-26): VPS shadow showed
        // [GUARD-BLOCK] engine=Us30Ensemble reason=SPREAD_CAP firing every tick
        // at post-NY-close DJ30 spread ~5.80 pts. 10.0 covers Asia/off-RTH
        // sessions; tighter live-session spreads (~2-4 pts) still pass.

        // ── S136 2026-05-24: NasBbRevLongH1Engine ──────────────────────────────
        // NAS100 H1 Bollinger-band reversion LONG (close<lower BB + RSI cross<30).
        // BE-then-trail at 1.5×ATR (BE arm at +1R, switch to ATR trail at +2R).
        // WF 4 folds all positive, OOS aggregate +$7912 / 145 trades.
        // No NAS L2 data yet; L2 validation pending.
        g_nas_bbrev_long_h1.shadow_mode = true;
        g_nas_bbrev_long_h1.enabled     = false;  // S47: scalp/mean-rev purge
        g_nas_bbrev_long_h1.symbol      = "NAS100";
        g_nas_bbrev_long_h1.seed_from_h1_csv("phase1/signal_discovery/warmup_NAS100_H1.csv");
        printf("[OMEGA-INIT] NasBbRevLongH1: shadow=%d enabled=%d bb=%d/%.1f rsi_lo=%.0f "
               "sl=%.1fx tp=%.1fR mb=%d trail=BE@%.1fR->ATR@%.1fR x%.1f\n",
               (int)g_nas_bbrev_long_h1.shadow_mode, (int)g_nas_bbrev_long_h1.enabled,
               g_nas_bbrev_long_h1.p.bb_period, g_nas_bbrev_long_h1.p.bb_k,
               g_nas_bbrev_long_h1.p.rsi_oversold,
               g_nas_bbrev_long_h1.p.sl_atr_mult, g_nas_bbrev_long_h1.p.tp_r_mult,
               g_nas_bbrev_long_h1.p.hold_max_bars,
               g_nas_bbrev_long_h1.p.trail_be_arm_R, g_nas_bbrev_long_h1.p.trail_switch_R,
               g_nas_bbrev_long_h1.p.trail_atr_mult);

        // TOMBSTONED 2026-06-11 (operator winners-only cull). Live shadow ledger
        // (May 11-Jun 11): n=7, 0 wins, PF=0.00, net -$206. Turtle breakout whipsaws
        // in the GER40 chop regime; the validated GER40 edge is Keltner (g_ger40_kelt).
        // Also in the n>=30 auto-demote gate list (L7132) but n<30 so it never fired.
        // Warm-seed GER40 H4 history (~1600 bars / 11 months) so 20-bar
        // Donchian + 14-bar ATR are populated. Without seed, cold-warm
        // requires 80+ hours of live GER40 ticks before first signal.

        // ── NasTurtleD1Engine (2026-06-14) ──────────────────────────────────
        // Seykota/Donchian D1 archetype on NAS100, long-only, SHADOW. Clone of
        // the Ger40TurtleH4 index-turtle chassis retuned to D1 + NAS100 (DAX
        // session filter removed; index VIX risk-off gate kept). NAS validated
        // as one of only two Omega trend horses with XAU (Yahoo daily 2016-26
        // long-only MAR 0.44 PF 2.10; reconfirms the FVG/Peachy NAS edge).
        // Warm-seeded from the bundled NAS100 D1 CSV so 20-day Donchian + ATR14
        // are hot on first tick. >=5 live shadow trades before any LIVE thought.
        g_nas_turtle_d1.p           = omega::make_nas_turtle_d1_params();
        g_nas_turtle_d1.shadow_mode = true;
        g_nas_turtle_d1.enabled     = true;   // S-2026-07-03 RE-ENABLED GATED-TO-BULL (operator: "redo the ndx and
        g_nas_turtle_d1.p.regime_bear_block = true; // ensure its gated to bull"). regime_bear_block sits new longs
                                              // out in sustained bear -> answers the 06-24 cull reason (it was
                                              // bull-beta / no bear edge): now it ONLY trades bull/neutral, so the
                                              // bear-beta hole is closed by construction. STAYS shadow_mode -> no
                                              // live money; writes shadow closes to prove the gated form.
                                              // Prior 06-24 cull rationale (kept for history):
                                              // 2026-06-22 = bull-beta, 2022 NEG (n=5, -$8 = noise, no bear edge).
                                              // SPX + DJ30 turtles (EDGE) carry the real cross-regime turtle edge,
                                              // so NAS adds only bull-beta shadow noise. Not good enough -> off.
                                              // SHADOW. CONFIG-DRIFT CORRECTED 2026-06-18: an earlier
        // comment claimed a high PF for a Donch20+ema100 variant that is NOT the deployed config (this
        // engine has NO ema100 filter by default). The faithful 10yr-daily class-driven audit
        // (backtest/index_turtle_d1_audit.cpp) of the ACTUAL shipped config (Donch20, no-ema100,
        // sl1.5/tp5/hold20) = MARGINAL-KEEP, 2022-bear ~flat, both WF halves+. ema100 + wide-exit
        // variants raise bull PF but DESTROY bear protection (bull-beta) -> NOT enabled. Sibling SPX +
        // DJ30 turtles are the real cross-regime edges. ALL faithful numbers live in
        // backtest/AUDITED_CONFIGS.tsv (the single source of truth) — do NOT cite PFs inline here.
        g_nas_turtle_d1.symbol      = "NAS100";
        g_nas_turtle_d1.seed_from_d1_csv("phase1/signal_discovery/warmup_NAS100_D1.csv");
        printf("[OMEGA-INIT] NasTurtleD1Engine: shadow=%d lb=%d sl=%.1fx tp=%.1fx hold=%d\n",
               (int)g_nas_turtle_d1.shadow_mode,
               g_nas_turtle_d1.p.lookback_bars,
               g_nas_turtle_d1.p.sl_atr_mult, g_nas_turtle_d1.p.tp_atr_mult,
               g_nas_turtle_d1.p.hold_max_bars);

        // ── DJ30 + SPX D1 turtles (2026-06-15) ──────────────────────────────
        // Same NasTurtleD1 chassis. Cross-regime validated Yahoo daily 2016-2026
        // (incl 2022 bear), cost-inclusive both-halves: DJ30 PF2.09 (+13173, H1+/H2+),
        // SPX PF2.49 (+2435, H1+/H2+). Self-aggregate D1 from ticks; warm-seeded.
        // dollars_per_pt set per BlackBull CFD lot (DJ30 5, US500 50). HARD shadow.
        g_dj30_turtle_d1.p               = omega::make_nas_turtle_d1_params();
        g_dj30_turtle_d1.p.dollars_per_pt = 5.0;
        // S-2026-06-26s fleet-sweep UPGRADE (workflow wn6lralw2, verify held=True):
        // sl_atr_mult 1.5 -> 2.0. DJ30 PF1.77->2.12, maxDD HALVED (-78->-34),
        // 2022 bear stays + (strongest cell). Both-regime+both-half. Per-instance
        // override only -- NAS turtle keeps 1.5 (its sweep cell did not upgrade).
        g_dj30_turtle_d1.p.sl_atr_mult    = 2.0;
        g_dj30_turtle_d1.shadow_mode     = false;  // S-2026-07-01: LIVE on IBKR 4002 paper (validated EDGE)
        g_dj30_turtle_d1.enabled         = true;
        g_dj30_turtle_d1.symbol          = "DJ30.F";
        g_dj30_turtle_d1.seed_from_d1_csv("phase1/signal_discovery/warmup_DJ30_D1.csv");
        g_spx_turtle_d1.p                = omega::make_nas_turtle_d1_params();
        g_spx_turtle_d1.p.dollars_per_pt = 50.0;
        // S-2026-06-26s fleet-sweep UPGRADE (workflow wn6lralw2, verify held=True):
        // sl_atr_mult 1.5 -> 2.0. Both-regime+both-half+, cuts maxDD. Per-instance.
        g_spx_turtle_d1.p.sl_atr_mult     = 2.0;
        g_spx_turtle_d1.shadow_mode      = false;  // S-2026-07-01: LIVE on IBKR 4002 paper (validated EDGE)
        g_spx_turtle_d1.enabled          = true;
        g_spx_turtle_d1.symbol           = "US500.F";
        g_spx_turtle_d1.seed_from_d1_csv("phase1/signal_discovery/warmup_US500_D1.csv");
        // S-2026-06-28 acct-guard DISABLED (faithful re-BT KILL). The earlier "SPX PF 3.80->5.30"
        // was bar-replay-INFLATED. Faithful next-open daily on 10yr /Tick data: the guard HURTS both
        // turtles -- SPX PF 1.76->1.38, DJ30 1.76->1.53, ~2x maxDD; it cuts the runner-exits the turtle
        // edge depends on. The sl_atr 1.5->2.0 change IS good and is KEPT (SPX/DJ30 PF->1.76, both-halves+,
        // 2022-bear+). Override of the 06-26 bar-replay claim with faithful evidence.
        g_dj30_turtle_d1.accounting_guard_.enabled = false;
        g_spx_turtle_d1.accounting_guard_.enabled  = false;
        printf("[OMEGA-INIT] DJ30+SPX D1 turtles: shadow=1 enabled=%d/%d acct_guard=OFF (faithful KILL S-2026-06-28; sl_atr=2.0 kept)\n",
               (int)g_dj30_turtle_d1.enabled, (int)g_spx_turtle_d1.enabled);

        // ── Ger40KeltnerH1Engine (S41 2026-05-30) ───────────────────────────
        // First robust non-gold trend edge. GER40 H1 Keltner EMA20 k2.0 sl3.0,
        // bull_LB=200. Self-aggregates H1 from the GER40 tick stream via
        // feed_tick() (wired in tick_indices.hpp alongside g_ger40_turtle_h4 --
        // there is no g_bars_ger40). Warm-seed = the bundled GER40 H1 CSV
        // (5903 bars, >> the 201 the LB200 gate needs). HARD shadow.
        fflush(stdout);

        // S-2026-06-03: GoldVolBreakoutM30Engine -- XAU M30 long-only vol-breakout
        // runner ("beoff" config from the XauVolBreakout audit + lever sweep).
        // HARD shadow: novel edge, fat-tail dependent, bull-only sample. Forward-
        // log only; do NOT flip to live until the shadow ledger + a bear-inclusive
        // dataset confirm. Reuses the existing bundled H1 + M30 warmup CSVs.
        g_gold_volbrk_m30.shadow_mode = false;   // S-2026-07-01: LIVE on IBKR 4002 paper (operator all-engines cutover overrides HARD-shadow pin)
        g_gold_volbrk_m30.enabled     = true;
        g_gold_volbrk_m30.lot         = 0.01;
        g_gold_volbrk_m30.max_spread  = 0.80;   // gold $ (~80 pts)
        g_gold_volbrk_m30.init();
        g_gold_volbrk_m30.seed_h1_from_csv ("phase1/signal_discovery/warmup_XAUUSD_H1.csv");
        g_gold_volbrk_m30.seed_m30_from_csv("phase1/signal_discovery/warmup_XAUUSD_M30.csv");
        printf("[OMEGA-INIT] GoldVolBreakoutM30Engine: shadow=%d enabled=%d lot=%.2f donch=%d stop=%.1f trail=%.1f impR=%.1f\n",
               (int)g_gold_volbrk_m30.shadow_mode, (int)g_gold_volbrk_m30.enabled, g_gold_volbrk_m30.lot,
               g_gold_volbrk_m30.kDonch, g_gold_volbrk_m30.kStopAtr, g_gold_volbrk_m30.kTrailAtr, g_gold_volbrk_m30.kImpRange);
        fflush(stdout);

        // ── SessionMomentumEngine x2 (S42 2026-05-31) ───────────────────────
        // Clock-based session-window long on XAU -- first non-trend-breakout
        // edge (axis: time-of-day). Both gated by close>EMA200(h1); pure time
        // exit (no TP/SL). Self-aggregate H1 from the gold tick stream via
        // feed_tick() (wired in tick_gold.hpp). Warm-seed = bundled XAU H1 CSV
        // (12359 bars >> the 202 the EMA200 gate needs). HARD shadow. Live 16h
        // spread confirmed mean ~1.1bp p90 3.1bp << 5bp edge-death line; the
        // cost gate blocks any single wide-spread fill regardless.
        g_xau_sess_nypm.symbol           = "XAUUSD";
        g_xau_sess_nypm.label            = "XauSessNYpm_h16L4_EMA200_S42";
        g_xau_sess_nypm.entry_hour       = 16;
        g_xau_sess_nypm.hold_hours       = 4;
        g_xau_sess_nypm.use_trend_filter = true;
        g_xau_sess_nypm.ema_period       = 200;
        g_xau_sess_nypm.sl_atr           = 0.0;     // pure time exit (validated)
        g_xau_sess_nypm.skip_dow_mask    = (1 << 5); // skip Friday (weekend de-risk;
                                                     // Fri NYpm 0.79 vs 1.80 w/o, ratio)
        g_xau_sess_nypm.shadow_mode      = true;
        // S-2026-06-26s fleet-sweep KILL (workflow wn6lralw2, verify held=True):
        // bull PF1.86 / BEAR PF0.85 = bull-beta, no cross-regime edge. ENABLED
        // shadow engine bleeding the ledger -> disabled. Time-of-day axis dead on
        // spot-CFD gold (cost wall). Code kept for futures-tape revival.
        g_xau_sess_nypm.enabled          = false;
        g_xau_sess_nypm.lot              = 0.01;
        g_xau_sess_nypm.max_spread       = 2.0;     // XAU $ (≈4-5bp at $4700)
        g_xau_sess_nypm.warmup_csv_path  = "phase1/signal_discovery/warmup_XAUUSD_H1.csv";
        g_xau_sess_nypm.init();
        omega::warmup_or_die(g_xau_sess_nypm, "XauSessNYpm");

        printf("[OMEGA-INIT] SessionMomentumEngine: NYpm(h%d L%d) "
               "shadow=%d enabled=%d lot=%.2f emaP=%d filter=%d\n",
               g_xau_sess_nypm.entry_hour, g_xau_sess_nypm.hold_hours,
               (int)g_xau_sess_nypm.shadow_mode, (int)g_xau_sess_nypm.enabled,
               g_xau_sess_nypm.lot, g_xau_sess_nypm.ema_period,
               (int)g_xau_sess_nypm.use_trend_filter);
        fflush(stdout);

        // ── FxTurtleH4 cohort (2026-05-23) ──────────────────────────────────
        // Post-S99 FX rebuild: long-only Donchian H4 (Turtle archetype) on FX
        // majors. [SUPERSEDED -- see TOMBSTONE 2026-06-16 immediately below.] The
        // earlier "EUR/GBP proven OOS PF1.14-1.30" claim (walkforward_b_long_EURUSD_
        // picks.csv) did NOT survive a cross-regime recheck: the GBP sibling on
        // 2022-bear+2025-26 came in PF0.88 both-halves NEG, marking the EUR figure
        // as single-regime luck. Whole cohort culled; 0 instances instantiated.
        //
        // Warm-seed pattern: H1 CSV aggregated to H4 inline by
        // FxTurtleH4Engine::warmup_from_csv() (no offline resample needed).

        // TOMBSTONED 2026-06-16 (operator: "marginal is not good enough; if we
        // cannot improve it, it goes"). Recheck on more data (backtest/fx_turtle_recheck.cpp):
        //   EURUSD 14mo (2025-26, single regime): PF1.31 both-halves+ — but MARGINAL
        //     and only one regime (no EURUSD bear data exists in Tick).
        //   GBPUSD 18mo CROSS-REGIME (2022 H2 bear + 2025-26), IDENTICAL params/construction:
        //     PF0.88 net-$30, both-halves NEG. Full sweep lb{20,30,40,55}×tp{3,4,6}:
        //     best is flat noise (PF1.01 +$3), NO improvable plateau, every wider config worse.
        //   The identical-param sibling failing across a bear ⇒ EURUSD PF1.31 = single-regime
        //   luck, not robust edge (same tell as PeachyOrb's 2022 slice). Cannot improve ⇒ culled.


        // 2026-05-25 AtrMeanRevGrid -- forex mean-reversion grid (CRTP).
        // Strategy: ATR-normalized entry + SL, RSI confirmation, vol-adaptive
        // add distance, unified trailing SL anchored to slow EMA, configurable
        // TP modes. Shadow only until backtest validates expectancy.
        //
        // Wiring still required (next session):
        //   - on_h1_bar() dispatch from quote_loop.hpp / OHLCBarEngine.hpp at
        //     H1 bar boundaries.
        //   - on_tick() dispatch from tick_fx.hpp.
        //   - Backtest harness backtest/amr_bt/AmrBacktest.cpp (port pattern
        //     from backtest/eurusd_bt/EurusdLondonOpenBacktest.cpp).
        {
            // S-2026-06-29: FX REMOVED (operator: "we have no FX"). FxEnsemble x6 +
            // AtrMeanRevGrid boot/seed deleted -- engines were already force-disabled.
            // Globals remain inert (default enabled=false, unticked). git history < this
            // commit restores them.
            {
                // S-2026-06-29: FX SHADOW BOOK REMOVED (FxCarry x8 + FxCrossRev EURGBP +
                // FxSeasonal x8). Boot/seed/disable code deleted; globals inert. The
                // EURGBP_D1 / FX warm-seed literals are gone -> no longer in the seed audit.
                // S44 2026-05-31: IndexSeasonal (day-of-week, Tue+Fri long) -- validated
                //   equity-index edge (index_seasonal_sharpe.cpp, 6 indices, 7.4yr incl.
                //   2020 crash + 2022 bear). best-2 sleeve Sharpe 0.69 vs 0.36 buy&hold,
                //   net +40775bp > hold, maxDD lower, 40% time-in-mkt, both halves+, blk 5/6,
                //   regime-robust (survives bear + high-vol), drift-controlled (not beta).
                //   VIX term-structure gate OFF: VIX3M not on VPS (only VIX.F level), and a
                //   VIX-level gate tested WORSE -- ungated core is the edge. usd_per_pt is
                //   a rough CFD per-point value (shadow-PnL scaling only).
                {
                    // D1 INDEX-BOOK EXPOSURE CAP (IndexBookBudget) -- global concurrent-leg
                    // cap shared by IndexSeasonal + CalendarTom + CrossSectional (they bypass
                    // PortfolioGovernor, which guards only the gold/bracket zoo). observe_only=true
                    // = SHADOW: logs [IDX-BUDGET] would-block but NEVER blocks, so the >=30-trade
                    // G2 promotion gate accumulates on the true uncapped book while we measure what
                    // the cap WOULD throttle. Flip observe_only=false in the SAME change that sets
                    // the index book shadow_mode=false (live promotion) -> caps net-long concurrent
                    // legs (the correlated long cluster: TOM window x5 + Tue/Fri seasonal) to bound
                    // book drawdown. Sized to the freq/DD frontier (omega-freq-dd-frontier).
                    omega::IndexBookBudget::g().observe_only        = true;
                    omega::IndexBookBudget::g().max_net_long_legs   = 6;
                    omega::IndexBookBudget::g().max_concurrent_legs = 12;

                    // tt = Turnaround-Tuesday gate: Tue leg only after a down prior session
                    // (Mon close < Fri close). Fri->Mon leg unaffected. Validated
                    // turnaround_tuesday_bt.cpp: gated Tue BEATS unconditional (survives 2x cost
                    // where uncond goes negative bull/half1; both-regime+ incl bear PF1.29; ~halves
                    // maxDD). Roster US500+USTEC+DJ30+UK100=ON; GER40 FAILS the gate (bull -1225bp)
                    // and ESTX50 was not in the tested set -> both stay unconditional (tt=false).
                    auto idx_seas_boot = [](omega::IndexSeasonalEngine& e, double upp, bool tt, const char* warm){
                        e.shadow_mode=true; e.enabled=true; e.lot=0.01; e.p.target_vol_bps=60.0; e.p.usd_per_pt=upp;
                        e.tue_require_down=tt;
                        // VIX term-structure gate at 1.05 (Sharpe 0.69->0.80, maxDD halved). Reads
                        // data/vix_term_ratio.txt ("epoch_sec,ratio") refreshed daily by an external
                        // fetcher (tools/fetch_vix_ratio.py). Missing/stale file -> ungated (proven edge).
                        e.gate_by_vix=true; e.vix_gate_ratio=1.05; e.vix_ratio_path="data/vix_term_ratio.txt";
                        e.seed_from_d1_csv(warm);
                    };
                    idx_seas_boot(g_idx_seas_us500,  50.0, true,  "phase1/signal_discovery/warmup_US500_D1.csv");
                    idx_seas_boot(g_idx_seas_ustec,  20.0, true,  "phase1/signal_discovery/warmup_USTEC_D1.csv");
                    idx_seas_boot(g_idx_seas_ger40,  25.0, false, "phase1/signal_discovery/warmup_GER40_D1_idx.csv");
                    idx_seas_boot(g_idx_seas_dj30,    5.0, true,  "phase1/signal_discovery/warmup_DJ30_D1.csv");
                    idx_seas_boot(g_idx_seas_uk100,  10.0, true,  "phase1/signal_discovery/warmup_UK100_D1.csv");
                    idx_seas_boot(g_idx_seas_estx50, 10.0, false, "phase1/signal_discovery/warmup_ESTX50_D1.csv");
                    std::printf("[OMEGA-INIT] IndexSeasonal x6 (Tue+Fri long; Turnaround-Tue gate on US500/USTEC/DJ30/UK100) -- shadow, warm-seeded\n");

                    // S-2026-06-21: CrossSectionalIndexEngine x3 (relative-value RANKING of the
                    //   index basket -- ORTHOGONAL to the per-symbol directional book). Faithful
                    //   (cross_sectional_relval.py + xs_engine_validate.cpp, 2016-2026, 5-idx panel,
                    //   signal close[t]->fill open[t+1], 2bp/leg): MOM_LONG bull-gated PF~1.56,
                    //   MOM_LS market-neutral PF~2.03 (BEAR-positive), MR_LS bear+chop PF~1.52.
                    //   Each mode regime-gated flat in the wrong regime (asymmetric: mom skips chop,
                    //   MR skips bull). Bear coverage = market-neutral L/S, never outright shorts.
                    //   Warm-seed every symbol's D1 closes (MOM_LS needs >=471/sym): long ~10yr CSVs
                    //   (GER40 uses the long _idx variant; US500 rebuilt to 10yr). Boot MUST show 15
                    //   [SEED][XsIndex_*-<sym>] lines.
                    {
                        const char* xs_fn[5] = {
                            "phase1/signal_discovery/warmup_US500_D1.csv",
                            "phase1/signal_discovery/warmup_USTEC_D1.csv",
                            "phase1/signal_discovery/warmup_DJ30_D1.csv",
                            "phase1/signal_discovery/warmup_GER40_D1_idx.csv",
                            "phase1/signal_discovery/warmup_UK100_D1.csv"};
                        auto seed_xs = [&](omega::CrossSectionalIndexEngine& e){
                            e.shadow_mode=true; e.use_cost_guard=true; e.lot=0.01;
                            for (int i=0;i<5;++i) e.seed_from_d1_csv(i, xs_fn[i]);
                            e.enabled=true;   // shadow-only; live flip gated on >=30 shadow trades + EngineGate
                        };
                        seed_xs(g_xs_mom_long); seed_xs(g_xs_mom_ls); seed_xs(g_xs_mr_ls);
                        std::printf("[OMEGA-INIT] CrossSectionalIndex x3 (MOM_LONG/MOM_LS/MR_LS, 5-idx rank) -- shadow, warm-seeded\n");
                    }

                    // S-2026-06-21: AdaptiveTfGoldEngine -- dynamic-timeframe XAUUSD (regime
                    //   classifier selects TREND 1h-4h / RANGE 5-15m / CHOP flat). Cost-gated;
                    //   the TREND leg is where gold's edge lives, RANGE leg is cost-gated against
                    //   the spot-CFD wall. SHADOW exploratory -- live shadow ledger reveals which
                    //   states pay; faithful BT of the switching owed before any live-size.
                    {
                        g_adaptive_tf_gold.shadow_mode=true; g_adaptive_tf_gold.use_cost_guard=true;
                        g_adaptive_tf_gold.lot=0.01;
                        // DISABLED 2026-06-21: faithful BT (adaptive_tf_gold_bt.cpp, real engine on
                        // gold ticks) LOSES every state/regime -- 2022 bear ALL PF0.61 (TREND0.49/
                        // RANGE0.71), 2024-26 bull ALL PF0.77 (TREND0.80/RANGE0.75). Dynamic-TF
                        // wrapper does not beat the spot-CFD gold-intraday cost wall; TREND edge
                        // already lives in XauTrendFollow1h. Code kept (revive w/ futures data or
                        // real-H1 sub-strategy). See memory omega-adaptive-tf-gold.
                        g_adaptive_tf_gold.enabled=false;
                        std::printf("[OMEGA-INIT] AdaptiveTfGold -- DISABLED (BT loses all states/regimes)\n");
                    }

                    // S-2026-06-21: CalendarTom (TURN-OF-MONTH, last3+first3 trading days, long).
                    //   Faithful (tom_engine_validate.cpp, real engine, 2016-2026): all 5 PASS both-
                    //   WF-halves + both-regimes; book PF~1.4, STRONGER in 2022 bear (PF1.8-2.1) = a
                    //   real flows/calendar effect NOT beta (in-mkt ~24%, +ve through the 2022 selloff).
                    //   Fills the book's bear-positive gap; orthogonal to trend/MR (calendar-timed).
                    //   Reuses the IndexSeasonal warmup CSVs. ESTX50 omitted (not validated).
                    {
                        auto tom_boot = [](omega::CalendarTomEngine& e, double upp, const char* warm){
                            e.shadow_mode=true; e.enabled=true; e.lot=0.01; e.p.target_vol_bps=60.0;
                            e.p.usd_per_pt=upp; e.p.last_n=3; e.p.first_n=3;
                            e.seed_from_d1_csv(warm);
                        };
                        tom_boot(g_tom_us500, 50.0, "phase1/signal_discovery/warmup_US500_D1.csv");
                        tom_boot(g_tom_ustec, 20.0, "phase1/signal_discovery/warmup_USTEC_D1.csv");
                        tom_boot(g_tom_ger40, 25.0, "phase1/signal_discovery/warmup_GER40_D1_idx.csv");
                        tom_boot(g_tom_dj30,   5.0, "phase1/signal_discovery/warmup_DJ30_D1.csv");
                        tom_boot(g_tom_uk100, 10.0, "phase1/signal_discovery/warmup_UK100_D1.csv");
                        // S-2026-06-21b GOLD TOM: gcf_daily 2010-2026 PF1.63 both-WF-halves+ (1.24/2.23),
                        // BULL1.61/BEAR1.92 -- TOM extends to gold (we trade it heavily). usd_per_pt=100 (XAU).
                        tom_boot(g_tom_xau,  100.0, "phase1/signal_discovery/warmup_XAUUSD_D1.csv");
                        std::printf("[OMEGA-INIT] CalendarTom x6 (turn-of-month long, +XAU) -- shadow, warm-seeded\n");
                    }

                    // S-2026-07-08c: GoldTsmomD1V2 -- gold deep-dive candidate #2 (Study 4,
                    //   outputs/GOLD_DEEP_DIVE_2026-07-08.md, evidence commit 4bca1036). D1 TSMOM
                    //   composite {42,63,84}d, vol-targeted min(2, 15%/realized20d), BOTH directions
                    //   (the one gold structure NET SHORT PROFITABLY through 2022: +129pt validated /
                    //   +117pt wired rule -- do NOT long-gate the shorts), monthly rebalance.
                    //   Validated cell (gdd_tsmom_cot.py chassis, GC=F daily 2015-2026, 0.31pt/
                    //   turnover): PF2.26 +2915pt n120 maxDD -413 2x-cost PF2.25. WIRED rule =
                    //   calendar-month rebalance (restart-proof), re-run same harness/data: PF2.09
                    //   +2689 n121 both-halves+ 2022 +117 2x PF2.08 maxDD -491; engine==python
                    //   parity EXACT (n129 +2604.1pt PF1.98 full warm window). 12m lookback is the
                    //   WORST cell (PF1.27, 2022 -540) -- the {42,63,84} band is load-bearing.
                    //   Allocation book: n(stat unit)=rebalance periods, ~10 adjustments/yr.
                    //   Auto-retirement latch -1000pt (2x worst wired-rule DD). SHADOW, 0.01 lot.
                    {
                        g_gold_tsmom_d1.enabled     = true;
                        g_gold_tsmom_d1.shadow_mode = true;
                        g_gold_tsmom_d1.lot         = 0.01;   // 0.01 XAU lot per weight-unit ($1/pt at |w|=1)
                        g_gold_tsmom_d1.seed_from_d1_csv("phase1/signal_discovery/warmup_XAUUSD_D1.csv");  // prints the [SEED] line
                        g_engine_heartbeat.register_engine("GoldTsmomD1V2", g_gold_tsmom_d1.enabled, 3600, 0, 24);
                        std::printf("[OMEGA-INIT] GoldTsmomD1V2 (D1 TSMOM {42,63,84} vol-targeted, both directions) -- shadow, warm-seeded\n");
                    }

                    // S-2026-06-03: GoldSeasonal (XAUUSD early-week long, Mon+Tue). The one
                    //   new gold edge found after exhausting price/book signals — calendar
                    //   axis. +24.5%/yr Sharpe 1.88 engine-driven on M5 (real 21:00 daily
                    //   break), win 59-61%, +ve every year, both WF halves+, cost-robust 5x,
                    //   DSR-survives. Long-only; exits on UTC day-flip; spread-guard skips
                    //   break/illiquid fills so the position holds through the daily break.
                    //   Risk-gate OFF (gold often does BEST risk-off). usd_per_pt=100 (XAU).
                    {
                        // S-2026-06-17 CULLED: ledger_analytics ranked flag --
                        // n=17 NEG-EXPECTANCY + COST-FRAGILE (58% of gross eaten by
                        // cost, expR -0.00). Calendar churn with no edge net of
                        // spread; flagged repeatedly in memory. Disabled.
                        std::printf("[OMEGA-INIT] GoldSeasonal -- CULLED 2026-06-17 (neg-expectancy, cost-fragile); class type-only, NOT loaded\n");
                    }

                    // S-2026-06-03: GoldOversoldBounce (XAUUSD daily RSI<30 capitulation
                    //   bounce). Mean-reversion — buys deep-oversold weakness, exits on RSI
                    //   recovery (>50) / 20-day cap / -2.5*ATR stop. 18yr GC=F (incl 2013
                    //   bear): t2.76 PF2.17 win73%, 14/19yr+, POSITIVE in bear windows where
                    //   the naive below-50ma dip-buy dies (falling-knife). Uncorrelated with
                    //   the trend/breakout book + GoldSeasonal. Long-only. usd_per_pt=100.
                    {
                        // S-2026-07-08c: printf de-staled -- said "RE-CHECK candidate" but CULL_LEDGER
                        // 2026-06-16 already holds the faithful x-regime verdict (+419/-455/+45,
                        // bull-beta dip-buyer dies in crash, gate-incompatible). C1Retuned's 2026-07-08
                        // bear-tape BT (PF0.54-0.59 gated, halt trip) re-confirmed the same family verdict.
                        std::printf("[OMEGA-INIT] GoldOversoldBounce -- class type-only, NOT loaded (CULLED 2026-06-16 faithful x-regime; gate-incompatible dip-buyer)\n");
                    }

                    // S44: IndexFomc (pre-FOMC drift, US indices). Long the trading day
                    //   before a scheduled FOMC announcement, exit FOMC-day close. Decayed
                    //   but alive (+11.8bp/event 2023-26, index_validate2.cpp). Respects the
                    //   portfolio risk-gate. usd_per_pt = shadow-PnL scaling only.
                    auto idx_fomc_boot = [](omega::IndexFomcEngine& e, double upp, const char* warm){
                        e.shadow_mode=true; e.enabled=true; e.lot=0.01; e.p.target_vol_bps=60.0; e.p.usd_per_pt=upp;
                        e.seed_from_d1_csv(warm);
                    };
                    idx_fomc_boot(g_idx_fomc_us500, 50.0, "phase1/signal_discovery/warmup_US500_D1.csv");
                    idx_fomc_boot(g_idx_fomc_ustec, 20.0, "phase1/signal_discovery/warmup_USTEC_D1.csv");
                    idx_fomc_boot(g_idx_fomc_dj30,   5.0, "phase1/signal_discovery/warmup_DJ30_D1.csv");
                    std::printf("[OMEGA-INIT] IndexFomc x3 (US, pre-FOMC long) -- shadow, warm-seeded\n");
                }
            }

            // ----------------------------------------------------------------
            // 2026-05-26: Index AMR. Configs from deep eval sweep on tick CSVs.
            //   US500   H1  X=8  SL_Y=6 ATR_FROM_WAP  PF 1.75 +$81  DD $32 Recov 2.56
            //   NAS100  M15 X=14 SL_Y=4 RSI_OR_MA     PF 1.55 +$15  DD $10 Recov 1.48
            //   GER40   M15 X=14 SL_Y=6 ATR_FROM_WAP  PF 1.86 (stage-4)
            // shadow_mode=true. Warmup CSV per CLAUDE.md "Engine Warm-Seed Mandate".
            // ----------------------------------------------------------------
            const char* warmup_us500  = "phase1/signal_discovery/warmup_US500_H1.csv";
            (void)warmup_us500;
            const char* warmup_nas100 = "phase1/signal_discovery/warmup_NAS100_H1.csv";
            (void)warmup_nas100;
            const char* warmup_ger40  = "phase1/signal_discovery/warmup_GER40_H1.csv";
            (void)warmup_ger40;




            std::printf("[OMEGA-INIT] AtrMeanRevGrid INDEX: US500(H1,X=8)+NAS100(M15,X=14)+GER40(M15,X=14) enabled (shadow)\n");
        }

        // [STALE-CORRECTED 2026-06-23 audit] AUD/NZD/JPY do NOT trade -- and the
        // reason is NOT "awaiting CSVs". The warmup CSVs (warmup_{AUDUSD,NZDUSD,
        // USDJPY}_H1.csv, ~347/347/330 days) ALREADY EXIST in phase1/signal_discovery/.
        // The real reason: the ENTIRE FxTurtleH4 cohort was TOMBSTONED 2026-06-16
        // (see L1866-1874) -- GBPUSD cross-regime (2022 bear + 2025-26) PF0.88
        // both-halves NEG; EURUSD PF1.31 judged single-regime luck. The cohort's
        // tick_fx dispatch is gutted to a no-op stub and NO FxTurtleH4 global is
        // instantiated. So EUR/GBP are NOT active either -- "EUR+GBP active" below
        // was stale. Do NOT enable AUD/NZD/JPY: it would resurrect a dead strategy
        // on a pair where the validated sibling already failed cross-regime.
        // FX majors currently have NO viable engine (consistent with the FX dead-end:
        // session-open=neg-exp/S99, AMR grid=culled, scalp=retired, turtle=tombstoned).



        printf("[OMEGA-INIT] FxTurtleH4 cohort: TOMBSTONED 2026-06-16 (GBP x-regime PF0.88) -- 0 instances live; AUD/NZD/JPY stay off (dead strategy, not missing CSVs)\n");

        // 2026-05-20 mega_sweep2 candle batch (3 D1 patterns)
        // CULL S-2026-06-17 (retest campaign): xau_d1_zoo_audit real-class
        // Sharpe=+0.18 (n=46) vs claimed 3.00 = 16.9x inline inflation.
        // Marginal no-edge (<0.5). Disabled. Reversible: shadow, lot 0.01.

        // CULL S-2026-06-17 (retest campaign): xau_d1_zoo_audit real-class
        // Sharpe=+0.30 (n=34) vs claimed 3.00 = 9.9x inline inflation.
        // Marginal no-edge (<0.5). Disabled. Reversible: shadow, lot 0.01.

        // 2026-05-27 S57: DISABLED -- regime split HIGH Sharpe -0.31 (slight
        // negative in high-vol regime, 12/51 trades). Operator strict policy.
        fflush(stdout);

        // TrendLineBreakEngine -- validated hull-break (2026-06-09). SHADOW only.
        fflush(stdout);

        // TrendLineBreakEngine FX -- GBPUSD PF1.53 + USDJPY PF1.37 (the real edge).
        //   SHADOW (sim-only): enabled=true so signals fire + ledger, but shadow_mode
        //   means no broker orders. H4-bar driven from tick_fx.hpp dispatch; on_tick
        //   runs the intrabar safety-line stop. Seeded from H4 warmup CSVs below.

        // DISABLED 2026-06-11 (operator: kill the open USDJPY trade, not viable).
        // Was the only open USDJPY position on the live dashboard (stale scratch,
        // SL-only/no-TP runner). Disabling stops new USDJPY trendline entries and
        // drops the open virtual position on next restart (shadow = no broker
        // order to close). Sister TrendLineBreakGBP left enabled (operator scoped
        // this to USDJPY); revisit if the GBP scratch is also unwanted.
        fflush(stdout);

        // ── GoldD1TrendState (2026-05-21) -- regime gate for shorts/longs.
        //   Seeded from XAU H4 CSV. Updated on every H4 close in tick_gold.hpp.
        //   Queried by bidirectional engines (XauTrendFollow2h InsideBar,
        //   DonchianBreakout short path) before firing direction-dependent entries.
        // 2026-06-23 stale-seed fix (mirrors gold_regime): restore live-accurate D1-trend state
        // across restarts first; only warm-seed from the (stale-prone) H4 CSV on a true cold start.
        // start_recording() so it self-captures live H4 going forward.
        {
            omega::gold_d1_trend().set_live_dump(log_root_dir() + "/gold_d1_trend_h4.csv");
            if (!omega::gold_d1_trend().load_state(log_root_dir() + "/gold_d1_trend_state.dat"))
                omega::gold_d1_trend().seed_from_h4_csv("phase1/signal_discovery/warmup_XAUUSD_H4.csv");
            omega::gold_d1_trend().start_recording();
        }
        fflush(stdout);

        // ── WARM SEED ALL 2026-05-20 NEW ENGINES FROM H4 CSV ────────────────
        // Replays ~3216 historical H4 bars (~134 days) through each engine to
        // populate internal ATR / EMA / Donchian / candle-history state.
        // Engines are momentarily disabled during seeding so no signals fire
        // on historical bars. After seeding they are HOT — can produce first
        // signal on the next live H4/D1 close instead of waiting 14-100 days
        // cold-warmup. Critical: without seed, deploy-day -> first signal gap
        // is impractical for D1 engines (e.g. PullbackContD1 needs EMA100 =
        // 100 daily bars from cold start).
        {
            const std::string seed_csv = "phase1/signal_discovery/warmup_XAUUSD_H4.csv";
            omega::seed_h4_engine(g_xau_swing_break_d1,   seed_csv, "XauSwingBreakD1");
        }
        fflush(stdout);

        // ── TrendLineBreak FX warm-seed (GBPUSD + USDJPY H4 CSVs) ───────────
        //   Separate CSVs per symbol (hull geometry uses an internal bar counter;
        //   seeding fires no entries while enabled is toggled off by the helper).
        fflush(stdout);

        // ── XauTrendFollow2hEngine (S33k 2026-05-11) ─────────────────────────
        // 4-cell 2h trend-follow ensemble built from Pass-8 deep_dive. Same
        // XAU trend regime as the 4h/D1 engines, denser cadence (~25
        // trades/month). Cells: Keltner K=2.0, Donchian N=20, Donchian N=50,
        // InsideBar -- all sl2.0_tp4.0, all 3/3 Duka years +ve.
        // Synthesises 2h bars internally from H1 stream.
        // 2026-05-15 (S96): 154M-tick fresh-tape backtest (all 4 2h cells profitable):
        //   Keltner PF=1.44, Donch20 PF=1.17, Donch50 PF=1.37, InsideBar PF=1.25.
        //   2h aggregate: 826 trades, PF=1.29. All cells enabled.
        // 2026-05-27 S52: DISABLED -- real-class audit (xau_trendfollow_audit)
        // confirmed Sharpe +1.14 over 869 trades BUT mdd_to_gross=75% (-32.52
        // / +43.08), avg only +$0.05/trade. High variance, thin edge. Streak
        // DD (10+ losers stacked, worst single -3.13). Coverage redundant:
        // XauTrendFollow1h (Sharpe +3.58 mdd 27%) + XauTrendFollow4h (+1.97
        // mdd 25%) cover same trend signals with clean DD profile. Off.
        // S88-followup 2026-05-27: REVIVED to HARD-SHADOW with ADX gate.
        // Full Duka 2yr backtest (XauTrendFollowBacktest --adx):
        //   Baseline (ungated): n=826 PF=1.29 net=$4763 MaxDD=$1108
        //   +ADX25:              n=597 PF=1.42 net=$4874 MaxDD=$ 738 (+10% PF, -33% DD)
        // Vol_band did NOT help 2h (PF dropped). ADX25 is the right gate
        // for this timeframe -- trend-strength threshold removes weak-trend
        // entries where the cell signal fires but no follow-through.
        // S52 disable was on UNGATED entries (Sharpe 1.14, mdd 75%); ADX
        // filtered backtest may flip the DD profile. 60+ days HARD shadow
        // before live promotion.
        // S88-followup post-sweep 2026-05-27: per-cell gate masks. The
        // engine-wide ADX25 hurt 2h_Donch50 specifically (PF 1.37 -> 1.22).
        // Per-cell breakdown:
        //   Keltner (bit 0): baseline PF 1.44 -> +ADX25 1.67  -> ADX
        //   Donch20 (bit 1): baseline PF 1.17 -> +ADX25 1.33  -> ADX
        //   Donch50 (bit 2): baseline PF 1.37 -> +ADX25 1.22  -> VolBand 1.54
        //   InsideBar (bit 3): baseline PF 1.25 -> +ADX25 1.45 -> ADX
        // Apply ADX to bits {0,1,3} = 0xB; vol_band to bit {2} = 0x4.
        // Stacked vol+ADX hurt (tested separately); per-cell exclusive is right.
        // S-2026-07-12 DISABLED (operator cull order: "known engines that fail — disable NOW").
        // GOLD_PHASE1B true-cost re-run (6bp+slip tiers, outputs/GOLD_PHASE1B_2026-07-11.md):
        // prod config NET -$4,542; killer = LOSS_CUT 0.5% on the H1-fed harness, but even the
        // best LC-off variant fails both-halves+2022. Recommend route-to-MGC-2h (backtest first).
        g_xau_tf_2h.shadow_mode = false;
        g_xau_tf_2h.enabled     = false;
        // S-2026-06-17 cold-loss protection. Faithful M30->H2 backtest: LC=0.5%
        // -> net +189, maxDD -66% (-802->-269), worst -238->-27 (strongest DD cut
        // of the cluster). See backtest/losscut_xau_faithful.py.
        g_xau_tf_2h.LOSS_CUT_PCT = 0.5;
        g_xau_tf_2h.use_adx_gate     = true;
        g_xau_tf_2h.adx_min          = 25.0;
        g_xau_tf_2h.cell_adx_mask    = 0xB;       // Keltner, Donch20, InsideBar
        g_xau_tf_2h.use_vol_band_gate= true;
        g_xau_tf_2h.vol_band_low_pct = 0.30;
        g_xau_tf_2h.vol_band_high_pct= 0.85;
        g_xau_tf_2h.cell_vol_band_mask = 0x4;     // Donch50 only
        g_xau_tf_2h.lot         = 0.01;
        g_xau_tf_2h.max_spread  = 1.0;
        g_xau_tf_2h.warmup_csv_path = "phase1/signal_discovery/warmup_XAUUSD_H1.csv";
        g_xau_tf_2h.init();
        omega::warmup_or_die(g_xau_tf_2h, "XauTrendFollow2h");
        printf("[OMEGA-INIT] XauTrendFollow2hEngine initialised: shadow=%d enabled=%d lot=%.2f cells=4"
               " (Keltner,Donchian20,Donchian50,InsideBar)\n",
               (int)g_xau_tf_2h.shadow_mode, (int)g_xau_tf_2h.enabled, g_xau_tf_2h.lot);
        fflush(stdout);

        // ── XauThreeBar30mEngine (S34 b1932d2 / S35-P3 retrofit 1684cfc /
        //    S35-P4 backtest 3ee31de) ──────────────────────────────────────
        // M30 three-bar continuation on XAU. Per-year cross-validation
        // backtest (S35-P4 commit 3ee31de) on M30 bars aggregated from the
        // 2024-03..2026-04 M15 dataset (fvg_phase0/XAUUSD_15min/...) shows:
        //
        //   Baseline    n= 727  WR=35.1%  net=+$551.79  PF=1.07  DD=$508.96
        //   TUNED       n=1058  WR=66.2%  net=+$1488.60 PF=1.27  DD=$282.83
        //   Strict-all  n=  74  WR=50.0%  net=-$10.90   PF=0.95  killswitch
        //
        // All three years positive in TUNED (2024 +$199, 2025 +$242,
        // 2026-partial +$1047). Production config below mirrors TUNED:
        // BE arm at +1*ATR favourable, trail SL at 0.75*ATR after BE, ATR
        // floor $0.30 (filter dead tape), $1 spread cap. Killswitch,
        // daily-cap, time-stop, and session-window block disabled until
        // shadow-live data refines their calibration -- the strict S35-P3
        // defaults trip prematurely on a 35%-native-WR baseline.
        //
        // HARD shadow_mode = true regardless of kShadowDefault until the
        // operator confirms ~1 month of shadow-live trade trace matches
        // the M15->M30 backtest expectations on the live broker tape.
        // After that confirmation, flip to kShadowDefault here.
        //
        // REQUIRES tick_gold.hpp to dispatch M30 bar closes to
        // g_xau_threebar_30m.on_30m_bar(...) and gold ticks to
        // g_xau_threebar_30m.on_tick(...). Until that wiring lands the
        // engine is instantiated but DORMANT (receives no bars, never
        // fires). Wiring is a separate commit; see HANDOFF_S35.md
        // Phase 4 for the dispatch sketch.
        //
        // 2026-05-14 (part W): S63 VWR-pattern in-flight protection — explicit
        //   re-affirm of the class defaults (XAU-scaled) for grep visibility.
        //   Mirrors g_vwap_rev_ger40 precedent at engine_init.hpp:649-651 and
        //   g_ustec_tf_5m at engine_init.hpp:976-978: the override matches the
        //   class default but documents intent and makes activation
        //   discoverable from engine_init.hpp alone. See
        //   outputs/S63_STATE_CLASSIFICATION_2026-05-14.md §3.5 for the audit
        //   that confirmed STATE A via class-default route (fields declared
        //   at XauThreeBar30mEngine.hpp:240-242; mgmt-path at L479-498 fires
        //   every tick during pos manage).
        //
        //   LOSS_CUT_PCT  = 0.05  -> XAU@3700: ~$1.85 cold-loss cut.
        //   BE_ARM_PCT    = 0.03  -> XAU@3700: ~$1.11 mfe arms ratchet.
        //   BE_BUFFER_PCT = 0.012 -> XAU@3700: ~$0.44 buffer (typical XAU spread).
        g_xau_threebar_30m.LOSS_CUT_PCT  = 0.05;
        g_xau_threebar_30m.BE_ARM_PCT    = 0.03;
        g_xau_threebar_30m.BE_BUFFER_PCT = 0.012;
        // 2026-05-15 (S96): 154M-tick fresh-tape backtest v2 (long-only):
        //   IS PF=1.45, OOS PF=1.24, 155 OOS trades, WR=36.1%, decay 15%.
        //   Short side PF=0.84 in v1 — removed. S63 in-flight protection
        //   already wired (LOSS_CUT=0.05, BE_ARM=0.03, BE_BUFFER=0.012).
        // S88-followup 2026-05-27: promote to HARD-SHADOW with two new
        // structural gates (slope_12 + vol_band). The 2026-05-17 disable
        // (PF=0.75) was based on a parameter sweep over the existing knobs
        // (BE_ARM, trail_atr, atr_floor); my evidence is STRUCTURAL — adding
        // a close-slope-direction filter and an ATR-percentile band, which
        // are not the same axis as the prior sweep.
        //
        // C++ harness on 6mo PKL (research/cpp_threebar_gates_summary.txt):
        //   TUNED baseline:    n=138 net=+$596  PF=1.55  MaxDD=$218
        //   TUNED+SLOPE12:     n=127 net=+$660  PF=1.74  MaxDD=$168
        //   TUNED+VOLBAND:     n= 77 net=+$545  PF=2.23  MaxDD= $97
        //
        // VOLBAND removes dead-vol (no follow-through) and crisis-vol
        // (gap-through-SL) regimes; SLOPE12 removes counter-trend 3-bar
        // fires. Both orthogonal, ANDed when stacked. Shadow A/B 60+ days
        // before flipping enabled=true.
        g_xau_threebar_30m.shadow_mode        = false;  // S-2026-07-01: LIVE on IBKR 4002 paper (operator all-engines cutover)
        g_xau_threebar_30m.enabled            = true;   // RESURRECTED-SHADOW 2026-06-18 (cull-audit): the 2026-06-15 "-$371 6mo shadow-book" cull was POLLUTED basis (same batch as wrongly-killed GoldOrb). Faithful re-check (backtest/threebar30m_xau_S35P3_backtest.cpp, production engine M30, 2024-26): PF1.29 n365, ALL YEARS POSITIVE — NOT a net loser. CAVEAT: bull-only window (no 2022 bear) + 2025-concentrated + long-only -> SHADOW-CANDIDATE, bear-test owed before any live size. shadow_mode=true above. See AUDITED_CONFIGS.tsv + CULL_LEDGER.tsv.
        g_xau_threebar_30m.long_only          = true;   // S96: short side no edge
        g_xau_threebar_30m.lot                = 0.01;
        g_xau_threebar_30m.max_spread         = 1.0;
        g_xau_threebar_30m.be_trigger_atr     = 1.0;    // S35-P4 TUNED
        g_xau_threebar_30m.be_cost_buffer_pts = 0.10;
        g_xau_threebar_30m.trail_after_be     = true;   // S35-P4 TUNED
        g_xau_threebar_30m.trail_atr_mult     = 0.75;
        g_xau_threebar_30m.min_atr_floor      = 0.30;   // S35-P4 TUNED
        g_xau_threebar_30m.max_bars_held      = 0;      // disabled
        g_xau_threebar_30m.daily_loss_limit   = 0.0;    // disabled (strict tripped easily)
        g_xau_threebar_30m.max_consec_losses  = 0;      // disabled (strict tripped at 5)
        g_xau_threebar_30m.max_atr_ceil       = 0.0;    // disabled
        g_xau_threebar_30m.block_hour_start   = -1;     // disabled (XAU Asia has flow)
        g_xau_threebar_30m.block_hour_end     = -1;
        // S88-followup gates (slope_12 + vol_band; HMM stays OFF — structural
        // redundancy with engine pre-filters, see XauThreeBar30mEngine.hpp:267).
        g_xau_threebar_30m.use_slope_gate      = true;
        g_xau_threebar_30m.slope_lookback_bars = 12;
        g_xau_threebar_30m.use_vol_band_gate   = true;
        g_xau_threebar_30m.vol_band_low_pct    = 0.30;
        g_xau_threebar_30m.vol_band_high_pct   = 0.85;
        g_xau_threebar_30m.warmup_csv_path = "phase1/signal_discovery/warmup_XAUUSD_M30.csv";
        g_xau_threebar_30m.init();
        omega::warmup_or_die(g_xau_threebar_30m, "XauThreeBar30m");
        printf("[OMEGA-INIT] XauThreeBar30mEngine initialised: shadow=%d enabled=%d lot=%.2f"
               " be_trig=%.2f*ATR trail=%.2f*ATR atr_floor=%.2f"
               " (S35-P4 TUNED; S36-P4 M30 dispatch wired in tick_gold.hpp 2026-05-12)\n",
               (int)g_xau_threebar_30m.shadow_mode,
               (int)g_xau_threebar_30m.enabled,
               g_xau_threebar_30m.lot,
               g_xau_threebar_30m.be_trigger_atr,
               g_xau_threebar_30m.trail_atr_mult,
               g_xau_threebar_30m.min_atr_floor);
        fflush(stdout);

        // ── GoldUltimateEngine (S91 2026-05-15) ──────────────────────────
        // Standalone v12 OOS-validated XAUUSD trend engine. 7-factor entry
        // filter + edge-hour gate (01/05/23 UTC) + ATR floor 2.5. Self-
        // contained 1-min bar aggregation, indicators, signal generation,
        // and position management. No dependency on other engines.
        //
        // 26-month backtest (154M ticks, Mar 2024 – Apr 2026):
        //   PF=1.36  WR=41.8%  Sharpe=8.30  311 trades
        //   BULL PF=1.45  BEAR PF=1.29
        //   OOS PF=1.39 (265 trades, Sep 2025 – Apr 2026)
        //   OOS PF retention: 117% — STRONG PASS
        //
        // Geometry: SL=2.0*ATR, TP=5.0*ATR, trail at 3.0*ATR MFE with
        // 2.0*ATR distance. No break-even, no profit lock.
        //
        // Shadow mode for initial live validation. User instruction:
        // "disable ALL the other gold engines, we only test this new one"
        fflush(stdout);

        // ── UstecTrendFollowHtfEngine (S35-P6 + S36-P1a + S36-P1b 2026-05-12) ─
        // Multi-timeframe USTEC.F trend-follow ensemble. 2 cells across H1/H4,
        // VERIFIED 2026-05-12 under S36-P1a-verify wrapped re-backtest on
        // 16mo NSXUSD HISTDATA (every period positive after S36-P1b drop):
        //
        //   [C] 1h  ATR_Mom mom=50;atr=0.2-0.8 sl2.0_tp4.0   * engine workhorse
        //   [E] 4h  Stochastic lo=20;hi=80     sl2.0_tp4.0   * cleanest cell
        //
        // Engine wraps the bare cells with engine_protections.hpp (BE arm,
        // trail-after-BE, ATR floor).
        //
        // S36-P1a-verify 2-cell wrapped backtest (89.7M ticks, 16mo HISTDATA):
        //   2025H1     n=??    net=+$14,173.72   (subset of n=473 3-cell run)
        //   2025H2     n=??    net=+$ 5,084.20
        //   2026       n=??    net=+$   675.24   (positive after cross-cell offset)
        //   ALL        n=953   net=+$19,933.15   WR ~60%  PF ~1.20+
        //   (per-cell within the 3-cell verify run: AtrMom1h +$11,603 ALL,
        //   Stoch4h +$8,329 ALL.)
        //
        // S36-P1b: InsideBar2h dropped after S36-P1a-verify revealed:
        //   - InsideBar2h was -$898 ALL in the 3-cell verify (vs +$2,287 in
        //     the 5-cell baseline) -- cells are NOT independent.
        //   - InsideBar2h 2026 was -$4,420 on n=43 (-$103 per trade) and
        //     dragged the 2026 period total to -$3,745.
        //   - With InsideBar2h removed: 2026 = AtrMom1h(-$1,784) +
        //     Stoch4h(+$2,459) = +$675 positive, satisfies operator directive.
        //
        // S36-P1a drop history (5-cell -> 3-cell):
        //   Stoch1h     2025H1 -$1,115 -- DROPPED S36-P1a (any-period directive)
        //   Donch15m    ALL    -$4,002 -- DROPPED S36-P1a (any-period directive)
        //
        // Existing UstecTrendFollow5mEngine cells (M5 Donchian + Keltner)
        // failed the 3-period test on this dataset (M5 Donchian was
        // -$2761/-$2420/-$621). This HTF engine is a COMPANION, not a
        // replacement; the M5 engine is untouched per operator instruction.
        //
        // HARD shadow_mode = true regardless of kShadowDefault until ~1 month
        // of shadow-live trade trace confirms the M15-aggregate backtest on
        // the live broker tape.
        //
        // M15 dispatch wired in tick_indices.hpp on 2026-05-12 under S36-P4
        // (commit b6e9495). Engine receives M15 bars and per-tick management.
        // S37 audit (2026-05-27): RE-ENABLED in HARD shadow. The S94 disable
        // citing replacement by Nas100ShortEngine was load-bearing on a ghost
        // -- the class header existed (include/Nas100ShortEngine.hpp) but
        // was never instantiated as a global, never dispatched, never built.
        // Net effect of S94: zero NAS/USTEC short coverage running. This
        // engine had been S35-P3 OOS-validated on 16mo of NSXUSD ticks
        // (+$11733 net, PF 1.05) and ships shadow-only here so it can be
        // observed and promoted on real evidence rather than on an
        // unimplemented "replacement". Promote to enabled=true && shadow_mode=false
        // only after ≥30 shadow trades w/ WR ≥35% net positive after costs.
        fflush(stdout);

        // ?? EmaPullbackPortfolio -- Tier-3 ship 2026-04-30 ?????????????????????
        // 4 ema_pullback long cells: H1, H2, H4, H6. Long-only -- shorts not
        // profitable in master_summary post-cut.
        // Verdict: phase1/signal_discovery/POST_CUT_FULL_REPORT.md
        // Combined: 796 trades/yr, +$4,006/yr/unit. Family A (sim_a) semantics.
        // 9/21 EMA pullback-and-recover pattern from sig_ema_pullback.
        // Reuses tsmom warmup CSV (same H1 stream input).
        // 2026-05-15 (S96): 154M-tick fresh-tape backtest v2 (H4+H6 only):
        //   H4 PF=1.60, H6 PF=1.53 (150+100 trades). H1 PF=1.13, H2 PF=1.27 dilute.
        //   v2 combined: IS PF=1.60, OOS PF=1.54, 91 OOS trades, WR=39.6%.
        //   S63 already active (LC=0.10, ARM=0.40, BUF=0.05 per S82 sweep).
        //   cell_enable_mask = bits 2,3 = 0x0C (H4+H6 only, disable H1+H2)
        // S63 in-flight protection -- state A (LC + BE_RATCHET, evidence-backed).
        //
        // History:
        //   S75 (69a2f83) -- wired hooks into EpbCell::on_tick + propagation
        //                    through EpbPortfolio::init(). Call-site at 0.0.
        //   S76           -- activated (1.0, 0.40, 0.05) reasoned from one
        //                    shadow trade. (No backtest evidence.)
        //   S77           -- created backtest/EmaPullbackBacktest.cpp harness
        //                    with 3x3x3 sweep mode.
        //   S78           -- discovered S75/S76 wired S63 into on_tick only.
        //                    EmaPullback is bar-driven; harness drives
        //                    on_h1_bar in lockstep with live engine's bar-end
        //                    management; on_tick never called in backtest.
        //                    Fix: replicate the BE_RATCHET + LOSS_CUT block
        //                    in EpbCell::on_bar with bar-extremes / mfe_pre
        //                    semantics. Both paths now exercise S63 correctly.
        //   S79           -- revert call-site to 0.0 per S78-enabled wide
        //                    sweep, which showed every LC>0 cell (LC ∈ {0.5,
        //                    1.0}) net-negative vs baseline.
        //   S80           -- activate (0.0, 0.40, 0.05) -- BE-only protection
        //                    from the wide sweep best-PF cell.
        //   S81           -- harness --sweep grid retuned to TIGHT LC zone
        //                    matching what other XAU engines actually use
        //                    (XauusdFvg/XauThreeBar30m=0.05%, PDHL=0.04%,
        //                    IndexFlow=0.07-0.08%). Original wide grid moved
        //                    to --wide-sweep flag for back-comparison. The
        //                    operational band for similar engines was
        //                    6-20x tighter than what S77 originally tested.
        //   S82 (this)    -- activate LC=0.10/ARM=0.40/BUF=0.05 per the S81
        //                    tight-zone sweep, which surfaced a non-monotonic
        //                    PF curve with a clear peak at LC=0.10%.
        //
        // Sweep evidence (S77 harness, S81 tight grid; 27 cells, 3x3x3 over
        //  LC ∈ {0.0, 0.05, 0.10}, ARM ∈ {0.0, 0.20, 0.40},
        //  BUF ∈ {0.0, 0.025, 0.05}
        //  on phase1/signal_discovery/tsmom_warmup_H1.csv, 6,156 H1 bars,
        //  Apr 2025 - Apr 2026 XAUUSD validation corpus):
        //
        //   Cell                          trades   PF      gross    max_dd
        //   ---------------------------------------------------------------
        //   (0.0,  0.0,  0.0)  baseline    475   1.4675   $7,204   -12.70%
        //   (0.0,  0.40, 0.05) S80         542   1.4904   $5,777   -12.18%
        //   (0.10, 0.40, 0.05) S82 ACTIVE  643   1.6194   $3,380    -5.45%
        //
        //   Wide-sweep LC values, for context (S77, --wide-sweep grid):
        //   (0.5,  0.40, 0.05)             555   1.2794   $3,333   -16.25%
        //   (1.0,  0.40, 0.05) S76         544   1.3574   $4,329   -14.25%
        //
        //   PF curve as a function of LC (with ARM=0.40, BUF=0.05 fixed):
        //     LC=0.00 : 1.490   (S80, BE-only)
        //     LC=0.05 : 1.295   (too tight, catches noise; LC fires 614x/682)
        //     LC=0.10 : 1.619   (S82, peak)
        //     LC=0.50 : 1.279   (loose; LC fires only 143x/555)
        //     LC=1.00 : 1.357   (very loose; LC fires only 41x/544)
        //
        //   The curve is NON-MONOTONIC. The S77 wide grid skipped over the
        //   peak (only tested LC ∈ {0, 0.5, 1.0}); the S81 tight grid found
        //   it. Lesson logged: test the operational band of similar engines
        //   in the codebase, not arbitrary coarse grids.
        //
        // S82 activation rationale (vs baseline):
        //   PF        : 1.6194 vs 1.4675 baseline -> +0.152  (big improvement)
        //   max_dd    : -5.45% vs -12.70% baseline -> -7.25pp (57% reduction)
        //   SL_HIT    : 0   vs 295 baseline (LC catches every loser first)
        //   LOSS_CUT  : 507 firings, ~$24 per cut (vs ~$45 SL hits replaced)
        //   BE_CUT    : 63 firings (smaller layer on top)
        //   TP_HIT    : 72  vs 177 baseline (cost: 105 winners cut early)
        //   gross_pnl : $3,380 vs $7,204 baseline (-$3,824, -53% absolute)
        //
        //   This is the protection profile the operator asked for: bad
        //   trades cut quickly, drawdown shrinks dramatically. Costs ~half
        //   the absolute return for the risk reduction. Same family as
        //   what XauusdFvg / XauThreeBar30m / PDHL run at the call-site
        //   level (LC ∈ 0.04-0.05%), just calibrated at the EmaPullback-
        //   specific operational peak (LC = 0.10%).
        //
        // To revisit: re-run EmaPullbackBacktest --sweep first. The
        // operational peak may shift with regime/tape changes. Engine code
        // (on_bar + on_tick S63 management blocks) is single-sourced --
        // no code change to tune; just edit these three lines.
        fflush(stdout);

        // ?? TrendRiderPortfolio -- Tier-4 ship 2026-04-30 ?????????????????????
        // 6 trend-rider cells: H2 L+S, H4 L+S, H6 L, D1 L.
        // 40-bar Donchian breakout entry + stage trail (1.5N init SL,
        // trail at 2N->1.5N, 5N->2.5N, 10N->3.5N), NO TP, NO time exit.
        // Validation source: 1-year H1/H2/H4/H6/D1 corpus.
        //   H2 long  +$3,705/yr  H2 short +$  943/yr
        //   H4 long  +$5,051/yr  H4 short +$2,637/yr
        //   H6 long  +$4,817/yr  D1 long  +$2,479/yr
        //   Total: ~$19,633/yr at 0.05 lot baseline; ~$39K at 0.10 cap.
        //
        // CONVICTION-TIERED SIZING -- Option B (~1/8 Kelly) (2026-04-30 PM):
        //   risk_pct=0.040 (4.0%) and max_lot_cap=0.50 are 8x the baseline used
        //   by tsmom/donchian/ema_pullback (each at 0.005 / 0.05).
        //   Justification: per-cell full-Kelly fractions on the validation
        //   data run 18-76% (avg ~44%). Practitioner standard is 1/4 to 1/8
        //   Kelly to handle backtest noise + concurrent-position correlation.
        //   1/8 Kelly = ~5.5% per trade -- we use 4.0% which sits inside the
        //   conservative end of that band.
        //   Worst case if all 6 cells hit initial-SL on same correlated bear
        //   day = ~24% portfolio drawdown. Still recoverable; well clear of
        //   margin_call (10% of equity = $1K floor).
        //   Projection: ~$90K/yr at this sizing vs $19,633 at baseline.
        // 2026-05-07 (S9 task 7): PINNED shadow_mode=true regardless of
        //   g_cfg.mode. Engine was effectively LIVE since 2026-04-30 because
        //   kShadowDefault=false in production. The intended 4-week paper
        //   validation never started -- the engine was firing real FIX orders
        //   with risk_pct=0.040 (8x tsmom baseline) on only 7 days of live
        //   experience. Discovered during S9 audit cross-check between
        //   chat/NEXT_SESSION ("validating in shadow") and engine_init.hpp
        //   (kShadowDefault). Pin remains in place until: PF >= 1.3,
        //   >= 30 trades, expectancy beats Tsmom, AND a deliberate human
        //   decision to flip back to kShadowDefault. Promotion gate per
        //   NEXT_SESSION.md S9 priority 3.
        // ================= TOMBSTONE — TrendRider (S-2026-07-07q) =================
        // CULLED on operator order 2026-07-07 ("backtest these engines, if they
        // fail cull them with a tombstone notice — I don't want to redo any of
        // this"). Faithful WF verdict already on record — NO further backtest owed:
        // WF RUN 2026-06-19 (backtest/trendrider_faithful.cpp, class-driven on
        // XAUUSD H1): 2022-2023 PF 0.75 / net -$838 / BOTH halves NEGATIVE
        // (2022 alone -$581); 2024-2026 bull PF 1.33 / +$2086.
        // => BULL-BETA, FAILS the WF gate. Bar-replay overstates trail engines
        // ~0.5-0.7 PF, so true edge is worse. The vault page (TrendRiderEngine.md)
        // said "shadow_mode must NOT flip to false"; the S-2026-07-01 blanket
        // all-engines cutover flipped it anyway — that discrepancy is what
        // triggered this cull. DO NOT re-enable, re-test, or re-derive. The gold
        // trend exposure it duplicated is already covered by the validated
        // XauTrendFollow 1h/2h/4h/D1 stack + riders.
        g_trend_rider.shadow_mode       = true;   // tombstone: restored to shadow (never valid live)
        g_trend_rider.enabled           = false;  // TOMBSTONED S-2026-07-07q — WF FAIL PF0.75 both halves NEG
        g_trend_rider.max_concurrent    = 6;
        // risk_pct 0.040 (4%) -> 0.0025 (0.25%). The 4% was "8x tsmom ~1/8 Kelly"
        // and is far hotter than the Phase-1 0.25-0.50% R band; 0.25% = the
        // conservative floor for an unvalidated engine (handoff item 3). max_lot_cap
        // 0.50 below is now a non-binding ceiling at 16x-lower risk.
        g_trend_rider.risk_pct          = 0.0025;         // 0.25% (was 0.040=4%) — R-consistent floor
        g_trend_rider.start_equity      = 10000.0;
        g_trend_rider.margin_call       = 1000.0;
        g_trend_rider.max_lot_cap       = 0.50;           // 10x tsmom baseline
        g_trend_rider.block_on_risk_off = true;
        g_trend_rider.warmup_csv_path   = "phase1/signal_discovery/tsmom_warmup_H1.csv";
        g_trend_rider.init();
        omega::warmup_or_die(g_trend_rider, "TrendRider");
        // 2026-06-23: warm-seed the BullGate so its chop/bear-kill is ACTIVE on boot (warmup_or_die
        // doesn't feed it). Otherwise TrendRider's regime kill stays cold -> trades any regime.
        g_trend_rider.seed_bull_gate_from_h1_csv("phase1/signal_discovery/tsmom_warmup_H1.csv");
        fflush(stdout);
    }

    // MinimalH4US30Breakout -- DJ30.F sister engine. Self-contained: builds
    // own H4 OHLC + ATR14 from tick stream (no g_bars_us30 exists). Validated
    // 27/27 profitable on 2yr Tickstory tick sweep. Default config (D=10
    // SL=1.0x TP=4.0x): n=184, PF=1.54, +$637, WR=28.3%. Initialised outside
    // the gold-conditional block above because it has no dependency on
    // g_bars_gold or any gold infrastructure.
    fflush(stdout);


    // MinimalH4GER40Breakout -- GER40/DAX sister engine. Same pattern as US30.
    // Multi-symbol scan 2026-05-20 (backtest/multi_symbol_scan.py): GER40 was
    // best performer of FX+indices scanned -- Sharpe 3.67 PnL $8.40 50 trades.
    fflush(stdout);

    // EurGbpPairsEngine -- EURUSD/GBPUSD H1 spread mean reversion (shadow mode).
    // Backtest 2026-05-20 (backtest/sweep_pairs_v2.csv, C++ engine, M5-interleaved 17mo data):
    //   Top config w=120 zi=1.5 zo=0.5 h=48: n=358 Sh=7.75 PnL=$638 MDD=$34.67 (cost 1pip/leg)
    //   6-mode rigor (pairs_rigor_cpp.cpp): IS=7.32 / OOS=7.23, 6/6 WF folds positive,
    //     14/14 months positive, Monte Carlo p<0.0001, robust to +/-20% perturbation.
    // CULL S-2026-06-17 (retest campaign): faithful pairs_rigor_cpp at THIS exact
    // config on real 7yr EURUSD+GBPUSD = Sh-2.04 both halves, robust all-9-neg;
    // clean recent-17mo = Sh-1.16 (n=359 ~ prior n=358, same entries); leg-swap
    // = also -2.04 (rules out orientation bug). The prior Sh+7.75/MC-p<0.0001
    // above used an M5 input file that no longer exists / can't be reproduced on
    // real EUR+GBP feeds. Decisive faithful failure -> disabled. Reversible:
    // shadow, lot 0.01. Re-open ONLY if the original blessed m5 reproduces +7.75.
    // Warm-seed pairs engine from EURUSD + GBPUSD H1 close CSVs (~7000 bars each).
    // Pre-populates spread_hist_ (z_window=120 bars needed). Without seed,
    // engine cold-warms 120 hours (5 trading days) before first z-eval.
    fflush(stdout);

    // DISABLED: Index TrendPullback never explicitly disabled -- no live validation.
    // GER40: tighter band (index moves more cleanly around EMAs)
    // NQ/SP TrendPullback: daily loss cap + tighter controls
    // Without DAILY_LOSS_CAP, NQ TrendPullback fired 7 consecutive losing entries
    // during the Apr 2 tariff crash (NQ dropped ~1000pts). Each SL hit was $12-13,
    // but the direction block (2 consec SL hits = 10min pause) was the only guard.
    // Daily loss cap stops the engine entirely after a bad sequence.
    // S37 audit (2026-05-27): RE-ENABLED in HARD shadow. S94 cited
    // consolidation into Nas100ShortEngine -- which was never instantiated
    // (see include/Nas100ShortEngine.hpp deletion in same commit). The
    // consolidation never happened; this engine was simply turned off and
    // its replacement never showed up. Restored in HARD shadow rather
    // than restoring to live (prior state) because the live PnL history
    // pre-S94 is unknown and worth re-observing in shadow first. Promote
    // by setting shadow_mode = kShadowDefault after evidence.
    // Load warm EMA state -- skips EMA_WARMUP_TICKS cold period on restart

    // ?? Nuke stale ctrader_bar_failed.txt on every startup ??????????????????
    // Old binaries wrote M5/M15 periods (5/7) and BOM-prefixed keys into this
    // file, permanently blacklisting the tick-data fallback requests that seed
    // Fix: delete the file and rewrite it clean from the pre-seeded in-memory set.
    // The in-memory set is pre-seeded in ctrader setup (XAUUSD:1 and live subs).
    // Any valid entries that were on disk will be re-discovered naturally --
    // GetTrendbarsReq is already blocked in-memory. The only thing we lose is
    // session-specific crash history, which resets anyway on every restart.
    {
        const std::string failed_path = log_root_dir() + "/ctrader_bar_failed.txt";
        if (std::remove(failed_path.c_str()) == 0) {
            printf("[STARTUP] Deleted stale ctrader_bar_failed.txt -- clean slate for bar requests\n");
        }
        fflush(stdout);
    }

    // Load OHLCBarEngine indicator state -- instant warm restart, no tick data request needed.
    // If .dat files exist and are <24hr old: m1_ready=true immediately on first tick.
    // Bars update live from on_spot_event (M15 bar closes pushed by broker every 15min).
    {
        const std::string base = log_root_dir();
        const bool m1_ok  = g_bars_gold.m1 .load_indicators(base + "/bars_gold_m1.dat");
        const bool m5_ok  = g_bars_gold.m5 .load_indicators(base + "/bars_gold_m5.dat");
        const bool m15_ok = g_bars_gold.m15.load_indicators(base + "/bars_gold_m15.dat");
        const bool h4_ok  = g_bars_gold.h4 .load_indicators(base + "/bars_gold_h4.dat");
        // H1 bars: gold + indices swing context (warm restart for H1SwingEngine + IndexSwingEngine)
        const bool h1_gold_ok = g_bars_gold.h1.load_indicators(base + "/bars_gold_h1.dat");
        g_bars_sp.h1  .load_indicators(base + "/bars_sp_h1.dat");
        g_bars_nq.h1  .load_indicators(base + "/bars_nq_h1.dat");
        if (h1_gold_ok) {
            // H1 indicators loaded -- H1SwingEngine is hot immediately.
            // m1_ready=true was set by load_indicators, so on_h1_bar() entry gate passes.
            printf("[STARTUP] H1 bar state loaded: EMA9=%.2f EMA21=%.2f EMA50=%.2f"
                   " ATR=%.2f RSI=%.1f ADX=%.1f trend=%+d -- H1SwingEngine hot\n",
                   g_bars_gold.h1.ind.ema9 .load(std::memory_order_relaxed),
                   g_bars_gold.h1.ind.ema21.load(std::memory_order_relaxed),
                   g_bars_gold.h1.ind.ema50.load(std::memory_order_relaxed),
                   g_bars_gold.h1.ind.atr14.load(std::memory_order_relaxed),
                   g_bars_gold.h1.ind.rsi14.load(std::memory_order_relaxed),
                   g_bars_gold.h1.ind.adx14.load(std::memory_order_relaxed),
                   g_bars_gold.h1.ind.trend_state.load(std::memory_order_relaxed));
            fflush(stdout);
        } else {
            printf("[STARTUP] H1 bar state cold -- H1SwingEngine needs 14 H1 bars to warm\n");
            fflush(stdout);
        }

        // S99g 2026-05-18: prime GoldScalpPyramidEngine from persisted atomics.
        // load_indicators() restores the atomic indicator values (EMA9/EMA21/ATR14)
        // but does NOT populate the bars_ deque -- that's hydrate_from_csv's job
        // and it may return zero on quiet days / missing CSVs. Live evidence
        // from S99f's startup log: '[GSP-WARMUP] fed=0' confirmed bars_ was
        // empty, so the prime_from_history-only approach left EMA21 cold.
        //
        // Fix: seed EMA9/EMA21/ATR14 directly from the disk-loaded atomics.
        // Donchian still needs live bar accumulation (~45 min) but that's the
        // FASTEST indicator -- the binding 105-min EMA21 cold-prime is gone.
        //
        // We also still call prime_from_history below: if bars_ IS populated,
        // it overwrites the rough atomic seed with recursion-correct values
        // AND fills the Donchian buffer from the same source.
        if (m5_ok) {
            const double m5_ema9  = g_bars_gold.m5.ind.ema9 .load(std::memory_order_relaxed);
            (void)m5_ema9;
            const double m5_ema21 = g_bars_gold.m5.ind.ema21.load(std::memory_order_relaxed);
            (void)m5_ema21;
            const double m5_atr14 = g_bars_gold.m5.ind.atr14.load(std::memory_order_relaxed);
            (void)m5_atr14;
            // Best-effort approximation for ATR prev_close: M5 EMA9 is the
            // tightest persisted reference to recent close. Drift bounded
            // by (live_first_close - EMA9) which converges in 1 bar.

            fflush(stdout);
        }

        // FxScalpPyramid x5 warm-seed TODO REMOVED (S-2026-06-29): engine retired,
        //   "no FX". Nothing to seed.

        // 2026-05-18 (part B): BBandScalp diagnostic prime.
        // Engine has no internal indicator state -- bb_upper/mid/lower, rsi14,
        // atr14 are read from g_bars_gold.m1.ind atomics on every tick.
        // prime_from_atomics is a no-op-with-log here, mirroring GSP's
        // startup-wiring shape so the startup log confirms M1 atomics are
        // live before the NY session opens.
        if (m1_ok) {
            const double m1_bbu = g_bars_gold.m1.ind.bb_upper.load(std::memory_order_relaxed);
            const double m1_bbm = g_bars_gold.m1.ind.bb_mid  .load(std::memory_order_relaxed);
            const double m1_bbl = g_bars_gold.m1.ind.bb_lower.load(std::memory_order_relaxed);
            const double m1_rsi = g_bars_gold.m1.ind.rsi14   .load(std::memory_order_relaxed);
            const double m1_atr = g_bars_gold.m1.ind.atr14   .load(std::memory_order_relaxed);
            printf("[STARTUP] BBandScalp primed: bb=[%.2f, %.2f, %.2f] rsi=%.1f atr=%.2f "
                   "(reads atomics each tick -- no warmup needed)\n",
                   m1_bbl, m1_bbm, m1_bbu, m1_rsi, m1_atr);
            fflush(stdout);
        }

        if (m15_ok) {
            // Immediately seed TrendPullback EMAs + ATR from M15 bar state
            // M1 EMA crossover for bar trend gate -- loaded from disk, no 15-min warmup
            if (m1_ok) {
                const double st_e9  = g_bars_gold.m1.ind.ema9 .load(std::memory_order_relaxed);
                const double st_e50 = g_bars_gold.m1.ind.ema50.load(std::memory_order_relaxed);
                const int st_trend  = (st_e9 > 0.0 && st_e50 > 0.0)
                    ? (st_e9 < st_e50 ? -1 : +1) : 0;
                (void)st_trend;
                printf("[STARTUP] M1 bar state loaded: EMA9=%.2f EMA50=%.2f RSI=%.1f trend=%+d"
                       " -- GoldStack bar gates active immediately\n",
                       st_e9, st_e50,
                       g_bars_gold.m1.ind.rsi14.load(std::memory_order_relaxed),
                       st_trend);
            } else if (m5_ok) {
                // Fallback: seed trend from M5 if M1 not available
                const double st_e9  = g_bars_gold.m1.ind.ema9 .load(std::memory_order_relaxed);
                const double st_e50 = g_bars_gold.m1.ind.ema50.load(std::memory_order_relaxed);
                const int st_trend  = (st_e9 > 0.0 && st_e50 > 0.0)
                    ? (st_e9 < st_e50 ? -1 : +1) : 0;
                (void)st_trend;
            }
            // Immediately seed H4 HTF trend gate -- no need to wait for first tick
            if (h4_ok) {
                // Seed H4RegimeEngine Donchian channel from saved H4 bar history.
                // Without this the channel needs 20 new H4 bars (80 hours) to warm.
                // With this: channel is ready from tick 1 -- engine is hot immediately.
                // Seed MinimalH4Breakout Donchian channel from same saved H4 bar history.
                // 10-bar channel: needs 40 hours warm-up cold vs. tick-1 ready warm.
            } else {
                printf("[STARTUP] H4 bar state cold -- H4RegimeEngine needs 20 H4 bars (~80hr)\n");
                fflush(stdout);
                // P1-11 (S18): cold-start CSV warm-load fallback for
                // MinimalH4Breakout. Drop a Dukascopy-style XAUUSD H4 OHLC CSV
                // at the path below before starting Omega and the Donchian
                // channel will be seeded immediately rather than waiting for
                // p.donchian_bars * 4hrs (default 40hrs) of fresh live H4
                // closes. Schema and supported timestamp formats are
                // documented at MinimalH4Breakout.hpp:163 (above
                // seed_channel_from_csv). The H4RegimeEngine remains cold --
                // CSV warm-load is implemented for MinimalH4Breakout only.
                const std::string xau_h4_csv = log_root_dir() + "/bars_xauusd_h4.csv";
                (void)xau_h4_csv;
                fflush(stdout);
            }
            printf("[STARTUP] Bar state loaded: M1=%s M5=%s M15=%s H1=%s H4=%s"
                   " EMA50=%.2f ATR=%.2f H4_trend=%d\n",
                   m1_ok?"ok":"cold", m5_ok?"ok":"cold", m15_ok?"ok":"cold",
                   h1_gold_ok?"ok":"cold", h4_ok?"ok":"cold",
                   g_bars_gold.m15.ind.ema50.load(std::memory_order_relaxed),
                   g_bars_gold.m15.ind.atr14.load(std::memory_order_relaxed),
                   g_bars_gold.h4.ind.trend_state.load(std::memory_order_relaxed));
        } else {
            printf("[STARTUP] No bar state on disk (cold start) -- 15min M1 warmup required\n");
            // P1-11 (S18): even when no bar state on disk at all, the CSV
            // warm-load for MinimalH4Breakout is still useful -- the channel
            // can be seeded from disk while the M1 EMAs warm up over the next
            // 15 minutes. Mirror the inner cold-fallback branch above.
            const std::string xau_h4_csv = log_root_dir() + "/bars_xauusd_h4.csv";
            (void)xau_h4_csv;
        }
        fflush(stdout);
    }

    // Load macro event calendar for OmegaVolTargeter high_impact_window() gate.
    // File: config\macro_events_today.txt (same dir as omega_config.ini)
    // Format: "HH:MM CURRENCY HIGH LABEL"  e.g. "13:30 USD HIGH NFP"
    // Must be updated daily (manually or via a pre-open script).
    // Graceful: if file absent, high_impact_window() returns false (no blocking).
    {
        // Mirror config resolution: try cwd first, then config\ subdir
        const std::string ev_cwd = "macro_events_today.txt";
        const std::string ev_cfg = "config\\macro_events_today.txt";
        std::ifstream test(ev_cwd);
        g_vol_targeter.load_events(test.is_open() ? ev_cwd : ev_cfg);
    }

    // TrendPullback gold: M15 bar EMAs seeded live from g_bars_gold.m15 each tick.
    g_bracket_gold.cancel_order_fn = [](const std::string& id) { send_cancel_order(id); };

    // ?? Configure new bracket engines ????????????????????????????????????????
    // US equity indices: arms both sides, captures whichever direction breaks out
    g_bracket_sp.symbol     = "US500.F"; g_bracket_sp.ENTRY_SIZE     = 0.01;
    g_bracket_nq.symbol     = "USTEC.F"; g_bracket_nq.ENTRY_SIZE     = 0.01;
    g_bracket_us30.symbol   = "DJ30.F";  g_bracket_us30.ENTRY_SIZE   = 0.01;
    g_bracket_nas100.symbol = "NAS100";  g_bracket_nas100.ENTRY_SIZE = 0.01;
    g_bracket_ger30.symbol  = "GER40";   g_bracket_ger30.ENTRY_SIZE  = 0.01;
    g_bracket_uk100.symbol  = "UK100";   g_bracket_uk100.ENTRY_SIZE  = 0.01;
    g_bracket_estx50.symbol = "ESTX50";  g_bracket_estx50.ENTRY_SIZE = 0.01;
    g_bracket_brent.symbol  = "BRENT"; g_bracket_brent.ENTRY_SIZE  = 0.01;
    g_bracket_eurusd.symbol = "EURUSD";  g_bracket_eurusd.ENTRY_SIZE = 0.01;
    g_bracket_gbpusd.symbol = "GBPUSD";  g_bracket_gbpusd.ENTRY_SIZE = 0.01;
    g_bracket_audusd.symbol = "AUDUSD";  g_bracket_audusd.ENTRY_SIZE = 0.01;
    g_bracket_nzdusd.symbol = "NZDUSD";  g_bracket_nzdusd.ENTRY_SIZE = 0.01;

    // ?? IndexSwingEngine configure -- shadow mode confirmed ??????????????????
    // sl_pts / min_ema_sep / pnl_scale set at construction in omega_types.hpp.
    // shadow_mode=true: engine tracks paper P&L only, no FIX orders sent.
    // NEVER set shadow_mode=false without explicit authorization + live validation.
    g_iswing_sp.shadow_mode = true;
    g_iswing_nq.shadow_mode = true;
    // CULLED 2026-06-02: IndexSwing net-negative in live shadow ledger
    // (logs/trades/omega_trade_closes_*: n=14, WR 43%, net -$79.91 over the
    // 2026-05-21..06-01 window; two -$200 US500 SL hits dominate). Confirms the
    // S39 live-edge audit which already flagged IndexSwing as the go-forward
    // bleeder -- two independent windows, not a single-trade reaction. H1/H4
    // EMA-cross swing on indices = trend-on-beta, loses to buy&hold (see
    // omega-indices-s44 / omega-index-ushours-drift). Re-enable only after a
    // signal-side rework + fresh walk-forward.
    g_iswing_sp.enabled = false;
    g_iswing_nq.enabled = false;

    // ── S38 SurvivorPortfolio configure -- 13 walk-forward survivors ─────────
    // All cells default enabled=true shadow_mode=true. Per-cell promotion gate
    // per CLAUDE.md: 30 shadow trades + WR >= 35% + net positive.
    // Seeded from phase1/signal_discovery/warmup_<SYM>_<TF>.csv where present.
    // Cells with missing CSV cold-warm (acceptable for shadow, will not fire
    // entries during seed because seed_from_csv sets enabled=false during replay).
    g_survivor.init_default_cells();
    g_survivor.seed_all("phase1/signal_discovery");
    // [history] S-2026-06-24 DISABLED (bear-protection hardening): bull-only (fleet-audit
    // bear 0.81) AND ZERO bear/regime gate AND self-enters (bypassed the bear chokepoint).
    // "Re-enable only with a wired bear gate + a both-regime audit pass." -> BOTH now done:
    g_survivor.enabled = true;   // S-2026-07-08c RE-ENABLED GATED (operator: "reinstate all winners,
                                 // correctly gated"). (1) Bear gate WIRED: Portfolio::entry_veto below
                                 // routes USTEC longs through index_market_regime().long_blocked() and
                                 // XAU longs through gold_regime().long_blocked() -- entries blocked in
                                 // sustained bear, management/exits unaffected. (2) Both-regime audit
                                 // PASS: backtest/survivor_gated_bt.cpp (REAL Portfolio, 2022-2026
                                 // XAU+USTEC H4 CERTIFIED tapes, veto seeded from NDX daily 2016+):
                                 // veto + USTEC_RSI_N7 culled = PF1.70 +$11,065 n=445, BULL PF1.89,
                                 // BEAR-2022 PF1.90 POSITIVE, both-WF-halves+, 2x-cost PF1.66 holds,
                                 // top3=38%. (3) Kill-reason cell USTEC_4h_RSI_N7 disabled below --
                                 // net-NEG even gated (-$1,022/4.3yr) AND its dedup lock starved the
                                 // profitable ZMR sibling (ZMR n 36->84 +$9,352 once freed). SHADOW
                                 // only; AUDITED row SurvivorGated; TOMBSTONES.tsv downgraded same commit.
    // S-2026-07-08c kill-reason cell cull (see re-enable note above): entries off,
    // cell object kept so persistence/dedup semantics match the audited BT exactly.
    for (auto& c : g_survivor.cells)
        if (std::strcmp(c.cfg.tag, "USTEC_4h_RSI_N7") == 0) c.st.enabled = false;
    // S-2026-07-08c bear chokepoint (the gate the 06-24 cull demanded); replicated
    // 1:1 by the audit harness via the same Portfolio::entry_veto hook.
    g_survivor.entry_veto = [](const char* sym, int side) -> bool {
        if (side <= 0) return false;                    // longs only; shorts never blocked
        if (std::strcmp(sym, "USTEC.F") == 0 || std::strcmp(sym, "USTEC") == 0)
            return omega::index_market_regime().long_blocked();
        if (std::strcmp(sym, "XAUUSD") == 0)
            return omega::gold_regime().long_blocked();
        return false;
    };
    // S-2026-06-17: BLANKET dedup (mode 1) -- operator policy override. NEVER run
    // two cells on the same symbol+side at once (the XAU DonchN20+N100 double-short
    // that prompted this). Supersedes the S-2026-06-16 regime-gated mode 2 (which
    // allowed same-side stacking in trend, ER>=0.25). TRADE-OFF: survivor_cap_test.cpp
    // measured the blanket cap at ~-29% vs OFF on a pure trend tape (it forgoes
    // riding a winner with a second cell). Operator accepts that: correlated
    // double-exposure on one symbol is a risk-concentration the book should not
    // carry, and the live book is net-negative anyway so the trend-stack upside
    // is not materialising. Revert to 2 to restore regime-gated stacking.
    g_survivor.dedup_mode = 1;
    printf("[SURV-INIT] portfolio armed cells=%zu spx_bars=%zu dedup_mode=%d(ER<%.2f)\n",
           g_survivor.cells.size(), g_survivor.spx.spx_bars.size(),
           g_survivor.dedup_mode, g_survivor.er_chop_thr);
    fflush(stdout);

    // ?? MAX_RANGE caps: ~0.4% of instrument price ?????????????????????????????
    // Prevents bracketing full day-range trending moves as if they were compression.
    // Evidence: ESTX50 bracket fired on 79.8pt range (1.4% of 5600) = entire London
    // open move, not compression. Rule: MAX_RANGE ? 2? MIN_RANGE ? 0.4% of price.
    g_bracket_sp.MAX_RANGE      = 25.0;   // ~0.40% of SP ~6200
    g_bracket_nq.MAX_RANGE      = 90.0;   // ~0.40% of NQ ~22500
    g_bracket_us30.MAX_RANGE    = 180.0;  // ~0.40% of DJ30 ~45000
    g_bracket_nas100.MAX_RANGE  = 90.0;   // ~0.40% of NAS100 ~22500
    g_bracket_ger30.MAX_RANGE   = 90.0;   // ~0.40% of GER40 ~22500
    g_bracket_uk100.MAX_RANGE   = 40.0;   // ~0.40% of UK100 ~10000
    g_bracket_estx50.MAX_RANGE  = 22.0;   // ~0.40% of ESTX50 ~5500
    g_bracket_brent.MAX_RANGE   = 1.20;   // ~0.40% of Brent ~$90 (oil tight)
    g_bracket_eurusd.MAX_RANGE  = 0.0008; // ~0.07% of EURUSD ~1.15 (FX tight)
    g_bracket_gbpusd.MAX_RANGE  = 0.0010; // ~0.08% of GBPUSD ~1.33
    g_bracket_audusd.MAX_RANGE  = 0.0006; // ~0.09% of AUDUSD ~0.70
    g_bracket_nzdusd.MAX_RANGE  = 0.0006; // ~0.10% of NZDUSD ~0.60
    // USDJPY/GOLD: MAX_RANGE set after their configure() calls below
    g_bracket_usdjpy.symbol = "USDJPY";  g_bracket_usdjpy.ENTRY_SIZE = 0.01;

    // ?? Bracket calibration -- March 2026 actual prices ?????????????????????????
    // All params derived from real price levels and observed daily ranges.
    // configure(buf, lookback, RR, cooldown_ms, min_range, cfm, ctout, min_hold_ms,
    //           vwap_dist, struct_ms, fail_win_ms, atr_per, atr_ck, atr_rk, slip_buf, edge_mult)
    //
    // RR=2.5 for equity indices -- trail kicks in at 2.5R, stepped SL locks gains
    // cooldown=60s for indices -- slightly choppier intraday than commodities
    // fail_win=10s for indices -- sweeps resolve faster on liquid index futures
    // vwap_dist=0 everywhere -- pre-breakout price near VWAP by definition
    //
    // US500.F (~$6,600): daily range $120, typical compression $8, spread $0.50
    // Index bracket engines -- MIN_RANGE calibrated to current 2026 price levels.
    // Rule: MIN_RANGE >= 0.20% of instrument price. Below that is tick noise.
    // At wrong (old) values: UK100 $4 @ 9720 = 0.041%, ESTX50 $3 @ 5387 = 0.056% -- pure noise.
    // Cooldown raised 60s?180s: 60s cooldown re-armed into same chop 1 min after SL hit.
    // MIN_STRUCTURE_MS raised 20s?30s: 20s is too short for real index compression.
    //
    // configure(buf, lookback, RR, cooldown_ms, MIN_RANGE, cfm, ctout, min_hold_ms,
    //           vwap_dist, MIN_STRUCTURE_MS, FAILURE_WINDOW_MS, atr_period,
    //           atr_confirm_k, atr_range_k, slippage_buffer, edge_multiplier)
    //
    // US500.F (~6000): 0.20% = $12.0 min range
    g_bracket_sp.configure(    0.25, 120, 2.5, 180000, 12.0, 0.05, 4000, 10000, 0.0, 30000, 10000, 20, 0.15, 2.0, 0.30, 1.5);
    // USTEC.F (~21000): 0.20% = $42.0 min range
    g_bracket_nq.configure(    0.75, 180, 2.5, 180000, 42.0, 0.05, 4000, 10000, 0.0, 45000, 10000, 20, 0.15, 2.0, 2.50, 1.5);
    // DJ30.F (~43000): 0.20% = $86.0 min range
    g_bracket_us30.configure(  2.50, 120, 2.5, 180000, 86.0, 0.05, 4000, 10000, 0.0, 30000, 10000, 20, 0.15, 2.0, 3.00, 1.5);
    // NAS100 (~21000): same as USTEC
    g_bracket_nas100.configure(0.75, 165, 2.5, 180000, 42.0, 0.05, 4000, 10000, 0.0, 30000, 10000, 20, 0.15, 2.0, 1.00, 1.5);
    // GER40 (~22000): 0.20% = $44.0 min range
    g_bracket_ger30.configure( 1.00, 120, 2.5, 180000, 44.0, 0.05, 4000, 10000, 0.0, 30000, 10000, 20, 0.15, 2.0, 1.00, 1.5);
    // UK100 (~9720): 0.20% = $19.5 min range
    g_bracket_uk100.configure( 0.50, 120, 2.5, 180000, 20.0, 0.05, 4000, 10000, 0.0, 30000, 10000, 20, 0.15, 2.0, 0.50, 1.5);
    // ESTX50 (~5387): 0.20% = $10.8 min range
    g_bracket_estx50.configure(0.50, 120, 2.5, 180000, 11.0, 0.05, 4000, 10000, 0.0, 30000, 10000, 20, 0.15, 2.0, 0.50, 1.5);

    // == S23 2026-04-25 : Port gold fix-set to index BracketEngines =============
    //   Ports the S19 / S20 / S21 / S22c fix stack from g_bracket_gold onto the
    //   7 index bracket instances. Each fix was originally calibrated against
    //   XAUUSD (~$4700 price, $12 MAX_RANGE, 195 ticks/min). Index scaling rule:
    //     * MAX_SL_DIST_PTS    ~ 50% of MAX_RANGE   (gold was 6.0 / 12.0)
    //     * CONFIRM_PTS        ~ 25% of MAX_RANGE   (gold was 3.0 / 12.0)
    //     * CONFIRM_SECS       = 30  (same window as gold -- same regime mechanic)
    //     * MAX_HOLD_SEC       = 1800 (30 min -- indices decay faster than gold's 60 min)
    //     * TRAIL_ACTIVATION_PTS / TRAIL_DISTANCE_PTS: per-instrument calibrated
    //
    //   S22c MAX_SL_DIST_PTS: every gold XAUUSD_BRACKET trade with dist > 6pt
    //     (>50% of MAX_RANGE) was a loser over 10 trading days. Applies
    //     identically to indices -- wide brackets are trends, not compressions.
    //
    //   S21 CONFIRM_PTS/SECS: saved $267 over 86 gold bracket trades by aborting
    //     brackets that fill then fail to move 3pt in 30s. Same mechanic ports.
    //
    //   S20 MAX_HOLD_SEC: absolute cap on filled-position hold. Indices set
    //     to 1800s (30 min) -- shorter than gold's 3600s because index tick
    //     rate is 2-5x higher so stale positions age faster.
    //
    //   S19 continuous trail (TRAIL_ACTIVATION_PTS / TRAIL_DISTANCE_PTS): gold
    //     defaults 3.0 / 2.0 in BracketEngine.hpp. Per the S19 commit comment,
    //     indices want roughly 5.0 / 3.5 at SP scale, scaled by price level.
    //     Engine code: header defaults 3.0 / 2.0 are gold-tuned. Override here.
    //
    //   NAS100 special per Jo 2026-04-25: "NAS needs different settings".
    //     NAS100 cash ($1/pt vs USTEC.F futures $20/pt) is noisier and has a
    //     historical 0% WR in NY-open bracket arms pre-S23 lookback widening.
    //     Uses tighter MAX_SL_DIST_PTS (45% of MAX_RANGE vs 50%), longer
    //     CONFIRM window (45s), and wider TRAIL_ACTIVATION_PTS (signal must
    //     exceed NY-open noise band before trail arms).
    //   Every other index gets 50% MAX_RANGE and 25% MAX_RANGE for MAX_SL_DIST
    //   and CONFIRM_PTS respectively, plus unified MAX_HOLD_SEC=1800.
    // US500.F: MAX_RANGE=25 -> MAX_SL_DIST=12.0, CONFIRM=6.0/30s.
    g_bracket_sp.MAX_SL_DIST_PTS      = 12.0;
    g_bracket_sp.CONFIRM_PTS          = 6.0;
    g_bracket_sp.CONFIRM_SECS         = 30;
    g_bracket_sp.MAX_HOLD_SEC         = 1800;
    g_bracket_sp.TRAIL_ACTIVATION_PTS = 5.0;
    g_bracket_sp.TRAIL_DISTANCE_PTS   = 3.0;
    // USTEC.F (NQ futures): MAX_RANGE=90 -> MAX_SL_DIST=45, CONFIRM=22/30s.
    g_bracket_nq.MAX_SL_DIST_PTS      = 45.0;
    g_bracket_nq.CONFIRM_PTS          = 22.0;
    g_bracket_nq.CONFIRM_SECS         = 30;
    g_bracket_nq.MAX_HOLD_SEC         = 1800;
    g_bracket_nq.TRAIL_ACTIVATION_PTS = 18.0;
    g_bracket_nq.TRAIL_DISTANCE_PTS   = 12.0;
    // DJ30.F: MAX_RANGE=180 -> MAX_SL_DIST=90, CONFIRM=45/30s.
    g_bracket_us30.MAX_SL_DIST_PTS      = 90.0;
    g_bracket_us30.CONFIRM_PTS          = 45.0;
    g_bracket_us30.CONFIRM_SECS         = 30;
    g_bracket_us30.MAX_HOLD_SEC         = 1800;
    g_bracket_us30.TRAIL_ACTIVATION_PTS = 35.0;
    g_bracket_us30.TRAIL_DISTANCE_PTS   = 22.0;
    // NAS100 (cash): NAS-SPECIFIC PER JO 2026-04-25. Historical 0% WR under
    //   original params -- runs tighter and slower than USTEC futures.
    //   MAX_RANGE=90 -> MAX_SL_DIST=40 (45% not 50% -- tighter kill on wide breakouts)
    //   CONFIRM_PTS=24 (27% of MAX_RANGE -- higher bar to clear NAS noise floor)
    //   CONFIRM_SECS=45 (NAS moves can take 40+s to prove out vs index futures)
    //   MAX_HOLD_SEC=1500 (25 min -- NAS cash chops faster than futures)
    //   TRAIL_ACTIVATION_PTS=25 (vs NQ 18 -- wider noise band on cash)
    //   TRAIL_DISTANCE_PTS=16   (vs NQ 12 -- give runners more room)
    g_bracket_nas100.MAX_SL_DIST_PTS      = 40.0;
    g_bracket_nas100.CONFIRM_PTS          = 24.0;
    g_bracket_nas100.CONFIRM_SECS         = 45;
    g_bracket_nas100.MAX_HOLD_SEC         = 1500;
    g_bracket_nas100.TRAIL_ACTIVATION_PTS = 25.0;
    g_bracket_nas100.TRAIL_DISTANCE_PTS   = 16.0;
    // GER40 (~22500): MAX_RANGE=90 -> MAX_SL_DIST=45, CONFIRM=22/30s.
    g_bracket_ger30.MAX_SL_DIST_PTS      = 45.0;
    g_bracket_ger30.CONFIRM_PTS          = 22.0;
    g_bracket_ger30.CONFIRM_SECS         = 30;
    g_bracket_ger30.MAX_HOLD_SEC         = 1800;
    g_bracket_ger30.TRAIL_ACTIVATION_PTS = 18.0;
    g_bracket_ger30.TRAIL_DISTANCE_PTS   = 12.0;
    // UK100 (~10000): MAX_RANGE=40 -> MAX_SL_DIST=20, CONFIRM=10/30s.
    g_bracket_uk100.MAX_SL_DIST_PTS      = 20.0;
    g_bracket_uk100.CONFIRM_PTS          = 10.0;
    g_bracket_uk100.CONFIRM_SECS         = 30;
    g_bracket_uk100.MAX_HOLD_SEC         = 1800;
    g_bracket_uk100.TRAIL_ACTIVATION_PTS = 8.0;
    g_bracket_uk100.TRAIL_DISTANCE_PTS   = 5.0;
    // ESTX50 (~5500): MAX_RANGE=22 -> MAX_SL_DIST=11, CONFIRM=5.5/30s.
    g_bracket_estx50.MAX_SL_DIST_PTS      = 11.0;
    g_bracket_estx50.CONFIRM_PTS          = 5.5;
    g_bracket_estx50.CONFIRM_SECS         = 30;
    g_bracket_estx50.MAX_HOLD_SEC         = 1800;
    g_bracket_estx50.TRAIL_ACTIVATION_PTS = 4.5;
    g_bracket_estx50.TRAIL_DISTANCE_PTS   = 3.0;
    // == end S23 index port ====================================================

    g_bracket_brent.configure(
        0.10,   // buffer
        30,     // lookback
        2.5,    // RR: raised 1.8?2.5. Brent has $2-5 daily ranges -- trail captures multi-hour moves.
        45000,  // cooldown_ms: reduced 120s?45s. Slightly longer than gold (oil choppier intraday).
        0.5,    // MIN_RANGE -- 0.50pts minimum (unchanged)
        0.05,   // CONFIRM_MOVE
        4000,   // confirm_timeout_ms
        10000,  // min_hold_ms
        0.0,    // VWAP_MIN_DIST: removed -- same rationale as gold/silver
        20000,  // MIN_STRUCTURE_MS
        12000,  // FAILURE_WINDOW_MS: raised 5s?12s -- oil sweeps similar duration to silver
        20,     // ATR_PERIOD
        0.15,   // ATR_CONFIRM_K
        2.0,    // ATR_RANGE_K
        0.10,   // SLIPPAGE_BUFFER
        1.5     // EDGE_MULTIPLIER
    );
    // FX brackets -- calibrated for March 2026 price levels
    // RR=2.0 for FX -- tighter pip-based moves, trail at 2R
    // cooldown=45s -- FX compresses and re-compresses faster than commodities
    // fail_win=8s -- FX sweeps resolve quickly (highly liquid, tight spread)
    // vwap_dist=0 everywhere -- pre-breakout FX near session VWAP
    //
    // MIN_RANGE raised across all FX pairs vs original calibration:
    //   Old EURUSD 0.00035 (3.5 pip) armed on 1-pip Asian tape noise ? junk bracket
    //   New 0.00060 (6 pip): genuine compression; below that is spread-level noise
    // MIN_STRUCTURE_MS raised 20s?45s: FX needs longer confirmation in thin tape
    // EDGE_MULTIPLIER raised 1.5?2.0: TP must cover 2? total round-trip cost
    //
    // EURUSD (~1.156): daily range ~100 pips, compression ~7 pips, spread ~1.4 pip
    g_bracket_eurusd.configure(0.00007, 30, 2.0, 45000, 0.00060, 0.05, 4000, 8000, 0.0, 45000, 8000, 20, 0.15, 2.0, 0.00010, 2.0);
    // GBPUSD (~1.330): daily range ~120 pips, compression ~8 pips, spread ~1.8 pip
    g_bracket_gbpusd.configure(0.00009, 30, 2.0, 45000, 0.00070, 0.05, 4000, 8000, 0.0, 45000, 8000, 20, 0.15, 2.0, 0.00012, 2.0);
    // AUDUSD (~0.701): daily range ~80 pips, compression ~5 pips, spread ~1.2 pip
    g_bracket_audusd.configure(0.00006, 30, 2.0, 45000, 0.00050, 0.05, 4000, 8000, 0.0, 45000, 8000, 20, 0.15, 2.0, 0.00008, 2.0);
    // NZDUSD (~0.583): similar to AUD, slightly wider spread
    g_bracket_nzdusd.configure(0.00007, 30, 2.0, 45000, 0.00050, 0.05, 4000, 8000, 0.0, 45000, 8000, 20, 0.15, 2.0, 0.00009, 2.0);
    // USDJPY (~149.5): daily range ~120 pips, compression ~25 pips, spread ~4 pip
    // MIN_RANGE raised 0.12?0.20 (2 pips)
    g_bracket_usdjpy.configure(0.02,    30, 2.0, 45000, 0.20,    0.05, 4000, 8000, 0.0, 45000, 8000, 20, 0.15, 2.0, 0.04,    2.0);
    g_bracket_usdjpy.MAX_RANGE = 0.60;   // ~0.40% of USDJPY ~150

    // Shadow mode + cancel wiring for all new bracket engines.
    //
    // 2026-05-08 S21 SINGLE-ENGINE-DEPLOY HARD-PIN (authorised by user in chat):
    //   Original code: const bool shadow = (g_cfg.mode != "LIVE");
    //   Problem: g_cfg.mode was just flipped SHADOW -> LIVE to unblock the
    //   send_live_order hard gate at order_exec.hpp:72 (the actual broker
    //   submit boundary). The original lambda would then have flipped all
    //   13 bracket engines (US500, USTEC, DJ30, NAS100, GER40, UK100, ESTX50,
    //   BRENT, EURUSD, GBPUSD, AUDUSD, NZDUSD, USDJPY) to shadow_mode=false
    //   at the same moment -- making 13 untested engines go live alongside
    //   the single authorised microscalper. User policy is single-engine
    //   live deploy: ONLY g_gold_microscalper trades live for now.
    //
    //   Fix: hard-pin shadow=true here, regardless of g_cfg.mode. Bracket
    //   engines stay shadow even with mode=LIVE. To re-arm any individual
    //   bracket for live in future, add an explicit
    //   `g_bracket_<sym>.shadow_mode = false;` line AFTER its wire_bracket
    //   call below. Restoring the original mode-following behaviour
    //   (`const bool shadow = (g_cfg.mode != "LIVE");`) in one edit would
    //   re-arm all 13 brackets at once and is the wrong unit of change.
    const bool shadow = true;
    auto wire_bracket = [&](auto& beng, int pending_timeout_sec = 180) {
        beng.shadow_mode         = shadow;
        beng.PENDING_TIMEOUT_SEC = pending_timeout_sec;
        beng.cancel_order_fn     = [](const std::string& id) { send_cancel_order(id); };
    };
    // Pending timeouts calibrated per symbol:
    // Commodities: longer compression periods before breaking (5-10 min)
    // Equity indices: faster breakouts (3-5 min)
    // FX: tightest -- often break within 2-3 min or reset
    wire_bracket(g_bracket_sp,      300);  // US500: 5min -- index compression holds ~3-5min
    wire_bracket(g_bracket_nq,      300);  // USTEC: 5min
    wire_bracket(g_bracket_us30,    300);  // DJ30:  5min
    wire_bracket(g_bracket_nas100,  300);  // NAS100: 5min
    wire_bracket(g_bracket_ger30,   300);  // GER40: 5min
    wire_bracket(g_bracket_uk100,   300);  // UK100: 5min
    wire_bracket(g_bracket_estx50,  300);  // ESTX50: 5min
    wire_bracket(g_bracket_brent,   480);  // Brent: 8min -- oil compresses longer than indices
    wire_bracket(g_bracket_eurusd,  180);  // EURUSD: 3min -- FX breaks fast or resets
    wire_bracket(g_bracket_gbpusd,  180);  // GBPUSD: 3min
    wire_bracket(g_bracket_audusd,  180);  // AUDUSD: 3min
    wire_bracket(g_bracket_nzdusd,  180);  // NZDUSD: 3min
    wire_bracket(g_bracket_usdjpy,  240);  // USDJPY: 4min -- JPY can compress longer
    apply_generic_fx_config(g_eng_eurusd);
    apply_generic_gbpusd_config(g_eng_gbpusd);
    apply_generic_audusd_config(g_eng_audusd);
    apply_generic_nzdusd_config(g_eng_nzdusd);
    apply_generic_usdjpy_config(g_eng_usdjpy);
    apply_generic_brent_config(g_eng_brent);

    // ?? SymbolConfig override -- load symbols.ini, apply per-symbol params ?????
    // Loads on top of the apply_* defaults above. Every symbol independently
    // tunable from symbols.ini with no shared inheritance.
    {
        // Try multiple paths -- binary may run from different working directories
        const std::vector<std::string> sym_ini_candidates = {
            "symbols.ini",
            "C:\\Omega\\symbols.ini",
            "C:\\Omega\\config\\symbols.ini",
        };
        std::string sym_ini;
        for (const auto& candidate : sym_ini_candidates) {
            if (g_sym_cfg.load(candidate)) { sym_ini = candidate; break; }
        }
        if (!sym_ini.empty()) {
            // Helper: apply SymbolConfig to a BreakoutEngine.
            // TP_PCT = SL_PCT * tp_mult (scales TP relative to existing SL).
            // MAX_SPREAD: symbols.ini stores absolute price units. BreakoutEngine
            // uses MAX_SPREAD_PCT (% of mid). Convert: pct = abs / typical_price * 100.
            // Typical prices: FX ~1-150, indices ~4000-50000, oil ~80-100.
            // For FX the absolute spread (e.g. 0.0002) divided by price (~1.1) * 100 = 0.018% -- correct.
            // For indices (e.g. 2.5 pts / 6000) * 100 = 0.042% -- correct.
            // So we store the raw abs value and let the caller provide typical price.
            // Simpler: we know the instrument types -- pass typical_price per symbol.
            auto apply_be = [](auto& eng, const SymbolConfig& c, double typical_price) {
                if (c.sl_mult > 0.0 && c.sl_mult != 1.0) eng.SL_PCT *= c.sl_mult;
                if (c.tp_mult > 0.0)                      eng.TP_PCT  = eng.SL_PCT * c.tp_mult;
                if (c.max_spread > 0.0 && typical_price > 0.0)
                    eng.MAX_SPREAD_PCT = (c.max_spread / typical_price) * 100.0;
                if (c.min_hold_ms  > 0)   eng.MAX_HOLD_SEC  = c.min_hold_ms / 1000;
                if (c.max_hold_sec > 0)   eng.MAX_HOLD_SEC  = c.max_hold_sec;
                // Use >= 0 so MIN_EDGE_BP=0 (indices) actually disables the bp check.
                // > 0.0 left the default 6.0 in place when symbols.ini said 0.
                if (c.min_edge_bp    >= 0.0) eng.EDGE_CFG.min_edge_bp  = c.min_edge_bp;
                if (c.min_breakout_pct> 0.0) eng.MIN_BREAKOUT_PCT  = c.min_breakout_pct;
                if (c.min_range      > 0.0)  eng.MIN_COMP_RANGE    = c.min_range;
                if (c.min_confirm_ticks > 0) eng.MIN_CONFIRM_TICKS = c.min_confirm_ticks;
            };

            // BreakoutEngine symbols -- typical prices for MAX_SPREAD_PCT conversion
            apply_be(g_eng_sp,     g_sym_cfg.get("US500.F"), 6000.0);
            apply_be(g_eng_nq,     g_sym_cfg.get("USTEC.F"), 20000.0);
            apply_be(g_eng_cl,     g_sym_cfg.get("USOIL.F"), 80.0);
            apply_be(g_eng_us30,   g_sym_cfg.get("DJ30.F"),  42000.0);
            apply_be(g_eng_nas100, g_sym_cfg.get("NAS100"),  20000.0);
            apply_be(g_eng_ger30,  g_sym_cfg.get("GER40"),   22000.0);
            apply_be(g_eng_uk100,  g_sym_cfg.get("UK100"),   8500.0);
            apply_be(g_eng_estx50, g_sym_cfg.get("ESTX50"),  5300.0);
            apply_be(g_eng_eurusd, g_sym_cfg.get("EURUSD"),  1.10);
            apply_be(g_eng_gbpusd, g_sym_cfg.get("GBPUSD"),  1.27);
            apply_be(g_eng_audusd, g_sym_cfg.get("AUDUSD"),  0.65);
            apply_be(g_eng_nzdusd, g_sym_cfg.get("NZDUSD"),  0.60);
            apply_be(g_eng_usdjpy, g_sym_cfg.get("USDJPY"),  150.0);
            apply_be(g_eng_brent,  g_sym_cfg.get("BRENT"), 85.0);

            // Per-symbol WATCH_TIMEOUT_SEC -- 120s indices, 120s FX
            g_eng_ger30.WATCH_TIMEOUT_SEC  = 240;
            g_eng_uk100.WATCH_TIMEOUT_SEC  = 240;
            g_eng_estx50.WATCH_TIMEOUT_SEC = 240;
            g_eng_eurusd.WATCH_TIMEOUT_SEC = 240;
            g_eng_gbpusd.WATCH_TIMEOUT_SEC = 240;
            g_eng_audusd.WATCH_TIMEOUT_SEC = 240;
            g_eng_nzdusd.WATCH_TIMEOUT_SEC = 240;
            g_eng_usdjpy.WATCH_TIMEOUT_SEC = 240;
            g_eng_brent.WATCH_TIMEOUT_SEC  = 240;

            // BracketEngine symbols -- override configure() fields from symbols.ini.
            // NOTE: SLIPPAGE_BUFFER is NOT overridden here. The configure() calls above
            // already set correct per-symbol slippage in price-point units (0.50 for ESTX50,
            // 0.80 for gold, 0.00010 for EURUSD etc). The SLIPPAGE_EST_BP field in symbols.ini
            // is in basis-points which is only meaningful for FX -- converting bp*price/10000
            // for indices produces wildly inflated values (5bp*5600=2.80pts vs correct 0.50pts)
            // and blocks all index bracket trades. Do not re-derive it here.
            auto apply_bracket = [](auto& eng, const SymbolConfig& c) {
                if (c.min_range        > 0.0) eng.MIN_RANGE          = c.min_range;
                if (c.max_range        > 0.0) eng.MAX_RANGE          = c.max_range;
                if (c.min_structure_ms > 0)   eng.MIN_STRUCTURE_MS   = c.min_structure_ms;
                if (c.breakout_fail_ms > 0)   eng.FAILURE_WINDOW_MS  = c.breakout_fail_ms;
                if (c.min_hold_ms      > 0)   eng.MIN_HOLD_MS        = c.min_hold_ms;
                if (c.max_hold_sec     > 0)   eng.PENDING_TIMEOUT_SEC = c.max_hold_sec;
                if (c.tp_mult          > 0.0) eng.RR                  = c.tp_mult;
                if (c.max_spread       > 0.0) eng.MAX_SPREAD          = c.max_spread;
                // SLIPPAGE_BUFFER intentionally NOT set here -- configure() has correct values
            };
            apply_bracket(g_bracket_gold,   g_sym_cfg.get("XAUUSD"));
            apply_bracket(g_bracket_sp,     g_sym_cfg.get("US500.F"));
            apply_bracket(g_bracket_nq,     g_sym_cfg.get("USTEC.F"));
            apply_bracket(g_bracket_us30,   g_sym_cfg.get("DJ30.F"));
            apply_bracket(g_bracket_nas100, g_sym_cfg.get("NAS100"));
            apply_bracket(g_bracket_ger30,  g_sym_cfg.get("GER40"));
            apply_bracket(g_bracket_uk100,  g_sym_cfg.get("UK100"));
            apply_bracket(g_bracket_estx50, g_sym_cfg.get("ESTX50"));
            apply_bracket(g_bracket_brent,  g_sym_cfg.get("BRENT"));
            apply_bracket(g_bracket_eurusd, g_sym_cfg.get("EURUSD"));
            apply_bracket(g_bracket_gbpusd, g_sym_cfg.get("GBPUSD"));
            apply_bracket(g_bracket_audusd, g_sym_cfg.get("AUDUSD"));
            apply_bracket(g_bracket_nzdusd, g_sym_cfg.get("NZDUSD"));
            apply_bracket(g_bracket_usdjpy, g_sym_cfg.get("USDJPY"));
            std::cout << "[SYMCFG] All bracket engine params overridden from " << sym_ini << "\n";

            // Apply supervisor config from symbols.ini to each supervisor
            // apply_supervisor: wires SymbolConfig fields into supervisor cfg.
            // max_spread_pct is NOT in SymbolConfig -- it lives in OmegaConfig
            // per-symbol fields. Pass it explicitly per symbol so the supervisor's
            // spread gate fires at the correct threshold (was stuck at 0.10 default,
            // which at $24,000 = $2,400 threshold -- never triggered).
            auto apply_supervisor = [](omega::SymbolSupervisor& sup,
                                       const std::string& sym,
                                       const SymbolConfig& c,
                                       double max_spread_pct_override) {
                sup.symbol                          = sym;
                sup.cfg.allow_bracket               = c.allow_bracket;
                sup.cfg.allow_breakout              = c.allow_breakout;
                sup.cfg.min_regime_confidence       = c.min_regime_confidence;
                sup.cfg.min_engine_win_margin       = c.min_engine_win_margin;
                sup.cfg.min_winner_score            = c.min_winner_score;
                sup.cfg.min_bracket_score           = c.min_bracket_score;
                sup.cfg.max_false_breaks            = c.max_false_breaks;
                sup.cfg.bracket_in_quiet_comp       = c.bracket_in_quiet_comp;
                sup.cfg.breakout_in_trend           = c.breakout_in_trend;
                sup.cfg.cooldown_fail_threshold     = c.cooldown_fail_threshold;
                sup.cfg.cooldown_duration_ms        = static_cast<int64_t>(c.cooldown_duration_ms);
                sup.cfg.max_spread_pct              = max_spread_pct_override;
            };
            apply_supervisor(g_sup_sp,     "US500.F", g_sym_cfg.get("US500.F"), g_cfg.sp_max_spread_pct);
            apply_supervisor(g_sup_nq,     "USTEC.F", g_sym_cfg.get("USTEC.F"), g_cfg.nq_max_spread_pct);
            apply_supervisor(g_sup_cl,     "USOIL.F", g_sym_cfg.get("USOIL.F"), g_cfg.oil_max_spread_pct);
            apply_supervisor(g_sup_us30,   "DJ30.F",  g_sym_cfg.get("DJ30.F"),  g_cfg.us30_max_spread_pct);
            apply_supervisor(g_sup_nas100, "NAS100",  g_sym_cfg.get("NAS100"),  g_cfg.nas100_max_spread_pct);
            apply_supervisor(g_sup_ger30,  "GER40",   g_sym_cfg.get("GER40"),   g_cfg.eu_index_max_spread_pct);
            apply_supervisor(g_sup_uk100,  "UK100",   g_sym_cfg.get("UK100"),   g_cfg.eu_index_max_spread_pct);
            apply_supervisor(g_sup_estx50, "ESTX50",  g_sym_cfg.get("ESTX50"),  g_cfg.eu_index_max_spread_pct);
            apply_supervisor(g_sup_eurusd, "EURUSD",  g_sym_cfg.get("EURUSD"),  g_cfg.fx_max_spread_pct);
            apply_supervisor(g_sup_gbpusd, "GBPUSD",  g_sym_cfg.get("GBPUSD"),  g_cfg.gbpusd_max_spread_pct);
            // AUDUSD/NZDUSD: use their dedicated max_spread_pct (0.030/0.035), NOT
            // fx_max_spread_pct (0.017). During Asia session AUD/NZD spreads widen to
            // ~2-3 pips (~0.025-0.030%), which exceeds 0.017% and locks the supervisor
            // in permanent HIGH_RISK_NO_TRADE. Their own spread thresholds were already
            // calibrated for this -- apply_supervisor was ignoring them.
            apply_supervisor(g_sup_audusd, "AUDUSD",  g_sym_cfg.get("AUDUSD"),  g_cfg.audusd_max_spread_pct);
            apply_supervisor(g_sup_nzdusd, "NZDUSD",  g_sym_cfg.get("NZDUSD"),  g_cfg.nzdusd_max_spread_pct);
            apply_supervisor(g_sup_usdjpy, "USDJPY",  g_sym_cfg.get("USDJPY"),  g_cfg.usdjpy_max_spread_pct);
            apply_supervisor(g_sup_brent,  "BRENT", g_sym_cfg.get("BRENT"), g_cfg.brent_max_spread_pct);
            apply_supervisor(g_sup_gold,   "XAUUSD",  g_sym_cfg.get("XAUUSD"),  g_cfg.bracket_gold_max_spread_pct);
            // S37-X (2026-05-28): tie supervisor allow_* to engine disable
            // state. Without this, supervisor logs "winner=X allow=1 reason=
            // valid_signal" on every approving tick while the engine downstream
            // is hardcoded-off or has no dispatch wired -- operator sees the
            // approvals on the dashboard but no fires happen, with no log line
            // naming the dead gate. This was a 28-day silent observability
            // hole for XAUUSD bracket. Apply the same tie to every other
            // supervisor whose downstream is known dead/disconnected:
            //
            //   - XAUUSD (g_sup_gold):   bracket dispatched via tick_gold.hpp,
            //                             gated by g_disable_bracket_gold.
            //   - INDICES (sp/nq/us30/nas100/uk100/estx50/ger30): supervisor
            //     dispatch sites in tick_indices.hpp are COMMENTED OUT
            //     (see lines 101-105, 440-445, 667-669, 944-946, 973-975,
            //     1030-1032). sup_decision() is called + LOGGED every tick
            //     but no consumer. Force allow_*=false so the log stops
            //     lying and CPU is saved on the gate evaluation.
            //   - FX (eurusd/gbpusd/audusd/nzdusd/usdjpy): instantiated +
            //     configured here but sup_decision() is NEVER called from
            //     tick_fx.hpp. Decorative. Same force-off treatment.
            //   - BRENT (g_sup_brent), USOIL (g_sup_cl): dispatch ACTIVE
            //     in tick_oil.hpp, leave as-is.
            if (g_disable_bracket_gold) {
                g_sup_gold.cfg.allow_bracket = false;
                std::cout << "[SUPERVISOR-XAUUSD] allow_bracket FORCE-OFF: "
                             "g_disable_bracket_gold=true (globals.hpp). "
                             "Re-enable engine first, then supervisor will fire.\n";
            }
            // Indices: dispatch commented out in tick_indices.hpp.
            for (auto* sup : {&g_sup_sp, &g_sup_nq, &g_sup_us30, &g_sup_nas100,
                              &g_sup_uk100, &g_sup_estx50, &g_sup_ger30}) {
                sup->cfg.allow_bracket  = false;
                sup->cfg.allow_breakout = false;
            }
            std::cout << "[SUPERVISOR-INDICES] allow_bracket/breakout FORCE-OFF on all 7 "
                         "index supervisors (sp/nq/us30/nas100/uk100/estx50/ger30): "
                         "dispatch sites in tick_indices.hpp commented out. Re-enable "
                         "the dispatch call first, then supervisor will fire.\n";
            // FX: sup_decision never called from tick_fx.hpp.
            for (auto* sup : {&g_sup_eurusd, &g_sup_gbpusd, &g_sup_audusd,
                              &g_sup_nzdusd, &g_sup_usdjpy}) {
                sup->cfg.allow_bracket  = false;
                sup->cfg.allow_breakout = false;
            }
            std::cout << "[SUPERVISOR-FX] allow_bracket/breakout FORCE-OFF on all 5 "
                         "FX supervisors (eurusd/gbpusd/audusd/nzdusd/usdjpy): "
                         "sup_decision() never called from tick_fx.hpp. Wire the "
                         "dispatch first, then supervisor will fire.\n";
            std::cout << "[SUPERVISOR] All supervisors configured from " << sym_ini << "\n";
            std::cout << "[SYMCFG] All engine params overridden from " << sym_ini << "\n";
        } else {
            // ?? CRITICAL: symbols.ini not found ??????????????????????????????????
            // Without symbols.ini: allow_bracket=false for all non-metals.
            // Bracket engines for indices/FX will NOT fire until symbols.ini is deployed.
            // This is intentional -- bracket config is not safe to run with compiled defaults.
            std::cout << "\n";
            std::cout << "[SYMCFG] !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n";
            std::cout << "[SYMCFG] !!! CRITICAL: symbols.ini NOT FOUND -- BRACKET DISABLED !!!\n";
            std::cout << "[SYMCFG] !!! Searched:                                           !!!\n";
            std::cout << "[SYMCFG] !!!   symbols.ini                                       !!!\n";
            std::cout << "[SYMCFG] !!!   C:\\Omega\\symbols.ini                              !!!\n";
            std::cout << "[SYMCFG] !!!   C:\\Omega\\config\\symbols.ini                      !!!\n";
            std::cout << "[SYMCFG] !!! Copy symbols.ini to Omega.exe directory and restart !!!\n";
            std::cout << "[SYMCFG] !!! allow_bracket=false for all non-metals until fixed  !!!\n";
            std::cout << "[SYMCFG] !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n";
            std::cout << "\n";
            // Assign symbol names directly so supervisor logs are readable even without ini
            g_sup_sp.symbol     = "US500.F"; g_sup_nq.symbol     = "USTEC.F";
            g_sup_cl.symbol     = "USOIL.F"; g_sup_us30.symbol   = "DJ30.F";
            g_sup_nas100.symbol = "NAS100";  g_sup_ger30.symbol  = "GER40";
            g_sup_uk100.symbol  = "UK100";   g_sup_estx50.symbol = "ESTX50";
            g_sup_gold.symbol   = "XAUUSD";
            g_sup_eurusd.symbol = "EURUSD";  g_sup_gbpusd.symbol = "GBPUSD";
            g_sup_audusd.symbol = "AUDUSD";  g_sup_nzdusd.symbol = "NZDUSD";
            g_sup_usdjpy.symbol = "USDJPY";  g_sup_brent.symbol  = "BRENT";
            // Without symbols.ini: disable bracket on non-metals (no bracket engine exists)
            for (auto* sup : {&g_sup_sp, &g_sup_nq, &g_sup_cl, &g_sup_us30, &g_sup_nas100,
                              &g_sup_ger30, &g_sup_uk100, &g_sup_estx50,
                              &g_sup_eurusd, &g_sup_gbpusd, &g_sup_audusd,
                              &g_sup_nzdusd, &g_sup_usdjpy, &g_sup_brent})
                sup->cfg.allow_bracket = false;
            // S37-X (2026-05-28): same engine-state tie as the symbols.ini path.
            if (g_disable_bracket_gold) {
                g_sup_gold.cfg.allow_bracket = false;
                std::cout << "[SUPERVISOR-XAUUSD] allow_bracket FORCE-OFF: "
                             "g_disable_bracket_gold=true.\n";
            }
            // Raise cooldown threshold from default 3 to 20
            for (auto* sup : {&g_sup_sp, &g_sup_nq, &g_sup_cl, &g_sup_us30, &g_sup_nas100,
                              &g_sup_ger30, &g_sup_uk100, &g_sup_estx50,
                              &g_sup_gold, &g_sup_eurusd, &g_sup_gbpusd, &g_sup_audusd,
                              &g_sup_nzdusd, &g_sup_usdjpy, &g_sup_brent})
                sup->cfg.cooldown_fail_threshold = 20;
        }
    }
    // Reset all supervisor hysteresis counters on startup.
    // Prevents stale HIGH_RISK/CHOP counts from a prior session blocking
    // trading before baseline vol warms (typically first 60-120 ticks).
    for (auto* sup : {&g_sup_sp, &g_sup_nq, &g_sup_cl, &g_sup_us30, &g_sup_nas100,
                      &g_sup_ger30, &g_sup_uk100, &g_sup_estx50,
                      &g_sup_gold, &g_sup_eurusd, &g_sup_gbpusd, &g_sup_audusd,
                      &g_sup_nzdusd, &g_sup_usdjpy, &g_sup_brent})
        sup->reset();
    printf("[SUPERVISOR] All hysteresis counters reset on startup\n");
    fflush(stdout);
    // ?? Hot-reload config watcher ??????????????????????????????????????????????
    // Watches omega_config.ini every 2s. On change: re-runs load_config() +
    // sanitize_config() + all apply_engine_config() calls -- zero downtime,
    // no position closes, no reconnect. Change any param in the .ini and it
    // takes effect within 2 seconds without rebooting Omega.
    // NOT reloadable: FIX host/port/credentials, mode (SHADOW/LIVE), code changes.
    OmegaHotReload::start(cfg_path, [&cfg_path]() {
        load_config(cfg_path);
        sanitize_config();
        apply_engine_config(g_eng_sp);
        apply_engine_config(g_eng_nq);
        apply_engine_config(g_eng_cl);
        apply_engine_config(g_eng_us30);
        apply_engine_config(g_eng_nas100);
        apply_generic_index_config(g_eng_ger30);
        apply_generic_index_config(g_eng_uk100);
        apply_generic_index_config(g_eng_estx50);
        apply_generic_fx_config(g_eng_eurusd);
        apply_generic_gbpusd_config(g_eng_gbpusd);
        apply_generic_audusd_config(g_eng_audusd);
        apply_generic_nzdusd_config(g_eng_nzdusd);
        apply_generic_usdjpy_config(g_eng_usdjpy);
        apply_generic_brent_config(g_eng_brent);
        printf("[HOT-RELOAD] All engine configs refreshed\n");
        fflush(stdout);
    });

    // ?? Position sizing ???????????????????????????????????????????????????????
    // ENTRY_SIZE on each engine is the FALLBACK lot used only when
    // risk_per_trade_usd == 0. When risk sizing is active, compute_size()
    // calculates the lot dynamically and ENTRY_SIZE is never used.
    // NAS100 broker minimum is 0.10 lots -- enforced as fallback and in compute_size floor.
    if (g_cfg.risk_per_trade_usd <= 0.0) {
        // Fixed lot mode -- risk sizing disabled, use hardcoded fallbacks
        g_eng_sp.ENTRY_SIZE       = 0.01;
        g_eng_nq.ENTRY_SIZE       = 0.01;
        g_eng_cl.ENTRY_SIZE       = 0.01;
        g_eng_us30.ENTRY_SIZE     = 0.01;
        g_eng_nas100.ENTRY_SIZE   = 0.10;  // NAS100 broker minimum
        g_eng_ger30.ENTRY_SIZE    = 0.01;
        g_eng_uk100.ENTRY_SIZE    = 0.01;
        g_eng_estx50.ENTRY_SIZE   = 0.01;
        g_eng_eurusd.ENTRY_SIZE   = 0.01;
        g_eng_gbpusd.ENTRY_SIZE   = 0.01;
        g_eng_brent.ENTRY_SIZE    = 0.01;
        // Bracket engine fallbacks -- same as breakout engines
        g_bracket_sp.ENTRY_SIZE       = 0.01;
        g_bracket_nq.ENTRY_SIZE       = 0.01;
        g_bracket_us30.ENTRY_SIZE     = 0.01;
        g_bracket_nas100.ENTRY_SIZE   = 0.10;  // NAS100 broker minimum
        g_bracket_ger30.ENTRY_SIZE    = 0.01;
        g_bracket_uk100.ENTRY_SIZE    = 0.01;
        g_bracket_estx50.ENTRY_SIZE   = 0.01;
        g_bracket_brent.ENTRY_SIZE    = 0.01;
        g_bracket_eurusd.ENTRY_SIZE   = 0.01;
        g_bracket_gbpusd.ENTRY_SIZE   = 0.01;
        g_bracket_audusd.ENTRY_SIZE   = 0.01;
        g_bracket_nzdusd.ENTRY_SIZE   = 0.01;
        g_bracket_usdjpy.ENTRY_SIZE   = 0.01;
        std::cout << "[SIZING] Fixed lot mode active (risk_per_trade_usd=0)\n"
                  << "[SIZING]   All instruments: 0.01 lots | NAS100: 0.10 lots\n";
    } else {
        // Risk-based sizing active -- ENTRY_SIZE is only a safety fallback,
        // compute_size() drives actual lot size from risk_per_trade_usd.
        // Set fallbacks to sensible minimums in case compute_size() ever bails.
        g_eng_sp.ENTRY_SIZE       = 0.01;
        g_eng_nq.ENTRY_SIZE       = 0.01;
        g_eng_cl.ENTRY_SIZE       = 0.01;
        g_eng_us30.ENTRY_SIZE     = 0.01;
        g_eng_nas100.ENTRY_SIZE   = 0.10;  // NAS100 broker minimum
        g_eng_ger30.ENTRY_SIZE    = 0.01;
        g_eng_uk100.ENTRY_SIZE    = 0.10;  // indices: $10 / ~8pt SL * $1/pt = 1.25 ? capped at max
        g_eng_estx50.ENTRY_SIZE   = 0.10;
        g_eng_eurusd.ENTRY_SIZE   = 0.01;
        g_eng_gbpusd.ENTRY_SIZE   = 0.01;
        g_eng_brent.ENTRY_SIZE    = 0.01;
        // Bracket engine fallbacks
        g_bracket_sp.ENTRY_SIZE       = 0.10;
        g_bracket_nq.ENTRY_SIZE       = 0.10;
        g_bracket_us30.ENTRY_SIZE     = 0.10;
        g_bracket_nas100.ENTRY_SIZE   = 0.10;  // NAS100 broker minimum
        g_bracket_ger30.ENTRY_SIZE    = 0.10;
        g_bracket_uk100.ENTRY_SIZE    = 0.10;
        g_bracket_estx50.ENTRY_SIZE   = 0.10;
        g_bracket_brent.ENTRY_SIZE    = 0.01;
        g_bracket_eurusd.ENTRY_SIZE   = 0.01;
        g_bracket_gbpusd.ENTRY_SIZE   = 0.01;
        g_bracket_audusd.ENTRY_SIZE   = 0.01;
        g_bracket_nzdusd.ENTRY_SIZE   = 0.01;
        g_bracket_usdjpy.ENTRY_SIZE   = 0.01;
        std::cout << "[SIZING] Risk-based sizing active (risk_per_trade_usd=$"
                  << g_cfg.risk_per_trade_usd << ")\n"
                  << "[SIZING]   Lot size computed dynamically per trade from SL distance\n"
                  << "[SIZING]   Fallback (if compute_size fails): indices=0.10 | others=0.01\n";
    }

    // Wire account equity to edge model -- applies in all modes.
    // Shadow uses the same account_equity for Kelly sizing as live would.
    {
        const double acct_eq = g_cfg.account_equity;
        g_eng_sp.ACCOUNT_EQUITY     = acct_eq;
        g_eng_nq.ACCOUNT_EQUITY     = acct_eq;
        g_eng_cl.ACCOUNT_EQUITY     = acct_eq;
        g_eng_us30.ACCOUNT_EQUITY   = acct_eq;
        g_eng_nas100.ACCOUNT_EQUITY = acct_eq;
        g_eng_ger30.ACCOUNT_EQUITY  = acct_eq;
        g_eng_uk100.ACCOUNT_EQUITY  = acct_eq;
        g_eng_estx50.ACCOUNT_EQUITY = acct_eq;
        g_eng_eurusd.ACCOUNT_EQUITY = acct_eq;
        g_eng_gbpusd.ACCOUNT_EQUITY = acct_eq;
        g_eng_brent.ACCOUNT_EQUITY  = acct_eq;
        std::cout << "[SIZING] account_equity=" << acct_eq
                  << (g_cfg.mode == "LIVE" ? " (LIVE)" : " (SHADOW -- same as live)") << "\n";
    }
    std::cout.flush();

    // GoldEngineStack config -- applies all [gold_stack] ini values.
    // Must be called AFTER load_config(). Defaults are safe (match prior constexpr).
    // MIN_TRADE_GATE cost basis: the gold PRICE feed is BlackBull spot (wide
    // ~$1.2/oz spread that snap.spread carries), but ORDERS route to IBKR GC
    // futures (~8x cheaper). When execution_broker==IBKR, rebase the gate's
    // spread-derived cost hurdle onto the execution venue via a conservative
    // 0.5 floor (verified real IBKR RT ~1/8 of BlackBull -- 0.5 under-claims).
    // execution_broker is finalized from default/config here; the late env
    // override (OMEGA_EXECUTION_BROKER) is an operator-forced edge case.
    g_cfg.gs_cfg.cost_basis_factor = (g_cfg.execution_broker == "IBKR") ? 0.5 : 1.0;
    g_gold_stack.configure(g_cfg.gs_cfg);

    // ?? GoldStack sub-engine audit-disables (2026-04-30, loser audit Wave 2) ??
    // Wired to globals.hpp g_disable_* flags. Each flag defaults to true
    // (DISABLED). Flip to false in globals.hpp to re-enable a sub-engine after
    // a fresh shadow-validation window. See GoldEngineStack::set_subengine_audit_disabled
    // for dispatch-loop gating semantics.
    if (g_disable_session_momentum) {
        g_gold_stack.set_subengine_audit_disabled("SessionMomentum", true);
    }
    if (g_disable_intraday_seasonality) {
        g_gold_stack.set_subengine_audit_disabled("IntradaySeasonality", true);
    }
    if (g_disable_vwap_snapback) {
        g_gold_stack.set_subengine_audit_disabled("VWAP_SNAPBACK", true);
    }
    if (g_disable_vwap_stretch_reversion) {
        g_gold_stack.set_subengine_audit_disabled("VWAPStretchReversion", true);
    }
    if (g_disable_dxy_divergence) {
        g_gold_stack.set_subengine_audit_disabled("DXYDivergence", true);
    }
    if (g_disable_asian_range) {
        // S99d 2026-05-18: live bleed — first London-window fire at 07:01 UTC took
        // -$5.28 on a 2.5R setup (entry 4548.65 / SL 4540.65 / TP 4568.65, exited
        // 4543.65 in 2m29s, "AsianRange SL"). Backtest profile is thin: 382 trades
        // over 2yr, WR=49.7% (~coinflip), total=$279, avg=$0.73, MaxDD=$105. One
        // bad day wipes weeks of edge. Disable now, retune with fresh sweep before
        // re-enable. Sub-engine name "AsianRange" matches engine_ ctor at L1183.
        g_gold_stack.set_subengine_audit_disabled("AsianRange", true);
        // TOMBSTONED 2026-06-11 (operator winners-only cull). Live shadow ledger
        // (May 11-Jun 11): DonchianBreakout n=16 PF=0.39 net -$22; DynamicRange
        // n=11 PF=0.04 net -$19 (WR 9%). Both bleed in the current regime; the
        // GoldEngineStack's gold edge does not live in these two sub-engines.
        // set_subengine_audit_disabled both skips the process loop AND setEnabled(false).
        g_gold_stack.set_subengine_audit_disabled("DonchianBreakout", true);
        g_gold_stack.set_subengine_audit_disabled("DynamicRange", true);
    }
    {
        char _msg[512];
        snprintf(_msg, sizeof(_msg),
                 "[GOLDSTACK-AUDIT] sub-engine gates: "
                 "session_mom=%s intraday_seas=%s vwap_snap=%s "
                 "vwap_stretch=%s dxy_div=%s asian_rng=%s\n",
                 g_disable_session_momentum       ? "DISABLED" : "active",
                 g_disable_intraday_seasonality   ? "DISABLED" : "active",
                 g_disable_vwap_snapback          ? "DISABLED" : "active",
                 g_disable_vwap_stretch_reversion ? "DISABLED" : "active",
                 g_disable_dxy_divergence         ? "DISABLED" : "active",
                 g_disable_asian_range            ? "DISABLED" : "active");
        std::cout << _msg;
        std::cout.flush();
    }

    // (LatencyEdgeStack config block removed S13 Finding B 2026-04-24 — engine culled)
    // Must be called AFTER load_config(). Defaults are safe (match prior constexpr).
    // g_le_stack.configure(...) REMOVED at S13 Finding B 2026-04-24 — engine culled.

    // ?? SHELVED ENGINE DISABLE -- 2026-03-31 ??????????????????????????????????
    // Engines disabled based on live performance audit. Each engine's own
    // guard (if (!enabled_) return noSignal()) prevents new entries.
    // Existing positions (if any) are still drained via has_open_position() paths.
    //
    // NBM indices: live data insufficient, not validated. Still shelved.
    // NBM gold london: RE-ENABLED 2026-04-01 -- live MT5 data confirms the logic
    // (London open ATR breakout, 51min hold, +$185). Omega NBM is identical concept.
    //
    // ORB (OpeningRange): no live data. Shelved pending shadow validation.
    // 2026-05-08 AUDIT-FIX (S15 P0-5): set per-instance UTC open times before
    //   disabling. The OpeningRangeEngine class defaults to OPEN_HOUR=13 /
    //   OPEN_MIN=30 (NY open). Only g_orb_us inherits a correct value; the
    //   three EU instances were silently tracking NY open while named for
    //   European cash markets. If anyone flips one of these `enabled=true`
    //   without these per-instance overrides, ORB fires at the wrong session.
    //   The daily reset path (CrossAssetEngines.hpp:1003-1009) is already
    //   correct and does not need changes.

    // ── ORB-SWING cohort (2026-05-23) ────────────────────────────────────────
    // Big-swing capture on NAS100 + DJ30 at NY open. Uses the same
    // OpeningRangeEngine class but with swing-scale geometry so the
    // CrossPosition manage() path (BE lock at 40% TP, trail at 60% TP,
    // TP-extend at full TP) has room to ride the daily move instead of
    // closing at a 0.10% scalp.
    //
    //   Range:   30 min from 13:30 UTC (NY equities open)
    //   Buffer:  0.02% above/below range (catch break a touch faster than
    //            the 0.03% default; index futures need only a small confirm)
    //   TP:      0.50% target -- typical NAS100 day range is 1.0-1.5%, so
    //            0.50% is reachable but past the 0.10% scalp default. With
    //            allow_tp_extend=true (default) the position rides past TP
    //            by another 0.50% via the manage()-path continuation logic.
    //   SL:      0.20% -- ~2.5x typical 30-min range. Wide enough to survive
    //            normal post-open chop but bounded.
    //   Hold:    6 hours (21600s) -- full NY session 13:30-19:30 UTC.
    //            Trail/timeout-when-flat exits much earlier on losing days.
    //
    // Per-pair instance: DJ30 has slightly wider TP (0.55%) because NYSE
    // composite Dow drifts further per-percent than tech-heavy NAS100.



    //
    // Cross-asset: EIA fade, BrentWTI spread, FX cascade, carry unwind.
    // All have insufficient live data. Shelved pending shadow validation.
    // ESNQ: already guarded by esnq_enabled=false in config. Belt-and-suspenders.
    // ?? END SHELVED ENGINE DISABLE ????????????????????????????????????????????

    // ── Ger40LondonBreakoutEngine (2026-05-17) ─────────────────────────────────
    // Asian range break-below during London open. SHORT only.
    // Validated: 2.3yr GER40 ticks, PF=1.42, WR=50.5%, E=+11.6pts,
    // 20/25 param-grid combos profitable. Structural edge: European
    // institutional flow breaks overnight Asia consolidation downward.
    //
    // Parameters from walk-forward optimal (robust across grid):
    //   TP_MULT=0.75, SL_MULT=0.50, entry window 07:00-09:00 UTC
    //   Asian range 21:00-07:00 UTC, max hold 4hr, short only
    {
        fflush(stdout);
    }

    // ── BreakBounceEngine (2026-05-31) ─────────────────────────────────────────
    // MT5 break-and-retest EA ported to Omega. XAUUSD D1 bias / H1 break / M20
    // retest. 2yr IS/OOS sweep: OOS PF 1.54, WR 55%, DD 46pts (all LONG, gold
    // bull only -- NOT bear-validated; the D1 bias filter should flatten longs
    // in a downtrend but that is untested on data). SHADOW until forward-proven.
    // Engine defaults already hold the validated config (H1/M20, STOP_ATR 1.2,
    // RR 1.5). L2 profit-protect is OFF by default -- validate via live L2
    // replay before enabling (the backtest tick file has no depth data).
    {
        // Regime guard: ADX chop-gate is OFF. IS/OOS sweep (2026-05-31) showed
        // EVERY threshold is SUBTRACTIVE on the available (bull-only) data --
        // OOS PF 1.54 -> 1.20-1.32, net halved -- the same IS-up/OOS-down
        // signature as the ER chop-gate dead-end. The D1 EMA bias is the bear
        // guard (no longs in a downtrend; shorts arm in a bear). Re-validate ADX
        // on forward shadow data once a chop/bear regime is captured.

        // Warm-seed: D1 EMA200 + H1/M20 ATR/range so the engine boots hot
        // (EMA200 on D1 cold-starts ~200 days). Per the Warm-Seed Mandate.

        // Ledger callback -- fires in BOTH shadow and live (GUI/ledger/CSV).

        // L2 capture: while a BreakBounce position is open, append a throttled
        // (5s) snapshot of price + live g_l2_gold imbalance to a CSV keyed by
        // entry_ms. This is the "data to come" -- bb_l2_replay A/Bs the protect
        // on vs off over it once enough trades accumulate. Capture runs even
        // though USE_L2_PROTECT is off: collect first, validate, then enable.

        fflush(stdout);
    }

    // ── IndexSessionEngine x3 (2026-06-01) ─────────────────────────────────────
    // Intraday cash-session LONG, flat overnight. Edge = hold into the US close
    // (SPX/NAS 14-22 UTC; GER40 09-20). Long-only (shorts lose); risk-off gated
    // (the bear guard). OOS Sharpe ~0.3-0.7. SHADOW. The per-tick set_risk_off()
    // and feed are wired in tick_indices.hpp (on_tick_us500/nas100/ger40).
    {
        // 2026-06-04: US500.F leg DISABLED (en=false). Live shadow ledger showed
        // it the entire IndexSession bleed: size/lot=1 = 1 ES contract = $50/pt,
        // i.e. ~50x the per-point risk of the other legs (~$1/pt CFD). One -37pt
        // session-close ride (2026-06-03) = -$1859 = 69% of the whole book's
        // losses. The other legs lose small ($1/pt chop) and stay shadow. Stop-
        // bleed: drop the $50/pt outlier; revisit only with risk-normalised
        // sizing + a working intraday stop + a backtest.
        struct { omega::IndexSessionEngine* e; const char* sym; int oh, ch; const char* d1; bool en; } idx[] = {
            // 2026-06-04: ALL legs DISABLED. IndexSession rides intraday to the
            // cash close with a 2-ATR stop that never fires -> uncapped session-
            // close losses (GER40 -$97.13, the prior US500.F -$1859). Net-negative
            // live + structurally flawed + now REDUNDANT: the index book is
            // FVGcont + OvernightDrift (Sharpe 2.0). Revisit only with a working
            // intraday stop + risk-normalised sizing + a backtest.
            { &g_idxsess_sp,    "US500.F", 14, 22, "phase1/signal_discovery/warmup_US500_D1.csv",     false },
            { &g_idxsess_nas,   "NAS100",  14, 22, "phase1/signal_discovery/warmup_USTEC_D1.csv",     false },
            { &g_idxsess_ger40, "GER40",    9, 20, "phase1/signal_discovery/warmup_GER40_D1_idx.csv", false },
            { &g_idxsess_uk100, "UK100",    9, 20, "phase1/signal_discovery/warmup_UK100_D1.csv",     false },
            { &g_idxsess_estx50,"ESTX50",   9, 20, "phase1/signal_discovery/warmup_ESTX50_D1.csv",    false },
        };
        for (auto& c : idx) {
            c.e->symbol      = c.sym;
            c.e->engine_name = std::string("IndexSession_") + c.sym;
            c.e->RTH_OPEN_H  = c.oh;
            c.e->RTH_CLOSE_H = c.ch;
            c.e->STOP_ATR    = 2.0;
            c.e->SKIP_FRIDAY = true;
            c.e->ENTER_ON_WEAK_ONLY = true;   // dip-buy: SPX OOS Sharpe 0.67->1.48
            c.e->shadow_mode = true;
            c.e->enabled     = c.en;
            c.e->lot         = 1.0;
            c.e->init();
            c.e->seed_from_d1_csv(omega::resolve_seed_path(c.d1));
            c.e->on_trade_record = [](const omega::TradeRecord& tr) { handle_closed_trade(tr); };
            printf("[OMEGA-INIT] IndexSession %s: enabled=%s shadow=true window=%02d-%02d UTC stop=2.0 skipFri\n",
                   c.sym, c.en ? "true" : "false", c.oh, c.ch);
        }
        fflush(stdout);
    }

    // ── FvgContinuationEngine (NAS100, 15m FVG → DOL, NY killzone) ──────────────
    // 2026-06-04 backtest-found edge (backtest/fvg_core.cpp; memory
    // omega-fvg-continuation-nas-edge): NAS100 only — PF 1.65-1.88, WR ~50%, both
    // halves +, 3x-cost-robust, 9/9 param-plateau, cross-validated on 2 NAS
    // datasets. CAVEAT: validated only in the 2025-26 bull regime → SHADOW only.
    {
                                            // live losers): n=11 net -$266 PF 0.65 WR 27% in live
                                            // shadow -- backtest validation (2-dataset 15m) did NOT
                                            // translate. Family total -$649/19 trades. Re-eval only
                                            // with a documented live-vs-backtest divergence root cause.
        // 2026-06-08: publish open position so it shows on the live dashboard (was invisible).
        g_open_positions.register_source("FvgContinuation", []() {
            std::vector<omega::PositionSnapshot> v;
            omega::PositionSnapshot s;
            return v;
        });
        printf("[OMEGA-INIT] FvgContinuation NAS100: shadow=true 15m NY-killzone(13:30-15:00 UTC) "
               "gap>=1.0ATR dol<=3ATR fresh<=8\n");

        // 10m variant — best HTF in the sweep (PF 2.37, Sharpe/yr 2.03, ret/DD
        // 11.5, both halves +, 3x-cost-robust, 9/9 param-plateau). Single-dataset
        // validated (can't build 10m from the 15m pkl), so run it in SHADOW
        // alongside the 2-dataset-validated 15m and let live data pick the winner.
        // 2026-06-09: 10m is the better FVG variant (sweep PF1.61 vs 15m 1.27) -> make it visible too.
        g_open_positions.register_source("FvgCont10m", []() {
            std::vector<omega::PositionSnapshot> v;
            omega::PositionSnapshot s;
            return v;
        });
        printf("[OMEGA-INIT] FvgCont10m NAS100: shadow=true 10m NY-killzone "
               "gap>=1.0ATR dol<=3ATR fresh<=8 (best-HTF shadow compare)\n");

        // 30m variant -- 2026-06-09 exhaustive FVG sweep WINNER: PF1.98 (3x-cost 1.91),
        // both halves ~2.0, ret/DD 5.12, WR46%. Beats 10m(1.61) and 15m(1.27). ungated.
                                              // live (sweep WINNER PF1.98 backtest -- worst live
                                              // translation in the book; -$417 family bleed on 06-12 alone).
        g_open_positions.register_source("FvgCont30m", []() {
            std::vector<omega::PositionSnapshot> v;
            omega::PositionSnapshot s;
            return v;
        });
        printf("[OMEGA-INIT] FvgCont30m NAS100: shadow=true 30m NY-killzone (sweep WINNER PF1.98 3x-robust)\n");

        // S-2026-06-13g FVG FAMILY DEDUP (operator: "we agreed to dedup -- why
        // 2 trades firing at the same time"). 2026-06-12 ledger: FvgContinuation
        // + FvgCont30m booked the IDENTICAL NAS100 long twice (13:30:03 and
        // 13:30:19, same entry px) -- ClusterGate caps same-direction US_EQUITY
        // at 2, so the pair passed. Family permit: an FVG variant may only enter
        // when ALL THREE variants are flat (first-to-fire wins; the A/B/C
        // comparison continues via per-variant EngineGate stats, just without
        // concurrent duplicate risk -- same tradeoff accepted for the pump trio
        // on 2026-06-11).
        {
            auto fvg_family_flat = []() {
            };
            (void)fvg_family_flat;
            printf("[OMEGA-INIT] FVG family dedup: 1 position across 10m/15m/30m (first-to-fire)\n");
        }

        // ── PeachyOrbEngine (NAS100, one-candle ORB-retest, risk-cap) ──────────
        // 2026-06-05 backtest-found edge (backtest/peachy_orb_nas.cpp +
        // peachy_sweep.sh; memory omega-peachy-onecandle-orb-deadend). The Peachy
        // "one-candle theory" LITERAL rules backtested DEAD (WF half-flip); the
        // real edge is her unstated risk-SELECTIVITY (only ~20-30pt-stop setups).
        // Config C (OR15 1330-open, body0.6, retr0.3, maxStop=1.0ATR risk-cap,
        // closeBuf0.3ATR, EMA100 trend, 2.5R, 1-shot/day, long-only):
        //   BULL (16mo NAS): n=103 PF 2.19 (H1 2.06/H2 2.34), 9/9 plateau, 3x-robust.
        //   BEAR (real 2022 NDX -30%): PF 2.25 net+313 maxDD 91pt, 3x:1.97.
        //   ==> bull AND bear robust (catches bear-bounces; tight stop = small
        //   losers). Volume filter is dead on proxy → no volume gate. SHADOW first.
        // ===== REVIVED 2026-06-13 (S-2026-06-13t) -- KILLER FOUND: the runner trail =====
        // Tombstoned r briefly (deployed config PF0.46 on current tape). Filter
        // ablation on 5 weeks of REAL NAS tape (May 6 - Jun 12 2026, 6.3M ticks,
        // n=14) isolated the killer: the runner-trail (trail_atr=3.0), NOT any entry
        // filter. The 2026-06-09 sweep that switched to the trail (+43% claim) was
        // OVERFIT to old data -- on current tape it gives back every breakout on the
        // choppy reversals (WR 64->46%, maxDD 1.1R->3.1R, PF 2.5->0.9).
        // ABLATION (fixed 2R vs each filter added):
        //   permissive fixed-2R: PF2.51 | +maxStop1.0: 2.51 (inert) | +EMA100: 2.51
        //   (inert) | +body0.4: 2.34 | +closeBuf0.5: 1.35 (hurts) | +trail3.0: 0.92 (KILL)
        // FIX = revert to a FIXED 2R TP (the original 2022 exit style was fixed 2.5R,
        // never the trail). Validated config (body0.4 maxStop1.0 EMA100, closeBuf OFF,
        // fixed 2R): PF2.34 WR61% maxDD1.1R; 3x-cost PF2.18; H1 PF3.36 / H2 PF2.06
        // (both halves +). MEETS the re-enable bar. Harness backtest/peachy_orb_nas.cpp,
        // tape /tmp/nas_5wk.csv. lot stays 0.3 (dollar-normalized). n=14/5wk still
        // modest -> shadow, watch the live ledger.
        //   NAS M1 data this session (the 5wk re-enable was slice-luck -- full 2024-2026 NAS Sharpe ~0.55,
        //   FVG/ORB-family slice artifacts); live shadow ledger -$253 over 4 closes, 0% WR. Both backtest +
        //   forward agree = dead. Do NOT re-enable without cross-regime (2022 incl) walk-forward proof.
        //   2026-06-15 CROSS-REGIME LEVER SWEEP DONE (operator: "she had it working"): full span
        //   2022-bear + 2024-2026 NAS, swept body{0.5-0.7} x tpR{2-3} x closeBuf{0-0.5}. In ISOLATED
        //   2022-bear her body0.6 config is brilliant (PF 3+) = the slice-luck source. But across the
        //   FULL span EVERY config is PF<1.1 (her documented winner b0.6/tp2.5/cb0.3 = PF0.93; best
        //   any-config = PF1.05; maxDD 28-41R). The discretionary edge does NOT mechanize cross-regime.
        //   CULL CONFIRMED. Tools: /tmp/peachy_sweep (backtest/peachy_orb_nas.cpp), /tmp/nas_fullspan.csv.
        // 2026-06-08: publish open position so it shows on the live dashboard (was invisible).
        printf("[OMEGA-INIT] PeachyOrb NAS100: shadow=true OR15(13:30-13:45 UTC) "
               "body0.6 retr0.3 maxStop1.0ATR EMA100 2.5R 1-shot long-only\n");

        // PeachyOrb GER40 REMOVED 2026-06-10: held-out OOS (GER40 5m, May11-Jun02,
        // the window BEFORE its 06-08/09 discovery) failed to reproduce PF2.06 —
        // net-negative at every cost (PF 0.75@0.5pt..0.65@3pt), no MINSTOP floor
        // rescued it. Overfit to the discovery window; live ran 0-second SL_HIT
        // churn. Engine instance deleted (PeachyOrbEngine class kept for NAS).

        // ── PumpScalpManager (micro-cap pump scalp, DYNAMIC universe) ──────────
        // Trades whatever explodes today (3m per pumping symbol; S-2026-06-11
        // trio->3m-only — 1-pos/symbol made 5m/15m pure slower-entry duplicates;
        // 3m is the backtested winner, pump_tf_bt.py), fed by
        // pump_feed_bridge.py via PumpFeedConsumer when OMEGA_PUMP_BRIDGE=1 (else
        // dormant — zero effect on the live service). gate>=100% (extreme movers),
        // HARD trail 3%, pyramid OFF, strict-exhaustion shorts. Validated
        // 2026-06-10 (memory pump-scalp-ah-momentum-edge): durable multi-month OOS,
        // survives 1-2%/side slip. Registers with g_open_positions so its trades
        // show in the live_trades GUI panel + ring the entry bell. SHADOW until
        // live fills + dud-rate are measured.
                                              // the penny stocks ... which we agreed are rubbish".
                                              // The 06-13a shadow re-enable lasted ~6h of data
                                              // gathering; operator direction is BIG CAPS ONLY --
                                              // g_bigcap_momo below is the continuation. Do NOT
                                              // re-enable the penny universe without an explicit
                                              // operator instruction.
        // S-2026-06-11 RECALIBRATION (operator model + full lever sweep,
        // pump_recalib_bt.py / cap_probe.py on 16-day basket, net$ @ $5k notional,
        // cost-inclusive both 1%/2% slip): jump in on the +100% mover, TRAIL
        // IMMEDIATELY at 2%, exit when price turns (trail) with a 15-min time
        // backstop. Findings: trail 0.5-1% = slippage artifact (trail inside the
        // 1-2% cost band, fantasy fills) — rejected. Strict 3-min cap (cap=1) was
        // the WORST cap in every trail row (cuts winners at bar 1); 15-min
        // (cap=5) nets more on BOTH monster + non-monster names. BE-lock dropped
        // (operator's simple model; sweep showed it marginal). Edge survives
        // removing the SLGB monster (~+$22k/15d ex-monster @ $5k), but sample is
        // small (n=44, 16d, 1 monster=62% of gross) => SHADOW, fat-tail dependent.
        // S-2026-06-11 RE-ENTRY CAP: live shadow showed CHOW entered 4x (+50,-32,
        // -38,-39 = re-entry chop bleed). reentry_cap_bt.py (16-day basket, deployed
        // cfg): cap2 = keeps 84% of net + best PF (42) vs unlimited (PF18, chop) or
        // cap1 (kills the edge: $13.6k->$1.7k -- monster continuations need a re-add).
        // S-2026-06-12b cap 2 -> 4 (operator-approved): pump_lever_bt.py (Yahoo 5m
        // extended-hours, 98 mover name-days incl. fresh OOS 06-11, both 1%/2% slip):
        // maxent4 n97 +$5485 PF4.3 vs maxent2 n58 +$3659 PF4.8 @2% -- +50% net, PF
        // holds, broad (top name 14%, ex-top2 +$4085), both halves +. Tension with
        // the 06-11 3m result (cap2 best PF) acknowledged -- this SHADOW run is the
        // prod-faithful A/B that settles it via EngineGate honest accounting.
        // S-2026-06-11 ANTI-SLIPPAGE recalibration (operator: "we cannot be caught
        // with the 5% issue"). Notional $5000->$1000 (smaller order walks the thin
        // book far less) + LIQUIDITY GATE: only trade names priced >=$1 with bar
        // $-volume >=$2M, so a $1k order is <0.05% of a bar's flow and the 2%
        // trail exits near the quote. liq_calib.py over the FULL last-month
        // universe (mover_scan.py: 173 names >100%): the gate RAISED net@2% to
        // +$22.7k AND cut the @5%-slip worst case from -$37k to -$7.4k. The thin
        // sub-$1 rockets (the slippage traps) are now skipped by design.
        printf("[OMEGA-INIT] PumpScalp manager: DISABLED (operator kill 2026-06-13 -- penny universe retired; "
               "BigCapMomo is the mover engine)\n");

        // ── BigCapMomo (NAS/SPX big-cap day-mover scalp; 5m) ────────────────────
        // 2026-06-12 (operator): the micro-cap pump died on SLIPPAGE, not momentum.
        // Same engine, BIG-CAP config: only scalp deep-liquidity NAS/SPX names that
        // are already up >= DAY_GATE on the session, ride continuation, exit on the
        // trail. Backtest bigcap_scalp_sweep.py (5m, ~2-3mo, 508 NAS/SPX names):
        //   day-gate 5% + trail 3% => PF 1.79 (n52);  gate 3% + trail 3% => PF 1.46
        //   (n149). A TIGHT 1-1.5% trail LOSES (stopped on noise) -- big-cap intraday
        //   moves need room, so trail 3%. CAVEAT: 2-3mo / one regime / thin n / 8bps
        //   slip assumed -> SHADOW until live-shadow confirms fills + frequency.
        g_bigcap_momo.enabled      = false;    // S-2026-06-28 CULL (faithful retest). The only profitable
                                               // config (PF1.98) is the UNCAPPED small-cap gapper universe the
                                               // $500B cap-fix is meant to EXCLUDE; the mega-cap-capped config
                                               // fires 2 trades/3mo (untradeable); data bull-only; live PF0.72.
                                               // No lever/gate rescues it. Dead -> disabled.
        g_bigcap_momo.shadow_mode  = true;
        g_bigcap_momo.tf_sec       = 300;      // 5m entry bars (validated TF)
        g_bigcap_momo.label        = "BigCapMomoCons";  // S-2026-06-20b: distinct ledger tag so the
                                               // bridge (conservative gate4.0) is separable from the
                                               // in-process IBKR engine (aggressive gate1.5, tag
                                               // "BigCapMomo") when BOTH run (operator A/B). Same scanner
                                               // universe -> bridge trades ~= subset of IBKR; this is an
                                               // aggressive-vs-conservative A/B, NOT 2x unique trades.
        g_bigcap_momo.day_gate_pct = 2.5;      // S-2026-06-23 4->2.5: deep faithful sweep (bigcap_momo_faithful.cpp,
                                               // real engine, 5m 60d, $20B floor) — gate2.5 ~2x trades; the new
                                               // breadth>=2 gate below skips the isolated single-name chop the
                                               // looser gate would otherwise admit. Prior: S-2026-06-12b 5->4.
                                               // sweep+stress (bigcap_sweep_ext/stress_ext, cached
                                               // 508-name 5m): gate4 n84 vs gate5 n51 (+65% trades)
                                               // AND higher PF at every slip tier (see trail note).
        g_bigcap_momo.trail_pct    = 0.0;      // S-2026-06-18: %-trail OFF; ATR-trail replaces it (below).
                                               // config: PF 2.37/2.14/2.00/1.73 @8/15/20/30bps vs
                                               // deployed gate5+trail4 1.83/1.65/1.53/1.32. gate3
                                               // tripled trades but died by 30bps (PF1.28) -- skipped.
                                               // Tight (<=2.5%) trails die by 20bps. Moves need room.
        g_bigcap_momo.volx         = 0.0;      // S-2026-06-13k OFF live: bridge bar volume = deltas
                                               // of delayed-feed cumulative ticker.volume -- spiky/
                                               // unreliable units vs the sweep's clean Yahoo bars
                                               // (observed absurd $bn-scale bar dvol). Same call as
                                               // the 2026-06-10 pump VOLX kill. Selectivity remains:
                                               // gate4% + ignition 3%/3-bars + strength 0.60.
                                               // DIVERGENCE FROM VALIDATED CONFIG -- EngineGate
                                               // shadow stats are the revalidation.
        // S-2026-06-18 GAIN-PROTECTION EXIT (faithful bigcap_sweep R_BEST_atr30x4_be3fl2_ride:
        // net+460 PF4.72 net30+363 both-WF-halves+ exTop5+147 ROBUST vs %-trail baseline +303/2.30).
        // The live give-back problem (sum-MFE +$624 -> net -$22, ~$451 returned; QURE/NTLA/PRAX
        // clocked out mid-run) is fixed by: ATR-trail rides the move + BE-ratchet locks gains once
        // +3% + ride-in-profit lets winners run past the time backstop. SHADOW; 60d/1-regime ->
        // gather live fills before any live-size. Manifest: BigCapMomo SHADOW-CANDIDATE.
        g_bigcap_momo.atr_len      = 30;       // ATR-trail length (best robust exit)
        g_bigcap_momo.atr_mult     = 5.0;      // S-2026-06-23 4->5: sweep — wider trail rides winners further (5>4>3).
        g_bigcap_momo.be_arm_pct   = 2.0;      // S-2026-06-23 3->2: tighter gain-lock lifts WR (arm BE-floor at +2%).
        g_bigcap_momo.be_floor_pct = 1.0;      // S-2026-06-23 2->1: floor at entry +1% (net-BE after slip).
        g_bigcap_momo.giveback_close_frac = 0.35;  // S-2026-06-24 LIVE: bank a runner on a 5m close
                                                   // retraced 35% of peak gain (real-trade replay: book
                                                   // -$133 -> +$160). Same fix on the bridge instance.
        g_bigcap_momo.maxhold_skip_if_profit = true;  // ride winners past the clock (don't cut QURE mid-run)
        g_bigcap_momo.maxhold_bars = 96;       // 96 x 5m = 8h backstop (losers only; in-profit rides)
        g_bigcap_momo.min_breadth  = 2;        // S-2026-06-23 CHOP/BEAR GATE: require >=2 distinct names igniting
                                               // same session-day before any entry. Faithful BT (bigcap_momo_faithful.cpp
                                               // merged-timeline): chop third -12%->-2% (83% of bleed removed), cost-robust
                                               // (PF11.8 @+30bps), maxDD$177->$66; bears have few broad-ignition days -> sits
                                               // out = structural bear protection. SHADOW; magnitudes bull-inflated (60d/1-regime).
        g_bigcap_momo.pyr_adds     = 0;
        g_bigcap_momo.max_entries_per_day = 1;   // S-2026-06-23b 2->1: kills re-entry-into-the-top (operator saw
                                                 // IONQ #1 scratch +$7 then #2 @HOD -> -$58). FAITHFUL on the FULL
                                                 // LIVE UNIVERSE (514 names, 60d, 5m, bigcap_momo_faithful):
                                                 // cap2 n62 PF2.51 +$957 -> cap1 n55 PF2.86 +$996, both-halves+
                                                 // (H1 2.21/H2 3.88), top3=28.8% of win (not fat-tail), cost-robust
                                                 // (+15bps/side PF2.36 +$830). Best net AND best PF. (Prior
                                                 // reentry_cap_bt cap2 was a thin bull-continuation sample.)
        g_bigcap_momo.entry_max_ext_pct = 0.0;   // S-2026-06-23b TESTED, LEFT OFF. Anti-chase ext-cap looked good
                                                 // on the 30-name mega-cap bin (n10) but on the FULL live universe
                                                 // it CUTS net: ext4 +$707 / ext3 +$644 < ext0 +$996. The extended
                                                 // ignition longs are net-POSITIVE in aggregate (running pumps pay
                                                 // for the fades) -- filtering them removes winners too. Plumbing
                                                 // kept (BC_EXT lever) but off. Don't re-enable without full-universe BT.
        g_bigcap_momo.notional_usd = 2000.0;   // S-2026-06-26 operator: DOUBLE the cons lot (1000->2000) on the
        // selective config's numbers (PF5.44, 8/39 losers vs the looser mover's 23/49). SHADOW -> doubles the
        // shadow $ + the peak deployed capital (17 concurrent => ~$34k peak); realized-DD still understates the
        // correlated-selloff risk (see memory project-revisit-scale-mgcfastdon-bigcapmomo). Scale again only on a green live ledger.
        g_bigcap_momo.slip_pct     = 0.15;     // big-cap realistic (vs micro 1.0%)
        g_bigcap_momo.min_dvol_usd = 0.0;      // S-2026-06-13k ZERO-TRADES ROOT CAUSE: $100M per
                                               // 5-MINUTE bar = $12B/day turnover -- virtually no
                                               // stock passes; the validated sweep (bigcap_scalp_
                                               // sweep.py) had NO dvol gate at all. Liquidity is
                                               // already enforced upstream by the scanner (cap
                                               // >=$2B + price>=$10 + TOP_PERC_GAIN universe).
        g_bigcap_momo.price_min    = 10.0;     // not a penny stock
        g_bigcap_momo.verbose      = true;
        g_bigcap_momo.on_trade_record = [](const omega::TradeRecord& tr) { handle_closed_trade(tr); };
        g_open_positions.register_source("BigCapMomoCons", []() { return g_bigcap_momo.collect_positions(); });
        printf("[OMEGA-INIT] BigCapMomo manager: 5m gate2.5%% breadth>=2 ATR-trail(30x5) BE-ratchet(arm2/floor1) "
               "MAXENT1 ride-in-profit 8h-backstop NO-volx NO-dvol-gate $1000-notional p>=10 "
               "slip0.15%%/side shadow (single-entry S-2026-06-23b, full-universe BT PF2.86; feed via OMEGA_BIGCAP_BRIDGE=1)\n");

        // ── A/B EXIT TWIN (S-2026-06-24, OMEGA_BIGCAP_AB=1, default OFF) ──────────
        // g_bigcap_momo_b = IDENTICAL config to g_bigcap_momo (copied field-by-field
        // from it below, so entries CANNOT diverge) + ONE change: a CLOSE-based give-
        // back exit (bank a runner once a 5m CLOSE retraces 35% of the peak gain).
        // Both shadow; same bridge feed (tee'd in omega_main); distinct ledger tag
        // "BigCapMomoGB". Tests "bank the reversal vs ride the wide trail" on IDENTICAL
        // entries -- the live-shadow answer the offline data (wrong universe) can't give.
        // Off by default: when OMEGA_BIGCAP_AB is unset, this block + the feed are skipped
        // -> production (g_bigcap_momo) is byte-identical.
        if (std::getenv("OMEGA_BIGCAP_AB")) {
            auto& a = g_bigcap_momo; auto& b = g_bigcap_momo_b;
            b.enabled=a.enabled; b.shadow_mode=true; b.tf_sec=a.tf_sec;
            b.day_gate_pct=a.day_gate_pct; b.hard_pct=a.hard_pct; b.min_breadth=a.min_breadth;
            b.trail_pct=a.trail_pct; b.be_arm_pct=a.be_arm_pct; b.be_floor_pct=a.be_floor_pct;
            b.atr_len=a.atr_len; b.atr_mult=a.atr_mult; b.atr_mult_tight=a.atr_mult_tight;
            b.pscale_full_pct=a.pscale_full_pct; b.giveback_frac=a.giveback_frac;
            b.struct_lb=a.struct_lb; b.rollover_ema=a.rollover_ema;
            b.maxhold_skip_if_profit=a.maxhold_skip_if_profit; b.volx=a.volx; b.pyr_adds=a.pyr_adds;
            b.notional_usd=a.notional_usd; b.slip_pct=a.slip_pct; b.min_dvol_usd=a.min_dvol_usd;
            b.price_min=a.price_min; b.maxhold_bars=a.maxhold_bars; b.max_symbols=a.max_symbols;
            b.stale_sec=a.stale_sec; b.max_entries_per_day=a.max_entries_per_day;
            b.entry_max_ext_pct=a.entry_max_ext_pct; b.verbose=a.verbose;
            b.label="BigCapMomoGB";
            b.giveback_close_frac=0.35;   // THE A/B difference: bank on a 35% close-retrace from peak
            b.on_trade_record = [](const omega::TradeRecord& tr) { handle_closed_trade(tr); };
            g_open_positions.register_source("BigCapMomoGB", []() { return g_bigcap_momo_b.collect_positions(); });
            printf("[OMEGA-INIT] BigCapMomo A/B TWIN 'BigCapMomoGB' ARMED: identical entries + close-based "
                   "give-back 0.35 (vs wide trail). Fed when OMEGA_BIGCAP_AB=1.\n");
        }

        // ── NqMomentumEngine (S-2026-06-18) ──────────────────────────────────
        // Regime-gated intraday momentum-continuation on NAS100/NQ. Same exit
        // chassis as BigCapMomo (ATR-trail + BE-ratchet + ride-in-profit) but a
        // SINGLE liquid instrument (the NAS100 index tick path) => no micro-cap
        // slippage. Fed in tick_indices.hpp on_tick_nas100. Ledger writes via the
        // bracket_on_close callback (same as the index straddle cells).
        // ⚠ DISABLED S-2026-06-19 — clean-data re-validation FAILED the wiki claim.
        // The "+both regimes both-WF-halves+ PF2.34/1.26" provenance below was run on
        // DOWNSAMPLE-GRADE data (10x + a missing 2023; nq_momentum_faithful.cpp also had
        // a HHMMSSmmm time-parse bug shifting ticks ~750d — both fixed S-2026-06-19).
        // Re-run on CLEAN continuous NSXUSD 2022-2026 (271M ticks, cost 3pt, gate ON):
        //   BASELINE PF1.01 net+616pt, WF NOT both-halves+ (H1 -1874 / H2 +2490) = bull-
        //   biased / breakeven. +ATR-BE -> net-2493 ; +loss_cut2.0 -> net-1817 (protection
        //   makes it WORSE — see ADVERSE-PROTECTION verdict in NqMomentumEngine.hpp).
        // Marginal edge + protection-resistant => PAUSE pending a real edge re-validation
        // on the live instrument (USTEC.F CFD), NOT the NSXUSD/NAS100 cash proxy.
        // Original (now-suspect) provenance: bull n129 PF2.34 +1395; bear n40 PF1.26 +426.
                                                // (nq_momentum_faithful.cpp, cross-regime) — tighter
                                                // trail lifts bull PF 2.60->3.24 (+1645pt, more trades,
                                                // lower DD) while bear stays both-WF-halves+ (PF1.18).
                                                // 2.0/2.5 over-tighten (bear H1 flips neg); 3.0 = robust optimum.

        // ── BigCapMomo IN-PROCESS IBKR engine (2026-06-16) ───────────────────
        // SAME validated big-cap momentum continuation edge as g_bigcap_momo
        // above, but running on its OWN IBKR scanner/data thread INSIDE Omega.exe
        // -- NOT the standalone ibkr/BigCapMomoEngine.cpp exe, and NOT the Python
        // :7784 bridge. This is the path that shows BOTH running + closed trades
        // in the GUI, because that panel is in-process telemetry (g_open_positions
        // + shared mem) a separate exe cannot inject into. Trading params mirror
        // the live-validated g_bigcap_momo values (gate4 / trail5% / volx-off /
        // regime-gate / $1000 shadow). Connection from env (defaults: live gateway
        // 4001, clientId 86 -- US-equity scanner needs the live entitlement; paper
        // 4002 returns nan prices). ACTIVATED by OMEGA_BIGCAP_IBKR=1 in omega_main
        // (set_enabled + start); off => dormant, collect_positions returns empty
        // so there are NO double GUI rows vs the bridge path. Use ONE path at a
        // time (OMEGA_BIGCAP_IBKR xor OMEGA_BIGCAP_BRIDGE).
        {
            // EXIT-SYNC DONE 2026-06-18: validated gain-protect exit PORTED into BigCapMomoIbkr
            // (ATR-trail30x4 + BE-ratchet arm0.03/floor0.02 + ride-in-profit; +52% / PF2.30->4.72,
            // bigcap_exit_compare.cpp). 4001 = PAPER account (data-entitled) -> set paper_only=false
            // + OMEGA_BIGCAP_IBKR=1 to start logging REAL paper fills + real slippage. KEEP the
            // big-cap liquidity floor tight (px_min + scanner cap>=$2B) so micro-caps can't leak in.
            omega::bigcap_momo_ibkr::Config bc;
            bc.min_breadth  = 2;       // S-2026-06-23 CHOP/BEAR GATE (ported from validated bridge): require
                                       // >=2 distinct names igniting same session-day. Faithful bridge BT:
                                       // chop third -12%->-2%, maxDD halved, bear = sits out low-breadth days.
            bc.gate_pct     = 2.5;     // S-2026-06-23 1.5->2.5: align to the validated bridge config. Day-gate
                                       // is only the "name in play" prefilter; entry still must clear the
                                       // 3 chop guards below (regime + ignition + impulse). Backtest
                                       // (/tmp/bigcap_freq.py, 10 mega-cap 2yr 15m): gate1.5 + impulse1.0
                                       // = PF3.51 WR66% DD6.5% ~6.5 trades/wk on 10 names (x full live
                                       // universe) vs gate4 PF6.83 1.34tw. ~5x the trades, PF still >3.
            bc.min_impulse_atr = 0.0;  // S-2026-06-23 1.0->0: faithful sweep (bigcap_momo_faithful.cpp,
                                       // breadth=2) shows impulse is IMMATERIAL once breadth gates chop
                                       // (the breadth>=2 gate IS the chop guard now). Aligns to validated.
                                       // (prior chop-guard rationale below was pre-breadth) -- thrust
                                       // >=1 ATR. /tmp/bigcap_chop.py @gate1.5: OFF=PF1.48 DD18.9%,
                                       // ON=PF3.51 DD6.5% -- discards 61% of entries = the chop trades.
            bc.trail_pct    = 0.0;     // %-trail OFF; ATR-trail replaces it (matches deployed g_bigcap_momo)
            bc.atr_len      = 30;      // ATR-trail (validated)
            bc.atr_mult     = 5.0;     // S-2026-06-23 4->5: wider trail rides winners (validated sweep)
            bc.be_arm_pct   = 0.02;    // S-2026-06-23 0.03->0.02: tighter gain-lock lifts WR
            bc.be_floor_pct = 0.01;    // S-2026-06-23 0.02->0.01: floor at entry +1%
            bc.maxhold_skip_if_profit = true;  // ride winners past the clock
            bc.paper_only   = false;   // 4001 = PAPER account -> route paper orders = REAL fills + real
                                       // slippage, ZERO capital risk. Still dormant until OMEGA_BIGCAP_IBKR=1
                                       // (master enable in omega_main) AND OMEGA_BIGCAP_BRIDGE off (xor).
            bc.volx         = 0.0;     // OFF live (realtime-bar volume = deltas, not surge-comparable)
            bc.ig_pct       = 3.0;
            bc.lb           = 3;       // S-2026-06-23 6->3: align to validated config (the only MATERIAL
                                       // entry lever per the faithful sweep -- strength/VWAP-filter immaterial).
                                       // LB3 = higher PF + 90% WR (LB6 = 2.4x trades, lower PF). 3*5m=15min.
            bc.maxhold      = 96;      // 96*5m = 8h backstop (losers only; in-profit rides past it)
            bc.px_min       = 10.0;    // not a penny stock
            bc.giveback_close_frac = 0.35; // S-2026-06-24 LIVE: bank a runner on a 5m close retraced 35%
                                           // of peak gain. Validated on the REAL live ledger trades
                                           // (backtest/coldcut_on_real_trades.py): book -$133 -> +$160.
                                           // Fixes the give-back losers (winner peaks then round-trips on
                                           // the wide trail). Shadow; watch the live ledger vs prior.
            bc.market_cap_above_musd = 20000.0;  // S-2026-06-23 $500B->$20B: align to validated bridge floor;
                                                // the $20-100B band carries the momentum (2026 semis), breadth>=2
                                                // handles the chop/bear the wider universe admits. (musd unit.)
                                                // S-2026-06-20c (prior, superseded): $500B mega-only. Note re old
                                                // small-cap caveat -- $100M floor traded
                                                // small-cap gappers (AEHR/PBLS/SHAZ) = live PF0.89 LOSING
                                                // (universe != the validated mega-cap set). Per-name +
                                                // cap-tier faithful re-test (bigcap_revalidate.py, 30 names
                                                // 2024-26): edge is monotonic in mcap -- >=$500B PF1.57
                                                // both-halves+ (NVDA/ORCL/PLTR/CRWD); $150-300B decays to
                                                // ~1.0; <$150B no edge. $500B = the correct universe.
                                                // TRADEOFF: low freq (~2/wk, megas rarely gap) -- inherent,
                                                // no floor gives edge+high-freq. millions unit ($500B=500000).
                                                // bull-span only (no bear test). Revert toward $300B for
                                                // slightly more names if scanner returns too few rows.
            bc.regime_gate  = true;    // SPY price>SMA200 AND SMA200 rising
            bc.notional_usd = 1000.0;
            bc.paper_only   = true;    // SHADOW: log trades, route NO live orders (paper fills)
            // S-2026-06-26 ACTIVATE Luke ARM-AND-WAIT on this in-process instance (paper/shadow).
            // The A/C daily setup ARMS a breakout trigger (luke_trig)+tight stop; entry = the intraday
            // BREAK of it (validated PF1.77 Sharpe0.74, IBKR top-111 ADR>=6). REPLACES ignition here;
            // the bridge instance (BigCapMomoCons) stays ignition -> live A/B on the shadow ledger.
            // Regime gate preserved (bc.regime_gate). env OMEGA_BIGCAP_LUKE=0 disables (revert to ignition).
            bc.luke_gate     = true;
            bc.luke_mode_A   = true;
            bc.luke_mode_C   = true;
            bc.luke_max_stopw= 0.06;
            bc.luke_adr_min  = 4.0;    // S-2026-06-26 6.0->4.0: ADR>=6 armed 0 setups in 17h live (too tight);
                                       // ADR>=4 ~5x more candidates (BT PF1.35 @adr4 vs 1.77 @adr6, but trades).
            if (const char* lk = std::getenv("OMEGA_BIGCAP_LUKE")) bc.luke_gate = (std::atoi(lk) != 0);
            // S-2026-06-26 fixed high-ADR daily-swing watchlist (the gainer scan misses the QUIET
            // names where A/C setups form -> 0 armed). These are evaluated for setups every day.
            bc.luke_watchlist = {
                "NVDA","AMD","MU","MRVL","SMCI","ARM","AVGO","COIN","MSTR","PLTR","SHOP","CRWD","SNOW",
                "NOW","PANW","ORCL","INTC","DELL","UBER","NFLX","QCOM","ANET","STX","FTNT","KMX","DLTR",
                "LRCX","KLAC","AMAT","MPWR","ON","MRNA","APP","CVNA","HOOD","CRDO" };
            // S-2026-06-26s NEVER-AGAIN GUARD: the bare "BigCapMomo" tag is the raft-
            // of-bad-trades variant (old gate1.5/no-breadth IBKR run, 22 losers net
            // -$72 purged from the ledger 2026-06-26). The aggressive config is GONE
            // (now gate2.5/breadth>=2 above), but pin the tag so the bare "BigCapMomo"
            // label can NEVER be written to the ledger/GUI again regardless of env.
            bc.engine_tag   = bc.luke_gate ? "BigCapMomoLuke" : "BigCapMomoIbkr";
            if (const char* h = std::getenv("OMEGA_BIGCAP_IBKR_HOST"))   bc.host      = h;
            if (const char* p = std::getenv("OMEGA_BIGCAP_IBKR_PORT"))   bc.port      = std::atoi(p);
            if (const char* c = std::getenv("OMEGA_BIGCAP_IBKR_CLIENT")) bc.client_id = std::atoi(c);
            if (const char* m = std::getenv("OMEGA_BIGCAP_IBKR_MDTYPE")) bc.market_data_type = std::atoi(m);
            // S-2026-06-24 not-BEAR gate A/B (Tier-1: not-BEAR > none > BULL-only on trend-long).
            // Default OFF = production BULL-only. OMEGA_BIGCAP_RELAXED_GATE=1 -> block only confirmed
            // downtrend (trade BULL+NEUTRAL). Shadow A/B; bear protection preserved either way.
            if (std::getenv("OMEGA_BIGCAP_RELAXED_GATE")) bc.regime_relaxed = true;
            omega::bigcap_momo_ibkr::configure(bc);
            omega::bigcap_momo_ibkr::set_on_trade_record(
                [](const omega::TradeRecord& tr) { handle_closed_trade(tr); });
            g_open_positions.register_source("BigCapMomoIbkr",
                []() { return omega::bigcap_momo_ibkr::collect_positions(); });
            printf("[OMEGA-INIT] BigCapMomo IN-PROCESS IBKR engine wired (gate1.5%% impulse1.0ATR "
                   "regime-gate ig3%% $1000 shadow); activate with OMEGA_BIGCAP_IBKR=1\n");
        }

        // ── NqFutMomo IN-PROCESS IBKR engine (NQ/MNQ futures momentum, S-2026-06-25) ──
        // !! DORMANT / NOT COST-ROBUST -- never enable (OMEGA_NQ_IBKR stays unset). !!
        // Intraday momentum-continuation on a SINGLE liquid index future with the BigCapMomo
        // exit chassis (fixed-ATR-at-entry trail + BE-ratchet + ride). The original "PF 2.27
        // @2pt / 1.89 @4pt both-halves+" was a TIMESTAMP-PARSER ARTIFACT in momo_cont_nq.cpp
        // (HHMMSSmmm read as HHMMSS -> 78.7M garbage "5m bars"; same bug that tombstoned the
        // old NqMomentumEngine). CORRECTED re-BT (momo_cont_nq_ls.cpp, 184127 real bars):
        // LONG PF1.34@2pt (bull-skewed, bear breakeven) but FAILS @4pt 2x-cost (WF-H1 -459,
        // bear -593 = NOT both-halves, NOT both-regime); SHORT dead PF0.95. Engine CODE is
        // correct (consumes real IBKR 5m bars; bug was offline-harness only) -- kept here
        // dormant + wired for a future cost-robust futures edge, but it does NOT pass the
        // deploy bar today. collect_positions stays empty unless OMEGA_NQ_IBKR=1 (do not set).
        // DISTINCT from the tombstoned NqMomentumEngine (g_nq_momentum, on_tick class path).
        {
            omega::nq_momo_ibkr::Config nc;
            nc.symbol       = "MNQ";    // micro NQ ($2/pt) for small shadow size; "NQ" = $20/pt
            nc.point_value  = 2.0;      // MNQ; set 20.0 for NQ
            nc.contracts    = 1;
            nc.ig_pct       = 0.4;      // validated momo_cont_nq.cpp config (verbatim)
            nc.lb           = 6;        // 30min ignition lookback
            nc.regime_gate  = true;     // close > SMA200 of own 5m closes (self-gate)
            nc.regime_sma   = 200;
            nc.atr_len      = 30;
            nc.atr_mult     = 4.0;      // wide ATR-trail, ATR captured AT ENTRY (held fixed)
            nc.be_arm_pct   = 0.03;     // BE-ratchet (adverse protection -- tight cut GUTS the trend edge)
            nc.be_floor_pct = 0.02;
            nc.maxhold      = 48;       // 4h backstop; rides a still-profitable winner past it
            nc.maxhold_skip_if_profit = true;
            nc.cost_pts     = 2.0;      // log-only (validated cost-robust to 4pt = $8/MNQ round trip)
            nc.paper_only   = true;     // SHADOW: log trades, route NO live orders
            nc.engine_tag   = "NqFutMomo";
            // CONTFUT continuous-future data by default (never dead on a roll). Set a front
            // month (OMEGA_NQ_IBKR_MONTH=202609) to pin a real FUT contract for live routing.
            if (const char* sym = std::getenv("OMEGA_NQ_IBKR_SYMBOL")) {
                nc.symbol = sym;
                if (std::string(sym) == "NQ") nc.point_value = 20.0;
            }
            if (const char* pv = std::getenv("OMEGA_NQ_IBKR_POINTVAL")) nc.point_value = std::atof(pv);
            if (const char* mo = std::getenv("OMEGA_NQ_IBKR_MONTH"))    nc.last_trade_month = mo;
            if (const char* h  = std::getenv("OMEGA_NQ_IBKR_HOST"))     nc.host      = h;
            if (const char* p  = std::getenv("OMEGA_NQ_IBKR_PORT"))     nc.port      = std::atoi(p);
            if (const char* c  = std::getenv("OMEGA_NQ_IBKR_CLIENT"))   nc.client_id = std::atoi(c);
            if (const char* m  = std::getenv("OMEGA_NQ_IBKR_MDTYPE"))   nc.market_data_type = std::atoi(m);
            omega::nq_momo_ibkr::configure(nc);
            omega::nq_momo_ibkr::set_on_trade_record(
                [](const omega::TradeRecord& tr) { handle_closed_trade(tr); });
            g_open_positions.register_source("NqFutMomo",
                []() { return omega::nq_momo_ibkr::collect_positions(); });
            printf("[OMEGA-INIT] NqFutMomo IN-PROCESS IBKR engine wired but DORMANT (corrected faithful BT "
                   "= NOT cost-robust: LONG PF1.34@2pt FAILS @4pt; SHORT dead). Do NOT set OMEGA_NQ_IBKR.\n");
        }

        // ── QndxSqf IN-PROCESS IBKR book (QNDX = Nasdaq-100 SQF, folded from ex-IBKRCrypto) ──
        // The IBKR AU account U23757894 is crypto-INELIGIBLE (2026-07-03): every crypto secType
        // (spot Paxos + QTF/QEF SQF) is err201 closing-only, leaving QNDX -- an index FUT, not
        // crypto -- as the ONLY tradeable leg. The standalone Crypto/build/ibkrcrypto_engine +
        // :8090 GUI book is folded here as an in-process sibling of NqFutMomo/BigCapMomo so it
        // surfaces in the Omega GUI + shadow ledger. TWO validated orthogonal DAILY legs:
        // TSMom50 trend + RSIrev mean-rev (verbatim QndxStrat, WF-locked 2026-06-24), vol-target
        // sized, long+short, EXIT-ON-TURN (no per-trade stop -- the backtested adverse-protection
        // verdict; a stop guts the trend edge). clientId 88 REUSED from the retired Crypto
        // executor (retire it FIRST -- strict cutover order). PAPER-only until a paper fill is
        // proven, then 4001. Activate with OMEGA_QNDX_IBKR=1. Connection knobs:
        // OMEGA_QNDX_IBKR_{HOST,PORT,CLIENT,MONTH}.
        {
            omega::qndx_sqf_ibkr::Config qc;   // defaults: QNDX FUT/CME, clientId 88, port 4002, paper_only, daily CSV
            if (const char* mo = std::getenv("OMEGA_QNDX_IBKR_MONTH"))  qc.last_trade_month = mo;
            if (const char* h  = std::getenv("OMEGA_QNDX_IBKR_HOST"))   qc.host      = h;
            if (const char* p  = std::getenv("OMEGA_QNDX_IBKR_PORT"))   qc.port      = std::atoi(p);
            if (const char* c  = std::getenv("OMEGA_QNDX_IBKR_CLIENT")) qc.client_id = std::atoi(c);
            if (const char* m  = std::getenv("OMEGA_QNDX_IBKR_MDTYPE")) qc.market_data_type = std::atoi(m);
            if (const char* cs = std::getenv("OMEGA_QNDX_IBKR_CSV"))    qc.daily_csv = cs;
            omega::qndx_sqf_ibkr::configure(qc);
            omega::qndx_sqf_ibkr::set_on_trade_record(
                [](const omega::TradeRecord& tr) { handle_closed_trade(tr); });
            g_open_positions.register_source("QndxSqf",
                []() { return omega::qndx_sqf_ibkr::collect_positions(); });
            printf("[OMEGA-INIT] QndxSqf IN-PROCESS IBKR book wired (QNDX daily TSMom50+RSIrev, "
                   "vol-target, exit-on-turn, PAPER, clientId 88); activate with OMEGA_QNDX_IBKR=1\n");
        }

        // ── GoldOrbRetraceEngine (XAUUSD, ORB 50%-retrace + structural RUNNER) ──
        // 2026-06-06 backtest edge (backtest/orb_gold_retrace.cpp; memory
        // omega-peachy-gold-orb-retrace-edge). Distilled from Peachy's gold-ORB
        // videos: her STRUCTURE (08:20 ET 30m ORB, retrace-to-38.2%, trend filter,
        // tight stop, one-shot) + her MANAGEMENT (let winners run = structural
        // RUNNER trail, no fixed TP). The runner was the missing piece -- it
        // DOUBLED avgR (+0.69->+1.20) AND made it 3x-cost-robust.
        //   2yr XAU m5 @IBKR 0.37pt: PF 2.38 avgR+1.20 WR30%, both halves 2.56/2.18,
        //   all years 2.31/2.69/1.53, 3x-cost 1.12, plateau RETR.382 x TRWIN{2,3,5}.
        //   Two-sided (ORB-break shorts too). CAVEAT: gold 2024-26 bull-dominated
        //   (no sustained gold-bear tape) -> SHADOW, not live-size.
        g_gold_orb_retrace.symbol      = "XAUUSD";
        g_gold_orb_retrace.engine_name = "GoldOrbRetrace";
        g_gold_orb_retrace.shadow_mode = true;     // prove on shadow before any live size
        g_gold_orb_retrace.enabled     = false;     // RE-TOMBSTONED 2026-06-23 (operator rule: 7 definitive live results -> dies). Live shadow ledger now has exactly 7 distinct closed GoldOrbRetrace results, 0 wins / 7, net -$808 (all TRAIL_STOP). Even discounting the 2 pre-06-09 lot=1.0 (100x) artifacts, the 5 clean post-fix results are ALSO all losses (0/5, net -$24, every exit a tiny sub-spread TRAIL_STOP) -- textbook intraday-spot-CFD cost wall (move never clears spread). The 06-16 UN-TOMBSTONE rested on a bar-replay PF2.38; faithful live shadow has now FALSIFIED it 7/7. See SystemAudit-2026-06-23 + [[intraday-spot-cfd-cost-wall]]. enabled=false (no dispatch/fire/GUI).
        g_gold_orb_retrace.verbose     = true;
        g_gold_orb_retrace.lot         = 0.01;   // 2026-06-09 FIX: was 1.0 (index default) -> 100x oversized on XAU (USD_PER_PT_LOT=100). All gold engines use 0.01.
        // SIZING GUARD: XAU engines MUST be <=0.05 lot (x100 multiplier). Clamp + loud-warn if a gold engine slips through oversized.
        if (g_gold_orb_retrace.lot > 0.05) { std::printf("[SIZING-WARN] GoldOrbRetrace lot %.2f >0.05 on XAU (x100) -- clamping to 0.01\n", g_gold_orb_retrace.lot); g_gold_orb_retrace.lot = 0.01; }
        g_gold_orb_retrace.seed_from_csv(
            omega::resolve_seed_path("phase1/signal_discovery/warmup_XAUUSD_M5.csv"));
        g_gold_orb_retrace.on_trade_record = [](const omega::TradeRecord& tr) { handle_closed_trade(tr); };
        g_open_positions.register_source("GoldOrbRetrace", []() {
            std::vector<omega::PositionSnapshot> v;
            const auto& p = g_gold_orb_retrace.pos_;
            if (p.active) {
                omega::PositionSnapshot s;
                s.symbol = "XAUUSD"; s.engine = "GoldOrbRetrace";
                s.side = p.side > 0 ? "LONG" : "SHORT"; s.size = p.lot;
                s.entry = p.entry; s.sl = p.sl; s.tp = 0.0;
                s.entry_ts = p.entry_ts_ms / 1000LL;
                v.push_back(s);
            }
            return v;
        });
        printf("[OMEGA-INIT] GoldOrbRetrace XAUUSD: shadow=true enabled=%d ORB30(08:20-08:50 ET) "
               "retr0.382 tightSL trendEMA50 RUNNER-trail(3) 1-shot two-sided\n",
               (int)g_gold_orb_retrace.enabled);

        // ── GoldOrbRetrace LONDON open (S-2026-06-20 orb-widen 2nd session) ────────
        // Additive 2nd gold ORB session (03:00 ET London open) — co-fires with COMEX only
        // 28% of days (real diversification). +daily BullGate (DD insurance), PF1.99 gated.
        g_gold_orb_london.symbol      = "XAUUSD";
        g_gold_orb_london.engine_name = "GoldOrbRetraceLDN";
        g_gold_orb_london.tag         = "GOLDORB-LDN";
        g_gold_orb_london.or_start_et = 180;   // 03:00 ET London open
        g_gold_orb_london.or_end_et   = 210;   // 03:30 ET first 30 min (exclusive)
        g_gold_orb_london.daily_gate  = true;  // BullGate DD-insurance
        g_gold_orb_london.shadow_mode = true;
        g_gold_orb_london.enabled     = false;    // RE-TOMBSTONED 2026-06-23 alongside GoldOrbRetrace: same dead intraday-gold-ORB class (cost wall). Live shadow result 0 wins / 1, net -$4.44 (TRAIL_STOP). Parent GoldOrbRetrace died at 7/7; killing the parent while leaving the identical London sibling firing the same dead pattern is incoherent. See SystemAudit-2026-06-23.
        g_gold_orb_london.verbose     = true;
        g_gold_orb_london.lot         = 0.01;
        if (g_gold_orb_london.lot > 0.05) { g_gold_orb_london.lot = 0.01; }
        g_gold_orb_london.seed_from_csv(
            omega::resolve_seed_path("phase1/signal_discovery/warmup_XAUUSD_M5.csv"));
        g_gold_orb_london.on_trade_record = [](const omega::TradeRecord& tr) { handle_closed_trade(tr); };
        g_open_positions.register_source("GoldOrbRetraceLDN", []() {
            std::vector<omega::PositionSnapshot> v;
            const auto& p = g_gold_orb_london.pos_;
            if (p.active) {
                omega::PositionSnapshot s;
                s.symbol = "XAUUSD"; s.engine = "GoldOrbRetraceLDN";
                s.side = p.side > 0 ? "LONG" : "SHORT"; s.size = p.lot;
                s.entry = p.entry; s.sl = p.sl; s.tp = 0.0;
                s.entry_ts = p.entry_ts_ms / 1000LL;
                v.push_back(s);
            }
            return v;
        });
        printf("[OMEGA-INIT] GoldOrbRetrace LONDON XAUUSD: shadow enabled=%d ORB30(03:00-03:30 ET) +BullGate (orb-widen 2nd session)\n",
               (int)g_gold_orb_london.enabled);

        // ── GoldPanicBounce (XAUUSD "big reversal day" V-bounce) ───────────────
        // 2026-06-12 deep-dive (backtest/panic_bounce_bt.cpp, H1, cost-incl 0.37):
        //   long-only capitulation bounce -- ALWAYS-ON monitor recomputes rolling
        //   drawdown depth in ATR each H1 bar; arm at depth>=DROP_K ATR + a TURN
        //   (bullish bar reclaims prior high after a red bar); exit = aggressive
        //   chandelier ATR-trail (NO TP, ride the V), structural selloff-low stop.
        //   XAU 24-26 drop8/lb250/tr4.5: PF 1.97 net +967pt n=113, BOTH halves+
        //   (1.82/2.01), robust ridge drop{4,6,8}/lb250/tr4.5 all both-halves+ AND
        //   2022-bear+ (OOS PF 1.08). Velocity gate HURTS gold (depth, not speed).
        //   Index version FAILED cross-instrument/bear -> NOT built. Same family as
        //   CapitulationEngine (PF1.82 equities). CAVEAT: low-win fat-tail, bull-
        //   dominated corpus though bear passed -> SHADOW, observe before live size.
        // S-2026-06-17 CULLED: ledger_analytics net -$205 (biggest $ loser),
        // MAEp90 $5781 = catastrophic adverse excursion -- falling-knife long that
        // catches dips that keep falling. n=4 thin but the structural flaw is clear
        // + it's the book's top bleeder. Disabled; needs an entry filter not an exit.
        // S-2026-06-29 REACTIVATED (SHADOW): the cull demanded "an entry filter not an
        // exit". That filter NOW EXISTS -- the macro-hostile gold_regime().long_blocked()
        // gate (GoldPanicBounceEngine.hpp L181) was added 2026-06-21, AFTER the cull, so
        // the engine that bled is NOT the engine running now. Re-judged at CORRECT IBKR
        // cost (2*0.00015*price + spread, not the 0.37 BlackBull legacy the cull implicitly
        // assumed): faithful BT (backtest/panic_bounce_bt.cpp) bull/normal PF~1.80 +850pt
        // n=113 BOTH-WF-halves+ (1.82/2.01), 2022-bear breakeven (PF 0.97-1.02). SHADOW
        // only -- this re-opens a tombstone, so observe a FRESH shadow MAE distribution
        // (the cull basis was MAE, not PnL) before ANY live size. Adverse-protection:
        // chandelier ATR-trail + structural selloff-low stop + 240-bar time-stop + the
        // macro long-block entry filter (NO cold loss-cut by design -- depth, not speed,
        // is the edge; a velocity/cut gate HURTS gold per the 2026-06-12 sweep).
        // S-2026-07-12 DISABLED (operator cull order). GOLD_PHASE1B true-cost re-run:
        // -$83 bull / -$608 bear; the S-2026-06-30 EMA200-slope gate ITSELF forfeits the
        // +$1,442 pre-gate bull edge. MGC port (roadmap) is the only revival path.
        g_gold_panic_bounce.shadow_mode = false;
        g_gold_panic_bounce.enabled     = false;
        // S-2026-06-30 EMA200-slope regime gate (faithful BT, both regimes):
        // block the dip-buy while EMA200 is falling (confirmed bear). gate_lb200
        // won the sweep outright -- bear 2022 bleed -6.14->-0.69 (-89%), bull 6mo
        // PF 2.49->4.44 keeping 95% of profit (cuts 9 losing pullback dips). A
        // slope gate, not a price-position gate, so it preserves the V-reversal.
        g_gold_panic_bounce.TREND_GATE      = true;
        g_gold_panic_bounce.TREND_SLOPE_LB  = 200;
        g_gold_panic_bounce.TREND_SLOPE_MIN = 0.0;
        g_gold_panic_bounce.on_close_cb = [](const omega::TradeRecord& tr) { handle_closed_trade(tr); };
        g_gold_panic_bounce.seed_from_h1_csv(
            omega::resolve_seed_path("phase1/signal_discovery/warmup_XAUUSD_H1.csv"));
        g_open_positions.register_source("GoldPanicBounce", []() {
            std::vector<omega::PositionSnapshot> v;
            const auto& p = g_gold_panic_bounce.m_pos;
            if (p.active) {
                omega::PositionSnapshot s;
                s.symbol = "XAUUSD"; s.engine = "GoldPanicBounce";
                s.side = "LONG"; s.size = p.size;
                s.entry = p.entry; s.sl = p.init_stop; s.tp = 0.0;
                s.entry_ts = p.entry_ts / 1000LL;
                v.push_back(s);
            }
            return v;
        });
        printf("[OMEGA-INIT] GoldPanicBounce XAUUSD: shadow=true enabled=%d REACTIVATED (macro long-block entry filter post-cull; bull PF~1.80 IBKR-cost; observe MAE before live)\n",
               (int)g_gold_panic_bounce.enabled);

        // ── IndexBearShort (NAS100 risk-off SHORT for bad days) ────────────────
        // 2026-06-12 deep-dive (backtest/index_bear_short_bt.cpp, H1, cost-incl):
        //   The long bounce-catcher FAILS on indices (fights the downtrend). The
        //   money on bad days is SHORT: sustained-bear gate (price<EMA200, EMA200
        //   falling over 100 bars, EMA50<EMA200) + Donchian-48 breakdown + FIXED
        //   2R TP (a trail gives it back on the bear counter-rally: PF0.87 vs
        //   fixed-TP 1.60). NAS 2022 bear PF 1.60 +1623pt n=18 BOTH halves+
        //   (1.70/1.50); regime gate keeps it ~flat in the 24-26 bull (pooled
        //   +702 vs ungated -9025). CAVEAT: ONE bear instrument so far -- SPX/GER
        //   2022 cross-validation PENDING -> SHADOW only, NAS100 instance first.
        g_idx_bear_short_nas.symbol      = "NAS100";
        g_idx_bear_short_nas.engine_name = "IndexBearShort";
        g_idx_bear_short_nas.shadow_mode = false;     // S-2026-07-01: LIVE on IBKR 4002 paper (operator all-engines cutover overrides prove-on-shadow note)
        g_idx_bear_short_nas.enabled     = true;   // SHIPPING DON24 all-weather (real engine): 2022 bear PF1.26 +1061 both-halves+, 2024-26 bull PF1.07 +514. Shadow. Manifest IdxBearShortNas. Full history (DON48 vindication, the within-session disable/revert, the DON48->24 bull-bleed fix, all figures) on the DON line below + memory feedback-drive-real-engine-not-port.
        g_idx_bear_short_nas.COST_PTS    = 2.0;      // NAS100 RT pts
        g_idx_bear_short_nas.lot         = 1.0;
        g_idx_bear_short_nas.USE_RISKOFF_GATE = false;  // price-structure gate is the validated one; flip on once VIX/credit feed trusted
        g_idx_bear_short_nas.DON = 24;  // BULL-BLEED FIX S-2026-06-22 (real-engine sweep, backtest/ibs_real_engine.cpp): DON 48->24 (shallower breakdown) flips 2024-26 bull PF0.84 -977pt -> PF1.07 +514pt AND keeps 2022 bear PF1.26 +1061pt both-halves+ = POSITIVE BOTH REGIMES (net cycle +1575 vs +993 at DON48). DON48 bled -977 in bull; TP_R=3.0 fixed bull but KILLED 2022 (H2 PF0.54) = rejected.
        g_idx_bear_short_nas.on_close_cb = [](const omega::TradeRecord& tr) { handle_closed_trade(tr); };
        g_idx_bear_short_nas.seed_from_h1_csv(
            omega::resolve_seed_path("phase1/signal_discovery/warmup_NAS100_H1.csv"));
        g_open_positions.register_source("IndexBearShort", []() {
            std::vector<omega::PositionSnapshot> v;
            const auto& p = g_idx_bear_short_nas.m_pos;
            if (p.active) {
                omega::PositionSnapshot s;
                s.symbol = "NAS100"; s.engine = "IndexBearShort";
                s.side = "SHORT"; s.size = p.size;
                s.entry = p.entry; s.sl = p.stop; s.tp = p.tp;
                s.entry_ts = p.entry_ts / 1000LL;
                v.push_back(s);
            }
            return v;
        });
        printf("[OMEGA-INIT] IndexBearShort NAS100: shadow=true enabled=%d sustained-bear-gate "
               "Donchian24-breakdown FIXED-2R-TP SHORT-only (bad-day engine)\n",
               (int)g_idx_bear_short_nas.enabled);

        // ── IndexBearShort (US500 instance — same class) ──────────────────────
        // 2026-06-22 ADDED + WIRED. The prior 2026-06-12 instance was removed (only a
        // vestigial printf survived) and the SPX leg was audit-disabled because its
        // validation FILE (SPX2022_bear_h1.csv) did not exist = unreproducible.
        // NOW reproduced on REAL HISTDATA SPXUSD 2022 ticks via the ACTUAL engine class
        // (backtest/ibs_real_engine.cpp --ticks /tmp/spx22 --symbol US500 --cost 0.6):
        // 2022 bear n=28 WR43% PF1.59 net +591pt, BOTH HALVES POSITIVE (H1 1.99 / H2 1.20).
        // Ships at the VALIDATED config DON=48 (NOT the NAS DON=24 bull-bleed fix — that
        // was validated on NAS only; no SPX intraday 2024-26 data exists to test the SPX
        // bull regime, so SPX bull behaviour is UNTESTED). Shadow; live-size gated on a
        // forward shadow-ledger read of bull-regime behaviour.
        g_idx_bear_short_sp.symbol      = "US500";
        g_idx_bear_short_sp.engine_name = "IndexBearShort";
        g_idx_bear_short_sp.shadow_mode = false;  // S-2026-07-01: LIVE on IBKR 4002 paper (operator all-engines cutover)
        g_idx_bear_short_sp.enabled     = true;
        g_idx_bear_short_sp.COST_PTS    = 0.6;       // US500 RT pts (real-engine SPX2022 cost)
        g_idx_bear_short_sp.lot         = 1.0;
        g_idx_bear_short_sp.USE_RISKOFF_GATE = false;
        // DON stays at class default 48 (the SPX-validated config; do NOT copy NAS DON=24).
        g_idx_bear_short_sp.on_close_cb = [](const omega::TradeRecord& tr) { handle_closed_trade(tr); };
        g_idx_bear_short_sp.seed_from_h1_csv(
            omega::resolve_seed_path("phase1/signal_discovery/warmup_US500_H1.csv"));
        g_open_positions.register_source("IndexBearShortSP", []() {
            std::vector<omega::PositionSnapshot> v;
            const auto& p = g_idx_bear_short_sp.m_pos;
            if (p.active) {
                omega::PositionSnapshot s;
                s.symbol = "US500"; s.engine = "IndexBearShort";
                s.side = "SHORT"; s.size = p.size;
                s.entry = p.entry; s.sl = p.stop; s.tp = p.tp;
                s.entry_ts = p.entry_ts / 1000LL;
                v.push_back(s);
            }
            return v;
        });
        printf("[OMEGA-INIT] IndexBearShort US500: shadow=true enabled=%d DON48 (SPX2022 real-engine "
               "PF1.59 +591pt both-halves+); bull-regime untested -- ledger-gated\n",
               (int)g_idx_bear_short_sp.enabled);

        // ── NasOrbRetrace — TOMBSTONED 2026-06-22 (fleet-audit, DEAD) ─────────
        // The 2026-06-07 "PF 1.87 transfers to NAS" was a BAR-REPLAY PORT ARTIFACT
        // (orb_gold_retrace.cpp idealized fills, no spread/cost gate). The 23-agent
        // fleet audit drove the REAL GoldOrbRetraceEngine class on NAS data =
        // PF 0.45-0.59, net-NEGATIVE every regime + both WF halves. No live instance
        // ever existed (this was a printf only) -> nothing to disable, just removed
        // the misleading boot line. Manifest AUDITED_CONFIGS.tsv: NasOrbRetrace DEAD.
        // Do NOT re-wire without a REAL-engine both-regime pass on faithful NAS data.

        // ── MondayRiskOn (NAS100/GBPUSD/AUDUSD) -- weekend-risk-reset calendar edge ──
        // 2026-06-07 data-mining find (anomaly_scan + monday_test + monday_gated).
        // Risk-on assets rally Monday; SMA50 risk-on gate is load-bearing (ungated flips
        // negative in a bear: gold 2022 Mon -0.27%/t-2.06). Validated SMA50-gated m5 2024-26:
        //   NAS t2.59 WR67% / GBP t2.04 WR71% / AUD t2.45 WR65%, all both-halves+, all-years+.
        // LONG at Monday UTC open if prevDayClose > SMA50(daily); flat at Monday close.
        // CAVEAT: bull tape; own-SMA gate = partial bear cover -> macro gate before live-size.
        {
            struct MonCfg { omega::MondayRiskOnEngine* e; const char* sym; const char* d1; };
            // S-2026-06-29: GBPUSD + AUDUSD legs REMOVED ("no FX"). NAS100 (index)
            //   kept. g_monday_gbp/aud globals now inert (default enabled=false).
            const MonCfg mons[] = {
                { &g_monday_nas, "NAS100", "phase1/signal_discovery/warmup_NAS100_D1.csv" },
            };
            for (const auto& m : mons) {
                m.e->symbol      = m.sym;
                m.e->engine_name = std::string("MondayRiskOn_") + m.sym;
                m.e->tag         = "MONRISK";
                m.e->sma_len     = 50;
                m.e->shadow_mode = true;
                m.e->enabled     = true;
                m.e->verbose     = false;
                m.e->lot         = 1.0;
                m.e->seed_from_csv(omega::resolve_seed_path(m.d1));
                const char* symc = m.sym; auto* eng = m.e;
                eng->on_trade_record = [](const omega::TradeRecord& tr) { handle_closed_trade(tr); };
                g_open_positions.register_source(eng->engine_name, [eng, symc]() {
                    std::vector<omega::PositionSnapshot> v;
                    if (eng->pos_.active) {
                        omega::PositionSnapshot s;
                        s.symbol = symc; s.engine = eng->engine_name;
                        s.side = "LONG"; s.size = eng->pos_.lot; s.entry = eng->pos_.entry;
                        s.sl = 0.0; s.tp = 0.0; s.entry_ts = eng->pos_.entry_ts / 1000LL;
                        v.push_back(s);
                    }
                    return v;
                });
            }
            printf("[OMEGA-INIT] MondayRiskOn NAS100 (FX legs removed): shadow=true Mon-long SMA50-gate\n");
        }

        // OvernightDrift — 2nd index edge (the "night effect"), trend-gated.
        // Long at cash close -> flat at open, only if close>SMA20. Backtest:
        // NDX cash Sharpe 1.62, NQ future 1.0 (no financing), both halves +,
        // bear-safe (flat overnight in downtrends). Preferred live vehicle:
        // IBKR MNQ future. Shadow on the live NAS100 feed for now.
        // ===== TOMBSTONED 2026-06-13 (S-2026-06-13r, operator cull) =====
        // 1.5% tail-cap = ~450pt NAS = ~$450 at lot 1.0 = a full month of
        // XauStraddleM15 profit ON ONE TRADE; live -453 realized on a single trade.
        // The stop was self-flagged "Re-backtest pending" -- never validated; the
        // overnight-drift edge on NAS was never reproduced with a tail this size.
        // Per-trade tail incompatible with a $1-50/trade book. (SPX sibling below
        // STAYS ENABLED -- it IS 2022-bear-validated, gated PF1.29, now lot 0.3.)
        // RE-ENABLE BAR: reproduced NAS backtest with a tight cap (<=$40/trade).
        printf("[OMEGA-INIT] OvernightDrift NAS100 -- TOMBSTONED 2026-06-13 (operator cull); class type-only, NOT loaded\n");

        // OvernightDrift US500 (S&P) — 2026-06-09. Same night-effect edge, SMA50
        // gate. PROPER backtest incl freshly-pulled FULL-YEAR 2022 bear (dukascopy
        // usa500idxusd): gated bull +18.6% PF1.29 Sharpe1.39 (halves +9/+9, maxDD7%),
        // 2022 bear -1.2% maxDD5% (vs B&H -20%) -- gate sits ~flat through the bear.
        // SMA50 beat SMA100 on SPX bull. Naked (ungated) FAILS 2022 (-14%) -> gate
        // is load-bearing. US cash session (same RTH as NAS). Shadow.
        //   "2022-bear-survived PF1.29 KEEP" claim was UNVERIFIABLE -- the 2026-06 index audit found
        //   OvernightDrift only had 2024-2026 (bull-only) data, no real 2022. Cross-regime retest of the
        //   long-index-overnight-w/-SMA20-gate archetype = -27% in 2022 (the gate does NOT dodge the bear);
        //   live shadow ledger -$453 (single catastrophic gap, 0% WR). Explicit new evidence overrides KEEP.
        // 2026-06-13 (S-2026-06-13q): lot 1.0 -> 0.3 dollar-risk normalization
        // (scale-invariant; edge unchanged). SPX overnight gap at 1.0 lot risked
        // ~$1/pt of a ~60pt overnight = comparable tail to the culled NAS sibling;
        // shrink the $ so one bad gap can't erase weeks of the gold book.
        printf("[OMEGA-INIT] OvernightDrift US500 -- class type-only, NOT loaded (2022 retest -27pct; KEEP claim overridden)\n");

        // ConnorsRSI2 NAS100 — daily mean-reversion dip-buy, ENHANCED close>SMA5 exit.
        // S-2026-06-19 WIRED + RE-ENABLED (shadow). Was class-only/never instantiated;
        // shelved 2026-06-04 ONLY for portfolio redundancy with FVGcont+OvernightDrift —
        // BOTH since DEAD (FvgCont = bar-replay artifact; OvernightDrift tombstoned), so
        // the index book now has ZERO mean-reversion and the redundancy reason is void.
        // NEW EVIDENCE (the engine_init "re-enable only with new evidence" bar): faithful
        // 10yr-daily revalidation (connors_opt.cpp, enhanced exit, cost-incl) — NDX PF1.90
        // both-halves+, 3x(8pt)-cost-robust, bear-safe (SMA200 sat out 2022: 4 trades -454).
        // The fixed-1-day-hold inline "Sharpe 2.46" was optimistic; the enhanced close>SMA5
        // exit is what makes it robust (PF1.33->1.90). GER40 also revives (PF1.39, 2022 +290)
        // -> v2 (needs session param); scale-in -> v2 (PF2.27 but 2x size + dip risk).
        g_connors_nas.symbol      = "NAS100";
        g_connors_nas.engine_name = "ConnorsRSI2";
        g_connors_nas.TREND_SMA   = 200;
        g_connors_nas.RSI_IN      = 10.0;
        g_connors_nas.SHORT_SMA   = 5;      // enhanced exit: close back above SMA5 = MR complete
        g_connors_nas.MAXHOLD     = 10;     // safety cap (days)
        g_connors_nas.SCALEIN     = true;   // v2: Connors cumulative avg-in (faithful NDX PF1.90->2.27)
        g_connors_nas.MAX_UNITS   = 2;      // up to 2x size; SMA200 filter caps dip risk
        // S-2026-06-26s SCALE 0.3 -> 3.0 (operator-approved, DD-budget-sized).
        // REAL-engine validated REGIME_GATE=1 (asym-veto) NDX 8pt: ALL PF4.17
        // +23965pt, BEAR2022 PF3.01 +1842 POSITIVE, both-halves+, 2x-cost robust.
        // NAS100=$1/pt -> lot3 ~= $7k/yr, maxDD ~$4k. CAVEAT: LOW FREQ (13.5 tr/yr)
        // = Sharpe/diversifier leg, freq (NOT lot) caps dollars. PF10.23 headline =
        // bull cherry-pick; true headline PF4.17. Do NOT build a breadth book
        // (sweep KILLED RSI2/IBS+breadth>=2 vs asym-veto -- see handoff 2026-06-26b).
        g_connors_nas.lot         = 3.0;    // dollar-normalized shadow size (index convention)
        g_connors_nas.shadow_mode = false;  // S-2026-07-01: LIVE on IBKR 4002 paper (validated EDGE)
        g_connors_nas.enabled     = true;   // SHADOW
        g_connors_nas.REGIME_GATE  = 1;     // S-2026-06-20: asym sustained-bear veto > SMA200 (faithful 6/6) — SHADOW
        g_connors_nas.on_trade_record = [](const omega::TradeRecord& tr){ handle_closed_trade(tr); };
        g_connors_nas.init();
        g_connors_nas.seed_from_d1_csv("phase1/signal_discovery/warmup_NAS100_D1.csv");
        g_engine_heartbeat.register_engine("ConnorsRSI2", g_connors_nas.enabled, 3600, 0, 24);
        // S-2026-06-29: register the 9 enabled-but-no-pulse engines surfaced by
        //   tools/engine_contract_check.py (ENABLED+NO_PULSE). Names MUST match
        //   the pulse() calls just added in tick_gold.hpp / tick_indices.hpp.
        //   cadence 3600s + session 0-24 mirrors the ConnorsRSI2/calendar pattern.
        g_engine_heartbeat.register_engine("TrendRider",      g_trend_rider.enabled,        3600, 0, 24);
        g_engine_heartbeat.register_engine("Rider4H",         g_rider_4h.enabled,           3600, 0, 24);
        g_engine_heartbeat.register_engine("RiderD1",         g_rider_d1.enabled,           3600, 0, 24);
        g_engine_heartbeat.register_engine("SpxTurtleD1",     g_spx_turtle_d1.enabled,      3600, 0, 24);
        g_engine_heartbeat.register_engine("Dj30TurtleD1",    g_dj30_turtle_d1.enabled,     3600, 0, 24);
        g_engine_heartbeat.register_engine("ConnorsNas",      g_connors_nas.enabled,        3600, 0, 24);
        g_engine_heartbeat.register_engine("GoldVolbrkM30",   g_gold_volbrk_m30.enabled,    3600, 0, 24);
        g_engine_heartbeat.register_engine("IdxBearShortNas", g_idx_bear_short_nas.enabled, 3600, 0, 24);
        g_engine_heartbeat.register_engine("IdxBearShortSp",  g_idx_bear_short_sp.enabled,  3600, 0, 24);
        printf("[OMEGA-INIT] ConnorsRSI2 NAS100: shadow=%d enabled=%d dip-buy close>SMA200 & RSI2<10, exit close>SMA5/maxhold10 scalein=%d\n",
               (int)g_connors_nas.shadow_mode, (int)g_connors_nas.enabled, (int)g_connors_nas.SCALEIN);
        fflush(stdout);

        // S-2026-06-23: register the DAILY INDEX/FX BOOK with the liveness heartbeat. These
        // engines aggregate every tick of their symbol; [STARTUP-SELFTEST] + the miss-detector
        // now COVER them (previously 0 registrations -> invisible: a severed dispatch looked
        // identical to "waiting"). Names MUST match the pulse() calls in on_tick.hpp. cadence
        // 7200s + session 0-24 is generous (index/fx CFDs tick ~all day; off-hours/weekend
        // silence is expected, not an alert). live_required tracks each instance's .enabled.
        g_engine_heartbeat.register_engine("IndexSeasonal_US500.F", g_idx_seas_us500.enabled, 7200, 0, 24);
        g_engine_heartbeat.register_engine("IndexSeasonal_USTEC.F", g_idx_seas_ustec.enabled, 7200, 0, 24);
        g_engine_heartbeat.register_engine("IndexSeasonal_GER40",   g_idx_seas_ger40.enabled, 7200, 0, 24);
        g_engine_heartbeat.register_engine("IndexSeasonal_DJ30.F",  g_idx_seas_dj30.enabled,  7200, 0, 24);
        g_engine_heartbeat.register_engine("IndexSeasonal_UK100",   g_idx_seas_uk100.enabled, 7200, 0, 24);
        g_engine_heartbeat.register_engine("IndexSeasonal_ESTX50",  g_idx_seas_estx50.enabled,7200, 0, 24);
        g_engine_heartbeat.register_engine("CalendarTom_US500.F",   g_tom_us500.enabled, 7200, 0, 24);
        g_engine_heartbeat.register_engine("CalendarTom_USTEC.F",   g_tom_ustec.enabled, 7200, 0, 24);
        g_engine_heartbeat.register_engine("CalendarTom_GER40",     g_tom_ger40.enabled, 7200, 0, 24);
        g_engine_heartbeat.register_engine("CalendarTom_DJ30.F",    g_tom_dj30.enabled,  7200, 0, 24);
        g_engine_heartbeat.register_engine("CalendarTom_UK100",     g_tom_uk100.enabled, 7200, 0, 24);
        g_engine_heartbeat.register_engine("CalendarTom_XAUUSD",    g_tom_xau.enabled,   7200, 0, 24);
        g_engine_heartbeat.register_engine("IndexFomc_US500.F",     g_idx_fomc_us500.enabled, 7200, 0, 24);
        g_engine_heartbeat.register_engine("IndexFomc_USTEC.F",     g_idx_fomc_ustec.enabled, 7200, 0, 24);
        g_engine_heartbeat.register_engine("IndexFomc_DJ30.F",      g_idx_fomc_dj30.enabled,  7200, 0, 24);
        g_engine_heartbeat.register_engine("XsIndex_MomLong", g_xs_mom_long.enabled, 7200, 0, 24);
        g_engine_heartbeat.register_engine("XsIndex_MomLS",   g_xs_mom_ls.enabled,   7200, 0, 24);
        g_engine_heartbeat.register_engine("XsIndex_MrLS",    g_xs_mr_ls.enabled,    7200, 0, 24);
        fflush(stdout);

        // S-2026-06-19 v2: ConnorsRSI2 GER40 (DAX) — the enhanced close>SMA5 exit REVIVES
        // GER40 (faithful 10yr PF1.39 both-halves+, 2022 +290 positive bear). CET session
        // (Xetra 09:00-17:30) via the session params. No scale-in (GER scale-in was neutral
        // in the sweep; base enhanced is the win). Distinct engine_name + warmup.
        g_connors_ger.symbol      = "GER40";
        g_connors_ger.engine_name = "ConnorsRSI2_GER";
        g_connors_ger.TREND_SMA   = 200;
        g_connors_ger.RSI_IN      = 10.0;
        g_connors_ger.SHORT_SMA   = 5;
        g_connors_ger.MAXHOLD     = 10;
        g_connors_ger.SESS_OPEN_HM   = 900;    // DAX Xetra cash 09:00-17:30 CET
        g_connors_ger.SESS_CLOSE_HM  = 1730;
        g_connors_ger.TZ_STD_OFF_MIN = 60;     // CET = UTC+1
        g_connors_ger.TZ_EU_DST      = true;   // EU last-Sunday DST rules
        g_connors_ger.lot         = 0.3;
        g_connors_ger.shadow_mode = true;
        g_connors_ger.enabled     = true;   // S-2026-07-08c RE-ENABLED GATED (operator: "reinstate all winners,
                                            // correctly gated"). The 06-24 cull rested on the freq_dd_frontier
                                            // next-open PROXY ("bear n=4 too thin"); the REAL class had never been
                                            // driven on GER40 (ConnorsNas-retraction precedent). Faithful real-class
                                            // audit backtest/connors_ger_gate_audit.cpp (GER40 daily 2016-26, CERTIFIED):
                                            // deployed config (close>SMA200 gate) PF1.38@3pt n=225 both-WF-halves+,
                                            // 2x-cost PF1.33 holds, 2022 bear = +285pt on n=4 with ZERO bear drawdown
                                            // -- the n=4 IS the gate sitting out the bear (by design, per operator
                                            // bull-gate-not-reject rule). REGIME_GATE=1 asym-veto tested and REJECTED
                                            // here (fails both-halves at 2x-cost; H1 -255) -> SMA200 gate stays.
                                            // SHADOW only; AUDITED_CONFIGS row ConnorsGerReal; TOMBSTONES.tsv row
                                            // downgraded same commit. Prior cull note kept for history above.
        g_connors_ger.on_trade_record = [](const omega::TradeRecord& tr){ handle_closed_trade(tr); };
        g_connors_ger.init();
        g_connors_ger.seed_from_d1_csv("phase1/signal_discovery/warmup_GER40_D1.csv");
        g_engine_heartbeat.register_engine("ConnorsRSI2_GER", g_connors_ger.enabled, 3600, 0, 24);
        printf("[OMEGA-INIT] ConnorsRSI2 GER40: shadow=%d enabled=%d CET 09:00-17:30 dip-buy close>SMA200 & RSI2<10\n",
               (int)g_connors_ger.shadow_mode, (int)g_connors_ger.enabled);
        fflush(stdout);

        // S-2026-06-19 v3: oversold-dip MR FAMILY (ENTRY_MODE), all shadow. mr_hunt.cpp 10yr,
        // 8pt-cost-robust both-halves+. Distinct family members on NDX (IBS/STREAK/DOUBLE) +
        // SPX (STREAK/DOUBLE). Correlated -> observe live-ledger correlation before sizing.
        auto cfg_mr = [&](omega::ConnorsRSI2Engine& e, const char* sym, const char* nm,
                          int mode, const char* warmup, int sess_open, int sess_close){
            e.symbol=sym; e.engine_name=nm; e.ENTRY_MODE=mode;
            e.TREND_SMA=200; e.SHORT_SMA=5; e.MAXHOLD=10;
            e.IBS_IN=0.10; e.STREAK_N=3; e.DBL_IBS=0.20; e.DBL_RSI=15.0;
            e.SESS_OPEN_HM=sess_open; e.SESS_CLOSE_HM=sess_close; e.TZ_STD_OFF_MIN=-300; e.TZ_EU_DST=false;
            e.lot=0.3; e.shadow_mode=true; e.enabled=true;
            e.on_trade_record=[](const omega::TradeRecord& tr){ handle_closed_trade(tr); };
            e.init(); e.seed_from_d1_csv(warmup);
            g_engine_heartbeat.register_engine(nm, e.enabled, 3600, 0, 24);
        };
        cfg_mr(g_ibs_nas,    "NAS100",  "ConnorsIBS_NAS",    1, "phase1/signal_discovery/warmup_NAS100_D1.csv", 930, 1600);
        g_ibs_nas.REGIME_GATE = 1;          // S-2026-06-20: asym sustained-bear veto > SMA200 (faithful biggest win) — SHADOW
        cfg_mr(g_streak_nas, "NAS100",  "ConnorsStreak_NAS", 2, "phase1/signal_discovery/warmup_NAS100_D1.csv", 930, 1600);
        cfg_mr(g_dbl_nas,    "NAS100",  "ConnorsDouble_NAS", 5, "phase1/signal_discovery/warmup_NAS100_D1.csv", 930, 1600);
        cfg_mr(g_streak_spx, "US500.F", "ConnorsStreak_SPX", 2, "phase1/signal_discovery/warmup_US500_D1.csv", 930, 1600);
        cfg_mr(g_dbl_spx,    "US500.F", "ConnorsDouble_SPX", 5, "phase1/signal_discovery/warmup_US500_D1.csv", 930, 1600);
        // ── S-2026-06-20 MR-breadth-book expansion (freq/DD frontier) ───────────────
        // Diversified daily-MR book: NDX+SPX+DJ30 across entry-modes. Diversification of
        // the ~uncorrelated legs cuts portfolio maxDD ~58% while ~12x'ing frequency.
        // Gate per the recheck: NAS=asym-veto (better net+DD), SPX/DJ30=close>SMA200.
        cfg_mr(g_rsi3_nas,  "NAS100",  "ConnorsRSI3_NAS",   4, "phase1/signal_discovery/warmup_NAS100_D1.csv", 930, 1600);
        g_rsi3_nas.REGIME_GATE = 1;   // NAS asym-veto (recheck: net+DD both better on NDX)
        cfg_mr(g_ibs_spx,   "US500.F", "ConnorsIBS_SPX",    1, "phase1/signal_discovery/warmup_US500_D1.csv", 930, 1600);
        cfg_mr(g_rsi2_spx,  "US500.F", "ConnorsRSI2_SPX",   0, "phase1/signal_discovery/warmup_US500_D1.csv", 930, 1600);
        cfg_mr(g_ibs_dj,    "DJ30.F",  "ConnorsIBS_DJ",     1, "phase1/signal_discovery/warmup_DJ30_D1.csv",  930, 1600);
        cfg_mr(g_rsi2_dj,   "DJ30.F",  "ConnorsRSI2_DJ",    0, "phase1/signal_discovery/warmup_DJ30_D1.csv",  930, 1600);
        cfg_mr(g_dbl_dj,    "DJ30.F",  "ConnorsDouble_DJ",  5, "phase1/signal_discovery/warmup_DJ30_D1.csv",  930, 1600);
        // SPX/DJ30 keep REGIME_GATE=0 (close>SMA200) — recheck: asym raises DD there.
        // Book-level concurrent-position cap = the freq/DD-frontier DD lever (cap=3 -> ~2.4% maxDD).
        omega::ConnorsRSI2Engine::BOOK_CAP = 3;
        printf("[OMEGA-INIT] Connors MR breadth book: NAS{RSI2,IBS,STREAK,DOUBLE,RSI3} + SPX{STREAK,DOUBLE,IBS,RSI2} + DJ30{IBS,RSI2,DOUBLE} (shadow); NAS=asym-veto, SPX/DJ30=SMA200, BOOK_CAP=3\n");
        fflush(stdout);
    }

    // ── AdaptiveHullEngine (Ehlers cycle -> adaptive HMA trend, long-only) ──────
    // 2026-06-05 backtest (backtest/adaptive_hull.cpp; memory omega-pa-adaptive-
    // hull-eval): XAU 60m all-session PF1.52 Sh1.61, GER40 60m EU-session PF2.03
    // Sh1.94, both halves+, 3x-cost-robust, long-only (shorts drag). Diversifier
    // (~0.6 corr w/ trend book). shadow.
    {

        // ===== TOMBSTONED 2026-06-13 (S-2026-06-13r, operator cull) =====
        // Live: -351 over 3 trades, 0% WR, all losing TRAIL exits in chop. At lot
        // 1.0 (~$1.10/pt) GER40's ~4x-gold point-swings made each loss $100-160.
        // The historical validation (PF2.03 GER40 H1) could NOT be re-confirmed on
        // current tape: no recent GER40 tick recordings exist (only XAUUSD + NAS/
        // US500 L2 are recorded), so a fresh walk-forward is not possible today.
        // Tombstoned on live evidence + operator directive; the validated claim is
        // stale and unverifiable. The XAU sibling (g_adhull_xau, lot 0.01, all-
        // session) stays enabled -- low $, separate instrument, not in the cull.
        // RE-ENABLE BAR: start recording GER40 ticks, then a fresh walk-forward
        //   (PF>=1.5 both-halves + 3x-cost) on >=3 months of current GER40 tape.
        printf("[OMEGA-INIT] AdaptiveHull: XAU(60m all-sess) + GER40(60m EU-sess) shadow long-only pmul2\n");
        fflush(stdout);
    }

    // ── SupertrendGoldEngine (XAUUSD 60m, Supertrend(10,3) long-only +EMA) ──────
    // 2026-06-05: strongest single-indicator result (PF 2.04, Sharpe 2.49, both
    // halves+, 3x-cost-robust). ST line = trailing stop; exit = flip-down. NO
    // BE/TP/time-stop (edge-killers); EMA100 regime = chop protection. shadow.
    {
        printf("[OMEGA-INIT] SupertrendGold XAUUSD: shadow 60m Supertrend(10,3) long-only +EMA100, flip-exit\n");
        fflush(stdout);
    }

    // ?? Adaptive intelligence layer startup ???????????????????????????????????
    {
        const int64_t now_s = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

        // Configure news blackout scheduler from config flags (defaults are fine, shown for clarity)
        g_news_blackout.scheduler.block_nfp   = true;
        g_news_blackout.scheduler.block_fomc  = true;
        g_news_blackout.scheduler.block_cpi   = true;
        g_news_blackout.scheduler.block_eia   = true;
        g_news_blackout.scheduler.block_cb    = true;

        // ?? Live calendar: inject exact event times from Forex Factory ????????
        // Fetches this week's HIGH-impact events over HTTPS and injects precise
        // blackout windows. Falls back gracefully to hardcoded schedule on failure.
        // Pre: 5 min before event. Post: 15 min after event.
        g_live_calendar.pre_min  = 5;
        g_live_calendar.post_min = 15;
        g_live_calendar.refresh_interval_sec = 86400; // re-fetch daily
        g_live_calendar.refresh(g_news_blackout, now_s);

        // Print final schedule (includes both hardcoded + live injected windows)
        g_news_blackout.print_schedule(now_s);

        // ?? Edge systems startup configuration ???????????????????????????????
        // TOD gate: load historical bucket data so gate is active from first trade
        {
            const std::string tod_path = log_root_dir() + "/omega_tod_buckets.csv";
            g_edges.tod.load_csv(tod_path);
            g_edges.tod.min_trades   = 30;    // need 30 trades per bucket before blocking
            g_edges.tod.min_win_rate = 0.38;  // block if WR < 38%
            g_edges.tod.min_avg_ev_usd = -2.0; // block if avg EV < -$2/trade
            g_edges.tod.enabled      = true;  // active in all modes -- shadow is a simulation
        }
        // Fill quality: load persisted fill history so adverse selection detection is
        // active from the first trade -- no minimum fill count, history survives restarts.
        g_edges.fill_quality.load_csv(log_root_dir() + "/fill_quality.csv");
        // Spread Z-score gate: starts building after first 20 ticks per symbol
        g_edges.spread_gate.window_ticks = 200;
        g_edges.spread_gate.max_z_score  = 3.0; // conservative: 3? anomaly
        g_edges.spread_gate.enabled      = true;  // active in all modes -- shadow is a simulation
        // Round number filter
        g_edges.round_numbers.proximity_frac = 0.08; // within 8% of increment = "near"
        // Previous day levels
        g_edges.prev_day.proximity_frac = 0.05; // within 5% of prior range = "near PDH/PDL"
        // FX fix engines
        g_edges.fx_fix.tp_pips        = 15.0;
        g_edges.fx_fix.sl_pips        = 8.0;
        g_edges.fx_fix.min_cvd_signal = 0.12; // need 12% normalised CVD to enter fix
        g_edges.fx_fix.enabled        = true;  // active in all modes -- shadow is a simulation
        // Fill quality
        g_edges.fill_quality.window_trades = 50;
        g_edges.fill_quality.adverse_threshold_bps = 2.0;
        g_edges.fill_quality.enabled = true;
        std::printf("[EDGES] All 7 edge systems initialised\n");

        // Adaptive risk: start with Kelly disabled for first 15 trades per symbol
        // (confidence ramps from 0?1 automatically as trades accumulate)
        g_adaptive_risk.kelly_enabled            = true;
        g_adaptive_risk.dd_throttle_enabled      = true;
        g_adaptive_risk.corr_heat_enabled        = true;
        g_adaptive_risk.vol_regime_enabled       = true;
        g_adaptive_risk.multiday_throttle_enabled = true;
        g_adaptive_risk.fill_quality_enabled     = true;
        g_adaptive_risk.fill_quality_scale       = 0.70;  // 30% size cut on adverse selection

        // Drawdown velocity circuit breaker: halt new entries for 15 min if we
        // lose more than 50% of daily_loss_limit within any 30-minute window.
        // e.g. daily_loss_limit=$200 ? fires if rolling 30-min loss > $100.
        // Set threshold_usd=0 to disable (off by default until shadow validates).
        g_adaptive_risk.dd_velocity.threshold_usd = g_cfg.daily_loss_limit * 0.5;
        g_adaptive_risk.dd_velocity.window_sec    = 1800;   // 30-minute window
        g_adaptive_risk.dd_velocity.halt_sec      = 900;    // 15-minute halt
        std::printf("[ADAPTIVE] DD-velocity threshold=$%.0f/30min halt=15min\n",
                    g_adaptive_risk.dd_velocity.threshold_usd);

        // Correlation cluster limits -- 2 is the safe default for all clusters
        g_adaptive_risk.corr_heat.max_per_cluster_us_equity = 2;
        g_adaptive_risk.corr_heat.max_per_cluster_eu_equity = 2;
        g_adaptive_risk.corr_heat.max_per_cluster_oil       = 1;  // never both OIL+BRENT open
        g_adaptive_risk.corr_heat.max_per_cluster_metals    = 2;
        g_adaptive_risk.corr_heat.max_per_cluster_jpy_risk  = 2;
        g_adaptive_risk.corr_heat.max_per_cluster_eur_gbp   = 1;  // EURUSD or GBPUSD, not both

        // Load Kelly performance history from previous sessions
        {
            const std::string kelly_dir = state_root_dir() + "/kelly";
            // ensure_parent_dir creates the directory if needed
            namespace fs = std::filesystem;
            std::error_code ec;
            fs::create_directories(kelly_dir, ec);
            g_adaptive_risk.load_perf(kelly_dir);
        }

        //  - ATR state load + backup fallback
        //  - Velocity trail shadow mode wiring
        //  - Hard-stop broker-side order callback
        //  - Velocity add-on (on_addon) callback
        //  - Bleed-flip (on_bleed_flip) callback)

        // Load GoldStack vol baseline + governor EWM -- skips 400-tick regime warmup
        {
            const std::string gs_path = state_root_dir() + "/gold_stack_state.dat";
            g_gold_stack.load_atr_state(gs_path);
        }

        // Multi-day drawdown throttle: load history, log startup state
        {
            const std::string md_path = log_root_dir() + "/day_results.csv";
            g_adaptive_risk.multiday.load(md_path);
            const int streak      = g_adaptive_risk.multiday.consecutive_losing_days();
            const double md_scale = g_adaptive_risk.multiday.size_scale();
            g_telemetry.UpdateMultiDayThrottle(streak, md_scale,
                                               g_adaptive_risk.multiday.is_active() ? 1 : 0);
            if (g_adaptive_risk.multiday.is_active())
                std::cout << "[MULTIDAY-THROTTLE] *** ACTIVE *** consec_loss=" << streak
                          << " -> sizes halved this session\n";
        }

        // Wire fill quality adverse selection check into adjusted_lot().
        // Lambda avoids circular include between OmegaAdaptiveRisk and OmegaEdges.
        g_adaptive_risk.fill_quality_check_fn = [](const std::string& sym) -> bool {
            return g_edges.fill_quality.adverse_selection_detected(sym);
        };

        // Weekend gap size scale: halve lots during Fri 21:00 - Sun 22:00 UTC.
        g_adaptive_risk.weekend_gap_scale_fn = []() -> double {
            return weekend_gap_size_scale();
        };

        // VPIN: enabled, registers size-scale callback into adjusted_lot.
        // block_threshold=0.80 (entry blocked), high_threshold=0.60 (0.5? size).
        g_edges.vpin.enabled         = true;
        g_edges.vpin.bucket_size     = 50;
        g_edges.vpin.window_buckets  = 10;
        g_edges.vpin.high_threshold  = 0.60;
        g_edges.vpin.block_threshold = 0.80;
        g_adaptive_risk.vpin_scale_fn = [](const std::string& sym) -> double {
            return g_edges.vpin.size_scale(sym);
        };

        // Walk-forward OOS validation scale (RenTec #6).
        // Updated every 20 trades via handle_closed_trade -> g_walk_forward.update().
        // Returns 1.0 (pass), 0.75 (degraded), 0.50 (failing).
        // No penalty during warmup (< 40 trades).
        g_adaptive_risk.wfo_scale_fn = [](const std::string& sym) -> double {
            return g_walk_forward.scale(sym);
        };

        // Regime adaptor is enabled; in SHADOW mode it is informational only
        g_regime_adaptor.enabled = true;

        // Portfolio VaR: correlation-adjusted exposure gate.
        // Limit = 1.5 ? daily_loss_limit. At $200 daily limit ? blocks when
        // correlated exposure implies >$300 of potential simultaneous loss.
        g_portfolio_var.init_betas();
        g_portfolio_var.var_limit_usd = g_cfg.daily_loss_limit * 1.5;
        // Correlation matrix -- load warm state from previous session
        g_corr_matrix.load_state(state_root_dir() + "/corr_matrix.dat");
        // VPIN -- reset at session start (stale tick-classification carries no meaning)
        g_vpin.reset();
        g_vpin.toxic_threshold = 0.70;  // block entries above 70% toxic flow
        std::printf("[PORTFOLIO-VAR] limit=$%.0f (1.5? daily_loss_limit)\n",
                    g_portfolio_var.var_limit_usd);

        std::cout << "[ADAPTIVE] Kelly=" << (g_adaptive_risk.kelly_enabled ? "ON" : "OFF")
                  << "  DDthrottle="    << (g_adaptive_risk.dd_throttle_enabled ? "ON" : "OFF")
                  << "  CorrHeat="      << (g_adaptive_risk.corr_heat_enabled ? "ON" : "OFF")
                  << "  VolRegime="     << (g_adaptive_risk.vol_regime_enabled ? "ON" : "OFF")
                  << "  MultiDayDD="    << (g_adaptive_risk.multiday_throttle_enabled ? "ON" : "OFF")
                  << "  FillQuality="   << (g_adaptive_risk.fill_quality_enabled ? "ON" : "OFF")
                  << "  NewsBlackout="  << (g_news_blackout.enabled ? "ON" : "OFF")
                  << "  PartialExit="   << (g_partial_exit.enabled ? "ON" : "OFF")
                  << "  RegimeAdaptor=" << (g_regime_adaptor.enabled ? "ON" : "OFF") << "\n";

        std::cout << "[ADAPTIVE] Corr cluster limits:"
                  << " US_EQ=" << g_adaptive_risk.corr_heat.max_per_cluster_us_equity
                  << " EU_EQ=" << g_adaptive_risk.corr_heat.max_per_cluster_eu_equity
                  << " OIL="   << g_adaptive_risk.corr_heat.max_per_cluster_oil
                  << " METALS=" << g_adaptive_risk.corr_heat.max_per_cluster_metals
                  << " JPY="   << g_adaptive_risk.corr_heat.max_per_cluster_jpy_risk
                  << " EUR_GBP=" << g_adaptive_risk.corr_heat.max_per_cluster_eur_gbp << "\n";
    }

    // ?? Startup parameter validation -- logged on every start ?????????????????
    // This block documents the exact live values every engine will use.
    // Any mismatch between config intent and actual values is visible immediately.
    std::cout << "\n[OMEGA-PARAMS] ???????????????????????????????????????????\n"
              << "[OMEGA-PARAMS] ENGINE PARAMETER AUDIT (live values after all config overrides)\n"
              << "[OMEGA-PARAMS] ???????????????????????????????????????????\n"
              << "[OMEGA-PARAMS] US500.F  TP=" << g_eng_sp.TP_PCT  << "% SL=" << g_eng_sp.SL_PCT
              << "% vol=" << g_eng_sp.VOL_THRESH_PCT << "% mom=" << g_eng_sp.MOMENTUM_THRESH_PCT
              << "% brk=" << g_eng_sp.MIN_BREAKOUT_PCT << "% gap=" << g_eng_sp.MIN_GAP_SEC
              << "s hold=" << g_eng_sp.MAX_HOLD_SEC << "s spread=" << g_eng_sp.MAX_SPREAD_PCT << "%\n"
              << "[OMEGA-PARAMS] USTEC.F  TP=" << g_eng_nq.TP_PCT  << "% SL=" << g_eng_nq.SL_PCT
              << "% vol=" << g_eng_nq.VOL_THRESH_PCT << "% mom=" << g_eng_nq.MOMENTUM_THRESH_PCT
              << "% brk=" << g_eng_nq.MIN_BREAKOUT_PCT << "% gap=" << g_eng_nq.MIN_GAP_SEC
              << "s hold=" << g_eng_nq.MAX_HOLD_SEC << "s spread=" << g_eng_nq.MAX_SPREAD_PCT << "%\n"
              << "[OMEGA-PARAMS] USOIL.F  TP=" << g_eng_cl.TP_PCT  << "% SL=" << g_eng_cl.SL_PCT
              << "% vol=" << g_eng_cl.VOL_THRESH_PCT << "% mom=" << g_eng_cl.MOMENTUM_THRESH_PCT
              << "% brk=" << g_eng_cl.MIN_BREAKOUT_PCT << "% gap=" << g_eng_cl.MIN_GAP_SEC
              << "s hold=" << g_eng_cl.MAX_HOLD_SEC << "s spread=" << g_eng_cl.MAX_SPREAD_PCT << "%\n"
              << "[OMEGA-PARAMS] DJ30.F   TP=" << g_eng_us30.TP_PCT << "% SL=" << g_eng_us30.SL_PCT
              << "% vol=" << g_eng_us30.VOL_THRESH_PCT << "% mom=" << g_eng_us30.MOMENTUM_THRESH_PCT
              << "% brk=" << g_eng_us30.MIN_BREAKOUT_PCT << "% gap=" << g_eng_us30.MIN_GAP_SEC
              << "s hold=" << g_eng_us30.MAX_HOLD_SEC << "s spread=" << g_eng_us30.MAX_SPREAD_PCT << "%\n"
              << "[OMEGA-PARAMS] NAS100   TP=" << g_eng_nas100.TP_PCT << "% SL=" << g_eng_nas100.SL_PCT
              << "% vol=" << g_eng_nas100.VOL_THRESH_PCT << "% mom=" << g_eng_nas100.MOMENTUM_THRESH_PCT
              << "% brk=" << g_eng_nas100.MIN_BREAKOUT_PCT << "% gap=" << g_eng_nas100.MIN_GAP_SEC
              << "s hold=" << g_eng_nas100.MAX_HOLD_SEC << "s spread=" << g_eng_nas100.MAX_SPREAD_PCT << "%\n"
              << "[OMEGA-PARAMS] AUDUSD   TP=" << g_eng_audusd.TP_PCT << "% SL=" << g_eng_audusd.SL_PCT
              << "% vol=" << g_eng_audusd.VOL_THRESH_PCT << "% mom=" << g_eng_audusd.MOMENTUM_THRESH_PCT
              << "% gap=" << g_eng_audusd.MIN_GAP_SEC << "s spread=" << g_eng_audusd.MAX_SPREAD_PCT << "% [ASIA]\n"
              << "[OMEGA-PARAMS] NZDUSD   TP=" << g_eng_nzdusd.TP_PCT << "% SL=" << g_eng_nzdusd.SL_PCT
              << "% vol=" << g_eng_nzdusd.VOL_THRESH_PCT << "% mom=" << g_eng_nzdusd.MOMENTUM_THRESH_PCT
              << "% gap=" << g_eng_nzdusd.MIN_GAP_SEC << "s spread=" << g_eng_nzdusd.MAX_SPREAD_PCT << "% [ASIA]\n"
              << "[OMEGA-PARAMS] USDJPY   TP=" << g_eng_usdjpy.TP_PCT << "% SL=" << g_eng_usdjpy.SL_PCT
              << "% vol=" << g_eng_usdjpy.VOL_THRESH_PCT << "% mom=" << g_eng_usdjpy.MOMENTUM_THRESH_PCT
              << "% gap=" << g_eng_usdjpy.MIN_GAP_SEC << "s spread=" << g_eng_usdjpy.MAX_SPREAD_PCT << "% [ASIA/TOKYO-FIX]\n"
              << "[OMEGA-PARAMS] GBPUSD   TP=" << g_eng_gbpusd.TP_PCT << "% SL=" << g_eng_gbpusd.SL_PCT
              << "% vol=" << g_eng_gbpusd.VOL_THRESH_PCT << "% mom=" << g_eng_gbpusd.MOMENTUM_THRESH_PCT
              << "% brk=" << g_eng_gbpusd.MIN_BREAKOUT_PCT << "% gap=" << g_eng_gbpusd.MIN_GAP_SEC
              << "s spread=" << g_eng_gbpusd.MAX_SPREAD_PCT << "%\n"
              << "[OMEGA-PARAMS] EURUSD   TP=" << g_eng_eurusd.TP_PCT << "% SL=" << g_eng_eurusd.SL_PCT
              << "% vol=" << g_eng_eurusd.VOL_THRESH_PCT << "% mom=" << g_eng_eurusd.MOMENTUM_THRESH_PCT
              << "% brk=" << g_eng_eurusd.MIN_BREAKOUT_PCT << "% gap=" << g_eng_eurusd.MIN_GAP_SEC
              << "s spread=" << g_eng_eurusd.MAX_SPREAD_PCT << "%\n"
              << "[OMEGA-PARAMS] GER40    TP=" << g_eng_ger30.TP_PCT  << "% SL=" << g_eng_ger30.SL_PCT
              << "% vol=" << g_eng_ger30.VOL_THRESH_PCT << "% mom=" << g_eng_ger30.MOMENTUM_THRESH_PCT
              << "% brk=" << g_eng_ger30.MIN_BREAKOUT_PCT << "% gap=" << g_eng_ger30.MIN_GAP_SEC
              << "s hold=" << g_eng_ger30.MAX_HOLD_SEC << "s spread=" << g_eng_ger30.MAX_SPREAD_PCT << "%\n"
              << "[OMEGA-PARAMS] UK100    TP=" << g_eng_uk100.TP_PCT  << "% SL=" << g_eng_uk100.SL_PCT
              << "% vol=" << g_eng_uk100.VOL_THRESH_PCT << "% mom=" << g_eng_uk100.MOMENTUM_THRESH_PCT
              << "% brk=" << g_eng_uk100.MIN_BREAKOUT_PCT << "% gap=" << g_eng_uk100.MIN_GAP_SEC
              << "s hold=" << g_eng_uk100.MAX_HOLD_SEC << "s spread=" << g_eng_uk100.MAX_SPREAD_PCT << "%\n"
              << "[OMEGA-PARAMS] ESTX50   TP=" << g_eng_estx50.TP_PCT << "% SL=" << g_eng_estx50.SL_PCT
              << "% vol=" << g_eng_estx50.VOL_THRESH_PCT << "% mom=" << g_eng_estx50.MOMENTUM_THRESH_PCT
              << "% brk=" << g_eng_estx50.MIN_BREAKOUT_PCT << "% minrange=" << g_eng_estx50.MIN_COMP_RANGE
              << " gap=" << g_eng_estx50.MIN_GAP_SEC
              << "s hold=" << g_eng_estx50.MAX_HOLD_SEC << "s spread=" << g_eng_estx50.MAX_SPREAD_PCT << "%\n"
              << "[OMEGA-PARAMS] UKBRENT  TP=" << g_eng_brent.TP_PCT  << "% SL=" << g_eng_brent.SL_PCT
              << "% vol=" << g_eng_brent.VOL_THRESH_PCT << "% mom=" << g_eng_brent.MOMENTUM_THRESH_PCT
              << "% brk=" << g_eng_brent.MIN_BREAKOUT_PCT << "% gap=" << g_eng_brent.MIN_GAP_SEC
              << "s hold=" << g_eng_brent.MAX_HOLD_SEC << "s spread=" << g_eng_brent.MAX_SPREAD_PCT << "%\n"
              << "[OMEGA-PARAMS] XAUUSD   GoldEngineStack active | gap=" << g_cfg.gs_cfg.min_entry_gap_sec
              << "s hold=" << g_cfg.gs_cfg.max_hold_sec << "s vwap_min=" << g_cfg.gs_cfg.min_vwap_dislocation
              << " spread_max=" << g_cfg.gs_cfg.max_entry_spread << "\n"
              << "[OMEGA-PARAMS] GoldStack MIN_ENTRY_GAP=30s MAX_HOLD=600s REGIME_FLIP_MIN=60s\n"
              << "[OMEGA-PARAMS] LeadLag=ACTIVE(3-window-confirm) SpreadDisloc=managed-only EventComp=managed-only\n"
              << "[OMEGA-PARAMS] ???????????????????????????????????????????\n\n";
    std::cout.flush();

    if (g_cfg.mode == "SHADOW") {
        if (g_cfg.shadow_ustec_pilot_only) {
            g_eng_nq.ENTRY_SIZE = g_cfg.ustec_pilot_size;
            g_eng_nq.MIN_GAP_SEC = g_cfg.ustec_pilot_min_gap_sec;
            g_eng_nq.MAX_TRADES_PER_MIN = std::min(g_eng_nq.MAX_TRADES_PER_MIN, 2);
            std::cout << "[OMEGA-PILOT] USTEC shadow pilot enabled | size=" << g_eng_nq.ENTRY_SIZE
                      << " min_gap=" << g_eng_nq.MIN_GAP_SEC
                      << " max_trades_per_min=" << g_eng_nq.MAX_TRADES_PER_MIN << "\n";
        } else {
            std::cout << "[OMEGA-PILOT] Multi-symbol shadow enabled | all configured engines may trade\n";
        }
        std::cout << "[OMEGA-SHADOW-POLICY] Newly-stamped engines follow g_cfg.mode"
                  << " (kShadowDefault=" << (kShadowDefault ? "true" : "false") << ")"
                  << " | locked-shadow: MCE, RSIReversal,"
                  << " PDHL, H1Swing, H4Regime, ISwingSP, ISwingNQ\n";
        std::cout << "[OMEGA-MODE] SHADOW -- exact live simulation (orders paper only, all risk gates active)\n";
    }

    build_id_map();

    // Open log file and tee stdout into it
    // Log file: stdout/stderr teed to file via RollingTeeBuffer
    // g_tee_buf wires std::cout -> file. Console output preserved.
    {
        const std::string log_dir = "C:\\Omega\\logs";
        CreateDirectoryA(log_dir.c_str(), nullptr);
        g_orig_cout = std::cout.rdbuf();
        g_tee_buf   = new RollingTeeBuffer(g_orig_cout, log_dir);
        if (!g_tee_buf->is_open()) {
            // Log failed -- print to stderr (not tee'd) and continue without log
            // Do NOT return 1 -- process must keep running
            fprintf(stderr, "[OMEGA-LOG-WARN] Cannot open log under %s -- continuing without log\n",
                    log_dir.c_str());
            delete g_tee_buf;
            g_tee_buf = nullptr;
        } else {
            std::cout.rdbuf(g_tee_buf);
            std::cerr.rdbuf(g_tee_buf);
            std::cout << "[OMEGA] Log: " << g_tee_buf->current_path() << "\n";
            // RUNNING COMMIT here so it goes into latest.log via the tee buffer.
            // Previously this was printed via std::printf before tee opened -- went to
            // NSSM stdout only, never to latest.log. Now it goes to both.
            std::cout << "[OMEGA] RUNNING COMMIT: " << OMEGA_VERSION << " built=" << OMEGA_BUILT << "\n";

#ifdef OMEGA_WITH_IBKR
            // --- IBKR execution wiring (2026-06-16 migration) ---
            // Env opt-ins (default = idle, no behaviour change):
            //   OMEGA_EXECUTION_BROKER=IBKR   route orders to IBKR instead of FIX
            //   OMEGA_IBKR_PORT=4001          connect to live gateway (default 4002 paper)
            //   OMEGA_IBKR_LIVE_ORDERS=1      allow REAL orders (default paper_only=true)
            // Real fills still require mode=LIVE (send_live_order's hard SHADOW gate).
            if (const char* eb = std::getenv("OMEGA_EXECUTION_BROKER")) g_cfg.execution_broker = eb;
            if (g_cfg.execution_broker == "IBKR") {
                const int  ib_port  = std::getenv("OMEGA_IBKR_PORT") ? atoi(std::getenv("OMEGA_IBKR_PORT")) : 4002;
                const bool ib_paper = !(std::getenv("OMEGA_IBKR_LIVE_ORDERS") &&
                                        std::string(std::getenv("OMEGA_IBKR_LIVE_ORDERS")) == "1");
                omega::ibkr_exec::configure("127.0.0.1", ib_port, ib_paper);
                omega::ibkr_exec::set_enabled(true);
                omega::ibkr_exec::set_on_fill([](const omega::IbkrFill& f){
                    std::cout << "[IBKR-EXEC] LEDGER-FILL " << f.omega_symbol << " " << f.side
                              << " qty=" << f.qty << " px=" << f.price
                              << " oid=" << f.order_id << " exec=" << f.exec_id << "\n";
                    // TODO Phase 2: reconcile into OmegaTradeLedger as a real broker fill
                    // (entry/exit match + pnl), mirroring the FIX ExecutionReport path.
                });
                if (omega::ibkr_exec::connect())
                    std::cout << "[IBKR-EXEC] execution_broker=IBKR ACTIVE port=" << ib_port
                              << " paper_only=" << (int)ib_paper << "\n";
                else
                    std::cout << "[IBKR-EXEC] execution_broker=IBKR but CONNECT FAILED -- watchdog will retry\n";
                // Keep the exec socket alive: retry on boot-race (gateway up ~110s
                // after boot) and reconnect on a mid-session drop, without a full
                // Omega restart. Runs whether the boot connect above succeeded or not.
                omega::ibkr_exec::start_watchdog();
            } else {
                std::cout << "[IBKR-EXEC] execution_broker=BLACKBULL_FIX (IBKR path idle)\n";
            }
#endif
            // ENGINE CONFIG SUMMARY -- must be emitted AFTER tee opens so it goes into the log file.
            std::cout.flush();

            // -- PDH/PDL Reversion Engine ------------------------------------
            // Research: 2yr/111M tick backtest proves mean reversion inside
            // daily range is the only statistically significant intraday edge.
            // 2026-05-14 (part W): S63 VWR-pattern in-flight protection — explicit
            //   re-affirm of the class defaults (XAU-scaled) for grep visibility.
            //   Mirrors g_vwap_rev_ger40 precedent at engine_init.hpp:649-651 and
            //   g_ustec_tf_5m at engine_init.hpp:976-978: the override matches the
            //   class default but documents intent and makes activation
            //   discoverable from engine_init.hpp alone. See
            //   outputs/S63_STATE_CLASSIFICATION_2026-05-14.md §3.3 for the audit
            //   that confirmed STATE A via class-default route (fields declared
            //   at PDHLReversionEngine.hpp:70-72; mgmt-path at L221-244 fires
            //   every tick).
            //
            //   LOSS_CUT_PCT  = 0.04  -> XAU@3700: ~$1.48 cold-loss cut.
            //   BE_ARM_PCT    = 0.025 -> XAU@3700: ~$0.93 mfe arms ratchet.
            //   BE_BUFFER_PCT = 0.01  -> XAU@3700: ~$0.37 buffer (typical XAU spread).
            // S88-followup 2026-05-27: DISABLED per backtest. 2yr Duka XAU
            // baseline on drift-fade-proxy branch (l2_real=false, which is
            // what production has been running since L2 depth logger was
            // broken until 2026-05-27): n=13,976, WR=10.8%, net=-$8,587,
            // PF~0.48. SL_HIT=12,352 vs TP_HIT=70. No vol-band tuning
            // rescues PF 0.48. Structural loser on this branch. Re-evaluate
            // after 30+ days of fresh L2 captures enable the l2_real branch.
            // Reference: research/PDHL_BASELINE_BROKEN.md.
            // shadow trades; in LIVE mode it would place real orders.
            // Historical 2yr backtest (14.8% WR, -$27k) predates current HMM
            // gate, VWAP filter, and chop gate; recent shadow WR is materially
            // different. Re-evaluate before promoting to LIVE.

            std::cout.flush();
        }
    }

    // ── Step 2 Omega Terminal: register engines with g_engines ──────────────
    // Each lambda returns the engine's current snapshot for OmegaApiServer's
    // GET /api/v1/omega/engines route. Lambdas read only scalar fields off
    // the engine globals -- no locks acquired, no engine state mutated.
    //
    // 14 engines registered, mapping STEP2_OPENER's "HBI, HBG, MCE, CFE,
    // Tsmom V1+V2, Donchian, EmaPullback, TrendRider, RSI variants" to actual
    // globals declared in include/globals.hpp:
    //
    //   HBG          -> g_hybrid_gold
    //   HBI x4       -> g_hybrid_sp / g_hybrid_nq / g_hybrid_us30 / g_hybrid_nas100
    //   MCE          -> g_macro_crash
    //   CFE          -> g_candle_flow
    //   Tsmom V1     -> g_tsmom
    //   Tsmom V2     -> g_tsmom_v2
    //   Donchian     -> g_donchian
    //   EmaPullback  -> g_ema_pullback
    //   TrendRider   -> g_trend_rider
    //   RSI Reversal -> g_rsi_reversal
    //   RSI Extreme  -> g_rsi_extreme
    //
    // Step 3: snapshot lambdas now read from g_engine_last (a side-table written
    // by handle_closed_trade in include/trade_lifecycle.hpp). Each registry name
    // is mapped to one or more trade-record engine strings -- bracket engines
    // use a single literal (e.g. "HybridBracketGold"), cell-based engines use
    // a prefix terminator like "Tsmom_" that matches every cell id ("Tsmom_H1_long",
    // "Tsmom_H4_short", ...) the engine emits via tr.engine = cell_id.
    //
    // The HBI four-pack (HybridSP/NQ/US30/NAS100) all stamp tr.engine =
    // "HybridBracketIndex" today, so until the engine differentiates by symbol
    // the four registry entries will show the SAME last_signal_ts / last_pnl.
    // Documented as a known limitation in EngineLastRegistry.hpp.
    auto reg = [](const char* name,
                  bool enabled,
                  bool shadow_mode,
                  std::initializer_list<const char*> trade_engine_patterns)
        -> omega::EngineSnapshot
    {
        omega::EngineSnapshot s;
        s.name    = name;
        s.enabled = enabled;
        s.mode    = shadow_mode ? "SHADOW" : "LIVE";
        s.state   = enabled ? "RUNNING" : "IDLE";
        const auto last = g_engine_last.get_latest_for_any(trade_engine_patterns);
        s.last_signal_ts = last.last_ts_ms;
        s.last_pnl       = last.last_pnl;
        return s;
    };

    // S11 P3b: HybridGold register_engine block removed (engine culled in P3a + P3b).
    // S-2026-06-02: MidScalperGold registration REMOVED -- the engine was
    // decommissioned (#if 0 in tick_gold.hpp 2026-05-12) but its registration
    // hardcoded enabled=true, so the /engines panel kept showing it RUNNING.
    // 2026-05-08 S19: register MicroScalperGold for /api/v1/omega/engines.
    //   Shadow-stamped initially. last_signal_ts/last_pnl resolved via
    //   g_engine_last lookup against tr.engine="MicroScalperGold".
    g_engines.register_engine("MicroScalperGold",
        [reg]{ return reg("MicroScalperGold",
                          !g_disable_microscalper,
                          g_gold_microscalper.shadow_mode,
                          {"MicroScalperGold"}); });
    // 2026-05-02: register EurusdLondonOpen for /api/v1/omega/engines.
    //   Shadow-stamped initially. last_signal_ts/last_pnl resolved via
    //   g_engine_last lookup against tr.engine="EurusdLondonOpen".
    g_engines.register_engine("EurusdLondonOpen",
        [reg]{ return reg("EurusdLondonOpen",
                          false /* S99 FX kill-switch */,
                          g_eurusd_london_open.shadow_mode,
                          {"EurusdLondonOpen"}); });
    // 2026-05-02: register UsdjpyAsianOpen for /api/v1/omega/engines.
    //   Same shadow-stamped pattern as EurusdLondonOpen.
    g_engines.register_engine("UsdjpyAsianOpen",
        [reg]{ return reg("UsdjpyAsianOpen",
                          false /* S99 FX kill-switch */,
                          g_usdjpy_asian_open.shadow_mode,
                          {"UsdjpyAsianOpen"}); });
    // 2026-05-04: register GbpusdLondonOpen for /api/v1/omega/engines.
    //   Same shadow-stamped pattern as EurusdLondonOpen. last_signal_ts /
    //   last_pnl resolved via g_engine_last lookup against
    //   tr.engine="GbpusdLondonOpen".
    g_engines.register_engine("GbpusdLondonOpen",
        [reg]{ return reg("GbpusdLondonOpen",
                          false /* S99 FX kill-switch */,
                          g_gbpusd_london_open.shadow_mode,
                          {"GbpusdLondonOpen"}); });
    // 2026-05-04: register AudusdSydneyOpen for /api/v1/omega/engines.
    //   Same shadow-stamped pattern as UsdjpyAsianOpen.
    g_engines.register_engine("AudusdSydneyOpen",
        [reg]{ return reg("AudusdSydneyOpen",
                          false /* S99 FX kill-switch */,
                          g_audusd_sydney_open.shadow_mode,
                          {"AudusdSydneyOpen"}); });
    // 2026-05-04: register NzdusdAsianOpen for /api/v1/omega/engines.
    //   Same shadow-stamped pattern as AudusdSydneyOpen.
    g_engines.register_engine("NzdusdAsianOpen",
        [reg]{ return reg("NzdusdAsianOpen",
                          false /* S99 FX kill-switch */,
                          g_nzdusd_asian_open.shadow_mode,
                          {"NzdusdAsianOpen"}); });
    // 2026-05-02: register XauusdFvg for /api/v1/omega/engines.
    //   Shadow-stamped initially (pinned in init block above). last_signal_ts
    //   / last_pnl resolved via g_engine_last lookup against
    //   tr.engine="XauusdFvg" (set in XauusdFvgEngine::_close).
    g_engines.register_engine("XauusdFvg",
        [reg]{ return reg("XauusdFvg",
                          !g_disable_xauusd_fvg,
                          g_xauusd_fvg.shadow_mode,
                          {"XauusdFvg"}); });
    // 2026-05-18: GoldScalpPyramid engine registration.
    // 2026-05-19 S110: GoldRegimeDaily engine registration.
    // S11 P3b: HybridSP / HybridNQ / HybridUS30 / HybridNAS100 register_engine
    //   blocks removed (engines culled in P3a + P3b).
    g_engines.register_engine("CandleFlow",
        [reg]{ return reg("CandleFlow",
                          !g_disable_candle_flow,
                          g_candle_flow.shadow_mode,
                          {"CandleFlowEngine"}); });
    g_engines.register_engine("TrendRider",
        [reg]{ return reg("TrendRider",
                          g_trend_rider.enabled,
                          g_trend_rider.shadow_mode,
                          {"TrendRider_"}); });
    // S-2026-06-02: register the current straddle / ORB / index-straddle book so
    // the /engines panel reflects the live engines instead of only the legacy set.
    g_engines.register_engine("IdxStraddleGER40_M30",
        [reg]{ return reg("IdxStraddleGER40_M30", g_idx_straddle_ger40_m30.enabled, g_idx_straddle_ger40_m30.shadow_mode, {"IdxStraddleGER40_M30"}); });
    g_engines.register_engine("IdxStraddleGER40_M15",
        [reg]{ return reg("IdxStraddleGER40_M15", g_idx_straddle_ger40_m15.enabled, g_idx_straddle_ger40_m15.shadow_mode, {"IdxStraddleGER40_M15"}); });
    g_engines.register_engine("IdxStraddleNAS100_M15",
        [reg]{ return reg("IdxStraddleNAS100_M15", g_idx_straddle_nas_m15.enabled, g_idx_straddle_nas_m15.shadow_mode, {"IdxStraddleNAS100_M15"}); });
    g_engines.register_engine("IdxStraddleNAS100_M30",
        [reg]{ return reg("IdxStraddleNAS100_M30", g_idx_straddle_nas_m30.enabled, g_idx_straddle_nas_m30.shadow_mode, {"IdxStraddleNAS100_M30"}); });
    g_engines.register_engine("IdxStraddleUK100_M30",
        [reg]{ return reg("IdxStraddleUK100_M30", g_idx_straddle_uk100_m30.enabled, g_idx_straddle_uk100_m30.shadow_mode, {"IdxStraddleUK100_M30"}); });
    g_engines.register_engine("IdxStraddleUK100_M240",
        [reg]{ return reg("IdxStraddleUK100_M240", g_idx_straddle_uk100_m240.enabled, g_idx_straddle_uk100_m240.shadow_mode, {"IdxStraddleUK100_M240"}); });
    std::cout << "[OmegaApi] g_engines registered ("
              << g_engines.snapshot_all().size() << " engines)\n";
    std::cout.flush();

    // ── ENGINE_TABLE startup audit surface (S-2026-06-19 Phase 1) ─────────
    // Phase-0 handoff item: dump every registered engine's enabled/mode/state
    // at boot so the live audit surface is one grep away in the stderr log.
    // ROOT REASON: this session-class repeatedly mis-stated which engines were
    // live (FxScalpPyramid x5 defaulted enabled=true; "disabled" docs lied).
    // A boot-time table from the authoritative registry (snapshot_all) makes
    // the truth verifiable directly off the running binary -- no scraping
    // engine_init.hpp's per-engine flags, no /api round-trip.
    //
    // Columns are limited to what the EngineRegistry actually holds today
    // (name/enabled/mode/state). symbol/TF/dir/risk/regime are NOT in the
    // snapshot struct -- adding them needs a per-engine metadata contract,
    // tracked as a Phase-1 follow-on, deliberately not bolted on here.
    {
        auto rows = g_engines.snapshot_all();
        int n_live = 0, n_shadow = 0, n_idle = 0;
        std::cout << "[ENGINE_TABLE] ==== " << rows.size()
                  << " registered engines (boot snapshot) ====\n";
        std::cout << "[ENGINE_TABLE] "
                  << std::left << std::setw(30) << "NAME"
                  << std::setw(9)  << "ENABLED"
                  << std::setw(8)  << "MODE"
                  << std::setw(9)  << "STATE" << "\n";
        for (const auto& r : rows) {
            if (!r.enabled)            ++n_idle;
            else if (r.mode == "LIVE") ++n_live;
            else                       ++n_shadow;
            std::cout << "[ENGINE_TABLE] "
                      << std::left << std::setw(30) << r.name
                      << std::setw(9)  << (r.enabled ? "yes" : "no")
                      << std::setw(8)  << r.mode
                      << std::setw(9)  << r.state << "\n";
        }
        std::cout << "[ENGINE_TABLE] ==== summary: "
                  << n_live   << " LIVE-enabled, "
                  << n_shadow << " SHADOW-enabled, "
                  << n_idle   << " disabled ====\n";
        std::cout.flush();
    }

    // ── EngineHeartbeat registrations (audit-fixes-40 / 2026-05-05) ───────
    // Per-engine liveness registry. Each entry declares:
    //   live_required       -- true if absence triggers MISS / STARTUP-FAIL
    //   expected_cadence_s  -- max seconds between pulses inside session window
    //   session_start_utc   -- UTC hour the engine is expected to be ticking
    //   session_end_utc     -- end of session window (wraparound if end<=start)
    //
    // Cadence rationale:
    //   24/7 engines (gold, Tsmom):     3600s (1h) -- generous slack for low-tape
    //                                                 weekend windows + bar-driven
    //                                                 engines that sleep between
    //                                                 H1/H4 closes.
    //   FX session-windowed engines:    600s (10min) inside their UTC window.
    //   Index session-windowed engines: 900s (15min) -- slot-driven, NY+London core.
    //   Brent / EU indices:             900s (15min) -- liquidity windows.
    //
    // The pulse is wired at the dispatcher level (top of each tick_*.hpp
    // handler) so this catches the exact failure mode that hid for 19h:
    //   active-symbols gate at on_tick.hpp:1786 was dropping FX ticks before
    //   the dispatch chain. With heartbeat wired the STARTUP-SELFTEST fires
    //   60s after init -- any engine that didn't pulse logs [STARTUP-FAIL].
    {
        // ---- Gold engines (24/7 cadence) ---------------------------------
        // S11 P3b: HybridGold heartbeat registration removed (engine culled in P3a + P3b).
        // S-2026-06-19 Phase 1: live_required was hardcoded `true` on engines
        // that are decommissioned/disabled but still PULSE (tick_gold.hpp:35-36
        // pulse unconditionally) -> heartbeat showed them "active" when off.
        // Mirror the engine's real enabled flag, matching the XAU-zoo blocks
        // below. MidScalper has no enabled member (engine #if 0'd) -> literal false.
        g_engine_heartbeat.register_engine("MidScalperGold",     false, 3600,  0, 24);
        g_engine_heartbeat.register_engine("MicroScalperGold",   !g_disable_microscalper, 3600,  6, 22);
        g_engine_heartbeat.register_engine("GoldStack",          true, 3600,  0, 24);
        g_engine_heartbeat.register_engine("CandleFlow",         true, 3600,  0, 24);
        g_engine_heartbeat.register_engine("EMACross",           true, 3600,  0, 24);
        g_engine_heartbeat.register_engine("XauusdFvg",          true, 3600,  0, 24);
        g_engine_heartbeat.register_engine("h1_swing_gold",      g_h1_swing_gold.enabled, 3600,  0, 24);
        g_engine_heartbeat.register_engine("NbmGoldLondon",      true,  900,  7, 14);

        // ---- XAU trend zoo (H1/H2/H4/D1, tick-driven dispatch) ----------
        // 2026-05-26 (Stage 2 of heartbeat coverage rollout): wire the 18
        // XAU trend / structure engines that ship enabled=true. on_tick is
        // forwarded to each from tick_gold.hpp every gold tick, so the
        // pulse fires high-frequency; the 3600s cadence is the silence
        // threshold (1hr no-tick = miss). live_required gated by the
        // engine's own .enabled flag so disabling the engine also disables
        // its heartbeat tracking automatically.
        g_engine_heartbeat.register_engine("XauTrendFollow1h",     g_xau_tf_1h.enabled,            3600,  0, 24);
        g_engine_heartbeat.register_engine("XauTrendFollow2h",     g_xau_tf_2h.enabled,            3600,  0, 24);
        g_engine_heartbeat.register_engine("XauTrendFollow4h",     g_xau_tf_4h.enabled,            3600,  0, 24);
        g_engine_heartbeat.register_engine("XauTrendFollowD1",     g_xau_tf_d1.enabled,            3600,  0, 24);
        g_engine_heartbeat.register_engine("XauSwingBreakD1",      g_xau_swing_break_d1.enabled,   3600,  0, 24);

        // ---- Gold portfolio (H1-bar driven, dispatched from gold ticks) --

        // ---- FX cross-asset engines (Stage 3 heartbeat rollout) ----------

        // ---- Indices (Stage 4 heartbeat rollout) -------------------------
        g_engine_heartbeat.register_engine("XauSessNYpm",          g_xau_sess_nypm.enabled,        3600, 16, 21);
        g_engine_heartbeat.register_engine("NasBbRevLongH1",       g_nas_bbrev_long_h1.enabled,    3600,  7, 22);

        // ---- Tsmom portfolio (5 cells, bar-driven on H1) -----------------
        // bar-driven, but driven from XAUUSD ticks via tick_gold.hpp's
        // forwarding to TsmomPortfolio::on_tick / on_h1_bar. Pulse fires from
        // the gold dispatcher so cadence is high; the 3600s envelope is
        // intentional for weekend safety.

        // ---- FX session-windowed engines (audit-fixes-37 + 41 cohort) ----
        g_engine_heartbeat.register_engine("EurusdLondonOpen",   true,  600,  6,  9);
        g_engine_heartbeat.register_engine("GbpusdLondonOpen",   true,  600,  7, 10);
        g_engine_heartbeat.register_engine("UsdjpyAsianOpen",    true,  600,  0,  4);
        g_engine_heartbeat.register_engine("AudusdSydneyOpen",   true,  600, 22,  2);  // wraparound
        g_engine_heartbeat.register_engine("NzdusdAsianOpen",    true,  600, 22,  4);  // wraparound

        // ---- Index flow + macro engines (NY+London core) ----------------
        // S11 P3b: HybridSP / HybridNQ / HybridUS30 / HybridNAS100 heartbeat
        //   registrations removed (engines culled in P3a + P3b).
        g_engine_heartbeat.register_engine("IFlowSP",            true,  900,  7, 22);
        g_engine_heartbeat.register_engine("IFlowNQ",            true,  900,  7, 22);
        g_engine_heartbeat.register_engine("IFlowUS30",          true,  900,  7, 22);
        g_engine_heartbeat.register_engine("IFlowNAS100",        true,  900,  7, 22);
        g_engine_heartbeat.register_engine("IMacroSP",           true,  900,  7, 22);
        g_engine_heartbeat.register_engine("IMacroNQ",           true,  900,  7, 22);
        g_engine_heartbeat.register_engine("IMacroUS30",         true,  900,  7, 22);
        g_engine_heartbeat.register_engine("IMacroNAS",          true,  900,  7, 22);

        // ---- TrendPullback indices ---------------------------------------
        g_engine_heartbeat.register_engine("TrendPullbackSP",    true,  900,  7, 22);
        g_engine_heartbeat.register_engine("TrendPullbackNQ",    true,  900,  7, 22);

        // ---- Minimal H4 US30 ---------------------------------------------
        g_engine_heartbeat.register_engine("MinimalH4US30",      true,  900,  7, 22);

        // ---- Cross-asset / commodities / EU indices ----------------------
        g_engine_heartbeat.register_engine("BrentEngine",        true,  900,  7, 22);
        g_engine_heartbeat.register_engine("UsoilEngine",        true,  900,  7, 22);
        g_engine_heartbeat.register_engine("Ger40",              true,  900,  7, 22);
        g_engine_heartbeat.register_engine("Uk100",              true,  900,  7, 22);
        g_engine_heartbeat.register_engine("Estx50",             true,  900,  7, 22);

        std::cout << "[HEARTBEAT] g_engine_heartbeat registered ("
                  << g_engine_heartbeat.size() << " engines)\n";
        std::cout.flush();
    }

    // ── Step 3: open-position sources for /api/v1/omega/positions ─────────
    // S11 P3b: HybridGold open-position source removed (engine culled in P3a +
    //   P3b). MidScalperGold below is the canonical XAUUSD source now.
    // 2026-05-01 SESSION_h: GoldMidScalper open-position source (parallel pattern to former HybridGold source).
    g_open_positions.register_source("MidScalperGold",
        []() -> std::vector<omega::PositionSnapshot> {
            std::vector<omega::PositionSnapshot> out;
            if (!g_gold_midscalper.has_open_position()) return out;

            const auto& p = g_gold_midscalper.pos;
            const double mult  = tick_value_multiplier(std::string("XAUUSD"));

            double current = p.entry;
            const auto it = g_last_tick_bid.find("XAUUSD");
            if (it != g_last_tick_bid.end() && it->second > 0.0) {
                current = it->second;
            }

            const double dir   = p.is_long ? 1.0 : -1.0;
            const double unrl  = (current - p.entry) * dir * p.size * mult;

            omega::PositionSnapshot ps;
            ps.symbol         = "XAUUSD";
            ps.side           = p.is_long ? "LONG" : "SHORT";
            ps.size           = p.size;
            ps.entry          = p.entry;
            ps.current        = current;
            ps.unrealized_pnl = unrl;
            ps.mfe            = p.mfe * p.size * mult;
            ps.mae            = p.mae * p.size * mult;
            ps.engine         = "MidScalperGold";
            out.push_back(ps);
            return out;
        });
    // 2026-05-08 S19: GoldMicroScalper open-position source. Same shape as
    //   MidScalperGold above; engine has the same pos struct fields
    //   (is_long, entry, size, mfe, mae). XAUUSD tick-value mult applied.
    g_open_positions.register_source("MicroScalperGold",
        []() -> std::vector<omega::PositionSnapshot> {
            std::vector<omega::PositionSnapshot> out;
            if (!g_gold_microscalper.has_open_position()) return out;

            const auto& p = g_gold_microscalper.pos;
            const double mult  = tick_value_multiplier(std::string("XAUUSD"));

            double current = p.entry;
            const auto it = g_last_tick_bid.find("XAUUSD");
            if (it != g_last_tick_bid.end() && it->second > 0.0) {
                current = it->second;
            }

            const double dir   = p.is_long ? 1.0 : -1.0;
            const double unrl  = (current - p.entry) * dir * p.size * mult;

            omega::PositionSnapshot ps;
            ps.symbol         = "XAUUSD";
            ps.side           = p.is_long ? "LONG" : "SHORT";
            ps.size           = p.size;
            ps.entry          = p.entry;
            ps.current        = current;
            ps.unrealized_pnl = unrl;
            ps.mfe            = p.mfe * p.size * mult;
            ps.mae            = p.mae * p.size * mult;
            ps.engine         = "MicroScalperGold";
            out.push_back(ps);
            return out;
        });
    // 2026-05-02: EurusdLondonOpen open-position source (parallel to MidScalperGold).
    //   Mirrors the pattern above, with EURUSD tick-value lookup. tick_value_multiplier
    //   on EURUSD returns the per-pip USD value normalised for unit price moves --
    //   for EURUSD this is typically 100000 (1.0 standard lot) or 10000 at 0.10 lot,
    //   resolved inside tick_value_multiplier from g_sym_cfg / FIX defaults.
    g_open_positions.register_source("EurusdLondonOpen",
        []() -> std::vector<omega::PositionSnapshot> {
            std::vector<omega::PositionSnapshot> out;
            if (!g_eurusd_london_open.has_open_position()) return out;

            const auto& p = g_eurusd_london_open.pos;
            const double mult  = tick_value_multiplier(std::string("EURUSD"));

            double current = p.entry;
            const auto it = g_last_tick_bid.find("EURUSD");
            if (it != g_last_tick_bid.end() && it->second > 0.0) {
                current = it->second;
            }

            const double dir   = p.is_long ? 1.0 : -1.0;
            const double unrl  = (current - p.entry) * dir * p.size * mult;

            omega::PositionSnapshot ps;
            ps.symbol         = "EURUSD";
            ps.side           = p.is_long ? "LONG" : "SHORT";
            ps.size           = p.size;
            ps.entry          = p.entry;
            ps.current        = current;
            ps.unrealized_pnl = unrl;
            ps.mfe            = p.mfe * p.size * mult;
            ps.mae            = p.mae * p.size * mult;
            ps.engine         = "EurusdLondonOpen";
            out.push_back(ps);
            return out;
        });
    // 2026-05-02: UsdjpyAsianOpen open-position source (parallel to EurusdLondonOpen).
    //   Mirrors the EURUSD pattern with USDJPY tick-value lookup.
    //   tick_value_multiplier on USDJPY returns 100000.0/g_usdjpy_mid (live JPY/USD
    //   rate) so unrealized PnL tracks the live conversion -- avoids the static
    //   approximation drifting ~8% as the rate moves between 140-160.
    g_open_positions.register_source("UsdjpyAsianOpen",
        []() -> std::vector<omega::PositionSnapshot> {
            std::vector<omega::PositionSnapshot> out;
            if (!g_usdjpy_asian_open.has_open_position()) return out;

            const auto& p = g_usdjpy_asian_open.pos;
            const double mult  = tick_value_multiplier(std::string("USDJPY"));

            double current = p.entry;
            const auto it = g_last_tick_bid.find("USDJPY");
            if (it != g_last_tick_bid.end() && it->second > 0.0) {
                current = it->second;
            }

            const double dir   = p.is_long ? 1.0 : -1.0;
            const double unrl  = (current - p.entry) * dir * p.size * mult;

            omega::PositionSnapshot ps;
            ps.symbol         = "USDJPY";
            ps.side           = p.is_long ? "LONG" : "SHORT";
            ps.size           = p.size;
            ps.entry          = p.entry;
            ps.current        = current;
            ps.unrealized_pnl = unrl;
            ps.mfe            = p.mfe * p.size * mult;
            ps.mae            = p.mae * p.size * mult;
            ps.engine         = "UsdjpyAsianOpen";
            out.push_back(ps);
            return out;
        });
    // 2026-05-04: GbpusdLondonOpen open-position source (parallel to EurusdLondonOpen).
    //   Mirrors the EUR pattern with GBPUSD tick-value lookup. tick_value_multiplier
    //   on GBPUSD resolves through the standard USD-quote path (no live cross-rate
    //   conversion needed -- USD is the quote currency, identical to EUR).
    g_open_positions.register_source("GbpusdLondonOpen",
        []() -> std::vector<omega::PositionSnapshot> {
            std::vector<omega::PositionSnapshot> out;
            if (!g_gbpusd_london_open.has_open_position()) return out;

            const auto& p = g_gbpusd_london_open.pos;
            const double mult  = tick_value_multiplier(std::string("GBPUSD"));

            double current = p.entry;
            const auto it = g_last_tick_bid.find("GBPUSD");
            if (it != g_last_tick_bid.end() && it->second > 0.0) {
                current = it->second;
            }

            const double dir   = p.is_long ? 1.0 : -1.0;
            const double unrl  = (current - p.entry) * dir * p.size * mult;

            omega::PositionSnapshot ps;
            ps.symbol         = "GBPUSD";
            ps.side           = p.is_long ? "LONG" : "SHORT";
            ps.size           = p.size;
            ps.entry          = p.entry;
            ps.current        = current;
            ps.unrealized_pnl = unrl;
            ps.mfe            = p.mfe * p.size * mult;
            ps.mae            = p.mae * p.size * mult;
            ps.engine         = "GbpusdLondonOpen";
            out.push_back(ps);
            return out;
        });
    // 2026-05-04: AudusdSydneyOpen open-position source (parallel to UsdjpyAsianOpen).
    //   Mirrors the EUR pattern with AUDUSD tick-value lookup. tick_value_multiplier
    //   on AUDUSD resolves through the standard USD-quote path (USD is the quote
    //   currency, identical to EUR/GBP -- no live cross-rate conversion needed,
    //   unlike USDJPY which uses g_usdjpy_mid).
    g_open_positions.register_source("AudusdSydneyOpen",
        []() -> std::vector<omega::PositionSnapshot> {
            std::vector<omega::PositionSnapshot> out;
            if (!g_audusd_sydney_open.has_open_position()) return out;

            const auto& p = g_audusd_sydney_open.pos;
            const double mult  = tick_value_multiplier(std::string("AUDUSD"));

            double current = p.entry;
            const auto it = g_last_tick_bid.find("AUDUSD");
            if (it != g_last_tick_bid.end() && it->second > 0.0) {
                current = it->second;
            }

            const double dir   = p.is_long ? 1.0 : -1.0;
            const double unrl  = (current - p.entry) * dir * p.size * mult;

            omega::PositionSnapshot ps;
            ps.symbol         = "AUDUSD";
            ps.side           = p.is_long ? "LONG" : "SHORT";
            ps.size           = p.size;
            ps.entry          = p.entry;
            ps.current        = current;
            ps.unrealized_pnl = unrl;
            ps.mfe            = p.mfe * p.size * mult;
            ps.mae            = p.mae * p.size * mult;
            ps.engine         = "AudusdSydneyOpen";
            out.push_back(ps);
            return out;
        });
    // 2026-05-04: NzdusdAsianOpen open-position source (parallel to AudusdSydneyOpen).
    //   Mirrors the AUD pattern with NZDUSD tick-value lookup.
    //   tick_value_multiplier on NZDUSD resolves through the standard
    //   USD-quote path (USD is the quote currency, identical to EUR/GBP/AUD --
    //   no live cross-rate conversion needed, unlike USDJPY which uses
    //   g_usdjpy_mid).
    g_open_positions.register_source("NzdusdAsianOpen",
        []() -> std::vector<omega::PositionSnapshot> {
            std::vector<omega::PositionSnapshot> out;
            if (!g_nzdusd_asian_open.has_open_position()) return out;

            const auto& p = g_nzdusd_asian_open.pos;
            const double mult  = tick_value_multiplier(std::string("NZDUSD"));

            double current = p.entry;
            const auto it = g_last_tick_bid.find("NZDUSD");
            if (it != g_last_tick_bid.end() && it->second > 0.0) {
                current = it->second;
            }

            const double dir   = p.is_long ? 1.0 : -1.0;
            const double unrl  = (current - p.entry) * dir * p.size * mult;

            omega::PositionSnapshot ps;
            ps.symbol         = "NZDUSD";
            ps.side           = p.is_long ? "LONG" : "SHORT";
            ps.size           = p.size;
            ps.entry          = p.entry;
            ps.current        = current;
            ps.unrealized_pnl = unrl;
            ps.mfe            = p.mfe * p.size * mult;
            ps.mae            = p.mae * p.size * mult;
            ps.engine         = "NzdusdAsianOpen";
            out.push_back(ps);
            return out;
        });
    // 2026-05-02: XauusdFvg open-position source.
    //   tick_value_multiplier on XAUUSD returns 100 USD per price-point per
    //   lot (per OmegaTradeLedger.hpp:88 / SymbolConfig). Unrealized PnL
    //   tracks the live mid relative to the gross entry boundary; the engine
    //   stores entry as zone_high/low (gross), entry_with_cost separately.
    g_open_positions.register_source("XauusdFvg",
        []() -> std::vector<omega::PositionSnapshot> {
            std::vector<omega::PositionSnapshot> out;
            if (!g_xauusd_fvg.has_open_position()) return out;

            const auto& p = g_xauusd_fvg.m_pos;
            const double mult = tick_value_multiplier(std::string("XAUUSD"));

            double current = p.entry;
            const auto it = g_last_tick_bid.find("XAUUSD");
            if (it != g_last_tick_bid.end() && it->second > 0.0) {
                current = it->second;
            }

            const double dir  = p.is_long ? 1.0 : -1.0;
            const double unrl = (current - p.entry) * dir * p.size * mult;

            omega::PositionSnapshot ps;
            ps.symbol         = "XAUUSD";
            ps.side           = p.is_long ? "LONG" : "SHORT";
            ps.size           = p.size;
            ps.entry          = p.entry;
            ps.current        = current;
            ps.unrealized_pnl = unrl;
            ps.mfe            = p.mfe * p.size * mult;
            ps.mae            = p.mae * p.size * mult;
            ps.engine         = "XauusdFvg";
            out.push_back(ps);
            return out;
        });
    // 2026-05-18: GoldScalpPyramid position source.
    // FxScalpPyramid x5 position-source registration REMOVED (S-2026-06-29):
    //   dead -- the register_fx_scalp lambda was (void)-cast/never invoked and
    //   the g_fx_scalp_* globals are retired (retired_micro_engines.hpp). "No FX".
    // 2026-05-19 S110: GoldRegimeDaily position source.
    // ====================================================================
    // S65 2026-05-13 GUI position-source expansion
    // --------------------------------------------------------------------
    // The /api/v1/omega/positions endpoint only sees engines that register
    // themselves here. Pre-S65 this was 8 sources -- which meant any in-
    // flight trade from a non-FX-London-Open engine was invisible in the
    // GUI until close (it would only appear in the standard trade ledger
    // after _close fires). The block below adds open-position visibility
    // for the engines that share a compatible pos shape with the existing
    // template.
    //
    // NOT yet registered (different / multi-leg pos shapes -- follow-up):
    //   BracketEngine (XAU + 12 FX/index instances)   -- pyramid leg array
    //   GoldEngineStack (18 sub-engines)              -- legs_ vector
    //   IndexFlowEngine / IndexMacroCrash / IndexSwing -- base_entry_ etc.
    //   CandleFlowEngine (3 paths)                    -- multi-path state
    //   XauTrendFollow 2h / 4h / D1                   -- TODO: check pos shape
    //   UstecTrendFollow 5m / HTF                     -- TODO: check pos shape
    //   EMACrossEngine, H4RegimeEngine, BreakoutEngine, MacroCrashEngine
    //   C1RetunedPortfolio                            -- DONE 2026-05-14b
    //                                                    (part M) — 4 cells
    //                                                    registered below
    //                                                    via per-cell pos()
    //                                                    accessors.
    //
    // Add those as a follow-up commit once each engine's pos struct is
    // mapped. The deferred set is the "show in GUI when fires" follow-up.
    // ====================================================================

    // PDHLReversion (XAU). pos has full {active, is_long, entry, size, mfe, mae}.

    // RSIReversal (XAU). pos has {active, is_long, entry, size, mfe}; no mae.

    // MinimalH4Breakout (XAU). pos_ has {active, is_long, entry, size}; no mfe/mae.

    // MinimalH4US30Breakout (DJ30.F). Same shape as MinimalH4Gold.

    // XauThreeBar30m (XAU). pos has {active, is_long, entry_px, mfe_pts, mae_pts}.
    // NOTE: XauThreeBar30mPos has no `size` member -- the engine's lot size
    // lives on the engine itself as `g_xau_threebar_30m.lot`. Build fix
    // 2026-05-13: was previously referencing p.size which fails compile.
    g_open_positions.register_source("XauThreeBar30m",
        []() -> std::vector<omega::PositionSnapshot> {
            std::vector<omega::PositionSnapshot> out;
            if (!g_xau_threebar_30m.has_open_position()) return out;
            const auto& p = g_xau_threebar_30m.pos;
            const double sz   = g_xau_threebar_30m.lot;
            const double mult = tick_value_multiplier(std::string("XAUUSD"));
            double current = p.entry_px;
            const auto it = g_last_tick_bid.find("XAUUSD");
            if (it != g_last_tick_bid.end() && it->second > 0.0) current = it->second;
            const double dir   = p.is_long ? 1.0 : -1.0;
            const double unrl  = (current - p.entry_px) * dir * sz * mult;
            omega::PositionSnapshot ps;
            ps.symbol = "XAUUSD"; ps.side = p.is_long ? "LONG" : "SHORT";
            ps.size = sz; ps.entry = p.entry_px; ps.current = current;
            ps.unrealized_pnl = unrl;
            ps.mfe = p.mfe_pts * sz * mult;
            ps.mae = p.mae_pts * sz * mult;
            ps.engine = "XauThreeBar30m";
            out.push_back(ps);
            return out;
        });

    // Straddle engines (S-2026-06-02): XAU + index OCO breakout straddles, all
    // XauStraddleM30Engine instances. Were holding positions for hours with zero
    // live visibility — register so they flow into live_trades via the universal
    // publisher in on_tick.hpp. reg_straddle captures the engine by pointer (a
    // global → valid for process life) so the snapshotter is safe to escape.
    {
        auto reg_straddle = [](const char* label, const char* sym,
                               const omega::XauStraddleM30Engine* e) {
            g_open_positions.register_source(label,
                [e, sym, label]() -> std::vector<omega::PositionSnapshot> {
                    std::vector<omega::PositionSnapshot> out;
                    if (!e->has_open_position()) return out;
                    const auto& p = e->pos_;
                    const double mult = tick_value_multiplier(std::string(sym));
                    double current = p.entry;
                    const auto it = g_last_tick_bid.find(sym);
                    if (it != g_last_tick_bid.end() && it->second > 0.0) current = it->second;
                    const double dir = (p.side > 0) ? 1.0 : -1.0;
                    omega::PositionSnapshot ps;
                    ps.symbol = sym; ps.side = (p.side > 0) ? "LONG" : "SHORT";
                    ps.size = p.lot; ps.entry = p.entry; ps.current = current;
                    ps.unrealized_pnl = (current - p.entry) * dir * p.lot * mult;
                    ps.tp = p.tp; ps.sl = p.sl; ps.entry_ts = p.entry_ts_ms / 1000;
                    ps.engine = label;
                    out.push_back(ps);
                    return out;
                });
        };
        reg_straddle("IdxStraddleGER40_M30", "GER40",  &g_idx_straddle_ger40_m30);
        reg_straddle("IdxStraddleGER40_M15", "GER40",  &g_idx_straddle_ger40_m15);
        reg_straddle("IdxStraddleNAS100_M15","NAS100", &g_idx_straddle_nas_m15);
        reg_straddle("IdxStraddleNAS100_M30","NAS100", &g_idx_straddle_nas_m30);
        reg_straddle("IdxStraddleUK100_M30", "UK100",  &g_idx_straddle_uk100_m30);
        reg_straddle("IdxStraddleUK100_M240","UK100",  &g_idx_straddle_uk100_m240);
    }

    // Ger40TurtleH4 (S-2026-06-02): pos_ has no direction field — derive from
    // sl vs entry (sl below entry => long). tp/sl/entry_ts available.

    // Ger40KeltnerH1 (S-2026-06-02): long-only (short side tombstoned). No tp
    // field (trail-managed) → tp=0.

    // SurvivorPortfolio (S-2026-06-02): 13-cell GER/XAU/USTEC/USDJPY book. One
    // source returns every open cell (cfg.tag as engine label).
    g_open_positions.register_source("Survivor",
        []() -> std::vector<omega::PositionSnapshot> {
            std::vector<omega::PositionSnapshot> out;
            for (const auto& c : g_survivor.cells) {
                if (!c.st.pos_active) continue;
                const char* sym = c.cfg.symbol;
                double current = c.st.pos_entry;
                const auto it = g_last_tick_bid.find(sym);
                if (it != g_last_tick_bid.end() && it->second > 0.0) current = it->second;
                const double dir = (c.st.pos_side > 0) ? 1.0 : -1.0;
                omega::PositionSnapshot ps;
                ps.symbol = sym;
                ps.side = (c.st.pos_side > 0) ? "LONG" : "SHORT";
                ps.size = c.cfg.lot; ps.entry = c.st.pos_entry; ps.current = current;
                ps.unrealized_pnl = (current - c.st.pos_entry) * dir
                                    * c.cfg.lot * c.cfg.tick_usd_per_lot;
                ps.tp = c.st.pos_tp; ps.sl = c.st.pos_sl;
                ps.entry_ts = c.st.pos_entry_ts;
                ps.engine = c.cfg.tag;
                out.push_back(ps);
            }
            return out;
        });

    // NoiseBandMomentum gold-london. Uses CrossPosition (active, is_long,
    //   entry, size, mfe, mae) -- private; we go through public accessors
    //   open_is_long() / open_entry() / open_size(). mfe/mae deferred (no
    //   public getter on CrossPosition.mfe yet -- S66 follow-up if needed).

    // VWAPReversion x 4 instances. CrossPosition pos_ is private; we go
    // through the S65 public accessors (added to VWAPReversionEngine in
    // CrossAssetEngines.hpp to match the NBM/TrendPullback convention).
    auto _make_vwap_source = [](const char* engine_name, const char* sym,
                                omega::cross::VWAPReversionEngine* eng) {
        return [engine_name, sym, eng]() -> std::vector<omega::PositionSnapshot> {
            std::vector<omega::PositionSnapshot> out;
            if (!eng->has_open_position()) return out;
            const bool   is_long = eng->open_is_long();
            const double entry   = eng->open_entry();
            const double sz      = eng->open_size();
            const double mult = tick_value_multiplier(std::string(sym));
            double current = entry;
            const auto it = g_last_tick_bid.find(sym);
            if (it != g_last_tick_bid.end() && it->second > 0.0) current = it->second;
            const double dir   = is_long ? 1.0 : -1.0;
            const double unrl  = (current - entry) * dir * sz * mult;
            omega::PositionSnapshot ps;
            ps.symbol = sym; ps.side = is_long ? "LONG" : "SHORT";
            ps.size = sz; ps.entry = entry; ps.current = current;
            ps.unrealized_pnl = unrl;
            ps.mfe = 0.0;  // CrossPosition.mfe is private; defer to S66
            ps.mae = 0.0;
            ps.engine = engine_name;
            out.push_back(ps);
            return out;
        };
    };
    (void)_make_vwap_source;

    // TrendPullback x 2 instances (gold + nq, the LIVE pair per part-F).
    // CrossPosition pos_ is private; use the public accessors already
    // exposed by TrendPullbackEngine (open_is_long/open_entry/open_size).
    auto _make_tpb_source = [](const char* engine_name, const char* sym,
                               omega::cross::TrendPullbackEngine* eng) {
        return [engine_name, sym, eng]() -> std::vector<omega::PositionSnapshot> {
            std::vector<omega::PositionSnapshot> out;
            if (!eng->has_open_position()) return out;
            const bool   is_long = eng->open_is_long();
            const double entry   = eng->open_entry();
            const double sz      = eng->open_size();
            const double mult = tick_value_multiplier(std::string(sym));
            double current = entry;
            const auto it = g_last_tick_bid.find(sym);
            if (it != g_last_tick_bid.end() && it->second > 0.0) current = it->second;
            const double dir   = is_long ? 1.0 : -1.0;
            const double unrl  = (current - entry) * dir * sz * mult;
            omega::PositionSnapshot ps;
            ps.symbol = sym; ps.side = is_long ? "LONG" : "SHORT";
            ps.size = sz; ps.entry = entry; ps.current = current;
            ps.unrealized_pnl = unrl;
            ps.mfe = 0.0;  // CrossPosition.mfe is private; defer to S66
            ps.mae = 0.0;
            ps.engine = engine_name;
            out.push_back(ps);
            return out;
        };
    };
    (void)_make_tpb_source;

    // ── S66-followup-2 (2026-05-14 part L): IndexFlowEngine x4 GUI sources.
    //   Uses the new IndexFlowEngine::pos() const accessor (added in
    //   IndexFlowEngine.hpp this session). pos has the full simple shape
    //   {active, is_long, entry, tp, sl, size, mfe, mae} — same template
    //   as PDHL/EMACrossGold/etc. Symbol mapping mirrors VWAPReversion:
    //   US500.F / USTEC.F / NAS100 / DJ30.F. tick_value_multiplier supports
    //   all four (sizing.hpp).
    auto _make_iflow_source = [](const char* engine_name, const char* sym,
                                 omega::idx::IndexFlowEngine* eng) {
        return [engine_name, sym, eng]() -> std::vector<omega::PositionSnapshot> {
            std::vector<omega::PositionSnapshot> out;
            if (!eng->has_open_position()) return out;
            const auto& p = eng->pos();
            const double mult = tick_value_multiplier(std::string(sym));
            double current = p.entry;
            const auto it = g_last_tick_bid.find(sym);
            if (it != g_last_tick_bid.end() && it->second > 0.0) current = it->second;
            const double dir  = p.is_long ? 1.0 : -1.0;
            const double unrl = (current - p.entry) * dir * p.size * mult;
            omega::PositionSnapshot ps;
            ps.symbol = sym;
            ps.side   = p.is_long ? "LONG" : "SHORT";
            ps.size   = p.size;
            ps.entry  = p.entry;
            ps.current = current;
            ps.unrealized_pnl = unrl;
            ps.mfe = p.mfe * p.size * mult;
            ps.mae = p.mae * p.size * mult;
            ps.engine = engine_name;
            out.push_back(ps);
            return out;
        };
    };
    g_open_positions.register_source("IndexFlowSP",
        _make_iflow_source("IndexFlowSP",   "US500.F", &g_iflow_sp));
    g_open_positions.register_source("IndexFlowNQ",
        _make_iflow_source("IndexFlowNQ",   "USTEC.F", &g_iflow_nq));
    g_open_positions.register_source("IndexFlowNAS",
        _make_iflow_source("IndexFlowNAS",  "NAS100",  &g_iflow_nas));
    g_open_positions.register_source("IndexFlowUS30",
        _make_iflow_source("IndexFlowUS30", "DJ30.F",  &g_iflow_us30));

    // ── S66-followup-2 (2026-05-14a): IndexMacroCrashEngine x4 GUI sources.
    //   Uses the new is_long()/entry()/sl()/mfe()/size() accessors added to
    //   IndexMacroCrashEngine this session (IndexFlowEngine.hpp ~L987-1025).
    //   Engine does NOT track MAE (no base_mae_), so the snapshot synthesises
    //   mae = 0.0. Symbol mapping mirrors IndexFlow: US500.F / USTEC.F /
    //   NAS100 / DJ30.F (same four-instrument parity as the rest of the
    //   index family). Effective size accounts for the bracket-floor leg:
    //   the size() accessor inside the engine returns velocity_size_ once
    //   the 30% bracket has fired, so the GUI shows the actually-open lot
    //   rather than the original base_size_.
    auto _make_imacro_source = [](const char* engine_name, const char* sym,
                                  omega::idx::IndexMacroCrashEngine* eng) {
        return [engine_name, sym, eng]() -> std::vector<omega::PositionSnapshot> {
            std::vector<omega::PositionSnapshot> out;
            if (!eng->has_open_position()) return out;
            const double mult  = tick_value_multiplier(std::string(sym));
            const double entry = eng->entry();
            const double sz    = eng->size();
            const bool   is_long = eng->is_long();
            double current = entry;
            const auto it = g_last_tick_bid.find(sym);
            if (it != g_last_tick_bid.end() && it->second > 0.0) current = it->second;
            const double dir  = is_long ? 1.0 : -1.0;
            const double unrl = (current - entry) * dir * sz * mult;
            omega::PositionSnapshot ps;
            ps.symbol = sym;
            ps.side   = is_long ? "LONG" : "SHORT";
            ps.size   = sz;
            ps.entry  = entry;
            ps.current = current;
            ps.unrealized_pnl = unrl;
            ps.mfe = eng->mfe() * sz * mult;
            ps.mae = 0.0;  // engine doesn't track MAE
            ps.engine = engine_name;
            out.push_back(ps);
            return out;
        };
    };
    g_open_positions.register_source("IndexMacroCrashSP",
        _make_imacro_source("IndexMacroCrashSP",   "US500.F", &g_imacro_sp));
    g_open_positions.register_source("IndexMacroCrashNQ",
        _make_imacro_source("IndexMacroCrashNQ",   "USTEC.F", &g_imacro_nq));
    g_open_positions.register_source("IndexMacroCrashNAS",
        _make_imacro_source("IndexMacroCrashNAS",  "NAS100",  &g_imacro_nas));
    g_open_positions.register_source("IndexMacroCrashUS30",
        _make_imacro_source("IndexMacroCrashUS30", "DJ30.F",  &g_imacro_us30));

    // ── S66-followup-2 (2026-05-14b part M): C1RetunedPortfolio x4 GUI sources.
    //   The portfolio (g_c1_retuned, globals.hpp:138) is a long-only XAUUSD
    //   strategy with FOUR cell instances split across two cell classes:
    //     - donchian_h1_   (C1DonchianH1LongCell)
    //     - bollinger_h2_  (C1BollingerLongCell, stride-2 synthesised)
    //     - bollinger_h4_  (C1BollingerLongCell, native H4)
    //     - bollinger_h6_  (C1BollingerLongCell, stride-6 synthesised)
    //   Each cell now exposes a const C1OpenPos& pos() accessor (added this
    //   session in C1RetunedPortfolio.hpp on both cell classes). The cells
    //   are long-only by class invariant — there is no is_long field on
    //   C1OpenPos — so we hard-code side="LONG" / dir=1.0. mfe and mae are
    //   tracked on the cell-level pos_, so we report real values (unlike
    //   IndexMacroCrash above which synthesises mae=0). The "two sub-strategies"
    //   wording in part-L's queued plan referred to the two cell classes;
    //   the actual GUI surface is four sources because each instance has
    //   independent open-position state. Two factory lambdas (one per cell
    //   class) match the part-K/L per-engine-family factory convention.
    auto _make_c1_donchian_source =
        [](const char* engine_name, omega::C1DonchianH1LongCell* cell) {
        return [engine_name, cell]() -> std::vector<omega::PositionSnapshot> {
            std::vector<omega::PositionSnapshot> out;
            if (!cell->has_open_position()) return out;
            const auto& p = cell->pos();
            const double mult = tick_value_multiplier(std::string("XAUUSD"));
            double current = p.entry;
            const auto it = g_last_tick_bid.find("XAUUSD");
            if (it != g_last_tick_bid.end() && it->second > 0.0) current = it->second;
            const double unrl = (current - p.entry) * p.size * mult;  // long-only
            omega::PositionSnapshot ps;
            ps.symbol = "XAUUSD";
            ps.side   = "LONG";
            ps.size   = p.size;
            ps.entry  = p.entry;
            ps.current = current;
            ps.unrealized_pnl = unrl;
            ps.mfe = p.mfe * p.size * mult;
            ps.mae = p.mae * p.size * mult;
            ps.engine = engine_name;
            out.push_back(ps);
            return out;
        };
    };
    (void)_make_c1_donchian_source;
    auto _make_c1_bollinger_source =
        [](const char* engine_name, omega::C1BollingerLongCell* cell) {
        return [engine_name, cell]() -> std::vector<omega::PositionSnapshot> {
            std::vector<omega::PositionSnapshot> out;
            if (!cell->has_open_position()) return out;
            const auto& p = cell->pos();
            const double mult = tick_value_multiplier(std::string("XAUUSD"));
            double current = p.entry;
            const auto it = g_last_tick_bid.find("XAUUSD");
            if (it != g_last_tick_bid.end() && it->second > 0.0) current = it->second;
            const double unrl = (current - p.entry) * p.size * mult;  // long-only
            omega::PositionSnapshot ps;
            ps.symbol = "XAUUSD";
            ps.side   = "LONG";
            ps.size   = p.size;
            ps.entry  = p.entry;
            ps.current = current;
            ps.unrealized_pnl = unrl;
            ps.mfe = p.mfe * p.size * mult;
            ps.mae = p.mae * p.size * mult;
            ps.engine = engine_name;
            out.push_back(ps);
            return out;
        };
    };
    (void)_make_c1_bollinger_source;

    // ── S66 (2026-05-13): 6 more engines (EMACross, H4RegimeGold,
    //    MacroCrash, XauTrendFollow 2h/4h/D1). Mechanical follow-ups to S65.
    //    Engines NOT registered yet (different pos shapes): BracketEngine
    //    multi-leg (XAU + 12 FX/index instances), GoldEngineStack legs_,
    //    CandleFlow (multi-path), H1SwingGold, MinimalH4 portfolio engines
    //    (Donchian/EmaPullback/TrendRider/Tsmom).
    //    UstecTrendFollow + FX BreakoutEngine x5 are NOW registered (below).
    //    IndexFlow x4 is NOW registered (block immediately above this).
    //    IndexMacroCrash x4 is NOW registered (block immediately above this,
    //    2026-05-14a).
    //    C1RetunedPortfolio x4 is NOW registered (block immediately above
    //    this, 2026-05-14b part M — note the C1Retuned wrapper has FOUR
    //    cells, not the "two-leg" wording from the part-L queued plan).

    // EMACrossGold (XAUUSD). EMACrossEngine.pos is public struct OpenPos
    //   with {active, is_long, entry, sl, tp, size, mfe}. No mae tracked.
    g_open_positions.register_source("EMACrossGold",
        []() -> std::vector<omega::PositionSnapshot> {
            std::vector<omega::PositionSnapshot> out;
            if (!g_ema_cross.has_open_position()) return out;
            const auto& p = g_ema_cross.pos;
            const double mult = tick_value_multiplier(std::string("XAUUSD"));
            double current = p.entry;
            const auto it = g_last_tick_bid.find("XAUUSD");
            if (it != g_last_tick_bid.end() && it->second > 0.0) current = it->second;
            const double dir   = p.is_long ? 1.0 : -1.0;
            const double unrl  = (current - p.entry) * dir * p.size * mult;
            omega::PositionSnapshot ps;
            ps.symbol = "XAUUSD"; ps.side = p.is_long ? "LONG" : "SHORT";
            ps.size = p.size; ps.entry = p.entry; ps.current = current;
            ps.unrealized_pnl = unrl;
            ps.mfe = p.mfe * p.size * mult;
            ps.mae = 0.0;  // EMACrossEngine doesn't track mae
            ps.engine = "EMACrossGold";
            out.push_back(ps);
            return out;
        });

    // H4RegimeGold (XAUUSD). H4RegimeEngine is a struct (public by default)
    //   with pos_.{active, is_long, entry, sl, tp, size, mfe, h4_atr, ...}.

    // MacroCrash (XAUUSD). MacroCrashEngine.pos is public struct Position
    //   with {active, is_long, entry, sl, full_size, size, mfe, mae, ...}.
    //   Reports the BASE position only (size, not full_size). Pyramid adds
    //   ride alongside but are tracked separately in pyramid_adds[].

    // XauTrendFollow x 3 (2h, 4h, D1). All three are multi-cell engines:
    //   pos is std::array<XauTf{2h,,D1}Pos, kXauTf{2h,,D1}NumCells>. Each
    //   cell can hold an independent position. We emit one PositionSnapshot
    //   per active cell. Field names are identical across all three pos
    //   structs (active, is_long, entry_px, mfe, mae) so one generic lambda
    //   factory handles them all -- C++14 generic lambdas.
    auto _make_xau_tf_source = [](const char* engine_name, auto* eng) {
        return [engine_name, eng]() -> std::vector<omega::PositionSnapshot> {
            std::vector<omega::PositionSnapshot> out;
            const double mult = tick_value_multiplier(std::string("XAUUSD"));
            double current_mid = 0.0;
            const auto it = g_last_tick_bid.find("XAUUSD");
            if (it != g_last_tick_bid.end() && it->second > 0.0) current_mid = it->second;
            const double sz = eng->lot;
            for (size_t i = 0; i < eng->pos.size(); ++i) {
                const auto& p = eng->pos[i];
                if (!p.active) continue;
                const double use_current = (current_mid > 0.0) ? current_mid : p.entry_px;
                const double dir  = p.is_long ? 1.0 : -1.0;
                const double unrl = (use_current - p.entry_px) * dir * sz * mult;
                omega::PositionSnapshot ps;
                ps.symbol = "XAUUSD"; ps.side = p.is_long ? "LONG" : "SHORT";
                ps.size = sz; ps.entry = p.entry_px; ps.current = use_current;
                ps.unrealized_pnl = unrl;
                ps.mfe = p.mfe * sz * mult;
                ps.mae = p.mae * sz * mult;
                ps.engine = engine_name;
                out.push_back(ps);
            }
            return out;
        };
    };
    g_open_positions.register_source("XauTrendFollow2h",
        _make_xau_tf_source("XauTrendFollow2h", &g_xau_tf_2h));
    g_open_positions.register_source("XauTrendFollow4h",
        _make_xau_tf_source("XauTrendFollow4h", &g_xau_tf_4h));
    g_open_positions.register_source("XauTrendFollowD1",
        _make_xau_tf_source("XauTrendFollowD1", &g_xau_tf_d1));
    // S118: H1 long-only ensemble.  Same generic lambda factory works
    // because XauTfPos1h shares the same field names as XauTfPos
    // (active, is_long, entry_px, mfe, mae, etc).
    g_open_positions.register_source("XauTrendFollow1h",
        _make_xau_tf_source("XauTrendFollow1h", &g_xau_tf_1h));

    // ── S-2026-07-07v: SpxTurtleD1 + ConnorsRSI2(NAS) dashboard sources ────────
    // Both engines persisted but had NO GUI source ([VIS-AUDIT] WARN class) →
    // their open positions never reached live_trades. That made the shipped
    // connors_mirror_x2 MirrorBook BLIND to its parent (watching a feed that
    // could never contain it), and would have done the same to the new
    // spx_turtle_mirror_x2. current px from g_last_tick_bid (straddle pattern)
    // so MirrorBook's `current<=0` guard passes and usd_per_pt can scale.
    g_open_positions.register_source("SpxTurtleD1", []() {
        std::vector<omega::PositionSnapshot> v;
        if (!g_spx_turtle_d1.has_open_position()) return v;
        const auto& p = g_spx_turtle_d1.pos_;
        double cur = p.entry;
        const auto it = g_last_tick_bid.find("US500.F");
        if (it != g_last_tick_bid.end() && it->second > 0.0) cur = it->second;
        omega::PositionSnapshot ps;
        ps.symbol = "US500.F"; ps.engine = "SpxTurtleD1"; ps.side = "LONG";
        ps.size = p.lot; ps.entry = p.entry; ps.current = cur;
        ps.tp = p.tp; ps.sl = p.sl; ps.entry_ts = p.entry_ts_ms / 1000;
        ps.unrealized_pnl = (cur - p.entry) * p.lot * g_spx_turtle_d1.p.dollars_per_pt;
        v.push_back(ps);
        return v;
    });
    g_open_positions.register_source("ConnorsRSI2", []() {
        std::vector<omega::PositionSnapshot> v;
        if (!g_connors_nas.pos.active) return v;
        const auto& p = g_connors_nas.pos;
        double cur = p.entry_px;
        const auto it = g_last_tick_bid.find("NAS100");
        if (it != g_last_tick_bid.end() && it->second > 0.0) cur = it->second;
        omega::PositionSnapshot ps;
        ps.symbol = "NAS100"; ps.engine = "ConnorsRSI2"; ps.side = "LONG";
        ps.size = p.size; ps.entry = p.entry_px; ps.current = cur;
        ps.entry_ts = p.entry_ms / 1000;
        ps.unrealized_pnl = (cur - p.entry_px) * p.size;   // engine pnl = pts * size (see _close)
        v.push_back(ps);
        return v;
    });

    // 2026-06-04: LIVE-DISPLAY sources for the new index engines. wire_cross only
    // registers the PERSIST source (.dat restore); the dashboard RUNNING TRADES
    // reads register_source. Without these the engines' open positions were
    // invisible. FVGcont = bidirectional; Overnight = long-only.
    auto _nas_px = [](double entry)->double {
        const auto it = g_last_tick_bid.find("NAS100");
        return (it!=g_last_tick_bid.end() && it->second>0.0) ? it->second : entry; };
    (void)_nas_px;
    // S-2026-06-09 dedup: FvgContinuation + FvgCont10m are ALSO registered earlier
    // (the persist_save-based sources at ~L4287/4313, added 2026-06-08, which win
    // the on_tick symbol+engine dedup). These 2026-06-04 direct-field duplicates
    // double-published into snapshot_all() (a double row on /api/v1/omega/positions).
    // Removed here; the earlier persist_save sources remain the single source.

    // AdaptiveHull live-display sources (long-only; current px per symbol)
    auto _hull_src = [](const char* label, omega::AdaptiveHullEngine* e, const char* sym) {
        return [label,e,sym]() {
            std::vector<omega::PositionSnapshot> out;
            if (!e->has_open_position()) return out;
            const auto& p = e->pos; const double mult = tick_value_multiplier(std::string(sym));
            double cur=p.entry_px; const auto it=g_last_tick_bid.find(sym);
            if (it!=g_last_tick_bid.end() && it->second>0.0) cur=it->second;
            omega::PositionSnapshot ps; ps.engine=label; ps.symbol=sym; ps.side="LONG";
            ps.size=p.size; ps.entry=p.entry_px; ps.current=cur; ps.sl=p.stop_px; ps.tp=0.0;
            ps.entry_ts=p.entry_ms/1000; ps.unrealized_pnl=(cur-p.entry_px)*p.size*mult;
            out.push_back(ps); return out; }; };
    (void)_hull_src;

    // S-2026-06-03: GoldSeasonal (XAUUSD Mon+Tue long). Long-only, no TP/SL
    //   (exits on UTC day-flip) → tp=sl=0.

    // S-2026-06-03: GoldOversoldBounce (XAUUSD RSI<30 long). Long-only, ATR stop
    //   (sl set), no TP (RSI-recovery / time exit) → tp=0.

    // ── S66-followup (2026-05-13): 8 more sources (H1SwingGold,
    //    UstecTrendFollow 5m/HTF, FX BreakoutEngine x5). Mechanical extension
    //    of the S66 pattern; pos struct shapes verified case-by-case before
    //    writing. Bumps source count 26 -> 34.
    //
    //    S66-followup-2 (part L, 2026-05-14) added IndexFlow x4 (34 -> 38).
    //    S66-followup-2 cont. (2026-05-14a) added IndexMacroCrash x4 (38 -> 42).
    //    S66-followup-2 cont. (2026-05-14b part M) added C1Retuned x4
    //    (DonchianH1 + Bollinger H2/H4/H6 cells, all long-only XAUUSD,
    //    42 -> 46).
    //
    //    Engines still NOT registered (more complex pos shapes deferred to a
    //    later session): BracketEngine multi-leg (XAU + 12 FX/index instances),
    //    GoldEngineStack legs_ vector, CandleFlow, MinimalH4 portfolio wrappers
    //    (Donchian/EmaPullback/TrendRider/Tsmom/Tsmom_v2).

    // H1SwingGold (XAUUSD). H1SwingEngine.pos_ is public (struct member;
    //   H1SwingEngine itself is a `struct`). Fields: {active, is_long,
    //   entry, sl, tp1, tp2_trail_sl, tp2_trail_active, partial_done,
    //   size_full, size_remaining, mfe, h1_atr, entry_ts_ms, h1_bars_held}.
    //   Reports `size_remaining` (live size after partial TP1 takes 50%).
    //   No mae tracked. `symbol` is a public std::string on the engine.
    g_open_positions.register_source("H1SwingGold",
        []() -> std::vector<omega::PositionSnapshot> {
            std::vector<omega::PositionSnapshot> out;
            if (!g_h1_swing_gold.has_open_position()) return out;
            const auto& p = g_h1_swing_gold.pos_;
            const std::string sym = g_h1_swing_gold.symbol;
            const double mult = tick_value_multiplier(sym);
            double current = p.entry;
            const auto it = g_last_tick_bid.find(sym);
            if (it != g_last_tick_bid.end() && it->second > 0.0) current = it->second;
            const double dir  = p.is_long ? 1.0 : -1.0;
            const double sz   = p.size_remaining;
            const double unrl = (current - p.entry) * dir * sz * mult;
            omega::PositionSnapshot ps;
            ps.symbol = sym; ps.side = p.is_long ? "LONG" : "SHORT";
            ps.size = sz; ps.entry = p.entry; ps.current = current;
            ps.unrealized_pnl = unrl;
            ps.mfe = p.mfe * sz * mult;
            ps.mae = 0.0;  // H1SwingEngine doesn't track mae
            ps.engine = "H1SwingGold";
            out.push_back(ps);
            return out;
        });

    // UstecTrendFollow x 2 (5m + HTF). Both are multi-cell engines like the
    //   XauTf family, but their pos struct uses `mfe_pts` / `mae_pts` (with
    //   `_pts` suffix) instead of plain `mfe` / `mae` -- so the XauTf
    //   generic lambda above will NOT compile for them. Separate factory
    //   here; identical otherwise. Both engines trade USTEC.F (hardcoded).
    auto _make_ustec_tf_source = [](const char* engine_name, auto* eng) {
        return [engine_name, eng]() -> std::vector<omega::PositionSnapshot> {
            std::vector<omega::PositionSnapshot> out;
            const double mult = tick_value_multiplier(std::string("USTEC.F"));
            double current_mid = 0.0;
            const auto it = g_last_tick_bid.find("USTEC.F");
            if (it != g_last_tick_bid.end() && it->second > 0.0) current_mid = it->second;
            const double sz = eng->lot;
            for (size_t i = 0; i < eng->pos.size(); ++i) {
                const auto& p = eng->pos[i];
                if (!p.active) continue;
                const double use_current = (current_mid > 0.0) ? current_mid : p.entry_px;
                const double dir  = p.is_long ? 1.0 : -1.0;
                const double unrl = (use_current - p.entry_px) * dir * sz * mult;
                omega::PositionSnapshot ps;
                ps.symbol = "USTEC.F"; ps.side = p.is_long ? "LONG" : "SHORT";
                ps.size = sz; ps.entry = p.entry_px; ps.current = use_current;
                ps.unrealized_pnl = unrl;
                ps.mfe = p.mfe_pts * sz * mult;
                ps.mae = p.mae_pts * sz * mult;
                ps.engine = engine_name;
                out.push_back(ps);
            }
            return out;
        };
    };
    (void)_make_ustec_tf_source;

    // S-2026-06-29 FULL FX REMOVAL ("no FX"): the 5 FX BreakoutEngine GUI
    //   position-sources (BreakoutEURUSD/GBPUSD/AUDUSD/NZDUSD/USDJPY) are
    //   REMOVED. The engines were dead behind the S99 kill-switch and never
    //   traded; their display registrations are dropped with the FX cohort.
    //   Their VIS-AUDIT tags are removed below.

    // ====================================================================
    // S-2026-06-09 visibility fix: register engines that were active-but-invisible
    //   These 23 engines were enabled + fed ticks + persisted (wire_cross/
    //   wire_multicell/wire_livepos in PositionPersistence.hpp) but had NO
    //   dashboard source, so their open positions held silently. Field names
    //   were read from each engine header. current px = g_last_tick_bid[sym]
    //   fallback to entry. unrealized = signed move * size * tick_value_mult.
    // ====================================================================
    {
        // small helper: current mid for a symbol (fallback to entry)
        auto _cur_px = [](const char* sym, double entry) -> double {
            const auto it = g_last_tick_bid.find(sym);
            return (it != g_last_tick_bid.end() && it->second > 0.0) ? it->second : entry;
        };

        // --- g_xau_tf_m15 : reuse the XauTf factory (XauTfPos1h field shape) ---

        // --- g_overnight_spx : OvernightDriftEngine, US500.F, long-only ---
        //   pos: {active, entry_px, size, entry_ms}; has_open_position(); no sl/tp.

        // S-2026-06-29 ("no FX"): FxTurtleH4 / TrendLineBreakEngine / EurGbpPairs
        //   GUI position-source lambdas REMOVED. All FX engines are non-trading
        //   (FxTurtle tombstoned 2026-06-16; TrendLineBreak + EurGbpPairs dispatch
        //   neutralized in tick_fx.hpp), so no FX engine holds positions to display.
        //   Their VIS-AUDIT tags are dropped below to stop phantom boot WARNs.

        // --- g_minimal_h4_ger40 : MinimalH4GER40Breakout, GER40 ---
        //   pos_: {active, is_long, entry, sl, tp, size, entry_ts_ms}; has_open_position().

        // --- g_ger40_london_brk : Ger40LondonBreakoutEngine, GER40 (short-only) ---
        //   pos: {active, is_long, entry_px, tp_px, sl_px, size, entry_ms, mfe, mae}.

        // --- g_gold_volbrk_m30 : GoldVolBreakoutM30Engine, XAUUSD (long-only) ---
        //   pos: {active, entry_px, sl_px, entry_ts_ms}; size from eng.lot; any_open(); no tp.
        g_open_positions.register_source("GoldVolBreakoutM30", [_cur_px]() {
            std::vector<omega::PositionSnapshot> out;
            if (!g_gold_volbrk_m30.any_open()) return out;
            const auto& p = g_gold_volbrk_m30.pos; const double mult = tick_value_multiplier(std::string("XAUUSD"));
            double cur=_cur_px("XAUUSD", p.entry_px); const double sz=g_gold_volbrk_m30.lot;
            omega::PositionSnapshot ps; ps.engine="GoldVolBreakoutM30"; ps.symbol="XAUUSD"; ps.side="LONG";
            ps.size=sz; ps.entry=p.entry_px; ps.current=cur; ps.sl=p.sl_px; ps.tp=0.0;
            ps.entry_ts=p.entry_ts_ms/1000; ps.unrealized_pnl=(cur-p.entry_px)*sz*mult;
            out.push_back(ps); return out; });

        // --- g_mgc_volbrk : GoldVolBreakoutM30Engine, MGC instance (S-2026-07-11
        //   PHASE 1b): held real (paper-book) MGC legs with NO dashboard source
        //   and NO persistence. Same shape as the spot source; symbol MGC
        //   ($10/pt/contract via tick_value_multiplier). Persist wire_cross tag
        //   matches this label ("MgcVolBreakoutM30", persistence audit contract).
        g_open_positions.register_source("MgcVolBreakoutM30", [_cur_px]() {
            std::vector<omega::PositionSnapshot> out;
            if (!g_mgc_volbrk.any_open()) return out;
            const auto& p = g_mgc_volbrk.pos; const double mult = tick_value_multiplier(std::string("MGC"));
            double cur=_cur_px("MGC", p.entry_px); const double sz=g_mgc_volbrk.lot;
            omega::PositionSnapshot ps; ps.engine="MgcVolBreakoutM30"; ps.symbol="MGC"; ps.side="LONG";
            ps.size=sz; ps.entry=p.entry_px; ps.current=cur; ps.sl=p.sl_px; ps.tp=0.0;
            ps.entry_ts=p.entry_ts_ms/1000; ps.unrealized_pnl=(cur-p.entry_px)*sz*mult;
            out.push_back(ps); return out; });

        // --- IndexIntradayDriftEngine x3 (US500.F, UK100, DJ30.F), long-only ---
        //   pos: {active, is_long(=true), entry, sl, size, entry_ts(sec)}; has_open_position(); no tp.
        auto _idd_src = [_cur_px](const char* label, omega::IndexIntradayDriftEngine* e, const char* sym) {
            return [label,e,sym,_cur_px]() {
                std::vector<omega::PositionSnapshot> out;
                if (!e->has_open_position()) return out;
                const auto& p = e->pos; const double mult = tick_value_multiplier(std::string(sym));
                double cur=_cur_px(sym, p.entry);
                omega::PositionSnapshot ps; ps.engine=label; ps.symbol=sym; ps.side=p.is_long?"LONG":"SHORT";
                ps.size=p.size; ps.entry=p.entry; ps.current=cur; ps.sl=p.sl; ps.tp=0.0;
                ps.entry_ts=p.entry_ts; ps.unrealized_pnl=(cur-p.entry)*(p.is_long?1.0:-1.0)*p.size*mult;
                out.push_back(ps); return out; }; };
        (void)_idd_src;

        // --- g_orb_estx50_v2 : OrbBreakoutEngine, ESTX50 ---
        //   pos_: {active, side(+1/-1 int), entry, sl, tp, lot, entry_ts_ms}; has_open_position().

        // --- g_us30_ensemble : Us30EnsembleEngine, DJ30.F (multi-cell array pos[]) ---
        //   pos[]: {active, is_long, entry_px, sl_px, tp_px, entry_ts_ms, mfe_pts}; size from eng.lot.
        g_open_positions.register_source("Us30Ensemble", [_cur_px]() {
            std::vector<omega::PositionSnapshot> out;
            return out; });

        // --- g_xau_breakbounce : BreakBounceEngine, XAUUSD ---
        //   pos: {active, is_long, entry_px, stop_px, tp_px?, size, entry_ms}; has_open_position().

        // --- XAU D1 engines x3 (DojiRej, OutsideBar, Turtle) ---
        //   pos_: {active, [is_long], entry, sl, tp, entry_ts_ms}; lot from p.lot; has_open_position().
        //   DojiRej + OutsideBar fire long only at entry; Turtle is long-only.

        // --- SessionMomentumEngine x2 (XAU NYpm + overnight), long-only ---
        //   pos: {active, entry_px, sl_px(0=none), entry_ts_ms}; size from eng.lot; any_open(); no tp.
        auto _sess_src = [_cur_px](const char* label, omega::SessionMomentumEngine* e) {
            return [label,e,_cur_px]() {
                std::vector<omega::PositionSnapshot> out;
                if (!e->any_open()) return out;
                const auto& p = e->pos; const double mult = tick_value_multiplier(std::string("XAUUSD"));
                double cur=_cur_px("XAUUSD", p.entry_px); const double sz=e->lot;
                omega::PositionSnapshot ps; ps.engine=label; ps.symbol="XAUUSD"; ps.side="LONG";
                ps.size=sz; ps.entry=p.entry_px; ps.current=cur; ps.sl=p.sl_px; ps.tp=0.0;
                ps.entry_ts=p.entry_ts_ms/1000; ps.unrealized_pnl=(cur-p.entry_px)*sz*mult;
                out.push_back(ps); return out; }; };
        g_open_positions.register_source("XauSessNYpm",     _sess_src("XauSessNYpm",     &g_xau_sess_nypm));

        // --- multi-cell portfolios: Donchian + EmaPullback (XAUUSD cells) ---
        //   DonchianCell / EpbCell public fields: {pos_active_, pos_entry_, pos_sl_,
        //   pos_tp_, pos_size_, pos_entry_ms_, direction(+1/-1), symbol, cell_id}.
        //   One snapshot per active cell; per-cell tag = "<base>#<cell_id>".
        auto _emit_donch_cell = [_cur_px](std::vector<omega::PositionSnapshot>& out,
                                          const char* base, const auto& c) {
            if (!c.pos_active_) return;
            const double mult = tick_value_multiplier(c.symbol);
            double cur=_cur_px(c.symbol.c_str(), c.pos_entry_); const double dir=(c.direction>0)?1.0:-1.0;
            omega::PositionSnapshot ps; ps.engine=std::string(base)+"#"+c.cell_id; ps.symbol=c.symbol;
            ps.side=(c.direction>0)?"LONG":"SHORT"; ps.size=c.pos_size_; ps.entry=c.pos_entry_; ps.current=cur;
            ps.sl=c.pos_sl_; ps.tp=c.pos_tp_; ps.entry_ts=c.pos_entry_ms_/1000;
            ps.unrealized_pnl=(cur-c.pos_entry_)*dir*c.pos_size_*mult;
            out.push_back(ps); };
        g_open_positions.register_source("DonchianPortfolio", [_emit_donch_cell]() {
            std::vector<omega::PositionSnapshot> out;
            return out; });
        g_open_positions.register_source("EmaPullbackPortfolio", [_emit_donch_cell]() {
            std::vector<omega::PositionSnapshot> out;
            return out; });

        // --- g_tsmom_v2 : TsmomPortfolioV2 = CellPortfolio<TsmomStrategy> ---
        //   public cells_ vector; each Cell (CellBase) has positions_ (vector<Position>),
        //   direction(+1/-1), symbol, cell_id. Position: {entry, sl, tp, size, entry_ms,
        //   mfe, mae}. One snapshot per open position across all cells.
    } // end S-2026-06-09 visibility fix block

    // S-2026-06-09 round-2 visibility: register the 10 persisted-but-invisible
    //   engines the guardrail flagged (FxTurtleH4 AUD/NZD/JPY, IndexSession x5,
    //   ConnorsRSI2, C1Retuned). Reuse each engine's own persist_save(_all) -- the
    //   proven snapshotter -- so no per-engine field access. -> [VIS-AUDIT] missing=0.
    {
        auto _ps_src = [](const char* tag, const char* sym, auto* eng) {
            g_open_positions.register_source(tag, [tag, sym, eng]() {
                std::vector<omega::PositionSnapshot> v; omega::PositionSnapshot ps;
                if (eng->persist_save(tag, sym, ps)) v.push_back(ps);
                return v; });
        };
        _ps_src("IndexSession_SP",     "US500.F", &g_idxsess_sp);
        _ps_src("IndexSession_NAS",    "NAS100",  &g_idxsess_nas);
        _ps_src("IndexSession_GER40",  "GER40",   &g_idxsess_ger40);
        _ps_src("IndexSession_UK100",  "UK100",   &g_idxsess_uk100);
        _ps_src("IndexSession_ESTX50", "ESTX50",  &g_idxsess_estx50);
        // S-2026-06-11 DEDUP: the "C1Retuned" persist_save_all GUI source is
        // REMOVED. Its cells are already published live (with current px) by the
        // per-cell sources C1RetunedDonchianH1/BollingerH2/H4/H6 (~L6297). This
        // source double-displayed every open cell under "C1Retuned#<cellkey>"
        // with current=0 -> the GUI rendered a phantom row with an SL distance
        // of -<sl> (operator report 2026-06-11). Persistence is unaffected —
        // wire_multicell in PositionPersistence.hpp owns save/restore.
    }

    // ====================================================================
    // S-2026-06-09 visibility GUARDRAIL (WARN-only): cross-check that every
    //   persist tag in PositionPersistence.hpp has a matching dashboard source.
    //   Tag list is read directly from that file's wire_cross/wire_multicell/
    //   wire_livepos calls. Never aborts — prints warnings + a summary.
    // ====================================================================
    {
        static const char* const kPersistTags[] = {
            // wire_livepos (7)
            "MidScalperGold", "MicroScalperGold", "EurusdLondonOpen", "UsdjpyAsianOpen",
            "GbpusdLondonOpen", "AudusdSydneyOpen", "NzdusdAsianOpen",
            // wire_cross CrossPosition/IdxOpenPosition (14)
            "VWAPReversionSP", "VWAPReversionNQ", "VWAPReversionGER40", "VWAPReversionEURUSD",
            "TrendPullbackGold", "TrendPullbackNQ", "IndexFlowSP", "IndexFlowNQ", "IndexFlowNAS",
            "IndexFlowUS30", "IndexMacroCrashSP", "IndexMacroCrashNQ", "IndexMacroCrashNAS",
            "IndexMacroCrashUS30",
            // batch 4: straddles + Ger40Keltner
            "XauStraddleM30", "XauStraddleM15", "IdxStraddleGER40_M30", "IdxStraddleGER40_M15",
            "IdxStraddleNAS100_M15", "IdxStraddleNAS100_M30", "IdxStraddleUK100_M30",
            "IdxStraddleUK100_M240", "Ger40KeltnerH1",
            // batch 5: single-pos gold/index
            "XauusdFvg", "PDHLReversion", "RSIReversal", "MinimalH4Gold", "MinimalH4US30",
            "MinimalH4GER40", "XauThreeBar30m", "EMACrossGold", "Ger40TurtleH4", "GoldSeasonal",
            "GoldOversoldBounce", "H1SwingGold", "H4RegimeGold",
            // batch 5: multi-cell ensembles (base tags)
            "XauTrendFollow1h", "XauTrendFollow2h", "XauTrendFollow4h", "XauTrendFollowD1",
            // "C1Retuned" intentionally absent (S-2026-06-11): its cells are
            // displayed by the per-cell C1Retuned* sources; the base-tag GUI
            // source was a double-display (see dedup note above).
            "UstecTrendFollow5m", "UstecTrendFollowHtf",
            // batch 6: NBM, pyramided (FX Breakout/turtle/scalp tags removed -- "no FX")
            // S-2026-06-29 FULL FX REMOVAL: BreakoutEURUSD/GBPUSD/AUDUSD/NZDUSD/USDJPY
            //   tags REMOVED with their register_source (engines dead, never traded).
            "NoiseBandMomentumGoldLdn", "GoldScalpPyramid", "GoldRegimeDaily", "MacroCrash",
            // S-2026-06-29 ("no FX"): FxTurtleH4_* x5 tags REMOVED -- cohort tombstoned
            //   2026-06-16, no register_source -> phantom [VIS-AUDIT] WARNs.
            // S-2026-06-29: FxScalpPyramid x5 tags REMOVED -- engine retired (no
            //   g_fx_scalp_* globals, no register_source) -> phantom [VIS-AUDIT] WARNs.
            // IndexSession + S118 batch
            "IndexSession_SP", "IndexSession_NAS", "FvgContinuation", "FvgCont10m", "OvernightDrift",
            "ConnorsRSI2", "ConnorsRSI2_GER",
            "ConnorsIBS_NAS", "ConnorsStreak_NAS", "ConnorsDouble_NAS", "ConnorsStreak_SPX", "ConnorsDouble_SPX",
            "AdaptiveHullXAU", "AdaptiveHullGER", "SupertrendGold",
            "IndexSession_GER40", "IndexSession_UK100", "IndexSession_ESTX50",
            // S-2026-06-09 visibility fix: never-persisted strays now registered.
            // Asserted here so a future drop of any registration trips [VIS-AUDIT].
            "XauTrendFollowM15", "OvernightDriftSPX",
            // S-2026-06-29 ("no FX"): EurGbpPairs tag REMOVED -- dispatch neutralized,
            //   no register_source -> phantom [VIS-AUDIT] WARN.
            // S-2026-06-29: FxCrossRevEURGBP tag REMOVED -- source un-registered with
            //   the FX shadow-book removal -> would trip a phantom [VIS-AUDIT] WARN.
            "Ger40LondonBrk", "GoldVolBreakoutM30", "OrbEstx50",
            // S-2026-06-26s: IndexIntradayDrift_SP/UK100/US30 phantom tags REMOVED
            // (fleet-sweep KILL, held=True). The engine is killed + un-instantiated
            // (no g_idd_* globals, no register_source) -> these tags tripped a false
            // [VIS-AUDIT] WARN every boot. Drop them with the engine.
            "Us30Ensemble", "BreakBounce", "XauDojiRejD1", "XauOutsideBarD1",
            "XauTurtleD1", "XauSessNYpm", "XauSessOvernight",
            "DonchianPortfolio", "EmaPullbackPortfolio", "TsmomPortfolioV2",
            // S-2026-06-29 ("no FX"): TrendLineBreakGBP/JPY tags REMOVED -- dispatch
            //   neutralized in tick_fx.hpp, no register_source -> phantom [VIS-AUDIT] WARNs.
        };
        const std::vector<std::string> registered = g_open_positions.source_labels();
        auto _is_registered = [&registered](const char* tag) -> bool {
            for (const auto& l : registered) if (l == tag) return true;
            return false;
        };
        int n_tags = 0, missing = 0;
        for (const char* tag : kPersistTags) {
            ++n_tags;
            if (!_is_registered(tag)) {
                std::cout << "[VIS-AUDIT] WARN: '" << tag
                          << "' persists positions but has NO dashboard source\n";
                ++missing;
            }
        }
        std::cout << "[VIS-AUDIT] persist-tags=" << n_tags
                  << " registered-sources=" << registered.size()
                  << " missing=" << missing << "\n";
        std::cout.flush();
    }

    // 2026-05-13 S66-followup: HybridGold removed from prose listing (engine
    //   was culled in S11 P3a+P3b, see line ~2683); count 34 then matched the
    //   actual count of register_source() calls in this block.
    // 2026-05-14 S66-followup-2 (part L): IndexFlow x4 added (SP/NQ/NAS/US30),
    //   count 34 -> 38.
    // 2026-05-14a S66-followup-2 cont.: IndexMacroCrash x4 added
    //   (SP/NQ/NAS/US30), count 38 -> 42.
    // 2026-05-14b S66-followup-2 cont. (part M): C1Retuned x4 added
    //   (DonchianH1/BollingerH2/BollingerH4/BollingerH6 — long-only XAU
    //   cells of the C1RetunedPortfolio wrapper), count 42 -> 46.
    // S-2026-06-09: dynamic count (was a hand-maintained "46 sources" prose list
    // that went stale every time a source was added). The [VIS-AUDIT] line below
    // prints the authoritative coverage check.
    std::cout << "[OmegaApi] g_open_positions sources registered ("
              << g_open_positions.source_labels().size() << " sources)\n";
    std::cout.flush();

    // ── Step 3: equity anchor for /api/v1/omega/equity ────────────────────
    // OmegaApiServer's equity-walk default of 10000.0 matches the schema
    // default in include/omega_types.hpp; here we override it with the live
    // config so the absolute equity values track the user's account.
    omega::set_equity_anchor(g_cfg.account_equity);
    std::cout << "[OmegaApi] equity anchor set to "
              << g_cfg.account_equity << "\n";
    std::cout.flush();

    // ── Step 4: GoldUltimateStrategy activation ─────────────────────────────
    // S90: v12 OOS-validated edge filters.
    // Backtest evidence (26 months, 154M ticks XAUUSD):
    //   PF=1.36, WR=41.8%, Sharpe=8.30 on 311 trades
    //   BULL PF=1.45, BEAR PF=1.29
    //   OOS PF=1.39 (265 trades), 117% PF retention
    // Edge hours: 01,05,23 UTC (Asian/early-London session)
    // ATR floor: 2.5 (low-vol band PF=0.80 removed)
    // Defaults baked into class — no overrides needed here.
    // shadow_mode semantics: the strategy is a GATE overlay, not an engine.
    // When enabled, it filters entries via evaluate_entry() for any engine
    // that calls gold_ultimate_gate(). Engines not wired to the gate are
    // unaffected. The gate adds HOUR_NOT_IN_EDGE_SET and ATR_BELOW_FLOOR
    // rejections before regime detection.
    omega::gold_ultimate::gold_ultimate_activate();

    // ── Auto-demote gate enforcement ────────────────────────────────────────
    // Load persistent per-engine lifetime stats and disable any STANDALONE
    // engine that has lost over a fair sample (n>=30, net<0, WR<35%). Runs LAST
    // so every engine's enabled flag is already set. Below 30 trades the gate
    // is a no-op -- it never cuts a validated engine in a short drawdown. Stats
    // are fed by g_engine_gate.record_close() from the universal close path.
    // Name keys match each engine's TradeRecord.engine string (prefix-aggregated
    // for multi-cell engines, e.g. "Ger40KeltnerH1" covers the _EMA20..._S41 tag).
    g_engine_gate.set_path("logs/engine_gate_stats.csv");
    g_engine_gate.load();
    g_engine_gate.evaluate_log();
    {
        struct GateTarget { const char* name; bool* flag; };
        const GateTarget kGateTargets[] = {
            { "IdxStraddleGER40_M30",   &g_idx_straddle_ger40_m30.enabled  },
            { "IdxStraddleGER40_M15",   &g_idx_straddle_ger40_m15.enabled  },
            { "IdxStraddleNAS100_M15",  &g_idx_straddle_nas_m15.enabled    },
            { "IdxStraddleNAS100_M30",  &g_idx_straddle_nas_m30.enabled    },
            { "IdxStraddleUK100_M30",   &g_idx_straddle_uk100_m30.enabled  },
            { "IdxStraddleUK100_M240",  &g_idx_straddle_uk100_m240.enabled },
        };
        for (const auto& t : kGateTargets) {
            if (g_engine_gate.is_demoted(t.name) && *t.flag) {
                *t.flag = false;
                std::printf("[ENGINE-GATE] AUTO-DISABLED %s "
                            "(>=20 trades, net-negative)\n", t.name);
            }
        }

        // GoldTrendMimicLadder: ARM now -- every trend engine has finished warm-seeding, so a
        // historical open replayed during seed can no longer spawn phantom mimic legs. Live trend
        // opens (post-boot) spawn the independent mimic legs. deploy-forward ($0 until first clip).
        omega::gold_trend_mimic().arm();
        std::printf("[OMEGA-INIT] GoldTrendMimicLadder ARMED (post-seed) -- live trend opens now spawn mimic legs\n");
        std::fflush(stdout);
        std::fflush(stdout);
    }

    // ── Universal catastrophe-net LIVE-readiness flag ──────────────────────────
    // Surfaces the one remaining gap AT the live flip: the guard can DETECT+LOG any
    // position past 3x dollar-stop across all engines, but auto-FLATTEN needs per-
    // engine register_flatten() hooks. In SHADOW this is silent (hooks not needed);
    // the moment mode=LIVE with zero hooks it prints a loud boot WARNING so we wire
    // them before sizing up. See CatastrophicGuard.hpp.
    g_catastrophic_guard.per_trade_usd = g_cfg.dollar_stop_usd;
    g_catastrophic_guard.warn_if_live_unhooked(g_cfg.mode == "LIVE");

    // ── INDEPENDENT profit-giveback clipper (S-2026-06-29, operator-mandated) ───
    // Trade engines ride wide (keep the trend edge). THIS guard, fully independent of them,
    // closes a position when it gives back >= trail of its peak (or stalls) -> locks the gain
    // WITHOUT touching the engine. Twin of the catastrophe net (AccountingGuard) but on the PROFIT
    // side. Uses g_open_positions.mfe + close_matching() (proven). Params = the operator's validated
    // companion settings (20% giveback / 1h stall). Closes via registered closers; an engine without
    // one logs [GIVEBACK] NO-CLOSER (same gap the catastrophe net surfaces -> register closers).
    // DISABLED 2026-06-29 (operator design correction): routine giveback/stall must NOT close the
    // REAL position (that touches the engine + can clip its fat-tail edge). The COMPANION engine
    // (stall_accountant.py) handles giveback/stall/fail on its OWN mirrored book -> real engine rides
    // wide, edge intact. This guard's REAL-position close is RESERVED for the one sanctioned case:
    // a CONFIRMED REVERSAL safe-stop (trigger to be built + backtested). Until then: OFF.
    g_giveback_guard.enabled   = false;
    g_giveback_guard.gate_usd  = 40.0;
    g_giveback_guard.trail     = 0.20;
    g_giveback_guard.stall_sec = 3600.0;
    printf("[OMEGA-INIT] GivebackGuard ARMED: gate=$%.0f trail=%.0f%% stall=%.0fmin (independent clipper, closes via registry)\n",
           g_giveback_guard.gate_usd, g_giveback_guard.trail*100.0, g_giveback_guard.stall_sec/60.0);
}

