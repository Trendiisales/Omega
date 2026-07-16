#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <chrono>
#include <cmath>
#include <unordered_set>

namespace omega {

// ?????????????????????????????????????????????????????????????????????????????
// Realistic cost model for shadow simulation.
// All values are per-unit (same unit as size).
//
// slippage_entry_pct  : one-way slippage on entry as % of entry price.
//                       Default 0.005% (~half-spread on liquid futures).
//                       Applied as adverse fill: LONG pays ask+slip, SHORT sells bid-slip.
// slippage_exit_pct   : one-way slippage on exit (TP fills at TP-slip LONG, SL at SL-slip LONG).
// commission_per_side : flat $ commission per side (entry + exit both charged).
//                       Default $0.0 (BlackBull CFD/futures -- spread is the cost).
//                       Set to 3.50 for CME futures (round-turn $7).
//
// net_pnl = gross_pnl - total_slippage_cost - total_commission_cost
// ?????????????????????????????????????????????????????????????????????????????
struct TradeRecord
{
    int         id          = 0;
    std::string symbol;          // "US500.F", "USTEC.F", "XAUUSD", etc.
    std::string side;            // "LONG" / "SHORT"
    double      entryPrice  = 0;
    double      exitPrice   = 0;
    double      tp          = 0;
    double      sl          = 0;
    double      size        = 1.0;
    double      pnl         = 0;   // gross pnl (price difference ? size, no costs)
    double      mfe         = 0;   // max favourable excursion (price units ? size)
    double      mae         = 0;   // max adverse excursion
    int64_t     entryTs     = 0;   // unix seconds
    int64_t     exitTs      = 0;
    std::string exitReason;        // "TP_HIT" / "SL_HIT" / "TIMEOUT" / "FORCE_CLOSE" / "SCRATCH"
    double      spreadAtEntry = 0; // full bid-ask spread at entry (price units)
    double      latencyMs   = 0;
    std::string engine      = "BreakoutEngine";
    std::string regime;            // macro regime at time of trade

    // ?? Realistic cost fields (populated by apply_realistic_costs()) ?????????
    double      slippage_entry  = 0;  // $ slippage cost on entry (adverse fill vs mid)
    double      slippage_exit   = 0;  // $ slippage cost on exit
    double      commission      = 0;  // $ total commission (both sides)
    double      net_pnl         = 0;  // pnl - slippage_entry - slippage_exit - commission

    // ?? Convenience: cost parameters used (set by apply_realistic_costs()) ???
    double      slip_entry_pct  = 0;  // entry slippage rate applied
    double      slip_exit_pct   = 0;  // exit slippage rate applied
    double      comm_per_side   = 0;  // commission per side applied

    // ?? Bracket metadata (only set for bracket engine trades) ????????????????
    double      bracket_hi      = 0;  // upper boundary of the bracket range at arm time
    double      bracket_lo      = 0;  // lower boundary of the bracket range at arm time

    // ?? L2 / DOM data at entry (for post-trade analysis and L2 threshold calibration) ?
    double      l2_imbalance    = 0.5; // bid/(bid+ask) volume ratio at entry (0=ask-heavy, 1=bid-heavy)
    bool        l2_live         = false; // true = real L2 data, false = synthetic

    // ?? Engine-internal volatility at entry (for post-hoc filter calibration) ???
    //    Currently populated by CrossPosition (TPB sets atr_) when engine passes
    //    its atr_ value into pos_.open(). Other engines default to 0.0 (harmless).
    //    Used by S44 Asia-filter analysis: filter trades by atr_at_entry >= 3.0.
    double      atr_at_entry    = 0.0; // engine ATR at entry (price units, EWM)

    // ?? Shadow-mode guard -- set true when the originating engine is running in shadow mode.
    //    handle_closed_trade() short-circuits shadow records to audit-only logging so they
    //    never pollute g_omegaLedger, daily_pnl, consec_losses, fast_loss_streak, engine_culled,
    //    crowding guard, walk-forward, param gate, time-of-day gate, or auto-disable state.
    //    Engines with their own shadow_mode member should stamp this at TradeRecord construction.
    //    Engines without shadow_mode rely on the g_cfg.mode != "LIVE" fallback in handle_closed_trade.
    bool        shadow          = false;

