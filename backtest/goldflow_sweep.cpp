// goldflow_sweep.cpp
// GoldFlow parameter sweep — parallel, ticks loaded once into memory
// Build: g++ -O3 -std=c++17 -pthread -o goldflow_sweep goldflow_sweep.cpp
// Run:   ./goldflow_sweep ticks.csv [results.csv] [n_threads]
//
// Speedup: ticks loaded once into RAM, all combos run against shared read-only array.
// N threads partition the combo list — scales linearly with core count.
// Default threads = hardware_concurrency (all logical cores).
//
// CALIBRATION (from gate diagnostics on 134M XAUUSD ticks):
//   - VWAP: daily-reset cumulative (matches live engine)
//   - WINDOW=300 or 600 ticks (~30s or ~1min)
//   - VWAP_TREND range 0.001–0.005 (real p75=0.0022, p99=0.0056)
//   - Cooldown set on entry (duplicate entry fix)

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
#include <thread>
#include <mutex>
#include <atomic>

// ─────────────────────────────────────────────
// Tick
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
    try { t.ts = std::stoull(tok); } catch (...) { return false; }
    if (!getline(ss, tok, ',')) return false;
    try { t.ask = std::stod(tok); } catch (...) { return false; }
    if (!getline(ss, tok, ',')) return false;
    try { t.bid = std::stod(tok); } catch (...) { return false; }
    return (t.ask > 0 && t.bid > 0 && t.ask >= t.bid);
}

inline int      utc_hour(uint64_t ts) { return (int)((ts/1000/3600)%24); }
inline uint64_t utc_day (uint64_t ts) { return ts/1000/86400; }
inline bool session_asia  (uint64_t ts) { int h=utc_hour(ts); return (h>=0&&h<=6); }
inline bool session_london(uint64_t ts) { int h=utc_hour(ts); return (h>=7&&h<=10); }
inline bool session_ny    (uint64_t ts) { int h=utc_hour(ts); return (h>=12&&h<=16); }
inline bool session_ok    (uint64_t ts) { return session_asia(ts)||session_london(ts)||session_ny(ts); }

// ─────────────────────────────────────────────
// Exit reasons
// ─────────────────────────────────────────────
enum class ExitReason { NONE, TP_HIT, SL_HIT, ADVERSE_EARLY, TIME_STOP, TRAIL_HIT };

// ─────────────────────────────────────────────
// Sweep parameters
// ─────────────────────────────────────────────
struct SweepParams
{
    double   tp;
    double   sl;
    double   impulse_min;
    uint32_t time_limit_s;
    double   pullback;
    double   vwap_trend;
    int      window;
};

// ─────────────────────────────────────────────
// Fixed constants
// ─────────────────────────────────────────────
static const double MAX_SPREAD      = 0.40;
static const int    VWAP_TREND_LOOK = 30;
static const int    COOLDOWN_TICKS  = 300;
static const int    ADVERSE_WINDOW  = 30;
static const double ADVERSE_MIN     = 2.0;
// Trail (fixed — sweep doesn't vary these, diag does)
static const bool   TRAIL_ENABLED   = true;
static const double TRAIL_TRIGGER   = 6.0;
static const double TRAIL_LOCK      = 0.75;
// All three sessions active — Asia has the biggest gold moves
static const bool   NY_ONLY         = false;

// ─────────────────────────────────────────────
// Result
// ─────────────────────────────────────────────
struct RunResult
{
    int    n_total   = 0;
    int    n_tp      = 0;
    int    n_sl      = 0;
    int    n_adverse = 0;
    int    n_time    = 0;
    int    n_trail   = 0;
    double pnl_total = 0;
    double pnl_tp    = 0;
    double pnl_sl    = 0;
    double pnl_adv   = 0;
    double pnl_time  = 0;
    double pnl_trail = 0;
    int    wins      = 0;
};

