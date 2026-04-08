// goldflow_diag.cpp
// GoldFlow diagnostic backtest — exit reason breakdown + per-trade CSV
// Build: g++ -O2 -std=c++17 -o goldflow_diag goldflow_diag.cpp
// Run:   ./goldflow_diag ticks.csv [trades_out.csv]
//
// Exit reasons:
//   TP_HIT        — target reached
//   SL_HIT        — stop hit
//   ADVERSE_EARLY — price moved adversely within first ADVERSE_WINDOW ticks after entry
//   TIME_STOP     — held past TIME_LIMIT_MS with no resolution
//   TRAIL_HIT     — trailing stop triggered (if trail enabled)
//
// CALIBRATION (from gate diagnostics on 134M XAUUSD ticks):
//   - VWAP delta p50=0.0017, p99=0.0056 => VWAP_TREND_PTS=0.003 (~p80)
//   - WINDOW=600 ticks (~1 real minute) — 60-tick window is sub-second noise
//   - VWAP: daily-reset cumulative (matches live engine)
//   - Cooldown set on entry (duplicate entry fix)
//
// VERSION HISTORY:
//   v19: London+NY baseline +$13,761 100 trades 59% WR
//   v22: IMPULSE_MIN=8 — over-filtered → 41 trades +$1,458
//   v23: IMPULSE_MIN=6 + MIN_PB_DEPTH=1.5 + per-session VWAP_TREND → 59 trades +$12,444 64% WR
//   v24: MAX_PB_DEPTH=5.0 global — pb correlation not causation → 36 trades -$2,232
//   v25: v23 + SL=5.5 + ADVERSE_MIN=3.0 → exit tightening backfired → 59 trades +$10,683
//   v26: NY-only MAX_PB_DEPTH=5.0 — revealed NY pb 3-5pt WR=42% -$5,070 → 47 trades +$636
//        London pb 3-5pt WR=100% +$5,063. NY pb 3-5pt (all remaining NY) -$5,070.
//        CONCLUSION: NY is structurally bad at these params regardless of pb filter.
//        London: consistent +$5-8k, 64-68% WR across all versions.
//        NY never cleanly positive once isolated.
// v27: LONDON ONLY. NY disabled.
//      GoldFlow edge = London 07:00-10:00 UTC pullback-retracement on impulse.
//      NY moves are too sharp/fast — by pullback entry time, impulse is stale.
//      MacroCrash handles explosive moves; GoldFlow handles London structure.

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <cmath>
#include <chrono>
#include <cctype>
#include <algorithm>
#include <iomanip>

// ─────────────────────────────────────────────
// Tick
// ─────────────────────────────────────────────
struct Tick
{
    uint64_t ts   = 0;
    double   ask  = 0;
    double   bid  = 0;
};

bool parse_tick(const std::string& line, Tick& t)
{
    if (line.empty()) return false;
    std::stringstream ss(line);
    std::string tok;

    if (!getline(ss, tok, ',')) return false;
    if (tok.empty() || !isdigit((unsigned char)tok[0])) return false;

    try { t.ts = std::stoull(tok); }
    catch (...) { return false; }

    if (!getline(ss, tok, ',')) return false;
    try { t.ask = std::stod(tok); }
    catch (...) { return false; }

    if (!getline(ss, tok, ',')) return false;
    try { t.bid = std::stod(tok); }
    catch (...) { return false; }

    return (t.ask > 0 && t.bid > 0 && t.ask >= t.bid);
}

// ─────────────────────────────────────────────
// Session filter (UTC)
// ─────────────────────────────────────────────
// GoldFlow active session: London 07:00-10:00 UTC only
// Asia:   disabled (MacroCrash handles Asia breakouts)
// NY:     disabled (v26 proved NY is structurally bad for this strategy)
inline int utc_hour(uint64_t ts_ms)
{
    return (int)((ts_ms / 1000 / 3600) % 24);
}

