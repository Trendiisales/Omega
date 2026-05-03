// =============================================================================
// IndexBacktest.cpp
// =============================================================================
// Standalone Mac-buildable backtest that exercises the index-engine cross-
// engine path WITH the Bug #3 fix and the IndexMacroCrash four-symbol wiring
// from include/tick_indices.hpp.
//
// Why this file exists:
//   OmegaBacktest.cpp links a gold-only engine list. IndexFlowBacktest.cpp
//   uses synthetic ticks and only instantiates IndexFlowEngine, not the
//   cross-engine gate composition. Neither exercises the production
//   tick_indices.hpp on_tick_nas100 path where the Bug #3 + IMC fixes live.
//
// What this file does:
//   1. Reads HistData NSX (NSXUSD) tick CSVs in
//      ~/Tick/Nas/HISTDATA_COM_ASCII_NSXUSD_T*/DAT_ASCII_NSXUSD_T_*.csv
//      (treats NSXUSD as NAS100 for symbol-routing purposes).
//   2. Instantiates the three NAS-participating index engines:
//        - IndexFlowEngine        (g_iflow_nas analog)
//        - IndexHybridBracketEngine (g_hybrid_nas100 analog)
//        - IndexMacroCrashEngine  (g_imacro_nas analog)
//   3. Replicates inline the entry-gate composition from on_tick_nas100
//      INCLUDING the Bug #3 cross-engine block (index_any_open() +
//      idx_recent_close_block()) and the IMC wiring with per-symbol slow-EWM
//      atr baseline driving vol_ratio.
//   4. Emits a trades CSV with columns matching the live shadow ledger
//      schema (entry_ts_unix, symbol, side, engine, net_pnl, gap_s columns
//      derivable in python) so the documented whipsaw filter
//      from KNOWN_BUGS.md Bug #3 runs against the output unchanged.
//
// Build:
//   cmake --build build --target IndexBacktest -j 8
// Run:
//   ./build/IndexBacktest <ticks.csv> [--out trades.csv] [--limit N]
//
// HistData CSV format (one tick per line):
//   YYYYMMDD HHMMSSmmm,bid,ask,volume
// Example:
//   20260401 000000045,23801.683000,23802.866000,0
// Timestamps are in EST (UTC-5). We convert to UTC seconds on read.
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <ctime>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <atomic>
#include <chrono>
#include <algorithm>

#include "../include/IndexFlowEngine.hpp"
#include "../include/IndexHybridBracketEngine.hpp"

// =============================================================================
// Bug #3 cross-engine state (replicated from globals.hpp, namespace omega::idx)
// =============================================================================
// In production these live in globals.hpp. We replicate them here so the
// backtest is self-contained and we can confirm the fix logic in isolation.
namespace bt {

static std::atomic<int64_t> g_idx_last_close_ts{0};   // unix seconds
static int g_index_min_entry_gap_sec = 120;           // KNOWN_BUGS.md default

static inline void record_index_close(const std::string& symbol) noexcept {
    if (symbol == "US500.F" || symbol == "USTEC.F" ||
        symbol == "DJ30.F"  || symbol == "NAS100") {
        const int64_t now_s = static_cast<int64_t>(std::time(nullptr));
        g_idx_last_close_ts.store(now_s, std::memory_order_relaxed);
    }
}

// Variant of record_index_close that takes an explicit timestamp from the
// tick data, so the post-close gap behaves correctly under historical
// playback (production uses wall-clock std::time(nullptr) which is wrong
// for backtest replay).
static inline void record_index_close_at(const std::string& symbol, int64_t ts_s) noexcept {
    if (symbol == "US500.F" || symbol == "USTEC.F" ||
        symbol == "DJ30.F"  || symbol == "NAS100") {
        g_idx_last_close_ts.store(ts_s, std::memory_order_relaxed);
    }
}

static inline bool idx_recent_close_block(int64_t now_s) noexcept {
    const int64_t last = g_idx_last_close_ts.load(std::memory_order_relaxed);
    if (last == 0) return false;
    return (now_s - last) < static_cast<int64_t>(g_index_min_entry_gap_sec);
}

// Per-NAS index_any_open. In production this checks all index engines across
// all four symbols. For NAS-only test harness we check the three NAS engines.
struct NasEngines {
    omega::idx::IndexFlowEngine*       iflow;
    omega::idx::IndexHybridBracketEngine* hybrid;
    omega::idx::IndexMacroCrashEngine* imacro;
};

static inline bool index_any_open(const NasEngines& e) noexcept {
    return  e.iflow->has_open_position()  ||
            e.hybrid->has_open_position() ||
            e.imacro->has_open_position();
}

} // namespace bt

// =============================================================================
// HistData NSX reader
// =============================================================================
struct HistTick {
    int64_t ts_s;     // unix seconds (UTC)
    int64_t ts_ms;    // unix ms
    double  bid;
    double  ask;
};

