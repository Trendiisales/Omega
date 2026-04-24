// =============================================================================
// pdhl_bt_2y.cpp -- PDH/PDL Reversion Engine 2-year XAUUSD tick backtest
//
// Runs the EXACT live engine logic from include/PDHLReversionEngine.hpp
// against /Users/jo/tick/2yr_XAUUSD_tick.csv (111M tick rows, 2 years).
//
// Tick format: YYYYMMDD,HH:MM:SS,bid,ask,last,volume
// No L2 depth available in Dukascopy data → runs in DRIFT-FADE PROXY mode
// (same code path live engine uses when l2_real=false).
//
// Build:
//   clang++ -O3 -std=c++17 -o pdhl_bt_2y pdhl_bt_2y.cpp
// Run:
//   ./pdhl_bt_2y /Users/jo/tick/2yr_XAUUSD_tick.csv [output.txt]
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <ctime>
#include <vector>
#include <string>
#include <deque>
#include <algorithm>
#include <chrono>

// ──────────────────────────────────────────────────────────────────────────
// Tick
// ──────────────────────────────────────────────────────────────────────────
struct Tick {
    int64_t ts_ms;   // UTC ms since epoch
    double  bid;
    double  ask;
    int     year;
    int     month;
    int     day;
    int     utc_hour;
    int     utc_min;
};

// Parse "20230927,00:00:00,1901.455,1901.745,1901.455,0"
// Returns true on success. Epoch ms assumes UTC input (Dukascopy default).
static bool parse_line(const char* s, Tick& t) {
    // date: YYYYMMDD
    if (strlen(s) < 18) return false;
    if (!isdigit(s[0])) return false;
    int year  = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
    int month = (s[4]-'0')*10 + (s[5]-'0');
    int day   = (s[6]-'0')*10 + (s[7]-'0');
    if (s[8] != ',') return false;
    // time: HH:MM:SS
    int H = (s[9]-'0')*10 + (s[10]-'0');
    int M = (s[12]-'0')*10 + (s[13]-'0');
    int S = (s[15]-'0')*10 + (s[16]-'0');
    if (s[17] != ',') return false;

    // Parse bid and ask via strtod
    const char* p = s + 18;
    char* end;
    double bid = strtod(p, &end);
    if (end == p || *end != ',') return false;
    p = end + 1;
    double ask = strtod(p, &end);
    if (end == p) return false;

    // Compute UTC ms via timegm-equivalent (days since epoch)
    // Efficient: days_from_civil algorithm (Howard Hinnant)
    int y = year;
    int m = month;
    if (m <= 2) { y -= 1; }
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    unsigned doe = yoe * 365 + yoe/4 - yoe/100 + doy;
    int64_t days = (int64_t)era * 146097 + (int64_t)doe - 719468;
    int64_t sec  = days * 86400 + H * 3600 + M * 60 + S;

    t.ts_ms    = sec * 1000LL;
    t.bid      = bid;
    t.ask      = ask;
    t.year     = year;
    t.month    = month;
    t.day      = day;
    t.utc_hour = H;
    t.utc_min  = M;
    return true;
}

// Session slot mirror of live engine's macro_ctx.session_slot.
// 0=dead 1=sydney 2=tokyo 3=london 4=ny_overlap 5=ny 6=late
static inline int session_slot_from_utc(int h) {
    if (h >= 22 || h < 1)     return 0;   // late-asia dead zone
    if (h < 7)                return 1;   // sydney / early tokyo
    if (h < 12)               return 2;   // tokyo → london warm
    if (h < 14)               return 3;   // london
    if (h < 17)               return 4;   // london/ny overlap
    if (h < 21)               return 5;   // ny
    return 6;                              // late ny
}

