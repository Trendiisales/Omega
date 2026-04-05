// research_backtest.cpp — standalone GoldFlow signal research backtest
// Compile: g++ -O3 -std=c++17 research_backtest.cpp -o research_backtest
// Run:     ./research_backtest ~/tick/xauusd_merged_24m.csv
//
// Tests multiple TP/SL combinations in one pass over the tick data.
// Signal: EWM drift + 30-tick persistence (same as live GoldFlowEngine).
// L2 proxy: bid direction over last 10 ticks (approximates order flow).
// No engine header dependencies. No patches. Clean slate.

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <cmath>
#include <algorithm>
#include <ctime>
#include <cstring>
#include <cstdio>

// ── Constants ────────────────────────────────────────────────────────────────
static constexpr double ALPHA_FAST      = 0.05;
static constexpr double ALPHA_SLOW      = 0.005;
static constexpr int    PERSIST_WINDOW  = 30;
static constexpr double PERSIST_THRESH  = 0.70;
static constexpr int    ATR_WINDOW      = 100;
static constexpr double ATR_MIN         = 2.0;
static constexpr double DRIFT_MULT      = 0.20;   // eff_thresh = DRIFT_MULT * ATR
static constexpr double SPREAD_MAX      = 2.0;    // reject wide-spread ticks
static constexpr int    COOLDOWN_TICKS  = 300;    // ~30s at 10 t/s
static constexpr double RISK_DOLLARS    = 80.0;
static constexpr double TICK_MULT       = 100.0;  // $100/pt/lot XAUUSD
static constexpr double LOT_STEP        = 0.001;
static constexpr double LOT_MIN         = 0.01;
static constexpr double LOT_MAX         = 0.50;
static constexpr double SLIPPAGE_PER_SIDE = 0.30; // pts per side (conservative)
static constexpr long long WARMUP_TICKS = 10000;

// Session filter: skip dead zone 21:00-01:00 UTC (gold low liquidity)
static bool in_session(long long ts_ms) {
    time_t t = (time_t)(ts_ms / 1000);
    struct tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &t);
#else
    gmtime_r(&t, &utc);
#endif
    int h = utc.tm_hour;
    return !(h >= 21 || h < 1);  // exclude 21:00-00:59 UTC
}

// Tighter session: London open (07-10) + NY open (13-17) only
static bool in_prime_session(long long ts_ms) {
    time_t t = (time_t)(ts_ms / 1000);
    struct tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &t);
#else
    gmtime_r(&t, &utc);
#endif
    int h = utc.tm_hour;
    return (h >= 7 && h < 10) || (h >= 13 && h < 17);
}

// ── Tick ─────────────────────────────────────────────────────────────────────
struct Tick {
    long long ts_ms = 0;
    double    bid   = 0;
    double    ask   = 0;
    double    mid() const { return (bid + ask) * 0.5; }
    double    spread() const { return ask - bid; }
};

static bool parse_tick(const std::string& line, Tick& t, bool ask_first) {
    // Format: timestamp_ms, price1, price2
    const char* p = line.c_str();
    char* end;
    t.ts_ms = std::strtoll(p, &end, 10); if (*end != ',') return false; p = end+1;
    double p1 = std::strtod(p, &end);    if (*end != ',') return false; p = end+1;
    double p2 = std::strtod(p, &end);
    if (t.ts_ms <= 0 || p1 <= 0 || p2 <= 0) return false;
    if (ask_first) { t.ask = p1; t.bid = p2; }
    else           { t.bid = p1; t.ask = p2; }
    // Sanity: gold should be $1000-$6000
    if (t.bid < 1000 || t.ask > 6000 || t.ask < t.bid) return false;
    return true;
}

static bool detect_ask_first(const std::string& line) {
    // Peek at first data line: if col2 > col3, it's ask-first
    const char* p = line.c_str();
    char* end;
    std::strtoll(p, &end, 10); if (*end != ',') return false; p = end+1;
    double p1 = std::strtod(p, &end); if (*end != ',') return false; p = end+1;
    double p2 = std::strtod(p, &end);
    return p1 > p2;  // ask > bid → ask is first
}

