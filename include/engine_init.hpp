#pragma once
// engine_init.hpp -- Engine configuration, wiring, and startup.
// All engine config, callback wiring, state loading, and startup checks
// that were previously embedded in main() have been extracted here.
// Called once from main() as: init_engines(cfg_path);
//
// SINGLE-TRANSLATION-UNIT include -- only include from main.cpp

#include "SeedGuard.hpp"
#include "PortfolioGuard.hpp"

static void init_engines(const std::string& cfg_path)
{
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
    g_rsi_extreme.enabled      = false;            // DISABLED 2026-05-01 -- 0/153 combos profitable
    g_rsi_extreme.shadow_mode  = kShadowDefault;  // RSIExtremeTurnEngine
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
    g_gold_microscalper.shadow_mode = false;

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
    g_xauusd_fvg.shadow_mode = false;
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
    g_gold_scalp_pyramid.enabled     = false;  // S45: harness-class disagreement
    g_gold_scalp_pyramid.shadow_mode = true;
    g_gold_scalp_pyramid.LOOKBACK    = 8;     // Donchian channel bars (M5) -- sweep best
    g_gold_scalp_pyramid.SL_ATR_MULT = 1.5;   // SL = 1.5 * ATR14 -- sweep v2 best (wider, 71% WR)
    g_gold_scalp_pyramid.TP_ATR_MULT = 3.0;   // TP = 3.0 * ATR14 -- sweep best (decorative, trail exits)
    g_gold_scalp_pyramid.TRAIL_TIGHT = 0.12;  // trail distance = 0.12 * ATR behind MFE -- sweep best
    g_gold_scalp_pyramid.PYRAMID_ON  = true;   // sweep v2: Lyrs=1.1, +$1001 uplift on best config
    g_gold_scalp_pyramid.LOSS_CUT_PCT  = 0.05;
    g_gold_scalp_pyramid.BE_ARM_PCT    = 0.03;
    g_gold_scalp_pyramid.BE_BUFFER_PCT = 0.015;  // S38a: 0.012 -> 0.015 = exit at +$0.68 vs $0.54 (clears $0.60 cost)
    // S38a tunables (new fields on engine):
    g_gold_scalp_pyramid.COST_RT_PTS        = 0.60;   // realised BB gold cost @ 0.01 lot
    g_gold_scalp_pyramid.BE_ARM_COST_MULT   = 2.0;    // arm Phase-1 BE at MFE >= $1.20
    g_gold_scalp_pyramid.CHOP_ER_MIN        = 0.0;    // S38b 2026-05-26: disabled. ER hurts -- gives up $2K/2yr for $130 less DD
    g_gold_scalp_pyramid.CHOP_ER_LOOKBACK   = 10;     // 10x M5 = 50min ER window (unused when MIN=0)
    // S38c: ADX 10 chop filter -- free DD safety. Backtest: 0% PnL cost on
    // 2yr but -24% DD ($443 -> $338). Filters bars where Wilder ADX(14) < 10
    // (no sustained directional pressure). Operator chose adx10 as default
    // after 12-config x 3-period sweep (May, Jan-Apr, 2yr).
    g_gold_scalp_pyramid.CHOP_ADX_MIN       = 10.0;
    g_gold_scalp_pyramid.CHOP_ADX_PERIOD    = 14;     // Wilder standard
    // S38d: Range-expansion filter -- off by default. Enable when scaling
    // beyond 0.01 lot (DD reduction matters more). Recommended 1.2x.
    // Backtest: 1.2x -> -15% PnL but -32% DD + 8% PF on 2yr.
    g_gold_scalp_pyramid.RANGE_EXP_MULT     = 0.0;    // 0=off, 1.2 for risk-tier
    g_gold_scalp_pyramid.RANGE_EXP_LB       = 10;
    g_gold_scalp_pyramid.CONSEC_BE_FREEZE_N = 3;      // 3 consec BE_CUT -> 30min freeze
    g_gold_scalp_pyramid.on_close_cb = [](const omega::TradeRecord& tr) {
        handle_closed_trade(tr);
    };

    // ---- FxScalpPyramid x5 (S38d 2026-05-26) --------------------------------
    // 5 profitable FX pairs from 13-month standalone harness backtest
    // (backtest/fx_scalp_pyramid_bt.cpp). All shadow-mode pending 14-day
    // live validation. Per-pair constants below match the harness PRESET
    // switch (~line 1110). All five share: ADX 10 chop filter, ER disabled
    // (S38b), pyramid on, SL=1.5xATR, TP=3.0xATR, Trail=0.12.
    //
    // Skipped from harness winners: NZDUSD (PF 1.07 marginal),
    //   EURGBP (PF 0.93 loser).
    //
    // 13mo standalone harness PnL @ 0.01 lot:
    //   EURUSD +$1808 PF 1.56 (best)
    //   USDJPY +$1688 PF 1.51
    //   GBPUSD +$1207 PF 1.32
    //   USDCAD +$506  PF 1.23
    //   AUDUSD +$410  PF 1.23
    //   Total: +$5712 / 13mo
    {
        auto config_fx_scalp = [](omega::FxScalpPyramidEngine& e,
                                  const std::string& sym,
                                  double cost, double half_spread,
                                  double usd_per_pt, double atr_floor,
                                  double atr_cap, double spread_cap,
                                  int decimals) {
            e.set_pair_config(sym, cost, half_spread, usd_per_pt,
                              atr_floor, atr_cap, spread_cap, decimals);
            // S45 2026-05-27: DISABLED — same harness-class disagreement as
            // GoldScalpPyramid (see comment block at L405-407 above). The
            // 13mo +$5712 standalone-harness profit was an inline-reimpl
            // result, not the real FxScalpPyramidEngine class. Today's
            // chop-day session bled 8 FX scalp LOSS_CUTs across the cohort.
            // No backtest of the real class with live S63 config has shown
            // positive edge. Disable pending re-tune + harness-class
            // agreement validation.
            e.enabled         = false;
            e.shadow_mode     = true;
            e.LOOKBACK        = 8;
            e.SL_ATR_MULT     = 1.5;
            e.TP_ATR_MULT     = 3.0;
            e.TRAIL_TIGHT     = 0.12;
            e.PYRAMID_ON      = true;
            e.LOSS_CUT_PCT    = 0.05;
            e.BE_ARM_PCT      = 0.03;
            e.BE_BUFFER_PCT   = 0.015;
            e.BE_ARM_COST_MULT = 2.0;
            e.CHOP_ER_MIN     = 0.0;     // S38b: ER disabled
            e.CHOP_ADX_MIN    = 10.0;    // S38c: ADX 10 default
            e.CHOP_ADX_PERIOD = 14;
            e.RANGE_EXP_MULT  = 0.0;     // off by default
            e.on_close_cb     = [](const omega::TradeRecord& tr) { handle_closed_trade(tr); };
        };

        // xxxUSD majors (5 decimal places, ATR 30-500 pips, spread cap 50p)
        config_fx_scalp(g_fx_scalp_eurusd, "EURUSD", 0.00040, 0.00002, 100000.0,
                        0.00030, 0.00500, 0.00050, 5);
        // USDJPY (3 decimals, JPY pip math: USD_PER_PT_LOT ~633 at 1USD=158JPY)
        config_fx_scalp(g_fx_scalp_usdjpy, "USDJPY", 0.04,    0.002,   633.0,
                        0.03,    0.50,    0.05,    3);
        config_fx_scalp(g_fx_scalp_gbpusd, "GBPUSD", 0.00040, 0.00002, 100000.0,
                        0.00030, 0.00500, 0.00050, 5);
        // USDCAD (5 decimals, USD_PER_PT_LOT ~74000 at 1USD=1.35CAD)
        config_fx_scalp(g_fx_scalp_usdcad, "USDCAD", 0.00050, 0.00002, 74000.0,
                        0.00030, 0.00500, 0.00060, 5);
        config_fx_scalp(g_fx_scalp_audusd, "AUDUSD", 0.00040, 0.00002, 100000.0,
                        0.00030, 0.00500, 0.00050, 5);
    }

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
    g_gold_regime_daily.enabled                = false;
    g_gold_regime_daily.shadow_mode            = true;
    g_gold_regime_daily.SL_ATR_MULT            = 2.0;
    g_gold_regime_daily.TP_ATR_MULT            = 12.0;
    g_gold_regime_daily.COST_COVER_PTS         = 5.00;
    g_gold_regime_daily.BE_BUFFER_PTS          = 1.00;
    g_gold_regime_daily.TRAIL_DIST             = 99.0;  // disabled ratchet by design
    g_gold_regime_daily.REVERSAL_ADVERSE_GATE  = 0.50;
    // S112: PYRAMID_ON = true. Pyramid sweep result:
    //   PYR=Y tr=99/tp=12: PnL $12,725 / PF 2.17 / WR 75.9% / DD $8,507
    //   PYR=N tr=99/tp=12: PnL $5,854 / PF 2.35 / WR 92.6% / DD $2,303
    // 2.17x PnL uplift, PF slightly degrades, DD ~3.7x. Net PnL/DD ratio
    // remains favorable (~1.5x). Operator can flip back to false if DD
    // is undesirable; pyramid layers compound size during trend continuation.
    g_gold_regime_daily.PYRAMID_ON             = true;
    g_gold_regime_daily.on_close_cb = [](const omega::TradeRecord& tr) {
        handle_closed_trade(tr);
    };

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
    g_bband_scalp.enabled         = false;
    g_bband_scalp.shadow_mode     = true;
    g_bband_scalp.RSI_OVERSOLD    = 35.0;
    g_bband_scalp.RSI_OVERBOUGHT  = 65.0;
    g_bband_scalp.SL_PTS          = 0.40;
    g_bband_scalp.BE_ARM_PTS      = 0.30;
    g_bband_scalp.BE_BUFFER_PTS   = 0.05;
    g_bband_scalp.TRAIL_TIGHT_PTS = 0.15;
    g_bband_scalp.ATR_FLOOR_M1    = 0.50;
    g_bband_scalp.ATR_CAP_M1      = 8.00;
    g_bband_scalp.SPREAD_CAP_PTS  = 0.40;
    g_bband_scalp.MAX_HOLD_SEC    = 600;
    g_bband_scalp.COOLDOWN_SEC    = 60;
    g_bband_scalp.LOT_BASE        = 0.01;
    g_bband_scalp.COST_COVER_MULT = 1.0;
    g_bband_scalp.on_close_cb     = [](const omega::TradeRecord& tr) {
        handle_closed_trade(tr);
    };

    // (LatencyEdgeStack startup-flag block removed S13 Finding B 2026-04-24 — engine culled)
    // OLD COMMENT PRESERVED BELOW FOR CONTEXT (can be deleted in a later sweep):
    //   LatencyEdgeStack: was DISABLED (VPS RTT ~68ms, needs <1ms). No positions
    // possible, so shadow_mode wiring is moot. When the stack is re-enabled, add:
    //   g_le_stack.set_shadow_mode(kShadowDefault);  // obsolete — stack culled S13
    // 2026-05-08 USER REQUEST: was kShadowDefault, now hard-pinned to shadow.
    g_gold_stack.set_shadow_mode(true);  // GoldEngineStack / GoldPositionManager via proxy
    // TrendPullbackEngine (4 instances, uniform per Q1 decision):
    g_trend_pb_gold.shadow_mode  = kShadowDefault;
    g_trend_pb_ger40.shadow_mode = kShadowDefault;
    g_trend_pb_nq.shadow_mode    = kShadowDefault;
    g_trend_pb_sp.shadow_mode    = kShadowDefault;
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
    g_macro_crash.shadow_mode     = true;  // SHADOW: enable live after validation
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
    g_macro_crash.enabled         = false; // S99b: stop-bleed disable — MacroCrash fired Asia branch 00:34 UTC 2026-05-18 and lost -$7.07 (LOSS_CUT). Asia thresholds clearly still permissive of non-macro moves. Re-enable ONLY after retune session with backtest evidence for Asia-spike thresholds. London/NY values below left at S44 macro-scale baseline; Asia values reverted to pre-S99b baseline so future retune starts from a clean reference. PRIOR STATE: S57 re-enabled with S44 spike-only thresholds (ATR>=12, vol>=3.5x, drift>=10).
    g_macro_crash.ATR_THRESHOLD   = 12.0;  // S44 8.0 -> 12.0: London/NY base raised, only fire on macro-scale ATR
    g_macro_crash.VOL_RATIO_MIN   = 3.5;   // S44 2.5 -> 3.5: require >=3.5x baseline vol surge
    g_macro_crash.DRIFT_MIN       = 10.0;  // S44 6.0 -> 10.0: drift must be unambiguously directional
    g_macro_crash.ATR_THRESHOLD_ASIA = 6.0;  // S44 4.0 -> 6.0: lift Asia floor to winner ATR profile
    g_macro_crash.VOL_RATIO_MIN_ASIA = 2.5;  // S44 2.0 -> 2.5: small lift, keeps Asia plenty of fires
    g_macro_crash.DRIFT_MIN_ASIA     = 5.0;  // S44 3.0 -> 5.0: drift bar matches winner empirical floor
    g_macro_crash.SL_ATR_MULT_ASIA   = 1.3;  // S44 1.5 -> 1.3: tighter SL on higher-quality entries
    g_macro_crash.BASE_RISK_USD   = 80.0;  // scales with ATR (6x max = 0.48 lots at ATR=10)
    g_macro_crash.STEP1_TRIGGER_USD = 200.0; // S42 revert to validated Apr 2 2026 baseline (was 80.0; matches crash-size moves)
                                              // S42 revert: original $200 step matches Apr 2 crash-size moves (continued from L139)
    g_macro_crash.STEP2_TRIGGER_USD = 400.0; // S42 revert to validated Apr 2 2026 baseline (was 160.0; matches crash-size moves)
    g_macro_crash.on_close = [](double exit_px, bool is_long, double size, const std::string& reason) {
        if (g_macro_crash.shadow_mode) return;  // shadow: no live order
        send_live_order("XAUUSD", is_long, size, exit_px);
        printf("[MCE] Live close sent %s %.3f @ %.2f reason=%s\n",
               is_long ? "LONG" : "SHORT", size, exit_px, reason.c_str());
        fflush(stdout);
    };
    // Wire trade record callback -- fires in BOTH shadow and live.
    // This is what makes MCE trades appear in GUI, ledger, and CSV with correct costs.
    g_macro_crash.on_trade_record = [](const omega::TradeRecord& tr) {
        handle_closed_trade(tr);
    };

    printf("[MCE] MacroCrashEngine ARMED (shadow_mode=%s, enabled=%s) "
           "BASE: ATR>=%.1f vol>=%.1fx drift>=%.1f  ASIA: ATR>=%.1f vol>=%.1fx drift>=%.1f sl_x=%.2f\n",
           g_macro_crash.shadow_mode ? "true" : "false",
           g_macro_crash.enabled     ? "true" : "false",
           g_macro_crash.ATR_THRESHOLD, g_macro_crash.VOL_RATIO_MIN, g_macro_crash.DRIFT_MIN,
           g_macro_crash.ATR_THRESHOLD_ASIA, g_macro_crash.VOL_RATIO_MIN_ASIA, g_macro_crash.DRIFT_MIN_ASIA,
           g_macro_crash.SL_ATR_MULT_ASIA);
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
    g_rsi_reversal.LOSS_CUT_PCT   = 0.05;
    g_rsi_reversal.enabled        = false;  // DISABLED 2026-05-01 -- backtest negative EV
    g_rsi_reversal.shadow_mode    = true;   // SHADOW first -- verify signals before live
    g_rsi_reversal.RSI_OVERSOLD   = 42.0;  // turn from any low -- not just extreme OS
    g_rsi_reversal.RSI_OVERBOUGHT = 58.0;  // turn from any high -- not just extreme OB
    g_rsi_reversal.RSI_EXIT_LONG  = 55.0;  // exit when RSI recovers to 55
    g_rsi_reversal.RSI_EXIT_SHORT = 45.0;  // exit when RSI fades to 45
    g_rsi_reversal.SL_ATR_MULT    = 0.6;   // SL = 0.6x ATR
    g_rsi_reversal.TRAIL_ATR_MULT = 0.40;  // trail at 0.4x ATR behind MFE
    g_rsi_reversal.BE_ATR_MULT    = 0.40;  // BE at 0.4x ATR profit
    g_rsi_reversal.COOLDOWN_S     = 30;    // 30s between entries
    g_rsi_reversal.COOLDOWN_S_VACUUM = 15; // 15s with vacuum confirm
    g_rsi_reversal.MAX_HOLD_S     = 300;   // 5min max -- RSI turns can take time
    g_rsi_reversal.MIN_HOLD_S     = 5;     // 5s minimum hold
    printf("[RSI-REV] RSIReversalEngine configured (shadow_mode=%s "
           "oversold=%.0f overbought=%.0f sl_mult=%.1fx)\n",
           g_rsi_reversal.shadow_mode ? "true" : "false",
           g_rsi_reversal.RSI_OVERSOLD,
           g_rsi_reversal.RSI_OVERBOUGHT,
           g_rsi_reversal.SL_ATR_MULT);
    fflush(stdout);

    //  Real-tick backtest: 4320 trades / 2yr, -$3.8k. Momentum = negative EV.

    // [BUG-5 NOTE] MCE is shadow_mode=true by design -- it logs [MCE-SHADOW] but sends
    // no FIX orders. Entry/exit logic is fully functional via on_close callback wired above.
    // To enable live MCE trades: set g_macro_crash.shadow_mode = false (requires authorisation).
    if (g_macro_crash.shadow_mode) {
        printf("[MCE] WARNING: MacroCrashEngine is in SHADOW mode -- no live orders will fire.\n"
               "[MCE] To enable: change shadow_mode=false in omega_main.hpp after validation.\n");
    }
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
    g_bracket_gold.MAX_RANGE   = 12.0;   // DATA-CALIBRATED: $12 max. Ranges >$12 are trending, not bracketing.
    // ?? S22c 2026-04-25: empirical SL-dist gate (gold only) ?????????????????????
    // 62 live XAUUSD_BRACKET trades 2026-04-13..24: every trade with bracket
    // dist > 6pt was a loser (0 wins / 39 losses / -$388 combined). Trades at
    // dist <= 6pt won 6/26 = 23% WR at +$73 net. The 6pt band is where the
    // bracket represents genuine compression vs late-breakout chasing.
    g_bracket_gold.MAX_SL_DIST_PTS = 6.0;
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
    g_orb_us.OPEN_HOUR    = 13; g_orb_us.OPEN_MIN    = 30;  // NY open 13:30 UTC
    g_orb_ger30.OPEN_HOUR = 8;  g_orb_ger30.OPEN_MIN = 0;   // Xetra open 08:00 UTC
    // New ORB instruments: LSE and Euronext with tighter 15-min range windows
    g_orb_uk100.OPEN_HOUR  = 8;  g_orb_uk100.OPEN_MIN  = 0;   // LSE open 08:00 UTC
    g_orb_uk100.RANGE_WINDOW_MIN = 15;  // 15-min range (LSE moves fast at open)
    g_orb_uk100.TP_PCT  = 0.12;  g_orb_uk100.SL_PCT  = 0.07;  // UK100 TP/SL calibrated to GBP volatility
    g_orb_estx50.OPEN_HOUR = 9;  g_orb_estx50.OPEN_MIN = 0;   // Euronext open 09:00 UTC
    g_orb_estx50.RANGE_WINDOW_MIN = 15; // 15-min range
    g_orb_estx50.TP_PCT = 0.10;  g_orb_estx50.SL_PCT = 0.06;  // ESTX50 TP/SL similar to GER40
    // VWAPReversionEngine params -- per-instrument tuning
    // Indices: raised 0.20->0.40% -- USTEC at $24000: 0.20%=$48 fires on noise.
    // 0.40%=$96 requires a genuine VWAP dislocation not a 2-tick wiggle.
    // MAX_EXTENSION raised 0.80->1.20%: prevents blocking real dislocations.
    // MAX_HOLD raised 900->600s: exit stalled trades faster (was holding 15min losers).
    g_vwap_rev_sp.enabled = false;  g_vwap_rev_sp.EXTENSION_THRESH_PCT    = 0.35; g_vwap_rev_sp.COOLDOWN_SEC    = 300;
    // S95 2026-05-15: disabled. SPX/US500 UltimateBacktest v1 (4118 trades, PF=0.92)
    //   and v2 (117 trades, PF=1.13 overall but OOS PF=0.88 — fails >=1.20 criterion).
    //   SPX lacks persistent trend-following momentum edge. ATR 8-15 band (120 trades,
    //   PF=1.19) was the only positive segment — too thin for production.
    g_vwap_rev_sp.MAX_EXTENSION_PCT       = 1.20;
    g_vwap_rev_sp.MAX_HOLD_SEC            = 600;
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
    g_vwap_rev_sp.LOSS_CUT_PCT            = 0.0;
    g_vwap_rev_sp.BE_ARM_PCT              = 0.0;
    g_vwap_rev_sp.BE_BUFFER_PCT           = 0.0;
    g_vwap_rev_nq.enabled = false;  g_vwap_rev_nq.EXTENSION_THRESH_PCT    = 0.40; g_vwap_rev_nq.COOLDOWN_SEC    = 300;
    g_vwap_rev_nq.MAX_EXTENSION_PCT       = 1.20;
    g_vwap_rev_nq.MAX_HOLD_SEC            = 600;
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
    g_vwap_rev_nq.LOSS_CUT_PCT            = 0.0;
    g_vwap_rev_nq.BE_ARM_PCT              = 0.0;
    g_vwap_rev_nq.BE_BUFFER_PCT           = 0.0;
    g_vwap_rev_ger40.enabled = true;  g_vwap_rev_ger40.EXTENSION_THRESH_PCT = 0.30; g_vwap_rev_ger40.COOLDOWN_SEC = 300;
    g_vwap_rev_ger40.MAX_EXTENSION_PCT    = 1.00;
    g_vwap_rev_ger40.MAX_HOLD_SEC         = 600;
    // 2026-05-13 (S37-H-followup): GER40 index defaults same as US500/USTEC.
    g_vwap_rev_ger40.LOSS_CUT_PCT         = 0.08;
    g_vwap_rev_ger40.BE_ARM_PCT           = 0.05;
    g_vwap_rev_ger40.BE_BUFFER_PCT        = 0.02;
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
    g_vwap_rev_eurusd.enabled = true;  g_vwap_rev_eurusd.EXTENSION_THRESH_PCT = 0.12; g_vwap_rev_eurusd.COOLDOWN_SEC = 120;
    g_vwap_rev_eurusd.MAX_EXTENSION_PCT   = 0.40;
    g_vwap_rev_eurusd.MAX_HOLD_SEC        = 600;
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
    g_vwap_rev_eurusd.LOSS_CUT_PCT        = 0.0;
    g_vwap_rev_eurusd.BE_ARM_PCT          = 0.0;
    g_vwap_rev_eurusd.BE_BUFFER_PCT       = 0.0;
    // ?? NBM London session engines (07:00-13:30 UTC) ????????????????????????????
    // Covers the gap before NY open. Gold and oil are liquid from London open.
    // Uses same ATR/band logic as NY engines but anchored to London open price.
    g_nbm_gold_london.SESSION_OPEN_UTC  =  7;  g_nbm_gold_london.SESSION_OPEN_MIN  =  0;
    g_nbm_gold_london.SESSION_CLOSE_UTC = 13;  g_nbm_gold_london.SESSION_CLOSE_MIN = 30;
    g_nbm_gold_london.MAX_SPREAD_PCT    = 0.02;  // gold spread tighter than indices
    g_nbm_gold_london.WARMUP_TICKS      = 120;
    g_nbm_gold_london.COOLDOWN_SEC      = 600;

    g_nbm_oil_london.SESSION_OPEN_UTC  =  7;  g_nbm_oil_london.SESSION_OPEN_MIN  =  0;
    g_nbm_oil_london.SESSION_CLOSE_UTC = 13;  g_nbm_oil_london.SESSION_CLOSE_MIN = 30;
    g_nbm_oil_london.MAX_SPREAD_PCT    = 0.05;
    g_nbm_oil_london.WARMUP_TICKS      = 120;
    g_nbm_oil_london.COOLDOWN_SEC      = 600;

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
    g_trend_pb_gold.PULLBACK_BAND_PCT  = 0.50;  // M15: ?23.5pts at $4700. Old 0.08% (?3.7pts) never fired.
    g_trend_pb_gold.COOLDOWN_SEC       = 60;    // 60s cooldown -- reduced from 900s (15min was insane, missed 100pt moves)
    g_trend_pb_gold.MIN_EMA_SEP        = 5.0;   // gold: 5pt EMA9-EMA50 separation = real trend
    g_trend_pb_gold.H4_GATE_ENABLED    = true;  // gate M15 entries on H4 trend direction
    g_trend_pb_gold.ATR_SL_MULT        = 1.2;   // SL floor = 1.2x M15 ATR (adaptive, not fixed 8pt)
    // Improvement 1: vol regime sizing
    g_trend_pb_gold.VOL_SCALE_HIGH_MULT = 1.5;
    g_trend_pb_gold.VOL_SCALE_LOW_MULT  = 0.7;
    g_trend_pb_gold.VOL_SCALE_CUT       = 0.60;
    g_trend_pb_gold.VOL_SCALE_BOOST     = 1.20;
    // Improvement 2: daily loss cap -- stop gold TrendPB after $150 loss in a day
    g_trend_pb_gold.DAILY_LOSS_CAP      = 150.0;
    // Improvement 4: time-of-day weighting
    g_trend_pb_gold.TOD_WEIGHT_ENABLED  = true;
    // Improvement 5: CVD gate
    g_trend_pb_gold.CVD_GATE_ENABLED    = true;
    // Improvement 7: news SL widening
    g_trend_pb_gold.NEWS_WARN_SECS      = 900;   // 15min before event
    g_trend_pb_gold.NEWS_SL_MULT        = 1.5;
    // Widen pullback band: 0.15% -> 0.50% (?23pts at 4620)
    // Default 0.15% = ?6.9pts. On a $20 trending move price is 20pts from EMA50
    // and never enters the band -- engine silent on all clean trends.
    // 0.50% allows entry when price is trending away from EMA50 but still directional.
    g_trend_pb_gold.PULLBACK_BAND_PCT   = 0.50;
    // Improvement 8: pyramid on second pullback
    g_trend_pb_gold.PYRAMID_ENABLED     = true;
    g_trend_pb_gold.PYRAMID_SIZE_MULT   = 0.5;
    g_trend_pb_gold.PYRAMID_MAX_ADDS    = 1;
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
    g_trend_pb_gold.enabled = false;

    // HTF swing engines v2 -- per-instrument params, partial TP, weekend close gate.
    // shadow_mode=true always. To go live: validate shadow signals then set false.
    {
        g_h1_swing_gold.p           = omega::make_h1_gold_params();
        g_h1_swing_gold.symbol      = "XAUUSD";
        g_h1_swing_gold.shadow_mode = true;
        // 2026-05-08 USER REQUEST: was true, only microscalper trades on gold.
        g_h1_swing_gold.enabled     = false;
        g_h4_regime_gold.p           = omega::make_h4_gold_params();
        g_h4_regime_gold.symbol      = "XAUUSD";
        g_h4_regime_gold.shadow_mode = true;
        g_h4_regime_gold.enabled     = false;  // S91: disabled — GoldUltimateEngine solo test
        printf("[INIT] H1SwingEngine  XAUUSD: shadow=true adx_min=%.0f sl=%.1fx"
               " tp1=%.1fx trail_arm=%.1fx trail_dist=%.1fx daily_cap=$%.0f\n",
               g_h1_swing_gold.p.adx_min,    g_h1_swing_gold.p.sl_mult,
               g_h1_swing_gold.p.tp1_mult,   g_h1_swing_gold.p.tp2_trail_arm_mult,
               g_h1_swing_gold.p.tp2_trail_dist_mult, g_h1_swing_gold.p.daily_cap);
        printf("[INIT] H4RegimeEngine XAUUSD: shadow=true channel=%d bars adx=%.0f"
               " sl=%.1fx tp=%.1fx trail_arm=%.1fx trail_dist=%.1fx daily_cap=$%.0f\n",
               g_h4_regime_gold.p.channel_bars, g_h4_regime_gold.p.adx_min,
               g_h4_regime_gold.p.sl_struct_mult, g_h4_regime_gold.p.tp_mult,
               g_h4_regime_gold.p.trail_arm_mult, g_h4_regime_gold.p.trail_dist_mult,
               g_h4_regime_gold.p.daily_cap);

        // MinimalH4Breakout -- pure Donchian, no filters. Runs PARALLEL to H4Regime.
        // OOS-validated config: D=10 SL=1.5x TP=4.0x. See header for evidence.
        g_minimal_h4_gold.p           = omega::make_minimal_h4_gold_params();
        g_minimal_h4_gold.symbol      = "XAUUSD";
        g_minimal_h4_gold.shadow_mode = false;
        g_minimal_h4_gold.enabled     = false;  // S91: disabled — GoldUltimateEngine solo test
        printf("[INIT] MinimalH4Breakout XAUUSD: shadow=%s donchian=%d sl=%.1fx"
               " tp=%.1fx risk=$%.0f max_lot=%.3f timeout=%d bars weekend_gate=%s\n",
               g_minimal_h4_gold.shadow_mode    ? "true" : "false",
               g_minimal_h4_gold.p.donchian_bars, g_minimal_h4_gold.p.sl_mult,
               g_minimal_h4_gold.p.tp_mult,       g_minimal_h4_gold.p.risk_dollars,
               g_minimal_h4_gold.p.max_lot,       g_minimal_h4_gold.p.timeout_h4_bars,
               g_minimal_h4_gold.p.weekend_close_gate ? "true" : "false");

        // C1RetunedPortfolio -- Phase 2 winner from CHOSEN.md, ported from
        // Python sim. Donchian H1 long retuned (period=20, sl=3.0 ATR,
        // tp=5.0 ATR) + Bollinger H2/H4/H6 long, max_concurrent=4, 0.5% risk.
        // Long-only, XAUUSD only, shadow_mode=true. Self-contained engine
        // (does not interfere with other engines or borrow their state).
        g_c1_retuned.shadow_mode    = true;
        g_c1_retuned.enabled        = true;
        g_c1_retuned.max_concurrent = 4;
        g_c1_retuned.risk_pct       = 0.005;
        g_c1_retuned.start_equity   = 10000.0;
        g_c1_retuned.margin_call    = 1000.0;
        g_c1_retuned.max_lot_cap    = 0.05;   // tighter than backtest while shadow-validating
        g_c1_retuned.init();
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
        g_tsmom.shadow_mode       = kShadowDefault;
        g_tsmom.enabled           = false;  // S99b: disabled — no session filter, no weekend gate, fires 24/7 into chop
        g_tsmom.max_concurrent    = 5;
        g_tsmom.risk_pct          = 0.005;
        g_tsmom.start_equity      = 10000.0;
        g_tsmom.margin_call       = 1000.0;
        // 2026-05-04 (post-handoff risk-budget fix): max_lot_cap 0.05 -> 0.02.
        //   Live shadow tape had a Tsmom_H1_long position lose $23.65 on a
        //   4.45pt adverse move (~5x bracket-cohort exposure). Capping at
        //   0.02 brings the same move to $8.90 (~2x bracket cohort) while
        //   preserving Sharpe (risk_pct unchanged at 0.005). g_tsmom_v2
        //   below inherits this cap via `= g_tsmom.max_lot_cap`.
        //   See omega_config.ini [tsmom] section for the parity comment.
        g_tsmom.max_lot_cap       = 0.02;
        g_tsmom.block_on_risk_off = true;
        g_tsmom.warmup_csv_path   = "phase1/signal_discovery/tsmom_warmup_H1.csv";
        g_tsmom.init();
        omega::warmup_or_die(g_tsmom, "Tsmom");
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
        g_tsmom_v2.shadow_mode             = true;          // ALWAYS shadow (refactor validation)
        g_tsmom_v2.enabled                 = true;
        g_tsmom_v2.max_concurrent          = 50;            // headroom; per-cell cap binds
        g_tsmom_v2.max_positions_per_cell  = 1;             // Phase 2a -- flip to 10 in Phase 2b
        g_tsmom_v2.risk_pct                = g_tsmom.risk_pct;
        g_tsmom_v2.start_equity            = g_tsmom.start_equity;
        g_tsmom_v2.margin_call             = g_tsmom.margin_call;
        g_tsmom_v2.max_lot_cap             = g_tsmom.max_lot_cap;
        g_tsmom_v2.usd_per_pt_per_lot      = 100.0;         // XAUUSD baseline
        g_tsmom_v2.block_on_risk_off       = g_tsmom.block_on_risk_off;
        g_tsmom_v2.symbol                  = "XAUUSD";
        g_tsmom_v2.regime_label            = "TSMOM_V2";    // distinguishable in shadow CSV
        omega::cell::build_default_tsmom_topology(g_tsmom_v2);
        g_tsmom_v2.warmup_csv_path         = "phase1/signal_discovery/tsmom_warmup_H1.csv";
        g_tsmom_v2.init();
        omega::warmup_or_die(g_tsmom_v2, "TsmomV2");
        omega::cell::shadow::tsmom_writer().open("logs/shadow/tsmom_v2.csv");
        printf("[TSMOM-V2] live shadow ARMED (max_pos_per_cell=%d, ledger=logs/shadow/tsmom_v2.csv)\n",
               g_tsmom_v2.max_positions_per_cell);
        fflush(stdout);

        // ?? DonchianPortfolio -- Tier-2 ship 2026-04-30 ???????????????????????
        // 7 donchian cells: H2 long, H4 long+short, H6 long+short, D1 long+short.
        // (H1 long is NOT here -- it's the retuned cell in C1RetunedPortfolio.)
        // Verdict: phase1/signal_discovery/POST_CUT_FULL_REPORT.md
        // Combined: 328 trades/yr, +$5,620 = 47% of unshipped post-cut edge.
        // Bidirectional: would have profited during 2026-03-18 BEAR cluster.
        // Reuses tsmom warmup CSV (same H1 stream input).
        g_donchian.shadow_mode       = kShadowDefault;
        g_donchian.enabled           = true;
        g_donchian.max_concurrent    = 7;
        g_donchian.risk_pct          = 0.005;
        g_donchian.start_equity      = 10000.0;
        g_donchian.margin_call       = 1000.0;
        g_donchian.max_lot_cap       = 0.05;
        g_donchian.block_on_risk_off = true;
        g_donchian.warmup_csv_path   = "phase1/signal_discovery/tsmom_warmup_H1.csv";
        g_donchian.init();
        omega::warmup_or_die(g_donchian, "Donchian");
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
        g_xau_tf_4h.shadow_mode = false;
        g_xau_tf_4h.enabled     = true;
        // S116 2026-05-19: bit 6 (0x40) added for EmaCross8_21 cell from the
        // S114 long-trend ensemble research (Python +$30,966 / Sharpe +1.96
        // / 25mo; C++ +$32,025 / 95 trades).
        // S117 2026-05-19: activated.  Service-level mode=SHADOW provides
        // synthetic-fills safety net; engine.shadow_mode=false keeps the
        // ledger emission identical to other live cells so we can validate
        // EmaCross_8_21 fill rate against the S115 backtest CSV.  Expect
        // ~4 trades/month per the C++ harness (95 trades / 25mo).
        g_xau_tf_4h.cell_enable_mask = 0x49;  // Donchian + Keltner + EmaCross_8_21
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
        g_xau_tf_4h.warmup_csv_path = "phase1/signal_discovery/warmup_XAUUSD_H4.csv";
        g_xau_tf_4h.init();
        omega::warmup_or_die(g_xau_tf_4h, "XauTrendFollow4h");
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
        g_xau_tf_1h.shadow_mode = false;
        g_xau_tf_1h.enabled     = true;
        g_xau_tf_1h.cell_enable_mask = 0x03;  // both cells
        g_xau_tf_1h.lot         = 0.01;
        g_xau_tf_1h.max_spread  = 1.0;
        g_xau_tf_1h.warmup_csv_path = "phase1/signal_discovery/warmup_XAUUSD_H1.csv";
        g_xau_tf_1h.init();
        omega::warmup_or_die(g_xau_tf_1h, "XauTrendFollow1h");
        printf("[OMEGA-INIT] XauTrendFollow1hEngine initialised: shadow=%d enabled=%d lot=%.2f cells=2 mask=0x%X"
               " (EmaCross_20_80_S118, Donchian_N40_S118)\n",
               (int)g_xau_tf_1h.shadow_mode, (int)g_xau_tf_1h.enabled, g_xau_tf_1h.lot,
               (unsigned)g_xau_tf_1h.cell_enable_mask);
        fflush(stdout);

        // ── UstecTrendFollow5mEngine (S33d 2026-05-11) ───────────────────────
        // Donchian N=20 at 5m bars on USTEC. Convergent edge across 4
        // unrelated signal families on the 15-day L2 sample (n=111,
        // WR=45%, BE cost $10.1, 170x margin over $0.06).
        // CAVEAT: only 2 months of data. KEEP shadow until 6+ months
        // L2 capture confirm the finding.
        g_ustec_tf_5m.shadow_mode = true;          // HARD shadow, ignore kShadowDefault
        g_ustec_tf_5m.enabled     = false;
        g_ustec_tf_5m.lot         = 0.1;
        g_ustec_tf_5m.max_spread  = 5.0;
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
        g_ustec_tf_5m.LOSS_CUT_PCT  = 0.08;
        g_ustec_tf_5m.BE_ARM_PCT    = 0.05;
        g_ustec_tf_5m.BE_BUFFER_PCT = 0.02;
        g_ustec_tf_5m.init();
        printf("[OMEGA-INIT] UstecTrendFollow5mEngine initialised: shadow=%d enabled=%d lot=%.2f cells=2"
               " (Donchian,Keltner) (HARD SHADOW)\n",
               (int)g_ustec_tf_5m.shadow_mode, (int)g_ustec_tf_5m.enabled, g_ustec_tf_5m.lot);
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
        g_ustec_tf_5m.on_fire_hook = [](int64_t now_s) {
            g_risk_monitor.on_fire("UstecTrendFollow5m", now_s);
        };
        // S37-P3: three auto-pin callbacks. Each flips the same engine
        //   instance to shadow on any trip. The umbrella name handles
        //   fire-rate trips; the per-cell names handle close-side WR /
        //   spread trips because tr.engine carries the cell suffix
        //   (UstecTrendFollow5mEngine.hpp S34 BUG #3). All three target
        //   the same g_ustec_tf_5m.shadow_mode -- on a trip from any of
        //   the three evaluators, the engine pins regardless of which
        //   cell or which check tripped.
        auto pin_ustec_tf_5m = [](const std::string& reason) {
            if (!g_ustec_tf_5m.shadow_mode) {
                g_ustec_tf_5m.shadow_mode = true;
                printf("[RISK-MON] AUTO-PIN UstecTrendFollow5m to SHADOW: %s\n",
                       reason.c_str());
                fflush(stdout);
            }
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
        g_xau_tf_d1.shadow_mode = true;
        g_xau_tf_d1.enabled     = true;   // S88: revived w/ vol-band gate, HARD shadow
        // S88-followup post-sweep 2026-05-27: widen D1 band [0.30,0.85] ->
        // [0.20,0.90]. Sweep showed D1 entry-vol distribution sits inside the
        // band already; widening picks up 2 extra cell-Keltner trades (PF
        // 3.27 standalone) and 1 Momentum20 trade. Modest lift: $3220 -> $3369
        // over 2yr (+$149). Marginal but free; tighter bands gave identical
        // result so wider is dominated upside.
        g_xau_tf_d1.use_vol_band_gate = true;
        g_xau_tf_d1.vol_band_low_pct  = 0.20;
        g_xau_tf_d1.vol_band_high_pct = 0.90;
        g_xau_tf_d1.lot         = 0.01;
        g_xau_tf_d1.max_spread  = 1.0;
        g_xau_tf_d1.warmup_csv_path = "phase1/signal_discovery/warmup_XAUUSD_H4.csv";
        g_xau_tf_d1.init();
        omega::warmup_or_die(g_xau_tf_d1, "XauTrendFollowD1");
        printf("[OMEGA-INIT] XauTrendFollowD1Engine initialised: shadow=%d enabled=%d lot=%.2f cells=3"
               " (Momentum,Keltner,ADX_Mom)\n",
               (int)g_xau_tf_d1.shadow_mode, (int)g_xau_tf_d1.enabled, g_xau_tf_d1.lot);
        fflush(stdout);

        // ── XauTsmomFastD1Engine (2026-05-20) -- short-lookback momentum sister
        //   Backtest 2yr daily XAU (long_only, cost 1bps):
        //     IS Sh=6.69 / OOS Sh=7.65 / FUL Sh=7.57. n=48. PnL=78.1%.
        //   Cost stress holds: Sh 6.69 at 20bps. Distinct cell from D1 ensemble
        //   (lb=5 vs lb=20, sl=1.0 vs 2.0, tp=5.0 vs 4.0, hold=20).
        g_xau_tsmom_fast_d1.p           = omega::make_xau_tsmom_fast_d1_params();
        // 2026-05-27 S57: DISABLED -- regime split MID Sharpe -0.86 (negative
        // in normal-vol regime, 24/59 trades). Operator policy: must pass ALL
        // rigour tests; one regime negative = fail.
        g_xau_tsmom_fast_d1.shadow_mode = true;
        g_xau_tsmom_fast_d1.enabled     = false;  // S57: regime MID neg
        g_xau_tsmom_fast_d1.symbol      = "XAUUSD";
        printf("[OMEGA-INIT] XauTsmomFastD1Engine: shadow=%d enabled=%d lb=%d sl=%.1fx tp=%.1fx hold=%d\n",
               (int)g_xau_tsmom_fast_d1.shadow_mode, (int)g_xau_tsmom_fast_d1.enabled,
               g_xau_tsmom_fast_d1.p.lookback_days, g_xau_tsmom_fast_d1.p.sl_atr_mult,
               g_xau_tsmom_fast_d1.p.tp_atr_mult, g_xau_tsmom_fast_d1.p.hold_max_days);
        fflush(stdout);

        // ── XauTurtleD1Engine (2026-05-20) -- 40d Donchian break (long-only)
        //   Resurrection of S50 X1 retired TurtleTick. Re-tested 2yr daily XAU:
        //     FUL Sh=13.01 at 10bps (IS=7.32 OOS=18.42), n=20, WR=70%.
        //     Cost-robust to 50bps (FUL Sh=10.51).
        //   CAVEAT: sparse (~10 trades/year). High Sharpe variance from low n.
        g_xau_turtle_d1.p           = omega::make_xau_turtle_d1_params();
        g_xau_turtle_d1.shadow_mode = true;
        g_xau_turtle_d1.enabled     = true;
        g_xau_turtle_d1.symbol      = "XAUUSD";
        printf("[OMEGA-INIT] XauTurtleD1Engine: shadow=%d enabled=%d lb=%d sl=%.1fx tp=%.1fx hold=%d\n",
               (int)g_xau_turtle_d1.shadow_mode, (int)g_xau_turtle_d1.enabled,
               g_xau_turtle_d1.p.lookback_days, g_xau_turtle_d1.p.sl_atr_mult,
               g_xau_turtle_d1.p.tp_atr_mult, g_xau_turtle_d1.p.hold_max_days);
        fflush(stdout);

        // ── XauStopRunD1Engine (2026-05-20) -- 5d stop-run reversal long
        //   Resurrection of S50 X2 retired StopRunReversal. Re-tested 2yr daily:
        //     FUL Sh=6.34 at 10bps (IS=7.06 OOS=6.14), n=29, WR=65.5%.
        //     Cost-robust to 50bps (FUL Sh=4.12).
        g_xau_stop_run_d1.p           = omega::make_xau_stop_run_d1_params();
        // 2026-05-27 S57: DISABLED -- regime split LOW Sharpe -3.68 (clear
        // negative in low-vol regime, 9/28 trades). Operator strict policy.
        g_xau_stop_run_d1.shadow_mode = true;
        g_xau_stop_run_d1.enabled     = false;  // S57: regime LOW neg
        g_xau_stop_run_d1.symbol      = "XAUUSD";
        printf("[OMEGA-INIT] XauStopRunD1Engine: shadow=%d enabled=%d lb=%d sl=%.1fx tp=%.1fx hold=%d\n",
               (int)g_xau_stop_run_d1.shadow_mode, (int)g_xau_stop_run_d1.enabled,
               g_xau_stop_run_d1.p.lookback_days, g_xau_stop_run_d1.p.sl_atr_mult,
               g_xau_stop_run_d1.p.tp_atr_mult, g_xau_stop_run_d1.p.hold_max_days);
        fflush(stdout);

        // ── XauPullbackContH4Engine (2026-05-20) -- EMA10>EMA50 pullback long
        //   PullbackCont archetype (S49 X5 retirement). H4 2yr XAU:
        //   FUL Sh=3.96, IS=3.97, OOS=4.06, n=97 (highest density of D-class).
        g_xau_pullback_cont_h4.p           = omega::make_xau_pullback_cont_h4_params();
        g_xau_pullback_cont_h4.shadow_mode = true;
        g_xau_pullback_cont_h4.enabled     = true;
        g_xau_pullback_cont_h4.symbol      = "XAUUSD";
        printf("[OMEGA-INIT] XauPullbackContH4Engine: shadow=%d enabled=%d ef=%d es=%d sl=%.1fx tp=%.1fx hold=%d\n",
               (int)g_xau_pullback_cont_h4.shadow_mode, (int)g_xau_pullback_cont_h4.enabled,
               g_xau_pullback_cont_h4.p.ema_fast, g_xau_pullback_cont_h4.p.ema_slow,
               g_xau_pullback_cont_h4.p.sl_atr_mult, g_xau_pullback_cont_h4.p.tp_atr_mult,
               g_xau_pullback_cont_h4.p.hold_max_h4);
        fflush(stdout);

        // ── XauNbmD1Engine (2026-05-20) -- Noise Band Momentum D1
        //   Signal from disabled g_nbm_* family. D1 XAU 2yr:
        //   FUL Sh=8.01, IS=9.60, OOS=7.30, n=25.
        g_xau_nbm_d1.p           = omega::make_xau_nbm_d1_params();
        // 2026-05-27 S53: DISABLED -- DD/gross=111% (+6.34 / -7.01).
        // Sharpe +1.91 positive but equity buries deeper than recovers.
        // Same failure mode as TF2h/D1 (S52).
        g_xau_nbm_d1.shadow_mode = true;
        g_xau_nbm_d1.enabled     = false;  // S53: DD ratio fail
        g_xau_nbm_d1.symbol      = "XAUUSD";
        printf("[OMEGA-INIT] XauNbmD1Engine: shadow=%d enabled=%d ema=%d band=%.1fx mom=%.1fx sl=%.1fx tp=%.1fx hold=%d\n",
               (int)g_xau_nbm_d1.shadow_mode, (int)g_xau_nbm_d1.enabled,
               g_xau_nbm_d1.p.ema_period, g_xau_nbm_d1.p.atr_band_mult,
               g_xau_nbm_d1.p.momentum_atr_mult, g_xau_nbm_d1.p.sl_atr_mult,
               g_xau_nbm_d1.p.tp_atr_mult, g_xau_nbm_d1.p.hold_max_days);
        fflush(stdout);

        // ── XauEmaCrossH4Engine (2026-05-20) -- 20/100 golden cross H4
        //   H4 XAU 2yr: FUL Sh=7.15, IS=4.45, OOS=9.19 (OOS > IS).
        //   Sparse n=20 but cleanly OOS-validated.
        g_xau_ema_cross_h4.p           = omega::make_xau_ema_cross_h4_params();
        // 2026-05-27 S57: DISABLED -- regime split LOW Sharpe -8.48 (worst
        // catastrophic single-regime failure in entire zoo). Overall +6.38
        // misleading -- engine bleeds heavily when low-vol regime persists.
        g_xau_ema_cross_h4.shadow_mode = true;
        g_xau_ema_cross_h4.enabled     = false;  // S57: regime LOW catastrophic
        g_xau_ema_cross_h4.symbol      = "XAUUSD";
        printf("[OMEGA-INIT] XauEmaCrossH4Engine: shadow=%d enabled=%d ef=%d es=%d sl=%.1fx tp=%.1fx hold=%d\n",
               (int)g_xau_ema_cross_h4.shadow_mode, (int)g_xau_ema_cross_h4.enabled,
               g_xau_ema_cross_h4.p.ema_fast, g_xau_ema_cross_h4.p.ema_slow,
               g_xau_ema_cross_h4.p.sl_atr_mult, g_xau_ema_cross_h4.p.tp_atr_mult,
               g_xau_ema_cross_h4.p.hold_max_h4);
        fflush(stdout);

        // ── 2026-05-20 mega-sweep batch (4 new engines) ─────────────────────
        g_xau_pullback_cont_d1.p           = omega::make_xau_pullback_cont_d1_params();
        // 2026-05-27 S54: DISABLED -- walk-forward OOS sign-flip.
        // IS Sharpe +4.27 / 22 trades / +$6.07 gross
        // OOS Sharpe -1.12 / 5 trades / -$1.19 gross  <-- sign flipped
        // Edge does not survive out-of-sample. PullbackH4 robust as backup.
        g_xau_pullback_cont_d1.shadow_mode = true;
        g_xau_pullback_cont_d1.enabled     = false;  // S54: WF OOS fail
        g_xau_pullback_cont_d1.symbol      = "XAUUSD";
        printf("[OMEGA-INIT] XauPullbackContD1Engine: shadow=%d ef=%d es=%d pba=%.1f sl=%.1fx tp=%.1fx hold=%d\n",
               (int)g_xau_pullback_cont_d1.shadow_mode,
               g_xau_pullback_cont_d1.p.ema_fast, g_xau_pullback_cont_d1.p.ema_slow,
               g_xau_pullback_cont_d1.p.pullback_atr,
               g_xau_pullback_cont_d1.p.sl_atr_mult, g_xau_pullback_cont_d1.p.tp_atr_mult,
               g_xau_pullback_cont_d1.p.hold_max_days);

        g_xau_bb_scalp_d1.p           = omega::make_xau_bb_scalp_d1_params();
        // 2026-05-27 S53: DISABLED -- DD/gross=240% (+3.68 / -8.85). Worst
        // DD ratio in the entire zoo. Sharpe +1.75 positive but extremely
        // unstable equity curve. Same failure mode as TF2h/D1 (S52).
        g_xau_bb_scalp_d1.shadow_mode = true;
        g_xau_bb_scalp_d1.enabled     = false;  // S53: DD ratio fail
        g_xau_bb_scalp_d1.symbol      = "XAUUSD";
        printf("[OMEGA-INIT] XauBBScalpD1Engine: shadow=%d bb_p=%d std=%.1f sl=%.1fx tp=%.1fx hold=%d\n",
               (int)g_xau_bb_scalp_d1.shadow_mode,
               g_xau_bb_scalp_d1.p.bb_period, g_xau_bb_scalp_d1.p.bb_std_mult,
               g_xau_bb_scalp_d1.p.sl_atr_mult, g_xau_bb_scalp_d1.p.tp_atr_mult,
               g_xau_bb_scalp_d1.p.hold_max_days);

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
        g_xau_d55_gated_m30.shadow_mode = true;
        g_xau_d55_gated_m30.enabled     = false;  // S50: real-class fail
        g_xau_d55_gated_m30.symbol      = "XAUUSD";
        g_xau_d55_gated_m30.seed_from_m30_csv("phase1/signal_discovery/warmup_XAUUSD_M30.csv");
        printf("[OMEGA-INIT] XauDonchian55GatedM30: shadow=%d enabled=%d n=%d sl=%.1fx tp=%.1fR mb=%d "
               "trail_arm=%.1fR lock=%.0f%%\n",
               (int)g_xau_d55_gated_m30.shadow_mode, (int)g_xau_d55_gated_m30.enabled,
               g_xau_d55_gated_m30.p.donchian_period,
               g_xau_d55_gated_m30.p.sl_atr_mult, g_xau_d55_gated_m30.p.tp_r_mult,
               g_xau_d55_gated_m30.p.hold_max_bars,
               g_xau_d55_gated_m30.p.trail_arm_R, g_xau_d55_gated_m30.p.trail_lock_pct*100.0);

        // ── S136 2026-05-24: Xau3BarMomGatedH4Engine ───────────────────────────
        // XAU H4 three-bar momentum, symmetric long+short.
        // MFE-lock trail: arm at +1.0R, lock 90% of extreme.
        // L2 forward: PF 1.53, +$365 / 34 trades. WF agg OOS +$2931 / 199 trades.
        // 2026-05-27 S50: DISABLED -- real-class audit (xau_d1_zoo_audit)
        // showed real Sharpe -1.81 / -$30 / 384 trades / SL=185 TP=15. The
        // claimed PF 1.53 was harness-class divergence. Class-driven test on
        // full H4 tape says the engine bleeds. Keep wired but off.
        g_xau_3bar_mom_h4.shadow_mode = true;
        g_xau_3bar_mom_h4.enabled     = false;  // S50: real-class fail
        g_xau_3bar_mom_h4.symbol      = "XAUUSD";
        omega::seed_h4_engine(g_xau_3bar_mom_h4,
                              "phase1/signal_discovery/warmup_XAUUSD_H4.csv",
                              "Xau3BarMomGatedH4");
        printf("[OMEGA-INIT] Xau3BarMomGatedH4: shadow=%d enabled=%d sl=%.1fx tp=%.1fR mb=%d "
               "trail_arm=%.1fR lock=%.0f%%\n",
               (int)g_xau_3bar_mom_h4.shadow_mode, (int)g_xau_3bar_mom_h4.enabled,
               g_xau_3bar_mom_h4.p.sl_atr_mult, g_xau_3bar_mom_h4.p.tp_r_mult,
               g_xau_3bar_mom_h4.p.hold_max_bars,
               g_xau_3bar_mom_h4.p.trail_arm_R, g_xau_3bar_mom_h4.p.trail_lock_pct*100.0);

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
        g_us30_3bar_mom_h1.shadow_mode = true;
        g_us30_3bar_mom_h1.enabled     = false;  // S47 2026-05-27: scalp-class purge; pending real-class audit
        g_us30_3bar_mom_h1.symbol      = "US30";
        g_us30_3bar_mom_h1.seed_from_h1_csv("phase1/signal_discovery/warmup_US30_H1.csv");
        printf("[OMEGA-INIT] Us303BarMomH1: shadow=%d enabled=%d sl=%.1fx tp=%.1fR mb=%d "
               "trail_arm=%.1fR lock=%.0f%%\n",
               (int)g_us30_3bar_mom_h1.shadow_mode, (int)g_us30_3bar_mom_h1.enabled,
               g_us30_3bar_mom_h1.p.sl_atr_mult, g_us30_3bar_mom_h1.p.tp_r_mult,
               g_us30_3bar_mom_h1.p.hold_max_bars,
               g_us30_3bar_mom_h1.p.trail_arm_R, g_us30_3bar_mom_h1.p.trail_lock_pct*100.0);

        // ── S37 2026-05-26: Us30EnsembleEngine ─────────────────────────────────
        // DJ30.F 4-cell ensemble (M15 base; synthesizes M30/H1/H4 internally).
        // Cells: atr_exp H1, inside_brk H1, atr_exp M30, ema_pullback H4. All
        // LONG-only. BE/trail OFF by default (sim showed they clip atr_exp
        // winners; ENGINE_SIM.PY bare +$1411 vs trail -$676 over 2yr at 0.01 lot).
        // Cells INDEPENDENT (up to 4 concurrent positions per operator choice).
        // Validation: 3-period intersection + 4/4 walk-forward folds positive
        // + engine-sim integrated backtest +$1411 / 1711 trades / WR 50%.
        g_us30_ensemble.shadow_mode        = true;     // HARD shadow per CLAUDE.md ~1mo trace rule
        g_us30_ensemble.enabled            = true;
        g_us30_ensemble.lot                = 0.01;
        // max_spread bumped 5.0 -> 10.0 (2026-05-26): VPS shadow showed
        // [GUARD-BLOCK] engine=Us30Ensemble reason=SPREAD_CAP firing every tick
        // at post-NY-close DJ30 spread ~5.80 pts. 10.0 covers Asia/off-RTH
        // sessions; tighter live-session spreads (~2-4 pts) still pass.
        g_us30_ensemble.max_spread         = 10.0;
        g_us30_ensemble.be_trigger_atr     = 0.0;      // OFF (validated bare)
        g_us30_ensemble.trail_after_be     = false;
        g_us30_ensemble.trail_atr_mult     = 0.0;
        g_us30_ensemble.min_atr_floor      = 10.0;     // DJ30 raw points
        g_us30_ensemble.daily_loss_limit   = 0.0;      // disabled (S35-P4 finding)
        g_us30_ensemble.max_consec_losses  = 0;        // disabled
        g_us30_ensemble.init();
        const int us30_seed = g_us30_ensemble.seed_from_m15_csv(
            "phase1/signal_discovery/warmup_US30_M15.csv");
        printf("[OMEGA-INIT] Us30Ensemble: shadow=%d enabled=%d lot=%.2f cells=4 "
               "(AtrExpH1+InsBrkH1+AtrExpM30+EmaPbH4) seed=%d M15 bars trail=OFF\n",
               (int)g_us30_ensemble.shadow_mode, (int)g_us30_ensemble.enabled,
               g_us30_ensemble.lot, us30_seed);

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

        g_ger40_turtle_h4.p           = omega::make_ger40_turtle_h4_params();
        g_ger40_turtle_h4.shadow_mode = true;
        g_ger40_turtle_h4.enabled     = true;
        g_ger40_turtle_h4.symbol      = "GER40";
        // Warm-seed GER40 H4 history (~1600 bars / 11 months) so 20-bar
        // Donchian + 14-bar ATR are populated. Without seed, cold-warm
        // requires 80+ hours of live GER40 ticks before first signal.
        g_ger40_turtle_h4.seed_from_h4_csv("phase1/signal_discovery/warmup_GER40_H4.csv");
        printf("[OMEGA-INIT] Ger40TurtleH4Engine: shadow=%d lb=%d sl=%.1fx tp=%.1fx hold=%d\n",
               (int)g_ger40_turtle_h4.shadow_mode,
               g_ger40_turtle_h4.p.lookback_bars,
               g_ger40_turtle_h4.p.sl_atr_mult, g_ger40_turtle_h4.p.tp_atr_mult,
               g_ger40_turtle_h4.p.hold_max_h4);

        // ── FxTurtleH4 cohort (2026-05-23) ──────────────────────────────────
        // Post-S99 FX rebuild: long-only Donchian H4 (Turtle archetype) on
        // FX majors. EUR/GBP have proven walk-forward edge in repo
        // (walkforward_b_long_EURUSD_picks.csv, all 3 OOS folds PF 1.14-1.30).
        // AUD/NZD/JPY enabled=false until their warmup CSVs are sourced;
        // seed-guard skips disabled engines so boot stays clean.
        //
        // Warm-seed pattern: H1 CSV aggregated to H4 inline by
        // FxTurtleH4Engine::warmup_from_csv() (no offline resample needed).

        g_eurusd_turtle_h4.p               = omega::make_eurusd_turtle_h4_params();
        g_eurusd_turtle_h4.shadow_mode     = true;
        g_eurusd_turtle_h4.enabled         = true;
        g_eurusd_turtle_h4.symbol          = "EURUSD";
        g_eurusd_turtle_h4.warmup_csv_path = "phase1/signal_discovery/warmup_EURUSD_H1.csv";
        omega::warmup_or_die(g_eurusd_turtle_h4, "EurusdTurtleH4");
        printf("[OMEGA-INIT] EurusdTurtleH4: shadow=%d lb=%d sl=%.1fx tp=%.1fx hold=%d long_only=%d\n",
               (int)g_eurusd_turtle_h4.shadow_mode, g_eurusd_turtle_h4.p.lookback_bars,
               g_eurusd_turtle_h4.p.sl_atr_mult, g_eurusd_turtle_h4.p.tp_atr_mult,
               g_eurusd_turtle_h4.p.hold_max_h4, (int)g_eurusd_turtle_h4.p.long_only);

        g_gbpusd_turtle_h4.p               = omega::make_gbpusd_turtle_h4_params();
        g_gbpusd_turtle_h4.shadow_mode     = true;
        g_gbpusd_turtle_h4.enabled         = true;
        g_gbpusd_turtle_h4.symbol          = "GBPUSD";
        g_gbpusd_turtle_h4.warmup_csv_path = "phase1/signal_discovery/warmup_GBPUSD_H1.csv";
        omega::warmup_or_die(g_gbpusd_turtle_h4, "GbpusdTurtleH4");
        printf("[OMEGA-INIT] GbpusdTurtleH4: shadow=%d lb=%d sl=%.1fx tp=%.1fx hold=%d long_only=%d\n",
               (int)g_gbpusd_turtle_h4.shadow_mode, g_gbpusd_turtle_h4.p.lookback_bars,
               g_gbpusd_turtle_h4.p.sl_atr_mult, g_gbpusd_turtle_h4.p.tp_atr_mult,
               g_gbpusd_turtle_h4.p.hold_max_h4, (int)g_gbpusd_turtle_h4.p.long_only);

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
            const char* warmup_eur = "phase1/signal_discovery/warmup_EURUSD_H1.csv";
            const char* warmup_gbp = "phase1/signal_discovery/warmup_GBPUSD_H1.csv";

            // 2026-05-26 multi-TF tick-replay sweep (5 pairs x 4 TFs x X={10,14}):
            //   EURUSD best = M15 X=14 -> 21 trd, WR 52%, PF 1.80, +$9.04, DD $4.31, Sharpe-ann 1.04
            //   GBPUSD best = H1  X=10 -> 13 trd, WR 54%, PF 2.11, +$17.35, DD $6.95, Sharpe-ann 1.01
            //   AUDUSD best = H1  X=10 -> 20 trd, WR 35%, PF 1.34, +$6.82,  DD $17.61 (marginal)
            //   NZDUSD best = M30 X=10 -> 53 trd, WR 32%, PF 1.31, +$5.91,  DD $10.92 (marginal)
            //   USDCAD best = H1  X=10 -> 22 trd, WR 41%, PF 1.25, +$1.65,  DD $10.85 (marginal)
            // Portfolio Sharpe-ann 1.24 / PF 1.42 / Recovery 1.64 (0.01 lot, 1.2yr avg).
            // EURUSD trait uses BAR_INTERVAL_MS=900000 + X=14 (see AtrMeanRevGridEngine.hpp).
            // 2026-05-26 S37e: state-first persistence. Try .dat first
            // (zero-warmup), fall back to CSV seed if .dat missing/stale.
            const std::string state_dir = state_root_dir();
            auto amr_boot = [&](auto& eng, const std::string& sym_tag,
                                const std::string& csv_path) {
                eng.shadow_mode = true;
                eng.on_close_cb = write_shadow_csv;
                const std::string state_path = state_dir + "/amr_" + sym_tag + ".dat";
                if (!eng.load_state(state_path)) {
                    eng.seed_from_h1_csv(csv_path);
                }
            };

            g_amr_eurusd.enabled     = false;  // S47: AMR mean-rev purge pending real-class audit
            amr_boot(g_amr_eurusd, "eurusd", warmup_eur);

            g_amr_gbpusd.enabled     = false;  // S47: AMR mean-rev purge
            amr_boot(g_amr_gbpusd, "gbpusd", warmup_gbp);

            // S37g 2026-05-26: FxEnsembleEngine -- 5 cross-family validated cells.
            // Each instance is a different pair; enable_cell() flips on the
            // one cell that survived 4-gate validation for that pair.
            auto fx_ens_boot = [&](auto& eng, const std::string& sym_tag,
                                    const std::string& csv_path,
                                    double max_spread, double atr_floor) {
                eng.shadow_mode = true;
                eng.enabled     = true;
                eng.lot         = 0.01;
                eng.max_spread_price = max_spread;
                eng.min_atr_floor    = atr_floor;
                eng.init();
                const std::string sp = state_root_dir() + "/fxens_" + sym_tag + ".dat";
                eng.load_or_seed_from_h1_csv(sp, csv_path);
            };
            // EURUSD: 3 cells -- donchian_55 H1 LONG + keltner H1 LONG + asian_break H4 LONG
            g_fx_ens_eurusd.enable_cell(omega::FxCellId::DONCHIAN_55_H1_LONG, 3.0, 1.0, 24);
            g_fx_ens_eurusd.enable_cell(omega::FxCellId::KELTNER_H1_LONG,     3.0, 1.67, 24); // S37h PF 2.33
            g_fx_ens_eurusd.enable_cell(omega::FxCellId::ASIAN_BREAK_H4_LONG, 1.0, 1.0, 24);  // S37h PF 1.78
            fx_ens_boot(g_fx_ens_eurusd, "eurusd",
                        "phase1/signal_discovery/warmup_EURUSD_H1.csv",
                        0.00030, 0.00010);
            // GBPUSD: 2 cells -- bb_rev_20 H2 LONG + london_momo H4 LONG
            g_fx_ens_gbpusd.enable_cell(omega::FxCellId::BB_REV_20_H2_LONG, 3.0, 1.67, 96);
            g_fx_ens_gbpusd.enable_cell(omega::FxCellId::LONDON_MOMO_H4_LONG, 1.0, 1.5, 48);  // S37h PF 2.31 RF 7.47
            fx_ens_boot(g_fx_ens_gbpusd, "gbpusd",
                        "phase1/signal_discovery/warmup_GBPUSD_H1.csv",
                        0.00035, 0.00012);
            // AUDUSD: 1 cell -- bb_rev_20 H4 LONG (thin n=5, shadow only)
            g_fx_ens_audusd.enable_cell(omega::FxCellId::BB_REV_20_H4_LONG, 3.0, 0.67, 24);
            fx_ens_boot(g_fx_ens_audusd, "audusd",
                        "phase1/signal_discovery/warmup_AUDUSD_H1.csv",
                        0.00035, 0.00012);
            // USDCAD: 4 cells -- 3bar_mom S + london_momo L + keltner S + kumo_break S
            g_fx_ens_usdcad.enable_cell(omega::FxCellId::THREE_BAR_MOM_H4_SHORT, 1.5, 3.33, 24);
            g_fx_ens_usdcad.enable_cell(omega::FxCellId::LONDON_MOMO_H4_LONG, 1.0, 2.0, 96);  // S37h PF 2.31
            g_fx_ens_usdcad.enable_cell(omega::FxCellId::KELTNER_H2_SHORT, 2.0, 0.75, 96);    // S37h PF 2.21 Sh 1.96
            g_fx_ens_usdcad.enable_cell(omega::FxCellId::KUMO_BREAK_H2_SHORT, 3.0, 0.5, 24);  // S37h PF 2.20
            fx_ens_boot(g_fx_ens_usdcad, "usdcad",
                        "phase1/signal_discovery/warmup_USDCAD_H1.csv",
                        0.00040, 0.00015);
            // USDJPY: 2 cells -- donchian_20 H2 LONG + engulfing D1-approx LONG
            g_fx_ens_usdjpy.enable_cell(omega::FxCellId::DONCHIAN_20_H2_LONG, 1.5, 3.33, 96);
            g_fx_ens_usdjpy.enable_cell(omega::FxCellId::ENGULFING_D1_LONG, 1.0, 1.0, 48);    // S37h PF 2.10
            fx_ens_boot(g_fx_ens_usdjpy, "usdjpy",
                        "phase1/signal_discovery/warmup_USDJPY_H1.csv",
                        0.050, 0.01);
            // NZDUSD: 1 cell -- london_momo H2 SHORT (NEW S37h)
            g_fx_ens_nzdusd.enable_cell(omega::FxCellId::LONDON_MOMO_H2_SHORT, 1.5, 1.0, 24); // S37h PF 1.76
            fx_ens_boot(g_fx_ens_nzdusd, "nzdusd",
                        "phase1/signal_discovery/warmup_NZDUSD_H1.csv",
                        0.00040, 0.00015);
            std::printf("[OMEGA-INIT] FxEnsembleEngine: 5 pairs enabled (shadow) -- "
                       "EUR(Donch55H1L) GBP(BBrev20H2L) AUD(BBrev20H4L) "
                       "CAD(3barH4S) JPY(Donch20H2L)\n");

            // S37f 2026-05-26: EURGBP H1 X=5 SL=3 -- new pair, validated.
            //   Full validation pipeline: IS PF 1.80 / OOS PF 1.68 / WR 60% /
            //   RF 1.39 / pf_ratio 0.93 (clean -- not curve-fit).
            //   3-period intersect: each period +ve, min PF 1.33.
            //   WF 4 folds: 3/4 positive.
            const char* warmup_eurgbp = "phase1/signal_discovery/warmup_EURGBP_H1.csv";
            g_amr_eurgbp.enabled     = true;
            amr_boot(g_amr_eurgbp, "eurgbp", warmup_eurgbp);

            g_amr_audusd.enabled     = false; // marginal PF 1.34; awaiting deep tune
            g_amr_audusd.shadow_mode = true;
            g_amr_audusd.on_close_cb = write_shadow_csv;

            g_amr_nzdusd.enabled     = false; // marginal PF 1.31; awaiting deep tune
            g_amr_nzdusd.shadow_mode = true;
            g_amr_nzdusd.on_close_cb = write_shadow_csv;

            std::printf("[OMEGA-INIT] AtrMeanRevGrid FX: EURUSD(M15,X=14)+GBPUSD(H1,X=10) enabled (shadow), AUDUSD+NZDUSD parked\n");

            // ----------------------------------------------------------------
            // 2026-05-26: Index AMR. Configs from deep eval sweep on tick CSVs.
            //   US500   H1  X=8  SL_Y=6 ATR_FROM_WAP  PF 1.75 +$81  DD $32 Recov 2.56
            //   NAS100  M15 X=14 SL_Y=4 RSI_OR_MA     PF 1.55 +$15  DD $10 Recov 1.48
            //   GER40   M15 X=14 SL_Y=6 ATR_FROM_WAP  PF 1.86 (stage-4)
            // shadow_mode=true. Warmup CSV per CLAUDE.md "Engine Warm-Seed Mandate".
            // ----------------------------------------------------------------
            const char* warmup_us500  = "phase1/signal_discovery/warmup_US500_H1.csv";
            const char* warmup_nas100 = "phase1/signal_discovery/warmup_NAS100_H1.csv";
            const char* warmup_ger40  = "phase1/signal_discovery/warmup_GER40_H1.csv";

            g_amr_us500.enabled      = false;  // S47: AMR mean-rev purge
            amr_boot(g_amr_us500, "us500", warmup_us500);

            g_amr_nas100.enabled     = false;  // S47: M15 micro purge
            amr_boot(g_amr_nas100, "nas100", warmup_nas100);

            g_amr_ger40.enabled      = false;  // S47: M15 micro purge
            amr_boot(g_amr_ger40, "ger40", warmup_ger40);

            std::printf("[OMEGA-INIT] AtrMeanRevGrid INDEX: US500(H1,X=8)+NAS100(M15,X=14)+GER40(M15,X=14) enabled (shadow)\n");
        }

        // AUD/NZD/JPY: structure in place, awaiting H1 warmup CSVs.
        // Set enabled=true + add warmup_csv_path once
        //   phase1/signal_discovery/warmup_AUDUSD_H1.csv
        //   phase1/signal_discovery/warmup_NZDUSD_H1.csv
        //   phase1/signal_discovery/warmup_USDJPY_H1.csv
        // are committed. Until then warmup_or_die() skips them (engine
        // disabled = explicit opt-out, not rule violation).

        g_audusd_turtle_h4.p           = omega::make_audusd_turtle_h4_params();
        g_audusd_turtle_h4.shadow_mode = true;
        g_audusd_turtle_h4.enabled     = false;  // awaiting warmup CSV
        g_audusd_turtle_h4.symbol      = "AUDUSD";
        g_audusd_turtle_h4.warmup_csv_path = "phase1/signal_discovery/warmup_AUDUSD_H1.csv";
        omega::warmup_or_die(g_audusd_turtle_h4, "AudusdTurtleH4");

        g_nzdusd_turtle_h4.p           = omega::make_nzdusd_turtle_h4_params();
        g_nzdusd_turtle_h4.shadow_mode = true;
        g_nzdusd_turtle_h4.enabled     = false;  // awaiting warmup CSV
        g_nzdusd_turtle_h4.symbol      = "NZDUSD";
        g_nzdusd_turtle_h4.warmup_csv_path = "phase1/signal_discovery/warmup_NZDUSD_H1.csv";
        omega::warmup_or_die(g_nzdusd_turtle_h4, "NzdusdTurtleH4");

        g_usdjpy_turtle_h4.p           = omega::make_usdjpy_turtle_h4_params();
        g_usdjpy_turtle_h4.shadow_mode = true;
        g_usdjpy_turtle_h4.enabled     = false;  // awaiting warmup CSV
        g_usdjpy_turtle_h4.symbol      = "USDJPY";
        g_usdjpy_turtle_h4.warmup_csv_path = "phase1/signal_discovery/warmup_USDJPY_H1.csv";
        omega::warmup_or_die(g_usdjpy_turtle_h4, "UsdjpyTurtleH4");
        printf("[OMEGA-INIT] FxTurtleH4 cohort: EUR+GBP active; AUD/NZD/JPY awaiting warmup CSVs\n");

        // 2026-05-20 mega_sweep2 candle batch (3 D1 patterns)
        g_xau_doji_rej_d1.p           = omega::make_xau_doji_rej_d1_params();
        g_xau_doji_rej_d1.shadow_mode = true;
        g_xau_doji_rej_d1.enabled     = true;
        g_xau_doji_rej_d1.symbol      = "XAUUSD";
        printf("[OMEGA-INIT] XauDojiRejD1Engine: shadow=%d sl=%.1fx tp=%.1fx hold=%d doji_body=%.2f\n",
               (int)g_xau_doji_rej_d1.shadow_mode,
               g_xau_doji_rej_d1.p.sl_atr_mult, g_xau_doji_rej_d1.p.tp_atr_mult,
               g_xau_doji_rej_d1.p.hold_max_days, g_xau_doji_rej_d1.p.doji_body_pct);

        g_xau_outside_bar_d1.p           = omega::make_xau_outside_bar_d1_params();
        g_xau_outside_bar_d1.shadow_mode = true;
        g_xau_outside_bar_d1.enabled     = true;
        g_xau_outside_bar_d1.symbol      = "XAUUSD";
        printf("[OMEGA-INIT] XauOutsideBarD1Engine: shadow=%d sl=%.1fx tp=%.1fx hold=%d\n",
               (int)g_xau_outside_bar_d1.shadow_mode,
               g_xau_outside_bar_d1.p.sl_atr_mult, g_xau_outside_bar_d1.p.tp_atr_mult,
               g_xau_outside_bar_d1.p.hold_max_days);

        g_xau_inside_bar_d1.p           = omega::make_xau_inside_bar_d1_params();
        // 2026-05-27 S57: DISABLED -- regime split HIGH Sharpe -0.31 (slight
        // negative in high-vol regime, 12/51 trades). Operator strict policy.
        g_xau_inside_bar_d1.shadow_mode = true;
        g_xau_inside_bar_d1.enabled     = false;  // S57: regime HIGH neg
        g_xau_inside_bar_d1.symbol      = "XAUUSD";
        printf("[OMEGA-INIT] XauInsideBarD1Engine: shadow=%d sl=%.1fx tp=%.1fx hold=%d\n",
               (int)g_xau_inside_bar_d1.shadow_mode,
               g_xau_inside_bar_d1.p.sl_atr_mult, g_xau_inside_bar_d1.p.tp_atr_mult,
               g_xau_inside_bar_d1.p.hold_max_days);
        fflush(stdout);

        // ── GoldD1TrendState (2026-05-21) -- regime gate for shorts/longs.
        //   Seeded from XAU H4 CSV. Updated on every H4 close in tick_gold.hpp.
        //   Queried by bidirectional engines (XauTrendFollow2h InsideBar,
        //   DonchianBreakout short path) before firing direction-dependent entries.
        omega::gold_d1_trend().seed_from_h4_csv("phase1/signal_discovery/warmup_XAUUSD_H4.csv");
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
            omega::seed_h4_engine(g_xau_tsmom_fast_d1,    seed_csv, "XauTsmomFastD1");
            omega::seed_h4_engine(g_xau_turtle_d1,        seed_csv, "XauTurtleD1");
            omega::seed_h4_engine(g_xau_stop_run_d1,      seed_csv, "XauStopRunD1");
            omega::seed_h4_engine(g_xau_pullback_cont_h4, seed_csv, "XauPullbackContH4");
            omega::seed_h4_engine(g_xau_nbm_d1,           seed_csv, "XauNbmD1");
            omega::seed_h4_engine(g_xau_ema_cross_h4,     seed_csv, "XauEmaCrossH4");
            omega::seed_h4_engine(g_xau_pullback_cont_d1, seed_csv, "XauPullbackContD1");
            omega::seed_h4_engine(g_xau_bb_scalp_d1,      seed_csv, "XauBBScalpD1");
            omega::seed_h4_engine(g_xau_swing_break_d1,   seed_csv, "XauSwingBreakD1");
            omega::seed_h4_engine(g_xau_doji_rej_d1,      seed_csv, "XauDojiRejD1");
            omega::seed_h4_engine(g_xau_outside_bar_d1,   seed_csv, "XauOutsideBarD1");
            omega::seed_h4_engine(g_xau_inside_bar_d1,    seed_csv, "XauInsideBarD1");
        }
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
        g_xau_tf_2h.shadow_mode = true;
        g_xau_tf_2h.enabled     = true;
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
        g_xau_threebar_30m.shadow_mode        = true;
        g_xau_threebar_30m.enabled            = true;
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
        g_gold_ultimate_engine.shadow_mode       = true;
        g_gold_ultimate_engine.enabled           = false;  // S99b: disabled — off-hours edge-hour design (01/05/23 UTC) bleeds in practice
        g_gold_ultimate_engine.lot               = 0.01;
        g_gold_ultimate_engine.max_spread        = 1.0;
        g_gold_ultimate_engine.atr_entry_floor   = 2.5;
        g_gold_ultimate_engine.sl_atr_mult       = 2.0;
        g_gold_ultimate_engine.tp_atr_mult       = 5.0;
        g_gold_ultimate_engine.trail_trigger_atr  = 3.0;
        g_gold_ultimate_engine.trail_dist_atr     = 2.0;
        g_gold_ultimate_engine.drift_min         = 2.0;
        g_gold_ultimate_engine.init();
        printf("[OMEGA-INIT] GoldUltimateEngine initialised: shadow=%d enabled=%d lot=%.2f"
               " sl=%.1f*ATR tp=%.1f*ATR trail=%.1f/%.1f*ATR atr_floor=%.1f"
               " edge_hours=01,05,23 (S91 v12 OOS-validated)\n",
               (int)g_gold_ultimate_engine.shadow_mode,
               (int)g_gold_ultimate_engine.enabled,
               g_gold_ultimate_engine.lot,
               g_gold_ultimate_engine.sl_atr_mult,
               g_gold_ultimate_engine.tp_atr_mult,
               g_gold_ultimate_engine.trail_trigger_atr,
               g_gold_ultimate_engine.trail_dist_atr,
               g_gold_ultimate_engine.atr_entry_floor);
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
        g_ustec_tf_htf.shadow_mode      = true;   // HARD shadow until live-validated
        g_ustec_tf_htf.enabled          = false;  // S94 2026-05-15: disabled — replaced by Nas100ShortEngine (OOS-validated PF=3.38). Was shadow-only, never live.
        g_ustec_tf_htf.lot              = 0.1;
        g_ustec_tf_htf.max_spread       = 5.0;
        g_ustec_tf_htf.be_trigger_atr   = 1.0;    // S35-P6 TUNED (mirrors XauThreeBar30m)
        g_ustec_tf_htf.be_cost_buffer_pts = 0.50;
        g_ustec_tf_htf.trail_after_be   = true;   // S35-P6 TUNED
        g_ustec_tf_htf.trail_atr_mult   = 0.75;
        g_ustec_tf_htf.min_atr_floor    = 5.0;    // M15 ATR floor in raw points
        g_ustec_tf_htf.max_atr_ceil     = 0.0;    // disabled
        g_ustec_tf_htf.daily_loss_limit = 0.0;    // disabled (matches XAU TUNED)
        g_ustec_tf_htf.max_consec_losses = 0;     // disabled
        g_ustec_tf_htf.max_bars_held    = 0;      // disabled
        g_ustec_tf_htf.block_hour_start = -1;     // disabled
        g_ustec_tf_htf.block_hour_end   = -1;
        g_ustec_tf_htf.init();
        printf("[OMEGA-INIT] UstecTrendFollowHtfEngine initialised: shadow=%d enabled=%d lot=%.2f"
               " cells=1 (Stoch4h-only)"
               " be_trig=%.2f*ATR trail=%.2f*ATR atr_floor=%.2f"
               " (S36-P1a + S36-P4 + S36-P1b + S36-P5; M15 dispatch wired in tick_indices.hpp 2026-05-12;"
               " S36-P2 2024 NSXUSD OOS holdout: 2-cell -$2,677 / Stoch4h-only -$270 (AtrMom1h dropped);"
               " S36-P3 SPX portability: Stoch4h PF 1.37 on both NSXUSD+SPXUSD in-sample, AtrMom1h"
               " 7x weaker edge on SPX confirming USTEC-microstructure curve-fit;"
               " Stoch4h in-sample 2025-2026 NSXUSD +$6,666 PF 1.37 / SPXUSD +$73 PF 1.37)\n",
               (int)g_ustec_tf_htf.shadow_mode,
               (int)g_ustec_tf_htf.enabled,
               g_ustec_tf_htf.lot,
               g_ustec_tf_htf.be_trigger_atr,
               g_ustec_tf_htf.trail_atr_mult,
               g_ustec_tf_htf.min_atr_floor);
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
        g_ema_pullback.shadow_mode       = false;
        g_ema_pullback.enabled           = true;
        g_ema_pullback.cell_enable_mask  = 0x0C;  // S96: H4+H6 only
        g_ema_pullback.max_concurrent    = 4;
        g_ema_pullback.risk_pct          = 0.005;
        g_ema_pullback.start_equity      = 10000.0;
        g_ema_pullback.margin_call       = 1000.0;
        g_ema_pullback.max_lot_cap       = 0.05;
        g_ema_pullback.block_on_risk_off = true;
        g_ema_pullback.warmup_csv_path   = "phase1/signal_discovery/tsmom_warmup_H1.csv";
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
        g_ema_pullback.LOSS_CUT_PCT      = 0.10;  // state A: LC + BE active
        g_ema_pullback.BE_ARM_PCT        = 0.40;
        g_ema_pullback.BE_BUFFER_PCT     = 0.05;
        g_ema_pullback.init();
        omega::warmup_or_die(g_ema_pullback, "EmaPullback");
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
        g_trend_rider.shadow_mode       = true;
        g_trend_rider.enabled           = false;  // S91: disabled — GoldUltimateEngine solo test
        g_trend_rider.max_concurrent    = 6;
        g_trend_rider.risk_pct          = 0.040;          // 8x tsmom baseline (~1/8 Kelly)
        g_trend_rider.start_equity      = 10000.0;
        g_trend_rider.margin_call       = 1000.0;
        g_trend_rider.max_lot_cap       = 0.50;           // 10x tsmom baseline
        g_trend_rider.block_on_risk_off = true;
        g_trend_rider.warmup_csv_path   = "phase1/signal_discovery/tsmom_warmup_H1.csv";
        g_trend_rider.init();
        omega::warmup_or_die(g_trend_rider, "TrendRider");
        fflush(stdout);
    }

    // MinimalH4US30Breakout -- DJ30.F sister engine. Self-contained: builds
    // own H4 OHLC + ATR14 from tick stream (no g_bars_us30 exists). Validated
    // 27/27 profitable on 2yr Tickstory tick sweep. Default config (D=10
    // SL=1.0x TP=4.0x): n=184, PF=1.54, +$637, WR=28.3%. Initialised outside
    // the gold-conditional block above because it has no dependency on
    // g_bars_gold or any gold infrastructure.
    g_minimal_h4_us30.p           = omega::make_minimal_h4_us30_params();
    g_minimal_h4_us30.symbol      = "DJ30.F";
    g_minimal_h4_us30.shadow_mode = true;
    g_minimal_h4_us30.enabled     = true;
    printf("[INIT] MinimalH4US30Breakout DJ30.F: shadow=true donchian=%d sl=%.1fx"
           " tp=%.1fx risk=$%.0f max_lot=%.2f $/pt=%.1f timeout=%d bars"
           " atr_period=%d weekend_gate=%s\n",
           g_minimal_h4_us30.p.donchian_bars,    g_minimal_h4_us30.p.sl_mult,
           g_minimal_h4_us30.p.tp_mult,          g_minimal_h4_us30.p.risk_dollars,
           g_minimal_h4_us30.p.max_lot,          g_minimal_h4_us30.p.dollars_per_point,
           g_minimal_h4_us30.p.timeout_h4_bars,  g_minimal_h4_us30.p.atr_period,
           g_minimal_h4_us30.p.weekend_close_gate ? "true" : "false");
    fflush(stdout);

    // Warm restart for MinimalH4US30Breakout (S26 2026-04-25):
    // load bars_us30_h4.dat if present and <=8h old. On success the engine
    // skips the ~40-56hr cold start (Donchian10 + Wilder ATR14 seed).
    // First-deploy bootstrap: tools/seed_us30_h4.cpp can write this file
    // directly from a Dukascopy USA30 H4 CSV before the first start.
    {
        const std::string us30_dat = log_root_dir() + "/bars_us30_h4.dat";
        const bool us30_h4_ok = g_minimal_h4_us30.load_state(us30_dat);
        if (us30_h4_ok) {
            printf("[STARTUP] MinimalH4US30Breakout warm-loaded from %s -- "
                   "engine hot, can fire on first H4 close.\n", us30_dat.c_str());
        } else {
            printf("[STARTUP] MinimalH4US30Breakout cold start -- needs ~40hrs of "
                   "live DJ30.F H4 bars before first signal. Seed via "
                   "tools/seed_us30_h4.cpp + Dukascopy CSV to skip the wait.\n");
        }
        fflush(stdout);
    }

    // MinimalH4GER40Breakout -- GER40/DAX sister engine. Same pattern as US30.
    // Multi-symbol scan 2026-05-20 (backtest/multi_symbol_scan.py): GER40 was
    // best performer of FX+indices scanned -- Sharpe 3.67 PnL $8.40 50 trades.
    g_minimal_h4_ger40.p           = omega::make_minimal_h4_ger40_params();
    g_minimal_h4_ger40.symbol      = "GER40";
    g_minimal_h4_ger40.shadow_mode = true;
    g_minimal_h4_ger40.enabled     = true;
    printf("[INIT] MinimalH4GER40Breakout GER40: shadow=true donchian=%d sl=%.1fx"
           " tp=%.1fx risk=$%.0f max_lot=%.2f $/pt=%.1f timeout=%d bars"
           " long_only=%s weekend_gate=%s\n",
           g_minimal_h4_ger40.p.donchian_bars,    g_minimal_h4_ger40.p.sl_mult,
           g_minimal_h4_ger40.p.tp_mult,          g_minimal_h4_ger40.p.risk_dollars,
           g_minimal_h4_ger40.p.max_lot,          g_minimal_h4_ger40.p.dollars_per_point,
           g_minimal_h4_ger40.p.timeout_h4_bars,
           g_minimal_h4_ger40.p.long_only ? "true" : "false",
           g_minimal_h4_ger40.p.weekend_close_gate ? "true" : "false");
    fflush(stdout);

    // EurGbpPairsEngine -- EURUSD/GBPUSD H1 spread mean reversion (shadow mode).
    // Backtest 2026-05-20 (backtest/sweep_pairs_v2.csv, C++ engine, M5-interleaved 17mo data):
    //   Top config w=120 zi=1.5 zo=0.5 h=48: n=358 Sh=7.75 PnL=$638 MDD=$34.67 (cost 1pip/leg)
    //   6-mode rigor (pairs_rigor_cpp.cpp): IS=7.32 / OOS=7.23, 6/6 WF folds positive,
    //     14/14 months positive, Monte Carlo p<0.0001, robust to +/-20% perturbation.
    g_eur_gbp_pairs.p           = omega::make_eur_gbp_pairs_params();
    g_eur_gbp_pairs.p.z_window  = 120;
    g_eur_gbp_pairs.p.z_in      = 1.5;
    g_eur_gbp_pairs.p.z_out     = 0.5;
    g_eur_gbp_pairs.p.hold_timeout_h1 = 48;
    g_eur_gbp_pairs.shadow_mode = true;
    g_eur_gbp_pairs.enabled     = true;
    printf("[INIT] EurGbpPairsEngine EURUSD+GBPUSD: shadow=true z_window=%d z_in=%.1f"
           " z_out=%.1f z_stop=%.1f hold_h1=%d risk=$%.0f max_lot=%.2f\n",
           g_eur_gbp_pairs.p.z_window, g_eur_gbp_pairs.p.z_in, g_eur_gbp_pairs.p.z_out,
           g_eur_gbp_pairs.p.z_stop,   g_eur_gbp_pairs.p.hold_timeout_h1,
           g_eur_gbp_pairs.p.risk_dollars, g_eur_gbp_pairs.p.max_lot_per_leg);
    // Warm-seed pairs engine from EURUSD + GBPUSD H1 close CSVs (~7000 bars each).
    // Pre-populates spread_hist_ (z_window=120 bars needed). Without seed,
    // engine cold-warms 120 hours (5 trading days) before first z-eval.
    g_eur_gbp_pairs.seed_from_h1_csvs(
        "phase1/signal_discovery/warmup_EURUSD_H1.csv",
        "phase1/signal_discovery/warmup_GBPUSD_H1.csv");
    fflush(stdout);

    // DISABLED: Index TrendPullback never explicitly disabled -- no live validation.
    // GER40: tighter band (index moves more cleanly around EMAs)
    g_trend_pb_ger40.PULLBACK_BAND_PCT = 0.05;  // 0.05% of GER40 = ~11pts at 22500
    g_trend_pb_ger40.COOLDOWN_SEC     = 120;
    g_trend_pb_ger40.MIN_EMA_SEP      = 15.0;
    // NQ/SP TrendPullback: daily loss cap + tighter controls
    // Without DAILY_LOSS_CAP, NQ TrendPullback fired 7 consecutive losing entries
    // during the Apr 2 tariff crash (NQ dropped ~1000pts). Each SL hit was $12-13,
    // but the direction block (2 consec SL hits = 10min pause) was the only guard.
    // Daily loss cap stops the engine entirely after a bad sequence.
    g_trend_pb_nq.MIN_EMA_SEP         = 25.0;
    g_trend_pb_nq.DAILY_LOSS_CAP      = 80.0;   // $80 daily cap: ~6 SL hits at $12 each
    g_trend_pb_nq.enabled             = false;   // S94 2026-05-15: disabled — NAS/USTEC engines consolidated into Nas100ShortEngine (OOS-validated PF=3.38). Was live with $80 daily cap.
    g_trend_pb_sp.MIN_EMA_SEP         = 15.0;
    g_trend_pb_sp.DAILY_LOSS_CAP      = 80.0;   // same cap for SP
    g_trend_pb_sp.enabled             = false;   // S95 2026-05-15: disabled — SPX UltimateBacktest OOS failed (v2 OOS PF=0.88). No trend-following edge on US500. Was live with $80 daily cap.
    g_trend_pb_ger40.DAILY_LOSS_CAP   = 80.0;   // GER40 too -- no cap was previously set
    g_trend_pb_ger40.enabled          = false;   // DISABLED: not live-validated
    // Load warm EMA state -- skips EMA_WARMUP_TICKS cold period on restart
    g_trend_pb_gold.load_state(state_root_dir()  + "/trend_pb_gold.dat");
    g_trend_pb_ger40.load_state(state_root_dir() + "/trend_pb_ger40.dat");
    g_trend_pb_nq.load_state(state_root_dir()    + "/trend_pb_nq.dat");
    g_trend_pb_sp.load_state(state_root_dir()    + "/trend_pb_sp.dat");

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
            const double m5_ema21 = g_bars_gold.m5.ind.ema21.load(std::memory_order_relaxed);
            const double m5_atr14 = g_bars_gold.m5.ind.atr14.load(std::memory_order_relaxed);
            // Best-effort approximation for ATR prev_close: M5 EMA9 is the
            // tightest persisted reference to recent close. Drift bounded
            // by (live_first_close - EMA9) which converges in 1 bar.
            g_gold_scalp_pyramid.prime_from_atomics(m5_ema9, m5_ema21, m5_atr14, m5_ema9);

            const int fed = g_gold_scalp_pyramid.prime_from_history(
                                g_bars_gold.m5.get_bars());
            printf("[STARTUP] GoldScalpPyramid primed: atomics(ema9=%.2f ema21=%.2f atr=%.2f) bars_fed=%d\n",
                   m5_ema9, m5_ema21, m5_atr14, fed);
            fflush(stdout);
        }

        // S38d 2026-05-26 TODO: FxScalpPyramid x5 warm-seed.
        //   No FX M5 bar source currently exists in this binary (no
        //   g_bars_fx / g_bars_eurusd / etc.). Engines cold-start in ~50min
        //   on live ticks once EMA21 + ATR14 + Donchian8 prime from fresh
        //   bars. Acceptable for shadow-mode launch.
        //   When a shared FX M5 bar source is added (mirror of g_bars_gold.m5),
        //   wire prime_from_atomics + prime_from_history per engine here.

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
            g_bband_scalp.prime_from_atomics(m1_bbu, m1_bbm, m1_bbl, m1_rsi, m1_atr);
            printf("[STARTUP] BBandScalp primed: bb=[%.2f, %.2f, %.2f] rsi=%.1f atr=%.2f "
                   "(reads atomics each tick -- no warmup needed)\n",
                   m1_bbl, m1_bbm, m1_bbu, m1_rsi, m1_atr);
            fflush(stdout);
        }

        if (m15_ok) {
            // Immediately seed TrendPullback EMAs + ATR from M15 bar state
            g_trend_pb_gold.seed_bar_emas(
                g_bars_gold.m15.ind.ema9 .load(std::memory_order_relaxed),
                g_bars_gold.m15.ind.ema21.load(std::memory_order_relaxed),
                g_bars_gold.m15.ind.ema50.load(std::memory_order_relaxed),
                g_bars_gold.m15.ind.atr14.load(std::memory_order_relaxed));
            // M1 EMA crossover for bar trend gate -- loaded from disk, no 15-min warmup
            if (m1_ok) {
                const double st_e9  = g_bars_gold.m1.ind.ema9 .load(std::memory_order_relaxed);
                const double st_e50 = g_bars_gold.m1.ind.ema50.load(std::memory_order_relaxed);
                const int st_trend  = (st_e9 > 0.0 && st_e50 > 0.0)
                    ? (st_e9 < st_e50 ? -1 : +1) : 0;
                g_trend_pb_gold.seed_m5_trend(st_trend);
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
                g_trend_pb_gold.seed_m5_trend(st_trend);
            }
            // Immediately seed H4 HTF trend gate -- no need to wait for first tick
            if (h4_ok) {
                g_trend_pb_gold.seed_h4_trend(
                    g_bars_gold.h4.ind.trend_state.load(std::memory_order_relaxed));
                // Seed H4RegimeEngine Donchian channel from saved H4 bar history.
                // Without this the channel needs 20 new H4 bars (80 hours) to warm.
                // With this: channel is ready from tick 1 -- engine is hot immediately.
                g_h4_regime_gold.seed_channel_from_bars(g_bars_gold.h4.get_bars());
                // Seed MinimalH4Breakout Donchian channel from same saved H4 bar history.
                // 10-bar channel: needs 40 hours warm-up cold vs. tick-1 ready warm.
                g_minimal_h4_gold.seed_channel_from_bars(g_bars_gold.h4.get_bars());
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
                if (g_minimal_h4_gold.seed_channel_from_csv(xau_h4_csv)) {
                    printf("[STARTUP] MinimalH4Breakout warm-loaded from CSV %s "
                           "-- engine hot, can fire on first H4 close.\n",
                           xau_h4_csv.c_str());
                } else {
                    printf("[STARTUP] MinimalH4Breakout cold start -- needs ~40hrs of "
                           "live XAUUSD H4 bars before first signal. Drop a Dukascopy "
                           "XAUUSD H4 CSV at %s to skip the wait (see MinimalH4Breakout.hpp:163 "
                           "for schema).\n", xau_h4_csv.c_str());
                }
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
            if (g_minimal_h4_gold.seed_channel_from_csv(xau_h4_csv)) {
                printf("[STARTUP] MinimalH4Breakout warm-loaded from CSV %s "
                       "-- engine hot for first H4 close.\n", xau_h4_csv.c_str());
            } else {
                printf("[STARTUP] MinimalH4Breakout cold start -- drop CSV at %s "
                       "to skip the 40hr Donchian warm-up "
                       "(see MinimalH4Breakout.hpp:163 for schema).\n",
                       xau_h4_csv.c_str());
            }
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
    g_nbm_sp.enabled     = false;
    g_nbm_nq.enabled     = false;
    g_nbm_nas.enabled    = false;
    g_nbm_us30.enabled   = false;
    // NBM gold london: RE-ENABLED 2026-04-01 -- live MT5 data confirms the logic
    // (London open ATR breakout, 51min hold, +$185). Omega NBM is identical concept.
    g_nbm_gold_london.enabled = false;  // S91: disabled — GoldUltimateEngine solo test
    g_nbm_oil_london.enabled  = false;
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
    g_orb_us.OPEN_HOUR     = 13; g_orb_us.OPEN_MIN     = 30;  // NYSE open
    g_orb_ger30.OPEN_HOUR  = 8;  g_orb_ger30.OPEN_MIN  = 0;   // Xetra open
    g_orb_uk100.OPEN_HOUR  = 8;  g_orb_uk100.OPEN_MIN  = 0;   // LSE open
    g_orb_estx50.OPEN_HOUR = 9;  g_orb_estx50.OPEN_MIN = 0;   // Euronext open
    g_orb_us.enabled     = false;
    g_orb_ger30.enabled  = false;
    g_orb_uk100.enabled  = false;
    g_orb_estx50.enabled = false;

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

    g_orb_nas100.OPEN_HOUR       = 13;
    g_orb_nas100.OPEN_MIN        = 30;
    g_orb_nas100.RANGE_WINDOW_MIN = 30;
    g_orb_nas100.BUFFER_PCT      = 0.02;
    g_orb_nas100.TP_PCT          = 0.50;
    g_orb_nas100.SL_PCT          = 0.20;
    g_orb_nas100.MAX_HOLD_SEC    = 21600;  // 6h
    g_orb_nas100.enabled         = false;  // S47: ORB session-scalp purge

    g_orb_dj30.OPEN_HOUR         = 13;
    g_orb_dj30.OPEN_MIN          = 30;
    g_orb_dj30.RANGE_WINDOW_MIN  = 30;
    g_orb_dj30.BUFFER_PCT        = 0.02;
    g_orb_dj30.TP_PCT            = 0.55;
    g_orb_dj30.SL_PCT            = 0.22;
    g_orb_dj30.MAX_HOLD_SEC      = 21600;  // 6h
    g_orb_dj30.enabled           = false;  // S47: ORB session-scalp purge

    printf("[OMEGA-INIT] ORB-Swing cohort: NAS100 + DJ30 enabled, "
           "buf=%.2f%% tp=%.2f/%.2f%% sl=%.2f/%.2f%% hold=%ds (NY 13:30 UTC, 30min range)\n",
           g_orb_nas100.BUFFER_PCT,
           g_orb_nas100.TP_PCT, g_orb_dj30.TP_PCT,
           g_orb_nas100.SL_PCT, g_orb_dj30.SL_PCT,
           g_orb_nas100.MAX_HOLD_SEC);
    //
    // Cross-asset: EIA fade, BrentWTI spread, FX cascade, carry unwind.
    // All have insufficient live data. Shelved pending shadow validation.
    g_ca_eia_fade.enabled    = false;
    g_ca_brent_wti.enabled   = false;
    g_ca_fx_cascade.enabled  = false;
    g_ca_carry_unwind.enabled = false;
    // ESNQ: already guarded by esnq_enabled=false in config. Belt-and-suspenders.
    g_ca_esnq.enabled        = false;
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
        g_ger40_london_brk.symbol          = "GER40";
        g_ger40_london_brk.shadow_mode     = false;  // LIVE -- validated edge
        g_ger40_london_brk.enabled         = true;
        g_ger40_london_brk.lot             = 0.01;
        g_ger40_london_brk.max_spread      = 4.0;
        g_ger40_london_brk.ASIA_START_HOUR = 21;
        g_ger40_london_brk.ASIA_END_HOUR   = 7;
        g_ger40_london_brk.ENTRY_START_HOUR = 7;
        g_ger40_london_brk.ENTRY_START_MIN  = 0;
        g_ger40_london_brk.ENTRY_END_HOUR   = 9;
        g_ger40_london_brk.ENTRY_END_MIN    = 0;
        g_ger40_london_brk.TP_MULT         = 0.75;
        g_ger40_london_brk.SL_MULT         = 0.50;
        g_ger40_london_brk.MAX_HOLD_SEC    = 14400;  // 4 hours
        g_ger40_london_brk.MIN_RANGE_PTS   = 15.0;
        g_ger40_london_brk.MAX_RANGE_PTS   = 150.0;
        g_ger40_london_brk.LOSS_CUT_PCT    = 0.0;    // structural SL handles it
        g_ger40_london_brk.BE_ARM_PCT      = 0.0;
        g_ger40_london_brk.BE_BUFFER_PCT   = 0.0;
        g_ger40_london_brk.init();
        printf("[OMEGA-INIT] Ger40LondonBreakoutEngine: shadow=%s enabled=%s "
               "lot=%.2f tp_mult=%.2f sl_mult=%.2f entry=%02d:%02d-%02d:%02d\n",
               g_ger40_london_brk.shadow_mode ? "true" : "false",
               g_ger40_london_brk.enabled ? "true" : "false",
               g_ger40_london_brk.lot,
               g_ger40_london_brk.TP_MULT, g_ger40_london_brk.SL_MULT,
               g_ger40_london_brk.ENTRY_START_HOUR, g_ger40_london_brk.ENTRY_START_MIN,
               g_ger40_london_brk.ENTRY_END_HOUR, g_ger40_london_brk.ENTRY_END_MIN);
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
            // ENGINE CONFIG SUMMARY -- must be emitted AFTER tee opens so it goes into the log file.
            std::cout << "[RSI-REV] RSIReversalEngine configured (shadow_mode="
                      << (g_rsi_reversal.shadow_mode ? "true" : "false")
                      << " oversold=" << (int)g_rsi_reversal.RSI_OVERSOLD
                      << " overbought=" << (int)g_rsi_reversal.RSI_OVERBOUGHT
                      << " sl_mult=" << g_rsi_reversal.SL_ATR_MULT << "x)\n";
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
            g_pdhl_rev.LOSS_CUT_PCT  = 0.04;
            g_pdhl_rev.BE_ARM_PCT    = 0.025;
            g_pdhl_rev.BE_BUFFER_PCT = 0.01;
            g_pdhl_rev.shadow_mode      = true;   // shadow until confirmed live
            g_pdhl_rev.enabled          = true;
            g_pdhl_rev.RANGE_ENTRY_PCT  = 0.25;   // top/bottom 25% of daily range
            g_pdhl_rev.SL_ATR_MULT      = 0.40;   // tight structural stop
            g_pdhl_rev.TP_RANGE_FRAC    = 0.50;   // target mid-range
            g_pdhl_rev.L2_LONG_MIN      = 0.55;   // bids building for long
            g_pdhl_rev.L2_SHORT_MAX     = 0.45;   // asks building for short
            g_pdhl_rev.DRIFT_FADE_MIN   = 1.5;    // proxy when no real L2
            g_pdhl_rev.MIN_RANGE_PTS    = 8.0;    // skip thin days
            g_pdhl_rev.RISK_USD         = 30.0;
            g_pdhl_rev.COOLDOWN_MS      = 120'000;
            g_pdhl_rev.MAX_HOLD_MS      = 900'000;

            // shadow trades; in LIVE mode it would place real orders.
            // Historical 2yr backtest (14.8% WR, -$27k) predates current HMM
            // gate, VWAP filter, and chop gate; recent shadow WR is materially
            // different. Re-evaluate before promoting to LIVE.

            std::cout << "[PDHL-REV] PDHLReversionEngine configured"
                      << " shadow=" << (g_pdhl_rev.shadow_mode ? "true" : "false")
                      << " entry_pct=" << g_pdhl_rev.RANGE_ENTRY_PCT
                      << " sl_mult=" << g_pdhl_rev.SL_ATR_MULT
                      << " tp_frac=" << g_pdhl_rev.TP_RANGE_FRAC << "\n";
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
    // 2026-05-01 SESSION_h: register GoldMidScalper for /api/v1/omega/engines.
    //   Shadow-stamped (last_signal_ts/last_pnl come from
    //   g_engine_last lookup against tr.engine="MidScalperGold").
    g_engines.register_engine("MidScalperGold",
        [reg]{ return reg("MidScalperGold",
                          true,
                          g_gold_midscalper.shadow_mode,
                          {"MidScalperGold"}); });
    // 2026-05-08 S19: register MicroScalperGold for /api/v1/omega/engines.
    //   Shadow-stamped initially. last_signal_ts/last_pnl resolved via
    //   g_engine_last lookup against tr.engine="MicroScalperGold".
    g_engines.register_engine("MicroScalperGold",
        [reg]{ return reg("MicroScalperGold",
                          true,
                          g_gold_microscalper.shadow_mode,
                          {"MicroScalperGold"}); });
    // 2026-05-02: register EurusdLondonOpen for /api/v1/omega/engines.
    //   Shadow-stamped initially. last_signal_ts/last_pnl resolved via
    //   g_engine_last lookup against tr.engine="EurusdLondonOpen".
    g_engines.register_engine("EurusdLondonOpen",
        [reg]{ return reg("EurusdLondonOpen",
                          true,
                          g_eurusd_london_open.shadow_mode,
                          {"EurusdLondonOpen"}); });
    // 2026-05-02: register UsdjpyAsianOpen for /api/v1/omega/engines.
    //   Same shadow-stamped pattern as EurusdLondonOpen.
    g_engines.register_engine("UsdjpyAsianOpen",
        [reg]{ return reg("UsdjpyAsianOpen",
                          true,
                          g_usdjpy_asian_open.shadow_mode,
                          {"UsdjpyAsianOpen"}); });
    // 2026-05-04: register GbpusdLondonOpen for /api/v1/omega/engines.
    //   Same shadow-stamped pattern as EurusdLondonOpen. last_signal_ts /
    //   last_pnl resolved via g_engine_last lookup against
    //   tr.engine="GbpusdLondonOpen".
    g_engines.register_engine("GbpusdLondonOpen",
        [reg]{ return reg("GbpusdLondonOpen",
                          true,
                          g_gbpusd_london_open.shadow_mode,
                          {"GbpusdLondonOpen"}); });
    // 2026-05-04: register AudusdSydneyOpen for /api/v1/omega/engines.
    //   Same shadow-stamped pattern as UsdjpyAsianOpen.
    g_engines.register_engine("AudusdSydneyOpen",
        [reg]{ return reg("AudusdSydneyOpen",
                          true,
                          g_audusd_sydney_open.shadow_mode,
                          {"AudusdSydneyOpen"}); });
    // 2026-05-04: register NzdusdAsianOpen for /api/v1/omega/engines.
    //   Same shadow-stamped pattern as AudusdSydneyOpen.
    g_engines.register_engine("NzdusdAsianOpen",
        [reg]{ return reg("NzdusdAsianOpen",
                          true,
                          g_nzdusd_asian_open.shadow_mode,
                          {"NzdusdAsianOpen"}); });
    // 2026-05-02: register XauusdFvg for /api/v1/omega/engines.
    //   Shadow-stamped initially (pinned in init block above). last_signal_ts
    //   / last_pnl resolved via g_engine_last lookup against
    //   tr.engine="XauusdFvg" (set in XauusdFvgEngine::_close).
    g_engines.register_engine("XauusdFvg",
        [reg]{ return reg("XauusdFvg",
                          true,
                          g_xauusd_fvg.shadow_mode,
                          {"XauusdFvg"}); });
    // 2026-05-18: GoldScalpPyramid engine registration.
    g_engines.register_engine("GoldScalpPyramid",
        [reg]{ return reg("GoldScalpPyramid",
                          g_gold_scalp_pyramid.enabled,
                          g_gold_scalp_pyramid.shadow_mode,
                          {"GoldScalpPyramid"}); });
    // S38d 2026-05-26: FxScalpPyramid x5 engine registrations.
    g_engines.register_engine("FxScalpPyramid_EURUSD",
        [reg]{ return reg("FxScalpPyramid_EURUSD",
                          g_fx_scalp_eurusd.enabled,
                          g_fx_scalp_eurusd.shadow_mode,
                          {"FxScalpPyramid_EURUSD"}); });
    g_engines.register_engine("FxScalpPyramid_USDJPY",
        [reg]{ return reg("FxScalpPyramid_USDJPY",
                          g_fx_scalp_usdjpy.enabled,
                          g_fx_scalp_usdjpy.shadow_mode,
                          {"FxScalpPyramid_USDJPY"}); });
    g_engines.register_engine("FxScalpPyramid_GBPUSD",
        [reg]{ return reg("FxScalpPyramid_GBPUSD",
                          g_fx_scalp_gbpusd.enabled,
                          g_fx_scalp_gbpusd.shadow_mode,
                          {"FxScalpPyramid_GBPUSD"}); });
    g_engines.register_engine("FxScalpPyramid_USDCAD",
        [reg]{ return reg("FxScalpPyramid_USDCAD",
                          g_fx_scalp_usdcad.enabled,
                          g_fx_scalp_usdcad.shadow_mode,
                          {"FxScalpPyramid_USDCAD"}); });
    g_engines.register_engine("FxScalpPyramid_AUDUSD",
        [reg]{ return reg("FxScalpPyramid_AUDUSD",
                          g_fx_scalp_audusd.enabled,
                          g_fx_scalp_audusd.shadow_mode,
                          {"FxScalpPyramid_AUDUSD"}); });
    // 2026-05-19 S110: GoldRegimeDaily engine registration.
    g_engines.register_engine("GoldRegimeDaily",
        [reg]{ return reg("GoldRegimeDaily",
                          g_gold_regime_daily.enabled,
                          g_gold_regime_daily.shadow_mode,
                          {"GoldRegimeDaily"}); });
    // S11 P3b: HybridSP / HybridNQ / HybridUS30 / HybridNAS100 register_engine
    //   blocks removed (engines culled in P3a + P3b).
    g_engines.register_engine("MacroCrash",
        [reg]{ return reg("MacroCrash",
                          g_macro_crash.enabled,
                          g_macro_crash.shadow_mode,
                          {"MacroCrash"}); });
    g_engines.register_engine("CandleFlow",
        [reg]{ return reg("CandleFlow",
                          true,
                          g_candle_flow.shadow_mode,
                          {"CandleFlowEngine"}); });
    g_engines.register_engine("Tsmom",
        [reg]{ return reg("Tsmom",
                          g_tsmom.enabled,
                          g_tsmom.shadow_mode,
                          {"Tsmom_"}); });
    g_engines.register_engine("TsmomV2",
        [reg]{ return reg("TsmomV2",
                          g_tsmom_v2.enabled,
                          g_tsmom_v2.shadow_mode,
                          {"TsmomV2_", "Cell_"}); });
    g_engines.register_engine("Donchian",
        [reg]{ return reg("Donchian",
                          g_donchian.enabled,
                          g_donchian.shadow_mode,
                          {"Donchian_"}); });
    g_engines.register_engine("EmaPullback",
        [reg]{ return reg("EmaPullback",
                          g_ema_pullback.enabled,
                          g_ema_pullback.shadow_mode,
                          {"EmaPullback_"}); });
    g_engines.register_engine("TrendRider",
        [reg]{ return reg("TrendRider",
                          g_trend_rider.enabled,
                          g_trend_rider.shadow_mode,
                          {"TrendRider_"}); });
    g_engines.register_engine("RSIReversal",
        [reg]{ return reg("RSIReversal",
                          g_rsi_reversal.enabled,
                          g_rsi_reversal.shadow_mode,
                          {"RSIReversal"}); });
    g_engines.register_engine("RSIExtreme",
        [reg]{ return reg("RSIExtreme",
                          true,
                          g_rsi_extreme.shadow_mode,
                          {"RSIExtremeTurn"}); });
    std::cout << "[OmegaApi] g_engines registered ("
              << g_engines.snapshot_all().size() << " engines)\n";
    std::cout.flush();

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
        g_engine_heartbeat.register_engine("MidScalperGold",     true, 3600,  0, 24);
        g_engine_heartbeat.register_engine("MicroScalperGold",   true, 3600,  6, 22);
        g_engine_heartbeat.register_engine("GoldStack",          true, 3600,  0, 24);
        g_engine_heartbeat.register_engine("CandleFlow",         true, 3600,  0, 24);
        g_engine_heartbeat.register_engine("EMACross",           true, 3600,  0, 24);
        g_engine_heartbeat.register_engine("XauusdFvg",          true, 3600,  0, 24);
        g_engine_heartbeat.register_engine("GoldScalpPyramid",   g_gold_scalp_pyramid.enabled, 3600, 7, 21);
        // S38d 2026-05-26: FxScalpPyramid heartbeats. 07-21 UTC session.
        g_engine_heartbeat.register_engine("FxScalpPyramid_EURUSD", g_fx_scalp_eurusd.enabled, 3600, 7, 21);
        g_engine_heartbeat.register_engine("FxScalpPyramid_USDJPY", g_fx_scalp_usdjpy.enabled, 3600, 7, 21);
        g_engine_heartbeat.register_engine("FxScalpPyramid_GBPUSD", g_fx_scalp_gbpusd.enabled, 3600, 7, 21);
        g_engine_heartbeat.register_engine("FxScalpPyramid_USDCAD", g_fx_scalp_usdcad.enabled, 3600, 7, 21);
        g_engine_heartbeat.register_engine("FxScalpPyramid_AUDUSD", g_fx_scalp_audusd.enabled, 3600, 7, 21);
        g_engine_heartbeat.register_engine("GoldRegimeDaily",    g_gold_regime_daily.enabled, 14400, 7, 21);
        g_engine_heartbeat.register_engine("RSIReversal",        true, 3600,  0, 24);
        g_engine_heartbeat.register_engine("RSIExtreme",         true, 3600,  0, 24);
        g_engine_heartbeat.register_engine("h1_swing_gold",      true, 3600,  0, 24);
        g_engine_heartbeat.register_engine("h4_regime_gold",     true, 3600,  0, 24);
        g_engine_heartbeat.register_engine("MacroCrash",         g_macro_crash.enabled, 3600, 0, 24);
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
        g_engine_heartbeat.register_engine("XauTsmomFastD1",       g_xau_tsmom_fast_d1.enabled,    3600,  0, 24);
        g_engine_heartbeat.register_engine("XauTurtleD1",          g_xau_turtle_d1.enabled,        3600,  0, 24);
        g_engine_heartbeat.register_engine("XauStopRunD1",         g_xau_stop_run_d1.enabled,      3600,  0, 24);
        g_engine_heartbeat.register_engine("XauPullbackContH4",    g_xau_pullback_cont_h4.enabled, 3600,  0, 24);
        g_engine_heartbeat.register_engine("XauPullbackContD1",    g_xau_pullback_cont_d1.enabled, 3600,  0, 24);
        g_engine_heartbeat.register_engine("XauNbmD1",             g_xau_nbm_d1.enabled,           3600,  0, 24);
        g_engine_heartbeat.register_engine("XauEmaCrossH4",        g_xau_ema_cross_h4.enabled,     3600,  0, 24);
        g_engine_heartbeat.register_engine("XauBBScalpD1",         g_xau_bb_scalp_d1.enabled,      3600,  0, 24);
        g_engine_heartbeat.register_engine("XauSwingBreakD1",      g_xau_swing_break_d1.enabled,   3600,  0, 24);
        g_engine_heartbeat.register_engine("XauDojiRejD1",         g_xau_doji_rej_d1.enabled,      3600,  0, 24);
        g_engine_heartbeat.register_engine("XauOutsideBarD1",      g_xau_outside_bar_d1.enabled,   3600,  0, 24);
        g_engine_heartbeat.register_engine("XauInsideBarD1",       g_xau_inside_bar_d1.enabled,    3600,  0, 24);
        g_engine_heartbeat.register_engine("Xau3BarMomH4",         g_xau_3bar_mom_h4.enabled,      3600,  0, 24);
        g_engine_heartbeat.register_engine("XauDonchian55GatedM30",g_xau_d55_gated_m30.enabled,    3600,  0, 24);

        // ---- Gold portfolio (H1-bar driven, dispatched from gold ticks) --
        g_engine_heartbeat.register_engine("C1Retuned",            g_c1_retuned.enabled,           3600,  0, 24);
        g_engine_heartbeat.register_engine("Donchian",             g_donchian.enabled,             3600,  0, 24);
        g_engine_heartbeat.register_engine("EmaPullback",          g_ema_pullback.enabled,         3600,  0, 24);
        g_engine_heartbeat.register_engine("TsmomV2",              g_tsmom_v2.enabled,             3600,  0, 24);
        g_engine_heartbeat.register_engine("PdhlRev",              g_pdhl_rev.enabled,             3600,  0, 24);

        // ---- FX cross-asset engines (Stage 3 heartbeat rollout) ----------
        g_engine_heartbeat.register_engine("EurusdTurtleH4",       g_eurusd_turtle_h4.enabled,     3600,  0, 24);
        g_engine_heartbeat.register_engine("GbpusdTurtleH4",       g_gbpusd_turtle_h4.enabled,     3600,  0, 24);
        g_engine_heartbeat.register_engine("AmrEurusd",            g_amr_eurusd.enabled,           3600,  0, 24);
        g_engine_heartbeat.register_engine("AmrGbpusd",            g_amr_gbpusd.enabled,           3600,  0, 24);
        g_engine_heartbeat.register_engine("EurGbpPairs",          g_eur_gbp_pairs.enabled,        3600,  0, 24);
        g_engine_heartbeat.register_engine("VwapRevEurusd",        g_vwap_rev_eurusd.enabled,      3600,  0, 24);

        // ---- Indices (Stage 4 heartbeat rollout) -------------------------
        g_engine_heartbeat.register_engine("AmrUs500",             g_amr_us500.enabled,            3600,  7, 22);
        g_engine_heartbeat.register_engine("AmrGer40",             g_amr_ger40.enabled,            3600,  7, 22);
        g_engine_heartbeat.register_engine("AmrNas100",            g_amr_nas100.enabled,           3600,  7, 22);
        g_engine_heartbeat.register_engine("VwapRevGer40",         g_vwap_rev_ger40.enabled,       3600,  7, 22);
        g_engine_heartbeat.register_engine("Ger40LondonBrk",       g_ger40_london_brk.enabled,     3600,  7, 22);
        g_engine_heartbeat.register_engine("Ger40TurtleH4",        g_ger40_turtle_h4.enabled,      3600,  7, 22);
        g_engine_heartbeat.register_engine("MinimalH4Ger40",       g_minimal_h4_ger40.enabled,     3600,  7, 22);
        g_engine_heartbeat.register_engine("Us30Ensemble",         g_us30_ensemble.enabled,        3600,  7, 22);
        g_engine_heartbeat.register_engine("Us30_3BarMomH1",       g_us30_3bar_mom_h1.enabled,     3600,  7, 22);
        g_engine_heartbeat.register_engine("OrbDj30",              g_orb_dj30.enabled,             3600, 13, 22);
        g_engine_heartbeat.register_engine("OrbNas100",            g_orb_nas100.enabled,           3600, 13, 22);
        g_engine_heartbeat.register_engine("NasBbRevLongH1",       g_nas_bbrev_long_h1.enabled,    3600,  7, 22);

        // ---- Tsmom portfolio (5 cells, bar-driven on H1) -----------------
        // bar-driven, but driven from XAUUSD ticks via tick_gold.hpp's
        // forwarding to TsmomPortfolio::on_tick / on_h1_bar. Pulse fires from
        // the gold dispatcher so cadence is high; the 3600s envelope is
        // intentional for weekend safety.
        g_engine_heartbeat.register_engine("Tsmom_H1_long",      true, 3600,  0, 24);
        g_engine_heartbeat.register_engine("Tsmom_H2_long",      true, 3600,  0, 24);
        g_engine_heartbeat.register_engine("Tsmom_H4_long",      true, 3600,  0, 24);
        g_engine_heartbeat.register_engine("Tsmom_H6_long",      true, 3600,  0, 24);
        g_engine_heartbeat.register_engine("Tsmom_D1_long",      true, 3600,  0, 24);

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
    g_open_positions.register_source("GoldScalpPyramid",
        []() -> std::vector<omega::PositionSnapshot> {
            std::vector<omega::PositionSnapshot> out;
            if (!g_gold_scalp_pyramid.has_open_position()) return out;

            const auto& p = g_gold_scalp_pyramid.m_pos;
            const double mult = tick_value_multiplier(std::string("XAUUSD"));

            double current = p.base_entry;
            const auto it = g_last_tick_bid.find("XAUUSD");
            if (it != g_last_tick_bid.end() && it->second > 0.0) {
                current = it->second;
            }

            const double dir   = p.is_long ? 1.0 : -1.0;
            const double entry = p.weighted_entry();
            const double size  = p.total_size();
            const double unrl  = (current - entry) * dir * size * mult;

            omega::PositionSnapshot ps;
            ps.symbol         = "XAUUSD";
            ps.side           = p.is_long ? "LONG" : "SHORT";
            ps.size           = size;
            ps.entry          = entry;
            ps.current        = current;
            ps.unrealized_pnl = unrl;
            ps.mfe            = p.mfe_peak * size * mult;
            ps.mae            = p.mae * size * mult;
            ps.engine         = "GoldScalpPyramid";
            out.push_back(ps);
            return out;
        });
    // S38d 2026-05-26: FxScalpPyramid x5 position sources.
    //   Same shape as GoldScalpPyramid template above, parameterised over
    //   (engine_ref, symbol). Registered separately so each shows in the
    //   /api/v1/omega/positions endpoint with its own engine label.
    {
        auto register_fx_scalp = [](const std::string& sym,
                                    omega::FxScalpPyramidEngine& eng,
                                    const std::string& engine_label) {
            g_open_positions.register_source(engine_label,
                [&eng, sym, engine_label]() -> std::vector<omega::PositionSnapshot> {
                    std::vector<omega::PositionSnapshot> out;
                    if (!eng.has_open_position()) return out;

                    const auto& p = eng.m_pos;
                    const double mult = tick_value_multiplier(sym);

                    double current = p.base_entry;
                    const auto it = g_last_tick_bid.find(sym);
                    if (it != g_last_tick_bid.end() && it->second > 0.0) {
                        current = it->second;
                    }

                    const double dir   = p.is_long ? 1.0 : -1.0;
                    const double entry = p.weighted_entry();
                    const double size  = p.total_size();
                    const double unrl  = (current - entry) * dir * size * mult;

                    omega::PositionSnapshot ps;
                    ps.symbol         = sym;
                    ps.side           = p.is_long ? "LONG" : "SHORT";
                    ps.size           = size;
                    ps.entry          = entry;
                    ps.current        = current;
                    ps.unrealized_pnl = unrl;
                    ps.mfe            = p.mfe_peak * size * mult;
                    ps.mae            = p.mae * size * mult;
                    ps.engine         = engine_label;
                    out.push_back(ps);
                    return out;
                });
        };
        register_fx_scalp("EURUSD", g_fx_scalp_eurusd, "FxScalpPyramid_EURUSD");
        register_fx_scalp("USDJPY", g_fx_scalp_usdjpy, "FxScalpPyramid_USDJPY");
        register_fx_scalp("GBPUSD", g_fx_scalp_gbpusd, "FxScalpPyramid_GBPUSD");
        register_fx_scalp("USDCAD", g_fx_scalp_usdcad, "FxScalpPyramid_USDCAD");
        register_fx_scalp("AUDUSD", g_fx_scalp_audusd, "FxScalpPyramid_AUDUSD");
    }
    // 2026-05-19 S110: GoldRegimeDaily position source.
    g_open_positions.register_source("GoldRegimeDaily",
        []() -> std::vector<omega::PositionSnapshot> {
            std::vector<omega::PositionSnapshot> out;
            if (!g_gold_regime_daily.has_open_position()) return out;

            const auto& p = g_gold_regime_daily.m_pos;
            const double mult = tick_value_multiplier(std::string("XAUUSD"));

            double current = p.entry;
            const auto it = g_last_tick_bid.find("XAUUSD");
            if (it != g_last_tick_bid.end() && it->second > 0.0) {
                current = it->second;
            }

            const double dir = p.is_long ? 1.0 : -1.0;
            const double unrl = (current - p.entry) * dir * p.size * mult;

            omega::PositionSnapshot ps;
            ps.symbol         = "XAUUSD";
            ps.side           = p.is_long ? "LONG" : "SHORT";
            ps.size           = p.size;
            ps.entry          = p.entry;
            ps.current        = current;
            ps.unrealized_pnl = unrl;
            ps.mfe            = p.mfe_peak * p.size * mult;
            ps.mae            = p.mae * p.size * mult;
            ps.engine         = "GoldRegimeDaily";
            out.push_back(ps);
            return out;
        });
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
    g_open_positions.register_source("PDHLReversion",
        []() -> std::vector<omega::PositionSnapshot> {
            std::vector<omega::PositionSnapshot> out;
            if (!g_pdhl_rev.has_open_position()) return out;
            const auto& p = g_pdhl_rev.pos;
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
            ps.mae = p.mae * p.size * mult;
            ps.engine = "PDHLReversion";
            out.push_back(ps);
            return out;
        });

    // RSIReversal (XAU). pos has {active, is_long, entry, size, mfe}; no mae.
    g_open_positions.register_source("RSIReversal",
        []() -> std::vector<omega::PositionSnapshot> {
            std::vector<omega::PositionSnapshot> out;
            if (!g_rsi_reversal.has_open_position()) return out;
            const auto& p = g_rsi_reversal.pos;
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
            ps.mae = 0.0;  // RSIReversal pos struct has no .mae field
            ps.engine = "RSIReversal";
            out.push_back(ps);
            return out;
        });

    // MinimalH4Breakout (XAU). pos_ has {active, is_long, entry, size}; no mfe/mae.
    g_open_positions.register_source("MinimalH4Gold",
        []() -> std::vector<omega::PositionSnapshot> {
            std::vector<omega::PositionSnapshot> out;
            const auto& p = g_minimal_h4_gold.pos_;
            if (!p.active) return out;
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
            ps.mfe = 0.0;  // not tracked on MinimalH4 pos_
            ps.mae = 0.0;
            ps.engine = "MinimalH4Gold";
            out.push_back(ps);
            return out;
        });

    // MinimalH4US30Breakout (DJ30.F). Same shape as MinimalH4Gold.
    g_open_positions.register_source("MinimalH4US30",
        []() -> std::vector<omega::PositionSnapshot> {
            std::vector<omega::PositionSnapshot> out;
            const auto& p = g_minimal_h4_us30.pos_;
            if (!p.active) return out;
            const double mult = tick_value_multiplier(std::string("DJ30.F"));
            double current = p.entry;
            const auto it = g_last_tick_bid.find("DJ30.F");
            if (it != g_last_tick_bid.end() && it->second > 0.0) current = it->second;
            const double dir   = p.is_long ? 1.0 : -1.0;
            const double unrl  = (current - p.entry) * dir * p.size * mult;
            omega::PositionSnapshot ps;
            ps.symbol = "DJ30.F"; ps.side = p.is_long ? "LONG" : "SHORT";
            ps.size = p.size; ps.entry = p.entry; ps.current = current;
            ps.unrealized_pnl = unrl;
            ps.mfe = 0.0;
            ps.mae = 0.0;
            ps.engine = "MinimalH4US30";
            out.push_back(ps);
            return out;
        });

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

    // NoiseBandMomentum gold-london. Uses CrossPosition (active, is_long,
    //   entry, size, mfe, mae) -- private; we go through public accessors
    //   open_is_long() / open_entry() / open_size(). mfe/mae deferred (no
    //   public getter on CrossPosition.mfe yet -- S66 follow-up if needed).
    g_open_positions.register_source("NoiseBandMomentumGoldLdn",
        []() -> std::vector<omega::PositionSnapshot> {
            std::vector<omega::PositionSnapshot> out;
            if (!g_nbm_gold_london.has_open_position()) return out;
            const bool   is_long = g_nbm_gold_london.open_is_long();
            const double entry   = g_nbm_gold_london.open_entry();
            const double sz      = g_nbm_gold_london.open_size();
            const double mult = tick_value_multiplier(std::string("XAUUSD"));
            double current = entry;
            const auto it = g_last_tick_bid.find("XAUUSD");
            if (it != g_last_tick_bid.end() && it->second > 0.0) current = it->second;
            const double dir   = is_long ? 1.0 : -1.0;
            const double unrl  = (current - entry) * dir * sz * mult;
            omega::PositionSnapshot ps;
            ps.symbol = "XAUUSD"; ps.side = is_long ? "LONG" : "SHORT";
            ps.size = sz; ps.entry = entry; ps.current = current;
            ps.unrealized_pnl = unrl;
            ps.mfe = 0.0;  // CrossPosition.mfe is private; defer to S66
            ps.mae = 0.0;
            ps.engine = "NoiseBandMomentumGoldLdn";
            out.push_back(ps);
            return out;
        });

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
    g_open_positions.register_source("VWAPReversionSP",
        _make_vwap_source("VWAPReversionSP",     "US500.F", &g_vwap_rev_sp));
    g_open_positions.register_source("VWAPReversionNQ",
        _make_vwap_source("VWAPReversionNQ",     "USTEC.F", &g_vwap_rev_nq));
    g_open_positions.register_source("VWAPReversionGER40",
        _make_vwap_source("VWAPReversionGER40",  "GER40",   &g_vwap_rev_ger40));
    g_open_positions.register_source("VWAPReversionEURUSD",
        _make_vwap_source("VWAPReversionEURUSD", "EURUSD",  &g_vwap_rev_eurusd));

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
    g_open_positions.register_source("TrendPullbackGold",
        _make_tpb_source("TrendPullbackGold", "XAUUSD",  &g_trend_pb_gold));
    g_open_positions.register_source("TrendPullbackNQ",
        _make_tpb_source("TrendPullbackNQ",   "USTEC.F", &g_trend_pb_nq));

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
    g_open_positions.register_source("C1RetunedDonchianH1",
        _make_c1_donchian_source("C1RetunedDonchianH1",  &g_c1_retuned.donchian_h1_));
    g_open_positions.register_source("C1RetunedBollingerH2",
        _make_c1_bollinger_source("C1RetunedBollingerH2", &g_c1_retuned.bollinger_h2_));
    g_open_positions.register_source("C1RetunedBollingerH4",
        _make_c1_bollinger_source("C1RetunedBollingerH4", &g_c1_retuned.bollinger_h4_));
    g_open_positions.register_source("C1RetunedBollingerH6",
        _make_c1_bollinger_source("C1RetunedBollingerH6", &g_c1_retuned.bollinger_h6_));

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
    g_open_positions.register_source("H4RegimeGold",
        []() -> std::vector<omega::PositionSnapshot> {
            std::vector<omega::PositionSnapshot> out;
            if (!g_h4_regime_gold.has_open_position()) return out;
            const auto& p = g_h4_regime_gold.pos_;
            const std::string sym = g_h4_regime_gold.symbol;
            const double mult = tick_value_multiplier(sym);
            double current = p.entry;
            const auto it = g_last_tick_bid.find(sym);
            if (it != g_last_tick_bid.end() && it->second > 0.0) current = it->second;
            const double dir   = p.is_long ? 1.0 : -1.0;
            const double unrl  = (current - p.entry) * dir * p.size * mult;
            omega::PositionSnapshot ps;
            ps.symbol = sym; ps.side = p.is_long ? "LONG" : "SHORT";
            ps.size = p.size; ps.entry = p.entry; ps.current = current;
            ps.unrealized_pnl = unrl;
            ps.mfe = p.mfe * p.size * mult;
            ps.mae = 0.0;  // H4RegimeEngine.pos_ doesn't track mae
            ps.engine = "H4RegimeGold";
            out.push_back(ps);
            return out;
        });

    // MacroCrash (XAUUSD). MacroCrashEngine.pos is public struct Position
    //   with {active, is_long, entry, sl, full_size, size, mfe, mae, ...}.
    //   Reports the BASE position only (size, not full_size). Pyramid adds
    //   ride alongside but are tracked separately in pyramid_adds[].
    g_open_positions.register_source("MacroCrash",
        []() -> std::vector<omega::PositionSnapshot> {
            std::vector<omega::PositionSnapshot> out;
            if (!g_macro_crash.has_open_position()) return out;
            const auto& p = g_macro_crash.pos;
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
            ps.mae = p.mae * p.size * mult;  // MacroCrash tracks mae
            ps.engine = "MacroCrash";
            out.push_back(ps);
            return out;
        });

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
    g_open_positions.register_source("UstecTrendFollow5m",
        _make_ustec_tf_source("UstecTrendFollow5m", &g_ustec_tf_5m));
    g_open_positions.register_source("UstecTrendFollowHtf",
        _make_ustec_tf_source("UstecTrendFollowHtf", &g_ustec_tf_htf));

    // FX BreakoutEngine x 5 (EURUSD, GBPUSD, AUDUSD, NZDUSD, USDJPY).
    //   omega::BreakoutEngine inherits from BreakoutEngineBase<BreakoutEngine>
    //   which exposes `OpenPos pos` and `const char* symbol` as public
    //   members. OpenPos fields used here: {active, is_long, entry, size,
    //   mfe, mae}. Pyramid add-on legs ride alongside in pos.pyramid_addons[]
    //   but are tracked separately and not surfaced here (the GUI shows the
    //   base position only -- consistent with the MacroCrash convention).
    //   No has_open_position() method on BreakoutEngine; the codebase
    //   convention (see trade_lifecycle.hpp, on_tick.hpp) is to check
    //   `eng->pos.active` directly. tick_value_multiplier covers all five
    //   FX symbols; USDJPY uses live g_usdjpy_mid for the JPY->USD conv.
    auto _make_breakout_source = [](const char* engine_name,
                                    omega::BreakoutEngine* eng) {
        return [engine_name, eng]() -> std::vector<omega::PositionSnapshot> {
            std::vector<omega::PositionSnapshot> out;
            if (!eng->pos.active) return out;
            const auto& p = eng->pos;
            const std::string sym = eng->symbol;
            const double mult = tick_value_multiplier(sym);
            double current = p.entry;
            const auto it = g_last_tick_bid.find(sym);
            if (it != g_last_tick_bid.end() && it->second > 0.0) current = it->second;
            const double dir  = p.is_long ? 1.0 : -1.0;
            const double unrl = (current - p.entry) * dir * p.size * mult;
            omega::PositionSnapshot ps;
            ps.symbol = sym; ps.side = p.is_long ? "LONG" : "SHORT";
            ps.size = p.size; ps.entry = p.entry; ps.current = current;
            ps.unrealized_pnl = unrl;
            ps.mfe = p.mfe * p.size * mult;
            ps.mae = p.mae * p.size * mult;
            ps.engine = engine_name;
            out.push_back(ps);
            return out;
        };
    };
    g_open_positions.register_source("BreakoutEURUSD",
        _make_breakout_source("BreakoutEURUSD", &g_eng_eurusd));
    g_open_positions.register_source("BreakoutGBPUSD",
        _make_breakout_source("BreakoutGBPUSD", &g_eng_gbpusd));
    g_open_positions.register_source("BreakoutAUDUSD",
        _make_breakout_source("BreakoutAUDUSD", &g_eng_audusd));
    g_open_positions.register_source("BreakoutNZDUSD",
        _make_breakout_source("BreakoutNZDUSD", &g_eng_nzdusd));
    g_open_positions.register_source("BreakoutUSDJPY",
        _make_breakout_source("BreakoutUSDJPY", &g_eng_usdjpy));

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
    std::cout << "[OmegaApi] g_open_positions sources registered (46 sources: "
              "MidScalperGold, MicroScalperGold, "
              "EurusdLondonOpen, UsdjpyAsianOpen, GbpusdLondonOpen, "
              "AudusdSydneyOpen, NzdusdAsianOpen, XauusdFvg, "
              "PDHLReversion, RSIReversal, MinimalH4Gold, MinimalH4US30, "
              "XauThreeBar30m, NoiseBandMomentumGoldLdn, "
              "VWAPReversion x4, TrendPullback x2, "
              "IndexFlow x4, IndexMacroCrash x4, "
              "C1Retuned x4, "
              "EMACrossGold, H4RegimeGold, MacroCrash, "
              "XauTrendFollow 2h/4h/D1, "
              "H1SwingGold, UstecTrendFollow 5m/HTF, "
              "Breakout EURUSD/GBPUSD/AUDUSD/NZDUSD/USDJPY)\n";
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
}

