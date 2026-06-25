// BigCapMomoIbkr.hpp -- THIN interface to the in-process IBKR BigCapMomo engine.
//
// Mirrors IbkrExec.hpp deliberately: this header contains NO TWS API headers --
// only std types + the two pure-std Omega structs the telemetry path needs
// (PositionSnapshot, TradeRecord). The heavy TWS-API impl lives in the single TU
// src/bigcap_momo_ibkr.cpp (the only Omega TU besides ibkr_exec.cpp that pulls in
// the vendored TWS client). This isolation keeps the TWS C <stdlib.h> includes
// out of the giant main Omega TU -- the same pollution that broke std::atoll
// resolution on the first IBKR integration build (2026-06-16).
//
// WHY in-process (not the standalone ibkr/BigCapMomoEngine.cpp exe): the Omega
// GUI running-trades panel is in-process telemetry (g_open_positions + shared
// mem) -- a separate exe cannot inject. So the validated big-cap momentum
// continuation engine (SPY-200MA regime gate + day-expansion gate + ignition +
// vol-surge + WIDE trail; see ibkr/BigCapMomoEngine.cpp header for the backtest)
// runs on its OWN IBKR scanner/data thread inside Omega.exe and reports through
// the same telemetry + shadow-ledger path as every other engine.
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
namespace bigcap_momo_ibkr {

// All knobs are the validated defaults from ibkr/BigCapMomoEngine.cpp / the
// g_bigcap_momo engine_init config. shadow == !paper_only inverse: paper_only
// true => logs only, NO live orders (shadow). port 4001=live data / 4002=paper.
struct Config {
    std::string host        = "127.0.0.1";
    int    port             = 4001;   // IBKR gateway (data); 4002 = paper
    int    client_id        = 86;     // matches the standalone engine's clientId
    bool   paper_only       = true;   // shadow: log trades, route NO live orders
    int    market_data_type = 1;      // IBKR mkt-data type: 1=live 2=frozen 3=delayed 4=delayed-frozen
    long   max_bar_age_sec  = 600;    // S-2026-06-25 STALE-DATA GUARD: refuse entry if the just-closed
                                      // 5m bar's start is older than this vs wall-clock (live completion
                                      // ~= one TF; a bar hours-old = IBKR feed stalled/delayed). 0 = off.
    double gate_pct         = 2.5;    // S-2026-06-24p: align to faithful-validated config (was 3.0). bigcap_momo_faithful full-universe n39 PF6.95 used gate2.5.
    double trail_pct        = 0.04;   // %-trail off peak (0 = off; ATR-trail below replaces it)
    // S-2026-06-18 gain-protect exit — ported from PumpScalpEngine (+52% / PF2.30->4.72,
    // bigcap_exit_compare.cpp). ATR-trail rides + BE-ratchet locks gains + ride-in-profit.
    int    atr_len          = 30;     // ATR-trail length in 5m bars (0 = off)
    double atr_mult         = 5.0;    // S-2026-06-24p: align to faithful-validated config (was 4.0; sweep 5>4>3, wider trail rides winners). trailing stop = peak - atr_mult * ATR ($)
    double giveback_close_frac = 0.0; // S-2026-06-24: bank a runner on a 5m CLOSE retraced this
                                      // frac of peak gain (before the wide trail round-trips it).
                                      // Validated on REAL live trades (coldcut_on_real_trades.py):
                                      // book -$133 -> +$160 @0.33. engine_init sets 0.35 LIVE. 0=off.
    double be_arm_pct       = 0.03;   // arm BE-floor once +3% in profit (fraction)
    double be_floor_pct     = 0.02;   // floor stop at entry +2% (net-breakeven)
    bool   maxhold_skip_if_profit = true;  // don't clock-cut a position still in profit
    double ig_pct           = 3.0;    // ignition: +IG% over LB 5m bars
    // S-2026-06-20 IMPULSE FILTER (the big validated upgrade): the ENTRY bar itself must
    // thrust >= min_impulse_atr * ATR ((bar high - prior close) >= mult*ATR). Filters the
    // weak/stalling breakouts (the +gate% names whose entry bar has no thrust) — on real
    // big-cap 15m data this lifted PF 2.4->5.8, WR 61->73%, maxDD 10.4->6.6% (entry
    // selectivity, NOT exit tinkering). 0 = disable.
    double min_impulse_atr  = 1.0;
    double volx             = 3.0;    // volume surge vs 20-bar avg (0 = disable, bridge-delta caveat)
    double px_min           = 10.0;   // big-cap / deep-liquidity floor (price only)
    // S-2026-06-19 universe risk fix: enforce a real market-cap floor in the IBKR
    // scanner so micro-caps (the slippage-death names PBLS/SHAZ-type that killed
    // PumpScalp) cannot leak in. px_min alone does NOT do this -- a $10 NASDAQ name
    // at 100k vol can be a $100M micro-cap. UNIT GOTCHA: IBKR marketCapAbove is in
    // MILLIONS of USD -- 2000 = $2B (50 rows); the raw 2e9 = the documented 0-rows
    // bug. Big-cap momo is the validated edge (small-cap = slippage death), so this
    // tightens risk WITHOUT touching the edge. 0 = disable.
    double market_cap_above_musd = 20000.0; // $20B floor, in MILLIONS (IBKR unit). S-2026-06-24:
                                             // default raised $100M->$20B to MATCH the engine_init.hpp
                                             // override + the validated bridge floor (MARKETCAP_MIN
                                             // 20000). Config-drift hardening: the old $100M default
                                             // let spiky small-caps (AEHR/SHAZ/PBLS, the give-back
                                             // names) leak in if the engine_init override were ever
                                             // dropped. Default-safe now. Runtime already $20B.
                                             // lowered from 2000 ($2B too tight -> near-zero scan rows)
    int    lb               = 6;      // ignition lookback (6*5m = 30min)
    int    maxhold          = 48;     // 48*5m = 4h backstop
    bool   regime_gate      = true;   // SPY price>SMA200 AND SMA200 rising
    bool   regime_relaxed   = false;  // S-2026-06-24: gate variant. false = BULL-only (production:
                                      //   close>SMA200 AND rising). true = not-BEAR (block ONLY a
                                      //   confirmed downtrend; trade BULL+NEUTRAL). Tier-1 daily proxy:
                                      //   not-BEAR > none > BULL-only on a turtle trend-long. SHADOW
                                      //   A/B ENABLER, default OFF. Env: OMEGA_BIGCAP_RELAXED_GATE=1.
    int    min_breadth      = 2;      // S-2026-06-24p: align to faithful-validated config (was 1). breadth>=2 = the chop/bear gate the full-universe BT used. cross-sectional BREADTH gate: require >= this many
                                      // DISTINCT names igniting same session-day before any entry fires.
                                      // 1 = off. 2 = skip isolated single-name chop false-breakouts AND sit
                                      // out bear (few broad-ignition days). Ported from the faithfully-BT'd
                                      // bridge engine (PumpScalpManager.min_breadth): chop third -12%->-2%.
    double notional_usd     = 1000.0; // per-entry notional (shadow sizing)
    std::string engine_tag  = "BigCapMomo";  // ledger + GUI source label