// ── Config (one TP/SL combination to test) ───────────────────────────────────
struct Config {
    double tp_pts;
    double sl_pts;
    int    time_limit_s;  // 0 = no limit
    const char* name;
};

// ── Position ─────────────────────────────────────────────────────────────────
struct Position {
    bool      open      = false;
    int       dir       = 0;    // +1 LONG, -1 SHORT
    double    entry_px  = 0;
    double    size      = 0;
    double    tp_px     = 0;
    double    sl_px     = 0;
    long long entry_ms  = 0;
    double    mfe_pts   = 0;
    double    mae_pts   = 0;
};

// ── Trade record ─────────────────────────────────────────────────────────────
struct Trade {
    long long entry_ms, exit_ms;
    int       dir;
    double    entry_px, exit_px, size;
    double    gross_pnl, net_pnl;
    double    mfe_pts, mae_pts;
    double    spread_at_entry;
    int       hold_s;
    const char* exit_reason;
    const char* config_name;
};

// ── Per-config state ──────────────────────────────────────────────────────────
struct State {
    const Config& cfg;

    // Signal state
    double ewm_fast = 0, ewm_slow = 0;
    bool   ewm_init = false;
    std::deque<double> price_buf;  // for persistence
    std::deque<double> atr_buf;    // for ATR
    std::deque<double> bid_buf;    // for L2 proxy (last 10 bids)

    // Position
    Position pos;
    int      cooldown = 0;

    // Results
    std::vector<Trade> trades;

    // Dedup: prevent same-millisecond double-fire
    long long last_entry_ms = 0;

    explicit State(const Config& c) : cfg(c) {}

    double atr() const {
        if ((int)atr_buf.size() < ATR_WINDOW) return ATR_MIN;
        double hi = *std::max_element(atr_buf.begin(), atr_buf.end());
        double lo = *std::min_element(atr_buf.begin(), atr_buf.end());
        return std::max(ATR_MIN, hi - lo);
    }

    // L2 proxy: fraction of last N bid ticks that were upticks
    double l2_proxy() const {
        if ((int)bid_buf.size() < 5) return 0.5;
        int ups = 0, dns = 0;
        for (int i = 1; i < (int)bid_buf.size(); ++i) {
            if (bid_buf[i] > bid_buf[i-1]) ++ups;
            else if (bid_buf[i] < bid_buf[i-1]) ++dns;
        }
        int n = ups + dns;
        if (n == 0) return 0.5;
        return (double)ups / n;
    }

