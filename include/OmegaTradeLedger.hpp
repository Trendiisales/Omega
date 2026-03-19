#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <chrono>
#include <cmath>

namespace omega {

// ─────────────────────────────────────────────────────────────────────────────
// Realistic cost model for shadow simulation.
// All values are per-unit (same unit as size).
//
// slippage_entry_pct  : one-way slippage on entry as % of entry price.
//                       Default 0.005% (~half-spread on liquid futures).
//                       Applied as adverse fill: LONG pays ask+slip, SHORT sells bid-slip.
// slippage_exit_pct   : one-way slippage on exit (TP fills at TP-slip LONG, SL at SL-slip LONG).
// commission_per_side : flat $ commission per side (entry + exit both charged).
//                       Default $0.0 (BlackBull CFD/futures — spread is the cost).
//                       Set to 3.50 for CME futures (round-turn $7).
//
// net_pnl = gross_pnl - total_slippage_cost - total_commission_cost
// ─────────────────────────────────────────────────────────────────────────────
struct TradeRecord
{
    int         id          = 0;
    std::string symbol;          // "US500.F", "USTEC.F", "GOLD.F", etc.
    std::string side;            // "LONG" / "SHORT"
    double      entryPrice  = 0;
    double      exitPrice   = 0;
    double      tp          = 0;
    double      sl          = 0;
    double      size        = 1.0;
    double      pnl         = 0;   // gross pnl (price difference × size, no costs)
    double      mfe         = 0;   // max favourable excursion (price units × size)
    double      mae         = 0;   // max adverse excursion
    int64_t     entryTs     = 0;   // unix seconds
    int64_t     exitTs      = 0;
    std::string exitReason;        // "TP_HIT" / "SL_HIT" / "TIMEOUT" / "FORCE_CLOSE" / "SCRATCH"
    double      spreadAtEntry = 0; // full bid-ask spread at entry (price units)
    double      latencyMs   = 0;
    std::string engine      = "BreakoutEngine";
    std::string regime;            // macro regime at time of trade

    // ── Realistic cost fields (populated by apply_realistic_costs()) ─────────
    double      slippage_entry  = 0;  // $ slippage cost on entry (adverse fill vs mid)
    double      slippage_exit   = 0;  // $ slippage cost on exit
    double      commission      = 0;  // $ total commission (both sides)
    double      net_pnl         = 0;  // pnl - slippage_entry - slippage_exit - commission