// Parse "YYYYMMDD HHMMSSmmm" as EST and return UTC unix seconds.
// HistData EST is fixed UTC-5 (no DST), per their convention.
static bool parse_histdata_line(const char* line, HistTick& out) {
    // Format: YYYYMMDD HHMMSSmmm,bid,ask,volume
    if (std::strlen(line) < 25) return false;
    int Y, M, D, h, m, s, ms;
    if (std::sscanf(line, "%4d%2d%2d %2d%2d%2d%3d", &Y, &M, &D, &h, &m, &s, &ms) != 7)
        return false;

    // Find first comma after timestamp
    const char* p = std::strchr(line, ',');
    if (!p) return false;
    double bid = std::strtod(p + 1, nullptr);
    p = std::strchr(p + 1, ',');
    if (!p) return false;
    double ask = std::strtod(p + 1, nullptr);
    if (bid <= 0.0 || ask <= 0.0 || bid >= ask) return false;

    // Build UTC tm and convert
    std::tm tm{};
    tm.tm_year = Y - 1900;
    tm.tm_mon  = M - 1;
    tm.tm_mday = D;
    tm.tm_hour = h;
    tm.tm_min  = m;
    tm.tm_sec  = s;
    // timegm interprets struct tm as UTC. The tm we just built is EST clock
    // time, so the UTC equivalent is +5h.
#if defined(__APPLE__) || defined(__linux__)
    int64_t t_est_as_utc = static_cast<int64_t>(timegm(&tm));
#else
    int64_t t_est_as_utc = static_cast<int64_t>(_mkgmtime(&tm));
#endif
    out.ts_s  = t_est_as_utc + 5 * 3600; // EST -> UTC
    out.ts_ms = out.ts_s * 1000LL + ms;
    out.bid   = bid;
    out.ask   = ask;
    return true;
}

