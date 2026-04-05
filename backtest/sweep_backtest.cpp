// sweep_backtest.cpp — liquidity sweep signal research backtest
// Compile: g++ -O3 -std=c++17 sweep_backtest.cpp -o sweep_backtest
// Run:     ./sweep_backtest ~/tick/xauusd_clean_24m.csv ~/tick/sweep_out
//
// Signal: liquidity sweep (stop hunt reversal)
// 1. Price breaks BELOW recent swing low by sweep_depth pts (sell stops triggered)
// 2. Price reverses back ABOVE swing low within confirm_ticks
// 3. Enter LONG — institutions absorbed the selling, snap-back incoming
// Mirror for SHORT (sweep of swing high)
//
// This is the ICT Silver Bullet / liquidity raid pattern
// Documented 60-70% WR on gold at specific time windows

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <deque>
#include <string>
#include <cmath>
#include <algorithm>
#include <ctime>
#include <cstring>
#include <cstdio>
#include <climits>

// ── Constants ─────────────────────────────────────────────────────────────
static constexpr double PRICE_MIN     = 1000.0;
static constexpr double PRICE_MAX     = 6000.0;
static constexpr double SPREAD_MAX    = 2.0;
static constexpr double RISK_DOLLARS  = 80.0;
static constexpr double TICK_MULT     = 100.0;
static constexpr double LOT_STEP      = 0.001;
static constexpr double LOT_MIN       = 0.01;
static constexpr double LOT_MAX       = 0.50;
static constexpr double SLIP_PER_SIDE = 0.20;  // pts
static constexpr long long WARMUP     = 20000; // ticks

// ── Session filter ─────────────────────────────────────────────────────────
static int utc_hour(long long ts_ms) {
    time_t t = (time_t)(ts_ms / 1000);
    struct tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &t);
#else
    gmtime_r(&t, &utc);
#endif
    return utc.tm_hour;
}

// London open: 07-10 UTC, NY open: 13-17 UTC
static bool prime_session(long long ts_ms) {
    int h = utc_hour(ts_ms);
    return (h >= 7 && h < 10) || (h >= 13 && h < 17);
}

// All session (01-21 UTC, exclude dead zone)
static bool any_session(long long ts_ms) {
    int h = utc_hour(ts_ms);
    return !(h >= 21 || h < 1);
}

// ── Tick ──────────────────────────────────────────────────────────────────
struct Tick {
    long long ts_ms = 0;
    double    bid   = 0;
    double    ask   = 0;
    double    mid() const { return (bid + ask) * 0.5; }
    double    spread() const { return ask - bid; }
};

static bool parse_tick(const std::string& line, Tick& t, bool ask_first) {
    const char* p = line.c_str();
    char* end;
    t.ts_ms = std::strtoll(p, &end, 10); if (*end != ',') return false; p = end+1;
    double p1 = std::strtod(p, &end);    if (*end != ',') return false; p = end+1;
    double p2 = std::strtod(p, &end);
    if (t.ts_ms <= 0 || p1 <= 0 || p2 <= 0) return false;
    if (ask_first) { t.ask = p1; t.bid = p2; }
    else           { t.bid = p1; t.ask = p2; }
    if (t.bid < PRICE_MIN || t.ask > PRICE_MAX || t.ask < t.bid) return false;
    return true;
}

static bool detect_ask_first(const std::string& line) {
    const char* p = line.c_str();
    char* end;
    std::strtoll(p, &end, 10); if (*end != ',') return false; p = end+1;
    double p1 = std::strtod(p, &end); if (*end != ',') return false; p = end+1;
    double p2 = std::strtod(p, &end);
    return p1 > p2;
}

// ── Config ────────────────────────────────────────────────────────────────
struct Config {
    int    swing_window;    // ticks to look back for swing hi/lo
    double sweep_depth;     // pts price must pierce beyond swing
    int    confirm_ticks;   // ticks within which reversal must happen
    double tp_pts;          // take profit pts from entry
    double sl_pts;          // stop loss pts from entry (tight — just below sweep)
    int    time_limit_s;    // max hold seconds (0=none)
    bool   prime_only;      // only trade London+NY open sessions
    const char* name;
};

// ── Trade ─────────────────────────────────────────────────────────────────
struct Trade {
    long long entry_ms, exit_ms;
    int       dir;          // +1 LONG, -1 SHORT
    double    entry_px, exit_px, size;
    double    gross_pnl, net_pnl;
    double    mfe_pts, mae_pts;
    int       hold_s;
    double    sweep_low;    // the sweep level that triggered entry
    const char* exit_reason;
    const char* config_name;
};

// ── Per-config state ───────────────────────────────────────────────────────
struct State {
    const Config& cfg;

