// =============================================================================
//  CellShadowLedger.hpp -- per-engine V2 shadow ledger writers.
//
//  Status: Phase 2a deployment helper -- ADDITIVE ONLY.
//
//  WHY THIS EXISTS:
//      During Phase 2a/2b shadow validation, the V2 CellEngine refactor
//      portfolios (g_tsmom_v2 today; g_donchian_v2 / g_epb_v2 / g_trend_v2
//      later) run alongside their V1 counterparts. V1 already writes to
//      g_omegaLedger via the standard ca_on_close -> handle_closed_trade
//      pipeline. V2 trades MUST NOT flow into g_omegaLedger -- doing so
//      would double-count every trade in the master ledger / daily PnL /
//      drawdown / fast-loss-streak / engine-cull / param-gate state.
//
//      Instead, V2 trades go into a parallel CSV per engine:
//          logs/shadow/tsmom_v2.csv
//          logs/shadow/donchian_v2.csv
//          logs/shadow/epb_v2.csv
//          logs/shadow/trend_v2.csv
//
//      These are pure side-files -- no consumer of g_omegaLedger sees them.
//      Diff workflow: nightly export V1's slice of g_omegaLedger to a CSV
//      with the same schema, then `cmp -s tsmom_v1.csv tsmom_v2.csv`.
//
//  THREAD SAFETY:
//      The cTrader depth thread + FIX read thread can both call into
//      runtime callbacks. Each writer is guarded by its own mutex; the
//      FILE* is opened once at engine_init time and stays open for the
//      process lifetime.
//
//  CSV SCHEMA:
//      Frozen schema, must match TsmomCellBacktest.cpp's kLedgerHeader so
//      diffs against backtest ledgers work without translation.
//
//      id,symbol,side,engine,entryTs,exitTs,entryPrice,exitPrice,sl,tp,
//      size,pnl,mfe,mae,atr_at_entry,spreadAtEntry,exitReason,regime,shadow
// =============================================================================
#pragma once

#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>

#include "OmegaTradeLedger.hpp"

namespace omega::cell::shadow {

// Frozen schema -- must match TsmomCellBacktest.cpp kLedgerHeader.
inline constexpr const char* kLedgerHeader =
    "id,symbol,side,engine,entryTs,exitTs,entryPrice,exitPrice,sl,tp,size,"
    "pnl,mfe,mae,atr_at_entry,spreadAtEntry,exitReason,regime,shadow\n";

// -----------------------------------------------------------------------------
//  Writer -- one instance per shadow engine. Wraps a FILE* + mutex. Open
//  once at startup; pass `bind()` as the runtime callback to the V2 engine.
// -----------------------------------------------------------------------------
class Writer {
public:
    // Open in append mode. Creates parent directories if missing. Writes
    // the CSV header if the file is empty (fresh start) but not on append
    // (so multiple process restarts share one CSV without dup headers).
    // Idempotent: subsequent calls to open() are no-ops.
    bool open(const std::string& path) noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        if (file_) return true;

        // Best-effort parent dir creation (Windows / Linux / Mac).
        std::error_code ec;
        const auto parent = std::filesystem::path(path).parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, ec);
            // Ignore error -- fopen below will report if it actually fails.
        }

        file_ = std::fopen(path.c_str(), "a");
        if (!file_) {
            std::fprintf(stderr,
                "[SHADOW-LEDGER] FAIL -- cannot open '%s' for append\n",
                path.c_str());
            std::fflush(stderr);
            return false;
        }
        // Write header only if the file is empty (fresh start).
        std::fseek(file_, 0, SEEK_END);
        const long sz = std::ftell(file_);
        if (sz == 0) {
            std::fputs(kLedgerHeader, file_);
            std::fflush(file_);
        }
        path_ = path;
        std::printf("[SHADOW-LEDGER] opened '%s' (size=%ld)\n",
                    path.c_str(), sz);
        std::fflush(stdout);
        return true;
    }

    // Append one TradeRecord row. Thread-safe. Flushes after each row so the
    // CSV is durable across crashes -- shadow volume is low enough that the
    // perf cost is negligible.
    void write(const ::omega::TradeRecord& tr) noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!file_) return;
        std::fprintf(file_,
            "%d,%s,%s,%s,%lld,%lld,%.6f,%.6f,%.6f,%.6f,%.4f,%.6f,%.6f,%.6f,%.6f,%.6f,%s,%s,%d\n",
            tr.id,
            tr.symbol.c_str(),
            tr.side.c_str(),
            tr.engine.c_str(),
            static_cast<long long>(tr.entryTs),
            static_cast<long long>(tr.exitTs),
            tr.entryPrice,
            tr.exitPrice,
            tr.sl,
            tr.tp,
            tr.size,
            tr.pnl,
            tr.mfe,
            tr.mae,
            tr.atr_at_entry,
            tr.spreadAtEntry,
            tr.exitReason.c_str(),
            tr.regime.c_str(),
            tr.shadow ? 1 : 0);
        std::fflush(file_);
    }

    // Close on shutdown. Engine_init wires open(); main shutdown can call this.
    void close() noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        if (file_) {
            std::fclose(file_);
            file_ = nullptr;
            std::printf("[SHADOW-LEDGER] closed '%s'\n", path_.c_str());
            std::fflush(stdout);
        }
    }

    ~Writer() { close(); }

    // Returns a std::function suitable for passing as the runtime_cb argument
    // to CellPortfolio::on_h1_bar / on_tick / force_close_all.
    auto bind() noexcept {
        return [this](const ::omega::TradeRecord& tr) noexcept {
            this->write(tr);
        };
    }

private:
    std::mutex   mtx_;
    std::FILE*   file_ = nullptr;
    std::string  path_;
};

// -----------------------------------------------------------------------------
//  Per-engine singleton accessors. Each shadow engine has its own Writer;
//  the call site (engine_init.hpp / tick_gold.hpp) just refers to
//  `omega::cell::shadow::tsmom_writer()`.
//
//  function-local statics so we don't fight static-init ordering across
//  TUs (this header may be included from globals.hpp -> main.cpp; the
//  Writer is constructed on first call).
// -----------------------------------------------------------------------------
inline Writer& tsmom_writer() {
    static Writer w;
    return w;
}

// Reserved for Phase 3 (next session):
// inline Writer& donchian_writer() { static Writer w; return w; }
// inline Writer& epb_writer()      { static Writer w; return w; }
// inline Writer& trend_writer()    { static Writer w; return w; }

}  // namespace omega::cell::shadow
