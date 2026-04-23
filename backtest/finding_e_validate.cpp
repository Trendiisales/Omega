// =============================================================================
// finding_e_validate.cpp -- Backtest validator for S16 Finding E (bracket bias
// propagation). Determines whether the proposed bracket_trend_bias() gate
// would have blocked the bad LONG entries on 2026-04-22's downtrend.
//
// PURPOSE:
//   1. Replay the day's BracketEngine exits through BracketTrendState::on_exit
//   2. For each non-bracket entry on the same day (from all 16 target engines),
//      query bracket_trend_bias("XAUUSD") at the entry timestamp
//   3. Report per-engine: entries that would have been blocked, PnL "saved"
//
// NOT PRODUCTION:
//   Validation-only. Uses BracketTrendState inline header (no include
//   transplantation needed). No trading engine bodies are replayed -- we work
//   off the actual entry ledger from the target day.
//
// INPUTS:
//   --bracket-exits <csv>  Bracket-engine exit ledger for the day:
//                          ts_ms,is_long,outcome
//                            outcome: 1=WIN, 0=NEUTRAL, -1=LOSS
//   --all-entries <csv>    All entries (or exits, with entry_ts) from ALL
//                          engines for the day:
//                          ts_ms,engine,side,pnl_usd
//                            side: LONG/SHORT
//                            pnl_usd: realized PnL (can be positive or negative)
//
// OUTPUTS:
//   --out-summary <csv>    Per-engine summary:
//                          engine,total_entries,would_block,blocked_pnl,
//                                 not_blocked,not_blocked_pnl
//   --out-bias-trace <csv> Per-tick bias trace (debug):
//                          ts_ms,bias,block_until_ms,last_exit_outcome
//
// BUILD:
//   g++ -std=c++17 -O2 -Wall -Wextra finding_e_validate.cpp
//       -I../include -o finding_e_validate
//
// The #include of BracketTrendState.hpp compiles clean standalone because
// the header depends only on <cstdint>, <cstdio>, <chrono>, <string>,
// <deque>, <unordered_map>, <algorithm> -- no Omega internals.
// =============================================================================

#include "BracketTrendState.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

// =============================================================================
// Input records
// =============================================================================
struct BracketExit {
    int64_t ts_ms;
    bool    is_long;
    int     outcome;   // BRACKET_EXIT_WIN / NEUTRAL / LOSS
};

struct AnyEntry {
    int64_t     ts_ms;
    std::string engine;
    std::string side;      // "LONG" or "SHORT"
    double      pnl_usd;
};

// =============================================================================
// CSV helpers
// =============================================================================
static std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        while (!tok.empty() && (tok.back() == '\r' || tok.back() == '\n'))
            tok.pop_back();
        out.push_back(tok);
    }
    return out;
}

static std::vector<BracketExit> load_bracket_exits(const std::string& path) {
    std::vector<BracketExit> out;
    std::ifstream in(path);
    if (!in.is_open()) {
        std::cerr << "[ERROR] cannot open " << path << "\n";
        return out;
    }
    std::string line;
    std::getline(in, line); // header
    int col_ts = -1, col_long = -1, col_outcome = -1;
    auto hdr = split_csv(line);
    for (size_t i = 0; i < hdr.size(); ++i) {
        if (hdr[i] == "ts_ms")   col_ts      = (int)i;
        if (hdr[i] == "is_long") col_long    = (int)i;
        if (hdr[i] == "outcome") col_outcome = (int)i;
    }
    if (col_ts < 0 || col_long < 0 || col_outcome < 0) {
        std::cerr << "[ERROR] missing header columns in " << path << "\n";
        return out;
    }
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        auto cols = split_csv(line);
        if ((int)cols.size() <= std::max({col_ts, col_long, col_outcome})) continue;
        BracketExit e;
        try {
            e.ts_ms   = std::stoll(cols[col_ts]);
            e.is_long = (cols[col_long] == "1" || cols[col_long] == "true" ||
                         cols[col_long] == "LONG");
            e.outcome = std::stoi(cols[col_outcome]);
        } catch (...) { continue; }
        out.push_back(e);
    }
    return out;
}