inline uint64_t utc_day(uint64_t ts_ms)
{
    return ts_ms / 1000 / 86400;
}

inline bool session_london(uint64_t ts_ms)
{
    int h = utc_hour(ts_ms);
    return (h >= 7 && h <= 10);
}

inline std::string session_name(uint64_t ts_ms)
{
    if (session_london(ts_ms)) return "LONDON";
    return "OFF";
}

// ─────────────────────────────────────────────
// Parameters
// ─────────────────────────────────────────────
// v27: London-only. All params from v23 (best parametric base).
// No NY_MAX_PB_DEPTH needed — NY disabled entirely.
// VWAP_TREND uses London threshold only.
static const int    WINDOW              = 600;
static const double IMPULSE_MIN         = 6.0;
static const double IMPULSE_MAX         = 15.0;
static const double TP_PTS              = 14.0;
static const double SL_PTS              = 7.0;
static const double PULLBACK_FRAC       = 0.50;
static const double VWAP_TREND_PTS      = 0.004;  // London only
static const int    VWAP_TREND_LOOK     = 30;
static const double MAX_SPREAD          = 0.40;
static const int    COOLDOWN_TICKS      = 300;
static const uint64_t TIME_LIMIT_MS     = 7200000;
static const uint64_t NO_TRAIL_MS       = 3600000;
static const int    ADVERSE_WINDOW      = 10;
static const double ADVERSE_MIN_PTS     = 4.0;
static const double MIN_PB_DEPTH        = 1.5;

// Trail
static const bool   TRAIL_ENABLED       = true;
static const double TRAIL_TRIGGER       = 6.0;
static const double TRAIL_LOCK          = 0.75;

static const double COMMISSION_PTS      = 0.0;

// ─────────────────────────────────────────────
// Trade record
// ─────────────────────────────────────────────
enum class ExitReason { NONE, TP_HIT, SL_HIT, ADVERSE_EARLY, TIME_STOP, TRAIL_HIT };

static const char* exit_name(ExitReason r)
{
    switch (r) {
        case ExitReason::TP_HIT:        return "TP_HIT";
        case ExitReason::SL_HIT:        return "SL_HIT";
        case ExitReason::ADVERSE_EARLY: return "ADVERSE_EARLY";
        case ExitReason::TIME_STOP:     return "TIME_STOP";
        case ExitReason::TRAIL_HIT:     return "TRAIL_HIT";
        default:                        return "NONE";
    }
}

struct TradeRecord
{
    int      id          = 0;
    bool     is_long     = false;
    double   entry       = 0;
    double   exit_price  = 0;
    double   tp          = 0;
    double   sl          = 0;
    double   spread_open = 0;
    double   impulse_sz  = 0;
    double   pb_depth    = 0;
    double   mfe         = 0;
    double   mae         = 0;
    double   gross_pnl   = 0;
    double   net_pnl     = 0;
    int      hold_ticks  = 0;
    uint64_t hold_ms     = 0;
    uint64_t entry_ts    = 0;
    ExitReason exit_why  = ExitReason::NONE;
    std::string session;
};

// ─────────────────────────────────────────────
// Engine state
// ─────────────────────────────────────────────
struct Engine
{
    std::vector<double> price_buf;
    std::vector<double> vwap_buf;

    // Daily-reset cumulative VWAP
    double   vwap       = 0;
    double   vwap_pv    = 0;
    uint64_t vwap_count = 0;
    uint64_t vwap_day   = 0;

    double hi = 0, lo = 0;

    bool     in_pos       = false;
    bool     is_long      = false;
    double   entry        = 0;
    double   tp           = 0;
    double   sl           = 0;
    double   mfe          = 0;
    double   mae          = 0;
    double   trail_sl     = 0;
    bool     trail_active = false;
    uint64_t entry_ts     = 0;

    int cooldown  = 0;
    int pos_ticks = 0;