    void on_tick(const Tick& t, long long tick_n) {
        double mid = t.mid();
        double spd = t.spread();

        // Update EWM
        if (!ewm_init) {
            ewm_fast = ewm_slow = mid;
            ewm_init = true;
        }
        ewm_fast = ALPHA_FAST * mid + (1.0 - ALPHA_FAST) * ewm_fast;
        ewm_slow = ALPHA_SLOW * mid + (1.0 - ALPHA_SLOW) * ewm_slow;
        double drift = ewm_fast - ewm_slow;

        // Update buffers
        price_buf.push_back(mid);
        if ((int)price_buf.size() > PERSIST_WINDOW) price_buf.pop_front();

        atr_buf.push_back(mid);
        if ((int)atr_buf.size() > ATR_WINDOW) atr_buf.pop_front();

        bid_buf.push_back(t.bid);
        if ((int)bid_buf.size() > 10) bid_buf.pop_front();

        if (cooldown > 0) { --cooldown; }

        double cur_atr = atr();
        double eff_thresh = std::max(0.5, DRIFT_MULT * cur_atr);

        // ── Manage open position ─────────────────────────────────────────
        if (pos.open) {
            double move = (pos.dir == 1)
                ? (t.bid - pos.entry_px)   // LONG: exit on bid
                : (pos.entry_px - t.ask);  // SHORT: exit on ask

            pos.mfe_pts = std::max(pos.mfe_pts, move);
            pos.mae_pts = std::min(pos.mae_pts, move);

            int held_s = (int)((t.ts_ms - pos.entry_ms) / 1000);

            bool hit_tp   = move >= cfg.tp_pts;
            bool hit_sl   = move <= -cfg.sl_pts;
            bool hit_time = cfg.time_limit_s > 0 && held_s >= cfg.time_limit_s;

            const char* reason = nullptr;
            double exit_px = 0;

            if (hit_tp) {
                reason  = "TP_HIT";
                exit_px = pos.entry_px + cfg.tp_pts * pos.dir;
            } else if (hit_sl) {
                reason  = "SL_HIT";
                exit_px = pos.entry_px - cfg.sl_pts * pos.dir;
            } else if (hit_time) {
                reason  = "TIME_EXIT";
                exit_px = (pos.dir == 1) ? t.bid : t.ask;
            }

            if (reason) {
                double gross = move * pos.size * TICK_MULT;
                // For TIME_EXIT use actual move, for TP/SL use exact pts
                if (hit_tp)      gross =  cfg.tp_pts * pos.size * TICK_MULT;
                else if (hit_sl) gross = -cfg.sl_pts * pos.size * TICK_MULT;

                double slip_cost = 2.0 * SLIPPAGE_PER_SIDE * pos.size * TICK_MULT;
                double net = gross - slip_cost;

                Trade tr;
                tr.entry_ms  = pos.entry_ms;
                tr.exit_ms   = t.ts_ms;
                tr.dir       = pos.dir;
                tr.entry_px  = pos.entry_px;
                tr.exit_px   = exit_px;
                tr.size      = pos.size;
                tr.gross_pnl = gross;
                tr.net_pnl   = net;
                tr.mfe_pts   = pos.mfe_pts;
                tr.mae_pts   = pos.mae_pts;
                tr.spread_at_entry = spd;
                tr.hold_s    = held_s;
                tr.exit_reason   = reason;
                tr.config_name   = cfg.name;

                trades.push_back(tr);
                pos.open  = false;
                cooldown  = COOLDOWN_TICKS;
            }
            return;
        }

        // ── Entry signal ─────────────────────────────────────────────────
        if (tick_n < WARMUP_TICKS) return;
        if (cooldown > 0) return;
        if (spd > SPREAD_MAX) return;
        // sess_ configs use prime session only (London+NY open)
        bool prime = (cfg.name[0]=='s' && cfg.name[1]=='e' && cfg.name[2]=='s' && cfg.name[3]=='s');
        if (prime ? !in_prime_session(t.ts_ms) : !in_session(t.ts_ms)) return;
        if ((int)price_buf.size() < PERSIST_WINDOW) return;

        // Persistence: count directional ticks
        int ups = 0, dns = 0;
        for (int i = 1; i < (int)price_buf.size(); ++i) {
            if (price_buf[i] > price_buf[i-1]) ++ups;
            else if (price_buf[i] < price_buf[i-1]) ++dns;
        }
        int n_moves = PERSIST_WINDOW - 1;

        // L2 proxy
        double imb = l2_proxy();

        // TREND + ATR EXPANSION: only trade when market is genuinely moving
        // cfg.name encodes ATR threshold: atr6=6pt, atr8=8pt, atr10=10pt etc
        double atr_threshold = 6.0;
        const char* n = cfg.name;
        if      (strstr(n,"atr8"))  atr_threshold = 8.0;
        else if (strstr(n,"atr10")) atr_threshold = 10.0;
        else if (strstr(n,"atr12")) atr_threshold = 12.0;
        else if (strstr(n,"atr15")) atr_threshold = 15.0;
        else if (strstr(n,"atr6"))  atr_threshold = 6.0;

        if (cur_atr < atr_threshold) return;  // only trade in expansion

        bool long_sig  = (ups  >= (int)(PERSIST_THRESH * n_moves))
                      && (drift > eff_thresh)
                      && (imb > 0.55);

        bool short_sig = (dns  >= (int)(PERSIST_THRESH * n_moves))
                      && (drift < -eff_thresh)
                      && (imb < 0.45);

        if (!long_sig && !short_sig) return;
        if (t.ts_ms == last_entry_ms) return;  // dedup same-ms

        int dir = long_sig ? 1 : -1;
        double entry_px = (dir == 1) ? t.ask : t.bid;
        double size = std::floor(RISK_DOLLARS / (cfg.sl_pts * TICK_MULT) / LOT_STEP) * LOT_STEP;
        size = std::max(LOT_MIN, std::min(LOT_MAX, size));

        pos.open     = true;
        pos.dir      = dir;
        pos.entry_px = entry_px;
        pos.size     = size;
        pos.tp_px    = entry_px + cfg.tp_pts * dir;
        pos.sl_px    = entry_px - cfg.sl_pts * dir;
        pos.entry_ms = t.ts_ms;
        pos.mfe_pts  = 0;
        pos.mae_pts  = 0;
        last_entry_ms = t.ts_ms;
    }
};