// ─────────────────────────────────────────────
// Single backtest pass (read-only tick array)
// ─────────────────────────────────────────────
RunResult run_backtest(const std::vector<Tick>& ticks, const SweepParams& p)
{
    RunResult res;

    const int vwap_buf_max = VWAP_TREND_LOOK + 5;
    const int price_buf_max = p.window + 5;

    std::vector<double> price_buf;
    std::vector<double> vwap_buf;
    price_buf.reserve(price_buf_max);
    vwap_buf.reserve(vwap_buf_max);

    // Daily-reset cumulative VWAP
    double   vwap       = 0;
    double   vwap_pv    = 0;
    uint64_t vwap_count = 0;
    uint64_t vwap_day   = 0;

    double hi = 0, lo = 0;

    bool     in_pos      = false;
    bool     is_long     = false;
    double   entry       = 0;
    double   tp_level    = 0;
    double   sl_level    = 0;
    double   trail_sl    = 0;
    bool     trail_active= false;
    double   spread_open = 0;
    double   mfe         = 0;
    double   mae         = 0;
    int      pos_ticks   = 0;
    uint64_t entry_ts    = 0;
    int      cooldown    = 0;

    for (const auto& t : ticks)
    {
        double spread = t.ask - t.bid;
        double mid    = (t.ask + t.bid) * 0.5;

        // Daily-reset cumulative VWAP (always)
        {
            uint64_t day = utc_day(t.ts);
            if (day != vwap_day) { vwap_pv=0; vwap_count=0; vwap_day=day; }
            vwap_pv += mid; vwap_count++;
            vwap = vwap_pv / (double)vwap_count;
        }
        vwap_buf.push_back(vwap);
        if ((int)vwap_buf.size() > vwap_buf_max)
            vwap_buf.erase(vwap_buf.begin());

        price_buf.push_back(mid);
        if ((int)price_buf.size() > price_buf_max)
            price_buf.erase(price_buf.begin());

        if (spread > MAX_SPREAD) continue;
        if (!session_ok(t.ts)) continue;

        if (cooldown > 0) { cooldown--; }

        // ── manage position ──────────────────────────────
        if (in_pos)
        {
            pos_ticks++;
            double exc = is_long ? mid - entry : entry - mid;
            if (exc > mfe) mfe = exc;
            if (exc < -mae) mae = -exc;

            // Trail update
            if (TRAIL_ENABLED) {
                if (!trail_active && mfe >= TRAIL_TRIGGER) trail_active = true;
                if (trail_active) {
                    double locked    = mfe * TRAIL_LOCK;
                    double new_trail = is_long ? entry + locked : entry - locked;
                    if (is_long) trail_sl = std::max(trail_sl, new_trail);
                    else         trail_sl = std::min(trail_sl, new_trail);
                }
            }

            bool adverse_early = (pos_ticks <= ADVERSE_WINDOW && mae >= ADVERSE_MIN);

            ExitReason why   = ExitReason::NONE;
            double     exit_px = 0;

            if (is_long) {
                double px = t.bid;
                if      (px >= tp_level) { why = ExitReason::TP_HIT; exit_px = tp_level; }
                else if (px <= sl_level) {
                    why = adverse_early ? ExitReason::ADVERSE_EARLY : ExitReason::SL_HIT;
                    exit_px = sl_level;
                }
                else if (TRAIL_ENABLED && trail_active && px <= trail_sl) {
                    why = ExitReason::TRAIL_HIT;
                    exit_px = trail_sl;
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
                else if (TRAIL_ENABLED && trail_active && px >= trail_sl) {
                    why = ExitReason::TRAIL_HIT;
                    exit_px = trail_sl;
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
                    case ExitReason::TP_HIT:        res.n_tp++;      res.pnl_tp    += net; break;
                    case ExitReason::SL_HIT:        res.n_sl++;      res.pnl_sl    += net; break;
                    case ExitReason::ADVERSE_EARLY: res.n_adverse++; res.pnl_adv   += net; break;
                    case ExitReason::TRAIL_HIT:     res.n_trail++;   res.pnl_trail += net; break;
                    case ExitReason::TIME_STOP:     res.n_time++;    res.pnl_time  += net; break;
                    default: break;
                }

                in_pos       = false;
                trail_active = false;
                // cooldown set at entry — leave running
            }
        }

        // ── entry ─────────────────────────────────────────
        if (!in_pos && cooldown == 0 && (int)price_buf.size() >= p.window)
        {
            int start = (int)price_buf.size() - p.window;
            hi = price_buf[start]; lo = price_buf[start];
            for (int i = start; i < (int)price_buf.size(); i++) {
                hi = std::max(hi, price_buf[i]);
                lo = std::min(lo, price_buf[i]);
            }

            if (hi - lo >= p.impulse_min)
            {
                double impulse  = hi - lo;
                double pb_long  = hi - p.pullback * impulse;
                double pb_short = lo + p.pullback * impulse;

                bool trend_up = false, trend_down = false;
                if ((int)vwap_buf.size() >= VWAP_TREND_LOOK) {
                    int n = (int)vwap_buf.size();
                    double delta = vwap_buf[n-1] - vwap_buf[n - VWAP_TREND_LOOK];
                    trend_up   = delta >  p.vwap_trend;
                    trend_down = delta < -p.vwap_trend;
                }

                bool can_long  = (mid <= pb_long  && mid > vwap && trend_up);
                bool can_short = (mid >= pb_short && mid < vwap && trend_down);

                if (can_long || can_short)
                {
                    in_pos  = true;
                    is_long = can_long;

                    if (is_long) {
                        entry     = t.ask;
                        tp_level  = entry + p.tp;
                        sl_level  = entry - p.sl;
                        trail_sl  = entry - p.sl;
                    } else {
                        entry     = t.bid;
                        tp_level  = entry - p.tp;
                        sl_level  = entry + p.sl;
                        trail_sl  = entry + p.sl;
                    }

                    spread_open  = spread;
                    entry_ts     = t.ts;
                    pos_ticks    = 0;
                    mfe          = 0;
                    mae          = 0;
                    trail_active = false;
                    cooldown     = COOLDOWN_TICKS;  // set on entry
                }
            }
        }
    }

    return res;
}

// ─────────────────────────────────────────────
// Combo record for sorting output
// ─────────────────────────────────────────────
struct ComboResult
{
    SweepParams p;
    RunResult   r;
};

// ─────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────
int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cout << "usage: goldflow_sweep ticks.csv [results.csv] [n_threads]\n";
        return 0;
    }

    // ── Load ticks ───────────────────────────────
    std::cout << "Loading ticks from " << argv[1] << " ...\n";
    auto load_start = std::chrono::high_resolution_clock::now();

    std::vector<Tick> ticks;
    ticks.reserve(140000000);

    {
        std::ifstream file(argv[1]);
        if (!file.is_open()) { std::cout << "cannot open: " << argv[1] << "\n"; return 1; }
        std::string line;
        while (getline(file, line)) {
            Tick t;
            if (parse_tick(line, t)) ticks.push_back(t);
        }
    }

    double load_sec = std::chrono::duration<double>(
        std::chrono::high_resolution_clock::now() - load_start).count();
    std::cout << "Loaded " << ticks.size() << " ticks in "
              << std::fixed << std::setprecision(1) << load_sec << "s\n";

    // ── Pre-filter to session ticks only ─────────
    // Each combo only needs to iterate session ticks — 56M vs 134M = 2.4x speedup.
    // VWAP still needs all ticks for warmup, so we keep full array for VWAP updates
    // but skip non-session ticks in the hot loop via a pre-built session index.
    // Simpler: just filter ticks to session+spread-ok, pass smaller array to combos.
    // NOTE: VWAP warmup requires off-session ticks — keep full array, just skip in loop.
    // The session filter is inside run_backtest already. Pre-filtering session ticks
    // would break VWAP warmup. Speedup comes purely from threading.
    std::cout << "Session ticks (approx): " << (ticks.size() * 56183864 / 134636174) << "\n\n";

    // ── Build combo list ──────────────────────────
    std::vector<double>   tp_vals     = {8.0, 10.0, 12.0, 14.0, 16.0, 18.0, 20.0};
    std::vector<double>   sl_vals     = {4.0, 5.0,  6.0,  7.0,  8.0,  10.0};
    std::vector<double>   imp_vals    = {6.0, 7.0,  8.0,  9.0,  10.0, 12.0};
    std::vector<uint32_t> time_vals   = {600, 900,  1200, 1800};
    std::vector<double>   pb_vals     = {0.45, 0.50, 0.55, 0.60};
    std::vector<double>   vwap_t_vals = {0.001, 0.002, 0.003, 0.004, 0.005};
    std::vector<int>      win_vals    = {300, 600};

    std::vector<SweepParams> combos;
    combos.reserve(50000);

    for (double   tp  : tp_vals)
    for (double   sl  : sl_vals)
    for (double   imp : imp_vals)
    for (uint32_t tl  : time_vals)
    for (double   pb  : pb_vals)
    for (double   vt  : vwap_t_vals)
    for (int      win : win_vals)
    {
        if (tp / sl < 1.2) continue;
        combos.push_back({tp, sl, imp, tl, pb, vt, win});
    }

    int total = (int)combos.size();
    std::cout << "Combos: " << total << "\n";

    // ── Thread setup ─────────────────────────────
    int n_threads = (int)std::thread::hardware_concurrency();
    if (n_threads < 1) n_threads = 4;
    if (argc >= 4) {
        try { n_threads = std::stoi(argv[3]); } catch (...) {}
    }
    if (n_threads > total) n_threads = total;
    std::cout << "Threads: " << n_threads << "\n";
    std::cout << "Progress printed every 100 combos across all threads.\n\n";

    // ── Per-thread results ────────────────────────
    std::vector<std::vector<ComboResult>> thread_results(n_threads);
    for (auto& v : thread_results) v.reserve(total / n_threads + 1);

    std::atomic<int> progress{0};
    std::mutex print_mutex;
    auto sweep_start = std::chrono::high_resolution_clock::now();

    // ── Launch threads ────────────────────────────
    auto worker = [&](int tid, int from, int to) {
        for (int i = from; i < to; i++) {
            RunResult r = run_backtest(ticks, combos[i]);
            thread_results[tid].push_back({combos[i], r});
            int done = ++progress;
            if (done % 100 == 0) {
                double elapsed = std::chrono::duration<double>(
                    std::chrono::high_resolution_clock::now() - sweep_start).count();
                double rate = done / elapsed;
                double eta  = (total - done) / rate;
                std::lock_guard<std::mutex> lock(print_mutex);
                std::cout << "  [" << std::setw(6) << done << "/" << total << "]"
                          << "  rate=" << std::fixed << std::setprecision(1) << rate << "/s"
                          << "  ETA=" << std::setprecision(0) << eta << "s"
                          << "  elapsed=" << std::setprecision(0) << elapsed << "s"
                          << "\n" << std::flush;
            }
        }
    };

    std::vector<std::thread> threads;
    int chunk = total / n_threads;
    for (int i = 0; i < n_threads; i++) {
        int from = i * chunk;
        int to   = (i == n_threads - 1) ? total : from + chunk;
        threads.emplace_back(worker, i, from, to);
    }
    for (auto& th : threads) th.join();

    double sweep_sec = std::chrono::duration<double>(
        std::chrono::high_resolution_clock::now() - sweep_start).count();
    std::cout << "\n\n";

    // ── Merge all results ────────────────────────
    std::vector<ComboResult> all_results;
    all_results.reserve(total);
    for (auto& v : thread_results)
        for (auto& cr : v)
            all_results.push_back(cr);

    // Sort by pnl descending
    std::sort(all_results.begin(), all_results.end(),
        [](const ComboResult& a, const ComboResult& b) {
            return a.r.pnl_total > b.r.pnl_total;
        });

    // ── CSV output ────────────────────────────────
    bool write_csv = (argc >= 3);
    if (write_csv) {
        std::ofstream csv(argv[2]);
        csv << "tp,sl,impulse_min,time_limit_s,pullback,vwap_trend,window,"
            << "n_total,wins,wr_pct,pnl_usd,"
            << "n_tp,pnl_tp_usd,"
            << "n_sl,pnl_sl_usd,"
            << "n_adverse,pnl_adverse_usd,"
            << "n_time,pnl_time_usd,"
            << "n_trail,pnl_trail_usd\n";
        for (auto& cr : all_results) {
            auto& p = cr.p; auto& r = cr.r;
            double wr = r.n_total > 0 ? 100.0 * r.wins / r.n_total : 0;
            csv << std::fixed << std::setprecision(3)
                << p.tp << "," << p.sl << "," << p.impulse_min << ","
                << p.time_limit_s << "," << p.pullback << ","
                << p.vwap_trend << "," << p.window << ","
                << r.n_total << "," << r.wins << ","
                << std::setprecision(1) << wr << ","
                << std::setprecision(0) << r.pnl_total*100 << ","
                << r.n_tp    << "," << r.pnl_tp*100    << ","
                << r.n_sl    << "," << r.pnl_sl*100    << ","
                << r.n_adverse << "," << r.pnl_adv*100 << ","
                << r.n_time  << "," << r.pnl_time*100  << ","
                << r.n_trail << "," << r.pnl_trail*100 << "\n";
        }
        std::cout << "  Full results (sorted by PnL) written to: " << argv[2] << "\n\n";
    }

    // ── Print top 10 ──────────────────────────────
    std::cout << "══════════════════════════════════════════════════════════════\n";
    std::cout << "  GoldFlow Sweep — " << total << " combos in "
              << std::fixed << std::setprecision(1) << sweep_sec << "s"
              << " (" << n_threads << " threads)\n";
    std::cout << "  TRAIL=ON trigger=" << TRAIL_TRIGGER << "pts lock=" << (int)(TRAIL_LOCK*100) << "%"
              << "  SESSION=ASIA(00-06)+LONDON(07-10)+NY(12-16)\n";
    std::cout << "══════════════════════════════════════════════════════════════\n\n";

    int show = std::min(10, (int)all_results.size());
    std::cout << "  TOP " << show << " RESULTS (sorted by PnL):\n\n";

    for (int i = 0; i < show; i++) {
        auto& p = all_results[i].p;
        auto& r = all_results[i].r;
        double wr = r.n_total > 0 ? 100.0 * r.wins / r.n_total : 0;
        std::cout << "  #" << std::left << std::setw(3) << (i+1)
                  << " TP=" << std::setw(5) << p.tp
                  << " SL=" << std::setw(5) << p.sl
                  << " IMP=" << std::setw(5) << p.impulse_min
                  << " TIME=" << std::setw(5) << p.time_limit_s
                  << " PB=" << std::setw(5) << p.pullback
                  << " VT=" << std::setw(6) << p.vwap_trend
                  << " WIN=" << std::setw(4) << p.window
                  << " | n=" << std::setw(5) << r.n_total
                  << " WR=" << std::setprecision(1) << std::setw(5) << wr << "%"
                  << " PnL=" << std::setprecision(0) << std::setw(8) << r.pnl_total*100 << " USD"
                  << " TR=" << r.n_trail
                  << "\n";
    }
    std::cout << "\n══════════════════════════════════════════════════════════════\n";

    return 0;
}
