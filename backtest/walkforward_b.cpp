// walkforward_b.cpp
//
// Walk-forward Option B harness — ANCHORED retuning simulation.
//
// Three OOS steps:
//   Step 1: IS = W1                  -> OOS = W2
//   Step 2: IS = W1+W2               -> OOS = W3
//   Step 3: IS = W1+W2+W3            -> OOS = W4
//
// At each step, the full 27-cell sweep is run on the in-sample slice.
// The winning cell is picked by:
//   1. profit_factor descending (cells with n_trades < 5 are excluded)
//   2. pnl_usd descending
//   3. donchian ascending
//   4. sl_mult ascending
//   5. tp_mult ascending
// That picked cell is then traded on the next out-of-sample window with
// its own ATR cold-start, and IS+OOS metrics are recorded.
//
// Combined OOS summary aggregates trades from W2_OOS + W3_OOS + W4_OOS
// and recomputes PF / win rate / total PnL on the concatenated stream.
//
// Parser, aggregator, ATR, session gate, and run_one() are byte-for-byte
// identical to walkforward_a2.cpp so IS metrics here are directly
// comparable to A2's per-window grids.
//
// Build (Mac):
//   cd ~/Omega/backtest
//   clang++ -O3 -std=c++17 -o walkforward_b walkforward_b.cpp
//
// Run XAUUSD (old gold windows):
//   ./walkforward_b XAUUSD /Users/jo/tick/2yr_XAUUSD_tick.csv \
//       2023-10-01 2024-04-01 2024-10-01 2025-04-01 2025-09-30
//
// Run NAS100 (Dukascopy windows) — only if you want to confirm A2's
// negative result; B is pointless on a strategy that failed Option A:
//   ./walkforward_b NAS100 ~/Omega/backtest/usatechidxusd-tick-2024-04-25-2026-04-25.csv \
//       2024-04-25 2024-10-25 2025-04-25 2025-10-25 2026-04-25
//
// Outputs:
//   walkforward_b_<SYMBOL>_picks.csv
//   walkforward_b_<SYMBOL>_summary.txt

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

// ============================================================
// Per-symbol contract spec
// ============================================================
struct SymbolSpec {
    std::string name;
    double usd_per_point;
    double lot_size;
    int fri_close_h_utc;
    int sun_open_h_utc;
};

// XAUUSD entry matches the original 27/27 sweep economics from
// htf_bt_minimal.cpp: pnl_pts * 100.0 means usd_per_point=100, lot=1.
// (i.e. 1.0 lot at $100/pt — full size, not 0.10.) This makes the
// gold walk-forward dollar values directly comparable to the
// historical 27/27 result.
// NOTE on `usd_per_point`: the field name is historical. It is really
// "quote-currency-per-point per 1.0 lot". For NAS100, US500, EURUSD,
// XAUUSD the quote currency happens to be USD so the legacy name is
// fine. For GER40 (BlackBull symbol "GER30"), the quote currency is
// EUR — confirmed via cTrader Symbol Info: Quote asset = EUR, Lot
// size = 1.0 Index, so 1.0 lot * 1 point = €1.00. All P&L numbers in
// the GER40 output are therefore EUR not USD; the runtime banner
// flags this. No FX conversion is applied — comparing GER40 dollar
// magnitudes against gold/NAS100 dollar magnitudes requires the
// reader to apply the appropriate EUR/USD rate(s) externally.
static const std::vector<SymbolSpec> kSymbolSpecs = {
    {"NAS100", 1.0,      0.10, 21, 22},
    {"US500",  1.0,      0.10, 21, 22},   // USD/pt at 1.0 lot on Dukascopy usa500idxusd CFD stream
                                          // (raw index level pricing, ~5040 in Apr 2024). 0.10 lots
                                          // matches NAS100 economics. Validated S32 by tick-head:
                                          // 5041.634/5041.114 = raw index, NOT futures multiplier.
    {"GER40",  1.0,      0.10, 21, 22},   // EUR/pt at 1.0 lot, sized at 0.10 lots like NAS100
    {"EURUSD", 100000.0, 0.10, 21, 22},   // USD per unit of raw price diff per 1.0 lot, sized to 0.10
                                          // lots ($10/pip * 0.10 = $1/pip effective). Validated S32.
    {"XAUUSD", 100.0,    1.00, 20, 22},   // gold: Fri close 20:00 UTC, Sun open 22:00
};

const SymbolSpec* find_spec(const std::string& sym) {
    for (const auto& s : kSymbolSpecs) {
        if (s.name == sym) return &s;
    }
    return nullptr;
}

// ============================================================
// Bar struct + Aggregator
// ============================================================
struct Bar {
    int64_t bar_open_ms;
    double open_bid, high_bid, low_bid, close_bid;
    double open_ask, high_ask, low_ask, close_ask;
    int64_t high_bid_ms, low_bid_ms;
    int64_t high_ask_ms, low_ask_ms;
    int64_t tick_count;
};

static const int64_t kBarMs = 14400000LL;

struct Aggregator {
    std::vector<Bar> bars;
    bool active = false;
    Bar cur;
    int64_t cur_bucket_open = 0;

