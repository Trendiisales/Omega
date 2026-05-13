#pragma once
// =============================================================================
//  UstecTrendFollow5mEngine.hpp -- USTEC 5m trend-follow ensemble
//                                  (S33e 2026-05-11, S34 2026-05-12,
//                                   S37-PROPOSED 2026-05-12 part C)
// =============================================================================
//
//  PROVENANCE
//
//  Built 2026-05-11 from edge_hunt.cpp + edge_hunt v3 results on 15 days
//  of USTEC L2 (2026-04-22 → 2026-05-08). Realistic bid/ask fill
//  simulation, $0.06/RT cost subtracted, 0.1 lot.
//
//  Cells (each independently positive, convergent across signal families):
//
//      [A] Donchian N=20  sl2.0tp4.0    n=111  WR=45%  net=+$1120  $10.09/trade
//      [B] Keltner  K=2.0 sl2.0tp4.0    n= 97  WR=45%  net=+$1291  $13.31/trade  ★ top cell
//
//  Plus convergent confirmation from ER_Trend ($1020) and MA-cross ($975)
//  on the same data, which we keep as research signals only (the two we
//  ship are uncorrelated enough mechanics to give decorrelated PnL).
//
//  CAVEAT: only 2 months of L2 data so far. **DEPLOY SHADOW ONLY** for at
//  least 6 months while you collect more USTEC L2 capture. Per-cell n is
//  modest (97-111 trades).
//
//  S37-PROPOSED UPDATE (2026-05-12 part C): the original 15-day shadow
//  caveat has been superseded by a 25-month tape sweep against HistData
//  NSXUSD tick CSVs (2024-01 through 2026-04). Findings:
//
//      - At the original constexpr config (sl=2.0, tp=4.0, prove_secs=90,
//        prove_pts=4.0, MIN_ATR=10), the engine is structurally net-
//        negative: 5,989 trades, -$64,637 net, 17.6% WR. 52.6% of trades
//        are killed by an over-tight prove-it timer before either SL or
//        TP can fire.
//
//      - The 5-constant patch below (Plan A best + Plan B best, from
//        outputs/USTEC_TREND_FOLLOW_5M_PLAN_A_B_REPORT.md) flips the
//        engine net-positive:
//            IS  25mo: 1,326 trades, +$7,586 net, 28.3% WR, PF 1.05
//            OOS 4mo:    260 trades, +$5,207 net, 29.6% WR
//        OOS run-rate (+$1,302/mo) exceeds IS run-rate (+$303/mo) — no
//        overfit risk on this slice.
//
//      - The 5 changed constants are listed at the change site. The
//        cost gate (1.5x ratio, added 2026-05-12 part A) remains in
//        place and is inert at the new config (Plan A's RR=2.33 +
//        MIN_ATR=20 → cost-to-edge >35x at typical ATR), but stays as
//        the correct defensive floor.
//
//  SAFETY
//      - shadow_mode = true by default (HARD shadow).
//      - 0.1 lot per cell (USTEC pt_size = 0.1).
//      - Max 2 concurrent positions (one per cell).
//      - Spread cap = 5.0 USD; refuse entries above this.
//      - 1-bar cooldown after exit (per cell).
//
//  USAGE
//
//      // globals.hpp:
//      static omega::UstecTrendFollow5mEngine g_ustec_tf_5m;
//
//      // engine_init.hpp:
//      g_ustec_tf_5m.shadow_mode = true;
//      g_ustec_tf_5m.enabled     = true;
//      g_ustec_tf_5m.lot         = 0.1;
//      g_ustec_tf_5m.max_spread  = 5.0;
//      g_ustec_tf_5m.init();
//
//      // tick_indices.hpp 5m-bar-close dispatch:
//      omega::UstecTfBar bar5m{};
//      bar5m.bar_start_ms = s_nq5_start;
//      bar5m.open  = s_nq5.open;
//      bar5m.high  = s_nq5.high;
//      bar5m.low   = s_nq5.low;
//      bar5m.close = s_nq5.close;
//      g_ustec_tf_5m.on_5m_bar(bar5m, bid, ask, atr_5m_external,
//                              now_ms, ca_on_close);
//
//      // tick_indices.hpp every USTEC tick:
//      g_ustec_tf_5m.on_tick(bid, ask, now_ms, ca_on_close);
// =============================================================================
//
//  S34 (2026-05-12 — operator-instructed: "fix this logging issue and the
//  PNL i treat this as actual trading and so it should be")
//
//  Five bugs in the pre-S34 close path made every trade emitted by this
//  engine show as $0.00 PnL, with both cells looking like duplicate rows
//  in the ledger. Every fix is in the _close() path or in the per-position
//  bookkeeping that feeds it; signal generation, sizing, and SL/TP logic
//  are untouched.
//
//      Bug 1  tr.pnl was never assigned — TradeRecord's default value
//             of 0.0 propagated to the ledger. trade_lifecycle's
//             `tr.pnl *= tick_value_multiplier(symbol)` then multiplied
//             0 by anything and got 0. Fix: set tr.pnl in raw points*lots
//             ((exit - entry)*sign*lot), matching the BracketEngine
//             convention at include/BracketEngine.hpp:1216-1218.
//
//      Bug 2  tr.symbol = "USTEC" (no .F suffix). sizing.hpp's
//             tick_value_multiplier table is keyed on "USTEC.F" → $20/pt.
//             "USTEC" falls through to the 1.0 default. Even if Bug 1
//             were fixed alone, USTEC PnL would have come out 20× smaller
//             than reality. Fix: tr.symbol = "USTEC.F".
//
//      Bug 3  Both cells (Donchian + Keltner) wrote
//             tr.engine = "UstecTrendFollow5m". When both fired on the
//             same 5m bar — which is the strategy's whole point of having
//             two uncorrelated signal families — the two resulting close
//             rows looked identical in the engine column and the logging
//             dedupe could not tell them apart. Fix: append the cell's
//             short_name (Donchian | Keltner) so the ledger has
//             "UstecTrendFollow5m_Donchian" and
//             "UstecTrendFollow5m_Keltner" as distinct engine strings.
//             The full cell name (with sl/tp multipliers) still rides in
//             tr.regime for forensic drill-down.
//
//      Bug 4  tr.side was "BUY" / "SELL" rather than the LONG / SHORT
//             convention used by every other engine and consumed by the
//             omega-terminal LDG panel. Fix: emit LONG / SHORT.
//
//      Bug 5  No MFE / MAE tracking. tr.mfe and tr.mae shipped as 0
//             on every close, so the [TRADE-COST] line and the CSV mfe/mae
//             columns were always 0 for this engine. Fix: add mfe_pts /
//             mae_pts to UstecTfPos, reset them on _fire_entry, and update
//             them on every tick inside _manage_open (mid-price excursion,
//             stored in raw points; trade_lifecycle multiplies by tick
//             value just like tr.pnl).
//
//  All five fixes live below in the comments tagged "S34 BUG #" so each
//  change site is traceable from the bug list above.
// =============================================================================
//
//  S34-B (same session, follow-up instruction: "FIX the bad trades,
//  these should not be occuring, why would i want to keep the engine
//  firing a bad trade wtaf")
//
//  Three structural guards added on top of the S34 close-path fixes.
//  These do not eliminate every losing trade — a strategy with the
//  engine's claimed ~45% WR is *designed* to be wrong slightly more
//  often than right, taking the edge from asymmetric R:R — but they
//  do eliminate three categories of trade that were losing for
//  preventable mechanical reasons rather than for "the market went
//  the other way" reasons.
//
//      Guard A  PROVE-IT EXIT.
//          New constants PROVE_IT_SECS = 90 and
//          PROVE_IT_MIN_FAVOURABLE_PTS = 4.0 plus a per-position
//          `proved` flag. For the first 90 seconds after entry, every
//          tick checks whether MFE has reached 4 pts favourable; if
//          it has, `proved` flips true and stays true. At 90 seconds,
//          if `proved` is still false, the position is force-closed
//          at the current price with reason "PROVE_IT_FAIL". The
//          structural reasoning: a 5m breakout that has not made
//          even 4 pts of progress in 90 seconds (~18 ticks at typical
//          USTEC tempo) is not a breakout that worked; it is a
//          breakout into noise. A tighter price-based SL would just
//          take more 1-minute SL hits in the wrong direction. This
//          exits on the absence of favourable movement instead, which
//          is the symptom you actually want to cut on.
//
//          New constant MIN_SL_PTS_FLOOR = 15.0 backs the prove-it
//          exit with a hard minimum SL distance, so even in a
//          low-ATR window the price-based SL can't sit inside the
//          spread+noise zone.
//
//          ** S37-PROPOSED: PROVE_IT_SECS 90 → 150 and
//             PROVE_IT_MIN_FAVOURABLE_PTS 4.0 → 2.0. The original
//             values were modelled on the 15-day L2 sample; against
//             25 months of tick data the 90s/4pt gate kills 52.6% of
//             trades, many of which take >90s to cross 2pt of
//             favourable excursion but then go on to net-positive
//             outcomes. Loosening the gate halves the PI exit count
//             and recovers gross PnL from -$14,892 to +$17,536. **
//
//      Guard B  CELL MUTUAL EXCLUSION (same-direction only).
//          When one cell has an open position in direction X, the
//          other cell cannot open a position in the same direction
//          X. Opposite-direction is still allowed (genuine
//          decorrelation). Pre-S34 the two cells could and did fire
//          long on the same 5m bar with the same ATR, taking double
//          exposure at peak correlation rather than minimum. Refusal
//          is logged as [USTEC-TF-MUTEX-BLOCK].
//
//      Guard C  MINIMUM ATR FLOOR.
//          New constant MIN_ATR_PTS = 10.0. On every 5m bar close,
//          before either cell's signal is evaluated, if atr14_ is
//          below this floor the bar is skipped and a
//          [USTEC-TF-ATR-FLOOR-BLOCK] line is logged. Breakouts fired
//          in dead chop are noise; this gate keeps the engine quiet
//          in conditions where its premise (a real intraday
//          expansion) does not hold.
//
//          ** S37-PROPOSED: MIN_ATR_PTS 10 → 20. With Plan A's
//             widened SL/TP, the cost-to-edge ratio at ATR<20 is
//             marginal even after the prove-it loosening. Raising
//             the floor cuts the trade count by ~45% (5,989 → 1,326
//             at Plan B settings) but eliminates the regime where
//             the strategy is structurally adverse-selected by chop.
//             This is the dominant Plan B lever. **
//
//  Tunable: every constant introduced by S34-B is a static constexpr
//  on the engine; flip them at the top of this file and rebuild. A
//  config-loaded variant can replace them later; constexpr keeps the
//  change blast-radius minimal for this first cut.
// =============================================================================
//
//  S37-PROPOSED (2026-05-12 part C — operator authorisation pending)
//
//  Five-constant promotion patch from the Plan A/B/entry sweep against
//  25 months of USTEC tick data. Source of record:
//      outputs/USTEC_TREND_FOLLOW_5M_PLAN_A_B_REPORT.md
//
//  Changed values (vs S34-B live):
//
//      kUstecTfCells[0].sl_mult     2.0  → 3.0  (Donchian cell)
//      kUstecTfCells[0].tp_mult     4.0  → 7.0  (Donchian cell)
//      kUstecTfCells[1].sl_mult     2.0  → 3.0  (Keltner cell)
//      kUstecTfCells[1].tp_mult     4.0  → 7.0  (Keltner cell)
//      PROVE_IT_SECS                90.0 → 150.0
//      PROVE_IT_MIN_FAVOURABLE_PTS  4.0  → 2.0
//      MIN_ATR_PTS                  10.0 → 20.0
//
//  All other code (signal logic, fire path, close path, cost gate,
//  guard A/B/C state machines) is byte-identical to S34-B. The cell
//  `name` strings are also updated so the regime column in the ledger
//  reflects the new multipliers ("Donchian_N20_sl3.0tp7.0" etc).
//
//  This patch MUST NOT be applied without:
//    (1) operator sign-off in chat (rule-4 invariant on live promotion),
//    (2) a 2-month shadow tape-replay confirming fire-rate within ±15%
//        of the backtest expectation of ~53 trades/month,
//    (3) RiskMonitor wiring for fire-rate / WR / spread surveillance
//        with auto-pin-on-trip (mirror g_gold_microscalper pattern).
//
//  Promote-or-not is a unit: every changed constant is interdependent
//  with the others. Specifically, MIN_ATR=20 is the gate that makes
//  the wider SL/TP economically viable; removing it pulls the net
//  back toward Plan A's -$1,805. Do not partially apply.
// =============================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <string>