    // Swing tracking
    std::deque<double> swing_buf;  // rolling window of mid prices

    // L2 proxy (bid direction)
    std::deque<double> bid_buf;

    // Sweep detection state
    bool   watching_long  = false;  // waiting for reversal after sweep low
    bool   watching_short = false;  // waiting for reversal after sweep high
    double sweep_low_px   = 0;      // price of the sweep (long setup)
    double sweep_high_px  = 0;      // price of the sweep (short setup)
    int    ticks_watching_long  = 0;
    int    ticks_watching_short = 0;

    // Position
    bool      in_pos    = false;
    int       dir       = 0;
    double    entry_px  = 0;
    double    size      = 0;
    long long entry_ms  = 0;
    double    mfe_pts   = 0;
    double    mae_pts   = 0;
    double    triggered_sweep = 0;
    int       cooldown  = 0;

    std::vector<Trade> trades;

    explicit State(const Config& c) : cfg(c) {}

    double swing_low() const {
        if (swing_buf.empty()) return 0;
        return *std::min_element(swing_buf.begin(), swing_buf.end());
    }
    double swing_high() const {
        if (swing_buf.empty()) return 0;
        return *std::max_element(swing_buf.begin(), swing_buf.end());
    }
    double l2_proxy() const {
        if ((int)bid_buf.size() < 5) return 0.5;
        int ups = 0, dns = 0;
        for (int i = 1; i < (int)bid_buf.size(); ++i) {
            if (bid_buf[i] > bid_buf[i-1]) ++ups;
            else if (bid_buf[i] < bid_buf[i-1]) ++dns;
        }
        int n = ups + dns;
        return n > 0 ? (double)ups / n : 0.5;
    }

    void close(const Tick& t, const char* reason) {
        double exit_px = (dir == 1) ? t.bid : t.ask;
        double move    = (dir == 1) ? (exit_px - entry_px) : (entry_px - exit_px);

        // Clamp to TP/SL exact prices for TP_HIT/SL_HIT
        double gross = move * size * TICK_MULT;
        if (strcmp(reason, "TP_HIT") == 0) gross =  cfg.tp_pts * size * TICK_MULT;
        if (strcmp(reason, "SL_HIT") == 0) gross = -cfg.sl_pts * size * TICK_MULT;

        double slip = 2.0 * SLIP_PER_SIDE * size * TICK_MULT;
        double net  = gross - slip;

        trades.push_back({
            entry_ms, t.ts_ms, dir,
            entry_px, exit_px, size,
            gross, net,
            mfe_pts, mae_pts,
            (int)((t.ts_ms - entry_ms) / 1000),
            triggered_sweep,
            reason, cfg.name
        });

        in_pos   = false;
        cooldown = 500;  // ~50s cooldown after each trade
    }

