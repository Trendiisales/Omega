#pragma once
// logging.hpp -- extracted from main.cpp
// Section: logging (original lines 2857-3042)
// SINGLE-TRANSLATION-UNIT include -- only include from main.cpp

// S34 (2026-05-12) explicit stdlib includes for the new dedupe ring + diag
// path. <cmath> is already pulled in by main.cpp:36 and <array> by several
// headers earlier in the include cascade (SweepableEnginesCRTP.hpp etc.),
// but listing them here is idempotent via header guards and survives any
// future re-ordering of the single-TU include chain.
#include <array>
#include <cmath>

//
// 2026-05-12 (S34, operator-instructed: "fix this logging issue and the PNL
//   i treat this as actual trading and so it should be"):
//   Two new defensive guards added to write_trade_close_logs():
//
//     (1) [CSV-DUP-BLOCK] — close-write dedupe.
//         A small ring buffer tracks recently-written close keys. The
//         writer can be reached from three call sites (trade_lifecycle.hpp
//         line 204 PARTIAL_1R, trade_lifecycle.hpp line 298 normal close,
//         and quote_loop.hpp line 558 stale_cb prior-day purge). If the
//         normal close pipeline AND the stale purge both fire for the
//         same TradeRecord (or any engine emits the same close twice),
//         the second write would land in the CSV as an exact-duplicate
//         row — that is the source of the identical 13:35:00 USTEC pair
//         observed in the operator's pasted log. The dedupe key is
//         engine|symbol|side|entryTs|exitTs|exitReason; window is
//         WRITE_DEDUPE_WINDOW_SEC seconds; ring capacity is
//         WRITE_DEDUPE_RING_SIZE entries. A duplicate within the window
//         is dropped at the writer and a [CSV-DUP-BLOCK] line is printed
//         so the upstream double-emit is still visible.
//
//     (2) [CSV-ZERO-SIZE] / [CSV-ZERO-PNL] — PnL sanity diagnostics.
//         Warn (but still write the row, for audit continuity) when a
//         close has size<=0, or when entry!=exit but net_pnl is zero.
//         These conditions catch the "$0.00 / -- fee" pattern that any
//         shadow-only engine produces when its size resolves to zero
//         (lot-table miss, symbol-key mismatch like "USTEC" vs
//         "USTEC.F", or shadow lot pinned to 0). Operator's directive is
//         to treat shadow as actual trading; a zero-PnL close means the
//         tape says "edge happened" but the ledger says "no money" —
//         that disagreement must be surfaced, not silently logged.
//
//   Both guards are local to logging.hpp; no protected core file (per
//   HANDOFF_S33_AFTER_S32.md §2.4 rule 3) is modified by this change.

static void rtt_record(double ms) {
    g_rtt_last = ms;
    g_rtts.push_back(ms);
    if (g_rtts.size() > 200u) g_rtts.pop_front();
    std::vector<double> v(g_rtts.begin(), g_rtts.end());
    std::sort(v.begin(), v.end());
    g_rtt_p50 = v[std::min(static_cast<size_t>(v.size() * 0.50), v.size() - 1)];
    g_rtt_p95 = v[std::min(static_cast<size_t>(v.size() * 0.95), v.size() - 1)];
}

// ?????????????????????????????????????????????????????????????????????????????
// Shadow CSV
// ?????????????????????????????????????????????????????????????????????????????
static std::mutex g_shadow_csv_mtx;
static void write_shadow_csv(const omega::TradeRecord& tr) {
    std::lock_guard<std::mutex> lk(g_shadow_csv_mtx);
    if (g_shadow_csv.is_open()) {
        g_shadow_csv << tr.entryTs << ',' << tr.symbol << ',' << tr.side
                     << ',' << tr.engine
                     << ',' << tr.entryPrice << ',' << tr.exitPrice
                     << ',' << tr.net_pnl   // net after slippage+commission (was tr.pnl = gross)
                     << ',' << tr.mfe << ',' << tr.mae
                     << ',' << (tr.exitTs - tr.entryTs)
                     << ',' << tr.exitReason
                     << ',' << tr.spreadAtEntry
                     << ',' << tr.latencyMs
                     << ',' << tr.regime << '\n';
        g_shadow_csv.flush();
    }
    if (g_daily_shadow_trade_log) {
        const int64_t bucket_ts = nowSec();  // wall-clock for file date
        g_daily_shadow_trade_log->append_row(bucket_ts, build_trade_close_csv_row(tr));
    }
}

// ?? Trade open logger ?????????????????????????????????????????????????????????
// Called at the moment of entry -- before any close event.
// Provides an audit trail of every position opened with full context.
// Uses the same TradeRecord struct populated at entry time (exitPrice/pnl are 0).
static std::mutex g_trade_open_csv_mtx;