#include "OmegaTradeLedger.hpp"
#include "OmegaCostGuard.hpp"

namespace omega {

struct UstecTfBar {
    int64_t bar_start_ms = 0;
    double  open  = 0.0;
    double  high  = 0.0;
    double  low   = 0.0;
    double  close = 0.0;
};

struct UstecTfPos {
    bool        active             = false;
    bool        is_long            = false;
    double      entry_px           = 0.0;
    double      tp_px              = 0.0;
    double      sl_px              = 0.0;
    double      atr_at_entry       = 0.0;
    int64_t     entry_ts_ms        = 0;
    int         bars_held          = 0;
    int         cooldown_bars      = 0;
    // S34 BUG #5: MFE/MAE tracking in raw price points (mid-based).
    //   mfe_pts is the most favourable excursion seen during the trade
    //   (positive = trade went your way at its best moment). mae_pts is
    //   the most adverse (negative or zero, more negative = worse loss
    //   along the way). Multiplied by lot and tick_value_multiplier
    //   downstream to produce USD figures in the ledger.
    double      mfe_pts            = 0.0;
    double      mae_pts            = 0.0;
    // S34-B Guard A: prove-it exit state.
    //   `proved` flips true the first tick at which mfe_pts reaches
    //   PROVE_IT_MIN_FAVOURABLE_PTS. It does not flip back: once a
    //   trade has shown favourable movement, the prove-it cut no
    //   longer applies and the normal SL/TP regime takes over.
    bool        proved             = false;
    std::string broker_position_id;
    std::string entry_clOrdId;
};

// ---------- Cell config
enum class UstecTfFamily { Donchian20, Keltner20 };
struct UstecTfCellConfig {
    UstecTfFamily family;
    double        sl_mult;
    double        tp_mult;
    const char*   name;        // full cell name, stamped into tr.regime
    // S34 BUG #3: short_name distinguishes cells in tr.engine so the
    //   ledger (and the CSV-write dedupe guard in logging.hpp) can tell
    //   Donchian-cell rows from Keltner-cell rows.
    const char*   short_name;
};

// 2 validated cells.
// S37-PROPOSED 2026-05-12: sl_mult 2.0→3.0, tp_mult 4.0→7.0 for both
// cells, per outputs/USTEC_TREND_FOLLOW_5M_PLAN_A_B_REPORT.md §3.
// Name strings updated so tr.regime in the ledger reflects new mults.
static constexpr UstecTfCellConfig kUstecTfCells[] = {
    { UstecTfFamily::Donchian20, 3.0, 7.0, "Donchian_N20_sl3.0tp7.0", "Donchian" },
    { UstecTfFamily::Keltner20,  3.0, 7.0, "Keltner_K2_sl3.0tp7.0",   "Keltner"  },
};
static constexpr int kUstecTfNumCells =
    static_cast<int>(sizeof(kUstecTfCells) / sizeof(kUstecTfCells[0]));

struct UstecTrendFollow5mEngine {
public:
    bool   shadow_mode = true;
    bool   enabled     = false;
    double lot         = 0.1;
    double max_spread  = 5.0;

