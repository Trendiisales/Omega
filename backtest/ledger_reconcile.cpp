// =============================================================================
// ledger_reconcile.cpp -- Omega vs cTrader trade-by-trade reconciliation tool
// =============================================================================
//
// 2026-05-11 S33 (Claude / Jo): Standalone C++ binary that diffs Omega's
//   per-day trade ledger against the broker's (cTrader) account ledger
//   export, producing a per-trade gap report. Built in response to the
//   May-8 NZ$310 live bleed, where the Omega GUI reported small losses
//   while cTrader actually executed materially worse fills + orphan legs.
//
//   This tool is the *measurement* layer. It does NOT modify the engine.
//   The actual fix (wiring broker_* TradeRecord fields into the persisted
//   CSV writer in omega_main.hpp:281) lives in protected core files and
//   needs explicit operator sign-off (rule 3 / 4 from HANDOFF_S32).
//
// USAGE
//   backtest/ledger_reconcile
//       --omega    C:/Omega/logs/trades/omega_trade_closes_2026-05-08.csv
//       --ctrader  ~/Downloads/ctrader_account_8077780_2026-05-08.csv
//       [--symbol XAUUSD]
//       [--account 8077780]
//       [--out backtest/reconcile_2026-05-08.csv]
//       [--match-window-sec 5]
//       [--commission-per-rt 0.06]
//       [--verbose]
//
// INPUTS
//
//   1. Omega trade CSV (from omega_main.hpp:281 schema):
//        trade_id,trade_ref,entry_ts_unix,entry_ts_utc,entry_utc_weekday,
//        exit_ts_unix,exit_ts_utc,exit_utc_weekday,symbol,engine,side,
//        entry_px,exit_px,tp,sl,size,gross_pnl,net_pnl,
//        slippage_entry,slippage_exit,commission,
//        slip_entry_pct,slip_exit_pct,comm_per_side,
//        mfe,mae,hold_sec,spread_at_entry,
//        latency_ms,regime,exit_reason,l2_imbalance,l2_live
//      Resolved by header name -- order-tolerant.
//
//   2. cTrader account ledger CSV (Spotware export format, broadly):
//        Order ID, Symbol, Side, Volume, Entry Price, Entry Time,
//        Exit Price, Exit Time, Net P&L, Commission, Swap, Comment, ...
//      cTrader exports vary by region/version. The tool resolves the
//      following columns by case-insensitive header substring match:
//        symbol  : "symbol"
//        side    : "side" / "direction" / "type"
//        volume  : "volume" / "lot" / "size"
//        entry_px: "entry price" / "open price"
//        entry_ts: "entry time" / "open time"  (parsed flexibly)
//        exit_px : "exit price" / "close price"
//        exit_ts : "exit time"  / "close time"
//        net_pnl : "net p&l" / "net pnl" / "profit"
//        comm    : "commission" / "commissions"
//      If your cTrader CSV has different column names, pass --map-col
//      OMEGA_NAME=CTRADER_SUBSTRING (repeatable) to override.
//
// MATCHING
//   For each Omega trade, find the cTrader trade with:
//     - same symbol (after stripping ".F" / ".CASH" suffix variants)
//     - same side (LONG/BUY  or  SHORT/SELL, case-insensitive)
//     - entry timestamp within --match-window-sec of the Omega entry
//   If multiple cTrader trades match, take the closest by entry time.
//   Unmatched Omega trades are flagged "ORPHAN_OMEGA" -- engine claims a
//     trade the broker has no record of (close-leg fail or pure paper).
//   Unmatched cTrader trades are flagged "ORPHAN_BROKER" -- broker
//     executed something Omega never recorded (entry-leg ack lost,
//     manual trade, or a phantom position from a prior session).
//
// OUTPUT
//   --out CSV with one row per matched pair plus orphan rows. Columns:
//     status, omega_trade_id, ctrader_order_id, symbol, side,
//     omega_entry_ts, ctrader_entry_ts, entry_ts_delta_sec,
//     omega_entry_px, ctrader_entry_px, entry_px_delta,
//     omega_exit_ts, ctrader_exit_ts, exit_ts_delta_sec,
//     omega_exit_px, ctrader_exit_px, exit_px_delta,
//     omega_size, ctrader_size,
//     omega_net_pnl, ctrader_net_pnl, pnl_delta,
//     omega_commission, ctrader_commission, comm_delta,
//     omega_exit_reason, notes
//
// SUMMARY (printed to stdout)
//   N matched pairs
//   Sum of pnl_delta (positive = Omega over-reported profit / under-reported loss)
//   Mean / median / p95 of |entry_px_delta|, |exit_px_delta|, |pnl_delta|
//   Orphan counts (Omega side, broker side)
//   Per-exit-reason breakdown of pnl_delta (so we can tell whether SL_HIT,
//     TP_HIT, BE_HIT, or MAX_HOLD_EXIT trades carry the worst gap)
//
// BUILD
//   clang++ -std=c++17 -O2 -Wall -Wextra
//       -I include backtest/ledger_reconcile.cpp -o backtest/ledger_reconcile
//
// EXIT CODES
//   0  : reconciliation completed (gap may be non-zero -- inspect output)
//   2  : usage error / unreadable input
//   3  : at least one input has no parseable rows
// =============================================================================

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// String helpers
// ---------------------------------------------------------------------------
std::string lower_copy(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s) r.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return r;
}

