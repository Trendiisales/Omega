#pragma once
// logging.hpp -- extracted from main.cpp
// Section: logging (original lines 2857-3042)
// SINGLE-TRANSLATION-UNIT include -- only include from main.cpp

static void rtt_record(double ms) {
    g_rtt_last = ms;
    g_rtts.push_back(ms);
    if (g_rtts.size() > 200u) g_rtts.pop_front();
    std::vector<double> v(g_rtts.begin(), g_rtts.end());
    std::sort(v.begin(), v.end());
    g_rtt_p50 = v[std::min(static_cast<size_t>(v.size() * 0.50), v.size() - 1)];
    g_rtt_p95 = v[std::min(static_cast<size_t>(v.size() * 0.95), v.size() - 1)];
}

// ?????????????????????????????????????????????????????????????????????????????
// Shadow CSV
// ?????????????????????????????????????????????????????????????????????????????
static std::mutex g_shadow_csv_mtx;
static void write_shadow_csv(const omega::TradeRecord& tr) {
    std::lock_guard<std::mutex> lk(g_shadow_csv_mtx);
    if (g_shadow_csv.is_open()) {
        g_shadow_csv << tr.entryTs << ',' << tr.symbol << ',' << tr.side
                     << ',' << tr.engine
                     << ',' << tr.entryPrice << ',' << tr.exitPrice
                     << ',' << tr.net_pnl   // net after slippage+commission (was tr.pnl = gross)
                     << ',' << tr.mfe << ',' << tr.mae
                     << ',' << (tr.exitTs - tr.entryTs)
                     << ',' << tr.exitReason
                     << ',' << tr.spreadAtEntry
                     << ',' << tr.latencyMs
                     << ',' << tr.regime << '\n';
        g_shadow_csv.flush();
    }
    if (g_daily_shadow_trade_log) {
        const int64_t bucket_ts = nowSec();  // wall-clock for file date
        g_daily_shadow_trade_log->append_row(bucket_ts, build_trade_close_csv_row(tr));
    }
}

// ?? Trade open logger ?????????????????????????????????????????????????????????
// Called at the moment of entry -- before any close event.
// Provides an audit trail of every position opened with full context.
// Uses the same TradeRecord struct populated at entry time (exitPrice/pnl are 0).
static std::mutex g_trade_open_csv_mtx;

static void write_trade_open_log(const std::string& symbol,
                                  const std::string& engine,
                                  const std::string& side,
                                  double entry_px,
                                  double tp,
                                  double sl,
                                  double size,
                                  double spread_at_entry,
                                  const std::string& regime,
                                  const std::string& reason)
{
    const int64_t ts   = nowSec();
    const std::string ts_utc = utc_iso8601(ts);
    const std::string day    = utc_weekday_name(ts);

    // Build CSV row
    std::ostringstream o;
    o << ts
      << ',' << csv_quote(ts_utc)
      << ',' << csv_quote(day)
      << ',' << csv_quote(symbol)
      << ',' << csv_quote(engine)
      << ',' << csv_quote(side)
      << std::fixed << std::setprecision(4)
      << ',' << entry_px
      << ',' << tp
      << ',' << sl
      << ',' << size
      << std::setprecision(4)
      << ',' << spread_at_entry
      << ',' << csv_quote(regime)
      << ',' << csv_quote(reason);
    const std::string row = o.str();

    // Persistent file -- survives restarts via append mode
    {
        std::lock_guard<std::mutex> lk(g_trade_open_csv_mtx);
        if (g_trade_open_csv.is_open()) {
            g_trade_open_csv << row << '\n';
            g_trade_open_csv.flush();
        }
    }
    // Daily rolling log
    if (g_daily_trade_open_log)
        g_daily_trade_open_log->append_row(ts, row);

    printf("[TRADE-OPEN] %s %s %s entry=%.4f tp=%.4f sl=%.4f size=%.4f spread=%.4f regime=%s reason=%s\n",
           symbol.c_str(), side.c_str(), engine.c_str(),
           entry_px, tp, sl, size, spread_at_entry,
           regime.c_str(), reason.c_str());
    fflush(stdout);
}

static std::string trade_ref_from_record(const omega::TradeRecord& tr) {
    std::ostringstream o;
    o << tr.symbol << '|' << tr.entryTs << '|' << tr.id << '|' << tr.engine;
    return o.str();
}