    void start_bar(int64_t bucket_open, int64_t ts, double bid, double ask) {
        cur = Bar{};
        cur.bar_open_ms = bucket_open;
        cur.open_bid = bid; cur.high_bid = bid; cur.low_bid = bid; cur.close_bid = bid;
        cur.open_ask = ask; cur.high_ask = ask; cur.low_ask = ask; cur.close_ask = ask;
        cur.high_bid_ms = ts; cur.low_bid_ms = ts;
        cur.high_ask_ms = ts; cur.low_ask_ms = ts;
        cur.tick_count = 1;
        cur_bucket_open = bucket_open;
        active = true;
    }
    void update(int64_t ts, double bid, double ask) {
        if (bid > cur.high_bid) { cur.high_bid = bid; cur.high_bid_ms = ts; }
        if (bid < cur.low_bid)  { cur.low_bid  = bid; cur.low_bid_ms  = ts; }
        if (ask > cur.high_ask) { cur.high_ask = ask; cur.high_ask_ms = ts; }
        if (ask < cur.low_ask)  { cur.low_ask  = ask; cur.low_ask_ms  = ts; }
        cur.close_bid = bid;
        cur.close_ask = ask;
        cur.tick_count += 1;
    }
    void close_bar() {
        if (active) { bars.push_back(cur); active = false; }
    }
    void on_tick(int64_t ts, double bid, double ask) {
        int64_t bucket = (ts / kBarMs) * kBarMs;
        if (!active) { start_bar(bucket, ts, bid, ask); return; }
        if (bucket != cur_bucket_open) {
            close_bar();
            start_bar(bucket, ts, bid, ask);
            return;
        }
        update(ts, bid, ask);
    }
    void finalize() { close_bar(); }
};

// ============================================================
// Format detection + parsers
// ============================================================
enum Format { FORMAT_UNKNOWN, FORMAT_DUKASCOPY, FORMAT_OLDGOLD };

static Format detect_format(const std::string& header) {
    // Dukascopy header: "timestamp,askPrice,bidPrice"
    if (header.find("timestamp") != std::string::npos &&
        header.find("askPrice")  != std::string::npos &&
        header.find("bidPrice")  != std::string::npos) {
        return FORMAT_DUKASCOPY;
    }
    // Old gold has NO header — first row is data. We detect by
    // counting commas (5) and checking first 8 chars are digits (YYYYMMDD).
    if (header.size() >= 8) {
        bool all_digits = true;
        for (int i = 0; i < 8; ++i) {
            if (header[i] < '0' || header[i] > '9') { all_digits = false; break; }
        }
        int commas = 0;
        for (char c : header) if (c == ',') commas++;
        if (all_digits && commas == 5) return FORMAT_OLDGOLD;
    }
    return FORMAT_UNKNOWN;
}

// Dukascopy: timestamp_ms,askPrice,bidPrice
static bool parse_duka(const char* p, int64_t& ts, double& ask, double& bid) {
    char* end = nullptr;
    ts = std::strtoll(p, &end, 10);
    if (end == p || *end != ',') return false;
    p = end + 1;
    ask = std::strtod(p, &end);
    if (end == p || *end != ',') return false;
    p = end + 1;
    bid = std::strtod(p, &end);
    if (end == p) return false;
    return true;
}

// Convert Y/M/D/H/M/S UTC -> epoch ms (no DST, no leap second handling needed)
static int64_t ymdhms_to_ms(int y, int m, int d, int H, int M, int S) {
    // Use Howard Hinnant's days_from_civil for portability + speed
    y -= (m <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe/4 - yoe/100 + doy;
    int64_t days = (int64_t)era * 146097 + (int64_t)doe - 719468;
    int64_t sec = days * 86400 + H * 3600 + M * 60 + S;
    return sec * 1000LL;
}

// Old gold: YYYYMMDD,HH:MM:SS,bid,ask,last,volume
//          0-7      9-16    after-comma...
// NOTE: bid is field 3, ask is field 4 (different order from Dukascopy)
static bool parse_oldgold(const char* p, int64_t& ts, double& ask, double& bid) {
    if (std::strlen(p) < 19) return false;
    // YYYYMMDD
    int y = (p[0]-'0')*1000 + (p[1]-'0')*100 + (p[2]-'0')*10 + (p[3]-'0');
    int m = (p[4]-'0')*10 + (p[5]-'0');
    int d = (p[6]-'0')*10 + (p[7]-'0');
    if (p[8] != ',') return false;
    // HH:MM:SS at offset 9
    int H = (p[9]-'0')*10 + (p[10]-'0');
    if (p[11] != ':') return false;
    int Mi = (p[12]-'0')*10 + (p[13]-'0');
    if (p[14] != ':') return false;
    int S = (p[15]-'0')*10 + (p[16]-'0');
    if (p[17] != ',') return false;
    p += 18;
    char* end = nullptr;
    bid = std::strtod(p, &end);
    if (end == p || *end != ',') return false;
    p = end + 1;
    ask = std::strtod(p, &end);
    if (end == p) return false;
    // Sub-second precision: old gold ticks are second-resolution. To preserve
    // chronological order within a second across ticks, we add a tiny
    // monotonic increment per same-second tick. Done outside this function.
    ts = ymdhms_to_ms(y, m, d, H, Mi, S);
    return true;
}

static std::vector<Bar> load_and_aggregate(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        std::cerr << "ERROR: cannot open " << path << "\n";
        std::exit(2);
    }
    std::string first_line;
    if (!std::getline(f, first_line)) {
        std::cerr << "ERROR: empty file " << path << "\n";
        std::exit(2);
    }
    Format fmt = detect_format(first_line);
    if (fmt == FORMAT_UNKNOWN) {
        std::cerr << "ERROR: cannot detect tick format from line: "
                  << first_line.substr(0, 80) << "\n";
        std::exit(2);
    }
    std::cerr << "Format: " << (fmt == FORMAT_DUKASCOPY ? "Dukascopy" : "OldGold")
              << "\n";

    Aggregator agg;
    agg.bars.reserve(8000);
    int64_t parsed = 0;
    int64_t bad = 0;
    int64_t last_ts = -1;
    int64_t intra_sec = 0;

    auto process_line = [&](const std::string& line) {
        if (line.empty()) return;
        int64_t ts;
        double ask, bid;
        bool ok = (fmt == FORMAT_DUKASCOPY)
            ? parse_duka(line.c_str(), ts, ask, bid)
            : parse_oldgold(line.c_str(), ts, ask, bid);
        if (!ok) { bad += 1; return; }

        // For old gold (1-second resolution) preserve intra-second order
        // by adding a sub-second increment to ticks sharing a timestamp.
        if (fmt == FORMAT_OLDGOLD) {
            if (ts == last_ts) {
                intra_sec += 1;
                if (intra_sec < 1000) ts += intra_sec;  // ms
            } else {
                last_ts = ts;
                intra_sec = 0;
            }
        }

        agg.on_tick(ts, bid, ask);
        parsed += 1;
        if ((parsed & 0xFFFFFFLL) == 0) {
            std::cerr << "  ... parsed " << parsed << " ticks, "
                      << agg.bars.size() << " bars so far\n";
        }
    };

    // Process the first line: for Dukascopy it's a header, skip; for old gold
    // it's data, parse.
    if (fmt == FORMAT_OLDGOLD) {
        process_line(first_line);
    }
    std::string line;
    while (std::getline(f, line)) process_line(line);
    agg.finalize();
    std::cerr << "Loaded " << parsed << " ticks ("
              << bad << " malformed) -> "
              << agg.bars.size() << " H4 bars\n";
    return agg.bars;
}

