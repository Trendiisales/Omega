// QndxSqfIbkr.hpp -- THIN interface to the in-process IBKR QNDX (Nasdaq-100 SQF) book.
//
// Mirrors NqMomoIbkr.hpp / BigCapMomoIbkr.hpp deliberately: NO TWS API headers here --
// only std types + the two pure-std Omega structs the telemetry path needs
// (PositionSnapshot, TradeRecord). The heavy TWS-API impl lives in the single TU
// src/qndx_sqf_ibkr.cpp (the 4th Omega IBKR TU, after ibkr_exec.cpp + bigcap_momo_ibkr.cpp
// + nq_momo_ibkr.cpp, that pulls in the vendored TWS client). Same isolation rule: keeps
// TWS C <stdlib.h> out of the giant main Omega TU.
//
// WHAT this is: the ex-IBKRCrypto "Tradeable Book" folded into Omega. The IBKR AU account
// U23757894 is crypto-INELIGIBLE (2026-07-03): every crypto secType (spot Paxos, QTF/QEF
// SQF) is err201 closing-only, so the ONLY tradeable leg is QNDX -- a Nasdaq-100 Spot-
// Quoted FUT (index, not crypto). This runs its TWO validated orthogonal legs on DAILY
// bars: TSMom50 trend + RSIrev mean-rev (see [[QndxStrat]], params locked WF 2026-06-24).
//
// CADENCE: DAILY-rebalance, NOT intraday. Unlike NqMomo (5m bars), this warms from a daily
// OHLC CSV (/Users/jo/Tick/NDX_daily_2016_2026.csv -- SQF has no IB HMDS history) and
// re-evaluates only on a NEW completed daily close, then routes an idempotent MKT delta to
// the net target across the two legs. Live daily close comes from the QNDX SQF last-trade
// (or the NQ->NDX basis scale the standalone GUI already uses).
//
// ADVERSE-PROTECTION: vol-target sizing (size_mult, 2%/day, clip[0.10,1.50]) + exit-on-turn
// (each leg flips/closes when its OWN signal reverses). NO hard profit-target / cold-loss
// cut BY DESIGN -- a per-trade stop GUTS the trend edge (refuted on gold/futures/crypto;
// see memory + [[QndxStrat]]). Trail-free, stop-free is the BACKTESTED verdict for this
// book, per the Engine Adverse-Protection Mandate.
//
// WHY in-process (not the standalone Crypto/build/ibkrcrypto_engine exe): the Omega GUI
// running-trades panel is in-process telemetry (g_open_positions + shared mem) -- a
// separate exe cannot inject. So the book runs on its OWN IBKR data thread inside Omega.exe
// and reports through the same telemetry + shadow-ledger path as every other engine. The
// standalone :8090 GUI is kept as a READ-ONLY mirror during the transition (operator
// decision 2026-07-03); only the LIVE executor moves here. clientId 88 is REUSED -- the
// Crypto executor must be retired FIRST to free it (strict cutover order).
//
// All entry points are no-ops returning safe defaults unless OMEGA_WITH_IBKR is defined
// for the compiling target (Omega.exe only).
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "OpenPositionRegistry.hpp"   // omega::PositionSnapshot  (pure std types)
#include "OmegaTradeLedger.hpp"       // omega::TradeRecord       (pure std types)

namespace omega {
namespace qndx_sqf_ibkr {

// All knobs are the validated QndxStrat defaults. paper_only true => logs only, NO live
// orders (shadow). port 4001=live gateway / 4002=paper.
struct Config {
    std::string host        = "127.0.0.1";
    int    port             = 4002;   // IBKR gateway; 4002 = paper (default), 4001 = live
    int    client_id        = 88;     // REUSED from the retired Crypto executor (free it first)
    bool   paper_only       = true;   // shadow: log trades, route NO live orders
    int    market_data_type = 1;      // 1=live 2=frozen 3=delayed 4=delayed-frozen

    // ── contract (QNDX = Nasdaq-100 Spot-Quoted FUT) ──
    std::string symbol      = "QNDX";      // CME SQF product code
    std::string sec_type    = "FUT";
    std::string exchange    = "CME";
    std::string currency    = "USD";
    std::string last_trade_month = "";     // e.g. "202612" front month; EMPTY -> resolve front
                                           // via reqContractDetails (SQF product code is ambiguous).
    double point_value      = 0.10;        // $/index-point. QNDX SQF = 0.10x index (mult 0.10).
                                           // PnL = (exit-entry)*point_value*contracts.
    int    base_contracts   = 1;           // base shadow size before vol-target size_mult()

    // ── daily-bar warm source (SQF has no IB HMDS history -> CSV only) ──
    std::string daily_csv   = "/Users/jo/Tick/NDX_daily_2016_2026.csv";  // time,o,h,l,c (headerless, sec ts)

    // ── strategy legs (VERBATIM QndxStrat; do NOT retune without a faithful re-BT) ──
    // Two orthogonal legs: TSMom50 (trend) + RSIrev (mean-rev). Params in QndxStrat::StratParams.

    double cost_pts         = 0.0;    // round-trip cost in index points (LOG ONLY). Roster cost
                                      // 4bps ~= real 2.35bps comm ($0.47/side) + 1bps slip.
    std::string engine_tag  = "QndxSqf";  // ledger + GUI source label (distinct; no TombstoneGuard collision)
};

// Set config BEFORE start(). Safe to call repeatedly while stopped.
void configure(const Config& cfg);

// Master gate. collect_positions() returns empty and start() refuses to connect unless
// enabled. Lets engine_init register the GUI source unconditionally while the runtime
// (env OMEGA_QNDX_IBKR) decides whether this path is active.
void set_enabled(bool on);
bool is_enabled();

// Liveness for the omega_main watchdog: wall-clock ms of the last bar event (0 = never).
long long last_activity_ms();

// Closed-trade sink -> handle_closed_trade (shadow ledger + telemetry).
void set_on_trade_record(std::function<void(const TradeRecord&)> cb);

// Open positions (one per active leg) for g_open_positions.register_source -> the
// live_trades GUI panel. Empty unless enabled + connected + holding. Thread-safe.
std::vector<PositionSnapshot> collect_positions();

// Connect to the gateway + start the reader/pump thread (the book's own IBKR data thread).
// Idempotent. Returns false if disabled, OMEGA_WITH_IBKR undefined, or connect/handshake failed.
bool start();

// Signal the reader thread to stop, join it, disconnect. Idempotent.
void stop();

} // namespace qndx_sqf_ibkr
} // namespace omega