    void on_tick(const Tick& t, long long tick_n) {
        double mid = t.mid();
        double spd = t.spread();

        // Update buffers
        swing_buf.push_back(mid);
        if ((int)swing_buf.size() > cfg.swing_window) swing_buf.pop_front();

        bid_buf.push_back(t.bid);
        if ((int)bid_buf.size() > 15) bid_buf.pop_front();

        if (cooldown > 0) --cooldown;

        // Need full swing window
        if ((int)swing_buf.size() < cfg.swing_window) return;
        if (tick_n < WARMUP) return;

        // ── Manage open position ─────────────────────────────────────
        if (in_pos) {
            double move = (dir == 1)
                ? (t.bid - entry_px)
                : (entry_px - t.ask);

            mfe_pts = std::max(mfe_pts, move);
            mae_pts = std::min(mae_pts, move);

            int held_s = (int)((t.ts_ms - entry_ms) / 1000);
            bool hit_tp   = move >= cfg.tp_pts;
            bool hit_sl   = move <= -cfg.sl_pts;
            bool hit_time = cfg.time_limit_s > 0 && held_s >= cfg.time_limit_s;

            if      (hit_tp)   close(t, "TP_HIT");
            else if (hit_sl)   close(t, "SL_HIT");
            else if (hit_time) close(t, "TIME_EXIT");
            return;
        }

        if (cooldown > 0 || spd > SPREAD_MAX) return;
        if (cfg.prime_only ? !prime_session(t.ts_ms) : !any_session(t.ts_ms)) return;

        double sw_lo = swing_low();
        double sw_hi = swing_high();

        // ── Sweep detection: LONG setup ─────────────────────────────
        // Step 1: detect sweep below swing low
        if (!watching_long && !watching_short) {
            if (t.bid < sw_lo - cfg.sweep_depth) {
                watching_long = true;
                sweep_low_px  = t.bid;
                ticks_watching_long = 0;
            }
            else if (t.ask > sw_hi + cfg.sweep_depth) {
                watching_short = true;
                sweep_high_px  = t.ask;
                ticks_watching_short = 0;
            }
        }

        // Step 2: watch for reversal confirmation (LONG)
        if (watching_long) {
            ++ticks_watching_long;
            // Update sweep low if price goes lower
            if (t.bid < sweep_low_px) sweep_low_px = t.bid;

            // Reversal: price recovers back above original swing low
            if (t.ask > sw_lo) {
                double imb = l2_proxy();
                // L2 proxy should confirm: buyers stepping in (imb > 0.5)
                if (imb >= 0.45) {  // relaxed — sweep reversal is the main signal
                    // Enter LONG
                    in_pos          = true;
                    dir             = 1;
                    entry_px        = t.ask;
                    size = std::max(LOT_MIN, std::min(LOT_MAX,
                        std::floor(RISK_DOLLARS / (cfg.sl_pts * TICK_MULT) / LOT_STEP) * LOT_STEP));
                    entry_ms        = t.ts_ms;
                    mfe_pts         = 0;
                    mae_pts         = 0;
                    triggered_sweep = sweep_low_px;
                }
                watching_long = false;
            }
            else if (ticks_watching_long > cfg.confirm_ticks) {
                // Reversal didn't happen in time — cancel
                watching_long = false;
            }
        }

        // Step 2: watch for reversal confirmation (SHORT)
        if (watching_short) {
            ++ticks_watching_short;
            if (t.ask > sweep_high_px) sweep_high_px = t.ask;

            // Reversal: price falls back below original swing high
            if (t.bid < sw_hi) {
                double imb = l2_proxy();
                if (imb <= 0.55) {
                    // Enter SHORT
                    in_pos          = true;
                    dir             = -1;
                    entry_px        = t.bid;
                    size = std::max(LOT_MIN, std::min(LOT_MAX,
                        std::floor(RISK_DOLLARS / (cfg.sl_pts * TICK_MULT) / LOT_STEP) * LOT_STEP));
                    entry_ms        = t.ts_ms;
                    mfe_pts         = 0;
                    mae_pts         = 0;
                    triggered_sweep = sweep_high_px;
                }
                watching_short = false;
            }
            else if (ticks_watching_short > cfg.confirm_ticks) {
                watching_short = false;
            }
        }
    }
};

// ── Stats ──────────────────────────────────────────────────────────────────
static void print_stats(const State& s) {
    const auto& trades = s.trades;
    if (trades.empty()) {
        printf("  %-35s  0 trades\n", s.cfg.name);
        return;
    }

    int n = (int)trades.size();
    int wins = 0;
    double total_net = 0, peak = 0, dd = 0, cur = 0;
    double sum_win = 0, sum_loss = 0;
    int n_win = 0, n_loss = 0;

    for (const auto& t : trades) {
        if (t.net_pnl > 0) { ++wins; sum_win  += t.net_pnl; ++n_win;  }
        else               {         sum_loss += t.net_pnl; ++n_loss; }
        total_net += t.net_pnl;
        cur += t.net_pnl;
        if (cur > peak) peak = cur;
        double d = peak - cur;
        if (d > dd) dd = d;
    }

    double wr      = 100.0 * wins / n;
    double avg_win  = n_win  ? sum_win  / n_win  : 0;
    double avg_loss = n_loss ? sum_loss / n_loss : 0;
    double payoff   = (n_loss && avg_loss != 0) ? std::fabs(avg_win / avg_loss) : 0;
    double expect   = total_net / n;

    printf("  %-35s  n=%5d  WR=%5.1f%%  net=$%8.0f  avg=$%6.2f  "
           "win=$%7.2f  loss=$%7.2f  poff=%.3f  DD=$%7.0f\n",
           s.cfg.name, n, wr, total_net, expect,
           avg_win, avg_loss, payoff, dd);
}