// ──────────────────────────────────────────────────────────────────────────
// Engine (inline port of PDHLReversionEngine — EXACT logic)
// ──────────────────────────────────────────────────────────────────────────
struct PDHLParams {
    double RANGE_ENTRY_PCT = 0.25;
    double SL_ATR_MULT     = 0.40;
    double TP_RANGE_FRAC   = 0.50;
    double DRIFT_FADE_MIN  = 1.5;
    double MIN_RANGE_PTS   = 8.0;
    double MAX_SPREAD_PTS  = 1.5;
    double MIN_ATR_PTS     = 1.0;
    double RISK_USD        = 30.0;
    double MIN_LOT         = 0.01;
    double MAX_LOT         = 0.01;
    int64_t COOLDOWN_MS    = 120000;
    int64_t MAX_HOLD_MS    = 900000;
    int64_t MIN_HOLD_MS    = 20000;
};

struct Trade {
    bool    is_long;
    double  entry;
    double  exit;
    double  sl;
    double  tp;
    double  size;
    int64_t entry_ms;
    int64_t exit_ms;
    double  mfe;
    double  mae;
    double  pnl_usd;
    int     year;
    int     month;
    int     session_slot;
    const char* exit_reason;
    double  range_pts;
    double  atr_at_entry;
};

struct Result {
    int     n          = 0;
    int     n_wins     = 0;
    double  pnl_sum    = 0.0;
    double  max_dd     = 0.0;
    double  peak_eq    = 0.0;
    std::vector<Trade> trades;
};

struct PDHLState {
    bool    active = false;
    bool    is_long = false;
    double  entry = 0, sl = 0, tp = 0, size = 0;
    double  atr = 0, range = 0;
    double  mfe = 0, mae = 0;
    int64_t entry_ms = 0;
    int     entry_year = 0, entry_month = 0, entry_session = 0;
    int64_t m_last_close_ms = 0;
};

static void close_trade(PDHLState& s, double exit_px, int64_t now_ms,
                        const char* reason, Result& R,
                        bool want_trades_detail)
{
    double pnl = (s.is_long ? (exit_px - s.entry) : (s.entry - exit_px)) * s.size * 100.0;
    R.n++;
    if (pnl > 0) R.n_wins++;
    R.pnl_sum += pnl;
    if (R.pnl_sum > R.peak_eq) R.peak_eq = R.pnl_sum;
    double dd = R.peak_eq - R.pnl_sum;
    if (dd > R.max_dd) R.max_dd = dd;

    if (want_trades_detail) {
        Trade t;
        t.is_long = s.is_long;
        t.entry   = s.entry;
        t.exit    = exit_px;
        t.sl      = s.sl;
        t.tp      = s.tp;
        t.size    = s.size;
        t.entry_ms = s.entry_ms;
        t.exit_ms  = now_ms;
        t.mfe     = s.mfe;
        t.mae     = s.mae;
        t.pnl_usd = pnl;
        t.year    = s.entry_year;
        t.month   = s.entry_month;
        t.session_slot = s.entry_session;
        t.exit_reason  = reason;
        t.range_pts    = s.range;
        t.atr_at_entry = s.atr;
        R.trades.push_back(t);
    }

    s.m_last_close_ms = now_ms;
    s.active = false;
}