// ── Stats printer ─────────────────────────────────────────────────────────────
static void print_stats(const State& s) {
    const auto& trades = s.trades;
    if (trades.empty()) {
        printf("  %-30s  0 trades\n", s.cfg.name);
        return;
    }

    int n = (int)trades.size();
    int wins = 0;
    double total_net = 0, total_gross = 0;
    double peak = 0, dd = 0, cur = 0;

    for (const auto& t : trades) {
        if (t.net_pnl > 0) ++wins;
        total_net   += t.net_pnl;
        total_gross += t.gross_pnl;
        cur += t.net_pnl;
        if (cur > peak) peak = cur;
        double d = peak - cur;
        if (d > dd) dd = d;
    }

    double wr  = 100.0 * wins / n;
    double avg = total_net / n;

    // Avg win / avg loss
    double sum_win = 0, sum_loss = 0;
    int n_win = 0, n_loss = 0;
    for (const auto& t : trades) {
        if (t.net_pnl > 0) { sum_win  += t.net_pnl; ++n_win;  }
        else               { sum_loss += t.net_pnl; ++n_loss; }
    }
    double avg_win  = n_win  ? sum_win  / n_win  : 0;
    double avg_loss = n_loss ? sum_loss / n_loss : 0;
    double payoff   = n_loss && avg_loss != 0 ? std::fabs(avg_win / avg_loss) : 0;

    printf("  %-30s  n=%5d  WR=%5.1f%%  net=$%8.0f  avg=$%6.2f  "
           "win=$%6.2f  loss=$%7.2f  poff=%.3f  DD=$%7.0f\n",
           s.cfg.name, n, wr, total_net, avg,
           avg_win, avg_loss, payoff, dd);
}

// ── Write trades CSV ──────────────────────────────────────────────────────────
static void write_csv(const State& s, const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) { fprintf(stderr, "Cannot write %s\n", path); return; }
    fprintf(f, "entry_ms,exit_ms,dir,entry_px,exit_px,size,gross_pnl,net_pnl,"
               "mfe_pts,mae_pts,hold_s,spread_at_entry,exit_reason,config\n");
    for (const auto& t : s.trades) {
        fprintf(f, "%lld,%lld,%d,%.3f,%.3f,%.4f,%.4f,%.4f,"
                   "%.4f,%.4f,%d,%.4f,%s,%s\n",
                (long long)t.entry_ms, (long long)t.exit_ms,
                t.dir, t.entry_px, t.exit_px, t.size,
                t.gross_pnl, t.net_pnl,
                t.mfe_pts, t.mae_pts,
                t.hold_s, t.spread_at_entry,
                t.exit_reason, t.config_name);
    }
    fclose(f);
    printf("  Wrote %zu trades → %s\n", s.trades.size(), path);
}