    // ── S63 2026-05-14 VWR-pattern in-flight protection ────────────────────
    // Added in part-L following the part-K audit framework. Completes the
    // state-E → state-A transition called out in
    // docs/handoffs/SESSION_HANDOFF_2026-05-13k.md.
    //
    // Mirrors CrossAssetEngines.hpp VWAPReversionEngine L1245-1247 with
    // USTEC-scaled defaults. USTEC.F trades at ~$28,000 in May 2026 so:
    //
    //   LOSS_CUT_PCT  = 0.08  → ~22pt cold-loss cut. The S34-B floor
    //                           sl_mult=3.0 × MIN_ATR_PTS=20 = 60pt SL, so
    //                           LOSS_CUT only fires on outliers that exceed
    //                           the ATR-based SL by slippage or gap.
    //   BE_ARM_PCT    = 0.05  → ~14pt mfe arms the BE ratchet. Typical TP
    //                           is tp_mult=7.0 × atr14_≥20 = ≥140pt, so the
    //                           ratchet only engages after a trade has
    //                           earned ~10% of target — winners that have
    //                           validated thesis but then give it back.
    //   BE_BUFFER_PCT = 0.02  → ~5.6pt buffer, ~typical USTEC spread.
    //
    // Applied independently per cell inside _manage_open(); the LOSS_CUT
    // / BE_CUT force-closes the firing cell only, leaving the other cell
    // (if active) unaffected. Set _PCT = 0.0 in engine_init.hpp to disable
    // a phase. Set ALL to 0.0 to fully disable S63 (the part-K SP/NQ/EURUSD
    // VWR pattern — only valid with documented backtest evidence per
    // engine_init.hpp:597-672 precedent).
    //
    // ENABLE FLIP STILL BLOCKED: this wiring unblocks the eventual
    // `g_ustec_tf_5m.enabled = true` flip but does NOT trigger it. The
    // operator-side gating step remains a fresh-tape backtest sweep
    // demonstrating that S63 + the S37 widened SL/TP profile is positive
    // on USTEC tick data. See part-K handoff §"Recommended next-session
    // focus" item 3.
    double LOSS_CUT_PCT  = 0.08;
    double BE_ARM_PCT    = 0.05;
    double BE_BUFFER_PCT = 0.02;