// ============================================================
// ATR + weekend gate (identical to walkforward_a2.cpp)
// ============================================================
static std::vector<double> compute_atr(const std::vector<Bar>& bars, int period) {
    std::vector<double> atr(bars.size(), 0.0);
    if (bars.empty()) return atr;
    std::vector<double> tr(bars.size(), 0.0);
    tr[0] = bars[0].high_bid - bars[0].low_bid;
    for (size_t i = 1; i < bars.size(); ++i) {
        double hl = bars[i].high_bid - bars[i].low_bid;
        double hc = std::fabs(bars[i].high_bid - bars[i-1].close_bid);
        double lc = std::fabs(bars[i].low_bid  - bars[i-1].close_bid);
        tr[i] = std::max(hl, std::max(hc, lc));
    }
    if ((int)bars.size() < period) return atr;
    double sum = 0.0;
    for (int i = 0; i < period; ++i) sum += tr[i];
    atr[period - 1] = sum / period;
    for (size_t i = period; i < bars.size(); ++i) {
        atr[i] = (atr[i-1] * (period - 1) + tr[i]) / period;
    }
    return atr;
}

static int day_of_week_utc(int64_t ms) {
    int64_t days = ms / 86400000LL;
    return (int)((days + 4) % 7);
}
static int hour_of_day_utc(int64_t ms) {
    int64_t sec = ms / 1000LL;
    return (int)((sec % 86400LL) / 3600LL);
}
static bool in_trading_window(int64_t bar_open_ms, const SymbolSpec& spec) {
    int dow = day_of_week_utc(bar_open_ms);
    int hod = hour_of_day_utc(bar_open_ms);
    if (dow == 6) return false;
    if (dow == 0) return hod >= spec.sun_open_h_utc;
    if (dow == 5) return hod < spec.fri_close_h_utc;
    return true;
}

// ============================================================
// TradeRecord — per-trade row for OOS path-tracking output.
// Populated only when run_one() is called with a non-null trades_out
// pointer (OOS picked-cell runs). Not populated during the IS sweep.
// ============================================================
struct TradeRecord {
    std::string window_label;     // "W2" / "W3" / "W4"
    int donchian_bars;
    double sl_mult;
    double tp_mult;
    int direction;                // +1 long, -1 short
    int64_t entry_ms;
    int64_t exit_ms;
    double entry_px;
    double exit_px;
    double pnl_usd;
    std::string exit_reason;      // "SL" "TP" "WEEKEND" "EOF"
    int bars_held;
    double atr_at_entry;          // ATR(14) value at the entry bar
    // S36 MAE logging: max adverse excursion observed during bars 1, 2, 3
    // after entry, expressed in ATR-at-entry units.
    //   long:  (entry_price - bar.low_bid) / atr_at_entry, clamped >= 0
    //   short: (bar.high_ask - entry_price) / atr_at_entry, clamped >= 0
    // Sentinel -1.0 means the trade had already closed before that bar;
    // i.e. the bar was not active for this position.
    double mae_bar1;
    double mae_bar2;
    double mae_bar3;
};

// ============================================================
// Result + run_one (identical strategy logic to walkforward_a2.cpp)
// ============================================================
struct Result {
    int donchian_bars;
    double sl_mult;
    double tp_mult;
    int n_trades;
    int n_wins;
    int n_losses;
    double gross_profit;
    double gross_loss;
    double pnl_usd;
    double profit_factor;
    double win_rate;
    double max_dd_usd;
};