std::string trim_copy(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::vector<std::string> split_csv_row(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool in_quotes = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '"') {
            if (in_quotes && i + 1 < line.size() && line[i + 1] == '"') {
                cur.push_back('"');
                ++i;
            } else {
                in_quotes = !in_quotes;
            }
        } else if (c == ',' && !in_quotes) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

std::string normalize_symbol(const std::string& s) {
    std::string r = lower_copy(trim_copy(s));
    // Strip common BlackBull / cTrader suffixes
    const char* suffixes[] = { ".f", ".cash", ".cfd", ".m", ".pro", ".raw" };
    for (const char* suf : suffixes) {
        const size_t L = std::strlen(suf);
        if (r.size() > L && r.compare(r.size() - L, L, suf) == 0) {
            r.erase(r.size() - L);
        }
    }
    return r;
}

std::string normalize_side(const std::string& s) {
    std::string r = lower_copy(trim_copy(s));
    if (r == "buy" || r == "long" || r == "b" || r == "1") return "long";
    if (r == "sell" || r == "short" || r == "s" || r == "-1") return "short";
    return r;
}

double parse_double_safe(const std::string& s) {
    const std::string t = trim_copy(s);
    if (t.empty()) return 0.0;
    try { return std::stod(t); } catch (...) { return 0.0; }
}

int64_t parse_int64_safe(const std::string& s) {
    const std::string t = trim_copy(s);
    if (t.empty()) return 0;
    try { return std::stoll(t); } catch (...) { return 0; }
}

// Parse an ISO-ish or cTrader-style timestamp into unix seconds (UTC).
// Accepts:
//   "2026-05-08T14:30:25Z"
//   "2026-05-08 14:30:25"
//   "2026-05-08 14:30:25.123"
//   "08/05/2026 14:30:25"   (cTrader European)
//   "05/08/2026 14:30:25"   (cTrader US)            -- ambiguous -> assume DD/MM
//   pure unix seconds       (e.g. "1746716425")
int64_t parse_ts_to_unix(const std::string& s_in) {
    std::string s = trim_copy(s_in);
    if (s.empty()) return 0;
    // Pure integer => unix seconds (or millis if too big).
    bool all_digits = true;
    for (char c : s) if (!std::isdigit(static_cast<unsigned char>(c))) { all_digits = false; break; }
    if (all_digits) {
        int64_t v = parse_int64_safe(s);
        if (v > 1'000'000'000'000LL) v /= 1000; // millis -> sec
        return v;
    }
    std::tm tm_v{};
    int Y=0, M=0, D=0, h=0, m=0, sec=0;
    auto try_parse = [&](const char* fmt) -> bool {
        if (std::sscanf(s.c_str(), fmt, &Y, &M, &D, &h, &m, &sec) >= 5) {
            tm_v.tm_year = Y - 1900;
            tm_v.tm_mon  = M - 1;
            tm_v.tm_mday = D;
            tm_v.tm_hour = h;
            tm_v.tm_min  = m;
            tm_v.tm_sec  = sec;
            return true;
        }
        return false;
    };
    bool ok = false;
    // ISO with T
    ok = ok || try_parse("%d-%d-%dT%d:%d:%d");
    // ISO with space
    ok = ok || try_parse("%d-%d-%d %d:%d:%d");
    // cTrader European DD/MM/YYYY
    if (!ok) {
        if (std::sscanf(s.c_str(), "%d/%d/%d %d:%d:%d", &D, &M, &Y, &h, &m, &sec) >= 5) {
            tm_v.tm_year = Y - 1900; tm_v.tm_mon = M - 1; tm_v.tm_mday = D;
            tm_v.tm_hour = h; tm_v.tm_min = m; tm_v.tm_sec = sec;
            ok = true;
        }
    }
    if (!ok) return 0;
#ifdef _WIN32
    return _mkgmtime(&tm_v);
#else
    return timegm(&tm_v);
#endif
}

// Map header -> column index, lowercased + trimmed for lookup.
struct HeaderMap {
    std::unordered_map<std::string, int> idx;

    void build(const std::vector<std::string>& cols) {
        for (size_t i = 0; i < cols.size(); ++i) {
            idx[lower_copy(trim_copy(cols[i]))] = static_cast<int>(i);
        }
    }
    // Exact-name lookup (Omega CSV).
    int get_exact(const std::string& name) const {
        auto it = idx.find(lower_copy(name));
        return (it != idx.end()) ? it->second : -1;
    }
    // Substring lookup (cTrader CSV -- column names vary).
    int get_substr(const std::vector<std::string>& candidates) const {
        for (const auto& cand : candidates) {
            const std::string c = lower_copy(cand);
            for (const auto& [k, v] : idx) {
                if (k.find(c) != std::string::npos) return v;
            }
        }
        return -1;
    }
};

// ---------------------------------------------------------------------------
// Trade records
// ---------------------------------------------------------------------------
struct OmegaTrade {
    int    trade_id   = 0;
    std::string symbol;
    std::string side;     // normalized "long"/"short"
    int64_t entry_ts  = 0;
    int64_t exit_ts   = 0;
    double  entry_px  = 0;
    double  exit_px   = 0;
    double  size      = 0;
    double  gross_pnl = 0;
    double  net_pnl   = 0;
    double  commission = 0;
    double  spread_at_entry = 0;
    std::string exit_reason;
    std::string engine;
};

struct CtraderTrade {
    std::string order_id;
    std::string symbol;
    std::string side;     // normalized
    int64_t entry_ts  = 0;
    int64_t exit_ts   = 0;
    double  entry_px  = 0;
    double  exit_px   = 0;
    double  size      = 0;
    double  net_pnl   = 0;
    double  commission = 0;
    bool    matched   = false;
};

// ---------------------------------------------------------------------------
// CSV loaders
// ---------------------------------------------------------------------------
std::vector<OmegaTrade> load_omega_csv(const std::string& path,
                                       const std::string& filter_symbol,
                                       std::string& err) {
    std::vector<OmegaTrade> out;
    std::ifstream f(path);
    if (!f.is_open()) { err = "cannot open " + path; return out; }
    std::string header_line;
    if (!std::getline(f, header_line)) { err = "empty file: " + path; return out; }
    HeaderMap h;
    h.build(split_csv_row(header_line));
    const int c_id    = h.get_exact("trade_id");
    const int c_sym   = h.get_exact("symbol");
    const int c_side  = h.get_exact("side");
    const int c_ets   = h.get_exact("entry_ts_unix");
    const int c_xts   = h.get_exact("exit_ts_unix");
    const int c_epx   = h.get_exact("entry_px");
    const int c_xpx   = h.get_exact("exit_px");
    const int c_size  = h.get_exact("size");
    const int c_gpnl  = h.get_exact("gross_pnl");
    const int c_npnl  = h.get_exact("net_pnl");
    const int c_comm  = h.get_exact("commission");
    const int c_spr   = h.get_exact("spread_at_entry");
    const int c_xr    = h.get_exact("exit_reason");
    const int c_eng   = h.get_exact("engine");
    if (c_sym < 0 || c_side < 0 || c_ets < 0 || c_xts < 0 ||
        c_epx < 0 || c_xpx < 0 || c_npnl < 0) {
        err = "Omega CSV missing required columns; found header: " + header_line;
        return out;
    }
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        const auto cols = split_csv_row(line);
        if ((int)cols.size() <= c_npnl) continue;
        OmegaTrade t;
        if (c_id >= 0 && c_id < (int)cols.size()) t.trade_id = (int)parse_int64_safe(cols[c_id]);
        t.symbol     = cols[c_sym];
        t.side       = normalize_side(cols[c_side]);
        t.entry_ts   = parse_int64_safe(cols[c_ets]);
        t.exit_ts    = parse_int64_safe(cols[c_xts]);
        t.entry_px   = parse_double_safe(cols[c_epx]);
        t.exit_px    = parse_double_safe(cols[c_xpx]);
        if (c_size >= 0 && c_size < (int)cols.size()) t.size = parse_double_safe(cols[c_size]);
        if (c_gpnl >= 0 && c_gpnl < (int)cols.size()) t.gross_pnl = parse_double_safe(cols[c_gpnl]);
        t.net_pnl    = parse_double_safe(cols[c_npnl]);
        if (c_comm >= 0 && c_comm < (int)cols.size()) t.commission = parse_double_safe(cols[c_comm]);
        if (c_spr  >= 0 && c_spr  < (int)cols.size()) t.spread_at_entry = parse_double_safe(cols[c_spr]);
        if (c_xr   >= 0 && c_xr   < (int)cols.size()) t.exit_reason = trim_copy(cols[c_xr]);
        if (c_eng  >= 0 && c_eng  < (int)cols.size()) t.engine = trim_copy(cols[c_eng]);
        if (!filter_symbol.empty() &&
            normalize_symbol(t.symbol) != normalize_symbol(filter_symbol)) continue;
        out.push_back(std::move(t));
    }
    return out;
}