    // ?? 2026-05-09 BROKER RECONCILIATION FIELDS (user-authorised after the
    //    NZ$308 disparity incident):
    //
    // The fundamental flaw exposed today: handle_closed_trade unconditionally
    // records every trade the engine THINKS happened, regardless of whether
    // the broker actually filled it. When live orders fail (volume reject,
    // FIX disconnect, hedging orphan, shadow-mode gate), the engine ledger
    // and dashboard keep counting paper wins while the real account bleeds.
    //
    // These fields track each trade's actual fate at the broker via inbound
    // ExecutionReports:
    //
    //   entry_clOrdId / close_clOrdId : the FIX 11=ClOrdID for each leg.
    //     Stamped at the engine dispatch site (entry: tick_gold.hpp;
    //     close: microscalper_on_close in trade_lifecycle.hpp) right after
    //     send_live_order returns. Used by handle_execution_report to find
    //     the matching trade when an inbound 35=8 arrives.
    //
    //   broker_entry_filled / broker_close_filled : flipped to true when an
    //     ExecReport with status=2 (Fill) arrives matching the corresponding
    //     clOrdId. Both true => trade fully confirmed at broker.
    //
    //   broker_entry_rejected / broker_close_rejected : flipped to true on
    //     ExecReport status=8 (Rejected). Distinct from "not yet acked".
    //
    //   broker_entry_fill_px / broker_close_fill_px : actual fill prices
    //     from FIX tag 31 (LastPx). Used to compute REAL realised P&L
    //     instead of engine-modeled P&L.
    //
    //   broker_pnl : computed when both legs filled, using actual fill
    //     prices times size times USD_PER_PT. This is the truth value;
    //     tr.pnl is the engine's prediction.
    //
    // Dashboard reads broker_pnl for trades where both flags are true to
    // display "Broker realised P&L". Trades where one leg filled but the
    // other didn't (orphans, the NZ$459 incident pattern) are flagged in
    // a separate "Orphans" counter.
    std::string entry_clOrdId;
    std::string close_clOrdId;
    bool        broker_entry_filled    = false;
    bool        broker_close_filled    = false;
    bool        broker_entry_rejected  = false;
    bool        broker_close_rejected  = false;
    double      broker_entry_fill_px   = 0.0;
    double      broker_close_fill_px   = 0.0;
    double      broker_pnl             = 0.0;   // realised in USD when both legs filled
};

// ?????????????????????????????????????????????????????????????????????????????
// apply_realistic_costs -- fills TradeRecord cost fields for shadow simulation.
//
// CALL ORDER: invoke this AFTER tr.pnl has been scaled to USD by tick_mult.
// All output cost fields (slippage_entry, slippage_exit, commission, net_pnl)
// are in USD.
//
// tick_mult: dollar value per price-point per lot from tick_value_multiplier().
//   e.g. XAGUSD=5000, USOIL.F=1000, XAUUSD=100, US500.F=1, EURUSD=100000
//   This is required to convert price-point slippage into USD correctly.
//   Without it, slippage on a $5000/pt instrument would be ~$0.08 instead of
//   ~$8, making net_pnl ? gross_pnl and the cost model meaningless.
//
// Instrument slippage presets (one-way, % of price):
//   XAUUSD / XAGUSD          : 0.010% -- gold/silver tick friction
//   USOIL.F / UKBRENT        : 0.012% -- oil slightly wider intraday
//   USTEC.F / US500.F / DJ30 : 0.006% -- liquid equity index futures
//   GER40 / UK100 / ESTX50   : 0.008% -- European index CFDs, slightly wider
//   EURUSD                   : 0.003% -- FX major, very liquid
//   default                  : 0.008%
//
// Commission: 0.0 per side (BlackBull CFD -- cost is embedded in spread).
// ?????????????????????????????????????????????????????????????????????????????
inline void apply_realistic_costs(TradeRecord& tr,
                                  double commission_per_side,
                                  double tick_mult)
{
    tr.comm_per_side = commission_per_side;

    // ENTRY SLIPPAGE:
    //   Engine records pos.entry = mid. Real fill: LONG=ask, SHORT=bid.
    //   Cost = half-spread x tick_mult x size (= crossing the spread once).
    tr.slippage_entry = (tr.spreadAtEntry / 2.0) * tick_mult * tr.size;
    tr.slip_entry_pct = (tr.entryPrice > 0.0)
        ? (tr.spreadAtEntry / 2.0) / tr.entryPrice * 100.0 : 0.0;

    // EXIT SLIPPAGE:
    //   TP_HIT    : fills at exact TP level. Zero exit slippage.
    //   All others: SL/trail/timeout/scratch/force-close all cost half a spread.
    //               SL in a fast move fills ~0.5 spread worse than the stop level.
    //               Timeout/scratch = market close at mid = half spread to cross.
    {
        const double half = (tr.spreadAtEntry / 2.0) * tick_mult * tr.size;
        tr.slippage_exit = (tr.exitReason == "TP_HIT") ? 0.0 : half;
        tr.slip_exit_pct = (tr.exitPrice > 0.0)
            ? (tr.spreadAtEntry / 2.0) / tr.exitPrice * 100.0 : 0.0;
    }

    // COMMISSION: $3/side (entry+exit) for FX/metals. $0 for indices/oil.
    tr.commission = commission_per_side * 2.0 * tr.size;

    // NET P&L = gross minus all execution costs
    tr.net_pnl = tr.pnl - tr.slippage_entry - tr.slippage_exit - tr.commission;
}

class OmegaTradeLedger
{
public:
    // Returns true if the trade was newly inserted, false if the dedup guard
    // blocked it as a replay. Callers that re-sync from the CSV journal (boot
    // reload + the periodic ledger self-heal in omega_main) use the return
    // value to gate side effects (engine-PnL attribution) so a re-read of an
    // already-recorded row cannot double-count. Void-context callers ignore it.
    bool record(const TradeRecord& tr)
    {
        std::lock_guard<std::mutex> lk(m_mtx);

        // ?? DEDUP GUARD ???????????????????????????????????????????????????????????
        // Prevents replayed close events from being double-booked into the ledger.
        // Root cause: GoldStack positions opened in a prior session persist
        // in engine memory across reconnects. On SL/trail hit after reconnect,
        // on_close fires -> handle_closed_trade -> ledger.record() for a trade that
        // may already have been recorded (or belongs to yesterday's session).
        //
        // Dedup key: symbol + entryTs + engine. entryTs is unix seconds at entry --
        // unique per trade per engine. exitReason excluded so partials (same entryTs,
        // different exitReason) are each counted, but the same full-close can't fire twice.
        //
        // PARTIAL exception: PARTIAL_1R/PARTIAL_2R share entryTs with the final close.
        // We dedup on symbol+entryTs+engine+exitReason for partials only, so they don't
        // block the final close from being recorded.
        {
            const std::string key = tr.symbol + "|" + std::to_string(tr.entryTs)
                                  + "|" + tr.engine + "|" + tr.exitReason;
            if (m_seen_keys.count(key)) {
                // Already recorded -- this is a replay. Log and discard.
                printf("[LEDGER-DEDUP] BLOCKED replay of %s %s entryTs=%lld reason=%s pnl=%.2f\n",
                       tr.symbol.c_str(), tr.engine.c_str(),
                       (long long)tr.entryTs, tr.exitReason.c_str(), tr.net_pnl);
                fflush(stdout);
                return false;
            }
            m_seen_keys.insert(key);
        }

        m_trades.push_back(tr);
        // net_pnl is used for all stats (after slippage + commission)
        const double pnl_for_stats = (tr.net_pnl != 0.0 || tr.slippage_entry != 0.0)
                                     ? tr.net_pnl : tr.pnl;
        // TOTALS FIX: PARTIAL_1R and PARTIAL_2R are banking events on an open position.
        // The dollars are real and must be counted in daily_pnl (broker paid them out).
        // But they must NOT count as wins/losses in the trade count -- doing so inflates
        // total_trades, distorts W/L ratio, and causes the GUI header to show e.g.
        // "8 closed 7W/1L" when only 4 full positions were taken (4 partials + 4 finals).
        // Only fully-closed positions (TRAIL/SL/TP/BE/FC exits) count as trades.
        const bool is_partial = (tr.exitReason == "PARTIAL_1R" || tr.exitReason == "PARTIAL_2R");
        if (!is_partial) {
            if (pnl_for_stats > 0) { m_wins++; m_sum_win += pnl_for_stats; }
            else                   { m_losses++; m_sum_loss += std::abs(pnl_for_stats); }
        }
        m_daily_pnl       += pnl_for_stats;
        m_cumulative_pnl  += pnl_for_stats;  // never resets -- true equity tracking
        m_gross_daily_pnl += tr.pnl;   // gross before costs -- for display transparency
        if (m_daily_pnl - m_peak_pnl < -m_max_dd) m_max_dd = m_peak_pnl - m_daily_pnl;
        if (m_daily_pnl > m_peak_pnl) m_peak_pnl = m_daily_pnl;
        return true;
    }

