// =============================================================================
// gbpusd_london_open_entry_sweep.cpp -- Entry-side sweep (Plan A/B v3)
// =============================================================================
//
// 2026-05-12 (Claude / Jo): After Plan A/B established the engine is structurally
//   net-negative on the explored exit-side grid, scan entry-side params:
//     STRUCTURE_LOOKBACK ∈ {300, 600, 900}
//     MIN_RANGE          ∈ {0.0008, 0.0010, 0.0012, 0.0015}
//     MAX_RANGE          ∈ {0.0050, 0.0075, 0.0100}
//     MIN_BREAK_TICKS    ∈ {3, 5, 8}
//   3 x 4 x 3 x 3 = 108 configs. Pinned at Plan B best exit-side:
//     TP_RR=2.0, SL_FRAC=0.60, TRAIL_FRAC=0.20, MFE_TRAIL_FRAC=0.40
//     MAX_SPREAD=0.00020 (Plan A best)
//
//   NOTE: STRUCTURE_LOOKBACK is normally a constexpr, but here we vary it per
//   engine; each engine maintains its own m_window deque sized at runtime.
//   This requires a slightly different harness: instead of a single shared
//   ring with monotonic deque, each engine owns its window. The cost is N x
//   per-tick max/min, but N=108 keeps it acceptable. We use 3 shared rings
//   one per STRUCTURE_LOOKBACK group {300, 600, 900}.
//
// BUILD
//   g++ -std=c++17 -O3 -DNDEBUG -I include \
//       backtest/gbpusd_london_open_entry_sweep.cpp -o /tmp/gbpusd_entry
// =============================================================================