    // ---- S-2026-06-25 Luke entry-quality gate (OPT-IN, SHADOW by default) ----
    // Faithfully BT'd daily setups A (pullback-to-rising-9/21-EMA) + C (inside-day
    // /micro-VCP breakout) + tight stop-width selectivity, layered as an entry
    // QUALITY filter on the existing 5m ignition trigger. See LukeEntryQuality.hpp
    // + Memory-Omega/wiki/concepts/luke-tight-stop-system.md. IMPL (operator MSVC
    // build): on a candidate ignition, request that name's DAILY bars (same call as
    // the SPY regime feed), call omega::luke::evaluate(); require a valid A/C setup
    // whose structural stop-width <= luke_max_stopw before allowing the entry, and
    // size shares = risk$/(fill-stop). DEFAULT OFF until revalidated at 5m fill
    // resolution (the edge is daily-resolution; do NOT live-size off this alone).
    bool   luke_gate        = false;  // master enable (env OMEGA_BIGCAP_LUKE=1)
    bool   luke_mode_A      = true;   // pullback-to-rising-EMA
    bool   luke_mode_C      = true;   // inside-day / micro-VCP breakout
    double luke_max_stopw   = 0.06;   // tight-stop selectivity cap (validated sweet spot)
    double luke_adr_min     = 4.0;    // high-ADR floor (%), matches scanner intent
    double luke_risk_pct    = 0.005;  // risk fraction/trade for stop-based sizing (video uses 0.5%)
    bool   luke_surge       = true;   // S-2026-06-26 also fire on a GENUINE surge (no armed setup needed):
    double luke_surge_impulse = 1.0;  // entry bar must thrust >= this*ATR (the "genuine" filter). gate_pct+ig_pct still apply.
    // S-2026-06-26 fixed high-ADR watchlist the engine evaluates for A/C setups EVERY day (the
    // daily-swing universe). TOP_PERC_GAIN feeds today's MOVERS; Luke setups form on QUIET names,
    // so the gainer scan never surfaces them (0 setups armed live). Set in engine_init.
    std::vector<std::string> luke_watchlist;
};

// Set config BEFORE start(). Safe to call repeatedly while stopped.
void configure(const Config& cfg);

// Master gate. collect_positions() returns empty and start() refuses to connect
// unless enabled. Lets engine_init register the GUI source unconditionally while
// the runtime decides (env) whether this path is the active one.
void set_enabled(bool on);
bool is_enabled();

// Liveness for the omega_main watchdog: wall-clock ms of the last scanner/bar event
// (0 = never / not running). A connected engine whose feed silently dies during RTH is
// the 06-19 failure class -- the watchdog uses this to raise [SYSTEM-ALERT] BIGCAP_STALE.
long long last_activity_ms();

// Closed-trade sink -> handle_closed_trade (shadow ledger + telemetry). Plain
// std::function, no TWS types. Set in engine_init alongside the other engines.
void set_on_trade_record(std::function<void(const TradeRecord&)> cb);

// Open positions for g_open_positions.register_source -> the live_trades GUI
// panel. Empty unless enabled + connected + holding. Thread-safe (the impl locks
// its book against the TWS reader thread).
std::vector<PositionSnapshot> collect_positions();

// S-2026-06-26 PERSISTENCE: re-adopt a saved open position at boot (PositionPersistence calls this
// before the reader thread connects). Returns true if adopted (the snapshot's engine tag matches).
// No-op returning false unless enabled + OMEGA_WITH_IBKR. The position resumes when its symbol subs.
bool restore_position(const PositionSnapshot& ps);

// Connect to the gateway + start the reader/pump thread (the IBKR scanner data
// thread the handoff calls for). Idempotent: a second call while already running
// is a no-op. Returns false if disabled, OMEGA_WITH_IBKR is undefined, or the
// connection failed.
bool start();

// Signal the reader thread to stop, join it, disconnect. Idempotent.
void stop();

} // namespace bigcap_momo_ibkr
} // namespace omega