    std::vector<TradeRecord> snapshot() const
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_trades;
    }

    double dailyPnl() const {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_daily_pnl;
    }
    double peakDailyPnl() const {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_peak_pnl;
    }
    double cumulativePnl() const {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_cumulative_pnl;
    }
    double grossDailyPnl() const {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_gross_daily_pnl;
    }
    double maxDD() const {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_max_dd;
    }
    int wins() const {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_wins;
    }
    int losses() const {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_losses;
    }
    int total() const {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_wins + m_losses;
    }
    double winRate() const {
        std::lock_guard<std::mutex> lk(m_mtx);
        const int t = m_wins + m_losses;
        return t > 0 ? (100.0 * m_wins / t) : 0.0;
    }
    double avgWin() const {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_wins > 0 ? (m_sum_win / m_wins) : 0.0;
    }
    double avgLoss() const {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_losses > 0 ? (m_sum_loss / m_losses) : 0.0;
    }

    // ?? 2026-05-09 BROKER RECONCILIATION ACCESSORS ???????????????????????????
    // Today's truth-of-state methods. Distinguish what the engine THINKS from
    // what the BROKER actually executed. See TradeRecord broker_* field
    // comments for the full design rationale.

    // Sum of broker_pnl across all trades where both legs filled at the
    // broker. This is the REAL realised P&L on the live account. Excludes
    // shadow trades (paper) and orphans (only one leg filled). USD.
    double brokerRealisedPnl() const {
        std::lock_guard<std::mutex> lk(m_mtx);
        double sum = 0.0;
        for (const auto& tr : m_trades) {
            if (tr.shadow) continue;
            if (tr.broker_entry_filled && tr.broker_close_filled) {
                sum += tr.broker_pnl;
            }
        }
        return sum;
    }

