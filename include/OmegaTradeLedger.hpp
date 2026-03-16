#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <chrono>
#include <cmath>

namespace omega {

struct TradeRecord
{
    int         id          = 0;
    std::string symbol;          // "MES", "MNQ", "MCL"
    std::string side;            // "LONG" / "SHORT"
    double      entryPrice  = 0;
    double      exitPrice   = 0;
    double      tp          = 0;
    double      sl          = 0;
    double      size        = 1.0;
    double      pnl         = 0;
    double      mfe         = 0;   // max favourable excursion
    double      mae         = 0;   // max adverse excursion
    int64_t     entryTs     = 0;   // unix seconds
    int64_t     exitTs      = 0;
    std::string exitReason;        // "TP_HIT" / "SL_HIT" / "TIMEOUT" / "FORCE_CLOSE"
    double      spreadAtEntry = 0;
    double      latencyMs   = 0;
    std::string engine      = "BreakoutEngine";
    std::string regime;            // macro regime at time of trade
};

class OmegaTradeLedger
{
public:
    void record(const TradeRecord& tr)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_trades.push_back(tr);
        // Update stats
        if (tr.pnl > 0) { m_wins++; m_sum_win += tr.pnl; }
        else             { m_losses++; m_sum_loss += std::abs(tr.pnl); }
        m_daily_pnl += tr.pnl;
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
        m_daily_pnl = 0; m_peak_pnl = 0; m_max_dd = 0;
        m_wins = 0; m_losses = 0;
        m_sum_win = 0; m_sum_loss = 0;
    }

private:
    mutable std::mutex        m_mtx;
    std::vector<TradeRecord>  m_trades;
    double m_daily_pnl = 0;
    double m_peak_pnl  = 0;
    double m_max_dd    = 0;
    int    m_wins      = 0;
    int    m_losses    = 0;
    double m_sum_win   = 0;
    double m_sum_loss  = 0;
};

} // namespace omega
