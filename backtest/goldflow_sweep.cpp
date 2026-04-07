// goldflow_sweep.cpp
// GoldFlow parameter sweep — tests all TP/SL/IMPULSE/TIME combinations
// Build: g++ -O3 -std=c++17 -o goldflow_sweep goldflow_sweep.cpp
// Run:   ./goldflow_sweep ticks.csv [results.csv]
//
// Reports every combination: n_trades, WR, PnL_USD, and breakdown by exit reason
// Sort the output CSV by PnL_USD descending to find the best parameter set

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
// Tick parse
// ─────────────────────────────────────────────
struct Tick
{
    uint64_t ts  = 0;
    double   ask = 0;
    double   bid = 0;
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

inline int utc_hour(uint64_t ts_ms) { return (int)((ts_ms / 1000 / 3600) % 24); }
inline bool session_ok(uint64_t ts_ms)
{
    int h = utc_hour(ts_ms);
    return (h >= 7 && h <= 10) || (h >= 12 && h <= 16);
}

// ─────────────────────────────────────────────
// Exit reasons
// ─────────────────────────────────────────────
enum class ExitReason { NONE, TP_HIT, SL_HIT, ADVERSE_EARLY, TIME_STOP };

// ─────────────────────────────────────────────
// Minimal trade for sweep (no CSV per-trade output — too much data)
// ─────────────────────────────────────────────
struct MiniTrade
{
    double     net_pnl  = 0;
    ExitReason why      = ExitReason::NONE;
};

// ─────────────────────────────────────────────
// Sweep parameters
// ─────────────────────────────────────────────
struct SweepParams
{
    double   tp;           // take profit pts
    double   sl;           // stop loss pts
    double   impulse_min;  // min range to trigger
    uint32_t time_limit_s; // seconds before TIME_STOP
    double   pullback;     // pullback fraction
};

// ─────────────────────────────────────────────
// Run one backtest pass (pre-loaded ticks)
// ─────────────────────────────────────────────
static const double MAX_SPREAD     = 0.40;
static const int    WINDOW         = 60;
static const double VWAP_TREND_PTS = 0.5;
static const int    VWAP_TREND_LOOK= 30;
static const int    COOLDOWN_TICKS = 300;
static const int    ADVERSE_WINDOW = 30;
static const double ADVERSE_MIN    = 2.0;

struct RunResult
{
    int    n_total   = 0;
    int    n_tp      = 0;
    int    n_sl      = 0;
    int    n_adverse = 0;
    int    n_time    = 0;
    double pnl_total = 0;
    double pnl_tp    = 0;
    double pnl_sl    = 0;
    double pnl_adv   = 0;
    double pnl_time  = 0;
    int    wins      = 0;
};

RunResult run_backtest(const std::vector<Tick>& ticks, const SweepParams& p)
{
    RunResult res;

    std::vector<double> price_buf;
    std::vector<double> vwap_buf;
    price_buf.reserve(WINDOW + 10);
    vwap_buf.reserve(VWAP_TREND_LOOK + 10);

    double vwap = 0;
    double hi = 0, lo = 0;
    std::vector<double> vwap_raw_buf;
    vwap_raw_buf.reserve(320);
    static const int VWAP_WINDOW = 300;

    bool     in_pos     = false;
    bool     is_long    = false;
    double   entry      = 0;
    double   tp_level   = 0;
    double   sl_level   = 0;
    double   spread_open = 0;
    double   mfe        = 0;
    double   mae        = 0;
    int      pos_ticks  = 0;
    uint64_t entry_ts   = 0;
    int      cooldown   = 0;

    for (const auto& t : ticks)
    {
        double spread = t.ask - t.bid;
        double mid    = (t.ask + t.bid) * 0.5;

        // Rolling VWAP update (always) -- 300-tick window, NOT cumulative
        vwap_raw_buf.push_back(mid);
        if ((int)vwap_raw_buf.size() > VWAP_WINDOW)
            vwap_raw_buf.erase(vwap_raw_buf.begin());
        {
            double sum = 0.0;
            for (double p : vwap_raw_buf) sum += p;
            vwap = sum / (double)vwap_raw_buf.size();
        }

        vwap_buf.push_back(vwap);
        if ((int)vwap_buf.size() > VWAP_TREND_LOOK + 5)
            vwap_buf.erase(vwap_buf.begin());

        price_buf.push_back(mid);
        if ((int)price_buf.size() > WINDOW + 5)
            price_buf.erase(price_buf.begin());

        if (spread > MAX_SPREAD) continue;
        if (!session_ok(t.ts))  continue;

        if (cooldown > 0) { cooldown--; }

        // ── manage position ──────────────────────────────
        if (in_pos)
        {
            pos_ticks++;

            double exc = is_long ? mid - entry : entry - mid;
            if (exc > mfe) mfe = exc;
            if (exc < -mae) mae = -exc;

            bool adverse_early = (pos_ticks <= ADVERSE_WINDOW && mae >= ADVERSE_MIN);

            ExitReason why = ExitReason::NONE;
            double     exit_px = 0;

            if (is_long) {
                double px = t.bid;
                if      (px >= tp_level) { why = ExitReason::TP_HIT; exit_px = tp_level; }
                else if (px <= sl_level) {
                    why = adverse_early ? ExitReason::ADVERSE_EARLY : ExitReason::SL_HIT;
                    exit_px = sl_level;
                }
                else if (t.ts - entry_ts >= (uint64_t)p.time_limit_s * 1000) {
                    why = ExitReason::TIME_STOP; exit_px = px;
                }
            } else {
                double px = t.ask;
                if      (px <= tp_level) { why = ExitReason::TP_HIT; exit_px = tp_level; }
                else if (px >= sl_level) {
                    why = adverse_early ? ExitReason::ADVERSE_EARLY : ExitReason::SL_HIT;
                    exit_px = sl_level;
                }
                else if (t.ts - entry_ts >= (uint64_t)p.time_limit_s * 1000) {
                    why = ExitReason::TIME_STOP; exit_px = px;
                }
            }

            if (why != ExitReason::NONE)
            {
                double gross = is_long ? exit_px - entry : entry - exit_px;
                double net   = gross - spread_open;

                res.n_total++;
                res.pnl_total += net;
                if (net > 0) res.wins++;

                switch (why) {
                    case ExitReason::TP_HIT:
                        res.n_tp++;    res.pnl_tp   += net; break;
                    case ExitReason::SL_HIT:
                        res.n_sl++;    res.pnl_sl   += net; break;
                    case ExitReason::ADVERSE_EARLY:
                        res.n_adverse++; res.pnl_adv += net; break;
                    case ExitReason::TIME_STOP:
                        res.n_time++;  res.pnl_time += net; break;
                    default: break;
                }

                in_pos   = false;
                cooldown = COOLDOWN_TICKS;
            }
        }

        // ── entry ─────────────────────────────────────────
        if (!in_pos && cooldown == 0)
        {
            // detect impulse
            if ((int)price_buf.size() >= WINDOW)
            {
                int start = (int)price_buf.size() - WINDOW;
                hi = price_buf[start];
                lo = price_buf[start];
                for (int i = start; i < (int)price_buf.size(); i++) {
                    hi = std::max(hi, price_buf[i]);
                    lo = std::min(lo, price_buf[i]);
                }

                if (hi - lo >= p.impulse_min)
                {
                    double impulse = hi - lo;
                    double pb_long  = hi - p.pullback * impulse;
                    double pb_short = lo + p.pullback * impulse;

                    // VWAP trend
                    bool trend_up = false, trend_down = false;
                    if ((int)vwap_buf.size() >= VWAP_TREND_LOOK) {
                        int n = (int)vwap_buf.size();
                        double delta = vwap_buf[n-1] - vwap_buf[n - VWAP_TREND_LOOK];
                        trend_up   = delta >  VWAP_TREND_PTS;
                        trend_down = delta < -VWAP_TREND_PTS;
                    }

                    bool can_long  = (mid <= pb_long  && mid > vwap && trend_up);
                    bool can_short = (mid >= pb_short && mid < vwap && trend_down);

                    if (can_long || can_short)
                    {
                        in_pos  = true;
                        is_long = can_long;

                        if (is_long) {
                            entry    = t.ask;
                            tp_level = entry + p.tp;
                            sl_level = entry - p.sl;
                        } else {
                            entry    = t.bid;
                            tp_level = entry - p.tp;
                            sl_level = entry + p.sl;
                        }

                        spread_open = spread;
                        entry_ts    = t.ts;
                        pos_ticks   = 0;
                        mfe         = 0;
                        mae         = 0;
                    }
                }
            }
        }
    }

    return res;
}

// ─────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────
int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cout << "usage: goldflow_sweep ticks.csv [results.csv]\n";
        return 0;
    }

    // ── Load ticks ───────────────────────────────
    std::cout << "Loading ticks from " << argv[1] << " ...\n";
    auto load_start = std::chrono::high_resolution_clock::now();

    std::vector<Tick> ticks;
    ticks.reserve(100000000);

    std::ifstream file(argv[1]);
    if (!file.is_open()) {
        std::cout << "cannot open: " << argv[1] << "\n";
        return 1;
    }

    std::string line;
    while (getline(file, line)) {
        Tick t;
        if (parse_tick(line, t)) ticks.push_back(t);
    }
    file.close();

    double load_sec = std::chrono::duration<double>(
        std::chrono::high_resolution_clock::now() - load_start).count();
    std::cout << "Loaded " << ticks.size() << " ticks in "
              << std::fixed << std::setprecision(1) << load_sec << "s\n\n";

    // ── Define sweep grid ────────────────────────
    std::vector<double>   tp_vals    = {10.0, 12.0, 14.0, 16.0, 18.0, 20.0};
    std::vector<double>   sl_vals    = {5.0,  6.0,  7.0,  8.0,  10.0};
    std::vector<double>   imp_vals   = {6.0,  7.0,  8.0,  9.0,  10.0};
    std::vector<uint32_t> time_vals  = {600, 900, 1200, 1800}; // seconds
    std::vector<double>   pb_vals    = {0.45, 0.50, 0.55, 0.60};

    int total_combos = (int)(tp_vals.size() * sl_vals.size() * imp_vals.size()
                             * time_vals.size() * pb_vals.size());
    std::cout << "Running " << total_combos << " parameter combinations...\n\n";

    // ── CSV output ────────────────────────────────
    bool write_csv = (argc >= 3);
    std::ofstream csv;
    if (write_csv) {
        csv.open(argv[2]);
        csv << "tp,sl,impulse_min,time_limit_s,pullback,"
            << "n_total,wins,wr_pct,pnl_usd,"
            << "n_tp,pnl_tp_usd,"
            << "n_sl,pnl_sl_usd,"
            << "n_adverse,pnl_adverse_usd,"
            << "n_time,pnl_time_usd\n";
    }

    // ── Run sweep ─────────────────────────────────
    struct BestResult {
        double    pnl = -1e12;
        SweepParams params{};
        RunResult   result{};
    } best;

    int done = 0;
    auto sweep_start = std::chrono::high_resolution_clock::now();

    for (double tp   : tp_vals)
    for (double sl   : sl_vals)
    for (double imp  : imp_vals)
    for (uint32_t tl : time_vals)
    for (double pb   : pb_vals)
    {
        // Skip obviously bad R:R
        if (tp / sl < 1.2) { done++; continue; }

        SweepParams p{ tp, sl, imp, tl, pb };
        RunResult   r = run_backtest(ticks, p);

        double wr = r.n_total > 0 ? 100.0 * r.wins / r.n_total : 0;

        if (write_csv) {
            csv << std::fixed << std::setprecision(2)
                << p.tp << "," << p.sl << "," << p.impulse_min << ","
                << p.time_limit_s << "," << p.pullback << ","
                << r.n_total << "," << r.wins << ","
                << std::setprecision(1) << wr << ","
                << std::setprecision(0) << r.pnl_total*100 << ","
                << r.n_tp   << "," << std::setprecision(0) << r.pnl_tp*100 << ","
                << r.n_sl   << "," << r.pnl_sl*100 << ","
                << r.n_adverse << "," << r.pnl_adv*100 << ","
                << r.n_time << "," << r.pnl_time*100 << "\n";
        }

        if (r.pnl_total > best.pnl) {
            best.pnl    = r.pnl_total;
            best.params = p;
            best.result = r;
        }

        done++;
        if (done % 100 == 0) {
            double elapsed = std::chrono::duration<double>(
                std::chrono::high_resolution_clock::now() - sweep_start).count();
            double eta = elapsed / done * (total_combos - done);
            std::cout << "\r  " << done << "/" << total_combos
                      << " (" << std::setprecision(0) << eta << "s remaining)   " << std::flush;
        }
    }

    double sweep_sec = std::chrono::duration<double>(
        std::chrono::high_resolution_clock::now() - sweep_start).count();

    std::cout << "\n\n";

    // ── Print top-10 results ──────────────────────
    // (Rerun to collect all results in memory for sorting top 10)
    // For memory efficiency, just print the best found
    std::cout << "══════════════════════════════════════════════════════════════\n";
    std::cout << "  GoldFlow Sweep — " << total_combos << " combinations in "
              << std::fixed << std::setprecision(1) << sweep_sec << "s\n";
    std::cout << "══════════════════════════════════════════════════════════════\n\n";

    std::cout << "  BEST RESULT:\n";
    std::cout << "    TP=" << best.params.tp
              << " SL=" << best.params.sl
              << " IMPULSE=" << best.params.impulse_min
              << " TIME=" << best.params.time_limit_s << "s"
              << " PULLBACK=" << best.params.pullback << "\n";

    auto& r = best.result;
    double wr = r.n_total > 0 ? 100.0 * r.wins / r.n_total : 0;
    std::cout << "    Trades=" << r.n_total
              << " WR=" << std::setprecision(1) << wr << "%"
              << " PnL=" << std::setprecision(0) << r.pnl_total*100 << " USD\n\n";

    std::cout << "  Exit breakdown:\n";
    auto pct = [&](int n) { return r.n_total > 0 ? 100.0*n/r.n_total : 0; };
    std::cout << "    TP_HIT        n=" << r.n_tp
              << " (" << std::setprecision(1) << pct(r.n_tp) << "%)"
              << " PnL=" << std::setprecision(0) << r.pnl_tp*100 << " USD\n";
    std::cout << "    SL_HIT        n=" << r.n_sl
              << " (" << pct(r.n_sl) << "%)"
              << " PnL=" << r.pnl_sl*100 << " USD\n";
    std::cout << "    ADVERSE_EARLY n=" << r.n_adverse
              << " (" << pct(r.n_adverse) << "%)"
              << " PnL=" << r.pnl_adv*100 << " USD\n";
    std::cout << "    TIME_STOP     n=" << r.n_time
              << " (" << pct(r.n_time) << "%)"
              << " PnL=" << r.pnl_time*100 << " USD\n\n";

    if (write_csv)
        std::cout << "  Full results written to: " << argv[2] << "\n"
                  << "  Sort by pnl_usd DESC to rank all combinations\n\n";

    std::cout << "══════════════════════════════════════════════════════════════\n";

    return 0;
}