    // Count of trades where exactly one leg filled at the broker. These are
    // the dangerous ones -- entry filled but close didn't (or vice versa)
    // means a position is open at the broker that the engine thinks is
    // closed. The NZ$459 hedging incident pattern.
    int brokerOrphanCount() const {
        std::lock_guard<std::mutex> lk(m_mtx);
        int n = 0;
        for (const auto& tr : m_trades) {
            if (tr.shadow) continue;
            const bool e = tr.broker_entry_filled;
            const bool c = tr.broker_close_filled;
            if (e != c) ++n;   // XOR: exactly one filled
        }
        return n;
    }

    // Count of trades where any leg was explicitly rejected by the broker.
    // The TRADING_BAD_VOLUME incident this morning would have set this
    // counter to ~30 and made the failure mode immediately visible.
    int brokerRejectCount() const {
        std::lock_guard<std::mutex> lk(m_mtx);
        int n = 0;
        for (const auto& tr : m_trades) {
            if (tr.shadow) continue;
            if (tr.broker_entry_rejected || tr.broker_close_rejected) ++n;
        }
        return n;
    }

    // Count of trades where both legs filled cleanly at broker -- the
    // "real wins/losses" count.
    int brokerConfirmedCount() const {
        std::lock_guard<std::mutex> lk(m_mtx);
        int n = 0;
        for (const auto& tr : m_trades) {
            if (tr.shadow) continue;
            if (tr.broker_entry_filled && tr.broker_close_filled) ++n;
        }
        return n;
    }

    // Engine-side P&L for non-shadow trades (what the engine THOUGHT it
    // earned on supposedly-live trades). Used as the "paper" comparator.
    double engineLivePnl() const {
        std::lock_guard<std::mutex> lk(m_mtx);
        double sum = 0.0;
        for (const auto& tr : m_trades) {
            if (tr.shadow) continue;
            sum += tr.net_pnl != 0.0 ? tr.net_pnl : tr.pnl;
        }
        return sum;
    }