std::vector<CtraderTrade> load_ctrader_csv(const std::string& path,
                                           const std::string& filter_symbol,
                                           const std::map<std::string,std::string>& col_overrides,
                                           std::string& err) {
    std::vector<CtraderTrade> out;
    std::ifstream f(path);
    if (!f.is_open()) { err = "cannot open " + path; return out; }
    std::string header_line;
    if (!std::getline(f, header_line)) { err = "empty file: " + path; return out; }
    HeaderMap h;
    h.build(split_csv_row(header_line));
    auto resolve = [&](const std::string& key,
                       const std::vector<std::string>& fallbacks) -> int {
        auto it = col_overrides.find(key);
        if (it != col_overrides.end()) return h.get_substr({ it->second });
        return h.get_substr(fallbacks);
    };
    const int c_id   = resolve("order_id", {"order id", "position id", "deal id", "id"});
    const int c_sym  = resolve("symbol",   {"symbol", "instrument"});
    const int c_side = resolve("side",     {"side", "direction", "type"});
    const int c_vol  = resolve("volume",   {"volume", "lot", "size", "quantity"});
    const int c_epx  = resolve("entry_px", {"entry price", "open price"});
    const int c_ets  = resolve("entry_ts", {"entry time", "open time", "opening time"});
    const int c_xpx  = resolve("exit_px",  {"exit price", "close price", "closing price"});
    const int c_xts  = resolve("exit_ts",  {"exit time", "close time", "closing time"});
    const int c_pnl  = resolve("net_pnl",  {"net p&l", "net pnl", "profit", "p/l", "p&l"});
    const int c_com  = resolve("comm",     {"commission", "commissions", "fee"});
    if (c_sym < 0 || c_side < 0 || c_epx < 0 || c_ets < 0 ||
        c_xpx < 0 || c_xts < 0 || c_pnl < 0) {
        err = "cTrader CSV missing required columns. Headers seen: " + header_line;
        return out;
    }
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        const auto cols = split_csv_row(line);
        const int max_idx = std::max({c_sym, c_side, c_epx, c_ets, c_xpx, c_xts, c_pnl});
        if ((int)cols.size() <= max_idx) continue;
        CtraderTrade t;
        if (c_id   >= 0 && c_id   < (int)cols.size()) t.order_id = trim_copy(cols[c_id]);
        t.symbol   = cols[c_sym];
        t.side     = normalize_side(cols[c_side]);
        t.entry_ts = parse_ts_to_unix(cols[c_ets]);
        t.exit_ts  = parse_ts_to_unix(cols[c_xts]);
        t.entry_px = parse_double_safe(cols[c_epx]);
        t.exit_px  = parse_double_safe(cols[c_xpx]);
        if (c_vol >= 0 && c_vol < (int)cols.size()) t.size = parse_double_safe(cols[c_vol]);
        t.net_pnl  = parse_double_safe(cols[c_pnl]);
        if (c_com >= 0 && c_com < (int)cols.size()) t.commission = parse_double_safe(cols[c_com]);
        if (!filter_symbol.empty() &&
            normalize_symbol(t.symbol) != normalize_symbol(filter_symbol)) continue;
        out.push_back(std::move(t));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Matching
// ---------------------------------------------------------------------------
struct MatchedPair {
    const OmegaTrade*   o = nullptr;   // null => ORPHAN_BROKER
    const CtraderTrade* c = nullptr;   // null => ORPHAN_OMEGA
};

std::vector<MatchedPair> match_trades(std::vector<OmegaTrade>& omega,
                                      std::vector<CtraderTrade>& ctrader,
                                      int match_window_sec) {
    std::vector<MatchedPair> out;
    out.reserve(omega.size() + ctrader.size());
    for (const auto& o : omega) {
        const CtraderTrade* best = nullptr;
        int64_t best_dt = std::numeric_limits<int64_t>::max();
        size_t  best_idx = 0;
        for (size_t i = 0; i < ctrader.size(); ++i) {
            CtraderTrade& c = ctrader[i];
            if (c.matched) continue;
            if (normalize_symbol(c.symbol) != normalize_symbol(o.symbol)) continue;
            if (c.side != o.side) continue;
            const int64_t dt = std::llabs(c.entry_ts - o.entry_ts);
            if (dt > match_window_sec) continue;
            if (dt < best_dt) { best_dt = dt; best = &c; best_idx = i; }
        }
        if (best) {
            ctrader[best_idx].matched = true;
            out.push_back({ &o, &ctrader[best_idx] });
        } else {
            out.push_back({ &o, nullptr });
        }
    }
    for (const auto& c : ctrader) {
        if (!c.matched) out.push_back({ nullptr, &c });
    }
    return out;
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------
std::string fmt_ts(int64_t ts) {
    if (ts <= 0) return "";
    std::time_t t = static_cast<std::time_t>(ts);
    std::tm tm_v{};
#ifdef _WIN32
    gmtime_s(&tm_v, &t);
#else
    gmtime_r(&t, &tm_v);
#endif
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
        tm_v.tm_year + 1900, tm_v.tm_mon + 1, tm_v.tm_mday,
        tm_v.tm_hour, tm_v.tm_min, tm_v.tm_sec);
    return buf;
}

void write_output_csv(const std::string& out_path,
                      const std::vector<MatchedPair>& pairs) {
    std::ofstream f(out_path);
    if (!f.is_open()) {
        std::cerr << "[reconcile] cannot write " << out_path << "\n";
        return;
    }
    f << "status,omega_trade_id,ctrader_order_id,symbol,side,"
         "omega_entry_ts,ctrader_entry_ts,entry_ts_delta_sec,"
         "omega_entry_px,ctrader_entry_px,entry_px_delta,"
         "omega_exit_ts,ctrader_exit_ts,exit_ts_delta_sec,"
         "omega_exit_px,ctrader_exit_px,exit_px_delta,"
         "omega_size,ctrader_size,"
         "omega_net_pnl,ctrader_net_pnl,pnl_delta,"
         "omega_commission,ctrader_commission,comm_delta,"
         "omega_exit_reason,notes\n";
    for (const auto& p : pairs) {
        std::string status;
        if (p.o && p.c) status = "MATCHED";
        else if (p.o)   status = "ORPHAN_OMEGA";
        else            status = "ORPHAN_BROKER";

        const auto Q = [](const std::string& s) -> std::string {
            if (s.find(',') == std::string::npos && s.find('"') == std::string::npos) return s;
            std::string r = "\"";
            for (char c : s) { if (c == '"') r += '"'; r += c; }
            r += '"';
            return r;
        };

        f << status << ","
          << (p.o ? std::to_string(p.o->trade_id) : "") << ","
          << (p.c ? Q(p.c->order_id) : "") << ","
          << (p.o ? Q(p.o->symbol) : (p.c ? Q(p.c->symbol) : "")) << ","
          << (p.o ? p.o->side : (p.c ? p.c->side : "")) << ","
          << (p.o ? fmt_ts(p.o->entry_ts) : "") << ","
          << (p.c ? fmt_ts(p.c->entry_ts) : "") << ","
          << ((p.o && p.c) ? std::to_string(p.c->entry_ts - p.o->entry_ts) : "") << ",";
        f << std::fixed << std::setprecision(5);
        f << (p.o ? p.o->entry_px : 0.0) << ","
          << (p.c ? p.c->entry_px : 0.0) << ","
          << ((p.o && p.c) ? (p.c->entry_px - p.o->entry_px) : 0.0) << ",";
        f << (p.o ? fmt_ts(p.o->exit_ts) : "") << ","
          << (p.c ? fmt_ts(p.c->exit_ts) : "") << ","
          << ((p.o && p.c) ? std::to_string(p.c->exit_ts - p.o->exit_ts) : "") << ",";
        f << (p.o ? p.o->exit_px : 0.0) << ","
          << (p.c ? p.c->exit_px : 0.0) << ","
          << ((p.o && p.c) ? (p.c->exit_px - p.o->exit_px) : 0.0) << ",";
        f << (p.o ? p.o->size : 0.0) << ","
          << (p.c ? p.c->size : 0.0) << ",";
        f << std::setprecision(2);
        f << (p.o ? p.o->net_pnl : 0.0) << ","
          << (p.c ? p.c->net_pnl : 0.0) << ","
          << ((p.o && p.c) ? (p.c->net_pnl - p.o->net_pnl) : 0.0) << ",";
        f << (p.o ? p.o->commission : 0.0) << ","
          << (p.c ? p.c->commission : 0.0) << ","
          << ((p.o && p.c) ? (p.c->commission - p.o->commission) : 0.0) << ",";
        f << (p.o ? Q(p.o->exit_reason) : "") << ",";
        f << "\n";
    }
}

double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double rank = p * (v.size() - 1);
    const size_t lo = static_cast<size_t>(std::floor(rank));
    const size_t hi = static_cast<size_t>(std::ceil(rank));
    if (lo == hi) return v[lo];
    const double frac = rank - lo;
    return v[lo] * (1.0 - frac) + v[hi] * frac;
}