static void write_csv(const State& s, const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) { fprintf(stderr, "Cannot write %s\n", path); return; }
    fprintf(f, "entry_ms,exit_ms,dir,entry_px,exit_px,size,gross_pnl,net_pnl,"
               "mfe_pts,mae_pts,hold_s,sweep_px,exit_reason,config\n");
    for (const auto& t : s.trades) {
        fprintf(f, "%lld,%lld,%d,%.3f,%.3f,%.4f,%.4f,%.4f,"
                   "%.4f,%.4f,%d,%.3f,%s,%s\n",
                (long long)t.entry_ms, (long long)t.exit_ms,
                t.dir, t.entry_px, t.exit_px, t.size,
                t.gross_pnl, t.net_pnl,
                t.mfe_pts, t.mae_pts,
                t.hold_s, t.sweep_low,
                t.exit_reason, t.config_name);
    }
    fclose(f);
    printf("  Wrote %zu trades → %s\n", s.trades.size(), path);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s ticks.csv [output_dir]\n", argv[0]);
        return 1;
    }
    const char* csv_path = argv[1];
    const char* out_dir  = argc >= 3 ? argv[2] : ".";

    // ── Configs ────────────────────────────────────────────────────────
    // swing_window, sweep_depth, confirm_ticks, tp_pts, sl_pts, time_limit_s, prime_only, name
    static const Config CONFIGS[] = {
        // vary sweep depth
        { 500, 0.5, 30, 8.0, 3.0, 1800, false, "sw500_d0.5_tp8_sl3_all"   },
        { 500, 1.0, 30, 8.0, 3.0, 1800, false, "sw500_d1.0_tp8_sl3_all"   },
        { 500, 1.5, 30, 8.0, 3.0, 1800, false, "sw500_d1.5_tp8_sl3_all"   },
        { 500, 2.0, 30, 8.0, 3.0, 1800, false, "sw500_d2.0_tp8_sl3_all"   },
        // vary TP/SL
        { 500, 1.0, 30, 6.0, 2.0, 1800, false, "sw500_d1.0_tp6_sl2_all"   },
        { 500, 1.0, 30, 10.0,3.0, 1800, false, "sw500_d1.0_tp10_sl3_all"  },
        { 500, 1.0, 30, 12.0,3.0, 1800, false, "sw500_d1.0_tp12_sl3_all"  },
        // vary swing window
        { 200, 1.0, 30, 8.0, 3.0, 1800, false, "sw200_d1.0_tp8_sl3_all"   },
        {1000, 1.0, 30, 8.0, 3.0, 1800, false, "sw1000_d1.0_tp8_sl3_all"  },
        // prime session only
        { 500, 0.5, 30, 8.0, 3.0, 1800, true,  "sw500_d0.5_tp8_sl3_prime" },
        { 500, 1.0, 30, 8.0, 3.0, 1800, true,  "sw500_d1.0_tp8_sl3_prime" },
        { 500, 1.0, 30, 10.0,3.0, 1800, true,  "sw500_d1.0_tp10_sl3_prime"},
    };
    static constexpr int N = (int)(sizeof(CONFIGS)/sizeof(CONFIGS[0]));

    std::vector<State> states;
    states.reserve(N);
    for (int i = 0; i < N; ++i) states.emplace_back(CONFIGS[i]);

    // ── Open file ──────────────────────────────────────────────────────
    std::ifstream f(csv_path);
    if (!f.is_open()) { fprintf(stderr, "Cannot open %s\n", csv_path); return 1; }

    printf("[SWEEP BACKTEST] %s\n", csv_path);

    std::string line;
    bool ask_first = false;

    std::getline(f, line);
    if (line.find("timestamp") != std::string::npos || line[0] < '0' || line[0] > '9') {
        std::streampos pos = f.tellg();
        std::string next;
        if (std::getline(f, next)) { ask_first = detect_ask_first(next); f.seekg(pos); }
    } else {
        ask_first = detect_ask_first(line);
        Tick t; if (parse_tick(line, t, ask_first)) for (auto& s : states) s.on_tick(t, 0);
    }

    printf("[SWEEP BACKTEST] Format: %s\n", ask_first ? "ask,bid" : "bid,ask");

    long long N_ticks = 0, skipped = 0;
    long long prev_ts = 0;
    Tick t;

    while (std::getline(f, line)) {
        if (line.empty()) continue;
        if (!parse_tick(line, t, ask_first)) { ++skipped; continue; }
        if (t.ts_ms < prev_ts) { ++skipped; continue; }
        prev_ts = t.ts_ms;
        ++N_ticks;
        for (auto& s : states) s.on_tick(t, N_ticks);
        if (N_ticks % 10000000 == 0) {
            printf("  [%lldM ticks]\r", N_ticks/1000000);
            fflush(stdout);
        }
    }

    printf("\n[SWEEP BACKTEST] %lld ticks, %lld skipped\n\n", N_ticks, skipped);

    printf("=== SWEEP SIGNAL RESULTS ===\n\n");
    printf("  %-35s  %5s  %6s  %9s  %7s  %7s  %7s  %5s  %8s\n",
           "Config","n","WR%","net$","avg$","win$","loss$","poff","DD$");
    printf("  %s\n", std::string(120,'-').c_str());

    for (const auto& s : states) print_stats(s);

    printf("\n=== WRITING CSVs ===\n");
    for (const auto& s : states) {
        if (s.trades.empty()) continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/sweep_%s.csv", out_dir, s.cfg.name);
        write_csv(s, path);
    }

    printf("\n[DONE]\n");
    return 0;
}