// One pass of the engine over the loaded tick stream.
// daily_highs/lows contain PDH/PDL for each YYYYMMDD key (computed outside).
// atr_series / drift_series are precomputed per-tick indicators.
static Result run_engine(const std::vector<Tick>& ticks,
                         const std::vector<double>& atr_series,
                         const std::vector<double>& drift_series,
                         const std::vector<double>& pdh_series,
                         const std::vector<double>& pdl_series,
                         const PDHLParams& P,
                         bool want_trades_detail)
{
    Result R;
    PDHLState s;

    const size_t N = ticks.size();
    for (size_t i = 0; i < N; ++i) {
        const Tick& t = ticks[i];
        double bid = t.bid, ask = t.ask;
        if (bid <= 0 || ask <= bid) continue;

        double mid    = (bid + ask) * 0.5;
        double spread = ask - bid;
        double pdh    = pdh_series[i];
        double pdl    = pdl_series[i];
        double atr    = atr_series[i];
        double drift  = drift_series[i];
        int    slot   = session_slot_from_utc(t.utc_hour);

        // manage open position
        if (s.active) {
            double move = s.is_long ? (mid - s.entry) : (s.entry - mid);
            if (move > s.mfe) s.mfe = move;
            if (move < s.mae) s.mae = move;
            int64_t hold_ms = t.ts_ms - s.entry_ms;

            if ((s.is_long && bid <= s.sl) || (!s.is_long && ask >= s.sl)) {
                close_trade(s, s.sl, t.ts_ms, "SL_HIT", R, want_trades_detail);
                continue;
            }
            if ((s.is_long && bid >= s.tp) || (!s.is_long && ask <= s.tp)) {
                close_trade(s, s.is_long ? bid : ask, t.ts_ms, "TP_HIT", R, want_trades_detail);
                continue;
            }
            if (hold_ms >= P.MAX_HOLD_MS) {
                close_trade(s, s.is_long ? bid : ask, t.ts_ms, "MAX_HOLD", R, want_trades_detail);
                continue;
            }
            continue;
        }

        // entry gates
        if (pdh <= 0 || pdl <= 0 || pdh <= pdl) continue;
        double range = pdh - pdl;
        if (range < P.MIN_RANGE_PTS) continue;
        if (mid > pdh + 1.0 || mid < pdl - 1.0) continue;
        if (t.ts_ms - s.m_last_close_ms < P.COOLDOWN_MS) continue;
        if (spread > P.MAX_SPREAD_PTS) continue;
        if (atr < P.MIN_ATR_PTS) continue;
        if (slot == 0) continue;

        double upper_zone = pdh - range * P.RANGE_ENTRY_PCT;
        double lower_zone = pdl + range * P.RANGE_ENTRY_PCT;
        bool in_upper = (mid >= upper_zone);
        bool in_lower = (mid <= lower_zone);
        if (!in_upper && !in_lower) continue;

        // No L2 → drift-fade proxy branch (exact live logic when l2_real=false)
        bool l2_long_ok  = (in_lower && drift < -P.DRIFT_FADE_MIN);
        bool l2_short_ok = (in_upper && drift >  P.DRIFT_FADE_MIN);
        bool enter_long  = in_lower && l2_long_ok;
        bool enter_short = in_upper && l2_short_ok;
        if (!enter_long && !enter_short) continue;

        bool   is_long = enter_long;
        double ep      = is_long ? ask : bid;
        double sl_pts  = std::max(atr * P.SL_ATR_MULT, spread * 2.0);
        double sl_px   = is_long ? (ep - sl_pts) : (ep + sl_pts);
        double mid_rng = (pdh + pdl) * 0.5;
        double tp_px   = is_long
            ? std::min(mid_rng, ep + range * P.TP_RANGE_FRAC)
            : std::max(mid_rng, ep - range * P.TP_RANGE_FRAC);

        double sz = P.RISK_USD / (sl_pts * 100.0);
        sz = std::floor(sz / 0.001) * 0.001;
        sz = std::max(P.MIN_LOT, std::min(P.MAX_LOT, sz));

        s.active = true;
        s.is_long = is_long;
        s.entry = ep;
        s.sl = sl_px;
        s.tp = tp_px;
        s.size = sz;
        s.atr = atr;
        s.range = range;
        s.mfe = 0;
        s.mae = 0;
        s.entry_ms = t.ts_ms;
        s.entry_year = t.year;
        s.entry_month = t.month;
        s.entry_session = slot;
    }
    return R;
}