static std::string build_trade_close_csv_row(const omega::TradeRecord& tr) {
    const int64_t hold_sec = std::max<int64_t>(0, tr.exitTs - tr.entryTs);
    std::ostringstream o;
    o << tr.id
      << ',' << csv_quote(trade_ref_from_record(tr))
      << ',' << tr.entryTs
      << ',' << csv_quote(utc_iso8601(tr.entryTs))
      << ',' << csv_quote(utc_weekday_name(tr.entryTs))
      << ',' << tr.exitTs
      << ',' << csv_quote(utc_iso8601(tr.exitTs))
      << ',' << csv_quote(utc_weekday_name(tr.exitTs))
      << ',' << csv_quote(tr.symbol)
      << ',' << csv_quote(tr.engine)
      << ',' << csv_quote(tr.side)
      << std::fixed << std::setprecision(4)
      << ',' << tr.entryPrice
      << ',' << tr.exitPrice
      << ',' << tr.tp
      << ',' << tr.sl
      << ',' << tr.size
      << ',' << tr.pnl          // gross
      << ',' << tr.net_pnl      // net (after slippage + commission)
      << ',' << tr.slippage_entry
      << ',' << tr.slippage_exit
      << ',' << tr.commission
      << std::setprecision(6)
      << ',' << tr.slip_entry_pct
      << ',' << tr.slip_exit_pct
      << ',' << tr.comm_per_side
      << std::fixed << std::setprecision(4)
      << ',' << tr.mfe
      << ',' << tr.mae
      << ',' << hold_sec
      << ',' << tr.spreadAtEntry
      << ',' << tr.latencyMs
      << ',' << csv_quote(tr.regime)
      << ',' << csv_quote(tr.exitReason)
      << std::setprecision(4)
      << ',' << tr.l2_imbalance
      << ',' << (tr.l2_live ? 1 : 0);
    return o.str();
}

static void write_trade_close_logs(const omega::TradeRecord& tr) {
    const std::string row = build_trade_close_csv_row(tr);
    {
        std::lock_guard<std::mutex> lk(g_trade_close_csv_mtx);
        if (g_trade_close_csv.is_open()) {
            g_trade_close_csv << row << '\n';
            g_trade_close_csv.flush();
        }
    }
    const int64_t bucket_ts = nowSec();  // always use wall-clock for file date -- exitTs could be stale on reload
    if (g_daily_trade_close_log) g_daily_trade_close_log->append_row(bucket_ts, row);
    if (tr.symbol == "XAUUSD" && g_daily_gold_trade_close_log)
        g_daily_gold_trade_close_log->append_row(bucket_ts, row);
}

// ?????????????????????????????????????????????????????????????????????????????
// session_tradeable() -- UTC hour gate for all engine entries
//
// TWO WINDOWS are evaluated. Either window active = trading allowed.
//
// PRIMARY WINDOW  (config: session_start_utc ? session_end_utc)
//   Default: 07:00-22:00 UTC  (London open ? NY close + 1hr buffer)
//   Covers:  London 07:00-16:00, NY 13:00-22:00
//   Supports wrap-through-midnight if start > end (e.g. 22?5)
//   Set start == end to run 24h (not recommended)
//
// ASIA WINDOW  (config: session_asia=true)
//   Fixed:  22:00-05:00 UTC  (Tokyo gold market + NZ/AU morning)
//   Active for gold, silver, oil -- all trade during Tokyo hours
//   Hardcoded range -- not affected by primary window values
//
// DEAD ZONE (intentional gap):
//   05:00-07:00 UTC -- Sydney close to London open
//   Genuinely thin liquidity, wide spreads, no engine runs here
//
// COVERAGE MAP:
//   00:00-05:00  Asia window active      ? trading
//   05:00-07:00  DEAD ZONE               ? blocked
//   07:00-22:00  Primary window active   ? trading
//   22:00-24:00  Asia window active      ? trading
//
// BUG HISTORY:
//   session_end_utc was 21 -- created a silent 21:00-22:00 dead zone each night.
//   Gold, oil, silver, forex all blocked for 1hr despite being open markets.
//   Fixed: session_end_utc raised to 22 -- primary window now hands off directly
//   to Asia window with no gap.
