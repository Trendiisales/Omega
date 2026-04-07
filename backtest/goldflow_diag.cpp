// goldflow_diag.cpp
// GoldFlow diagnostic backtest — exit reason breakdown + per-trade CSV
// Build: g++ -O2 -std=c++17 -o goldflow_diag goldflow_diag.cpp
// Run:   ./goldflow_diag ticks.csv [trades_out.csv]
//
// Exit reasons:
//   TP_HIT       — target reached
//   SL_HIT       — stop hit
//   ADVERSE_EARLY — price moved adversely within first ADVERSE_WINDOW ticks after entry
//   TIME_STOP    — held past TIME_LIMIT_MS with no resolution
//   TRAIL_HIT    — trailing stop triggered (if trail enabled)
//
// Key insight from prior run: ADVERSE_EARLY=2942 trades -$537, TIME_STOP=1405 trades -$668
// This build logs MFE, MAE, hold time, spread at entry, impulse strength for every trade
// Use the CSV to find what separates ADVERSE_EARLY losers from TP_HIT winners

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
inline int utc_hour(uint64_t ts_ms)
{
    return (int)((ts_ms / 1000 / 3600) % 24);
}

inline bool session_ok(uint64_t ts_ms)
{
    int h = utc_hour(ts_ms);
    return (h >= 7 && h <= 10) || (h >= 12 && h <= 16);
}

inline std::string session_name(uint64_t ts_ms)
{
    int h = utc_hour(ts_ms);
    if (h >= 7 && h <= 10)  return "LONDON";
    if (h >= 12 && h <= 16) return "NY";
    return "OFF";
}

// ─────────────────────────────────────────────
// Parameters (edit these for experiments)
// ─────────────────────────────────────────────
static const int    WINDOW          = 60;    // ticks for impulse detection
static const double IMPULSE_MIN     = 7.0;   // min range to qualify as impulse (pts)
static const double TP_PTS          = 14.0;  // take profit distance (pts)
static const double SL_PTS          = 8.0;   // stop loss distance (pts)
static const double PULLBACK_FRAC   = 0.55;  // pullback zone: hi - frac*range or lo + frac*range
static const double VWAP_TREND_PTS  = 0.5;   // VWAP delta over 30 ticks to confirm trend
static const int    VWAP_TREND_LOOK = 30;    // lookback for VWAP trend
static const double MAX_SPREAD      = 0.40;  // skip ticks with spread above this
static const int    COOLDOWN_TICKS  = 300;   // ticks to wait after close before new entry
static const uint64_t TIME_LIMIT_MS = 1800000; // 30 min position time limit (ms)
static const int    ADVERSE_WINDOW  = 30;    // ticks: if price moves adversely within this, tag ADVERSE_EARLY
static const double ADVERSE_MIN_PTS = 2.0;   // pts of adverse move to trigger ADVERSE_EARLY tag

// Trail (set TRAIL_ENABLED=false to disable)
static const bool   TRAIL_ENABLED   = false;
static const double TRAIL_TRIGGER   = 6.0;   // MFE needed to activate trail (pts)
static const double TRAIL_LOCK      = 0.80;  // fraction of MFE to lock

