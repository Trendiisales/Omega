// NqMomoIbkr.hpp -- THIN interface to the in-process IBKR NQ/MNQ futures momentum engine.
//
// Mirrors BigCapMomoIbkr.hpp deliberately: NO TWS API headers here -- only std types
// + the two pure-std Omega structs the telemetry path needs (PositionSnapshot,
// TradeRecord). The heavy TWS-API impl lives in the single TU src/nq_momo_ibkr.cpp
// (the third Omega TU, after ibkr_exec.cpp + bigcap_momo_ibkr.cpp, that pulls in the
// vendored TWS client). Same isolation rule: keeps TWS C <stdlib.h> out of the giant
// main Omega TU (the pollution that broke std::atoll on the first IBKR build).
//
// WHAT this is: intraday momentum-continuation on a SINGLE liquid index future
// (NQ/MNQ) with the BigCapMomo exit chassis (fixed-ATR-at-entry trail + BE-ratchet +
// ride-past-maxhold). LONG-only. Config: IG=0.4% ignition over LB=6 (30min), ATR_LEN=30
// ATR_MULT=4.0, BE_ARM=0.03 BE_FLOOR=0.02, MAXHOLD=48, regime = close > SMA200 of the
// instrument's OWN 5m closes (self-gate; NOT SPY).
//
// !! STATUS: DORMANT -- NOT cost-robust, do NOT enable (leave OMEGA_NQ_IBKR unset). !!
// The original "PF 2.27 @2pt / 1.89 @4pt both-WF-halves+" figures were a TIMESTAMP-PARSER
// ARTIFACT: backtest/momo_cont_nq.cpp read the HHMMSSmmm (millisecond) time field as
// HHMMSS -> tm_hour=t/10000=18000 -> timegm rolled ~750 days + scrambled the intraday
// order -> 78.7M garbage "5m bars" (should be ~184k). SAME bug class that tombstoned the
// old NqMomentumEngine. Re-run on a CORRECT parser (backtest/momo_cont_nq_ls.cpp, 184127
// real 5m bars) = the truth:
//   LONG  2pt: PF 1.34 +10012pt both-WF-halves+ both-regime+ (BEAR PF1.01 breakeven /
//              BULL PF1.64) -- a real but WEAK, bull-skewed, 2024-concentrated edge.
//   LONG  4pt (2x cost): PF 1.28, WF-H1 -459 (neg), BEAR -593 (neg) -> FAILS both-WF-
//              halves+ AND both-regime+ = NOT cost-robust (the deploy bar).
//   SHORT 2pt: PF 0.95 net-NEGATIVE, not both-halves, not both-regime -> DEAD.
// The engine CODE is correct (consumes real IBKR 5m bars; the bug was offline-harness
// only). Kept dormant for a future cost-robust futures edge; shorts will not ship
// without a faithful BT that passes.
//
// DISTINCT from the tombstoned [[NqMomentumEngine]] (g_nq_momentum, on_tick class
// path, DISABLED -- its PF2.34 was a downsample+parser artifact). This is a separate
// in-process IBKR TU with its own validated edge; named nq_momo_ibkr / "NqFutMomo" to
// avoid the TombstoneGuard + any naming collision with that retired engine.
//
// WHY in-process (not a standalone exe / the Aurora :7781 feed): the Omega GUI
// running-trades panel is in-process telemetry (g_open_positions + shared mem) -- a
// separate exe cannot inject. So the engine runs on its OWN IBKR data thread inside
// Omega.exe and reports through the same telemetry + shadow-ledger path as every
// other engine.
//
// ADVERSE-PROTECTION: BE-ratchet (arm0.03/floor0.02) + ATR-trail(30x4.0) is the
// backtested in-flight protection -- a tight cold-loss cut GUTS the trend edge
// (refuted on gold/futures/crypto; see memory). Trail+BE is the validated verdict,
// per the Engine Adverse-Protection Mandate.
//
// All entry points are no-ops returning safe defaults unless OMEGA_WITH_IBKR is
// defined for the compiling target (Omega.exe only).
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "OpenPositionRegistry.hpp"   // omega::PositionSnapshot  (pure std types)
#include "OmegaTradeLedger.hpp"       // omega::TradeRecord       (pure std types)