void print_summary(const std::vector<MatchedPair>& pairs) {
    int matched = 0, orphan_o = 0, orphan_c = 0;
    double sum_pnl_delta = 0.0;
    std::vector<double> abs_entry, abs_exit, abs_pnl, abs_comm;
    std::map<std::string, std::pair<int,double>> by_reason; // reason -> (n, sum_delta)
    for (const auto& p : pairs) {
        if (p.o && p.c) {
            ++matched;
            const double dE = std::fabs(p.c->entry_px - p.o->entry_px);
            const double dX = std::fabs(p.c->exit_px  - p.o->exit_px);
            const double dP = p.c->net_pnl - p.o->net_pnl;
            const double dC = std::fabs(p.c->commission - p.o->commission);
            sum_pnl_delta += dP;
            abs_entry.push_back(dE);
            abs_exit.push_back(dX);
            abs_pnl.push_back(std::fabs(dP));
            abs_comm.push_back(dC);
            auto& slot = by_reason[p.o->exit_reason];
            slot.first  += 1;
            slot.second += dP;
        } else if (p.o) ++orphan_o;
        else            ++orphan_c;
    }
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "\n=== RECONCILIATION SUMMARY ===\n";
    std::cout << "Matched trade pairs        : " << matched << "\n";
    std::cout << "Orphan Omega  (eng-only)   : " << orphan_o
              << "   (engine claims trade broker has no record of)\n";
    std::cout << "Orphan broker (cT-only)    : " << orphan_c
              << "   (broker executed trade Omega never recorded)\n";
    std::cout << "Sum pnl_delta (cT - Omega) : " << sum_pnl_delta << " USD"
              << "   (negative = Omega over-reports profit / under-reports loss)\n";

    if (!abs_pnl.empty()) {
        std::cout << "\nGap distributions (matched pairs):\n";
        std::cout << std::setprecision(5);
        std::cout << "  |entry_px_delta| : mean=" << (std::accumulate(abs_entry.begin(), abs_entry.end(), 0.0) / abs_entry.size())
                  << "  median=" << percentile(abs_entry, 0.5)
                  << "  p95=" << percentile(abs_entry, 0.95) << "\n";
        std::cout << "  |exit_px_delta|  : mean=" << (std::accumulate(abs_exit.begin(), abs_exit.end(), 0.0) / abs_exit.size())
                  << "  median=" << percentile(abs_exit, 0.5)
                  << "  p95=" << percentile(abs_exit, 0.95) << "\n";
        std::cout << std::setprecision(2);
        std::cout << "  |pnl_delta|      : mean=" << (std::accumulate(abs_pnl.begin(), abs_pnl.end(), 0.0) / abs_pnl.size())
                  << "  median=" << percentile(abs_pnl, 0.5)
                  << "  p95=" << percentile(abs_pnl, 0.95) << " USD\n";
        std::cout << "  |comm_delta|     : mean=" << (std::accumulate(abs_comm.begin(), abs_comm.end(), 0.0) / abs_comm.size())
                  << "  median=" << percentile(abs_comm, 0.5)
                  << "  p95=" << percentile(abs_comm, 0.95) << " USD\n";
    }

    if (!by_reason.empty()) {
        std::cout << "\nPnL gap by Omega exit_reason (cT - Omega):\n";
        for (const auto& [reason, val] : by_reason) {
            std::cout << "  " << std::setw(16) << std::left << reason
                      << " n=" << val.first
                      << "   sum_delta=" << val.second << " USD"
                      << "   mean_delta=" << (val.second / std::max(1, val.first)) << "\n";
        }
    }
    std::cout << "\nIf sum_pnl_delta is materially negative (or matches Friday's NZ$ bleed\n"
                 "to within ~10%), the GUI-vs-cTrader disparity hypothesis is confirmed and\n"
                 "the next priority is wiring TradeRecord broker_* fields into the persisted\n"
                 "CSV writer (omega_main.hpp:281) -- see HANDOFF_S33 P1.\n";
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------
void usage() {
    std::cout <<
      "ledger_reconcile -- Omega vs cTrader trade-by-trade reconciliation\n"
      "\n"
      "Usage:\n"
      "  ledger_reconcile --omega <omega_csv> --ctrader <ctrader_csv>\n"
      "                   [--symbol XAUUSD] [--out <path>]\n"
      "                   [--match-window-sec N] [--commission-per-rt 0.06]\n"
      "                   [--map-col KEY=SUBSTR ...] [--verbose]\n"
      "\n"
      "Required:\n"
      "  --omega    PATH    path to omega_trade_closes_YYYY-MM-DD.csv\n"
      "  --ctrader  PATH    path to cTrader account ledger CSV export\n"
      "\n"
      "Optional:\n"
      "  --symbol            filter to a single instrument (default: all)\n"
      "  --out               output diff CSV (default: backtest/reconcile.csv)\n"
      "  --match-window-sec  +/- seconds for entry-timestamp match (default 5)\n"
      "  --commission-per-rt round-turn commission baseline (informational, default 0.06)\n"
      "  --map-col KEY=SUB   override a cTrader column resolver. KEY one of:\n"
      "                       order_id, symbol, side, volume, entry_px, entry_ts,\n"
      "                       exit_px, exit_ts, net_pnl, comm. Repeatable.\n"
      "  --verbose           extra logging during load\n";
}

} // namespace