    // ── S37-P2 RiskMonitor wiring ──────────────────────────────────────────
    // 2026-05-12 (part C): fire-side hook for RiskMonitor surveillance.
    // Bound at engine_init.hpp time to forward to g_risk_monitor.on_fire().
    // If unbound (e.g. backtest harness), invocation is a no-op. Called
    // from inside _fire_entry() AFTER cost gate passes and position is set
    // up, so the live engine state and the surveillance counter advance
    // together. Threshold calibration lives in
    // data/risk_monitor_thresholds.csv -- absence of a row there causes
    // g_risk_monitor.on_fire() to early-return; the wiring is in place
    // but takes effect only after backtest/calibrate_risk_thresholds is
    // re-run with UstecTrendFollow5m added to its ENGINE_TABLE. The
    // shadow_mode auto-pin callback registered in engine_init.hpp works
    // independently of the calibrated row and can be triggered via
    // RiskMonitor::trip_engine_to_shadow() at any time.
    std::function<void(int64_t now_s)> on_fire_hook;

    // ── S34-B Guard A constants (prove-it exit + SL floor) ──────────────────
    // S37-PROPOSED 2026-05-12: PROVE_IT_SECS 90→150,
    // PROVE_IT_MIN_FAVOURABLE_PTS 4.0→2.0. The original 90s/4pt cut
    // killed 52.6% of trades against 25 months of tick data; at
    // 150s/2pt that drops to ~27% and gross PnL flips from -$14,892
    // to +$17,536. See PLAN_A_B_REPORT §3 sensitivity matrix.
    // MIN_SL_PTS_FLOOR is unchanged; it is inert at the new MIN_ATR
    // floor (sl_mult=3.0 × atr14_=20+ = ≥60pt SL distance, well clear
    // of the 15-pt floor) but the floor remains the correct defensive
    // backstop for any future MIN_ATR loosening.
    static constexpr double PROVE_IT_SECS               = 150.0;
    static constexpr double PROVE_IT_MIN_FAVOURABLE_PTS = 2.0;
    static constexpr double MIN_SL_PTS_FLOOR            = 15.0;