#define OMEGA_BACKTEST 1

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace gbpfx {

struct TradeRecord {
    int64_t ts_ms_entry  = 0;
    int64_t ts_ms_exit   = 0;
    bool    is_long      = false;
    double  entry_price  = 0.0;
    double  exit_price   = 0.0;
    std::string exit_reason;
    double  gross_usd    = 0.0;
    double  spread_at_entry_pts = 0.0;
    double  modeled_cost_usd = 0.0;
    double  net_usd      = 0.0;
    int     hour_utc     = 0;
};

class GbpusdLondonOpenEngine {
public:
    int     STRUCTURE_LOOKBACK   = 600;
    int     MIN_BREAK_TICKS      = 5;
    double  MIN_RANGE            = 0.0012;
    double  MAX_RANGE            = 0.0075;

    static constexpr int    MIN_ENTRY_TICKS      = 60;
    static constexpr double SL_BUFFER            = 0.0002;
    static constexpr double MIN_TRAIL_ARM_PTS    = 0.0006;
    static constexpr int    MIN_TRAIL_ARM_SECS   = 30;
    static constexpr double BE_TRIGGER_PTS       = 0.0006;
    static constexpr double BE_OFFSET_PTS        = 0.00015;
    static constexpr double SAME_LEVEL_BLOCK_PTS = 0.0012;
    static constexpr int    SAME_LEVEL_POST_SL_BLOCK_S  = 1200;
    static constexpr int    SAME_LEVEL_POST_WIN_BLOCK_S = 600;
    static constexpr double MAX_FILL_SPREAD      = 0.00050;
    static constexpr double RISK_DOLLARS         = 30.0;
    static constexpr double USD_PER_PRICE_UNIT   = 10000.0;
    static constexpr double ENTRY_SIZE_DEFAULT   = 0.10;
    static constexpr double LOT_MIN              = 0.01;
    static constexpr double LOT_MAX              = 0.10;
    static constexpr int    PENDING_TIMEOUT_S    = 180;
    static constexpr int    COOLDOWN_S           = 120;
    static constexpr int    SESSION_START_HOUR_UTC = 7;
    static constexpr int    SESSION_END_HOUR_UTC   = 10;
    static constexpr int    EXPANSION_HISTORY_LEN = 20;
    static constexpr int    EXPANSION_MIN_HISTORY = 5;
    static constexpr double EXPANSION_MULT        = 1.10;

    // Exit-side held at Plan B best
    double SL_FRAC = 0.60;
    double TP_RR = 2.0;
    double TRAIL_FRAC = 0.20;
    double MFE_TRAIL_FRAC = 0.40;
    double MAX_SPREAD = 0.00020;

    enum class Phase { IDLE, ARMED, PENDING, LIVE, COOLDOWN };
    Phase phase = Phase::IDLE;

    struct LivePos {
        bool    active   = false;
        bool    is_long  = false;
        double  entry    = 0.0;
        double  tp       = 0.0;
        double  sl       = 0.0;
        double  size     = ENTRY_SIZE_DEFAULT;
        double  mfe      = 0.0;
        double  mae      = 0.0;
        double  spread_at_entry = 0.0;
        int64_t entry_ts_ms = 0;
        int     hour_utc_at_entry = 0;
        bool    be_locked = false;
    } pos;

    double bracket_high = 0.0, bracket_low = 0.0, range = 0.0;
    std::function<void(const TradeRecord&)> trade_sink = nullptr;

    void on_tick(double bid, double ask, int64_t now_ms,
                 int win_count, double w_hi, double w_lo, int hour_utc) noexcept
    {
        if (bid <= 0.0 || ask <= 0.0) return;
        const double mid = (bid + ask) * 0.5;
        const double spread = ask - bid;
        const int64_t now_s = now_ms / 1000;
        ++m_ticks_received;

        if (phase == Phase::COOLDOWN) {
            if (now_s - m_cooldown_start >= COOLDOWN_S) phase = Phase::IDLE;
            else return;
        }
        if (phase == Phase::LIVE) { _manage(bid, ask, mid, now_s, now_ms); return; }
        if (phase == Phase::PENDING) {
            const bool would_L = (ask >= bracket_high);
            const bool would_S = (bid <= bracket_low);
            if ((would_L || would_S) && spread > MAX_FILL_SPREAD) {
                phase = Phase::IDLE; bracket_high = bracket_low = range = 0.0; return;
            }
            if (would_L && would_S) { _fill(true, bracket_high, spread, now_ms, hour_utc); return; }
            if (would_L) { _fill(true, bracket_high, spread, now_ms, hour_utc); return; }
            if (would_S) { _fill(false, bracket_low, spread, now_ms, hour_utc); return; }
            if (now_s - m_armed_ts > PENDING_TIMEOUT_S) {
                phase = Phase::IDLE; bracket_high = bracket_low = range = 0.0;
            }
            return;
        }

        if (m_ticks_received < MIN_ENTRY_TICKS) return;
        if (win_count < STRUCTURE_LOOKBACK) return;
        if (spread > MAX_SPREAD) return;
        if (hour_utc < SESSION_START_HOUR_UTC || hour_utc >= SESSION_END_HOUR_UTC) return;

        range = w_hi - w_lo;

        if (phase == Phase::IDLE) {
            if (m_sl_price > 0.0 && now_s < m_sl_cooldown_ts) {
                if (std::fabs(w_hi - m_sl_price) < SAME_LEVEL_BLOCK_PTS ||
                    std::fabs(w_lo - m_sl_price) < SAME_LEVEL_BLOCK_PTS) return;
            }
            if (m_win_exit_price > 0.0 && now_s < m_win_exit_block_ts) {
                if (std::fabs(w_hi - m_win_exit_price) < SAME_LEVEL_BLOCK_PTS ||
                    std::fabs(w_lo - m_win_exit_price) < SAME_LEVEL_BLOCK_PTS) return;
            }
            if (range >= MIN_RANGE && range <= MAX_RANGE) {
                phase = Phase::ARMED;
                bracket_high = w_hi; bracket_low = w_lo;
                m_inside_ticks = 0; m_armed_ts = now_s;
            }
            return;
        }

        if (phase == Phase::ARMED) {
            bracket_high = std::max(bracket_high, w_hi);
            bracket_low  = std::min(bracket_low,  w_lo);
            range = bracket_high - bracket_low;
            if (range > MAX_RANGE) { phase = Phase::IDLE; bracket_high = bracket_low = range = 0.0; return; }
            if (range < MIN_RANGE) { phase = Phase::IDLE; return; }
            if (mid >= bracket_low && mid <= bracket_high) ++m_inside_ticks;
            else { m_inside_ticks = 0; phase = Phase::IDLE;
                   bracket_high = bracket_low = range = 0.0; return; }
            if (m_inside_ticks < MIN_BREAK_TICKS) return;

            const double sl_dist = range * SL_FRAC + SL_BUFFER;
            const double tp_dist = sl_dist * TP_RR;
            if (tp_dist < spread*2.0 + 0.0001) { phase = Phase::IDLE; return; }

            m_range_history.push_back(range);
            if ((int)m_range_history.size() > EXPANSION_HISTORY_LEN) m_range_history.pop_front();
            if ((int)m_range_history.size() < EXPANSION_MIN_HISTORY) {
                phase = Phase::IDLE; bracket_high = bracket_low = 0.0; return;
            }
            {
                std::vector<double> s(m_range_history.begin(), m_range_history.end());
                std::sort(s.begin(), s.end());
                const size_t n = s.size();
                const double med = (n%2==1) ? s[n/2] : 0.5*(s[n/2-1]+s[n/2]);
                if (range < med * EXPANSION_MULT) {
                    phase = Phase::IDLE; bracket_high = bracket_low = 0.0; return;
                }
            }
            const double risk_lot = (sl_dist * USD_PER_PRICE_UNIT > 0.0)
                ? (RISK_DOLLARS / (sl_dist * USD_PER_PRICE_UNIT)) : LOT_MAX;
            m_pending_lot = std::max(LOT_MIN, std::min(LOT_MAX, risk_lot));
            phase = Phase::PENDING; m_armed_ts = now_s;
        }
    }

private:
    int     m_ticks_received = 0;
    int     m_inside_ticks   = 0;
    int64_t m_armed_ts       = 0;
    int64_t m_cooldown_start = 0;
    int64_t m_sl_cooldown_ts = 0;
    double  m_sl_price       = 0.0;
    double  m_win_exit_price = 0.0;
    int64_t m_win_exit_block_ts = 0;
    double  m_pending_lot    = ENTRY_SIZE_DEFAULT;
    std::deque<double> m_range_history;

    void _fill(bool is_long, double px, double sp, int64_t now_ms, int hr) noexcept {
        const double sl_dist = range * SL_FRAC + SL_BUFFER;
        const double tp_dist = sl_dist * TP_RR;
        pos.active = true; pos.is_long = is_long; pos.entry = px;
        pos.sl = is_long ? (px - sl_dist) : (px + sl_dist);
        pos.tp = is_long ? (px + tp_dist) : (px - tp_dist);
        pos.size = m_pending_lot; pos.mfe = 0; pos.mae = 0;
        pos.spread_at_entry = sp; pos.entry_ts_ms = now_ms;
        pos.hour_utc_at_entry = hr; pos.be_locked = false;
        phase = Phase::LIVE;
    }

    void _manage(double bid, double ask, double mid, int64_t now_s, int64_t now_ms) noexcept {
        if (!pos.active) return;
        const double move = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
        if (move > pos.mfe) pos.mfe = move;
        if (move < pos.mae) pos.mae = move;
        const int64_t held_s = now_s - pos.entry_ts_ms / 1000;
        if (move > 0 && !pos.be_locked && pos.mfe >= BE_TRIGGER_PTS) {
            const double off = (move >= BE_OFFSET_PTS) ? BE_OFFSET_PTS : 0.0;
            const double t = pos.is_long ? (pos.entry + off) : (pos.entry - off);
            if (pos.is_long  && t > pos.sl) pos.sl = t;
            if (!pos.is_long && t < pos.sl) pos.sl = t;
            pos.be_locked = true;
        }
        if (move > 0 && pos.mfe >= MIN_TRAIL_ARM_PTS && held_s >= MIN_TRAIL_ARM_SECS) {
            const double mt = pos.mfe * MFE_TRAIL_FRAC;
            const double rt = range * TRAIL_FRAC;
            const double td = (mt > 0.0) ? std::min(rt, mt) : rt;
            const double tsl = pos.is_long ? (pos.entry + pos.mfe - td)
                                           : (pos.entry - pos.mfe + td);
            if (pos.is_long  && tsl > pos.sl) pos.sl = tsl;
            if (!pos.is_long && tsl < pos.sl) pos.sl = tsl;
        }
        const bool tp_hit = pos.is_long ? (ask >= pos.tp) : (bid <= pos.tp);
        if (tp_hit) { _close(pos.tp, "TP_HIT", now_s, now_ms); return; }
        const bool sl_hit = pos.is_long ? (bid <= pos.sl) : (ask >= pos.sl);
        if (sl_hit) {
            const double exit_px = pos.is_long ? bid : ask;
            const bool be = (pos.sl <= pos.entry + 0.00005) && (pos.sl >= pos.entry - 0.00005);
            const bool in_profit = pos.is_long ? (pos.sl > pos.entry + 0.00005)
                                               : (pos.sl < pos.entry - 0.00005);
            const char* r = be ? "BE_HIT" : (in_profit ? "TRAIL_HIT" : "SL_HIT");
            _close(exit_px, r, now_s, now_ms);
        }
    }

    void _close(double exit_px, const char* reason, int64_t now_s, int64_t now_ms) noexcept {
        if (!pos.active) return;
        const bool is_long_ = pos.is_long; const double entry_ = pos.entry;
        const double size_ = pos.size; const double sp_ = pos.spread_at_entry;
        const int64_t entry_ts_ = pos.entry_ts_ms;
        const int hr_ = pos.hour_utc_at_entry;
        const double gross_pts = is_long_ ? (exit_px - entry_) : (entry_ - exit_px);
        const double gross = gross_pts * size_ * 100000.0;
        const double cost = 6.0*size_ + sp_*100000.0*size_ + 0.0002*100000.0*size_;
        const double net = gross - cost;
        if (std::string(reason) == "SL_HIT") {
            m_sl_cooldown_ts = now_s + SAME_LEVEL_POST_SL_BLOCK_S; m_sl_price = entry_;
        }
        if (std::string(reason) == "TRAIL_HIT" || std::string(reason) == "TP_HIT") {
            m_win_exit_price = exit_px; m_win_exit_block_ts = now_s + SAME_LEVEL_POST_WIN_BLOCK_S;
        }
        TradeRecord tr;
        tr.ts_ms_entry = entry_ts_; tr.ts_ms_exit = now_ms;
        tr.is_long = is_long_; tr.entry_price = entry_; tr.exit_price = exit_px;
        tr.exit_reason = reason; tr.gross_usd = gross;
        tr.spread_at_entry_pts = sp_; tr.modeled_cost_usd = cost;
        tr.net_usd = net; tr.hour_utc = hr_;
        pos = LivePos{}; phase = Phase::COOLDOWN;
        m_cooldown_start = now_s; bracket_high = bracket_low = range = 0.0;
        if (trade_sink) trade_sink(tr);
    }
};

struct HistTickStreamer {
    std::ifstream f; std::string line; bool opened = false;
    bool open(const char* p) {
        f.open(p); if (!f.is_open()) { std::fprintf(stderr,"err: %s\n",p); return false; }
        opened = true; return true;
    }
    static int64_t parse_ts(const char* s, size_t len) {
        if (len < 18) return -1;
        int Y=(s[0]-'0')*1000+(s[1]-'0')*100+(s[2]-'0')*10+(s[3]-'0');
        int M=(s[4]-'0')*10+(s[5]-'0'); int D=(s[6]-'0')*10+(s[7]-'0');
        int hh=(s[9]-'0')*10+(s[10]-'0'); int mm=(s[11]-'0')*10+(s[12]-'0');
        int ss=(s[13]-'0')*10+(s[14]-'0'); int ms=(s[15]-'0')*100+(s[16]-'0')*10+(s[17]-'0');
        struct tm utc{}; utc.tm_year=Y-1900; utc.tm_mon=M-1; utc.tm_mday=D;
        utc.tm_hour=hh; utc.tm_min=mm; utc.tm_sec=ss;
        return (int64_t)timegm(&utc)*1000 + ms;
    }
    bool next(int64_t& ts, double& b, double& a) {
        if (!opened) return false;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            const char* p = line.c_str();
            const char* c1 = std::strchr(p,','); if (!c1) continue;
            const char* c2 = std::strchr(c1+1,','); if (!c2) continue;
            ts = parse_ts(p, (size_t)(c1-p)); if (ts < 0) continue;
            char buf[32]; size_t L1=(size_t)(c2-(c1+1));
            if (L1 >= sizeof(buf)) continue;
            std::memcpy(buf,c1+1,L1); buf[L1]=0; b = std::strtod(buf,nullptr);
            const char* c3 = std::strchr(c2+1,',');
            size_t L2 = c3 ? (size_t)(c3-(c2+1)) : std::strlen(c2+1);
            if (L2 >= sizeof(buf)) continue;
            std::memcpy(buf,c2+1,L2); buf[L2]=0; a = std::strtod(buf,nullptr);
            return true;
        }
        return false;
    }
};