static Result run_one(const std::vector<Bar>& bars,
                      const std::vector<double>& atr,
                      int donchian_n,
                      double sl_mult,
                      double tp_mult,
                      const SymbolSpec& spec,
                      std::vector<TradeRecord>* trades_out = nullptr,
                      const std::string& window_label = std::string(),
                      double early_sl_mult = -1.0,
                      int    early_bar_count = 0) {
    Result R{};
    R.donchian_bars = donchian_n;
    R.sl_mult = sl_mult;
    R.tp_mult = tp_mult;

    const int atr_period = 14;
    const int warmup = std::max(donchian_n, atr_period) + 1;
    if ((int)bars.size() <= warmup + 1) return R;

    int pos_sign = 0;
    double entry_price = 0.0;
    double sl = 0.0, tp = 0.0;

    // Per-trade tracking for trades_out (only used when non-null).
    int64_t entry_ms = 0;
    size_t entry_bar_idx = 0;
    double entry_atr = 0.0;
    // S36: MAE per-bar tracker. Index 0 = bar 1 after entry, etc.
    // Reset to -1.0 on every entry; -1.0 means "bar not observed".
    double bar_mae[3] = { -1.0, -1.0, -1.0 };

    double equity = 0.0, peak = 0.0, max_dd = 0.0;
    auto record_eq = [&]() {
        if (equity > peak) peak = equity;
        double dd = peak - equity;
        if (dd > max_dd) max_dd = dd;
    };
    auto close_trade = [&](double exit_price, int64_t exit_ms,
                           const char* exit_reason, size_t exit_bar_idx) {
        double pts = (exit_price - entry_price) * pos_sign;
        double pnl = pts * spec.usd_per_point * spec.lot_size;
        R.pnl_usd += pnl;
        if (pnl >= 0.0) { R.gross_profit += pnl; R.n_wins += 1; }
        else            { R.gross_loss   += pnl; R.n_losses += 1; }
        R.n_trades += 1;
        equity += pnl;
        record_eq();
        if (trades_out != nullptr) {
            TradeRecord tr;
            tr.window_label = window_label;
            tr.donchian_bars = donchian_n;
            tr.sl_mult = sl_mult;
            tr.tp_mult = tp_mult;
            tr.direction = pos_sign;
            tr.entry_ms = entry_ms;
            tr.exit_ms = exit_ms;
            tr.entry_px = entry_price;
            tr.exit_px = exit_price;
            tr.pnl_usd = pnl;
            tr.exit_reason = exit_reason;
            tr.bars_held = (exit_bar_idx >= entry_bar_idx)
                ? (int)(exit_bar_idx - entry_bar_idx) : 0;
            tr.atr_at_entry = entry_atr;
            tr.mae_bar1 = bar_mae[0];
            tr.mae_bar2 = bar_mae[1];
            tr.mae_bar3 = bar_mae[2];
            trades_out->push_back(tr);
        }
        pos_sign = 0;
    };

    for (size_t i = warmup; i < bars.size(); ++i) {
        const Bar& b = bars[i];
        // Time-conditional SL: if early-SL mode is active, recompute sl
        // from entry_atr based on bars-since-entry. No-op when
        // early_bar_count <= 0 or early_sl_mult < 0 (defaults).
        if (pos_sign != 0 && early_bar_count > 0 && early_sl_mult >= 0.0) {
            int bars_since_entry = (i >= entry_bar_idx)
                ? (int)(i - entry_bar_idx) : 0;
            double effective_mult = (bars_since_entry < early_bar_count)
                ? early_sl_mult : sl_mult;
            if (pos_sign == +1) {
                sl = entry_price - effective_mult * entry_atr;
            } else {
                sl = entry_price + effective_mult * entry_atr;
            }
        }
        // S36: record MAE for bars 1, 2, 3 after entry, in ATR-at-entry
        // units. Done BEFORE the SL/TP hit check so the bar's adverse
        // extreme is captured even if the trade closes this same bar.
        // Convention matches the SL hit logic above:
        //   long  : adverse extreme = b.low_bid  (bid trough)
        //   short : adverse extreme = b.high_ask (ask peak)
        if (pos_sign != 0 && entry_atr > 0.0) {
            int bars_since_entry = (i >= entry_bar_idx)
                ? (int)(i - entry_bar_idx) : 0;
            if (bars_since_entry >= 1 && bars_since_entry <= 3) {
                double adverse_pts = 0.0;
                if (pos_sign == +1) {
                    adverse_pts = entry_price - b.low_bid;
                } else {
                    adverse_pts = b.high_ask - entry_price;
                }
                if (adverse_pts < 0.0) adverse_pts = 0.0;
                double mae_atr = adverse_pts / entry_atr;
                bar_mae[bars_since_entry - 1] = mae_atr;
            }
        }
        if (pos_sign != 0) {
            bool hit_sl = false, hit_tp = false;
            int64_t hit_sl_ms = 0, hit_tp_ms = 0;
            if (pos_sign == +1) {
                if (b.low_bid  <= sl) { hit_sl = true; hit_sl_ms = b.low_bid_ms;  }
                if (b.high_bid >= tp) { hit_tp = true; hit_tp_ms = b.high_bid_ms; }
            } else {
                if (b.high_ask >= sl) { hit_sl = true; hit_sl_ms = b.high_ask_ms; }
                if (b.low_ask  <= tp) { hit_tp = true; hit_tp_ms = b.low_ask_ms;  }
            }
            if (hit_sl && hit_tp) {
                if (hit_sl_ms <= hit_tp_ms) close_trade(sl, hit_sl_ms, "SL", i);
                else                        close_trade(tp, hit_tp_ms, "TP", i);
            } else if (hit_sl) close_trade(sl, hit_sl_ms, "SL", i);
            else if (hit_tp)   close_trade(tp, hit_tp_ms, "TP", i);
        }
        if (pos_sign != 0 && (i + 1 < bars.size())) {
            const Bar& nb = bars[i+1];
            if (!in_trading_window(nb.bar_open_ms, spec)) {
                double exit_px = (pos_sign == +1) ? b.close_bid : b.close_ask;
                close_trade(exit_px, b.bar_open_ms, "WEEKEND", i);
            }
        }
        if (pos_sign == 0 && in_trading_window(b.bar_open_ms, spec)) {
            double dc_high = -1e300;
            double dc_low  =  1e300;
            for (int k = 1; k <= donchian_n; ++k) {
                const Bar& pb = bars[i - k];
                if (pb.high_bid > dc_high) dc_high = pb.high_bid;
                if (pb.low_bid  < dc_low ) dc_low  = pb.low_bid;
            }
            double a = atr[i];
            if (a <= 0.0) continue;
            if (b.close_bid > dc_high) {
                pos_sign = +1;
                entry_price = b.close_ask;
                sl = entry_price - sl_mult * a;
                tp = entry_price + tp_mult * a;
                entry_ms = b.bar_open_ms;
                entry_bar_idx = i;
                entry_atr = a;
                bar_mae[0] = -1.0; bar_mae[1] = -1.0; bar_mae[2] = -1.0;
            } else if (b.close_bid < dc_low) {
                pos_sign = -1;
                entry_price = b.close_bid;
                sl = entry_price + sl_mult * a;
                tp = entry_price - tp_mult * a;
                entry_ms = b.bar_open_ms;
                entry_bar_idx = i;
                entry_atr = a;
                bar_mae[0] = -1.0; bar_mae[1] = -1.0; bar_mae[2] = -1.0;
            }
        }
    }
    if (pos_sign != 0 && !bars.empty()) {
        const Bar& b = bars.back();
        double exit_px = (pos_sign == +1) ? b.close_bid : b.close_ask;
        close_trade(exit_px, b.bar_open_ms, "EOF", bars.size() - 1);
    }
    R.win_rate = (R.n_trades > 0) ? (double)R.n_wins / (double)R.n_trades : 0.0;
    R.profit_factor = (R.gross_loss < 0.0)
        ? (R.gross_profit / -R.gross_loss)
        : (R.gross_profit > 0.0 ? 1e9 : 0.0);
    R.max_dd_usd = max_dd;
    return R;
}

