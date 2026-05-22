// XauScalper v3 -- structural fixes over v2 (2026-05-22)
//
// Changes vs v2:
//   1. Pre-trade cost gate: prospective TP must cover spread + slippage + buffer.
//      Enforced by default. Disable via --enforce-cost-gate 0.
//   2. RR geometry: be_trigger raised so BE-locked R:R is positive
//      (be_trigger > emergency_loss). Defaults adjusted; user can override.
//   3. has_l2 now row-level: requires real bid_vol + ask_vol OR depth_events>0
//      OR imb != 0.5 (placeholder rows do not trip the L2 filter).
//   4. min_atr_points raised to ~25 (matches XAU tick noise; prior 0.80 was below
//      a single spread crossing and let entries fire into ranges that can't pay).
//   5. Spread gate at ENTRY uses danger_spread_points -- engine won't open into
//      widening spread regimes only to immediately exit via l2_exit_votes.
//   6. Consec-loss circuit breaker: N losses in a row -> long cooldown
//      (default 5 losses -> 600s pause). Prevents Asia-tape bleed-out.
//   7. Session trade cap separate from max_trades absolute -- daily/session
//      limit (default 100/session) so runaway is bounded.
//   8. min_atr scaled vs spread: atr must be >= (spread * atr_vs_spread_min).
//      Catches the "ATR=0.8pts vs spread=22pts" pathology directly.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using std::cerr;
using std::cout;
using std::deque;
using std::fixed;
using std::ifstream;
using std::ofstream;
using std::setprecision;
using std::string;
using std::vector;