static std::vector<AnyEntry> load_any_entries(const std::string& path) {
    std::vector<AnyEntry> out;
    std::ifstream in(path);
    if (!in.is_open()) {
        std::cerr << "[ERROR] cannot open " << path << "\n";
        return out;
    }
    std::string line;
    std::getline(in, line);
    int col_ts = -1, col_eng = -1, col_side = -1, col_pnl = -1;
    auto hdr = split_csv(line);
    for (size_t i = 0; i < hdr.size(); ++i) {
        if (hdr[i] == "ts_ms")   col_ts   = (int)i;
        if (hdr[i] == "engine")  col_eng  = (int)i;
        if (hdr[i] == "side")    col_side = (int)i;
        if (hdr[i] == "pnl_usd") col_pnl  = (int)i;
    }
    if (col_ts < 0 || col_eng < 0 || col_side < 0 || col_pnl < 0) {
        std::cerr << "[ERROR] missing header columns in " << path << "\n";
        return out;
    }
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        auto cols = split_csv(line);
        if ((int)cols.size() <= std::max({col_ts, col_eng, col_side, col_pnl})) continue;
        AnyEntry e;
        try {
            e.ts_ms   = std::stoll(cols[col_ts]);
            e.engine  = cols[col_eng];
            e.side    = cols[col_side];
            e.pnl_usd = std::stod(cols[col_pnl]);
        } catch (...) { continue; }
        out.push_back(e);
    }
    return out;
}