// ============================================================
// CLI date parsing + slicing + utility
// ============================================================
static int64_t parse_ymd(const std::string& s) {
    if (s.size() < 10 || s[4] != '-' || s[7] != '-') {
        std::cerr << "ERROR: bad date (need YYYY-MM-DD): " << s << "\n";
        std::exit(1);
    }
    int y = std::atoi(s.substr(0,4).c_str());
    int m = std::atoi(s.substr(5,2).c_str());
    int d = std::atoi(s.substr(8,2).c_str());
    return ymdhms_to_ms(y, m, d, 0, 0, 0);
}

static std::string fmt_ts(int64_t ms) {
    std::time_t t = (std::time_t)(ms / 1000);
    std::tm* tmv = std::gmtime(&t);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                  tmv->tm_year + 1900, tmv->tm_mon + 1, tmv->tm_mday,
                  tmv->tm_hour, tmv->tm_min, tmv->tm_sec);
    return std::string(buf);
}

static std::vector<Bar> slice_bars(const std::vector<Bar>& bars,
                                   int64_t start_ms, int64_t end_ms) {
    std::vector<Bar> out;
    out.reserve(bars.size() / 4 + 16);
    for (const auto& b : bars) {
        if (b.bar_open_ms >= start_ms && b.bar_open_ms < end_ms) {
            out.push_back(b);
        }
    }
    return out;
}

// ============================================================
// Option B specifics: pick best cell + per-step record
// ============================================================
struct PickedCell {
    int donchian;
    double sl;
    double tp;
};

// Tie-breaking: PF desc -> PnL desc -> donchian asc -> sl asc -> tp asc.
// Cells with n_trades < 5 are excluded entirely.
static const int kMinTradesForPick = 5;

static int pick_best_cell(const std::vector<Result>& grid) {
    int best = -1;
    for (int i = 0; i < (int)grid.size(); ++i) {
        const Result& r = grid[i];
        if (r.n_trades < kMinTradesForPick) continue;
        if (best < 0) { best = i; continue; }
        const Result& bb = grid[best];
        // Compare: higher PF wins
        if (r.profit_factor > bb.profit_factor) { best = i; continue; }
        if (r.profit_factor < bb.profit_factor) continue;
        // PF tie: higher PnL wins
        if (r.pnl_usd > bb.pnl_usd) { best = i; continue; }
        if (r.pnl_usd < bb.pnl_usd) continue;
        // PnL tie: lower donchian wins
        if (r.donchian_bars < bb.donchian_bars) { best = i; continue; }
        if (r.donchian_bars > bb.donchian_bars) continue;
        // donchian tie: lower sl wins
        if (r.sl_mult < bb.sl_mult) { best = i; continue; }
        if (r.sl_mult > bb.sl_mult) continue;
        // sl tie: lower tp wins
        if (r.tp_mult < bb.tp_mult) { best = i; continue; }
    }
    return best;
}

struct StepRecord {
    std::string is_label;     // e.g. "W1"  or "W1+W2"
    std::string oos_label;    // e.g. "W2"  or "W3"
    int64_t is_start_ms;
    int64_t is_end_ms;
    int64_t oos_start_ms;
    int64_t oos_end_ms;
    size_t is_bars;
    size_t oos_bars;
    int eligible_cells;       // cells in IS sweep that met min-trades
    PickedCell picked;
    Result is_result;
    Result oos_result;
};