namespace omega {
namespace nq_momo_ibkr {

// All knobs are the validated momo_cont_nq.cpp defaults. paper_only true => logs only,
// NO live orders (shadow). port 4001=live data gateway / 4002=paper.
struct Config {
    std::string host        = "127.0.0.1";
    int    port             = 4001;   // IBKR gateway (data); 4002 = paper
    int    client_id        = 87;     // distinct from BigCapMomoIbkr (86) so both can run
    bool   paper_only       = true;   // shadow: log trades, route NO live orders
    int    market_data_type = 1;      // 1=live 2=frozen 3=delayed 4=delayed-frozen
    long   max_bar_age_sec  = 600;    // STALE-DATA GUARD: refuse entry if the just-closed 5m bar's
                                      // start is older than this vs wall-clock (feed stalled). 0=off.

    // ── contract (NQ/MNQ index future) ──
    // MNQ = micro NQ = identical signals, $2/pt (vs NQ $20/pt) -> deploy on MNQ for small size.
    std::string symbol      = "MNQ";       // "MNQ" (micro, $2/pt) or "NQ" ($20/pt)
    std::string sec_type    = "FUT";       // "FUT" (needs last_trade_month) or "CONTFUT" (continuous; data-only)
    std::string exchange    = "CME";
    std::string currency    = "USD";
    std::string last_trade_month = "";     // e.g. "202609" for FUT front month. EMPTY -> sec_type forced to
                                           // CONTFUT (continuous-future data so the feed never goes dead on a
                                           // roll). Set the front month (env OMEGA_NQ_IBKR_MONTH) for FUT.
    double point_value      = 2.0;         // $/index-point. MNQ=2.0, NQ=20.0 (PnL = (exit-entry)*pv*contracts)
    int    contracts        = 1;           // shadow size (1 MNQ ~= $40k notional)

    // ── signal (VERBATIM from backtest/momo_cont_nq.cpp -- do NOT retune without a faithful re-BT) ──
    double ig_pct           = 0.4;    // ignition: close up >= IG% over the close LB bars ago
    int    lb               = 6;      // ignition lookback in 5m bars (6 = 30min)
    bool   regime_gate      = true;   // require close > SMA200 of the instrument's OWN 5m closes (self-gate)
    int    regime_sma       = 200;    // SMA length for the self-regime gate (RG in the harness)

    // ── exit chassis (validated: fixed-ATR-at-entry trail + BE-ratchet + ride) ──
    int    atr_len          = 30;     // Wilder ATR length in 5m bars (ATR is captured AT ENTRY and held
                                      // FIXED for the trail -- matches momo_cont_nq.cpp, NOT a live-ATR trail)
    double atr_mult         = 4.0;    // trailing stop = peak - atr_mult * atr_at_entry (index points)
    double be_arm_pct       = 0.03;   // arm BE-floor once +3% in profit (fraction)
    double be_floor_pct     = 0.02;   // floor stop at entry +2% once armed (net-breakeven)
    int    maxhold          = 48;     // 48*5m = 4h backstop
    bool   maxhold_skip_if_profit = true;  // ride a still-profitable winner past the clock

    double cost_pts         = 2.0;    // round-trip cost in index points (LOG ONLY; validated cost-robust to 4pt)
    std::string engine_tag  = "NqFutMomo";  // ledger + GUI source label (distinct from tombstoned NqMomentum)
};

// Set config BEFORE start(). Safe to call repeatedly while stopped.
void configure(const Config& cfg);

// Master gate. collect_positions() returns empty and start() refuses to connect
// unless enabled. Lets engine_init register the GUI source unconditionally while the
// runtime (env OMEGA_NQ_IBKR) decides whether this path is active.
void set_enabled(bool on);
bool is_enabled();

// Liveness for the omega_main watchdog: wall-clock ms of the last bar event (0 = never).
long long last_activity_ms();

// Closed-trade sink -> handle_closed_trade (shadow ledger + telemetry).
void set_on_trade_record(std::function<void(const TradeRecord&)> cb);

// Open position for g_open_positions.register_source -> the live_trades GUI panel.
// Empty unless enabled + connected + holding. Thread-safe.
std::vector<PositionSnapshot> collect_positions();

// Connect to the gateway + start the reader/pump thread (the engine's own IBKR data
// thread). Idempotent. Returns false if disabled, OMEGA_WITH_IBKR undefined, or the
// connection/handshake failed.
bool start();

// Signal the reader thread to stop, join it, disconnect. Idempotent.
void stop();

} // namespace nq_momo_ibkr
} // namespace omega