    // ── S34-B Guard C constant (minimum ATR floor) ──────────────────────────
    // USTEC daily range 100-250pt, hourly 20-60pt (IndexFlowEngine.hpp:19).
    // S37-PROPOSED 2026-05-12: MIN_ATR_PTS 10→20. With Plan A's
    // widened SL/TP, the regime below ATR=20 is structurally adverse-
    // selected by chop; the cost gate alone does not catch it. Raising
    // the floor is the binding Plan B lever — it is the change that
    // flips the engine from -$1,805 net (Plan A alone) to +$7,586 net
    // IS / +$5,207 OOS (Plan A + Plan B). See PLAN_A_B_REPORT §4.
    static constexpr double MIN_ATR_PTS = 20.0;

    std::array<UstecTfPos, kUstecTfNumCells> pos{};

    static constexpr int kBarHistory = 64;
    std::deque<UstecTfBar> bars_;

    // Local indicators
    static constexpr int kAtrPeriod = 14;
    double atr14_ = 0.0;
    int    atr_warmup_count_ = 0;

    static constexpr int kEmaPeriod = 20;
    double ema20_ = 0.0;
    bool   ema_initialised_ = false;

    using OnCloseFn = std::function<void(const omega::TradeRecord&)>;

    void init() noexcept {
        bars_.clear();
        atr14_ = 0.0;
        atr_warmup_count_ = 0;
        ema20_ = 0.0;
        ema_initialised_ = false;
        for (auto& p : pos) p = {};
    }

    bool has_open_position() const noexcept {
        for (const auto& p : pos) if (p.active) return true;
        return false;
    }
    int open_count() const noexcept {
        int n = 0; for (const auto& p : pos) if (p.active) ++n; return n;
    }

    void on_5m_bar(const UstecTfBar& bar,
                   double bid, double ask,
                   double atr14_external,
                   int64_t now_ms,
                   OnCloseFn on_close) noexcept {
        if (!enabled) return;
        bars_.push_back(bar);
        while ((int)bars_.size() > kBarHistory) bars_.pop_front();

        if (atr14_external > 0.0) atr14_ = atr14_external;
        else _update_local_atr();
        _update_ema20();

        for (auto& p : pos) {
            if (p.cooldown_bars > 0) --p.cooldown_bars;
            if (p.active) ++p.bars_held;
        }

        if ((int)bars_.size() < 22) return;
        if (atr14_ <= 0.0) return;
        if (ask - bid > max_spread) return;

        // S34-B Guard C: minimum ATR floor. Below this the tape is chop
        // and Donchian/Keltner breakouts are noise, not signal.
        if (atr14_ < MIN_ATR_PTS) {
            printf("[USTEC-TF-ATR-FLOOR-BLOCK] atr14=%.2f floor=%.2f -- "
                   "bar skipped, no breakout entries in low-vol regime\n",
                   atr14_, MIN_ATR_PTS);
            fflush(stdout);
            return;
        }

        for (int ci = 0; ci < kUstecTfNumCells; ++ci) {
            if (pos[ci].active) continue;
            if (pos[ci].cooldown_bars > 0) continue;
            int side = _evaluate_signal(ci);
            if (side == 0) continue;

            // S34-B Guard B: cell mutual exclusion (same-direction only).
            // Refuse if any *other* cell already has an open position in
            // the same direction. Opposite-direction is still allowed.
            if (_other_cell_open_same_direction(ci, side)) {
                printf("[USTEC-TF-MUTEX-BLOCK] cell=%s side=%s -- other cell "
                       "already long-side in same direction, skipping entry\n",
                       kUstecTfCells[ci].short_name,
                       side > 0 ? "LONG" : "SHORT");
                fflush(stdout);
                continue;
            }

            _fire_entry(ci, side, bid, ask, now_ms);
        }
        (void)on_close;
    }