static void write_trade_open_log(const std::string& symbol,
                                  const std::string& engine,
                                  const std::string& side,
                                  double entry_px,
                                  double tp,
                                  double sl,
                                  double size,
                                  double spread_at_entry,
                                  const std::string& regime,
                                  const std::string& reason)
{
    const int64_t ts   = nowSec();
    const std::string ts_utc = utc_iso8601(ts);
    const std::string day    = utc_weekday_name(ts);

    // Build CSV row
    std::ostringstream o;
    o << ts
      << ',' << csv_quote(ts_utc)
      << ',' << csv_quote(day)
      << ',' << csv_quote(symbol)
      << ',' << csv_quote(engine)
      << ',' << csv_quote(side)
      << std::fixed << std::setprecision(4)
      << ',' << entry_px
      << ',' << tp
      << ',' << sl
      << ',' << size
      << std::setprecision(4)
      << ',' << spread_at_entry
      << ',' << csv_quote(regime)
      << ',' << csv_quote(reason);
    const std::string row = o.str();

    // Persistent file -- survives restarts via append mode
    {
        std::lock_guard<std::mutex> lk(g_trade_open_csv_mtx);
        if (g_trade_open_csv.is_open()) {
            g_trade_open_csv << row << '\n';
            g_trade_open_csv.flush();
        }
    }
    // Daily rolling log
    if (g_daily_trade_open_log)
        g_daily_trade_open_log->append_row(ts, row);

    printf("[TRADE-OPEN] %s %s %s entry=%.4f tp=%.4f sl=%.4f size=%.4f spread=%.4f regime=%s reason=%s\n",
           symbol.c_str(), side.c_str(), engine.c_str(),
           entry_px, tp, sl, size, spread_at_entry,
           regime.c_str(), reason.c_str());
    fflush(stdout);
}

static std::string trade_ref_from_record(const omega::TradeRecord& tr) {
    std::ostringstream o;
    o << tr.symbol << '|' << tr.entryTs << '|' << tr.id << '|' << tr.engine;
    return o.str();
}

static std::string build_trade_close_csv_row(const omega::TradeRecord& tr) {
    const int64_t hold_sec = std::max<int64_t>(0, tr.exitTs - tr.entryTs);
    std::ostringstream o;
    o << tr.id
      << ',' << csv_quote(trade_ref_from_record(tr))
      << ',' << tr.entryTs
      << ',' << csv_quote(utc_iso8601(tr.entryTs))
      << ',' << csv_quote(utc_weekday_name(tr.entryTs))
      << ',' << tr.exitTs
      << ',' << csv_quote(utc_iso8601(tr.exitTs))
      << ',' << csv_quote(utc_weekday_name(tr.exitTs))
      << ',' << csv_quote(tr.symbol)
      << ',' << csv_quote(tr.engine)
      << ',' << csv_quote(tr.side)
      << std::fixed << std::setprecision(4)
      << ',' << tr.entryPrice
      << ',' << tr.exitPrice
      << ',' << tr.tp
      << ',' << tr.sl
      << ',' << tr.size
      << ',' << tr.pnl          // gross
      << ',' << tr.net_pnl      // net (after slippage + commission)
      << ',' << tr.slippage_entry
      << ',' << tr.slippage_exit
      << ',' << tr.commission
      << std::setprecision(6)
      << ',' << tr.slip_entry_pct
      << ',' << tr.slip_exit_pct
      << ',' << tr.comm_per_side
      << std::fixed << std::setprecision(4)
      << ',' << tr.mfe
      << ',' << tr.mae
      << ',' << hold_sec
      << ',' << tr.spreadAtEntry
      << ',' << tr.latencyMs
      << ',' << csv_quote(tr.regime)
      << ',' << csv_quote(tr.exitReason)
      << std::setprecision(4)
      << ',' << tr.l2_imbalance
      << ',' << (tr.l2_live ? 1 : 0)
      // S33b 2026-05-11: broker reconciliation fields. Column order matches
      // the extended header in include/omega_main.hpp. clOrdId strings are
      // CSV-quoted; bools serialised 0/1; fill prices and broker_pnl at 4dp.
      // Empty values (e.g. entry filled but close still pending) write as
      // empty string for ids, 0 for bools, 0.0 for prices/pnl -- consumers
      // should gate on the broker_*_filled flags before trusting fill_px.
      << ',' << csv_quote(tr.entry_clOrdId)
      << ',' << csv_quote(tr.close_clOrdId)
      << ',' << (tr.broker_entry_filled    ? 1 : 0)
      << ',' << (tr.broker_close_filled    ? 1 : 0)
      << ',' << (tr.broker_entry_rejected  ? 1 : 0)
      << ',' << (tr.broker_close_rejected  ? 1 : 0)
      << ',' << tr.broker_entry_fill_px
      << ',' << tr.broker_close_fill_px
      << ',' << tr.broker_pnl;
    return o.str();
}

