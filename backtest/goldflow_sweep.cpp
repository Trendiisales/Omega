// goldflow_sweep.cpp
// GoldFlow parameter sweep — parallel, circular buffers, pre-computed VWAP
// Build: g++ -O3 -std=c++17 -pthread -o goldflow_sweep goldflow_sweep.cpp
// Run:   ./goldflow_sweep ticks.csv [results.csv] [n_threads]
//
// SPEEDUPS vs previous version:
//   1. Circular buffers — price_buf/vwap_buf use fixed arrays + head index, O(1) vs O(n)
//   2. Pre-computed VWAP — daily-reset cumulative VWAP computed once for all ticks,
//      stored in a shared array. Each combo reads it, never recomputes. ~36480x saving.
//   3. Session-index array — indices of session ticks pre-built once. Each combo
//      iterates only ~56M session ticks instead of 134M total. 2.4x saving.
//   4. Parallel threads — combos split across all cores.
//
// Sessions: ASIA(00-06 UTC) + LONDON(07-10 UTC) + NY(12-16 UTC)

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <deque>
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
// Tick (compact — only what we need)
// ─────────────────────────────────────────────
struct Tick
{
    uint64_t ts  = 0;
    float    ask = 0;
    float    bid = 0;
    float    mid = 0;
    float    vwap= 0;   // pre-computed daily-reset cumulative VWAP
    bool     session = false; // in any trading session
};

bool parse_tick_raw(const std::string& line, uint64_t& ts, double& ask, double& bid)
{
    if (line.empty()) return false;
    const char* p = line.c_str();
    char* end;
    ts = (uint64_t)strtoull(p, &end, 10);
    if (end == p || *end != ',') return false;
    p = end + 1;
    ask = strtod(p, &end);
    if (end == p || *end != ',') return false;
    p = end + 1;
    bid = strtod(p, &end);
    return (ask > 0 && bid > 0 && ask >= bid);
}

inline int      utc_hour(uint64_t ts) { return (int)((ts/1000/3600)%24); }
inline uint64_t utc_day (uint64_t ts) { return ts/1000/86400; }
inline bool in_session  (uint64_t ts)
{
    int h = utc_hour(ts);
    return (h>=0&&h<=6)||(h>=7&&h<=10)||(h>=12&&h<=16);
}

// ─────────────────────────────────────────────
// Circular buffer (fixed capacity, O(1) push)
// ─────────────────────────────────────────────
template<int CAP>
struct CircBuf
{
    float data[CAP] = {};
    int   head = 0;
    int   count = 0;

    void push(float v)
    {
        data[head] = v;
        head = (head + 1) % CAP;
        if (count < CAP) count++;
    }

    // index 0 = oldest, count-1 = newest
    float operator[](int i) const
    {
        int idx = (head - count + i + CAP * 4) % CAP;
        return data[idx];
    }

    float newest() const { return data[(head - 1 + CAP) % CAP]; }
    float oldest() const { return (*this)[0]; }
    bool  full()   const { return count == CAP; }
};

// ─────────────────────────────────────────────
// Sweep parameters
// ─────────────────────────────────────────────
struct SweepParams
{
    float    tp;
    float    sl;
    float    impulse_min;
    uint32_t time_limit_s;
    float    pullback;
    float    vwap_trend;
    int      window;
};

// ─────────────────────────────────────────────
// Fixed constants
// ─────────────────────────────────────────────
static const float  MAX_SPREAD      = 0.40f;
static const int    VWAP_TREND_LOOK = 30;
static const int    COOLDOWN_TICKS  = 300;
static const int    ADVERSE_WINDOW  = 30;
static const float  ADVERSE_MIN     = 2.0f;
static const bool   TRAIL_ENABLED   = true;
static const float  TRAIL_TRIGGER   = 6.0f;
static const float  TRAIL_LOCK      = 0.75f;
static const int    MAX_WINDOW      = 600;  // max window size for circular buffer

// ─────────────────────────────────────────────
// Result
// ─────────────────────────────────────────────
struct RunResult
{
    int   n_total=0, n_tp=0, n_sl=0, n_adverse=0, n_time=0, n_trail=0, wins=0;
    float pnl_total=0, pnl_tp=0, pnl_sl=0, pnl_adv=0, pnl_time=0, pnl_trail=0;
};