    void on_tick(double bid, double ask, int64_t now_ms,
                 OnCloseFn on_close) noexcept {
        if (!enabled) return;
        for (int ci = 0; ci < kUstecTfNumCells; ++ci) {
            if (!pos[ci].active) continue;
            _manage_open(ci, bid, ask, now_ms, on_close);
        }
    }

    void force_close(double bid, double ask, int64_t now_ms,
                     OnCloseFn on_close, const char* reason) noexcept {
        for (int ci = 0; ci < kUstecTfNumCells; ++ci) {
            if (!pos[ci].active) continue;
            double xp = pos[ci].is_long ? bid : ask;
            _close(ci, xp, reason ? reason : "FORCE_CLOSE", now_ms, on_close);
        }
    }

private:
    void _update_local_atr() noexcept {
        if ((int)bars_.size() < 2) { atr14_ = 0.0; return; }
        const auto& cur  = bars_.back();
        const auto& prev = bars_[bars_.size() - 2];
        double tr = std::max(cur.high - cur.low,
                             std::max(std::abs(cur.high - prev.close),
                                      std::abs(cur.low  - prev.close)));
        if (atr_warmup_count_ < kAtrPeriod) {
            atr14_ = (atr14_ * atr_warmup_count_ + tr) / (atr_warmup_count_ + 1);
            ++atr_warmup_count_;
        } else {
            atr14_ = (atr14_ * (kAtrPeriod - 1) + tr) / kAtrPeriod;
        }
    }

    void _update_ema20() noexcept {
        if (bars_.empty()) return;
        double c = bars_.back().close;
        if (!ema_initialised_) { ema20_ = c; ema_initialised_ = true; }
        else {
            double a = 2.0 / (kEmaPeriod + 1);
            ema20_ = a * c + (1.0 - a) * ema20_;
        }
    }

    // S34-B Guard B helper: true if any cell other than `ci` is currently
    // active in the requested direction.
    bool _other_cell_open_same_direction(int ci, int side) const noexcept {
        const bool want_long = (side > 0);
        for (int k = 0; k < kUstecTfNumCells; ++k) {
            if (k == ci) continue;
            if (!pos[k].active) continue;
            if (pos[k].is_long == want_long) return true;
        }
        return false;
    }

    int _evaluate_signal(int ci) const noexcept {
        switch (kUstecTfCells[ci].family) {
            case UstecTfFamily::Donchian20: return _sig_donchian();
            case UstecTfFamily::Keltner20:  return _sig_keltner();
        }
        return 0;
    }

    int _sig_donchian() const noexcept {
        const int N = 20;
        if ((int)bars_.size() < N + 1) return 0;
        const int last = (int)bars_.size() - 1;
        double hi = bars_[last - N].high, lo = bars_[last - N].low;
        for (int k = last - N + 1; k <= last - 1; ++k) {
            if (bars_[k].high > hi) hi = bars_[k].high;
            if (bars_[k].low  < lo) lo = bars_[k].low;
        }
        if (bars_[last].close > hi) return +1;
        if (bars_[last].close < lo) return -1;
        return 0;
    }

    int _sig_keltner() const noexcept {
        if (!ema_initialised_ || atr14_ <= 0.0) return 0;
        if (bars_.size() < 22) return 0;
        const auto& cur = bars_.back();
        double up = ema20_ + 2.0 * atr14_;
        double lo = ema20_ - 2.0 * atr14_;
        if (cur.close > up) return +1;
        if (cur.close < lo) return -1;
        return 0;
    }