    void update_vwap(double price, uint64_t ts_ms)
    {
        uint64_t day = utc_day(ts_ms);
        if (day != vwap_day) {
            vwap_pv    = 0.0;
            vwap_count = 0;
            vwap_day   = day;
        }
        vwap_pv    += price;
        vwap_count += 1;
        vwap        = vwap_pv / (double)vwap_count;

        vwap_buf.push_back(vwap);
        if ((int)vwap_buf.size() > std::max(WINDOW, VWAP_TREND_LOOK) + 10)
            vwap_buf.erase(vwap_buf.begin());
    }

    void update_price(double price)
    {
        price_buf.push_back(price);
        if ((int)price_buf.size() > WINDOW + 5)
            price_buf.erase(price_buf.begin());
    }

    bool detect_impulse()
    {
        if ((int)price_buf.size() < WINDOW) return false;
        int start = (int)price_buf.size() - WINDOW;
        hi = price_buf[start];
        lo = price_buf[start];
        for (int i = start; i < (int)price_buf.size(); i++) {
            hi = std::max(hi, price_buf[i]);
            lo = std::min(lo, price_buf[i]);
        }
        return (hi - lo) >= IMPULSE_MIN;
    }

    bool vwap_trend_up()
    {
        if ((int)vwap_buf.size() < VWAP_TREND_LOOK) return false;
        int n = (int)vwap_buf.size();
        return (vwap_buf[n-1] - vwap_buf[n - VWAP_TREND_LOOK]) > VWAP_TREND_PTS;
    }

    bool vwap_trend_down()
    {
        if ((int)vwap_buf.size() < VWAP_TREND_LOOK) return false;
        int n = (int)vwap_buf.size();
        return (vwap_buf[n-1] - vwap_buf[n - VWAP_TREND_LOOK]) < -VWAP_TREND_PTS;
    }
};

// ─────────────────────────────────────────────
// Stats aggregator
// ─────────────────────────────────────────────
struct Stats
{
    int    count         = 0;
    int    wins          = 0;
    double total_pnl     = 0;
    double total_mfe     = 0;
    double total_mae     = 0;
    double total_hold_ms = 0;
    double total_imp     = 0;
    double total_pb      = 0;

    void add(const TradeRecord& tr)
    {
        count++;
        total_pnl    += tr.net_pnl;
        total_mfe    += tr.mfe;
        total_mae    += tr.mae;
        total_hold_ms+= (double)tr.hold_ms;
        total_imp    += tr.impulse_sz;
        total_pb     += tr.pb_depth;
        if (tr.net_pnl > 0) wins++;
    }

    void print(const char* label) const
    {
        if (count == 0) { std::cout << "  " << label << ": 0 trades\n"; return; }
        double wr         = 100.0 * wins / count;
        double avg        = total_pnl / count;
        double avg_mfe    = total_mfe / count;
        double avg_mae    = total_mae / count;
        double avg_hold_s = total_hold_ms / count / 1000.0;
        std::cout
            << "  " << std::left << std::setw(16) << label
            << " n=" << std::setw(5) << count
            << " WR=" << std::fixed << std::setprecision(1) << std::setw(5) << wr << "%"
            << " PnL=" << std::setw(9) << std::setprecision(1) << total_pnl*100 << " USD"
            << " avg=" << std::setw(7) << std::setprecision(2) << avg << " pts"
            << " MFE=" << std::setw(5) << std::setprecision(1) << avg_mfe
            << " MAE=" << std::setw(5) << std::setprecision(1) << avg_mae
            << " hold=" << std::setprecision(0) << avg_hold_s << "s"
            << "\n";
    }