    // Disparity between engine view and broker truth (engine - broker).
    // Positive = engine is overstating gains. Triggers the auto-shadow
    // safety circuit at >$30 in the entry path.
    double disparity() const {
        return engineLivePnl() - brokerRealisedPnl();
    }

    // 2026-05-11 S26 §2.2: count of non-shadow ("live-marked") closed trades
    // in the ledger. Used by the broker-reconciliation plausibility check in
    // OmegaTelemetryServer.cpp to detect the failure mode where the engine
    // claims live trades but the broker has none of them on its account
    // (root cause of the 2026-05-08..-11 demo-under-LIVE incident -- the
    // engine had ~thousands of trades, broker account 8077780 had zero).
    int engineLiveTradeCount() const {
        std::lock_guard<std::mutex> lk(m_mtx);
        int n = 0;
        for (const auto& tr : m_trades) {
            if (!tr.shadow) ++n;
        }
        return n;
    }

    // Find a trade by clOrdId on either leg. Returns pointer or nullptr.
    // CALLER MUST HOLD m_mtx (or call findTradeByClOrdIdLocked which does).
    // Used by handle_execution_report to update broker_* fields.
    TradeRecord* findTradeByClOrdIdLocked(const std::string& clOrdId, bool& is_entry) {
        for (auto& tr : m_trades) {
            if (!clOrdId.empty() && tr.entry_clOrdId == clOrdId) { is_entry = true;  return &tr; }
            if (!clOrdId.empty() && tr.close_clOrdId == clOrdId) { is_entry = false; return &tr; }
        }
        return nullptr;
    }