// ── Main ──────────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s ticks.csv [output_dir]\n", argv[0]);
        return 1;
    }

    const char* csv_path = argv[1];
    const char* out_dir  = argc >= 3 ? argv[2] : ".";

    // ── Configs to test ──────────────────────────────────────────────────
    static const Config CONFIGS[] = {
        // ATR expansion filter — only trade when market is genuinely moving
        // tp8/sl6 had best payoff (1.121) — test with ATR thresholds
        { 8.0,  6.0, 0,  "tp8_sl6_atr6"   },   // baseline (ATR>6, current)
        { 8.0,  6.0, 0,  "tp8_sl6_atr8"   },   // mild expansion
        { 8.0,  6.0, 0,  "tp8_sl6_atr10"  },   // real momentum
        { 8.0,  6.0, 0,  "tp8_sl6_atr12"  },   // strong expansion
        { 8.0,  6.0, 0,  "tp8_sl6_atr15"  },   // extreme (crash days only)
        // Also test tp10 with ATR filter
        { 10.0, 6.0, 0,  "tp10_sl6_atr8"  },
        { 10.0, 6.0, 0,  "tp10_sl6_atr10" },
        { 10.0, 6.0, 0,  "tp10_sl6_atr12" },
        // tp6 (higher WR) with ATR filter
        { 6.0,  6.0, 0,  "tp6_sl6_atr8"   },
        { 6.0,  6.0, 0,  "tp6_sl6_atr10"  },
        { 6.0,  6.0, 0,  "tp6_sl6_atr12"  },
    };

    static constexpr int N_CONFIGS = (int)(sizeof(CONFIGS)/sizeof(CONFIGS[0]));

    // Build states
    std::vector<State> states;
    states.reserve(N_CONFIGS);
    for (int i = 0; i < N_CONFIGS; ++i)
        states.emplace_back(CONFIGS[i]);

    // ── Open file and detect format ──────────────────────────────────────
    std::ifstream f(csv_path);
    if (!f.is_open()) {
        fprintf(stderr, "Cannot open %s\n", csv_path);
        return 1;
    }

    printf("[BACKTEST] Reading %s\n", csv_path);

    std::string line;
    bool ask_first = false;
    bool format_detected = false;

    // Skip header if present
    std::getline(f, line);
    if (line.find("timestamp") != std::string::npos ||
        line.find("bid") != std::string::npos) {
        // It's a header — detect format from next line
        std::streampos pos = f.tellg();
        std::string next;
        if (std::getline(f, next)) {
            ask_first = detect_ask_first(next);
            format_detected = true;
            // Rewind to just after header
            f.seekg(pos);
        }
    } else {
        // No header — use first line for detection
        ask_first = detect_ask_first(line);
        format_detected = true;
        // Process this line too
        Tick t;
        if (parse_tick(line, t, ask_first)) {
            for (auto& s : states) s.on_tick(t, 0);
        }
    }

    if (!format_detected) {
        fprintf(stderr, "Cannot detect CSV format\n");
        return 1;
    }

    printf("[BACKTEST] Format: %s\n", ask_first ? "ask,bid" : "bid,ask");

    // ── Main tick loop ───────────────────────────────────────────────────
    long long N = 0;
    long long skipped = 0;
    long long last_report = 0;
    Tick t;

    // Dedup: skip duplicate timestamps
    std::unordered_set<long long> seen_ms;  // only keep recent window
    long long prev_ts = 0;

    while (std::getline(f, line)) {
        if (line.empty()) continue;
        if (!parse_tick(line, t, ask_first)) { ++skipped; continue; }

        // Skip if timestamp went backwards (file ordering issue)
        if (t.ts_ms < prev_ts - 1000) { ++skipped; continue; }
        prev_ts = t.ts_ms;

        ++N;

        for (auto& s : states)
            s.on_tick(t, N);

        if (N - last_report >= 10'000'000) {
            printf("  [ticks: %lld M]\r", N / 1'000'000);
            fflush(stdout);
            last_report = N;
        }
    }

    printf("\n[BACKTEST] %lld ticks processed, %lld skipped\n", N, skipped);
    printf("\n=== RESULTS ===\n\n");
    printf("  %-30s  %5s  %6s  %9s  %7s  %6s  %7s  %5s  %8s\n",
           "Config", "n", "WR%", "net$", "avg$", "win$", "loss$", "poff", "DD$");
    printf("  %s\n", std::string(110,'-').c_str());

    for (const auto& s : states)
        print_stats(s);

    printf("\n=== WRITING CSVs ===\n");

    for (const auto& s : states) {
        if (s.trades.empty()) continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/bt_%s.csv", out_dir, s.cfg.name);
        write_csv(s, path);
    }

    printf("\n[DONE]\n");
    return 0;
}