// ─────────────────────────────────────────────
// Ring-buffer monotonic deque — zero heap allocation
// ─────────────────────────────────────────────
template<int CAP>
struct RingDeque {
    struct E { float v; int i; };
    E    buf[CAP];
    int  head=0, tail=0;
    bool empty()  const { return head==tail; }
    void push_back(E e)  { buf[tail]=e; tail=(tail+1)%CAP; }
    void pop_back()      { tail=(tail-1+CAP)%CAP; }
    void pop_front()     { head=(head+1)%CAP; }
    E&   front()         { return buf[head]; }
    E&   back()          { return buf[(tail-1+CAP)%CAP]; }
};

struct SlidingMinMax
{
    RingDeque<640> mx, mn;
    int tick_idx = 0;

    void push(float v, int window)
    {
        int oldest = tick_idx - window;
        if (!mx.empty() && mx.front().i <= oldest) mx.pop_front();
        if (!mn.empty() && mn.front().i <= oldest) mn.pop_front();
        while (!mx.empty() && mx.back().v <= v) mx.pop_back();
        while (!mn.empty() && mn.back().v >= v) mn.pop_back();
        mx.push_back({v, tick_idx});
        mn.push_back({v, tick_idx});
        tick_idx++;
    }

    float max() const { return mx.buf[mx.head].v; }
    float min() const { return mn.buf[mn.head].v; }
    bool  ready(int window) const { return tick_idx >= window; }
};