int main(int argc, char** argv) {
    if (argc != 8) {
        std::cerr << "Usage: " << argv[0] << " <SYMBOL> <tick_csv> "
                  << "<W1_start> <W2_start> <W3_start> <W4_start> <W4_end>\n";
        std::cerr << "Dates are YYYY-MM-DD UTC. 5 dates define 4 windows.\n";
        std::cerr << "\nAnchored Option B walk-forward:\n";
        std::cerr << "  Step 1: IS = W1            -> OOS = W2\n";
        std::cerr << "  Step 2: IS = W1+W2         -> OOS = W3\n";
        std::cerr << "  Step 3: IS = W1+W2+W3      -> OOS = W4\n";
        std::cerr << "\nExample (XAUUSD):\n";
        std::cerr << "  " << argv[0] << " XAUUSD /Users/jo/tick/2yr_XAUUSD_tick.csv "
                  << "2023-10-01 2024-04-01 2024-10-01 2025-04-01 2025-09-30\n";
        return 1;
    }
    std::string symbol = argv[1];
    std::string tick_path = argv[2];

    const SymbolSpec* spec = find_spec(symbol);
    if (!spec) {
        std::cerr << "ERROR: unknown symbol " << symbol << "\n";
        return 1;
    }
    if (spec->usd_per_point <= 0.0) {
        std::cerr << "ERROR: " << symbol << " has placeholder usd_per_point=0\n";
        return 1;
    }

    int64_t d1 = parse_ymd(argv[3]);
    int64_t d2 = parse_ymd(argv[4]);
    int64_t d3 = parse_ymd(argv[5]);
    int64_t d4 = parse_ymd(argv[6]);
    int64_t d5 = parse_ymd(argv[7]);
    if (!(d1 < d2 && d2 < d3 && d3 < d4 && d4 < d5)) {
        std::cerr << "ERROR: window dates must be strictly increasing\n";
        return 1;
    }

    std::cerr << "Symbol:        " << spec->name << "\n";
    std::cerr << "usd_per_point: " << spec->usd_per_point << "\n";
    if (symbol == "GER40") {
        std::cerr << "*** CURRENCY:  EUR (NOT USD). All P&L figures below are EUR. ***\n";
    }
    std::cerr << "lot_size:      " << spec->lot_size << "\n";
    std::cerr << "Session UTC:   Sun " << spec->sun_open_h_utc
              << ":00 -> Fri " << spec->fri_close_h_utc << ":00\n";
    std::cerr << "Min trades for pick: " << kMinTradesForPick << "\n";
    std::cerr << "Tick file:     " << tick_path << "\n\n";

    // ------------------------------------------------------------
    // S36: Time-conditional SL via env vars (default = no-op).
    // OMEGA_EARLY_SL_MULT  : float, multiplier applied to entry_atr
    //                        for the first OMEGA_EARLY_BAR_COUNT bars
    //                        after entry. Must be >= 0.0 to activate.
    // OMEGA_EARLY_BAR_COUNT: int, number of bars after entry during
    //                        which the early SL applies. Must be > 0
    //                        to activate.
    // Both must be set to activate; otherwise behavior is byte-
    // identical to the pre-S36 harness.
    // ------------------------------------------------------------
    double early_sl_mult = -1.0;
    int    early_bar_count = 0;
    {
        const char* env_sl = std::getenv("OMEGA_EARLY_SL_MULT");
        const char* env_bc = std::getenv("OMEGA_EARLY_BAR_COUNT");
        if (env_sl != nullptr && env_sl[0] != '\0') {
            char* end = nullptr;
            double v = std::strtod(env_sl, &end);
            if (end != env_sl && v >= 0.0) early_sl_mult = v;
        }
        if (env_bc != nullptr && env_bc[0] != '\0') {
            char* end = nullptr;
            long v = std::strtol(env_bc, &end, 10);
            if (end != env_bc && v > 0) early_bar_count = (int)v;
        }
        bool active = (early_sl_mult >= 0.0) && (early_bar_count > 0);
        std::cerr << "Early-SL mode: "
                  << (active ? "ACTIVE" : "OFF (default)") << "\n";
        if (active) {
            std::cerr << "  early_sl_mult   = " << early_sl_mult << "\n";
            std::cerr << "  early_bar_count = " << early_bar_count << "\n";
        } else {
            std::cerr << "  (set OMEGA_EARLY_SL_MULT and OMEGA_EARLY_BAR_COUNT to activate)\n";
        }
        std::cerr << "\n";
    }

    std::vector<Bar> all_bars = load_and_aggregate(tick_path);
    if (all_bars.size() < 1000) {
        std::cerr << "ERROR: too few bars (" << all_bars.size() << ")\n";
        return 2;
    }
    std::cerr << "Total bars: " << all_bars.size()
              << "  first=" << fmt_ts(all_bars.front().bar_open_ms)
              << "  last=" << fmt_ts(all_bars.back().bar_open_ms) << "\n\n";

    std::cerr << "W1: " << fmt_ts(d1) << " -> " << fmt_ts(d2) << "\n";
    std::cerr << "W2: " << fmt_ts(d2) << " -> " << fmt_ts(d3) << "\n";
    std::cerr << "W3: " << fmt_ts(d3) << " -> " << fmt_ts(d4) << "\n";
    std::cerr << "W4: " << fmt_ts(d4) << " -> " << fmt_ts(d5) << "\n\n";

    const std::vector<int>    donchian_set = {10, 15, 20};
    const std::vector<double> sl_set       = {1.0, 1.5, 2.0};
    const std::vector<double> tp_set       = {2.0, 3.0, 4.0};
    const int n_cells = (int)(donchian_set.size() * sl_set.size() * tp_set.size());

    struct StepDef {
        std::string is_label;
        std::string oos_label;
        int64_t is_start;
        int64_t is_end;
        int64_t oos_start;
        int64_t oos_end;
    };
    std::vector<StepDef> steps = {
        {"W1",       "W2", d1, d2, d2, d3},
        {"W1+W2",    "W3", d1, d3, d3, d4},
        {"W1+W2+W3", "W4", d1, d4, d4, d5},
    };

    std::vector<StepRecord> records;
    records.reserve(steps.size());

    // Per-trade rows captured across all OOS picked-cell runs (W2 + W3 + W4).
    // Written to walkforward_b_<SYM>_trades.csv after the step loop.
    std::vector<TradeRecord> all_oos_trades;
    all_oos_trades.reserve(2048);

    for (size_t si = 0; si < steps.size(); ++si) {
        const StepDef& sd = steps[si];
        std::cerr << "===== STEP " << (si + 1) << ": IS=" << sd.is_label
                  << " OOS=" << sd.oos_label << " =====\n";

        // Slice IS + OOS
        auto is_slice  = slice_bars(all_bars, sd.is_start,  sd.is_end);
        auto oos_slice = slice_bars(all_bars, sd.oos_start, sd.oos_end);
        std::cerr << "IS bars:  " << is_slice.size()
                  << "  (" << fmt_ts(sd.is_start) << " -> " << fmt_ts(sd.is_end) << ")\n";
        std::cerr << "OOS bars: " << oos_slice.size()
                  << "  (" << fmt_ts(sd.oos_start) << " -> " << fmt_ts(sd.oos_end) << ")\n";

        if (is_slice.size() < 100) {
            std::cerr << "ERROR: IS slice too small for step " << (si + 1) << "\n";
            return 4;
        }
        if (oos_slice.size() < 50) {
            std::cerr << "WARNING: OOS slice very small for step " << (si + 1)
                      << " (" << oos_slice.size() << " bars)\n";
        }

        // ATR cold-start per slice (matches A2 convention)
        auto is_atr  = compute_atr(is_slice,  14);
        auto oos_atr = compute_atr(oos_slice, 14);

        // Sweep all 27 cells on IS
        std::vector<Result> is_grid;
        is_grid.reserve(n_cells);
        int cell = 0;
        for (int dn : donchian_set) {
            for (double sm : sl_set) {
                for (double tm : tp_set) {
                    cell += 1;
                    Result r = run_one(is_slice, is_atr, dn, sm, tm, *spec,
                                      nullptr, std::string(),
                                      early_sl_mult, early_bar_count);
                    std::cerr << "[IS " << sd.is_label << " " << cell << "/" << n_cells
                              << "] d=" << dn << " sl=" << sm << " tp=" << tm
                              << " trades=" << r.n_trades
                              << " pnl=$" << r.pnl_usd
                              << " pf=" << r.profit_factor
                              << " wr=" << r.win_rate << "\n";
                    is_grid.push_back(r);
                }
            }
        }

        // Count eligible cells (>= kMinTradesForPick)
        int eligible = 0;
        for (const auto& r : is_grid) {
            if (r.n_trades >= kMinTradesForPick) eligible += 1;
        }
        std::cerr << "Eligible IS cells (n_trades>=" << kMinTradesForPick
                  << "): " << eligible << "/" << n_cells << "\n";

        // Pick winner
        int pick_idx = pick_best_cell(is_grid);
        if (pick_idx < 0) {
            std::cerr << "ERROR: no IS cell met min-trades threshold for step "
                      << (si + 1) << "\n";
            return 5;
        }
        const Result& pick = is_grid[pick_idx];
        std::cerr << "PICKED: d=" << pick.donchian_bars
                  << " sl=" << pick.sl_mult
                  << " tp=" << pick.tp_mult
                  << "  IS pf=" << pick.profit_factor
                  << " IS pnl=$" << pick.pnl_usd
                  << " IS trades=" << pick.n_trades << "\n";

        // Run picked cell on OOS — capture per-trade rows for trades.csv.
        // We pass &all_oos_trades and the OOS window label ("W2"/"W3"/"W4").
        Result oos_r = run_one(oos_slice, oos_atr,
                               pick.donchian_bars, pick.sl_mult, pick.tp_mult,
                               *spec, &all_oos_trades, sd.oos_label,
                               early_sl_mult, early_bar_count);
        std::cerr << "OOS:    pf=" << oos_r.profit_factor
                  << " pnl=$" << oos_r.pnl_usd
                  << " trades=" << oos_r.n_trades
                  << " wr=" << oos_r.win_rate
                  << " maxdd=$" << oos_r.max_dd_usd << "\n\n";

        StepRecord rec;
        rec.is_label  = sd.is_label;
        rec.oos_label = sd.oos_label;
        rec.is_start_ms  = sd.is_start;
        rec.is_end_ms    = sd.is_end;
        rec.oos_start_ms = sd.oos_start;
        rec.oos_end_ms   = sd.oos_end;
        rec.is_bars      = is_slice.size();
        rec.oos_bars     = oos_slice.size();
        rec.eligible_cells = eligible;
        rec.picked = {pick.donchian_bars, pick.sl_mult, pick.tp_mult};
        rec.is_result  = pick;
        rec.oos_result = oos_r;
        records.push_back(rec);
    }

    // Combined OOS: aggregate trades from all OOS results.
    // We only have aggregate counts per step (not per-trade lists), so
    // combined PF is computed from summed gross_profit / gross_loss; this
    // is the correct definition (PF = sum-of-wins / |sum-of-losses|).
    Result combined{};
    combined.donchian_bars = -1;
    combined.sl_mult = -1.0;
    combined.tp_mult = -1.0;
    double combined_max_dd = 0.0;
    for (const auto& rec : records) {
        const Result& o = rec.oos_result;
        combined.n_trades     += o.n_trades;
        combined.n_wins       += o.n_wins;
        combined.n_losses     += o.n_losses;
        combined.gross_profit += o.gross_profit;
        combined.gross_loss   += o.gross_loss;
        combined.pnl_usd      += o.pnl_usd;
        if (o.max_dd_usd > combined_max_dd) combined_max_dd = o.max_dd_usd;
    }
    combined.win_rate = (combined.n_trades > 0)
        ? (double)combined.n_wins / (double)combined.n_trades : 0.0;
    combined.profit_factor = (combined.gross_loss < 0.0)
        ? (combined.gross_profit / -combined.gross_loss)
        : (combined.gross_profit > 0.0 ? 1e9 : 0.0);
    combined.max_dd_usd = combined_max_dd;  // upper-bound only — true cross-segment DD requires equity stream

    // ---------- picks.csv ----------
    std::string picks_path = "walkforward_b_" + symbol + "_picks.csv";
    std::ofstream pf(picks_path);
    if (!pf) {
        std::cerr << "ERROR: cannot write " << picks_path << "\n";
        return 3;
    }
    pf << "step,is_label,oos_label,is_start,is_end,oos_start,oos_end,"
       << "is_bars,oos_bars,eligible_cells,"
       << "picked_donchian,picked_sl,picked_tp,"
       << "is_trades,is_pnl,is_pf,is_wr,is_maxdd,"
       << "oos_trades,oos_pnl,oos_pf,oos_wr,oos_maxdd\n";
    for (size_t si = 0; si < records.size(); ++si) {
        const StepRecord& r = records[si];
        const Result& ir = r.is_result;
        const Result& orr = r.oos_result;
        pf << (si + 1) << ","
           << r.is_label << "," << r.oos_label << ","
           << fmt_ts(r.is_start_ms).substr(0,10) << ","
           << fmt_ts(r.is_end_ms).substr(0,10) << ","
           << fmt_ts(r.oos_start_ms).substr(0,10) << ","
           << fmt_ts(r.oos_end_ms).substr(0,10) << ","
           << r.is_bars << "," << r.oos_bars << "," << r.eligible_cells << ","
           << r.picked.donchian << "," << r.picked.sl << "," << r.picked.tp << ","
           << ir.n_trades << "," << ir.pnl_usd << "," << ir.profit_factor << ","
           << ir.win_rate << "," << ir.max_dd_usd << ","
           << orr.n_trades << "," << orr.pnl_usd << "," << orr.profit_factor << ","
           << orr.win_rate << "," << orr.max_dd_usd << "\n";
    }
    pf.close();
    std::cerr << "Wrote " << picks_path << "\n";

    // ---------- trades.csv (per-trade OOS rows) ----------
    {
        std::string trades_path = "walkforward_b_" + symbol + "_trades.csv";
        std::ofstream tf(trades_path);
        if (!tf) {
            std::cerr << "ERROR: cannot write " << trades_path << "\n";
            return 3;
        }
        tf << "window,donchian,sl_mult,tp_mult,direction,"
           << "entry_ts,exit_ts,entry_px,exit_px,pnl,exit_reason,bars_held,"
           << "atr_at_entry,mae_bar1,mae_bar2,mae_bar3\n";
        for (const TradeRecord& tr : all_oos_trades) {
            tf << tr.window_label << ","
               << tr.donchian_bars << ","
               << tr.sl_mult << ","
               << tr.tp_mult << ","
               << (tr.direction == +1 ? "long" : "short") << ","
               << fmt_ts(tr.entry_ms) << ","
               << fmt_ts(tr.exit_ms) << ","
               << tr.entry_px << ","
               << tr.exit_px << ","
               << tr.pnl_usd << ","
               << tr.exit_reason << ","
               << tr.bars_held << ","
               << tr.atr_at_entry << ","
               << tr.mae_bar1 << ","
               << tr.mae_bar2 << ","
               << tr.mae_bar3 << "\n";
        }
        tf.close();
        std::cerr << "Wrote " << trades_path
                  << " (" << all_oos_trades.size() << " OOS trades)\n";
    }

    // ---------- summary.txt ----------
    std::string sum_path = "walkforward_b_" + symbol + "_summary.txt";
    std::ofstream uf(sum_path);
    if (!uf) {
        std::cerr << "ERROR: cannot write " << sum_path << "\n";
        return 3;
    }
    uf << "Walk-forward Option B (anchored) summary - " << symbol << "\n";
    uf << "==================================================\n\n";
    if (symbol == "GER40") {
        uf << "*** CURRENCY: EUR (NOT USD). All P&L figures below are EUR. ***\n\n";
    }
    uf << "Selection rule: max profit_factor (n_trades >= "
       << kMinTradesForPick << "), tie -> max pnl, "
       << "tie -> min donchian, tie -> min sl, tie -> min tp\n\n";

    for (size_t si = 0; si < records.size(); ++si) {
        const StepRecord& r = records[si];
        const Result& ir = r.is_result;
        const Result& orr = r.oos_result;
        uf << "STEP " << (si + 1)
           << " | IS=" << r.is_label
           << " (" << fmt_ts(r.is_start_ms).substr(0,10)
           << " -> " << fmt_ts(r.is_end_ms).substr(0,10)
           << ", " << r.is_bars << " bars, "
           << r.eligible_cells << "/" << n_cells << " eligible cells)\n"
           << "       OOS=" << r.oos_label
           << " (" << fmt_ts(r.oos_start_ms).substr(0,10)
           << " -> " << fmt_ts(r.oos_end_ms).substr(0,10)
           << ", " << r.oos_bars << " bars)\n"
           << "  PICKED: d=" << r.picked.donchian
           << " sl=" << r.picked.sl
           << " tp=" << r.picked.tp << "\n";
        char buf[320];
        std::snprintf(buf, sizeof(buf),
            "  IS:  trades=%d  pnl=$%.2f  pf=%.3f  wr=%.3f  maxdd=$%.2f\n",
            ir.n_trades, ir.pnl_usd, ir.profit_factor, ir.win_rate, ir.max_dd_usd);
        uf << buf;
        std::snprintf(buf, sizeof(buf),
            "  OOS: trades=%d  pnl=$%.2f  pf=%.3f  wr=%.3f  maxdd=$%.2f\n\n",
            orr.n_trades, orr.pnl_usd, orr.profit_factor, orr.win_rate, orr.max_dd_usd);
        uf << buf;
    }

    uf << "Combined OOS (W2_OOS + W3_OOS + W4_OOS):\n";
    char buf[512];
    std::snprintf(buf, sizeof(buf),
        "  trades=%d  wins=%d  losses=%d  wr=%.3f\n"
        "  gross_profit=$%.2f  gross_loss=$%.2f  pnl=$%.2f  pf=%.3f\n"
        "  max_dd_per_segment_upper_bound=$%.2f\n\n",
        combined.n_trades, combined.n_wins, combined.n_losses, combined.win_rate,
        combined.gross_profit, combined.gross_loss, combined.pnl_usd,
        combined.profit_factor, combined.max_dd_usd);
    uf << buf;

    // Pick stability check
    uf << "Pick stability across steps:\n";
    bool same_d = true, same_sl = true, same_tp = true;
    for (size_t si = 1; si < records.size(); ++si) {
        if (records[si].picked.donchian != records[0].picked.donchian) same_d = false;
        if (records[si].picked.sl       != records[0].picked.sl)       same_sl = false;
        if (records[si].picked.tp       != records[0].picked.tp)       same_tp = false;
    }
    uf << "  donchian stable: " << (same_d  ? "YES" : "NO") << "\n";
    uf << "  sl stable:       " << (same_sl ? "YES" : "NO") << "\n";
    uf << "  tp stable:       " << (same_tp ? "YES" : "NO") << "\n";
    uf << "\n";

    uf << "Pick history:\n";
    for (size_t si = 0; si < records.size(); ++si) {
        uf << "  step " << (si + 1) << ": d="
           << records[si].picked.donchian
           << " sl=" << records[si].picked.sl
           << " tp=" << records[si].picked.tp << "\n";
    }
    uf.close();
    std::cerr << "Wrote " << sum_path << "\n";

    std::cerr << "\n----- summary -----\n";
    std::ifstream summary_in(sum_path);
    if (summary_in) {
        std::string line;
        while (std::getline(summary_in, line)) std::cerr << line << "\n";
    }
    return 0;
}