// =============================================================================
// Replay: interleave bracket_exits and any_entries in timestamp order.
// For each bracket_exit: feed it to BracketTrendState::on_exit
// For each any_entry: compute bias_at_ts and decide if it would have been
// blocked.
// =============================================================================
int main(int argc, char** argv) {
    std::string bracket_path, entries_path;
    std::string out_summary = "finding_e_summary.csv";
    std::string out_trace   = "finding_e_trace.csv";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--bracket-exits" && i+1 < argc) bracket_path = argv[++i];
        else if (a == "--all-entries"  && i+1 < argc) entries_path = argv[++i];
        else if (a == "--out-summary"  && i+1 < argc) out_summary  = argv[++i];
        else if (a == "--out-bias-trace" && i+1 < argc) out_trace  = argv[++i];
    }
    if (bracket_path.empty() || entries_path.empty()) {
        std::cerr << "usage: finding_e_validate --bracket-exits <csv> "
                  << "--all-entries <csv> [--out-summary <csv>] "
                  << "[--out-bias-trace <csv>]\n";
        return 2;
    }

    auto b_exits = load_bracket_exits(bracket_path);
    auto entries = load_any_entries(entries_path);
    std::cerr << "[INFO] bracket exits: " << b_exits.size()
              << "   entries: " << entries.size() << "\n";

    // Merge by timestamp, exits processed before entries at same ts.
    struct Event {
        int64_t ts_ms;
        int     kind;   // 0 = exit, 1 = entry
        int     idx;    // index into b_exits or entries
    };
    std::vector<Event> events;
    events.reserve(b_exits.size() + entries.size());
    for (size_t i = 0; i < b_exits.size(); ++i)
        events.push_back({b_exits[i].ts_ms, 0, (int)i});
    for (size_t i = 0; i < entries.size(); ++i)
        events.push_back({entries[i].ts_ms, 1, (int)i});
    std::sort(events.begin(), events.end(), [](const Event& a, const Event& b){
        if (a.ts_ms != b.ts_ms) return a.ts_ms < b.ts_ms;
        return a.kind < b.kind;   // exit first at equal ts
    });

    // Local state map — we only care about XAUUSD
    BracketTrendState st;

    // Open trace
    std::ofstream trace(out_trace);
    trace << "ts_ms,kind,bias,block_until_ms,detail\n";

    // Per-engine, per-side accumulators
    struct Cell {
        int    total = 0;
        int    blocked = 0;
        double blocked_pnl = 0.0;
        int    not_blocked = 0;
        double not_blocked_pnl = 0.0;
    };
    std::map<std::pair<std::string,std::string>, Cell> by_eng_side;

    // Run
    for (const auto& e : events) {
        if (e.kind == 0) {
            const auto& be = b_exits[e.idx];
            st.on_exit(be.is_long, be.outcome, be.ts_ms);
            trace << be.ts_ms << ",exit,"
                  << st.bias << "," << st.block_until_ms << ","
                  << "is_long=" << be.is_long << " outcome=" << be.outcome << "\n";
        } else {
            const auto& en = entries[e.idx];
            // Compute bias at this entry's timestamp
            // (replicates accessor logic but using our local st instead of map)
            int bias_now = 0;
            if (en.ts_ms < st.block_until_ms) bias_now = st.bias;

            bool is_long = (en.side == "LONG");
            bool would_block = (is_long  && bias_now == -1) ||
                               (!is_long && bias_now ==  1);

            auto& cell = by_eng_side[{en.engine, en.side}];
            ++cell.total;
            if (would_block) {
                ++cell.blocked;
                cell.blocked_pnl += en.pnl_usd;
            } else {
                ++cell.not_blocked;
                cell.not_blocked_pnl += en.pnl_usd;
            }

            trace << en.ts_ms << ",entry," << bias_now << ","
                  << st.block_until_ms << ","
                  << en.engine << "/" << en.side << "/"
                  << std::fixed << std::setprecision(2) << en.pnl_usd
                  << (would_block ? "/BLOCKED" : "/PASS") << "\n";
        }
    }

    trace.close();

    // Summary
    std::ofstream sum(out_summary);
    sum << "engine,side,total_entries,would_block,blocked_pnl,not_blocked,not_blocked_pnl\n";
    std::cout << "\n"
              << std::setw(32) << std::left << "engine"
              << std::setw(6)  << "side"
              << std::setw(6)  << "tot"
              << std::setw(8)  << "block"
              << std::setw(12) << "blk_pnl"
              << std::setw(8)  << "nblk"
              << std::setw(12) << "nblk_pnl"
              << "\n";
    std::cout << std::string(84, '-') << "\n";

    double total_saved = 0.0, total_not_saved = 0.0;
    int    total_blocked = 0, total_not_blocked = 0;

    for (const auto& kv : by_eng_side) {
        const auto& key = kv.first;
        const auto& c   = kv.second;
        sum << key.first << "," << key.second << "," << c.total << ","
            << c.blocked << "," << std::fixed << std::setprecision(2) << c.blocked_pnl
            << "," << c.not_blocked << "," << c.not_blocked_pnl << "\n";
        std::cout << std::setw(32) << std::left << key.first
                  << std::setw(6)  << key.second
                  << std::setw(6)  << c.total
                  << std::setw(8)  << c.blocked
                  << std::setw(12) << std::fixed << std::setprecision(2) << c.blocked_pnl
                  << std::setw(8)  << c.not_blocked
                  << std::setw(12) << c.not_blocked_pnl
                  << "\n";
        total_blocked     += c.blocked;
        total_not_blocked += c.not_blocked;
        total_saved       += c.blocked_pnl;
        total_not_saved   += c.not_blocked_pnl;
    }
    std::cout << std::string(84, '-') << "\n";
    std::cout << std::setw(32) << std::left << "TOTAL"
              << std::setw(6)  << ""
              << std::setw(6)  << (total_blocked + total_not_blocked)
              << std::setw(8)  << total_blocked
              << std::setw(12) << total_saved
              << std::setw(8)  << total_not_blocked
              << std::setw(12) << total_not_saved
              << "\n";

    std::cout << "\n[DONE] summary -> " << out_summary
              << "   trace -> " << out_trace << "\n";

    // Interpretation hints
    std::cout << "\n--- Interpretation ---\n"
              << "- 'would_block' = bias flag was active against entry's direction at ts\n"
              << "- 'blocked_pnl' = sum of realized PnL of entries that would have been blocked\n"
              << "    If blocked_pnl is strongly negative, the gate would have saved losses.\n"
              << "    If blocked_pnl is positive, the gate would have blocked winners.\n"
              << "- Net effect on day = -blocked_pnl (hypothetical avoided loss)\n";

    return 0;
}