// ──────────────────────────────────────────────────────────────────────────
// Indicator pre-computation: ATR (EWM-alpha=2/(14+1)) on per-tick TR,
// and EWM drift (alpha=0.05, ~20-tick halflife, same ballpark as live 30s EWM).
// ──────────────────────────────────────────────────────────────────────────
static void compute_indicators(const std::vector<Tick>& ticks,
                               std::vector<double>& atr_out,
                               std::vector<double>& drift_out)
{
    const size_t N = ticks.size();
    atr_out.assign(N, 0.0);
    drift_out.assign(N, 0.0);

    const double atr_alpha   = 2.0 / 15.0;
    const double drift_alpha = 0.05;

    double atr   = 0.0;
    double drift = 0.0;
    double prev_mid = 0.0;
    bool   have_prev = false;

    // Warmup: seed ATR from first 50 ticks with simple avg of |Δmid|
    double seed_sum = 0.0;
    int    seed_n   = 0;
    for (size_t i = 0; i < N && seed_n < 50; ++i) {
        double mid = (ticks[i].bid + ticks[i].ask) * 0.5;
        if (have_prev) { seed_sum += std::fabs(mid - prev_mid); ++seed_n; }
        prev_mid = mid; have_prev = true;
    }
    if (seed_n > 0) atr = (seed_sum / seed_n) * 14.0;
    else            atr = 1.0;
    have_prev = false;
    prev_mid = 0.0;

    for (size_t i = 0; i < N; ++i) {
        double mid = (ticks[i].bid + ticks[i].ask) * 0.5;
        if (have_prev) {
            double delta = mid - prev_mid;
            double tr    = std::fabs(delta);
            atr   = atr * (1.0 - atr_alpha)   + tr    * atr_alpha * 14.0;
            drift = drift * (1.0 - drift_alpha) + delta * drift_alpha * 100.0;
        }
        atr_out[i]   = atr;
        drift_out[i] = drift;
        prev_mid = mid; have_prev = true;
    }
}

// PDH/PDL per tick: for each tick on day D, PDH/PDL = high/low of day D-1.
// We build a (YYYYMMDD → {high, low}) map in a single pass, then assign
// PDH/PDL to each tick by looking up day-1.
static void compute_pdh_pdl(const std::vector<Tick>& ticks,
                            std::vector<double>& pdh_out,
                            std::vector<double>& pdl_out)
{
    const size_t N = ticks.size();
    pdh_out.assign(N, 0.0);
    pdl_out.assign(N, 0.0);

    // First pass: compute (date_key → high, low) where date_key = y*10000+m*100+d
    std::vector<std::pair<int,std::pair<double,double>>> day_hl; // sorted by key
    int cur_key = 0;
    double cur_hi = -1e9, cur_lo = 1e9;
    for (size_t i = 0; i < N; ++i) {
        int key = ticks[i].year * 10000 + ticks[i].month * 100 + ticks[i].day;
        double mid = (ticks[i].bid + ticks[i].ask) * 0.5;
        if (key != cur_key) {
            if (cur_key != 0) day_hl.push_back({cur_key, {cur_hi, cur_lo}});
            cur_key = key; cur_hi = mid; cur_lo = mid;
        } else {
            if (mid > cur_hi) cur_hi = mid;
            if (mid < cur_lo) cur_lo = mid;
        }
    }
    if (cur_key != 0) day_hl.push_back({cur_key, {cur_hi, cur_lo}});

    // Second pass: for each tick assign previous-day high/low.
    // day_hl is naturally sorted by key (chronological).
    // Build quick index: date_key → position in day_hl.
    // Linear scan with a moving "today_idx" pointer.
    size_t today_idx = 0;
    int    today_key = day_hl.empty() ? 0 : day_hl[0].first;
    for (size_t i = 0; i < N; ++i) {
        int key = ticks[i].year * 10000 + ticks[i].month * 100 + ticks[i].day;
        while (today_idx + 1 < day_hl.size() && day_hl[today_idx+1].first <= key) {
            ++today_idx;
            today_key = day_hl[today_idx].first;
        }
        // Previous day = today_idx - 1 (if exists)
        if (today_idx > 0 && day_hl[today_idx].first == key) {
            pdh_out[i] = day_hl[today_idx-1].second.first;
            pdl_out[i] = day_hl[today_idx-1].second.second;
        } else {
            pdh_out[i] = 0.0;
            pdl_out[i] = 0.0;
        }
    }
}

// ──────────────────────────────────────────────────────────────────────────
// CSV loader
// ──────────────────────────────────────────────────────────────────────────
static bool load_ticks(const char* path, std::vector<Tick>& out) {
    FILE* f = fopen(path, "r");
    if (!f) { fprintf(stderr, "[LOAD] Cannot open %s\n", path); return false; }
    out.reserve(115000000);
    char line[256];
    size_t parsed = 0, failed = 0;
    while (fgets(line, sizeof(line), f)) {
        Tick t;
        if (parse_line(line, t)) {
            out.push_back(t);
            ++parsed;
            if (parsed % 20000000 == 0) {
                fprintf(stderr, "[LOAD] %zu ticks\n", parsed); fflush(stderr);
            }
        } else {
            ++failed;
            if (failed < 5) fprintf(stderr, "[LOAD] parse fail: %s", line);
        }
    }
    fclose(f);
    fprintf(stderr, "[LOAD] DONE: %zu ticks, %zu parse failures\n", parsed, failed);
    return !out.empty();
}