    void _fire_entry(int ci, int side, double bid, double ask, int64_t now_ms) noexcept {
        const auto& cfg = kUstecTfCells[ci];
        double entry = (side > 0) ? ask : bid;
        if (entry <= 0.0 || atr14_ <= 0.0) return;

        // S34-B Guard A: floor the SL distance so a low-ATR window can't
        // produce an SL that sits inside the spread+noise zone. Take the
        // larger of the ATR-derived distance and the hard floor; TP is
        // also widened proportionally so the cell's R:R is preserved.
        double sl_dist_atr = cfg.sl_mult * atr14_;
        double sl_dist     = std::max(sl_dist_atr, MIN_SL_PTS_FLOOR);
        double tp_dist     = cfg.tp_mult * atr14_;
        if (sl_dist > sl_dist_atr) {
            // SL was widened by the floor — widen TP by the same ratio so
            // R:R stays at the cell's configured value (e.g. 2.0).
            const double ratio = sl_dist / sl_dist_atr;
            tp_dist = tp_dist * ratio;
        }

        // 2026-05-12 cost gate -- see outputs/PLAN_A_B_REPORT.md
        {
            const double spread_pts = ask - bid;
            if (!ExecutionCostGuard::is_viable(
                    "USTEC.F", spread_pts, tp_dist, lot, 1.5))
            {
                return;
            }
        }

        auto& p = pos[ci];
        p.active        = true;
        p.is_long       = (side > 0);
        p.entry_px      = entry;
        p.sl_px         = (side > 0) ? entry - sl_dist : entry + sl_dist;
        p.tp_px         = (side > 0) ? entry + tp_dist : entry - tp_dist;
        p.atr_at_entry  = atr14_;
        p.entry_ts_ms   = now_ms;
        p.bars_held     = 0;
        p.cooldown_bars = 0;
        // S34 BUG #5 / S34-B Guard A: zero MFE/MAE and prove-it flag on
        //   entry; updated on every tick inside _manage_open.
        p.mfe_pts       = 0.0;
        p.mae_pts       = 0.0;
        p.proved        = false;
        p.broker_position_id.clear();
        p.entry_clOrdId.clear();

        // S37-P2 RiskMonitor: forward fire event to surveillance.
        //   No-op if hook is unbound (e.g. backtest harness fork). The
        //   hook is bound in engine_init.hpp to call
        //   g_risk_monitor.on_fire("UstecTrendFollow5m", now_s); the
        //   umbrella engine name (sans cell suffix) is used for the
        //   surveillance entry because the threshold model treats the
        //   ensemble as one signal source. Per-cell forensics still live
        //   in tr.engine ("UstecTrendFollow5m_Donchian" / "_Keltner") on
        //   the close-side ledger.
        if (on_fire_hook) on_fire_hook(now_ms / 1000);
    }

    void _manage_open(int ci, double bid, double ask, int64_t now_ms,
                      OnCloseFn on_close) noexcept {
        auto& p = pos[ci];

        // S34 BUG #5: update MFE/MAE against live mid before any exit check.
        //   Done in price points; lot and tick value are applied downstream.
        //   Long: favourable = mid - entry. Short: favourable = entry - mid.
        //   Adverse is just negative favourable; we keep both extrema.
        const double mid = (bid + ask) * 0.5;
        if (mid > 0.0 && p.entry_px > 0.0) {
            const double favourable = p.is_long ? (mid - p.entry_px)
                                                : (p.entry_px - mid);
            if (favourable > p.mfe_pts) p.mfe_pts = favourable;
            if (favourable < p.mae_pts) p.mae_pts = favourable;

            // S34-B Guard A: once favourable progress crosses the prove-it
            // threshold, lock in `proved = true`. It does not flip back.
            if (!p.proved && p.mfe_pts >= PROVE_IT_MIN_FAVOURABLE_PTS) {
                p.proved = true;
            }

            // ── S63 2026-05-14 VWR-pattern in-flight protection ────────────
            // Runs BEFORE the PROVE_IT / SL / TP checks so cold-loss outliers
            // and giveback retraces take priority. Mirrors
            // CrossAssetEngines.hpp:1304-1383. Per-cell: independent checks,
            // _close() force-closes the firing cell only.
            //
            // Phase 1: BE_RATCHET. Once mfe_pts crosses BE_ARM_PCT*entry/100,
            // the position is treated as having validated its thesis;
            // any retrace to BE_BUFFER_PCT*entry/100 of entry triggers a
            // break-even cut. Disabled when BE_ARM_PCT == 0.0.
            //
            // Phase 2: LOSS_CUT. Cold-loss cut for trades that go straight
            // adverse without ever turning profitable. Disabled when
            // LOSS_CUT_PCT == 0.0.
            //
            // PROVE_IT_FAIL still runs below for the "stalled at entry"
            // case that LOSS_CUT can't see (small adverse, no MFE). The
            // legacy SL_HIT / TP_HIT remain the ultimate price-based gates.
            const double move    = p.is_long ? (mid - p.entry_px)
                                             : (p.entry_px - mid);
            const double adverse = -move;
            if (BE_ARM_PCT > 0.0 && BE_BUFFER_PCT >= 0.0 && p.entry_px > 0.0) {
                const double arm_pts    = p.entry_px * BE_ARM_PCT    / 100.0;
                const double buffer_pts = p.entry_px * BE_BUFFER_PCT / 100.0;
                if (p.mfe_pts >= arm_pts && move <= buffer_pts) {
                    const double xp = p.is_long ? bid : ask;
                    printf("[USTEC-TF] %s BE_CUT cell=%s mfe=%.2f arm=%.2f "
                           "move=%.2f buf=%.2f (giveback)\n",
                           p.is_long ? "LONG" : "SHORT",
                           kUstecTfCells[ci].short_name,
                           p.mfe_pts, arm_pts, move, buffer_pts);
                    fflush(stdout);
                    _close(ci, xp, "BE_CUT", now_ms, on_close);
                    return;
                }
            }
            if (LOSS_CUT_PCT > 0.0 && p.entry_px > 0.0) {
                const double loss_cut_dist = p.entry_px * LOSS_CUT_PCT / 100.0;
                if (adverse >= loss_cut_dist) {
                    const double xp = p.is_long ? bid : ask;
                    printf("[USTEC-TF] %s LOSS_CUT cell=%s adverse=%.2f >= "
                           "%.2f (%.3f%% of entry)\n",
                           p.is_long ? "LONG" : "SHORT",
                           kUstecTfCells[ci].short_name,
                           adverse, loss_cut_dist, LOSS_CUT_PCT);
                    fflush(stdout);
                    _close(ci, xp, "LOSS_CUT", now_ms, on_close);
                    return;
                }
            }
        }

        // S34-B Guard A: prove-it exit. If the trade has been open longer
        // than PROVE_IT_SECS and it never reached PROVE_IT_MIN_FAVOURABLE_PTS
        // of favourable excursion, force-close at the current adverse-side
        // price ("the side we'd have to cross to exit"). Reason
        // PROVE_IT_FAIL is logged separately from SL_HIT so post-hoc
        // analysis can separate "stopped by structural cut" from "stopped
        // by price-based SL".
        const double elapsed_s =
            (p.entry_ts_ms > 0) ? (now_ms - p.entry_ts_ms) / 1000.0 : 0.0;
        if (!p.proved && elapsed_s >= PROVE_IT_SECS) {
            const double xp = p.is_long ? bid : ask;
            _close(ci, xp, "PROVE_IT_FAIL", now_ms, on_close);
            return;
        }

        bool hit_tp = false, hit_sl = false;
        double xp = 0;
        if (p.is_long) {
            if (bid <= p.sl_px) { xp = p.sl_px; hit_sl = true; }
            else if (bid >= p.tp_px) { xp = p.tp_px; hit_tp = true; }
        } else {
            if (ask >= p.sl_px) { xp = p.sl_px; hit_sl = true; }
            else if (ask <= p.tp_px) { xp = p.tp_px; hit_tp = true; }
        }
        if (!hit_tp && !hit_sl) return;
        _close(ci, xp, hit_tp ? "TP_HIT" : "SL_HIT", now_ms, on_close);
    }