// ─────────────────────────────────────────────
// Single backtest — uses pre-computed tick array
// ─────────────────────────────────────────────
RunResult run_backtest(const std::vector<Tick>& ticks, const SweepParams& p)
{
    RunResult res;

    SlidingMinMax smm;         // O(1) sliding hi/lo — replaces O(window) scan
    CircBuf<35>   vwap_buf;    // VWAP trend lookback (30+5)

    float hi = 0, lo = 0;
    bool  in_pos      = false;
    bool  is_long     = false;
    float entry       = 0;
    float tp_level    = 0;
    float sl_level    = 0;
    float trail_sl    = 0;
    bool  trail_active= false;
    float spread_open = 0;
    float mfe         = 0;
    float mae         = 0;
    int   pos_ticks   = 0;
    uint64_t entry_ts = 0;
    int   cooldown    = 0;

    for (const auto& t : ticks)
    {
        // Update sliding hi/lo and VWAP trend buffer
        smm.push(t.mid, p.window);
        vwap_buf.push(t.vwap);

        // All ticks in this array are session ticks — no filter needed
        float spread = t.ask - t.bid;
        if (spread > MAX_SPREAD) continue;

        if (cooldown > 0) cooldown--;

        // ── manage position ──────────────────────────────
        if (in_pos)
        {
            pos_ticks++;
            float exc = is_long ? t.mid - entry : entry - t.mid;
            if (exc > mfe) mfe = exc;
            if (exc < -mae) mae = -exc;

            if (TRAIL_ENABLED) {
                if (!trail_active && mfe >= TRAIL_TRIGGER) trail_active = true;
                if (trail_active) {
                    float locked    = mfe * TRAIL_LOCK;
                    float new_trail = is_long ? entry + locked : entry - locked;
                    if (is_long) trail_sl = std::max(trail_sl, new_trail);
                    else         trail_sl = std::min(trail_sl, new_trail);
                }
            }

            bool adverse = (pos_ticks <= ADVERSE_WINDOW && mae >= ADVERSE_MIN);

            int    why    = 0;  // 1=TP 2=SL 3=ADV 4=TIME 5=TRAIL
            float  exit_px = 0;

            if (is_long) {
                float px = t.bid;
                if      (px >= tp_level)                              { why=1; exit_px=tp_level; }
                else if (px <= sl_level)                              { why=adverse?3:2; exit_px=sl_level; }
                else if (TRAIL_ENABLED&&trail_active&&px<=trail_sl)  { why=5; exit_px=trail_sl; }
                else if (t.ts-entry_ts>=(uint64_t)p.time_limit_s*1000){ why=4; exit_px=px; }
            } else {
                float px = t.ask;
                if      (px <= tp_level)                              { why=1; exit_px=tp_level; }
                else if (px >= sl_level)                              { why=adverse?3:2; exit_px=sl_level; }
                else if (TRAIL_ENABLED&&trail_active&&px>=trail_sl)  { why=5; exit_px=trail_sl; }
                else if (t.ts-entry_ts>=(uint64_t)p.time_limit_s*1000){ why=4; exit_px=px; }
            }

            if (why)
            {
                float gross = is_long ? exit_px - entry : entry - exit_px;
                float net   = gross - spread_open;
                res.n_total++; res.pnl_total += net;
                if (net > 0) res.wins++;
                switch (why) {
                    case 1: res.n_tp++;      res.pnl_tp    += net; break;
                    case 2: res.n_sl++;      res.pnl_sl    += net; break;
                    case 3: res.n_adverse++; res.pnl_adv   += net; break;
                    case 4: res.n_time++;    res.pnl_time  += net; break;
                    case 5: res.n_trail++;   res.pnl_trail += net; break;
                }
                in_pos = false; trail_active = false;
            }
        }

        // ── entry ─────────────────────────────────────────
        if (!in_pos && cooldown == 0 && smm.ready(p.window))
        {
            hi = smm.max();
            lo = smm.min();

            if (hi - lo >= p.impulse_min)
            {
                float impulse  = hi - lo;
                float pb_long  = hi - p.pullback * impulse;
                float pb_short = lo + p.pullback * impulse;

                // VWAP trend from pre-computed vwap_buf
                bool trend_up = false, trend_down = false;
                if (vwap_buf.count >= VWAP_TREND_LOOK) {
                    float delta = vwap_buf[vwap_buf.count-1] - vwap_buf[vwap_buf.count-VWAP_TREND_LOOK];
                    trend_up   = delta >  p.vwap_trend;
                    trend_down = delta < -p.vwap_trend;
                }

                float vwap = t.vwap;
                bool can_long  = (t.mid <= pb_long  && t.mid > vwap && trend_up);
                bool can_short = (t.mid >= pb_short && t.mid < vwap && trend_down);

                if (can_long || can_short)
                {
                    in_pos  = true;
                    is_long = can_long;
                    if (is_long) { entry=t.ask; tp_level=entry+p.tp; sl_level=entry-p.sl; trail_sl=sl_level; }
                    else         { entry=t.bid; tp_level=entry-p.tp; sl_level=entry+p.sl; trail_sl=sl_level; }
                    spread_open=spread; entry_ts=t.ts; pos_ticks=0; mfe=0; mae=0;
                    trail_active=false; cooldown=COOLDOWN_TICKS;
                }
            }
        }
    }
    return res;
}

// ─────────────────────────────────────────────
// Combo result
// ─────────────────────────────────────────────
struct ComboResult { SweepParams p; RunResult r; };