    void print_with_pb(const char* label) const
    {
        if (count == 0) { std::cout << "  " << label << ": 0 trades\n"; return; }
        double wr         = 100.0 * wins / count;
        double avg        = total_pnl / count;
        double avg_mfe    = total_mfe / count;
        double avg_mae    = total_mae / count;
        double avg_hold_s = total_hold_ms / count / 1000.0;
        double avg_imp    = total_imp / count;
        double avg_pb     = total_pb / count;
        std::cout
            << "  " << std::left << std::setw(16) << label
            << " n=" << std::setw(5) << count
            << " WR=" << std::fixed << std::setprecision(1) << std::setw(5) << wr << "%"
            << " PnL=" << std::setw(9) << std::setprecision(1) << total_pnl*100 << " USD"
            << " avg=" << std::setw(7) << std::setprecision(2) << avg << " pts"
            << " MFE=" << std::setw(5) << std::setprecision(1) << avg_mfe
            << " MAE=" << std::setw(5) << std::setprecision(1) << avg_mae
            << " imp=" << std::setw(5) << std::setprecision(1) << avg_imp
            << " pb=" << std::setw(5) << std::setprecision(2) << avg_pb
            << " hold=" << std::setprecision(0) << avg_hold_s << "s"
            << "\n";
    }
};

// ─────────────────────────────────────────────
// Session gate — London only
// ─────────────────────────────────────────────
inline bool trade_session_ok(uint64_t ts_ms)
{
    return session_london(ts_ms);
}