static inline int hour_utc(int64_t ts_ms) {
    time_t t=(time_t)(ts_ms/1000); struct tm u{}; gmtime_r(&t,&u); return u.tm_hour;
}

// Monotonic-deque rolling max/min for a given window size.
struct RollingMaxMin {
    int win;
    std::vector<double> ring;
    int size = 0;
    int64_t idx = 0;
    std::deque<int64_t> dmax, dmin;
    void init(int w) { win = w; ring.assign(w, 0.0); }
    void push(double v) {
        const int64_t expire = idx - win + 1;
        while (!dmax.empty() && dmax.front() < expire) dmax.pop_front();
        while (!dmin.empty() && dmin.front() < expire) dmin.pop_front();
        while (!dmax.empty() && ring[dmax.back() % win] <= v) dmax.pop_back();
        while (!dmin.empty() && ring[dmin.back() % win] >= v) dmin.pop_back();
        ring[idx % win] = v;
        dmax.push_back(idx); dmin.push_back(idx);
        if (size < win) ++size;
        ++idx;
    }
    bool ready() const { return size >= win; }
    double hi() const { return ring[dmax.front() % win]; }
    double lo() const { return ring[dmin.front() % win]; }
};

} // namespace gbpfx

struct Cfg {
    int    LOOKBACK;
    double MIN_RANGE;
    double MAX_RANGE;
    int    MIN_BREAK;
};
struct Result {
    Cfg cfg;
    int n=0, w=0, tp=0, trail=0, sl=0, be=0;
    double gross=0, net=0, cost=0, ws=0, ls=0;
    double eq=0, peak=0, dd=0;
};

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr,"usage: %s <csv...>\n",argv[0]); return 2; }
    std::vector<std::string> csv_paths;
    std::string out = "backtest/gbpusd_entry_sweep.csv";
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a,"--out")==0 && i+1<argc) out = argv[++i];
        else if (a[0] != '-') csv_paths.push_back(a);
    }
    if (csv_paths.empty()) { std::fprintf(stderr,"err: no inputs\n"); return 2; }
    std::sort(csv_paths.begin(), csv_paths.end());

    std::vector<int>    lb_grid = {300, 600, 900};
    std::vector<double> minr    = {0.0008, 0.0010, 0.0012, 0.0015};
    std::vector<double> maxr    = {0.0050, 0.0075, 0.0100};
    std::vector<int>    mbk     = {3, 5, 8};

    std::vector<Cfg> combos;
    for (int L : lb_grid) for (double mn : minr) for (double mx : maxr) for (int b : mbk)
        combos.push_back({L, mn, mx, b});
    const int N = (int)combos.size();
    std::printf("[entry] %d combos\n", N);

    std::vector<gbpfx::GbpusdLondonOpenEngine> engines(N);
    std::vector<Result> results(N);
    for (int i = 0; i < N; ++i) {
        engines[i].STRUCTURE_LOOKBACK = combos[i].LOOKBACK;
        engines[i].MIN_RANGE = combos[i].MIN_RANGE;
        engines[i].MAX_RANGE = combos[i].MAX_RANGE;
        engines[i].MIN_BREAK_TICKS = combos[i].MIN_BREAK;
        results[i].cfg = combos[i];
        Result* rp = &results[i];
        engines[i].trade_sink = [rp](const gbpfx::TradeRecord& tr) {
            Result& r = *rp;
            r.n++; r.gross += tr.gross_usd; r.net += tr.net_usd; r.cost += tr.modeled_cost_usd;
            if (tr.net_usd > 0) { r.w++; r.ws += tr.net_usd; } else r.ls += tr.net_usd;
            r.eq += tr.net_usd;
            if (r.eq > r.peak) r.peak = r.eq;
            const double d = r.peak - r.eq;
            if (d > r.dd) r.dd = d;
            if      (tr.exit_reason == "TP_HIT")    r.tp++;
            else if (tr.exit_reason == "TRAIL_HIT") r.trail++;
            else if (tr.exit_reason == "SL_HIT")    r.sl++;
            else if (tr.exit_reason == "BE_HIT")    r.be++;
        };
    }

    // 3 rolling windows: 300, 600, 900. Mapping per engine.
    gbpfx::RollingMaxMin rmm300, rmm600, rmm900;
    rmm300.init(600);  // engine's m_window cap = LOOKBACK*2, so 300 -> 600 cap, 600 -> 1200, 900 -> 1800
    rmm600.init(1200);
    rmm900.init(1800);

    gbpfx::HistTickStreamer s;
    int64_t ts_ms = 0; double bid = 0, ask = 0;
    int64_t total = 0;
    const auto t0 = std::chrono::steady_clock::now();

    for (const auto& path : csv_paths) {
        std::printf("[entry] streaming %s\n", path.c_str());
        std::fflush(stdout);
        if (!s.open(path.c_str())) continue;
        int64_t fc = 0;
        while (s.next(ts_ms, bid, ask)) {
            ++fc; ++total;
            if (bid <= 0.0 || ask <= 0.0) continue;
            const double mid = 0.5 * (bid + ask);
            rmm300.push(mid); rmm600.push(mid); rmm900.push(mid);
            const int hr = gbpfx::hour_utc(ts_ms);
            const int wc300 = rmm300.size, wc600 = rmm600.size, wc900 = rmm900.size;
            const double hi300 = wc300 >= 300 ? rmm300.hi() : 0.0;
            const double lo300 = wc300 >= 300 ? rmm300.lo() : 0.0;
            const double hi600 = wc600 >= 600 ? rmm600.hi() : 0.0;
            const double lo600 = wc600 >= 600 ? rmm600.lo() : 0.0;
            const double hi900 = wc900 >= 900 ? rmm900.hi() : 0.0;
            const double lo900 = wc900 >= 900 ? rmm900.lo() : 0.0;
            for (int i = 0; i < N; ++i) {
                if (engines[i].STRUCTURE_LOOKBACK == 300)
                    engines[i].on_tick(bid, ask, ts_ms, wc300, hi300, lo300, hr);
                else if (engines[i].STRUCTURE_LOOKBACK == 600)
                    engines[i].on_tick(bid, ask, ts_ms, wc600, hi600, lo600, hr);
                else
                    engines[i].on_tick(bid, ask, ts_ms, wc900, hi900, lo900, hr);
            }
            if (total % 5000000 == 0) {
                const auto dt = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now()-t0).count();
                std::printf("  [entry] %lld ticks elapsed=%llds\n",
                            (long long)total, (long long)dt);
                std::fflush(stdout);
            }
        }
        s.f.close();
        std::printf("  [entry] %lld ticks in %s\n",(long long)fc, path.c_str());
        std::fflush(stdout);
    }
    const auto dt = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now()-t0).count();
    std::printf("[entry] processed %lld ticks across %d engines in %llds\n",
                (long long)total, N, (long long)dt);

    std::vector<int> order(N);
    for (int i = 0; i < N; ++i) order[i] = i;
    std::sort(order.begin(), order.end(),
              [&](int a, int b){ return results[a].net > results[b].net; });

    std::ofstream lf(out);
    lf << "rank,combo_id,LOOKBACK,MIN_RANGE,MAX_RANGE,MIN_BREAK,"
          "n_trades,n_wins,WR_pct,net_usd,gross_usd,cost_usd,mean_net,PF,max_dd,"
          "tp,trail,sl,be\n";
    for (int rk = 0; rk < N; ++rk) {
        const int ci = order[rk]; const Result& r = results[ci];
        const double wr = r.n>0 ? 100.0*r.w/r.n : 0.0;
        const double mn = r.n>0 ? r.net/r.n : 0.0;
        const double pf = (std::fabs(r.ls)>1e-9) ? -r.ws/r.ls : 0.0;
        lf << (rk+1) << "," << ci << ","
           << r.cfg.LOOKBACK << ","
           << std::fixed << std::setprecision(4) << r.cfg.MIN_RANGE << ","
           << r.cfg.MAX_RANGE << "," << r.cfg.MIN_BREAK << ","
           << r.n << "," << r.w << ","
           << std::setprecision(2) << wr << ","
           << r.net << "," << r.gross << "," << r.cost << ","
           << std::setprecision(4) << mn << "," << pf << ","
           << std::setprecision(2) << r.dd << ","
           << r.tp << "," << r.trail << "," << r.sl << "," << r.be << "\n";
    }
    lf.close();

    std::printf("\n=== TOP 10 by net_usd ===\n");
    std::printf("%4s %4s %5s %8s %8s %4s %7s %12s %8s\n","rk","id","LB","MIN_R","MAX_R","BRK","trades","net$","WR%");
    for (int rk = 0; rk < std::min(10, N); ++rk) {
        const int ci = order[rk]; const Result& r = results[ci];
        const double wr = r.n>0 ? 100.0*r.w/r.n : 0.0;
        std::printf("%4d %4d %5d %8.4f %8.4f %4d %7d %12.2f %8.2f\n",
                    rk+1, ci, r.cfg.LOOKBACK, r.cfg.MIN_RANGE, r.cfg.MAX_RANGE, r.cfg.MIN_BREAK,
                    r.n, r.net, wr);
    }
    std::printf("[entry] wrote %s\n", out.c_str());
    return 0;
}