// ── S34 close-write dedupe (see header comment) ──────────────────────────────
// Ring-buffer + mutex; constant memory footprint, no allocation on hot path.
// Window length and ring capacity tuned to keep the buffer covering the
// realistic re-fire window for any single trade (stale_cb fires within ~1s
// of the normal close; identical-tick double-emit fires within microseconds).
static constexpr int    OMEGA_CSV_DEDUPE_WINDOW_SEC = 5;
static constexpr size_t OMEGA_CSV_DEDUPE_RING_SIZE  = 64;

struct OmegaCsvDedupeEntry {
    std::string key;
    int64_t     ts;
};

static std::mutex                                                 g_csv_dedupe_mtx;
static std::array<OmegaCsvDedupeEntry, OMEGA_CSV_DEDUPE_RING_SIZE> g_csv_dedupe_ring{};
static size_t                                                     g_csv_dedupe_head = 0;

static std::string make_close_dedupe_key(const omega::TradeRecord& tr) {
    std::ostringstream o;
    o << tr.engine << '|' << tr.symbol << '|' << tr.side
      << '|' << tr.entryTs << '|' << tr.exitTs
      << '|' << tr.exitReason;
    return o.str();
}

// Returns true if this close is a duplicate within the dedupe window.
// On the first sighting, records the key in the ring buffer and returns
// false. The ring is a fixed-capacity circular buffer; oldest entries are
// silently overwritten when full. The dedupe is intentionally lenient
// (key match only, not full row hash) -- if two genuinely-distinct closes
// happen to share engine/symbol/side/entryTs/exitTs/exitReason within 5
// seconds, that pair is treated as a duplicate. Given that entryTs is a
// unix-second timestamp and a position cannot enter+exit at the same
// integer second under any real engine, this collision is non-physical.
static bool close_already_written(const omega::TradeRecord& tr) {
    const std::string key = make_close_dedupe_key(tr);
    const int64_t now_s = nowSec();
    std::lock_guard<std::mutex> lk(g_csv_dedupe_mtx);
    for (const auto& e : g_csv_dedupe_ring) {
        if (e.key.empty()) continue;
        if (e.key == key && (now_s - e.ts) <= OMEGA_CSV_DEDUPE_WINDOW_SEC) {
            return true;
        }
    }
    g_csv_dedupe_ring[g_csv_dedupe_head] = {key, now_s};
    g_csv_dedupe_head = (g_csv_dedupe_head + 1) % OMEGA_CSV_DEDUPE_RING_SIZE;
    return false;
}