// =============================================================================
// Main
// =============================================================================
int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "Usage: IndexBacktest <ticks.csv> [options]\n"
            "  --out <f>     trades CSV output       (default bt_index_real_trades.csv)\n"
            "  --limit <N>   max ticks to process    (default unlimited)\n"
            "  --gap <N>     index_min_entry_gap_sec (default 120)\n"
            "  --no-gate     disable Bug #3 cross-engine gate (pre-fix behaviour)\n");
        return 1;
    }
    const char* in_path  = argv[1];
    const char* out_path = "bt_index_real_trades.csv";
    int64_t     limit    = -1;
    bool        no_gate  = false;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--out")    && i+1 < argc) out_path = argv[++i];
        else if (!std::strcmp(argv[i], "--limit")  && i+1 < argc) limit = std::atoll(argv[++i]);
        else if (!std::strcmp(argv[i], "--gap")    && i+1 < argc) bt::g_index_min_entry_gap_sec = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--no-gate")) no_gate = true;
    }

    std::printf("================================================================\n");
    std::printf("  Omega IndexBacktest -- NAS100 (HistData NSXUSD)\n");
    std::printf("  Input    : %s\n", in_path);
    std::printf("  Output   : %s\n", out_path);
    std::printf("  Bug #3 gate: %s (gap=%ds)\n",
        no_gate ? "DISABLED (pre-fix)" : "ENABLED",
        bt::g_index_min_entry_gap_sec);
    std::printf("================================================================\n");

    // Engine instances (NAS-only)
    // ── HBI config patched for HistData NSX volatility profile ───────────────
    // HistData tick-by-tick range is tighter than the cTrader live feed the
    // production NAS100 config (min_range=8, lookback=180) is tuned for.
    // Without these patches the bracket never arms on HistData and we get
    // zero trades, which means no Bug #3 gate exercise. Patches are diagnostic
    // only -- do NOT propagate to omega::idx::make_nas100_config() in
    // include/IndexHybridBracketEngine.hpp.
    auto hbi_cfg = omega::idx::make_nas100_config();
    hbi_cfg.min_range          = 0.5;   // was 8.0
    hbi_cfg.max_range          = 50.0;  // was 40.0
    hbi_cfg.structure_lookback = 30;    // was 180
    hbi_cfg.min_entry_ticks    = 20;    // was 150
    hbi_cfg.max_spread         = 50.0;  // open wide so spread doesn't gate
    hbi_cfg.cooldown_s         = 5;     // re-arm fast after close
    hbi_cfg.dir_sl_cooldown_s  = 5;
    hbi_cfg.min_hold_s         = 1;     // allow rapid exits
    hbi_cfg.pending_timeout_s  = 30;    // give up on unfilled brackets fast
    omega::idx::IndexFlowEngine          iflow_nas("NAS100");
    omega::idx::IndexHybridBracketEngine hybrid_nas(hbi_cfg);
    omega::idx::IndexMacroCrashEngine    imacro_nas("NAS100");
    bt::NasEngines engines{&iflow_nas, &hybrid_nas, &imacro_nas};

    // Seed IFLOW's atr tracker so the entry gates clear from the first tick
    // rather than waiting for the EWM to warm up. 30pt is the typical NAS100
    // hourly ATR per IndexFlowEngine.hpp comments.
    iflow_nas.seed_atr(30.0);

    // Trades output
    std::FILE* f_trades = std::fopen(out_path, "w");
    if (!f_trades) {
        std::fprintf(stderr, "[ERROR] Cannot open output: %s\n", out_path);
        return 1;
    }
    std::fprintf(f_trades,
        "entry_ts_unix,symbol,side,engine,entry,exit,net_pnl,mfe,mae,"
        "exit_reason,hold_sec\n");

    int64_t n_trades = 0;
    int64_t blocked_by_gate = 0;
    double  total_pnl = 0.0;

    // Close callback -- mirrors ca_on_close() in trade_lifecycle.hpp:
    // 1) records the trade to CSV, 2) updates the cross-engine close timestamp.
    int64_t cur_tick_ts_s = 0;  // updated each tick for use in close timestamps
    auto on_close = [&](const omega::TradeRecord& tr) {
        const double pnl_usd = tr.pnl * 1.0;  // NAS100 = $1/pt per OmegaCostGuard.hpp
        std::fprintf(f_trades,
            "%lld,%s,%s,%s,%.4f,%.4f,%.4f,%.4f,%.4f,%s,%lld\n",
            (long long)(tr.entryTs > 0 ? tr.entryTs : cur_tick_ts_s),
            tr.symbol.c_str(),
            tr.side.c_str(),
            tr.engine.c_str(),
            tr.entryPrice, tr.exitPrice, pnl_usd, tr.mfe, tr.mae,
            tr.exitReason.c_str(),
            (long long)(tr.exitTs - tr.entryTs));
        ++n_trades;
        total_pnl += pnl_usd;
        // Bug #3 hook: record the close so successive entries get gated.
        bt::record_index_close_at(tr.symbol, cur_tick_ts_s);
    };

    // Per-NAS slow-EWM atr baseline (matches tick_indices.hpp on_tick_nas100
    // IMC wiring block).
    double s_atr_base_nas = 0.0;

    // Read ticks
    std::ifstream fin(in_path);
    if (!fin) {
        std::fprintf(stderr, "[ERROR] Cannot open input: %s\n", in_path);
        std::fclose(f_trades);
        return 1;
    }

    std::string line;
    int64_t n_ticks = 0;
    int64_t n_parse_fail = 0;
    double last_bid = 0.0, last_ask = 0.0;  // for end-of-run force_close
    auto t0 = std::chrono::steady_clock::now();

    while (std::getline(fin, line)) {
        if (limit > 0 && n_ticks >= limit) break;
        HistTick t;
        if (!parse_histdata_line(line.c_str(), t)) { ++n_parse_fail; continue; }
        ++n_ticks;
        cur_tick_ts_s = t.ts_s;

        // Drive the IFLOW/IMC/IndexSwing test clock so their idx_now_ms()
        // / idx_now_sec() helpers return tick-time rather than wall-clock.
        // Without this, entry_ts / cooldowns / hold-times reference today's
        // wall-clock but compare against historical tick timestamps, which
        // breaks every time-gated transition in those engines.
        omega::idx::set_idx_test_clock_ms(t.ts_ms);

        // Diagnostic: every 200k ticks, dump engine state.
        if (n_ticks % 200000 == 0) {
            const auto phase = static_cast<int>(hybrid_nas.phase);
            std::printf("[BT-DIAG] tick=%lld ts=%lld bid=%.2f hbi_phase=%d "
                "hbi_active=%d hbi_mfe=%.2f hbi_sl=%.2f hbi_be=%d "
                "iflow_active=%d imacro_active=%d\n",
                (long long)n_ticks, (long long)t.ts_s, t.bid,
                phase,
                (int)hybrid_nas.pos.active,
                hybrid_nas.pos.mfe,
                hybrid_nas.pos.sl,
                (int)hybrid_nas.pos.be_locked,
                (int)iflow_nas.has_open_position(),
                (int)imacro_nas.has_open_position());
            std::fflush(stdout);
        }

        const std::string sym = "NAS100";
        const double bid = t.bid;
        const double ask = t.ask;
        last_bid = bid;
        last_ask = ask;

        // Approximate L2 imbalance (no L2 data in HistData) -- neutral.
        const double l2_imb = 0.5;

        // ── 1. IndexHybridBracket: two-call pattern, matching production
        //       tick_indices.hpp:252-278 (us500 handler).
        //
        //   First call (manage path): pass the BASE can_enter (true here --
        //     no session/risk gate in the backtest). The engine's PENDING
        //     branch reads can_enter to decide whether to cancel resting
        //     orders. Passing false here triggers the 15s PENDING cancel
        //     timer at IndexHybridBracketEngine.hpp:320-326, and because
        //     m_pending_blocked_since is never reset by reset_to_idle(),
        //     subsequent FIRE attempts hit a "blocked since first tick"
        //     reading and immediately cancel.
        //
        //   Second call (entry path): pass the FULL gate composition
        //     (Bug #3 cross-engine block + post-close gap). Engine consults
        //     this only on IDLE/ARMED -> PENDING transition.
        const bool base_can_enter = true;  // proxy for production base_can_<sym>
        if (hybrid_nas.has_open_position()) {
            hybrid_nas.on_tick(bid, ask, t.ts_ms, base_can_enter,
                               false, false, 0, on_close);
        }

        // Entry-gate composition (mirrors on_tick_nas100, lines 915-922 of
        // tick_indices.hpp post-fix). For the backtest we drop the
        // session_slot check (live data has session windows; HistData covers
        // all hours). The Bug #3 gate is the part being tested.
        const bool gate_active = !no_gate;
        const bool block_concurrent = gate_active && bt::index_any_open(engines);
        const bool block_postclose  = gate_active && bt::idx_recent_close_block(t.ts_s);
        const bool can_enter = !block_concurrent && !block_postclose;
        if (!can_enter) {
            // Track why we blocked, for diagnostic.
            ++blocked_by_gate;
        }

        if (!hybrid_nas.has_open_position()) {
            hybrid_nas.on_tick(bid, ask, t.ts_ms, can_enter,
                               false, false, 0, on_close);
        }

        // ── 2. IndexFlow: management + entry attempt ────────────────────────
        // Same gate composition.
        if (iflow_nas.has_open_position()) {
            iflow_nas.on_tick(sym, bid, ask, l2_imb, on_close, /*can_enter=*/false);
        } else if (can_enter && !hybrid_nas.has_open_position()) {
            const auto sig = iflow_nas.on_tick(sym, bid, ask, l2_imb, on_close, true);
            if (sig.valid) {
                // Mirror the patch_size pattern. In production
                // enter_directional() returns the live broker lot; here we
                // use the engine's own internal sizing.
                iflow_nas.patch_size(sig.size);
            }
        }

        // ── 3. IndexMacroCrash: per-tick wiring with vol_ratio EWM baseline ─
        // Mirrors the IMC wiring block at tick_indices.hpp on_tick_nas100.
        {
            const double cur_atr_nas = iflow_nas.atr();
            if (s_atr_base_nas <= 0.0) {
                if (cur_atr_nas > 0.0) s_atr_base_nas = cur_atr_nas;
            } else {
                s_atr_base_nas = 0.999 * s_atr_base_nas + 0.001 * cur_atr_nas;
            }
            const double nas_vol_ratio = (s_atr_base_nas > 0.0)
                ? (cur_atr_nas / s_atr_base_nas) : 1.0;
            const bool nas_trend_regime = iflow_nas.is_trending();
            imacro_nas.on_tick(bid, ask, cur_atr_nas, iflow_nas.drift(),
                               nas_vol_ratio, nas_trend_regime, on_close,
                               /*can_enter=*/can_enter);
        }
    }

    // End-of-run: force-close any open position so all P&L lands in the CSV.
    if (iflow_nas.has_open_position() && last_bid > 0.0 && last_ask > 0.0) {
        iflow_nas.force_close(last_bid, last_ask, on_close);
    }
    // (HBI / IMC don't expose a public force_close; their open positions
    // (if any) won't appear in the CSV. The IndexBacktest is primarily a
    // gate-validation harness so unflushed IMC/HBI tail positions are an
    // acceptable simplification.)

    std::fclose(f_trades);

    auto t1 = std::chrono::steady_clock::now();
    const double elapsed_s = std::chrono::duration<double>(t1 - t0).count();

    std::printf("\n");
    std::printf("================================================================\n");
    std::printf("  RESULTS\n");
    std::printf("================================================================\n");
    std::printf("  Ticks processed   : %lld\n", (long long)n_ticks);
    std::printf("  Parse failures    : %lld\n", (long long)n_parse_fail);
    std::printf("  Run time          : %.2fs (%.0f kt/s)\n",
        elapsed_s, n_ticks / elapsed_s / 1000.0);
    std::printf("  Trades emitted    : %lld\n", (long long)n_trades);
    std::printf("  Gate-blocked tick : %lld\n", (long long)blocked_by_gate);
    std::printf("  Net P&L (USD)     : $%.2f\n", total_pnl);
    std::printf("  Trades file       : %s\n", out_path);
    std::printf("================================================================\n");
    return 0;
}