    // ── Convenience: cost parameters used (set by apply_realistic_costs()) ───
    double      slip_entry_pct  = 0;  // entry slippage rate applied
    double      slip_exit_pct   = 0;  // exit slippage rate applied
    double      comm_per_side   = 0;  // commission per side applied
};

// ─────────────────────────────────────────────────────────────────────────────
// apply_realistic_costs — fills TradeRecord cost fields for shadow simulation.
//
// CALL ORDER: invoke this AFTER tr.pnl has been scaled to USD by tick_mult.
// All output cost fields (slippage_entry, slippage_exit, commission, net_pnl)
// are in USD.
//
// tick_mult: dollar value per price-point per lot from tick_value_multiplier().
//   e.g. XAGUSD=5000, USOIL.F=1000, GOLD.F=100, US500.F=1, EURUSD=100000
//   This is required to convert price-point slippage into USD correctly.
//   Without it, slippage on a $5000/pt instrument would be ~$0.08 instead of
//   ~$8, making net_pnl ≈ gross_pnl and the cost model meaningless.
//
// Instrument slippage presets (one-way, % of price):
//   GOLD.F / XAGUSD          : 0.010% — gold/silver tick friction
//   USOIL.F / UKBRENT        : 0.012% — oil slightly wider intraday
//   USTEC.F / US500.F / DJ30 : 0.006% — liquid equity index futures
//   GER30 / UK100 / ESTX50   : 0.008% — European index CFDs, slightly wider
//   EURUSD                   : 0.003% — FX major, very liquid
//   default                  : 0.008%
//
// Commission: 0.0 per side (BlackBull CFD — cost is embedded in spread).
// ─────────────────────────────────────────────────────────────────────────────
inline void apply_realistic_costs(TradeRecord& tr,
                                  double commission_per_side,
                                  double tick_mult)
{
    tr.slip_entry_pct = 0.0;  // unused — cost now derived from spreadAtEntry directly
    tr.slip_exit_pct  = 0.0;  // unused — exit has no cost gap (bid/ask used for TP/SL)
    tr.comm_per_side  = commission_per_side;

    // ── Exact cost model derived from engine mechanics ───────────────────────
    // Entry: engine records pos.entry = mid. Real fill on LONG = ask = mid + spread/2.
    //        Cost = spread_at_entry/2 × tick_mult × size.
    //
    // Exit:  TP fires when bid >= TP level → closePos(pos.tp) → exit recorded at pos.tp.
    //        SL fires when bid <= SL level → closePos(pos.sl) → exit recorded at pos.sl.
    //        Real fill = bid at that moment = pos.tp or pos.sl exactly.
    //        Shadow exit = pos.tp or pos.sl exactly.
    //        No cost gap on exit — engine already uses aggressive bid/ask for checks.
    //
    // For non-index instruments (FX, commodities) the engine also uses mid for entry,
    // so the same half-spread model applies. The slip_pct approach approximated this
    // but overcounted on index CFDs (price level 46,000 × 0.006% = 2.8pts, exceeding TP).
    //
    // tr.spreadAtEntry is set at entry time (= ask - bid in price points).
    tr.slippage_entry = (tr.spreadAtEntry / 2.0) * tick_mult * tr.size;
    tr.slippage_exit  = 0.0;  // TP/SL checks use bid/ask — exit fill = TP/SL level exactly
    tr.commission     = commission_per_side * 2.0 * tr.size; // entry + exit, already in USD

    // tr.pnl must already be in USD before this call
    tr.net_pnl = tr.pnl - tr.slippage_entry - tr.slippage_exit - tr.commission;
}

class OmegaTradeLedger
{
public:
    void record(const TradeRecord& tr)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_trades.push_back(tr);
        // net_pnl is used for all stats (after slippage + commission)
        const double pnl_for_stats = (tr.net_pnl != 0.0 || tr.slippage_entry != 0.0)
                                     ? tr.net_pnl : tr.pnl;
        if (pnl_for_stats > 0) { m_wins++; m_sum_win += pnl_for_stats; }
        else                   { m_losses++; m_sum_loss += std::abs(pnl_for_stats); }
        m_daily_pnl       += pnl_for_stats;
        m_cumulative_pnl  += pnl_for_stats;  // never resets — true equity tracking
        m_gross_daily_pnl += tr.pnl;   // gross before costs — for display transparency
        if (m_daily_pnl - m_peak_pnl < -m_max_dd) m_max_dd = m_peak_pnl - m_daily_pnl;
        if (m_daily_pnl > m_peak_pnl) m_peak_pnl = m_daily_pnl;
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

    void resetDaily()
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_trades.clear();
        m_daily_pnl = 0; m_gross_daily_pnl = 0; m_peak_pnl = 0; m_max_dd = 0;
        m_wins = 0; m_losses = 0;
        m_sum_win = 0; m_sum_loss = 0;
    }

private:
    mutable std::mutex        m_mtx;
    std::vector<TradeRecord>  m_trades;
    double m_daily_pnl       = 0;
    double m_gross_daily_pnl = 0;
    double m_cumulative_pnl  = 0;  // never reset — survives daily rollover
    double m_peak_pnl        = 0;
    double m_max_dd          = 0;
    int    m_wins      = 0;
    int    m_losses    = 0;
    double m_sum_win   = 0;
    double m_sum_loss  = 0;
};

} // namespace omega