// ──────────────────────────────────────────────────────────────────────────
// Reporting helpers
// ──────────────────────────────────────────────────────────────────────────
static void print_result_summary(FILE* out, const char* tag, const Result& R) {
    double wr = R.n > 0 ? 100.0 * R.n_wins / R.n : 0.0;
    fprintf(out, "%-40s  N=%6d  WR=%5.1f%%  PnL=$%+10.2f  "
                 "MaxDD=$%9.2f  PF=%s  Avg=$%+6.2f\n",
            tag, R.n, wr, R.pnl_sum, R.max_dd,
            R.pnl_sum > 0 && R.max_dd > 0 ?
                (R.pnl_sum / R.max_dd > 99 ? "inf" : [](double x){
                    static char b[16]; snprintf(b,sizeof(b),"%.2f",x); return b;
                }(R.pnl_sum / R.max_dd)) : "--",
            R.n > 0 ? R.pnl_sum / R.n : 0.0);
}

static void print_breakdown(FILE* out, const Result& R) {
    if (R.trades.empty()) { fprintf(out, "  (no trades)\n"); return; }

    // By year
    fprintf(out, "\n  -- BY YEAR --\n");
    std::vector<int> years;
    for (auto& t : R.trades) if (std::find(years.begin(), years.end(), t.year)==years.end()) years.push_back(t.year);
    std::sort(years.begin(), years.end());
    for (int y : years) {
        int n=0, nw=0; double pnl=0;
        for (auto& t : R.trades) if (t.year==y) { ++n; if (t.pnl_usd>0) ++nw; pnl += t.pnl_usd; }
        double wr = n ? 100.0*nw/n : 0;
        fprintf(out, "    %d    n=%4d  wr=%5.1f%%  pnl=$%+9.2f  avg=$%+6.2f\n",
                y, n, wr, pnl, n>0?pnl/n:0);
    }

    // By year-month
    fprintf(out, "\n  -- BY MONTH --\n");
    std::vector<int> keys;
    for (auto& t : R.trades) {
        int k = t.year*100 + t.month;
        if (std::find(keys.begin(),keys.end(),k)==keys.end()) keys.push_back(k);
    }
    std::sort(keys.begin(), keys.end());
    double cum=0;
    for (int k : keys) {
        int n=0, nw=0; double pnl=0;
        for (auto& t : R.trades) if (t.year*100+t.month==k) { ++n; if(t.pnl_usd>0)++nw; pnl+=t.pnl_usd; }
        cum += pnl;
        double wr = n ? 100.0*nw/n : 0;
        fprintf(out, "    %d-%02d  n=%3d  wr=%5.1f%%  pnl=$%+8.2f  cum=$%+9.2f\n",
                k/100, k%100, n, wr, pnl, cum);
    }

    // By session slot
    fprintf(out, "\n  -- BY SESSION --\n");
    const char* names[] = {"0_DEAD", "1_SYD", "2_TKY", "3_LON", "4_OVERLAP", "5_NY", "6_LATE"};
    for (int ss = 0; ss < 7; ++ss) {
        int n=0, nw=0; double pnl=0;
        for (auto& t : R.trades) if (t.session_slot==ss) { ++n; if(t.pnl_usd>0)++nw; pnl+=t.pnl_usd; }
        if (n==0) continue;
        double wr = 100.0*nw/n;
        fprintf(out, "    %-11s n=%4d  wr=%5.1f%%  pnl=$%+9.2f  avg=$%+6.2f\n",
                names[ss], n, wr, pnl, pnl/n);
    }

    // By exit reason
    fprintf(out, "\n  -- BY EXIT REASON --\n");
    const char* reasons[] = {"SL_HIT", "TP_HIT", "MAX_HOLD"};
    for (const char* rsn : reasons) {
        int n=0, nw=0; double pnl=0;
        for (auto& t : R.trades) if (strcmp(t.exit_reason, rsn)==0) {
            ++n; if(t.pnl_usd>0)++nw; pnl+=t.pnl_usd;
        }
        if (n==0) continue;
        double wr = 100.0*nw/n;
        fprintf(out, "    %-10s n=%4d  wr=%5.1f%%  pnl=$%+9.2f\n", rsn, n, wr, pnl);
    }

    // By direction
    fprintf(out, "\n  -- BY DIRECTION --\n");
    {
        int ln=0, lw=0; double lp=0;
        int sn=0, sw=0; double sp=0;
        for (auto& t : R.trades) {
            if (t.is_long) { ++ln; if(t.pnl_usd>0)++lw; lp+=t.pnl_usd; }
            else           { ++sn; if(t.pnl_usd>0)++sw; sp+=t.pnl_usd; }
        }
        fprintf(out, "    LONG   n=%4d  wr=%5.1f%%  pnl=$%+9.2f\n",
                ln, ln?100.0*lw/ln:0, lp);
        fprintf(out, "    SHORT  n=%4d  wr=%5.1f%%  pnl=$%+9.2f\n",
                sn, sn?100.0*sw/sn:0, sp);
    }
}