// ─────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────
int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cout << "usage: goldflow_diag ticks.csv [trades_out.csv]\n";
        return 0;
    }

    std::ifstream file(argv[1]);
    if (!file.is_open()) {
        std::cout << "cannot open: " << argv[1] << "\n";
        return 1;
    }

    std::ofstream csv_out;
    bool write_csv = (argc >= 3);
    if (write_csv) {
        csv_out.open(argv[2]);
        csv_out << "id,is_long,session,entry_ts,entry,exit,tp,sl,"
                << "spread_open,impulse_sz,pb_depth,mfe,mae,gross_pnl,net_pnl,"
                << "hold_ticks,hold_ms,exit_why\n";
    }

    Engine e;
    std::vector<TradeRecord> trades;
    TradeRecord cur;

    uint64_t ticks_total = 0;
    uint64_t ticks_used  = 0;

    std::string line;
    auto t_start = std::chrono::high_resolution_clock::now();
    int trade_id = 0;

    while (getline(file, line))
    {
        Tick t;
        if (!parse_tick(line, t)) continue;
        ticks_total++;

        double spread = t.ask - t.bid;
        double mid    = (t.ask + t.bid) * 0.5;

        // VWAP always updated (warm before session opens)
        e.update_vwap(mid, t.ts);
        e.update_price(mid);

        if (spread > MAX_SPREAD) continue;

        // Force close any open position when session ends
        if (e.in_pos && !trade_session_ok(t.ts))
        {
            cur.exit_price = e.is_long ? t.bid : t.ask;
            cur.gross_pnl  = e.is_long ? cur.exit_price - e.entry : e.entry - cur.exit_price;
            cur.net_pnl    = cur.gross_pnl - cur.spread_open - COMMISSION_PTS;
            cur.hold_ms    = t.ts - e.entry_ts;
            cur.exit_why   = ExitReason::TIME_STOP;
            cur.mfe        = std::max(0.0, cur.mfe);
            cur.mae        = std::max(0.0, cur.mae);
            trades.push_back(cur);
            e.in_pos = false; e.trail_active = false;
        }

        if (!trade_session_ok(t.ts)) continue;
        ticks_used++;

        if (e.cooldown > 0) { e.cooldown--; }

        // ── manage open position ──────────────────────────
        if (e.in_pos)
        {
            e.pos_ticks++;
            cur.hold_ticks++;

            double excursion = e.is_long ? mid - e.entry : e.entry - mid;
            if (excursion > cur.mfe) cur.mfe = excursion;
            if (excursion < -cur.mae) cur.mae = -excursion;

            // Trail update
            if (TRAIL_ENABLED) {
                if (!e.trail_active && cur.mfe >= TRAIL_TRIGGER)
                    e.trail_active = true;
                if (e.trail_active) {
                    double locked    = cur.mfe * TRAIL_LOCK;
                    double new_trail = e.is_long ? e.entry + locked : e.entry - locked;
                    if (e.is_long) e.trail_sl = std::max(e.trail_sl, new_trail);
                    else           e.trail_sl = std::min(e.trail_sl, new_trail);
                }
            }

            bool adverse_early    = (e.pos_ticks <= ADVERSE_WINDOW && cur.mae >= ADVERSE_MIN_PTS);
            bool no_trail_timeout = (!e.trail_active && (t.ts - e.entry_ts) >= NO_TRAIL_MS);

            ExitReason why   = ExitReason::NONE;
            double     exit_px = 0;

            if (e.is_long) {
                double px = t.bid;
                if      (px >= e.tp)                                    { why = ExitReason::TP_HIT;        exit_px = e.tp; }
                else if (adverse_early)                                  { why = ExitReason::ADVERSE_EARLY; exit_px = px; }
                else if (no_trail_timeout)                               { why = ExitReason::TIME_STOP;     exit_px = px; }
                else if (px <= e.sl)                                    { why = ExitReason::SL_HIT;        exit_px = e.sl; }
                else if (TRAIL_ENABLED && e.trail_active && px <= e.trail_sl) { why = ExitReason::TRAIL_HIT; exit_px = e.trail_sl; }
                else if (t.ts - e.entry_ts >= TIME_LIMIT_MS)            { why = ExitReason::TIME_STOP;     exit_px = px; }
            } else {
                double px = t.ask;
                if      (px <= e.tp)                                    { why = ExitReason::TP_HIT;        exit_px = e.tp; }
                else if (adverse_early)                                  { why = ExitReason::ADVERSE_EARLY; exit_px = px; }
                else if (no_trail_timeout)                               { why = ExitReason::TIME_STOP;     exit_px = px; }
                else if (px >= e.sl)                                    { why = ExitReason::SL_HIT;        exit_px = e.sl; }
                else if (TRAIL_ENABLED && e.trail_active && px >= e.trail_sl) { why = ExitReason::TRAIL_HIT; exit_px = e.trail_sl; }
                else if (t.ts - e.entry_ts >= TIME_LIMIT_MS)            { why = ExitReason::TIME_STOP;     exit_px = px; }
            }

            if (why != ExitReason::NONE)
            {
                cur.exit_price = exit_px;
                cur.gross_pnl  = e.is_long ? exit_px - e.entry : e.entry - exit_px;
                cur.net_pnl    = cur.gross_pnl - cur.spread_open - COMMISSION_PTS;
                cur.hold_ms    = t.ts - e.entry_ts;
                cur.exit_why   = why;
                cur.mfe        = std::max(0.0, cur.mfe);
                cur.mae        = std::max(0.0, cur.mae);

                trades.push_back(cur);

                if (write_csv) {
                    csv_out
                        << cur.id << ","
                        << (cur.is_long ? 1 : 0) << ","
                        << cur.session << ","
                        << cur.entry_ts << ","
                        << std::fixed << std::setprecision(2)
                        << cur.entry << "," << cur.exit_price << ","
                        << cur.tp << "," << cur.sl << ","
                        << cur.spread_open << "," << cur.impulse_sz << ","
                        << cur.pb_depth << ","
                        << cur.mfe << "," << cur.mae << ","
                        << cur.gross_pnl << "," << cur.net_pnl << ","
                        << cur.hold_ticks << "," << cur.hold_ms << ","
                        << exit_name(why) << "\n";
                }

                e.in_pos       = false;
                e.trail_active = false;
            }
        }

        // ── entry logic ──────────────────────────────────
        if (!e.in_pos && e.cooldown == 0 && e.detect_impulse())
        {
            double impulse  = e.hi - e.lo;
            if (impulse > IMPULSE_MAX) continue;

            double pb_long  = e.hi - PULLBACK_FRAC * impulse;
            double pb_short = e.lo + PULLBACK_FRAC * impulse;

            double pb_depth_long  = e.hi - mid;
            double pb_depth_short = mid - e.lo;

            bool pb_ok_long  = (pb_depth_long  >= MIN_PB_DEPTH);
            bool pb_ok_short = (pb_depth_short >= MIN_PB_DEPTH);

            bool can_long  = (mid <= pb_long  && mid > e.vwap && e.vwap_trend_up()   && pb_ok_long);
            bool can_short = (mid >= pb_short && mid < e.vwap && e.vwap_trend_down() && pb_ok_short);

            if (can_long || can_short)
            {
                e.in_pos  = true;
                e.is_long = can_long;

                if (e.is_long) {
                    e.entry    = t.ask;
                    e.tp       = e.entry + TP_PTS;
                    e.sl       = e.entry - SL_PTS;
                    e.trail_sl = e.entry - SL_PTS;
                } else {
                    e.entry    = t.bid;
                    e.tp       = e.entry - TP_PTS;
                    e.sl       = e.entry + SL_PTS;
                    e.trail_sl = e.entry + SL_PTS;
                }

                e.entry_ts     = t.ts;
                e.trail_active = false;
                e.cooldown     = COOLDOWN_TICKS;

                cur              = TradeRecord{};
                cur.id           = ++trade_id;
                cur.is_long      = e.is_long;
                cur.entry        = e.entry;
                cur.tp           = e.tp;
                cur.sl           = e.sl;
                cur.spread_open  = spread;
                cur.impulse_sz   = impulse;
                cur.pb_depth     = e.is_long ? pb_depth_long : pb_depth_short;
                cur.entry_ts     = t.ts;
                cur.session      = session_name(t.ts);
                e.pos_ticks      = 0;
                cur.mfe          = 0;
                cur.mae          = 0;
            }
        }
    }

    auto t_end   = std::chrono::high_resolution_clock::now();
    double runtime = std::chrono::duration<double>(t_end - t_start).count();

    // ─────────────────────────────────────────────
    // Aggregate stats
    // ─────────────────────────────────────────────
    Stats s_total, s_tp, s_sl, s_adverse, s_time, s_trail;

    for (const auto& tr : trades)
    {
        s_total.add(tr);
        switch (tr.exit_why) {
            case ExitReason::TP_HIT:        s_tp.add(tr);      break;
            case ExitReason::SL_HIT:        s_sl.add(tr);      break;
            case ExitReason::ADVERSE_EARLY: s_adverse.add(tr); break;
            case ExitReason::TIME_STOP:     s_time.add(tr);    break;
            case ExitReason::TRAIL_HIT:     s_trail.add(tr);   break;
            default: break;
        }
    }

    // ─────────────────────────────────────────────
    // Print report
    // ─────────────────────────────────────────────
    std::cout << "\n";
    std::cout << "══════════════════════════════════════════════════════════════\n";
    std::cout << "  GoldFlow Diagnostic Backtest  v27  [LONDON ONLY]\n";
    std::cout << "══════════════════════════════════════════════════════════════\n";
    std::cout << "  Ticks total   : " << ticks_total << "\n";
    std::cout << "  Ticks in-sess : " << ticks_used << "\n";
    std::cout << "  Runtime       : " << std::fixed << std::setprecision(2) << runtime << " s\n";
    std::cout << "\n";
    std::cout << "  Parameters:\n";
    std::cout << "    WINDOW=" << WINDOW << " IMPULSE=" << IMPULSE_MIN << "-" << IMPULSE_MAX
              << " TP=" << TP_PTS << " SL=" << SL_PTS << "\n";
    std::cout << "    PULLBACK_FRAC=" << PULLBACK_FRAC
              << " MIN_PB_DEPTH=" << MIN_PB_DEPTH << "pts\n";
    std::cout << "    VWAP_TREND=" << std::setprecision(4) << VWAP_TREND_PTS
              << " TIME_LIMIT=" << TIME_LIMIT_MS/1000 << "s\n";
    std::cout << "    ADVERSE_WINDOW=" << ADVERSE_WINDOW
              << " ADVERSE_MIN=" << ADVERSE_MIN_PTS << " pts\n";
    std::cout << "    VWAP=daily-reset-cumulative  SESSION=LONDON(07-10) ONLY  force-close-at-end\n";
    std::cout << "    TRAIL=" << (TRAIL_ENABLED ? "ON" : "OFF");
    if (TRAIL_ENABLED)
        std::cout << " trigger=" << TRAIL_TRIGGER << "pts lock=" << (int)(TRAIL_LOCK*100) << "%";
    std::cout << "\n\n";

    std::cout << "── By Exit Reason ────────────────────────────────────────────\n";
    s_total.print("TOTAL");
    s_tp.print("TP_HIT");
    s_sl.print("SL_HIT");
    s_adverse.print("ADVERSE_EARLY");
    s_time.print("TIME_STOP");
    if (TRAIL_ENABLED) s_trail.print("TRAIL_HIT");
    std::cout << "\n";

    // ADVERSE_EARLY deep-dive
    if (s_adverse.count > 0)
    {
        std::cout << "── ADVERSE_EARLY: Impulse Size Buckets ──────────────────────\n";
        std::vector<std::pair<std::string, std::pair<double,double>>> buckets = {
            {"<8pts",    {0,   8}},
            {"8-10pts",  {8,  10}},
            {"10-12pts", {10, 12}},
            {"12-15pts", {12, 15}},
        };
        for (auto& [label, range] : buckets) {
            int n = 0; double pnl = 0;
            for (const auto& tr : trades) {
                if (tr.exit_why != ExitReason::ADVERSE_EARLY) continue;
                if (tr.impulse_sz >= range.first && tr.impulse_sz < range.second) { n++; pnl += tr.net_pnl; }
            }
            if (n > 0)
                std::cout << "  " << std::left << std::setw(12) << label
                          << " n=" << std::setw(5) << n
                          << " PnL=" << std::setprecision(1) << pnl*100 << " USD\n";
        }
        std::cout << "\n";
    }

    // SL_HIT deep-dive
    if (s_sl.count > 0)
    {
        std::cout << "── SL_HIT: MFE Groups ────────────────────────────────────────\n";
        std::vector<std::pair<std::string, std::pair<double,double>>> mfe_groups = {
            {"MFE<1pts",  {0,   1}},
            {"MFE 1-3pts",{1,   3}},
            {"MFE 3-6pts",{3,   6}},
            {"MFE>6pts",  {6, 999}},
        };
        for (auto& [label, range] : mfe_groups) {
            int n = 0; double pnl = 0; double imp_sum = 0; double pb_sum = 0;
            for (const auto& tr : trades) {
                if (tr.exit_why != ExitReason::SL_HIT) continue;
                if (tr.mfe >= range.first && tr.mfe < range.second) {
                    n++; pnl += tr.net_pnl; imp_sum += tr.impulse_sz; pb_sum += tr.pb_depth;
                }
            }
            if (n > 0)
                std::cout << "  " << std::left << std::setw(14) << label
                          << " n=" << std::setw(5) << n
                          << " PnL=" << std::setprecision(1) << pnl*100 << " USD"
                          << " avg_imp=" << std::setprecision(1) << imp_sum/n
                          << " avg_pb=" << std::setprecision(2) << pb_sum/n << "\n";
        }
        std::cout << "\n";
    }

    // TIME_STOP deep-dive
    if (s_time.count > 0)
    {
        std::cout << "── TIME_STOP: MFE at timeout ─────────────────────────────────\n";
        std::vector<std::pair<std::string,std::pair<double,double>>> mfe_buckets = {
            {"MFE<2pts",  {0,  2}},
            {"MFE 2-5pts",{2,  5}},
            {"MFE 5-8pts",{5,  8}},
            {"MFE>8pts",  {8, 999}},
        };
        for (auto& [label, range] : mfe_buckets) {
            int cnt = 0; double pnl = 0;
            for (const auto& tr : trades) {
                if (tr.exit_why != ExitReason::TIME_STOP) continue;
                if (tr.mfe >= range.first && tr.mfe < range.second) { cnt++; pnl += tr.net_pnl; }
            }
            if (cnt > 0)
                std::cout << "  " << std::left << std::setw(14) << label
                          << " n=" << std::setw(5) << cnt
                          << " PnL=" << std::setprecision(1) << pnl*100 << " USD\n";
        }
        std::cout << "\n";
    }

    // TRAIL deep-dive
    if (TRAIL_ENABLED && s_trail.count > 0)
    {
        std::cout << "── TRAIL_HIT: MFE at trail exit ──────────────────────────────\n";
        std::vector<std::pair<std::string,std::pair<double,double>>> tmfe = {
            {"MFE 6-10pts", {6,  10}},
            {"MFE 10-15pts",{10, 15}},
            {"MFE 15-20pts",{15, 20}},
            {"MFE>20pts",   {20, 999}},
        };
        for (auto& [label, range] : tmfe) {
            int cnt = 0; double pnl = 0;
            for (const auto& tr : trades) {
                if (tr.exit_why != ExitReason::TRAIL_HIT) continue;
                if (tr.mfe >= range.first && tr.mfe < range.second) { cnt++; pnl += tr.net_pnl; }
            }
            if (cnt > 0)
                std::cout << "  " << std::left << std::setw(14) << label
                          << " n=" << std::setw(5) << cnt
                          << " PnL=" << std::setprecision(1) << pnl*100 << " USD\n";
        }
        std::cout << "\n";
    }

    // Pullback depth distribution
    std::cout << "── Pullback Depth at Entry (London) ──────────────────────────\n";
    std::vector<std::pair<std::string,std::pair<double,double>>> pb_buckets = {
        {"pb 1-3pt",  {1,  3}},
        {"pb 3-5pt",  {3,  5}},
        {"pb 5-7pt",  {5,  7}},
        {"pb 7-10pt", {7, 10}},
        {"pb>10pt",   {10,999}},
    };
    for (auto& [label, range] : pb_buckets) {
        int n = 0; double pnl = 0; int wins_pb = 0;
        for (const auto& tr : trades) {
            if (tr.pb_depth >= range.first && tr.pb_depth < range.second) {
                n++; pnl += tr.net_pnl;
                if (tr.net_pnl > 0) wins_pb++;
            }
        }
        if (n > 0)
            std::cout << "  " << std::left << std::setw(12) << label
                      << " n=" << std::setw(5) << n
                      << " WR=" << std::fixed << std::setprecision(1)
                      << 100.0*wins_pb/n << "%"
                      << " PnL=" << std::setprecision(1) << pnl*100 << " USD\n";
    }
    std::cout << "\n";

    double total_usd  = 0;
    int    total_wins = 0;
    for (const auto& tr : trades) {
        total_usd += tr.net_pnl * 100.0;
        if (tr.net_pnl > 0) total_wins++;
    }
    int    total_n = (int)trades.size();
    double wr      = total_n > 0 ? 100.0 * total_wins / total_n : 0;

    std::cout << "══════════════════════════════════════════════════════════════\n";
    std::cout << "  RESULT: " << total_n << " trades | WR=" << std::fixed
              << std::setprecision(1) << wr << "% | PnL="
              << std::setprecision(0) << total_usd << " USD\n";
    std::cout << "══════════════════════════════════════════════════════════════\n\n";

    if (write_csv)
        std::cout << "  Per-trade CSV written to: " << argv[2] << "\n\n";

    return 0;
}