int main(int argc, char** argv) {
    std::string omega_path, ctrader_path, symbol, out_path = "backtest/reconcile.csv";
    int match_window = 5;
    double commission_per_rt = 0.06;
    bool verbose = false;
    std::map<std::string,std::string> col_overrides;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* tag) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "[reconcile] missing value after " << tag << "\n";
                std::exit(2);
            }
            return std::string(argv[++i]);
        };
        if      (a == "--omega")             omega_path     = next("--omega");
        else if (a == "--ctrader")           ctrader_path   = next("--ctrader");
        else if (a == "--symbol")            symbol         = next("--symbol");
        else if (a == "--out")               out_path       = next("--out");
        else if (a == "--match-window-sec")  match_window   = (int)parse_int64_safe(next("--match-window-sec"));
        else if (a == "--commission-per-rt") commission_per_rt = parse_double_safe(next("--commission-per-rt"));
        else if (a == "--verbose")           verbose        = true;
        else if (a == "--map-col") {
            const std::string kv = next("--map-col");
            const auto eq = kv.find('=');
            if (eq == std::string::npos) {
                std::cerr << "[reconcile] --map-col expects KEY=SUBSTR (got '" << kv << "')\n";
                std::exit(2);
            }
            col_overrides[lower_copy(kv.substr(0, eq))] = trim_copy(kv.substr(eq + 1));
        }
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else {
            std::cerr << "[reconcile] unknown argument: " << a << "\n";
            usage();
            return 2;
        }
    }
    if (omega_path.empty() || ctrader_path.empty()) {
        usage();
        return 2;
    }

    // Suppress unused warning if no more uses.
    (void)commission_per_rt;

    std::string err;
    auto omega   = load_omega_csv(omega_path,   symbol, err);
    if (!err.empty()) { std::cerr << "[reconcile] " << err << "\n"; return 3; }
    auto ctrader = load_ctrader_csv(ctrader_path, symbol, col_overrides, err);
    if (!err.empty()) { std::cerr << "[reconcile] " << err << "\n"; return 3; }

    if (verbose) {
        std::cout << "[reconcile] loaded " << omega.size()   << " Omega rows from "   << omega_path   << "\n";
        std::cout << "[reconcile] loaded " << ctrader.size() << " cTrader rows from " << ctrader_path << "\n";
        std::cout << "[reconcile] match window: +/- " << match_window << " s\n";
        if (!symbol.empty()) std::cout << "[reconcile] symbol filter: " << symbol << "\n";
    }
    if (omega.empty() && ctrader.empty()) {
        std::cerr << "[reconcile] both inputs are empty after symbol filter -- nothing to do\n";
        return 3;
    }

    auto pairs = match_trades(omega, ctrader, match_window);
    write_output_csv(out_path, pairs);
    print_summary(pairs);
    std::cout << "\n[reconcile] wrote diff CSV to " << out_path << "\n";
    return 0;
}