// ──────────────────────────────────────────────────────────────────────────
// Main
// ──────────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    if (argc < 2) {
        puts("Usage: pdhl_bt_2y <tick_csv> [output.txt]");
        return 1;
    }
    FILE* out = stdout;
    if (argc >= 3) {
        out = fopen(argv[2], "w");
        if (!out) { fprintf(stderr, "Cannot open %s for writing\n", argv[2]); return 1; }
    }

    auto t0 = std::chrono::steady_clock::now();

    fprintf(stderr, "\n=== PHASE 1: LOAD ===\n");
    std::vector<Tick> ticks;
    if (!load_ticks(argv[1], ticks)) return 1;
    fprintf(stderr, "Loaded %zu ticks from %s to %s\n",
            ticks.size(),
            [&]{static char b[32]; snprintf(b,sizeof(b),"%04d-%02d-%02d",ticks.front().year,ticks.front().month,ticks.front().day); return b;}(),
            [&]{static char b[32]; snprintf(b,sizeof(b),"%04d-%02d-%02d",ticks.back().year,ticks.back().month,ticks.back().day); return b;}());

    fprintf(stderr, "\n=== PHASE 2: INDICATORS ===\n");
    std::vector<double> atr_series, drift_series;
    compute_indicators(ticks, atr_series, drift_series);
    fprintf(stderr, "ATR + drift computed\n");

    fprintf(stderr, "\n=== PHASE 3: PDH/PDL ===\n");
    std::vector<double> pdh_series, pdl_series;
    compute_pdh_pdl(ticks, pdh_series, pdl_series);
    fprintf(stderr, "PDH/PDL computed (rolling prev-day)\n");

    fprintf(stderr, "\n=== PHASE 4: BASELINE ENGINE RUN ===\n");
    fprintf(out, "========================================================================\n");
    fprintf(out, "PDH/PDL Reversion Engine 2-Year Backtest\n");
    fprintf(out, "File: %s\n", argv[1]);
    fprintf(out, "Ticks: %zu\n", ticks.size());
    fprintf(out, "Mode: drift-fade proxy (no L2 in Dukascopy data — matches live when l2_real=false)\n");
    fprintf(out, "========================================================================\n\n");

    PDHLParams base;
    Result R_base = run_engine(ticks, atr_series, drift_series, pdh_series, pdl_series, base, true);
    fprintf(stderr, "Baseline complete: %d trades, $%+.2f PnL\n", R_base.n, R_base.pnl_sum);

    fprintf(out, "========== BASELINE CONFIG (live-engine defaults) ==========\n");
    fprintf(out, "RANGE_ENTRY_PCT=%.2f  SL_ATR_MULT=%.2f  TP_RANGE_FRAC=%.2f  DRIFT_FADE_MIN=%.2f\n",
            base.RANGE_ENTRY_PCT, base.SL_ATR_MULT, base.TP_RANGE_FRAC, base.DRIFT_FADE_MIN);
    print_result_summary(out, "BASELINE", R_base);
    print_breakdown(out, R_base);
    fprintf(out, "\n");

    fprintf(stderr, "\n=== PHASE 5: PARAMETER GRID SWEEP ===\n");
    // Small grid — focus on the 4 most-impactful params
    double rep_v[]  = {0.20, 0.25, 0.30};        // 3
    double sl_v[]   = {0.30, 0.40, 0.60};        // 3
    double tp_v[]   = {0.40, 0.50, 0.60};        // 3
    double df_v[]   = {1.0, 1.5, 2.0};           // 3
    // = 81 configs
    const int TOTAL = 3*3*3*3;

    struct GridRes { PDHLParams p; Result r; };
    std::vector<GridRes> grid;
    grid.reserve(TOTAL);

    int done = 0;
    for (double rep : rep_v)
    for (double sl  : sl_v)
    for (double tp  : tp_v)
    for (double df  : df_v)
    {
        PDHLParams p = base;
        p.RANGE_ENTRY_PCT = rep;
        p.SL_ATR_MULT     = sl;
        p.TP_RANGE_FRAC   = tp;
        p.DRIFT_FADE_MIN  = df;
        Result r = run_engine(ticks, atr_series, drift_series, pdh_series, pdl_series, p, false);
        grid.push_back({p, r});
        ++done;
        fprintf(stderr, "  [%2d/%d] rep=%.2f sl=%.2f tp=%.2f df=%.1f  N=%d  PnL=$%+.2f\n",
                done, TOTAL, rep, sl, tp, df, r.n, r.pnl_sum);
    }

    // Sort by PnL desc
    std::sort(grid.begin(), grid.end(), [](const GridRes& a, const GridRes& b){
        return a.r.pnl_sum > b.r.pnl_sum;
    });

    fprintf(out, "========== PARAMETER GRID (81 configs, sorted by PnL) ==========\n");
    fprintf(out, "%-4s %-6s %-6s %-6s %-6s %6s %6s %10s %9s %6s\n",
            "#", "rep", "sl", "tp", "df", "N", "WR%", "PnL$", "MaxDD", "Avg$");
    fprintf(out, "%s\n", std::string(80,'-').c_str());
    for (size_t i = 0; i < grid.size(); ++i) {
        auto& g = grid[i];
        double wr = g.r.n ? 100.0 * g.r.n_wins / g.r.n : 0.0;
        fprintf(out, "%-4zu %-6.2f %-6.2f %-6.2f %-6.1f %6d %6.1f %+10.2f %9.2f %+6.2f\n",
                i+1, g.p.RANGE_ENTRY_PCT, g.p.SL_ATR_MULT, g.p.TP_RANGE_FRAC, g.p.DRIFT_FADE_MIN,
                g.r.n, wr, g.r.pnl_sum, g.r.max_dd, g.r.n?g.r.pnl_sum/g.r.n:0);
    }

    // Re-run best config WITH trade detail for full breakdown
    if (!grid.empty()) {
        auto& best = grid[0];
        fprintf(stderr, "\n=== PHASE 6: BEST CONFIG WITH BREAKDOWN ===\n");
        Result R_best = run_engine(ticks, atr_series, drift_series, pdh_series, pdl_series, best.p, true);
        fprintf(out, "\n========== BEST CONFIG BREAKDOWN ==========\n");
        fprintf(out, "RANGE_ENTRY_PCT=%.2f  SL_ATR_MULT=%.2f  TP_RANGE_FRAC=%.2f  DRIFT_FADE_MIN=%.2f\n",
                best.p.RANGE_ENTRY_PCT, best.p.SL_ATR_MULT, best.p.TP_RANGE_FRAC, best.p.DRIFT_FADE_MIN);
        print_result_summary(out, "BEST", R_best);
        print_breakdown(out, R_best);
    }

    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1-t0).count();
    fprintf(stderr, "\nTotal runtime: %.1fs\n", secs);
    fprintf(out, "\nTotal runtime: %.1fs\n", secs);
    if (out != stdout) fclose(out);
    return 0;
}