    // Apply a broker fill confirmation. Thread-safe; takes the lock.
    // is_entry: true = entry leg, false = close leg.
    // fill_px from FIX tag 6 (AvgPx) preferred, tag 31 (LastPx) fallback;
    //   resolution happens in the caller (handle_execution_report).
    // When both legs become filled, computes broker_pnl from fill prices.
    // Returns true if the trade was found and updated.
    //
    // 2026-05-11 S26 §2.0a/§2.0b (CORRECTED 2026-05-12 per Part 1B handoff):
    //   On entry- and close-leg fills, we update the user-visible trade
    //   fields from broker truth so the dashboard's Recent Trades row and
    //   the [TRADE-COST] line match what actually happened on the live
    //   account. The engine's previously-stored exitPrice was the TP
    //   target, not the actual fill -- 6 trades on the morning of
    //   2026-05-11 logged as wins (engine target = TP) were actually
    //   losses (broker fill below TP). See HANDOFF_S26.md §2.0 for the
    //   incident and HANDOFF_S26_PART1B_VERIFICATION_REBUILD.md §3 for
    //   the bug in the prior implementation (which recomputed tr->pnl
    //   from broker fills end-to-end and then ALSO subtracted slip_exit,
    //   double-counting cost and reporting ~10x the real loss).
    //
    //   The contract this implementation holds:
    //     tr.pnl          = engine's intended gross (entry_intent → exit_intent)
    //     tr.slippage_*   = USD cost of (broker_actual − engine_intent)
    //     tr.net_pnl      = pnl − slip_entry − slip_exit − commission
    //     tr.broker_pnl   = round-trip from broker fills only (independent)
    //
    //   Entry-leg update sequence (when broker entry ExecReport arrives):
    //     - Overwrite tr->slippage_entry with measured entry slip cost,
    //       replacing the synthetic half-spread from apply_realistic_costs.
    //     - Recompute tr->net_pnl with the apply_realistic_costs formula.
    //     - Emit a [TRADE-COST-RECON-ENTRY] log line for forensics.
    //
    //   Close-leg update sequence (when broker close ExecReport arrives):
    //     (1) Capture engine's intended close (target_close = tr->exitPrice).
    //     (2) Overwrite tr->exitPrice with the broker's actual fill.
    //     (3) Compute slip_units in price-space, signed so that a worse-
    //         than-target fill is positive cost (matches the existing
    //         slippage_exit cost convention from apply_realistic_costs).
    //         LONG : cost_units = target - actual (lower close is worse)
    //         SHORT: cost_units = actual - target (higher close is worse)
    //     (4) Convert to USD via size * tick_mult and overwrite
    //         tr->slippage_exit. This may now be NEGATIVE on a price-
    //         improvement (broker fill better than target) -- that is a
    //         genuine credit and should reduce reported costs accordingly.
    //     (5) DO NOT recompute tr->pnl from broker fills here. The
    //         codebase contract is that tr.pnl is the engine's intended
    //         gross. Recomputing it would double-count the slip already
    //         applied in step (4) when (6) subtracts it again.
    //     (6) Recompute net_pnl = pnl - slippage_entry - slippage_exit
    //         - commission, same formula apply_realistic_costs uses.
    //         engineLivePnl() reflects broker truth on confirmed trades
    //         and disparity() collapses to ~0 on cleanly-filled trades.
    //         Trades the broker never confirmed keep engine-only pnl, so
    //         disparity rises and §2.0c / §2.2 monitors catch divergence.
    //     (7) Emit a one-line [TRADE-COST-RECONCILED] log so the operator
    //         has forensic visibility on the divergence in real time.
    //         Distinct prefix from the prior [TRADE-COST] line emitted in
    //         handle_closed_trade (which fires before the broker reports
    //         back) so log-greppers can compare engine prediction vs
    //         broker truth side-by-side.
    bool applyBrokerFill(const std::string& clOrdId, double fill_px,
                         double tick_mult)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        bool is_entry = false;
        TradeRecord* tr = findTradeByClOrdIdLocked(clOrdId, is_entry);
        if (!tr) return false;
        const bool was_long = (tr->side == "LONG");
        if (is_entry) {
            tr->broker_entry_filled  = true;
            tr->broker_entry_fill_px = fill_px;

            // ── S26 §2.0a/§2.0b ENTRY-SIDE: real slip vs engine intent ─────
            // Symmetric to the close-side update below. Replaces the modeled
            // half-spread that apply_realistic_costs put into slippage_entry
            // with the actual measured cost of (broker fill − engine intent).
            // Sign convention: positive = adverse (cost), negative = credit.
            //   LONG entry: paying higher than intent is adverse.
            //   SHORT entry: receiving lower than intent is adverse.
            const double entry_target = tr->entryPrice;
            const double entry_actual = fill_px;
            const double entry_slip_units = was_long
                ? (entry_actual - entry_target)
                : (entry_target - entry_actual);
            const double prior_slip_in = tr->slippage_entry;
            tr->slippage_entry = entry_slip_units * tr->size * tick_mult;
            tr->net_pnl = tr->pnl - tr->slippage_entry - tr->slippage_exit
                                  - tr->commission;
            std::printf("[TRADE-COST-RECON-ENTRY] %s %s id=%d"
                        " target_entry=%.4f actual_entry=%.4f"
                        " slip_units=%.4f slip_usd=%.4f"
                        " (was slip_in=%.4f) net_pnl=%.4f"
                        " size=%.4f side=%s\n",
                        tr->symbol.c_str(), tr->engine.c_str(), tr->id,
                        entry_target, entry_actual,
                        entry_slip_units, tr->slippage_entry,
                        prior_slip_in, tr->net_pnl,
                        tr->size, tr->side.c_str());
            std::fflush(stdout);
        } else {
            tr->broker_close_filled  = true;
            tr->broker_close_fill_px = fill_px;

            // ── S26 §2.0a/§2.0b CLOSE-SIDE ─────────────────────────────────
            // (1) target_close = engine's intended exit (TP/SL/scratch level
            //     the engine wrote into tr->exitPrice when it sent the close)
            // (2) overwrite tr->exitPrice with broker actual so the dashboard
            //     Recent Trades row shows broker truth not engine target
            // (3) measured exit slip (positive = adverse for the trade
            //     direction; LONG closing lower is adverse, SHORT closing
            //     higher is adverse)
            // (4) overwrite tr->slippage_exit with USD cost; may be negative
            //     on price-improvement (genuine credit; do not floor at 0)
            // (5) DO NOT recompute tr->pnl from broker fills here -- the
            //     codebase contract is tr.pnl = engine's intended gross,
            //     net = pnl - slip_in - slip_out - commission. Recomputing
            //     pnl from broker fills would double-count the slip we just
            //     measured and report ~10x the real loss. The S26 P1 code
            //     prior to this correction had that bug.
            // (6) recompute net_pnl with the existing apply_realistic_costs
            //     formula so the dashboard Net column reflects broker truth
            //     to within ~commission accuracy
            // (7) forensic log line so [TRADE-COST] (engine prediction) and
            //     [TRADE-COST-RECONCILED] (broker truth) can be diff'd in
            //     latest.log
            const double target_close = tr->exitPrice;
            const double actual_close = fill_px;
            const double slip_units = was_long
                ? (target_close - actual_close)
                : (actual_close - target_close);
            const double slip_out_usd = slip_units * tr->size * tick_mult;
            const double prior_slip_out = tr->slippage_exit;
            tr->exitPrice    = actual_close;
            tr->slippage_exit = slip_out_usd;
            tr->net_pnl = tr->pnl - tr->slippage_entry - tr->slippage_exit
                                  - tr->commission;
            std::printf("[TRADE-COST-RECONCILED] %s %s id=%d"
                        " target_close=%.4f actual_close=%.4f"
                        " slip_units=%.4f slip_usd=%.4f"
                        " (was slip_out=%.4f) net_pnl=%.4f"
                        " entry=%.4f size=%.4f side=%s\n",
                        tr->symbol.c_str(), tr->engine.c_str(), tr->id,
                        target_close, actual_close,
                        slip_units, slip_out_usd,
                        prior_slip_out, tr->net_pnl,
                        tr->entryPrice, tr->size, tr->side.c_str());
            std::fflush(stdout);
        }
        // Compute broker-truth round-trip pnl when both legs are filled.
        // This is independent of the engine-intent pnl path above -- it is
        // the broker's view, used by brokerRealisedPnl() / disparity().
        if (tr->broker_entry_filled && tr->broker_close_filled
            && tr->broker_entry_fill_px > 0.0
            && tr->broker_close_fill_px > 0.0)
        {
            const double diff = was_long
                ? (tr->broker_close_fill_px - tr->broker_entry_fill_px)
                : (tr->broker_entry_fill_px - tr->broker_close_fill_px);
            tr->broker_pnl = diff * tr->size * tick_mult;
        }
        return true;
    }

    // Apply a broker rejection. Thread-safe.
    bool applyBrokerReject(const std::string& clOrdId)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        bool is_entry = false;
        TradeRecord* tr = findTradeByClOrdIdLocked(clOrdId, is_entry);
        if (!tr) return false;
        if (is_entry) tr->broker_entry_rejected = true;
        else          tr->broker_close_rejected = true;
        return true;
    }

    // Stamp an entry clOrdId onto the most recent matching trade.
    // Used by engine dispatch sites right after send_live_order returns.
    bool stampEntryClOrdId(int trade_id, const std::string& clOrdId)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        for (auto it = m_trades.rbegin(); it != m_trades.rend(); ++it) {
            if (it->id == trade_id) { it->entry_clOrdId = clOrdId; return true; }
        }
        return false;
    }
    bool stampCloseClOrdId(int trade_id, const std::string& clOrdId)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        for (auto it = m_trades.rbegin(); it != m_trades.rend(); ++it) {
            if (it->id == trade_id) { it->close_clOrdId = clOrdId; return true; }
        }
        return false;
    }

    // Clear dedup set -- called on daily reset so new-day trades aren't blocked
    // by prior-day keys. cumulative_pnl intentionally NOT reset (true equity).
    void resetDaily()
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_trades.clear();
        m_daily_pnl = 0; m_gross_daily_pnl = 0; m_peak_pnl = 0; m_max_dd = 0;
        m_wins = 0; m_losses = 0;
        m_sum_win = 0; m_sum_loss = 0;
        m_seen_keys.clear();  // allow same-entryTs trades to re-record on new day
    }

private:
    mutable std::mutex        m_mtx;
    std::vector<TradeRecord>  m_trades;
    std::unordered_set<std::string> m_seen_keys;  // dedup: symbol|entryTs|engine|exitReason
    double m_daily_pnl       = 0;
    double m_gross_daily_pnl = 0;
    double m_cumulative_pnl  = 0;  // never reset -- survives daily rollover
    double m_peak_pnl        = 0;
    double m_max_dd          = 0;
    int    m_wins      = 0;
    int    m_losses    = 0;
    double m_sum_win   = 0;
    double m_sum_loss  = 0;
};

} // namespace omega