static void write_trade_close_logs(const omega::TradeRecord& tr) {
    // Phantom-record guard: a valid trade always has a positive entry timestamp
    // and positive entry price. Records with entryTs<=0 or entryPrice<=0 are
    // corrupted (most often: closer fired for a position whose entry record
    // was lost across a restart, leaving entry_ts_unix=0 and entry_px=0.0).
    // Their exit_px - 0 = exit_px math produces six-figure phantom losses
    // that pollute every aggregation. Reject them at the writer.
    if (tr.entryTs <= 0 || tr.entryPrice <= 0.0) {
        printf("[CSV-PHANTOM-BLOCK] symbol=%s engine=%s entryTs=%lld entryPx=%.4f "
               "exitTs=%lld exitPx=%.4f reason=%s -- record dropped (corrupted)\n",
               tr.symbol.c_str(), tr.engine.c_str(),
               (long long)tr.entryTs, tr.entryPrice,
               (long long)tr.exitTs,  tr.exitPrice,
               tr.exitReason.c_str());
        fflush(stdout);
        return;
    }

    // S34 dedupe guard: drop exact-duplicate close writes within the window.
    // This is the writer-side defence against the identical-row pattern in
    // the operator's pasted log (two USTEC BUYs at 13:35:00 with identical
    // entry/exit/hold). Upstream double-emit still gets a one-line warning
    // so the engine bug is visible, but the CSV stays clean.
    if (close_already_written(tr)) {
        printf("[CSV-DUP-BLOCK] engine=%s symbol=%s side=%s entryTs=%lld exitTs=%lld "
               "reason=%s pnl=%.4f net=%.4f -- duplicate close write blocked "
               "(already written within %d sec)\n",
               tr.engine.c_str(), tr.symbol.c_str(), tr.side.c_str(),
               (long long)tr.entryTs, (long long)tr.exitTs,
               tr.exitReason.c_str(), tr.pnl, tr.net_pnl,
               OMEGA_CSV_DEDUPE_WINDOW_SEC);
        fflush(stdout);
        return;
    }

    // S34 PnL sanity diagnostics. Still write the row for audit continuity,
    // but emit a clear warning so the operator can find the upstream cause.
    //   [CSV-ZERO-SIZE] -- engine emitted a close with size<=0. Almost always
    //                      a sizing-table miss (symbol-key mismatch like
    //                      "USTEC" vs "USTEC.F") or a shadow lot pinned to 0.
    //   [CSV-ZERO-PNL]  -- size is positive and entry != exit, but net_pnl
    //                      came out zero. Indicates the cost/multiplier path
    //                      did not run (tick_value_multiplier returned 1.0
    //                      against an unrecognised symbol, or the close
    //                      callback skipped apply_realistic_costs).
    if (tr.size <= 0.0) {
        printf("[CSV-ZERO-SIZE] engine=%s symbol=%s side=%s size=%.4f "
               "entry=%.4f exit=%.4f pnl=%.4f net=%.4f reason=%s "
               "-- zero-size close; PnL will be $0 (check sizing table for symbol)\n",
               tr.engine.c_str(), tr.symbol.c_str(), tr.side.c_str(),
               tr.size, tr.entryPrice, tr.exitPrice,
               tr.pnl, tr.net_pnl, tr.exitReason.c_str());
        fflush(stdout);
    } else if (std::fabs(tr.net_pnl) < 1e-9 &&
               std::fabs(tr.entryPrice - tr.exitPrice) > 1e-6) {
        printf("[CSV-ZERO-PNL] engine=%s symbol=%s side=%s entry=%.4f exit=%.4f "
               "size=%.4f pnl=%.4f net=%.4f reason=%s "
               "-- price moved but PnL is zero (check tick_value_multiplier "
               "for symbol or close-pipeline cost path)\n",
               tr.engine.c_str(), tr.symbol.c_str(), tr.side.c_str(),
               tr.entryPrice, tr.exitPrice, tr.size,
               tr.pnl, tr.net_pnl, tr.exitReason.c_str());
        fflush(stdout);
    }

    const std::string row = build_trade_close_csv_row(tr);
    {
        std::lock_guard<std::mutex> lk(g_trade_close_csv_mtx);
        if (g_trade_close_csv.is_open()) {
            g_trade_close_csv << row << '\n';
            g_trade_close_csv.flush();
        }
    }
    const int64_t bucket_ts = nowSec();  // always use wall-clock for file date -- exitTs could be stale on reload
    if (g_daily_trade_close_log) g_daily_trade_close_log->append_row(bucket_ts, row);
    if (tr.symbol == "XAUUSD" && g_daily_gold_trade_close_log)
        g_daily_gold_trade_close_log->append_row(bucket_ts, row);
}

// ?????????????????????????????????????????????????????????????????????????????
// session_tradeable() -- UTC hour gate for all engine entries
//
// TWO WINDOWS are evaluated. Either window active = trading allowed.
//
// PRIMARY WINDOW  (config: session_start_utc ? session_end_utc)
//   Default: 07:00-22:00 UTC  (London open ? NY close + 1hr buffer)
//   Covers:  London 07:00-16:00, NY 13:00-22:00
//   Supports wrap-through-midnight if start > end (e.g. 22?5)
//   Set start == end to run 24h (not recommended)
//
// ASIA WINDOW  (config: session_asia=true)
//   Fixed:  22:00-05:00 UTC  (Tokyo gold market + NZ/AU morning)
//   Active for gold, silver, oil -- all trade during Tokyo hours
//   Hardcoded range -- not affected by primary window values
//
// DEAD ZONE (intentional gap):
//   05:00-07:00 UTC -- Sydney close to London open
//   Genuinely thin liquidity, wide spreads, no engine runs here
//
// COVERAGE MAP:
//   00:00-05:00  Asia window active      ? trading
//   05:00-07:00  DEAD ZONE               ? blocked
//   07:00-22:00  Primary window active   ? trading
//   22:00-24:00  Asia window active      ? trading
//
// BUG HISTORY:
//   session_end_utc was 21 -- created a silent 21:00-22:00 dead zone each night.
//   Gold, oil, silver, forex all blocked for 1hr despite being open markets.
//   Fixed: session_end_utc raised to 22 -- primary window now hands off directly
//   to Asia window with no gap.