// Spread cost per trade (one-way, applied at open)
// BlackBull XAUUSD typical: 0.25 pts = $25 per lot
// We track actual entry spread separately
static const double COMMISSION_PTS  = 0.0;   // additional commission in pts (set if known)

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
    double   spread_open = 0;   // bid-ask spread at entry (pts)
    double   impulse_sz  = 0;   // hi - lo of trigger window (pts)
    double   mfe         = 0;   // max favourable excursion (pts)
    double   mae         = 0;   // max adverse excursion (pts)
    double   gross_pnl   = 0;   // raw pts
    double   net_pnl     = 0;   // after spread cost
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
    // price + vwap history
    std::vector<double> price_buf;
    std::vector<double> vwap_buf;

    double vwap = 0;
    double hi = 0, lo = 0;
    std::vector<double> vwap_raw_buf;  // raw prices for rolling VWAP

    // position state
    bool     in_pos     = false;
    bool     is_long    = false;
    double   entry      = 0;
    double   tp         = 0;
    double   sl         = 0;
    double   spread_at_entry = 0;
    double   impulse_at_entry = 0;
    double   mfe        = 0;
    double   mae        = 0;
    double   trail_sl   = 0;
    bool     trail_active = false;
    uint64_t entry_ts   = 0;
    std::string entry_session;

    int cooldown  = 0;
    int pos_ticks = 0;  // ticks held in current position (for ADVERSE_EARLY detection)

    // Rolling VWAP over VWAP_WINDOW ticks -- NOT cumulative.
    // Cumulative VWAP over 334M ticks produces delta~0.0001/tick -- trend gate never fires.
    // Rolling window matches live engine behaviour: VWAP reacts to recent price action.
    static const int VWAP_WINDOW = 300;  // ~5min of ticks at typical XAUUSD tick rate

    void update_vwap(double price)
    {
        vwap_raw_buf.push_back(price);
        if ((int)vwap_raw_buf.size() > VWAP_WINDOW)
            vwap_raw_buf.erase(vwap_raw_buf.begin());

        // Rolling mean of last VWAP_WINDOW prices
        double sum = 0.0;
        for (double p : vwap_raw_buf) sum += p;
        vwap = sum / (double)vwap_raw_buf.size();

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

        // look at last WINDOW ticks
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
    int    count     = 0;
    int    wins      = 0;
    double total_pnl = 0;
    double total_mfe = 0;
    double total_mae = 0;
    double total_hold_ms = 0;

    void add(const TradeRecord& tr)
    {
        count++;
        total_pnl    += tr.net_pnl;
        total_mfe    += tr.mfe;
        total_mae    += tr.mae;
        total_hold_ms+= (double)tr.hold_ms;
        if (tr.net_pnl > 0) wins++;
    }

    void print(const char* label) const
    {
        if (count == 0) { std::cout << "  " << label << ": 0 trades\n"; return; }
        double wr  = 100.0 * wins / count;
        double avg = total_pnl / count;
        double avg_mfe = total_mfe / count;
        double avg_mae = total_mae / count;
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
};

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

    // optional CSV output
    std::ofstream csv_out;
    bool write_csv = (argc >= 3);
    if (write_csv) {
        csv_out.open(argv[2]);
        csv_out << "id,is_long,session,entry_ts,entry,exit,tp,sl,"
                << "spread_open,impulse_sz,mfe,mae,gross_pnl,net_pnl,"
                << "hold_ticks,hold_ms,exit_why\n";
    }

    Engine e;

    std::vector<TradeRecord> trades;
    TradeRecord cur;

    uint64_t ticks_total = 0;
    uint64_t ticks_used  = 0;  // in-session

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

        // always update VWAP (even off-session, so it's warm)
        e.update_vwap(mid);
        e.update_price(mid);

        // spread filter
        if (spread > MAX_SPREAD) continue;

        if (!session_ok(t.ts)) continue;
        ticks_used++;

        // ── cooldown ────────────────────────────────────
        if (e.cooldown > 0) { e.cooldown--; }

        // ── manage open position ─────────────────────────
        if (e.in_pos)
        {
            e.pos_ticks++;
            cur.hold_ticks++;

            // MFE / MAE
            double excursion = e.is_long
                ? mid - e.entry
                : e.entry - mid;

            if (excursion > cur.mfe) cur.mfe = excursion;
            if (excursion < -cur.mae) cur.mae = -excursion;

            // Trail update
            if (TRAIL_ENABLED) {
                if (!e.trail_active && cur.mfe >= TRAIL_TRIGGER) {
                    e.trail_active = true;
                }
                if (e.trail_active) {
                    double locked = cur.mfe * TRAIL_LOCK;
                    double new_trail = e.is_long
                        ? e.entry + locked
                        : e.entry - locked;
                    if (e.is_long)
                        e.trail_sl = std::max(e.trail_sl, new_trail);
                    else
                        e.trail_sl = std::min(e.trail_sl, new_trail);
                }
            }

            // ADVERSE_EARLY tag: within first ADVERSE_WINDOW ticks, adverse move >= ADVERSE_MIN
            bool adverse_early = false;
            if (e.pos_ticks <= ADVERSE_WINDOW && cur.mae >= ADVERSE_MIN_PTS) {
                adverse_early = true;
            }

            // Exit checks
            ExitReason why = ExitReason::NONE;
            double exit_px = 0;

            if (e.is_long) {
                double px = t.bid; // we sell at bid
                if (px >= e.tp) {
                    why = ExitReason::TP_HIT;
                    exit_px = e.tp;
                } else if (px <= e.sl) {
                    why = (adverse_early && e.pos_ticks <= ADVERSE_WINDOW)
                        ? ExitReason::ADVERSE_EARLY
                        : ExitReason::SL_HIT;
                    exit_px = e.sl;
                } else if (TRAIL_ENABLED && e.trail_active && px <= e.trail_sl) {
                    why = ExitReason::TRAIL_HIT;
                    exit_px = e.trail_sl;
                } else if (t.ts - e.entry_ts >= TIME_LIMIT_MS) {
                    why = ExitReason::TIME_STOP;
                    exit_px = px;
                }
            } else {
                double px = t.ask; // we buy back at ask
                if (px <= e.tp) {
                    why = ExitReason::TP_HIT;
                    exit_px = e.tp;
                } else if (px >= e.sl) {
                    why = (adverse_early && e.pos_ticks <= ADVERSE_WINDOW)
                        ? ExitReason::ADVERSE_EARLY
                        : ExitReason::SL_HIT;
                    exit_px = e.sl;
                } else if (TRAIL_ENABLED && e.trail_active && px >= e.trail_sl) {
                    why = ExitReason::TRAIL_HIT;
                    exit_px = e.trail_sl;
                } else if (t.ts - e.entry_ts >= TIME_LIMIT_MS) {
                    why = ExitReason::TIME_STOP;
                    exit_px = px;
                }
            }

            if (why != ExitReason::NONE)
            {
                cur.exit_price = exit_px;
                cur.gross_pnl  = e.is_long
                    ? exit_px - e.entry
                    : e.entry - exit_px;
                cur.net_pnl    = cur.gross_pnl - cur.spread_open - COMMISSION_PTS;
                cur.hold_ms    = t.ts - e.entry_ts;
                cur.exit_why   = why;
                // clamp MFE/MAE to non-negative
                cur.mfe = std::max(0.0, cur.mfe);
                cur.mae = std::max(0.0, cur.mae);

                trades.push_back(cur);

                if (write_csv) {
                    csv_out
                        << cur.id << ","
                        << (cur.is_long ? 1 : 0) << ","
                        << cur.session << ","
                        << cur.entry_ts << ","
                        << std::fixed << std::setprecision(2)
                        << cur.entry << ","
                        << cur.exit_price << ","
                        << cur.tp << ","
                        << cur.sl << ","
                        << cur.spread_open << ","
                        << cur.impulse_sz << ","
                        << cur.mfe << ","
                        << cur.mae << ","
                        << cur.gross_pnl << ","
                        << cur.net_pnl << ","
                        << cur.hold_ticks << ","
                        << cur.hold_ms << ","
                        << exit_name(why) << "\n";
                }

                e.in_pos      = false;
                e.trail_active= false;
                e.cooldown    = COOLDOWN_TICKS;
            }
        }

        // ── entry logic ──────────────────────────────────
        if (!e.in_pos && e.cooldown == 0 && e.detect_impulse())
        {
            double impulse = e.hi - e.lo;

            double pb_long  = e.hi  - PULLBACK_FRAC * impulse;
            double pb_short = e.lo  + PULLBACK_FRAC * impulse;

            bool can_long  = (mid <= pb_long  && mid > e.vwap && e.vwap_trend_up());
            bool can_short = (mid >= pb_short && mid < e.vwap && e.vwap_trend_down());

            if (can_long || can_short)
            {
                e.in_pos  = true;
                e.is_long = can_long;

                if (e.is_long) {
                    e.entry = t.ask;
                    e.tp    = e.entry + TP_PTS;
                    e.sl    = e.entry - SL_PTS;
                    e.trail_sl = e.entry - SL_PTS;
                } else {
                    e.entry = t.bid;
                    e.tp    = e.entry - TP_PTS;
                    e.sl    = e.entry + SL_PTS;
                    e.trail_sl = e.entry + SL_PTS;
                }

                e.entry_ts = t.ts;
                e.trail_active = false;

                cur = TradeRecord{};
                cur.id           = ++trade_id;
                cur.is_long      = e.is_long;
                cur.entry        = e.entry;
                cur.tp           = e.tp;
                cur.sl           = e.sl;
                cur.spread_open  = spread;
                cur.impulse_sz   = impulse;
                cur.entry_ts     = t.ts;
                cur.session      = session_name(t.ts);
                e.pos_ticks     = 0;
                cur.mfe          = 0;
                cur.mae          = 0;
            }
        }
    }

    auto t_end = std::chrono::high_resolution_clock::now();
    double runtime = std::chrono::duration<double>(t_end - t_start).count();

    // ─────────────────────────────────────────────
    // Aggregate stats by exit reason
    // ─────────────────────────────────────────────
    Stats s_total, s_tp, s_sl, s_adverse, s_time, s_trail;
    Stats s_london, s_ny;

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

        if (tr.session == "LONDON") s_london.add(tr);
        else if (tr.session == "NY") s_ny.add(tr);
    }

    // ─────────────────────────────────────────────
    // Print report
    // ─────────────────────────────────────────────
    std::cout << "\n";
    std::cout << "══════════════════════════════════════════════════════════════\n";
    std::cout << "  GoldFlow Diagnostic Backtest\n";
    std::cout << "══════════════════════════════════════════════════════════════\n";
    std::cout << "  Ticks total   : " << ticks_total << "\n";
    std::cout << "  Ticks in-sess : " << ticks_used << "\n";
    std::cout << "  Runtime       : " << std::fixed << std::setprecision(2) << runtime << " s\n";
    std::cout << "\n";
    std::cout << "  Parameters:\n";
    std::cout << "    WINDOW=" << WINDOW << " IMPULSE_MIN=" << IMPULSE_MIN
              << " TP=" << TP_PTS << " SL=" << SL_PTS << "\n";
    std::cout << "    PULLBACK_FRAC=" << PULLBACK_FRAC
              << " VWAP_TREND=" << VWAP_TREND_PTS
              << " TIME_LIMIT=" << TIME_LIMIT_MS/1000 << "s\n";
    std::cout << "    ADVERSE_WINDOW=" << ADVERSE_WINDOW
              << " ADVERSE_MIN=" << ADVERSE_MIN_PTS << " pts\n";
    std::cout << "    TRAIL=" << (TRAIL_ENABLED ? "ON" : "OFF");
    if (TRAIL_ENABLED) std::cout << " trigger=" << TRAIL_TRIGGER << " lock=" << TRAIL_LOCK*100 << "%";
    std::cout << "\n";
    std::cout << "\n";
    std::cout << "── By Exit Reason ────────────────────────────────────────────\n";
    s_total.print("TOTAL");
    s_tp.print("TP_HIT");
    s_sl.print("SL_HIT");
    s_adverse.print("ADVERSE_EARLY");
    s_time.print("TIME_STOP");
    if (TRAIL_ENABLED) s_trail.print("TRAIL_HIT");
    std::cout << "\n";
    std::cout << "── By Session ────────────────────────────────────────────────\n";
    s_london.print("LONDON (07-10 UTC)");
    s_ny.print("NY (12-16 UTC)");
    std::cout << "\n";

    // ADVERSE_EARLY deep-dive: impulse size buckets
    if (s_adverse.count > 0)
    {
        std::cout << "── ADVERSE_EARLY: Impulse Size Buckets ──────────────────────\n";
        // buckets: <8, 8-10, 10-12, 12-15, >15
        std::vector<std::pair<std::string, std::pair<double,double>>> buckets = {
            {"<8pts",   {0,   8}},
            {"8-10pts", {8,  10}},
            {"10-12pts",{10, 12}},
            {"12-15pts",{12, 15}},
            {">15pts",  {15, 999}},
        };

        for (auto& [label, range] : buckets)
        {
            int n = 0; double pnl = 0;
            for (const auto& tr : trades) {
                if (tr.exit_why != ExitReason::ADVERSE_EARLY) continue;
                if (tr.impulse_sz >= range.first && tr.impulse_sz < range.second) {
                    n++; pnl += tr.net_pnl;
                }
            }
            if (n > 0)
                std::cout << "  " << std::left << std::setw(12) << label
                          << " n=" << std::setw(5) << n
                          << " PnL=" << std::setprecision(1) << pnl*100 << " USD\n";
        }
        std::cout << "\n";

        std::cout << "── ADVERSE_EARLY: Hold-time distribution ─────────────────────\n";
        // how long were these held?
        double avg_hold = 0;
        int n = 0;
        for (const auto& tr : trades) {
            if (tr.exit_why != ExitReason::ADVERSE_EARLY) {
                avg_hold += tr.hold_ms;
                n++;
            }
        }
        // buckets by ticks
        std::vector<std::pair<std::string,std::pair<int,int>>> tbuckets = {
            {"1-5 ticks",  {1,  5}},
            {"6-15 ticks", {6, 15}},
            {"16-30 ticks",{16,30}},
            {"31-60 ticks",{31,60}},
            {">60 ticks",  {61,99999}},
        };
        for (auto& [label, range] : tbuckets)
        {
            int cnt = 0; double pnl = 0;
            for (const auto& tr : trades) {
                if (tr.exit_why != ExitReason::ADVERSE_EARLY) continue;
                if (tr.hold_ticks >= range.first && tr.hold_ticks <= range.second) {
                    cnt++; pnl += tr.net_pnl;
                }
            }
            if (cnt > 0)
                std::cout << "  " << std::left << std::setw(14) << label
                          << " n=" << std::setw(5) << cnt
                          << " PnL=" << std::setprecision(1) << pnl*100 << " USD\n";
        }
        std::cout << "\n";
    }

    // TIME_STOP deep-dive: MFE at timeout
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
                if (tr.mfe >= range.first && tr.mfe < range.second) {
                    cnt++; pnl += tr.net_pnl;
                }
            }
            if (cnt > 0)
                std::cout << "  " << std::left << std::setw(14) << label
                          << " n=" << std::setw(5) << cnt
                          << " PnL=" << std::setprecision(1) << pnl*100 << " USD\n";
        }
        std::cout << "\n";
    }

    // Overall summary line
    double total_usd = 0;
    int    total_wins = 0;
    for (const auto& tr : trades) {
        total_usd  += tr.net_pnl * 100.0;
        if (tr.net_pnl > 0) total_wins++;
    }
    int total_n = (int)trades.size();
    double wr = total_n > 0 ? 100.0 * total_wins / total_n : 0;

    std::cout << "══════════════════════════════════════════════════════════════\n";
    std::cout << "  RESULT: " << total_n << " trades | WR=" << std::fixed
              << std::setprecision(1) << wr << "% | PnL="
              << std::setprecision(0) << total_usd << " USD\n";
    std::cout << "══════════════════════════════════════════════════════════════\n\n";

    if (write_csv)
        std::cout << "  Per-trade CSV written to: " << argv[2] << "\n\n";

    return 0;
}