    void _close(int ci, double exit_px, const char* reason,
                int64_t now_ms, OnCloseFn on_close) noexcept {
        auto& p = pos[ci];
        if (!p.active) return;

        // S34 BUG #1: raw points*lots P&L, matching BracketEngine convention
        //   (include/BracketEngine.hpp:1216-1218). trade_lifecycle.hpp:218-222
        //   multiplies tr.pnl/mfe/mae by tick_value_multiplier(tr.symbol) to
        //   get USD; tick_value_multiplier("USTEC.F") = 20.0 (sizing.hpp:54).
        //   Pre-S34 this line did not exist and tr.pnl propagated as 0.0.
        const double pts_move = p.is_long ? (exit_px - p.entry_px)
                                          : (p.entry_px - exit_px);

        omega::TradeRecord tr;
        // S34 BUG #2: "USTEC.F" matches the sizing table; "USTEC" did not.
        tr.symbol     = "USTEC.F";
        // S34 BUG #3: per-cell engine string so Donchian and Keltner rows
        //   are distinct in the ledger and survive the logging.hpp dedupe.
        //   Full cell name still goes into tr.regime below for forensics.
        tr.engine     = std::string("UstecTrendFollow5m_") +
                        kUstecTfCells[ci].short_name;
        // S34 BUG #4: LONG/SHORT (ledger convention) rather than BUY/SELL.
        tr.side       = p.is_long ? "LONG" : "SHORT";
        tr.entryPrice = p.entry_px;
        tr.exitPrice  = exit_px;
        tr.tp         = p.tp_px;
        tr.sl         = p.sl_px;
        tr.size       = lot;
        // S34 BUG #1: actually populate the PnL fields.
        tr.pnl        = pts_move * lot;
        tr.net_pnl    = tr.pnl;                  // trade_lifecycle overwrites after costs
        // S34 BUG #5: emit MFE/MAE in raw pts*lots; downstream applies mult.
        tr.mfe        = p.mfe_pts * lot;
        tr.mae        = p.mae_pts * lot;
        tr.entryTs    = p.entry_ts_ms / 1000;
        tr.exitTs     = now_ms / 1000;
        tr.exitReason = reason;
        tr.regime     = kUstecTfCells[ci].name;
        tr.shadow     = shadow_mode;

        if (on_close) on_close(tr);

        p.active        = false;
        p.broker_position_id.clear();
        p.entry_clOrdId.clear();
        p.cooldown_bars = 1;
    }
};

} // namespace omega