static string trim(const string& s)
{
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

static string lower_copy(string s)
{
    for (size_t i = 0; i < s.size(); ++i) s[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
    return s;
}

static vector<string> split_csv(const string& line)
{
    vector<string> out;
    string cur;
    bool q = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '"') q = !q;
        else if (c == ',' && !q) { out.push_back(trim(cur)); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(trim(cur));
    return out;
}

static bool has_alpha(const string& s)
{
    for (size_t i = 0; i < s.size(); ++i) {
        if (std::isalpha(static_cast<unsigned char>(s[i]))) return true;
    }
    return false;
}

static bool to_double(const string& s, double& v)
{
    if (s.empty()) return false;
    char* e = nullptr;
    v = std::strtod(s.c_str(), &e);
    if (e == s.c_str()) return false;
    while (*e) {
        if (!std::isspace(static_cast<unsigned char>(*e))) return false;
        ++e;
    }
    return std::isfinite(v);
}

static bool to_i64(const string& s, int64_t& v)
{
    if (s.empty()) return false;
    char* e = nullptr;
    long long x = std::strtoll(s.c_str(), &e, 10);
    if (e == s.c_str()) return false;
    while (*e) {
        if (!std::isspace(static_cast<unsigned char>(*e))) return false;
        ++e;
    }
    v = static_cast<int64_t>(x);
    return true;
}

struct Config {
    double point_size = 0.01;

    // Spread limits.
    // max_spread_points = absolute upper bound for ENTRY (formerly 32; raised so
    //   danger_spread is the actual gate per fix #5).
    // danger_spread_points = used for BOTH entry block AND exit signal.
    double max_spread_points = 60.0;
    double danger_spread_points = 45.0;

    // Minimum market-condition thresholds.
    // atr_vs_spread_min = atr must be >= spread * this. Default 1.2 means
    //   atr alone must cover one round-trip cross plus 20% slack.
    double atr_vs_spread_min = 1.2;
    double min_range_points = 42.0;
    double min_velocity_points = 8.0;
    double min_atr_points = 25.0;     // was 0.80 -- pathological vs $0.22 spread

    // Cost gate -- prospective TP must cover round-trip cost.
    // round_trip_cost_points = spread + slippage + buffer
    double slippage_points = 4.0;
    double cost_buffer_points = 6.0;
    bool   enforce_cost_gate = true;

    // Indicators.
    double rsi_long = 58.0;
    double rsi_short = 42.0;
    int rsi_period = 14;
    int range_window_ms = 45000;
    int velocity_window_ms = 4000;

    // State timing.
    double arm_seconds = 1.50;
    double cooldown_seconds = 8.00;

    // Position management.
    double emergency_loss_points = 36.0;
    double early_fail_seconds = 8.0;
    double early_fail_max_profit_points = 14.0;

    // RR geometry. Raised from v2 (32) so BE-locked path has positive RR.
    // be_trigger_points should be >= emergency_loss_points to avoid the
    //   "lose more on stop than you lock in on BE" pathology.
    double be_trigger_points = 40.0;
    double be_lock_points = 4.0;
    double profit_trail_trigger_points = 55.0;
    double loose_trail_points = 16.0;
    double tight_trail_points = 7.0;
    double reversal_exit_min_profit_points = 28.0;
    double hard_profit_take_points = 100.0;  // 100 / 36 = RR 2.78

    // L2 entry filters.
    double long_l2_imb_floor = 0.4800;
    double short_l2_imb_ceiling = 0.5200;
    double long_exit_l2_imb = 0.4800;
    double short_exit_l2_imb = 0.5200;
    double l2_extreme_long = 0.5550;
    double l2_extreme_short = 0.4450;

    double ewm_drift_block_long = -0.0800;
    double ewm_drift_block_short = 0.0800;
    double ewm_drift_exit_long = -0.0200;
    double ewm_drift_exit_short = 0.0200;

    double micro_edge_block_long = 0.4700;
    double micro_edge_block_short = 0.5300;
    double micro_edge_exit_long = 0.4850;
    double micro_edge_exit_short = 0.5150;

    double max_vpin = 0.85;
    double min_vol_ratio = 0.0;
    double max_vol_ratio = 50.0;
    int min_depth_events_total = 1;

    bool require_l2 = false;
    bool block_watchdog_dead = true;
    bool use_l2_entry_filter = true;
    bool use_l2_exit_filter = true;
    bool allow_long = true;
    bool allow_short = true;

    // Trade-rate caps.
    int max_trades = 1000000;        // absolute (debug)
    int max_session_trades = 100;    // per backtest run / live session
    int consec_loss_breaker = 5;     // N consecutive losses -> long pause
    double consec_loss_pause_sec = 600.0;

    // Session-hour filter (UTC). Entries only allowed when current UTC hour
    // is in [allow_hour_utc_start, allow_hour_utc_end). Set to -1 to disable.
    // Default disabled. Use 7,21 to block Asia (22-06 UTC). Handles wrap when
    // start > end (e.g. 22,6 = 22:00-06:00 window).
    int allow_hour_utc_start = -1;
    int allow_hour_utc_end = -1;
};

struct Layout {
    bool has_header = false;
    int ts_ms = -1;
    int date = -1;
    int time = -1;
    int bid = -1;
    int ask = -1;
    int mid = -1;
    int l2_imb = -1;
    int l2_bid_vol = -1;
    int l2_ask_vol = -1;
    int depth_bid_levels = -1;
    int depth_ask_levels = -1;
    int depth_events_total = -1;
    int watchdog_dead = -1;
    int vol_ratio = -1;
    int regime = -1;
    int vpin = -1;
    int micro_edge = -1;
    int ewm_drift = -1;
};

static int find_col(const vector<string>& h, const vector<string>& names)
{
    for (size_t i = 0; i < h.size(); ++i) {
        string x = lower_copy(trim(h[i]));
        for (size_t j = 0; j < names.size(); ++j) {
            if (x == lower_copy(names[j])) return static_cast<int>(i);
        }
    }
    return -1;
}

static Layout detect_layout(const vector<string>& row)
{
    Layout l;
    string joined;
    for (size_t i = 0; i < row.size(); ++i) joined += row[i] + " ";
    l.has_header = has_alpha(joined);

    if (l.has_header) {
        vector<string> h;
        for (size_t i = 0; i < row.size(); ++i) h.push_back(lower_copy(trim(row[i])));
        l.ts_ms = find_col(h, {"ts_ms", "timestamp_ms", "timestamp", "ts", "time"});
        l.date = find_col(h, {"date", "yyyymmdd"});
        l.time = find_col(h, {"hhmmss", "time_text", "clock"});
        l.bid = find_col(h, {"bid", "bidprice", "bid_price", "best_bid", "bestbid"});
        l.ask = find_col(h, {"ask", "askprice", "ask_price", "best_ask", "bestask"});
        l.mid = find_col(h, {"mid", "midprice", "mid_price"});
        l.l2_imb = find_col(h, {"l2_imb", "imb", "imbalance", "book_imbalance"});
        l.l2_bid_vol = find_col(h, {"l2_bid_vol", "bid_vol", "bid_volume", "bid_size"});
        l.l2_ask_vol = find_col(h, {"l2_ask_vol", "ask_vol", "ask_volume", "ask_size"});
        l.depth_bid_levels = find_col(h, {"depth_bid_levels", "bid_levels"});
        l.depth_ask_levels = find_col(h, {"depth_ask_levels", "ask_levels"});
        l.depth_events_total = find_col(h, {"depth_events_total", "depth_events"});
        l.watchdog_dead = find_col(h, {"watchdog_dead", "l2_dead", "feed_dead"});
        l.vol_ratio = find_col(h, {"vol_ratio", "volume_ratio"});
        l.regime = find_col(h, {"regime"});
        l.vpin = find_col(h, {"vpin"});
        l.micro_edge = find_col(h, {"micro_edge", "microprice_edge", "micro_price_edge"});
        l.ewm_drift = find_col(h, {"ewm_drift", "drift", "ema_drift"});
        return l;
    }

    if (row.size() >= 6 && row[1].find(':') != string::npos) {
        l.date = 0; l.time = 1; l.bid = 2; l.ask = 3;
        return l;
    }

    if (row.size() >= 3) {
        l.ts_ms = 0;
        double a = 0.0, b = 0.0;
        if (to_double(row[1], a) && to_double(row[2], b)) {
            if (a > b) { l.ask = 1; l.bid = 2; }
            else { l.bid = 1; l.ask = 2; }
        }
    }
    return l;
}

static double optional_double(const vector<string>& row, int idx, double def)
{
    if (idx < 0 || idx >= static_cast<int>(row.size())) return def;
    double v = def;
    if (!to_double(row[static_cast<size_t>(idx)], v)) return def;
    return v;
}

static int optional_int(const vector<string>& row, int idx, int def)
{
    if (idx < 0 || idx >= static_cast<int>(row.size())) return def;
    int64_t v = def;
    if (!to_i64(row[static_cast<size_t>(idx)], v)) return def;
    return static_cast<int>(v);
}

static int64_t make_ts_ms_from_date_time(const string& d, const string& t)
{
    if (d.size() < 8 || t.size() < 8) return 0;
    int yy = std::atoi(d.substr(0, 4).c_str());
    int mo = std::atoi(d.substr(4, 2).c_str());
    int dd = std::atoi(d.substr(6, 2).c_str());
    int hh = std::atoi(t.substr(0, 2).c_str());
    int mm = std::atoi(t.substr(3, 2).c_str());
    int ss = std::atoi(t.substr(6, 2).c_str());
    std::tm tmv{};
    tmv.tm_year = yy - 1900;
    tmv.tm_mon = mo - 1;
    tmv.tm_mday = dd;
    tmv.tm_hour = hh;
    tmv.tm_min = mm;
    tmv.tm_sec = ss;
    tmv.tm_isdst = 0;
    std::time_t sec = std::mktime(&tmv);
    if (sec <= 0) return 0;
    return static_cast<int64_t>(sec) * 1000;
}

struct Tick {
    int64_t ts_ms = 0;
    double bid = 0.0;
    double ask = 0.0;
    double mid = 0.0;
    bool has_l2 = false;
    double l2_imb = 0.5;
    double l2_bid_vol = 0.0;
    double l2_ask_vol = 0.0;
    int depth_bid_levels = 0;
    int depth_ask_levels = 0;
    int depth_events_total = 0;
    int watchdog_dead = 0;
    double vol_ratio = 0.0;
    int regime = 0;
    double vpin = 0.0;
    double micro_edge = 0.5;
    double ewm_drift = 0.0;
};

static bool parse_tick(const vector<string>& row, const Layout& l, Tick& t)
{
    if (l.bid < 0 || l.ask < 0) return false;
    if (l.bid >= static_cast<int>(row.size()) || l.ask >= static_cast<int>(row.size())) return false;

    double bid = 0.0, ask = 0.0;
    if (!to_double(row[static_cast<size_t>(l.bid)], bid)) return false;
    if (!to_double(row[static_cast<size_t>(l.ask)], ask)) return false;
    if (bid <= 0.0 || ask <= 0.0 || !std::isfinite(bid) || !std::isfinite(ask)) return false;
    if (ask < bid) std::swap(ask, bid);

    int64_t ts = 0;
    if (l.ts_ms >= 0 && l.ts_ms < static_cast<int>(row.size())) {
        if (!to_i64(row[static_cast<size_t>(l.ts_ms)], ts)) {
            double d = 0.0;
            if (!to_double(row[static_cast<size_t>(l.ts_ms)], d)) return false;
            ts = static_cast<int64_t>(d);
        }
        if (ts > 0 && ts < 100000000000LL) ts *= 1000;
    } else if (l.date >= 0 && l.time >= 0 && l.date < static_cast<int>(row.size()) && l.time < static_cast<int>(row.size())) {
        ts = make_ts_ms_from_date_time(row[static_cast<size_t>(l.date)], row[static_cast<size_t>(l.time)]);
    } else {
        return false;
    }
    if (ts <= 0) return false;

    t.ts_ms = ts;
    t.bid = bid;
    t.ask = ask;
    t.mid = optional_double(row, l.mid, (bid + ask) * 0.5);

    t.l2_imb = optional_double(row, l.l2_imb, 0.5);
    t.l2_bid_vol = optional_double(row, l.l2_bid_vol, 0.0);
    t.l2_ask_vol = optional_double(row, l.l2_ask_vol, 0.0);
    t.depth_bid_levels = optional_int(row, l.depth_bid_levels, 0);
    t.depth_ask_levels = optional_int(row, l.depth_ask_levels, 0);
    t.depth_events_total = optional_int(row, l.depth_events_total, 0);
    t.watchdog_dead = optional_int(row, l.watchdog_dead, 0);
    t.vol_ratio = optional_double(row, l.vol_ratio, 0.0);
    t.regime = optional_int(row, l.regime, 0);
    t.vpin = optional_double(row, l.vpin, 0.0);
    // micro_edge fallback BREAK -- if column missing it stays 0.5 (neutral)
    // rather than mirroring l2_imb. v2 used l2_imb as fallback which made the
    // micro_edge entry block redundant with the imb floor.
    t.micro_edge = optional_double(row, l.micro_edge, 0.5);
    t.ewm_drift = optional_double(row, l.ewm_drift, 0.0);

    // FIX #3: row-level has_l2 instead of column-presence has_l2.
    // Row has REAL L2 if any of:
    //   - bid_vol + ask_vol > 0 (real volume reported)
    //   - depth_events_total > 0
    //   - l2_imb is not the neutral placeholder 0.5
    // This prevents synthesized/zero-padded rows from tripping the L2 entry
    // filter chain (which would degrade to "any l2_imb close to 0.5 blocks
    // entry" -- the opposite of intent).
    const bool any_vol  = (t.l2_bid_vol + t.l2_ask_vol) > 0.0;
    const bool any_evts = t.depth_events_total > 0;
    const bool any_imb  = std::fabs(t.l2_imb - 0.5) > 1e-9;
    t.has_l2 = any_vol || any_evts || any_imb;
    return true;
}

enum class Side { NONE = 0, LONG = 1, SHORT = -1 };
enum class State { NO_TRADE = 0, ARMING, COST_RECOVERY, BREAKEVEN_LOCKED, PROFIT_TRAIL, COOLDOWN };

static const char* side_text(Side s)
{
    if (s == Side::LONG) return "LONG";
    if (s == Side::SHORT) return "SHORT";
    return "NONE";
}

struct WindowTick { int64_t ts_ms; double mid; };

struct Indicators {
    bool ready = false;
    int warm = 0;
    double prev_mid = 0.0;
    double avg_gain = 0.0;
    double avg_loss = 0.0;
    double rsi = 50.0;
    double atr_points = 0.0;
    double velocity_points = 0.0;
    double range_points = 0.0;
    double flow_score = 0.0;
};

struct Trade {
    int id = 0;
    Side side = Side::NONE;
    int64_t entry_ts_ms = 0;
    int64_t exit_ts_ms = 0;
    double entry_price = 0.0;
    double exit_price = 0.0;
    double entry_spread_points = 0.0;
    double pnl_points = 0.0;
    double mfe_points = 0.0;
    double mae_points = 0.0;
    double entry_l2_imb = 0.5;
    double exit_l2_imb = 0.5;
    double entry_ewm_drift = 0.0;
    double exit_ewm_drift = 0.0;
    string reason;
};

class XauScalper {
public:
    explicit XauScalper(const Config& c) : cfg(c) {}

    void on_tick(const Tick& t)
    {
        ++ticks_seen;
        if (last_ts_ms > 0 && t.ts_ms < last_ts_ms) { ++out_of_order; return; }
        last_ts_ms = t.ts_ms;
        update_indicators(t);
        if (!ind.ready) return;

        if (state == State::COOLDOWN) {
            if (t.ts_ms >= cooldown_until_ms) state = State::NO_TRADE;
            else return;
        }

        if (in_position) manage_position(t);
        else manage_entry(t);
    }

    const vector<Trade>& trades() const { return trade_log; }

    void summary() const
    {
        int wins = 0, losses = 0, be = 0, longs = 0, shorts = 0;
        double net = 0.0, best = -1e100, worst = 1e100;
        std::map<string, int> reasons;
        for (size_t i = 0; i < trade_log.size(); ++i) {
            const Trade& tr = trade_log[i];
            net += tr.pnl_points;
            if (tr.pnl_points > 0.0) ++wins;
            else if (tr.pnl_points < 0.0) ++losses;
            else ++be;
            if (tr.side == Side::LONG) ++longs;
            if (tr.side == Side::SHORT) ++shorts;
            best = std::max(best, tr.pnl_points);
            worst = std::min(worst, tr.pnl_points);
            ++reasons[tr.reason];
        }
        double wr = trade_log.empty() ? 0.0 : 100.0 * static_cast<double>(wins) / static_cast<double>(trade_log.size());
        cout << "\nXauScalper summary\n";
        cout << "ticks_seen=" << ticks_seen << "\n";
        cout << "out_of_order=" << out_of_order << "\n";
        cout << "cost_gate_rejects=" << cost_gate_rejects << "\n";
        cout << "circuit_breaker_trips=" << circuit_breaker_trips << "\n";
        cout << "session_cap_hit=" << (session_cap_hit ? 1 : 0) << "\n";
        cout << "trades=" << trade_log.size() << "\n";
        cout << "longs=" << longs << " shorts=" << shorts << "\n";
        cout << "wins=" << wins << " losses=" << losses << " breakeven=" << be << "\n";
        cout << "win_rate_pct=" << fixed << setprecision(2) << wr << "\n";
        cout << "net_points=" << fixed << setprecision(2) << net << "\n";
        if (!trade_log.empty()) {
            cout << "avg_points=" << fixed << setprecision(2) << net / static_cast<double>(trade_log.size()) << "\n";
            cout << "best_points=" << fixed << setprecision(2) << best << "\n";
            cout << "worst_points=" << fixed << setprecision(2) << worst << "\n";
        }
        cout << "exit_reasons:\n";
        for (const auto& kv : reasons) cout << "  " << kv.first << "=" << kv.second << "\n";
    }

private:
    Config cfg;
    Indicators ind;
    deque<WindowTick> range_win;
    deque<WindowTick> vel_win;
    vector<Trade> trade_log;

    State state = State::NO_TRADE;
    Side armed_side = Side::NONE;
    int64_t armed_ts_ms = 0;
    int64_t cooldown_until_ms = 0;
    int64_t last_ts_ms = 0;
    int64_t ticks_seen = 0;
    int64_t out_of_order = 0;

    bool in_position = false;
    Side pos_side = Side::NONE;
    int trade_id = 0;
    int64_t entry_ts_ms = 0;
    double entry_price = 0.0;
    double entry_spread_points = 0.0;
    double stop_price = 0.0;
    double best_exit_price = 0.0;
    double mfe_points = 0.0;
    double mae_points = 0.0;
    double entry_l2_imb = 0.5;
    double entry_ewm_drift = 0.0;

    // Diagnostics.
    int64_t cost_gate_rejects = 0;
    int64_t circuit_breaker_trips = 0;
    int     consec_losses = 0;
    bool    session_cap_hit = false;

    double points(double px) const { return px / cfg.point_size; }
    double spread_points(const Tick& t) const { return points(t.ask - t.bid); }
    double entry_px(const Tick& t, Side s) const { return s == Side::LONG ? t.ask : t.bid; }
    double exit_px(const Tick& t, Side s) const { return s == Side::LONG ? t.bid : t.ask; }

    double pnl_now(const Tick& t) const
    {
        if (pos_side == Side::LONG) return points(exit_px(t, pos_side) - entry_price);
        if (pos_side == Side::SHORT) return points(entry_price - exit_px(t, pos_side));
        return 0.0;
    }

    void update_indicators(const Tick& t)
    {
        if (ind.prev_mid <= 0.0) { ind.prev_mid = t.mid; return; }
        double diff = points(t.mid - ind.prev_mid);
        double gain = std::max(0.0, diff);
        double loss = std::max(0.0, -diff);
        double a = 1.0 / static_cast<double>(std::max(2, cfg.rsi_period));

        if (ind.warm < cfg.rsi_period) {
            ind.avg_gain += gain;
            ind.avg_loss += loss;
            ++ind.warm;
            if (ind.warm == cfg.rsi_period) {
                ind.avg_gain /= static_cast<double>(cfg.rsi_period);
                ind.avg_loss /= static_cast<double>(cfg.rsi_period);
            }
        } else {
            ind.avg_gain = ind.avg_gain * (1.0 - a) + gain * a;
            ind.avg_loss = ind.avg_loss * (1.0 - a) + loss * a;
        }

        if (ind.avg_loss <= 0.0000001) ind.rsi = 100.0;
        else {
            double rs = ind.avg_gain / ind.avg_loss;
            ind.rsi = 100.0 - 100.0 / (1.0 + rs);
        }

        ind.atr_points = ind.atr_points * 0.88 + std::abs(diff) * 0.12;

        range_win.push_back({t.ts_ms, t.mid});
        while (!range_win.empty() && range_win.front().ts_ms < t.ts_ms - cfg.range_window_ms) range_win.pop_front();
        double lo = t.mid, hi = t.mid;
        for (size_t i = 0; i < range_win.size(); ++i) { lo = std::min(lo, range_win[i].mid); hi = std::max(hi, range_win[i].mid); }
        ind.range_points = points(hi - lo);

        vel_win.push_back({t.ts_ms, t.mid});
        while (!vel_win.empty() && vel_win.front().ts_ms < t.ts_ms - cfg.velocity_window_ms) vel_win.pop_front();
        ind.velocity_points = vel_win.empty() ? 0.0 : points(t.mid - vel_win.front().mid);

        double l2_dir = 0.0;
        if (t.has_l2) {
            l2_dir = (t.l2_imb - 0.5) * 2.0;
            l2_dir += (t.micro_edge - 0.5) * 1.0;
            l2_dir += std::max(-1.0, std::min(1.0, t.ewm_drift)) * 0.35;
        }
        double vel_dir = ind.velocity_points / std::max(1.0, cfg.min_velocity_points);
        vel_dir = std::max(-2.0, std::min(2.0, vel_dir));
        double rsi_dir = (ind.rsi - 50.0) / 50.0;
        ind.flow_score = 0.55 * vel_dir + 0.25 * rsi_dir + 0.20 * l2_dir;

        ind.prev_mid = t.mid;
        if (ind.warm >= cfg.rsi_period && range_win.size() >= 10 && vel_win.size() >= 2) ind.ready = true;
    }

    bool l2_quality_ok(const Tick& t) const
    {
        if (cfg.require_l2 && !t.has_l2) return false;
        if (!t.has_l2) return !cfg.require_l2;
        if (cfg.block_watchdog_dead && t.watchdog_dead != 0) return false;
        if (t.depth_events_total < cfg.min_depth_events_total) return false;
        if (t.vpin > cfg.max_vpin) return false;
        if (t.vol_ratio < cfg.min_vol_ratio || t.vol_ratio > cfg.max_vol_ratio) return false;
        return true;
    }

    // FIX #1 + #5: pre-trade cost gate + danger spread gate at ENTRY.
    bool cost_gate_pass(const Tick& t) const
    {
        if (!cfg.enforce_cost_gate) return true;
        const double sp = spread_points(t);
        // Don't even consider opening when spread is in the danger zone.
        if (sp >= cfg.danger_spread_points) return false;
        // Prospective TP must cover round-trip + slip + buffer.
        const double round_trip =
            sp                          // pay spread on entry
            + sp                        // pay spread again on exit (worst case)
            + cfg.slippage_points
            + cfg.cost_buffer_points;
        if (cfg.hard_profit_take_points < round_trip) return false;
        // ATR scaled vs spread -- catches the "ATR=0.8 vs sp=22" pathology.
        if (cfg.atr_vs_spread_min > 0.0
            && ind.atr_points < sp * cfg.atr_vs_spread_min) return false;
        return true;
    }

    bool hour_allowed(const Tick& t) const
    {
        if (cfg.allow_hour_utc_start < 0 || cfg.allow_hour_utc_end < 0) return true;
        std::time_t sec = static_cast<std::time_t>(t.ts_ms / 1000);
        std::tm* g = std::gmtime(&sec);
        if (!g) return true;
        const int h = g->tm_hour;
        const int s = cfg.allow_hour_utc_start;
        const int e = cfg.allow_hour_utc_end;
        if (s == e) return false;
        if (s < e) return (h >= s && h < e);
        // wrap: e.g. start=22, end=6 -> allow 22,23,0,1,2,3,4,5
        return (h >= s || h < e);
    }

    bool general_filters(const Tick& t) const
    {
        if (!hour_allowed(t)) return false;
        double sp = spread_points(t);
        if (sp <= 0.0 || sp > cfg.max_spread_points) return false;
        if (sp >= cfg.danger_spread_points) return false;          // entry-side danger
        if (ind.range_points < cfg.min_range_points) return false;
        if (ind.atr_points < cfg.min_atr_points) return false;
        if (!l2_quality_ok(t)) return false;
        return true;
    }

    bool l2_allows_long(const Tick& t) const
    {
        if (!cfg.use_l2_entry_filter || !t.has_l2) return true;
        if (t.l2_imb < cfg.long_l2_imb_floor) return false;
        if (t.ewm_drift < cfg.ewm_drift_block_long) return false;
        if (t.micro_edge < cfg.micro_edge_block_long) return false;
        return true;
    }

    bool l2_allows_short(const Tick& t) const
    {
        if (!cfg.use_l2_entry_filter || !t.has_l2) return true;
        if (t.l2_imb > cfg.short_l2_imb_ceiling) return false;
        if (t.ewm_drift > cfg.ewm_drift_block_short) return false;
        if (t.micro_edge > cfg.micro_edge_block_short) return false;
        return true;
    }

    Side signal(const Tick& t) const
    {
        if (!general_filters(t)) return Side::NONE;
        bool long_ok = cfg.allow_long
            && ind.velocity_points >= cfg.min_velocity_points
            && ind.rsi >= cfg.rsi_long
            && ind.flow_score > 0.45
            && l2_allows_long(t);
        bool short_ok = cfg.allow_short
            && ind.velocity_points <= -cfg.min_velocity_points
            && ind.rsi <= cfg.rsi_short
            && ind.flow_score < -0.45
            && l2_allows_short(t);
        if (long_ok && !short_ok) return Side::LONG;
        if (short_ok && !long_ok) return Side::SHORT;
        return Side::NONE;
    }

    bool signal_still_valid(const Tick& t, Side s) const
    {
        if (!general_filters(t)) return false;
        if (s == Side::LONG) return ind.velocity_points >= cfg.min_velocity_points * 0.65 && ind.rsi >= 54.0 && ind.flow_score > 0.20 && l2_allows_long(t);
        if (s == Side::SHORT) return ind.velocity_points <= -cfg.min_velocity_points * 0.65 && ind.rsi <= 46.0 && ind.flow_score < -0.20 && l2_allows_short(t);
        return false;
    }

    void manage_entry(const Tick& t)
    {
        // FIX #7: session cap.
        if (trade_log.size() >= static_cast<size_t>(cfg.max_trades)) return;
        if (trade_log.size() >= static_cast<size_t>(cfg.max_session_trades)) {
            session_cap_hit = true;
            return;
        }

        if (state == State::NO_TRADE) {
            Side s = signal(t);
            if (s != Side::NONE) { armed_side = s; armed_ts_ms = t.ts_ms; state = State::ARMING; }
            return;
        }

        if (state == State::ARMING) {
            if (!signal_still_valid(t, armed_side)) { armed_side = Side::NONE; armed_ts_ms = 0; state = State::NO_TRADE; return; }
            double age = static_cast<double>(t.ts_ms - armed_ts_ms) / 1000.0;
            if (age >= cfg.arm_seconds) {
                // FIX #1: cost gate final check before commit.
                if (!cost_gate_pass(t)) {
                    ++const_cast<XauScalper*>(this)->cost_gate_rejects;
                    armed_side = Side::NONE;
                    armed_ts_ms = 0;
                    state = State::NO_TRADE;
                    return;
                }
                open_position(t, armed_side);
            }
        }
    }

    void open_position(const Tick& t, Side s)
    {
        in_position = true;
        pos_side = s;
        entry_ts_ms = t.ts_ms;
        entry_price = entry_px(t, s);
        entry_spread_points = spread_points(t);
        best_exit_price = exit_px(t, s);
        mfe_points = 0.0;
        mae_points = 0.0;
        entry_l2_imb = t.l2_imb;
        entry_ewm_drift = t.ewm_drift;
        stop_price = s == Side::LONG ? entry_price - cfg.emergency_loss_points * cfg.point_size : entry_price + cfg.emergency_loss_points * cfg.point_size;
        state = State::COST_RECOVERY;
        armed_side = Side::NONE;
        armed_ts_ms = 0;
    }

    bool l2_exit_votes(const Tick& t) const
    {
        if (!cfg.use_l2_exit_filter || !t.has_l2) return false;
        int votes = 0;
        if (cfg.block_watchdog_dead && t.watchdog_dead != 0) ++votes;
        if (spread_points(t) >= cfg.danger_spread_points) ++votes;
        if (pos_side == Side::LONG) {
            if (t.l2_imb < cfg.long_exit_l2_imb) ++votes;
            if (t.ewm_drift < cfg.ewm_drift_exit_long) ++votes;
            if (t.micro_edge < cfg.micro_edge_exit_long) ++votes;
        } else if (pos_side == Side::SHORT) {
            if (t.l2_imb > cfg.short_exit_l2_imb) ++votes;
            if (t.ewm_drift > cfg.ewm_drift_exit_short) ++votes;
            if (t.micro_edge > cfg.micro_edge_exit_short) ++votes;
        }
        return votes >= 2;
    }

    bool reversal_against(const Tick& t) const
    {
        int votes = 0;
        if (pos_side == Side::LONG) {
            if (ind.velocity_points <= -cfg.min_velocity_points * 0.35) ++votes;
            if (ind.rsi < 52.0) ++votes;
            if (ind.flow_score < -0.10) ++votes;
            if (l2_exit_votes(t)) ++votes;
        } else if (pos_side == Side::SHORT) {
            if (ind.velocity_points >= cfg.min_velocity_points * 0.35) ++votes;
            if (ind.rsi > 48.0) ++votes;
            if (ind.flow_score > 0.10) ++votes;
            if (l2_exit_votes(t)) ++votes;
        }
        return votes >= 2;
    }

    void manage_position(const Tick& t)
    {
        double pnl = pnl_now(t);
        mfe_points = std::max(mfe_points, pnl);
        mae_points = std::min(mae_points, pnl);
        double xp = exit_px(t, pos_side);
        if (pos_side == Side::LONG) best_exit_price = std::max(best_exit_price, xp);
        else best_exit_price = std::min(best_exit_price, xp);

        if (pnl <= -cfg.emergency_loss_points) { close_position(t, "EMERGENCY_STOP"); return; }

        double age = static_cast<double>(t.ts_ms - entry_ts_ms) / 1000.0;
        if (age >= cfg.early_fail_seconds && mfe_points < cfg.early_fail_max_profit_points && pnl <= 0.0) {
            close_position(t, "EARLY_FAIL_NO_COST_RECOVERY"); return;
        }

        bool rev = reversal_against(t);
        bool l2_bad = l2_exit_votes(t);

        if (state == State::COST_RECOVERY && pnl >= cfg.be_trigger_points) {
            stop_price = pos_side == Side::LONG ? entry_price + cfg.be_lock_points * cfg.point_size : entry_price - cfg.be_lock_points * cfg.point_size;
            state = State::BREAKEVEN_LOCKED;
        }

        if (state == State::BREAKEVEN_LOCKED && pnl >= cfg.profit_trail_trigger_points) state = State::PROFIT_TRAIL;

        if (state == State::PROFIT_TRAIL) {
            double trail = (rev || l2_bad) ? cfg.tight_trail_points : cfg.loose_trail_points;
            if (pos_side == Side::LONG) stop_price = std::max(stop_price, best_exit_price - trail * cfg.point_size);
            else stop_price = std::min(stop_price, best_exit_price + trail * cfg.point_size);
        }

        if ((rev || l2_bad) && pnl >= cfg.reversal_exit_min_profit_points) { close_position(t, l2_bad ? "L2_REVERSAL_PROFIT_EXIT" : "FLOW_REVERSAL_PROFIT_EXIT"); return; }
        if (pnl >= cfg.hard_profit_take_points) { close_position(t, "HARD_PROFIT_TAKE"); return; }

        if (pos_side == Side::LONG && xp <= stop_price) { close_position(t, state == State::PROFIT_TRAIL ? "TRAIL_STOP" : "BREAKEVEN_OR_INITIAL_STOP"); return; }
        if (pos_side == Side::SHORT && xp >= stop_price) { close_position(t, state == State::PROFIT_TRAIL ? "TRAIL_STOP" : "BREAKEVEN_OR_INITIAL_STOP"); return; }
    }

    void close_position(const Tick& t, const string& reason)
    {
        Trade tr;
        tr.id = ++trade_id;
        tr.side = pos_side;
        tr.entry_ts_ms = entry_ts_ms;
        tr.exit_ts_ms = t.ts_ms;
        tr.entry_price = entry_price;
        tr.entry_spread_points = entry_spread_points;
        tr.exit_price = exit_px(t, pos_side);
        tr.pnl_points = pnl_now(t);
        tr.mfe_points = mfe_points;
        tr.mae_points = mae_points;
        tr.entry_l2_imb = entry_l2_imb;
        tr.exit_l2_imb = t.l2_imb;
        tr.entry_ewm_drift = entry_ewm_drift;
        tr.exit_ewm_drift = t.ewm_drift;
        tr.reason = reason;
        trade_log.push_back(tr);

        // FIX #6: consecutive-loss circuit breaker.
        if (tr.pnl_points < 0.0) {
            ++consec_losses;
            if (cfg.consec_loss_breaker > 0
                && consec_losses >= cfg.consec_loss_breaker) {
                cooldown_until_ms = t.ts_ms
                    + static_cast<int64_t>(cfg.consec_loss_pause_sec * 1000.0);
                consec_losses = 0;
                ++circuit_breaker_trips;
            } else {
                cooldown_until_ms = t.ts_ms
                    + static_cast<int64_t>(cfg.cooldown_seconds * 1000.0);
            }
        } else if (tr.pnl_points > 0.0) {
            consec_losses = 0;
            cooldown_until_ms = t.ts_ms
                + static_cast<int64_t>(cfg.cooldown_seconds * 1000.0);
        } else {
            cooldown_until_ms = t.ts_ms
                + static_cast<int64_t>(cfg.cooldown_seconds * 1000.0);
        }

        in_position = false;
        pos_side = Side::NONE;
        entry_ts_ms = 0;
        entry_price = 0.0;
        stop_price = 0.0;
        state = State::COOLDOWN;
    }
};

static void write_trades(const string& path, const vector<Trade>& trades)
{
    ofstream out(path.c_str(), std::ios::out | std::ios::trunc);
    if (!out) throw std::runtime_error("Could not open output file: " + path);
    out << "id,side,entry_ts_ms,exit_ts_ms,duration_sec,entry_price,exit_price,entry_spread_pts,pnl_points,mfe_points,mae_points,entry_l2_imb,exit_l2_imb,entry_ewm_drift,exit_ewm_drift,reason\n";
    for (size_t i = 0; i < trades.size(); ++i) {
        const Trade& tr = trades[i];
        double dur = static_cast<double>(tr.exit_ts_ms - tr.entry_ts_ms) / 1000.0;
        out << tr.id << "," << side_text(tr.side) << "," << tr.entry_ts_ms << "," << tr.exit_ts_ms << ","
            << fixed << setprecision(3) << dur << ","
            << fixed << setprecision(5) << tr.entry_price << "," << tr.exit_price << ","
            << fixed << setprecision(2) << tr.entry_spread_points << ","
            << fixed << setprecision(2) << tr.pnl_points << "," << tr.mfe_points << "," << tr.mae_points << ","
            << fixed << setprecision(4) << tr.entry_l2_imb << "," << tr.exit_l2_imb << ","
            << fixed << setprecision(4) << tr.entry_ewm_drift << "," << tr.exit_ewm_drift << ","
            << tr.reason << "\n";
    }
}

static void print_usage()
{
    cout << "Usage:\n";
    cout << "  XauScalper input.csv output_trades.csv [--flag value]...\n";
    cout << "\nKey flags:\n";
    cout << "  --require-l2 1                  -- reject ticks without L2\n";
    cout << "  --enforce-cost-gate 0/1         -- pre-trade cost gate (default 1)\n";
    cout << "  --atr-vs-spread-min 1.2         -- atr / spread minimum ratio\n";
    cout << "  --max-spread-points 60          -- absolute spread cap for entry\n";
    cout << "  --danger-spread-points 45       -- entry+exit danger spread\n";
    cout << "  --min-atr-points 25             -- absolute ATR floor\n";
    cout << "  --be-trigger-points 40          -- BE arm threshold\n";
    cout << "  --emergency-loss-points 36      -- hard stop in points\n";
    cout << "  --max-session-trades 100        -- per-session cap\n";
    cout << "  --consec-loss-breaker 5         -- N losses -> long pause\n";
    cout << "  --consec-loss-pause-sec 600     -- pause length\n";
    cout << "  --allow-hour-utc-start 7        -- session-hour filter start (UTC)\n";
    cout << "  --allow-hour-utc-end 21         -- session-hour filter end (UTC); both -1 disables\n";
}

static bool parse_bool(const string& s)
{
    string x = lower_copy(s);
    return x == "1" || x == "true" || x == "yes" || x == "on";
}

static void apply_arg(Config& cfg, const string& k, const string& v)
{
    double d = 0.0;
    int64_t n = 0;
    if (k == "--point-size" && to_double(v, d)) cfg.point_size = d;
    else if (k == "--max-spread-points" && to_double(v, d)) cfg.max_spread_points = d;
    else if (k == "--danger-spread-points" && to_double(v, d)) cfg.danger_spread_points = d;
    else if (k == "--atr-vs-spread-min" && to_double(v, d)) cfg.atr_vs_spread_min = d;
    else if (k == "--slippage-points" && to_double(v, d)) cfg.slippage_points = d;
    else if (k == "--cost-buffer-points" && to_double(v, d)) cfg.cost_buffer_points = d;
    else if (k == "--enforce-cost-gate") cfg.enforce_cost_gate = parse_bool(v);
    else if (k == "--min-range-points" && to_double(v, d)) cfg.min_range_points = d;
    else if (k == "--min-velocity-points" && to_double(v, d)) cfg.min_velocity_points = d;
    else if (k == "--min-atr-points" && to_double(v, d)) cfg.min_atr_points = d;
    else if (k == "--rsi-long" && to_double(v, d)) cfg.rsi_long = d;
    else if (k == "--rsi-short" && to_double(v, d)) cfg.rsi_short = d;
    else if (k == "--rsi-period" && to_i64(v, n)) cfg.rsi_period = static_cast<int>(n);
    else if (k == "--range-window-ms" && to_i64(v, n)) cfg.range_window_ms = static_cast<int>(n);
    else if (k == "--velocity-window-ms" && to_i64(v, n)) cfg.velocity_window_ms = static_cast<int>(n);
    else if (k == "--arm-seconds" && to_double(v, d)) cfg.arm_seconds = d;
    else if (k == "--cooldown-seconds" && to_double(v, d)) cfg.cooldown_seconds = d;
    else if (k == "--emergency-loss-points" && to_double(v, d)) cfg.emergency_loss_points = d;
    else if (k == "--be-trigger-points" && to_double(v, d)) cfg.be_trigger_points = d;
    else if (k == "--be-lock-points" && to_double(v, d)) cfg.be_lock_points = d;
    else if (k == "--profit-trail-trigger-points" && to_double(v, d)) cfg.profit_trail_trigger_points = d;
    else if (k == "--loose-trail-points" && to_double(v, d)) cfg.loose_trail_points = d;
    else if (k == "--tight-trail-points" && to_double(v, d)) cfg.tight_trail_points = d;
    else if (k == "--reversal-exit-min-profit-points" && to_double(v, d)) cfg.reversal_exit_min_profit_points = d;
    else if (k == "--hard-profit-take-points" && to_double(v, d)) cfg.hard_profit_take_points = d;
    else if (k == "--long-l2-imb-floor" && to_double(v, d)) cfg.long_l2_imb_floor = d;
    else if (k == "--short-l2-imb-ceiling" && to_double(v, d)) cfg.short_l2_imb_ceiling = d;
    else if (k == "--long-exit-l2-imb" && to_double(v, d)) cfg.long_exit_l2_imb = d;
    else if (k == "--short-exit-l2-imb" && to_double(v, d)) cfg.short_exit_l2_imb = d;
    else if (k == "--ewm-drift-block-long" && to_double(v, d)) cfg.ewm_drift_block_long = d;
    else if (k == "--ewm-drift-block-short" && to_double(v, d)) cfg.ewm_drift_block_short = d;
    else if (k == "--ewm-drift-exit-long" && to_double(v, d)) cfg.ewm_drift_exit_long = d;
    else if (k == "--ewm-drift-exit-short" && to_double(v, d)) cfg.ewm_drift_exit_short = d;
    else if (k == "--micro-edge-block-long" && to_double(v, d)) cfg.micro_edge_block_long = d;
    else if (k == "--micro-edge-block-short" && to_double(v, d)) cfg.micro_edge_block_short = d;
    else if (k == "--micro-edge-exit-long" && to_double(v, d)) cfg.micro_edge_exit_long = d;
    else if (k == "--micro-edge-exit-short" && to_double(v, d)) cfg.micro_edge_exit_short = d;
    else if (k == "--max-vpin" && to_double(v, d)) cfg.max_vpin = d;
    else if (k == "--min-vol-ratio" && to_double(v, d)) cfg.min_vol_ratio = d;
    else if (k == "--max-vol-ratio" && to_double(v, d)) cfg.max_vol_ratio = d;
    else if (k == "--min-depth-events-total" && to_i64(v, n)) cfg.min_depth_events_total = static_cast<int>(n);
    else if (k == "--max-trades" && to_i64(v, n)) cfg.max_trades = static_cast<int>(n);
    else if (k == "--max-session-trades" && to_i64(v, n)) cfg.max_session_trades = static_cast<int>(n);
    else if (k == "--consec-loss-breaker" && to_i64(v, n)) cfg.consec_loss_breaker = static_cast<int>(n);
    else if (k == "--consec-loss-pause-sec" && to_double(v, d)) cfg.consec_loss_pause_sec = d;
    else if (k == "--allow-hour-utc-start" && to_i64(v, n)) cfg.allow_hour_utc_start = static_cast<int>(n);
    else if (k == "--allow-hour-utc-end"   && to_i64(v, n)) cfg.allow_hour_utc_end   = static_cast<int>(n);
    else if (k == "--require-l2") cfg.require_l2 = parse_bool(v);
    else if (k == "--block-watchdog-dead") cfg.block_watchdog_dead = parse_bool(v);
    else if (k == "--use-l2-entry-filter") cfg.use_l2_entry_filter = parse_bool(v);
    else if (k == "--use-l2-exit-filter") cfg.use_l2_exit_filter = parse_bool(v);
    else if (k == "--allow-long") cfg.allow_long = parse_bool(v);
    else if (k == "--allow-short") cfg.allow_short = parse_bool(v);
    else if (k == "--tag") { /* handled in main */ }
}

static void print_config(const Config& cfg, const string& tag)
{
    cout << "XauScalper v3 config [" << tag << "]\n";
    cout << "  enforce_cost_gate=" << (cfg.enforce_cost_gate ? 1 : 0) << "\n";
    cout << "  atr_vs_spread_min=" << cfg.atr_vs_spread_min << "\n";
    cout << "  min_atr_points=" << cfg.min_atr_points << "\n";
    cout << "  max_spread_points=" << cfg.max_spread_points
         << "  danger_spread_points=" << cfg.danger_spread_points << "\n";
    cout << "  emergency_loss_points=" << cfg.emergency_loss_points
         << "  be_trigger_points=" << cfg.be_trigger_points
         << "  hard_profit_take_points=" << cfg.hard_profit_take_points << "\n";
    cout << "  max_session_trades=" << cfg.max_session_trades
         << "  consec_loss_breaker=" << cfg.consec_loss_breaker
         << "  pause_sec=" << cfg.consec_loss_pause_sec << "\n";
    cout << "  require_l2=" << (cfg.require_l2 ? 1 : 0)
         << "  use_l2_entry_filter=" << (cfg.use_l2_entry_filter ? 1 : 0)
         << "  use_l2_exit_filter=" << (cfg.use_l2_exit_filter ? 1 : 0) << "\n\n";
}

int main(int argc, char** argv)
{
    if (argc < 3) { print_usage(); return 1; }
    string input = argv[1];
    string output = argv[2];
    string tag = "run";

    Config cfg;
    for (int i = 3; i + 1 < argc; i += 2) {
        if (string(argv[i]) == "--tag") tag = argv[i + 1];
        apply_arg(cfg, argv[i], argv[i + 1]);
    }
    if (cfg.point_size <= 0.0) { cerr << "Invalid point_size\n"; return 1; }
    print_config(cfg, tag);

    ifstream in(input.c_str());
    if (!in) { cerr << "Could not open input file: " << input << "\n"; return 1; }

    string line;
    vector<string> first;
    while (std::getline(in, line)) {
        if (!trim(line).empty()) { first = split_csv(line); break; }
    }
    if (first.empty()) { cerr << "Input file is empty\n"; return 1; }

    Layout layout = detect_layout(first);
    if (layout.bid < 0 || layout.ask < 0) { cerr << "Could not detect bid/ask columns\n"; return 1; }

    cerr << "[layout] header=" << (layout.has_header ? 1 : 0)
         << " ts=" << layout.ts_ms
         << " bid=" << layout.bid << " ask=" << layout.ask
         << " imb=" << layout.l2_imb << " bv=" << layout.l2_bid_vol
         << " av=" << layout.l2_ask_vol << " micro=" << layout.micro_edge
         << " drift=" << layout.ewm_drift << "\n";

    XauScalper engine(cfg);
    int64_t raw = 0, parsed = 0, rejected = 0;

    if (!layout.has_header) {
        Tick t;
        ++raw;
        if (parse_tick(first, layout, t)) { engine.on_tick(t); ++parsed; }
        else ++rejected;
    }

    while (std::getline(in, line)) {
        if (trim(line).empty()) continue;
        ++raw;
        vector<string> row = split_csv(line);
        Tick t;
        if (!parse_tick(row, layout, t)) { ++rejected; continue; }
        engine.on_tick(t);
        ++parsed;
    }

    write_trades(output, engine.trades());
    cerr << "raw=" << raw << " parsed=" << parsed << " rejected=" << rejected << "\n";
    engine.summary();
    return 0;
}