// ─────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────
int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cout << "usage: goldflow_sweep ticks.csv [results.csv] [n_threads]\n";
        return 0;
    }

    // ── Load + preprocess ticks ──────────────────
    std::cout << "Loading ticks from " << argv[1] << " ...\n";
    auto t0 = std::chrono::high_resolution_clock::now();

    std::vector<Tick> ticks;
    ticks.reserve(140000000);

    {
        std::ifstream file(argv[1]);
        if (!file.is_open()) { std::cout << "cannot open: " << argv[1] << "\n"; return 1; }

        // Pre-compute daily-reset cumulative VWAP for every tick
        double   vwap_pv    = 0;
        uint64_t vwap_count = 0;
        uint64_t vwap_day   = 0;

        std::string line;
        while (getline(file, line)) {
            uint64_t ts; double ask, bid;
            if (!parse_tick_raw(line, ts, ask, bid)) continue;

            double mid = (ask + bid) * 0.5;

            // Daily-reset cumulative VWAP
            uint64_t day = utc_day(ts);
            if (day != vwap_day) { vwap_pv=0; vwap_count=0; vwap_day=day; }
            vwap_pv += mid; vwap_count++;
            float vwap = (float)(vwap_pv / (double)vwap_count);

            Tick t;
            t.ts      = ts;
            t.ask     = (float)ask;
            t.bid     = (float)bid;
            t.mid     = (float)mid;
            t.vwap    = vwap;
            t.session = in_session(ts);
            ticks.push_back(t);
        }
    }

    double load_sec = std::chrono::duration<double>(
        std::chrono::high_resolution_clock::now() - t0).count();

    uint64_t sess_count = 0;
    for (auto& t : ticks) if (t.session) sess_count++;

    // ── Build session-only array ─────────────────
    // CRITICAL speedup: each combo iterates 56M ticks not 134M.
    // VWAP already embedded per tick so warmup is preserved.
    std::vector<Tick> sess_ticks;
    sess_ticks.reserve(sess_count);
    for (auto& t : ticks) if (t.session) sess_ticks.push_back(t);
    ticks.clear();
    ticks.shrink_to_fit(); // free 134M tick memory

    std::cout << "Loaded in " << std::fixed << std::setprecision(1) << load_sec << "s"
              << "  session ticks: " << sess_ticks.size() << " (was " << (sess_ticks.size()*134636174/sess_count) << " full)\n\n";

    // ── Tight grid around sweep best ─────────────
    // Best confirmed: tp=14, sl=7, imp=5, time=1800, pb=0.45, vt=0, win=600
    // Sweep 3 key variables finely, hold others at best known value
    // ~800 combos => ~5min on M4 Pro
    std::vector<float>    tp_vals     = {10.0f, 12.0f, 14.0f, 16.0f, 18.0f, 20.0f};
    std::vector<float>    sl_vals     = {5.0f,  6.0f,  7.0f,  8.0f,  9.0f};
    std::vector<float>    imp_vals    = {4.0f,  5.0f,  6.0f,  7.0f};
    std::vector<uint32_t> time_vals   = {1200,  1800,  2700,  3600};
    std::vector<float>    pb_vals     = {0.35f, 0.40f, 0.45f, 0.50f};
    std::vector<float>    vwap_t_vals = {0.0f,  0.001f,0.002f};
    std::vector<int>      win_vals    = {600};

    std::vector<SweepParams> combos;
    combos.reserve(50000);
    for (float tp:tp_vals) for (float sl:sl_vals) for (float imp:imp_vals)
    for (uint32_t tl:time_vals) for (float pb:pb_vals)
    for (float vt:vwap_t_vals) for (int win:win_vals)
    {
        if (tp/sl < 1.2f) continue;
        combos.push_back({tp,sl,imp,tl,pb,vt,win});
    }

    int total = (int)combos.size();
    int n_threads = (int)std::thread::hardware_concurrency();
    if (n_threads < 1) n_threads = 4;
    if (argc >= 4) try { n_threads = std::stoi(argv[3]); } catch (...) {}
    if (n_threads > total) n_threads = total;

    std::cout << "Combos: " << total << "  Threads: " << n_threads << "\n";
    std::cout << "Progress every 500 combos.\n\n";

    // ── Thread workers ────────────────────────────
    std::vector<std::vector<ComboResult>> thread_results(n_threads);
    for (auto& v : thread_results) v.reserve(total/n_threads+1);

    std::atomic<int> progress{0};
    std::mutex       mtx;
    auto sweep_start = std::chrono::high_resolution_clock::now();

    auto worker = [&](int tid, int from, int to) {
        for (int i = from; i < to; i++) {
            thread_results[tid].push_back({combos[i], run_backtest(sess_ticks, combos[i])});
            int done = ++progress;
            if (done % 500 == 0) {
                double el  = std::chrono::duration<double>(
                    std::chrono::high_resolution_clock::now() - sweep_start).count();
                double eta = (total - done) / (done / el);
                std::lock_guard<std::mutex> lock(mtx);
                std::cout << "  [" << std::setw(6) << done << "/" << total << "]"
                          << "  " << std::fixed << std::setprecision(1) << done/el << "/s"
                          << "  ETA " << std::setprecision(0) << eta << "s"
                          << "  (" << std::setprecision(1) << el << "s elapsed)\n"
                          << std::flush;
            }
        }
    };

    std::vector<std::thread> threads;
    int chunk = total / n_threads;
    for (int i = 0; i < n_threads; i++) {
        int from = i * chunk;
        int to   = (i == n_threads-1) ? total : from+chunk;
        threads.emplace_back(worker, i, from, to);
    }
    for (auto& th : threads) th.join();

    double sweep_sec = std::chrono::duration<double>(
        std::chrono::high_resolution_clock::now() - sweep_start).count();

    // ── Merge + sort ──────────────────────────────
    std::vector<ComboResult> all;
    all.reserve(total);
    for (auto& v : thread_results) for (auto& cr : v) all.push_back(cr);
    std::sort(all.begin(), all.end(),
        [](const ComboResult& a, const ComboResult& b){ return a.r.pnl_total > b.r.pnl_total; });

    // ── CSV ───────────────────────────────────────
    if (argc >= 3) {
        std::ofstream csv(argv[2]);
        csv << "tp,sl,impulse_min,time_limit_s,pullback,vwap_trend,window,"
            << "n_total,wins,wr_pct,pnl_usd,n_tp,pnl_tp,n_sl,pnl_sl,"
            << "n_adverse,pnl_adverse,n_time,pnl_time,n_trail,pnl_trail\n";
        for (auto& cr : all) {
            auto& p=cr.p; auto& r=cr.r;
            float wr = r.n_total>0 ? 100.f*r.wins/r.n_total : 0;
            csv << std::fixed << std::setprecision(3)
                << p.tp<<","<<p.sl<<","<<p.impulse_min<<","<<p.time_limit_s<<","
                << p.pullback<<","<<p.vwap_trend<<","<<p.window<<","
                << r.n_total<<","<<r.wins<<","
                << std::setprecision(1)<<wr<<","
                << std::setprecision(0)<<r.pnl_total*100<<","
                << r.n_tp<<","<<r.pnl_tp*100<<","
                << r.n_sl<<","<<r.pnl_sl*100<<","
                << r.n_adverse<<","<<r.pnl_adv*100<<","
                << r.n_time<<","<<r.pnl_time*100<<","
                << r.n_trail<<","<<r.pnl_trail*100<<"\n";
        }
        std::cout << "Results written to: " << argv[2] << "\n\n";
    }

    // ── Top 10 ────────────────────────────────────
    std::cout << "══════════════════════════════════════════════════════════════\n";
    std::cout << "  GoldFlow Sweep — " << total << " combos in "
              << std::fixed << std::setprecision(1) << sweep_sec << "s"
              << " (" << n_threads << " threads)\n";
    std::cout << "  TRAIL=ON  SESSION=ASIA+LONDON+NY\n";
    std::cout << "══════════════════════════════════════════════════════════════\n\n";
    std::cout << "  TOP 10:\n\n";
    int show = std::min(10,(int)all.size());
    for (int i=0;i<show;i++) {
        auto& p=all[i].p; auto& r=all[i].r;
        float wr = r.n_total>0 ? 100.f*r.wins/r.n_total : 0;
        std::cout << "  #"<<std::left<<std::setw(3)<<(i+1)
                  <<" TP="<<std::setw(5)<<p.tp<<" SL="<<std::setw(4)<<p.sl
                  <<" IMP="<<std::setw(5)<<p.impulse_min<<" TIME="<<std::setw(5)<<p.time_limit_s
                  <<" PB="<<std::setw(5)<<p.pullback<<" VT="<<std::setw(6)<<p.vwap_trend
                  <<" WIN="<<std::setw(4)<<p.window
                  <<" | n="<<std::setw(5)<<r.n_total
                  <<" WR="<<std::setprecision(1)<<std::setw(5)<<wr<<"%"
                  <<" PnL="<<std::setprecision(0)<<std::setw(8)<<r.pnl_total*100<<" USD"
                  <<" TR="<<r.n_trail<<"\n";
    }
    std::cout << "\n══════════════════════════════════════════════════════════════\n";
    return 0;
}
