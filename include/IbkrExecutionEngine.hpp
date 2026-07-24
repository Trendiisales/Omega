// IbkrExecutionEngine.hpp -- Omega-side NATIVE C++ IBKR order execution (TWS API).
//
// Migration (2026-06-16): operator moving the Omega trading regime off BlackBull/FIX
// onto IBKR. OmegaFIX.hpp is IMMUTABLE and stays untouched -- this is ADDITIVE and
// gated. order_exec.hpp routes order intents here when execution_broker==IBKR.
//
// Models the proven ibkr/IbkrClient.cpp + ibkr/GapShortEngine.cpp pattern (which
// compile against third_party/twsapi/client and run live on the VPS gateway).
//
// CONTRACT TIER: FULL-SIZE CME/Eurex/ICE futures (operator decision 2026-06-16).
//   XAUUSD->GC(COMEX)  US500.F->ES(CME)  USTEC.F/NAS100->NQ(CME)  DJ30.F->YM(CBOT)
//   GER40->DAX(EUREX)  ESTX50->ESTX50(EUREX,FESX)  UK100->Z(ICEEU,FTSE)
//   EURGBP/EURUSD/GBPUSD/etc -> CASH(IDEALPRO)
//   tick_usd values already correct for full-size in ExecutionCostGuard (gold 100,
//   ES 50, NQ 20, YM 5). COMMISSIONS still need real IBKR numbers (Phase 2/3).
//
// SAFETY:
//   * enabled=false by default (execution_broker flag flips it).
//   * paper_only=true by default + HARD refuse if paper_only && port==4001 (live).
//   * quantity sizing uses DecimalFunctions (vendored). Live order SIZING needs the
//     real Intel RDFP lib (bid64 stub today, per ibkr/README) -- paper is fine.
//
// Build wiring (Phase 1, not done here): add TWS_SOURCES + the twsapi include dir
// to the Omega.exe target in CMakeLists (mirror the GapShortEngine target), then
// include this header from engine_init/omega_main and set the on_fill sink.
//
// MSVC-built on the VPS (same as the ibkr/ subsystem). Mac canary cannot fully
// compile (winsock); header is shaped to match the vendored API signatures exactly.
#pragma once

#include "DefaultEWrapper.h"
#include "EClientSocket.h"
#include "EReader.h"
#include "EReaderOSSignal.h"
#include "Contract.h"
#include "Order.h"
#include "OrderState.h"   // S-22i: openOrder override reads st.status (fwd-decl only in EWrapper.h)
#include "Execution.h"

#include "IbkrExec.hpp"   // omega::IbkrFill (shared, TWS-free)
#include "broker_confirmed.hpp"   // omega::g_broker_confirmed — records real fills for the display gate

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace omega {

// IbkrFill is defined in IbkrExec.hpp (shared, TWS-free) so the main TU can
// reference fills without pulling TWS headers.

// Static per-symbol contract spec (full-size). Expiry resolved at runtime via
// reqContractDetails (front month) so we never hardcode a rolling contract month.
struct IbkrContractSpec {
    std::string ibkr_symbol;
    std::string sec_type;       // "FUT" / "CASH" / "STK"
    std::string exchange;       // "COMEX","CME","CBOT","EUREX","ICEEU","IDEALPRO","SMART"
    std::string currency;       // "USD","EUR","GBP"
    double      qty_per_lot = 1.0;   // Omega lot -> IBKR contracts (1 lot = 1 contract)
    // STK only: primary listing exchange, disambiguates SMART routing when a ticker
    // trades on multiple venues (leaving it empty risks contractDetails ambiguity ->
    // an arbitrary "first-wins" resolve). Empty for FUT/CASH.
    std::string primary_exchange;
};

struct IbkrExecutionEngine : public DefaultEWrapper {
    // ---- config (set by engine_init) ----
    std::atomic<bool> enabled{false};       // execution_broker==IBKR
    bool   paper_only = true;               // refuse live orders until explicitly cleared
    std::string host  = "127.0.0.1";
    int    port       = 4002;               // 4002=paper, 4001=live (operator-gated)
    int    client_id  = 70;                 // distinct from data bridges 34/88/99 + ibkr engines 84/85

    // fill sink -> OmegaTradeLedger (wired in engine_init). Decoupled like the
    // engines' on_close callbacks so this header has zero ledger dependency.
    std::function<void(const IbkrFill&)> on_fill;

    // reject sink (gap 3, 2026-07-24): fired with the omega sym when an order is
    // REJECTED by the broker (201/460/10289). The TWS thin interface never routes a
    // reject to the fill callback, so without this an engine believes it is "in
    // position" on a rejected order (phantom-by-reject). Engines wire this to clear
    // the corresponding intent. Decoupled like on_fill (zero engine dependency).
    std::function<void(const std::string&)> on_reject;

    // book-wide exposure cap (gap 4): reject a NEW-symbol entry once the broker
    // already holds this many distinct confirmed positions. Orders for a symbol
    // already held (manage/close) always pass. Generous default; raise explicitly.
    int                                     max_book_positions_ = 12;

    // ── PER-ORDER $ NOTIONAL CAP (gap 8, 2026-07-24). The lot-COUNT size cap clamps
    //    contract count; this bounds DOLLARS. The real "25K" incident was a $ problem
    //    (mis-mapped instrument -> huge notional). A single order whose notional
    //    (send_qty * last_px * multiplier) exceeds this ceiling is REFUSED, not clamped
    //    -- a $-blowout means the sizing/mapping is wrong and the order must not fire.
    //    Default $50k allows one legit future (1 MGC ~$26k, 1 MNQ ~$40k) and blocks 2x+.
    //    Raise explicitly per operator instruction. 0 = disabled. Priced from last_px_;
    //    if no price is known yet the cap FAILS SAFE (blocks) rather than passing blind.
    double                                  max_order_notional_usd_ = 50000.0;

    // ── NATIVE BROKER STOP-ON-FILL (2026-07-24, operator: protections must survive
    //    Omega death). On every OPENING fill, place a RESTING STP order AT THE BROKER
    //    at fill_px*(1-pct). The broker holds it -> it fires autonomously even if Omega
    //    dies. Cancelled on flat (positionEnd, via reqPositions truth) so a stale stop
    //    can't fire into a reverse. The stop qty equals the position, so it bypasses the
    //    min-lot size cap (a stop must match what's held). Long-only for now (BOT->SELL
    //    STP below); short protection is a follow-on.
    bool                                    native_stops_enabled_ = true;
    double                                  native_disaster_stop_pct_ = 15.0;  // % below entry
    std::map<std::string, long>             resting_stop_oid_;    // omega sym -> broker stop orderId
    std::map<std::string, Contract>         stop_contract_;       // omega sym -> contract (for cancel)
    std::map<std::string, double>           broker_avgcost_;      // omega sym -> avg entry (from reqPositions)
    std::map<std::string, Contract>         broker_contract_;     // omega sym -> contract (from reqPositions)
    std::map<std::string, double>           contract_min_tick_;   // omega sym -> minTick (contractDetails)

    // Snap a stop trigger price to the contract's minTick so IBKR never rejects it
    // (error 110: "price does not conform to the minimum price variation"). Rounds
    // AWAY from entry (long stop -> floor, short stop -> ceil) so snapping never makes
    // the disaster stop tighter than intended. Unknown tick (0) -> pass through raw.
    double snap_stop_px_(const std::string& om, double px, bool is_long) const {
        auto it = contract_min_tick_.find(om);
        if (it == contract_min_tick_.end() || it->second <= 0.0) return px;
        const double t = it->second;
        return is_long ? std::floor(px / t) * t : std::ceil(px / t) * t;
    }

    // ---- TWS plumbing ----
    EReaderOSSignal               signal_{2000};
    std::unique_ptr<EClientSocket> client_;
    std::unique_ptr<EReader>      reader_;
    std::thread                   pump_thread_;
    std::atomic<bool>             run_{false};
    std::atomic<bool>             connected_{false};
    std::atomic<long>             next_id_{0};

    // ---- reconnect watchdog (2026-07-03) ----
    // The exec socket previously had NO self-heal: connect() was called once at
    // boot and, if the gateway wasn't up yet (boot-race, IBC auto-login ~110s) OR
    // the socket dropped mid-session (err 509 / connectionClosed), the engine
    // stayed dead for the whole process life and every order was BLOCKED. This
    // thread retries connect() while enabled && !connected_ so a boot-race or a
    // mid-session drop self-recovers without a full Omega restart.
    std::atomic<bool>             wd_run_{false};
    std::thread                   wd_thread_;
    int                           reconnect_secs_ = 15;   // (re)connect cadence

    std::mutex                            mtx_;
    std::map<std::string, IbkrContractSpec> spec_;      // omega sym -> spec
    std::map<std::string, Contract>         resolved_;  // omega sym -> qualified front-month contract
    std::map<int, std::string>              cd_req_;    // reqContractDetails reqId -> omega sym
    std::map<std::string, std::string>      ibkr_to_omega_;  // "GC" -> "XAUUSD" (fill reverse-map)

    // ── ORDER CIRCUIT-BREAKER (2026-07-24) — makes an order-storm PHYSICALLY
    //    IMPOSSIBLE. Root incident: ConnorsRSI2 fired 24,776 unfilled SELL NAS100
    //    in ~40s the instant the exec reconnected (0 fills, but real exposure /
    //    IBKR pacing risk) because there was NO cap between the connect-check and
    //    placeOrder. Two hard limits + a STICKY halt: (a) global > MAX_ORDERS_PER_SEC
    //    in any 1s window, (b) per-symbol >= MAX_UNFILLED_PER_SYM sends with 0 fills.
    //    On trip: circuit_tripped_ + enabled=false -> every place_order returns -1;
    //    one-shot loud log; cleared ONLY by a restart (operator investigates the
    //    runaway first). A real FILL resets that symbol's unfilled counter, so
    //    legit trading that fills never trips.
    static constexpr int          MAX_ORDERS_PER_SEC   = 25;
    static constexpr int          MAX_UNFILLED_PER_SYM = 8;

    // ── PER-ORDER SIZE CAP (2026-07-24, operator: "size caps ... lowest possible
    //    order size/lot size unless changed explicitly by me"). Hard-clamps EVERY
    //    order's quantity to the instrument MINIMUM lot. Minimums differ per
    //    instrument, so a single flat number is WRONG (operator: "gold min lot is
    //    0.001" — a 1.0 gold lot = 100oz ≈ $200K would sail through a unit cap):
    //      • FUT / STK  → fut_stk_max_lot_  (default 1.0 = 1 contract/share; this is
    //        the ~25K mis-sized path last week)
    //      • CASH / CFD → cash_max_lot_     (default 0.001 = gold min lot; FX raise
    //        explicitly via the override map)
    //    A mis-sized order is now PHYSICALLY IMPOSSIBLE — it clamps to the minimum
    //    lot and logs loudly. This is the SIZE analog of the order-storm circuit
    //    breaker (which caps rate/count, NOT size). Raise a cap ONLY on an explicit
    //    operator instruction — per symbol via max_lot_override_[SYM], or the
    //    per-secType default.
    double                              fut_stk_max_lot_ = 1.0;
    double                              cash_max_lot_    = 0.001;
    std::map<std::string, double>       max_lot_override_;   // omega sym -> explicit cap
    double max_lot_for(const std::string& sym, const std::string& secType) const {
        auto it = max_lot_override_.find(sym);
        if (it != max_lot_override_.end()) return it->second;
        return (secType == "FUT" || secType == "STK") ? fut_stk_max_lot_ : cash_max_lot_;
    }
    std::atomic<bool>             circuit_tripped_{false};
    long long                     rate_win_start_ms_{0};   // guarded by mtx_
    int                           rate_win_count_{0};      // guarded by mtx_
    std::map<std::string,int>     unfilled_by_sym_;        // guarded by mtx_
    // B1 fix (2026-07-24 audit): per-symbol unfilled counter is WINDOW-decayed. A slow
    // legit reject (unfunded GER40, metals err-460) must NOT accumulate a lifetime trip
    // that halts the WHOLE desk — only a burst (>=MAX_UNFILLED_PER_SYM within the window)
    // is a runaway. If the last unfilled send for a symbol was > this window ago, its
    // counter resets before the next increment.
    std::map<std::string,long long> unfilled_win_start_ms_;  // guarded by mtx_
    static constexpr long long    UNFILLED_WINDOW_MS   = 30000;   // 30s burst window

    // ── EXTERNAL HALT (2026-07-24): the real-time ACT actuator driven by
    //    tools/ml_loss_miner/sentinel_act.py. The sentinel writes halt_omega.flag
    //    into the control dir on a CAUGHT anomaly (fill-drought / reject-storm /
    //    stale-feed / phantom). This poll (throttled ~1/sec) flips external_halt_ so
    //    place_order BLOCKS all new entries. FAIL-SAFE: existence == halt; halt is
    //    the safe state, so a stale flag errs safe; the operator/sentinel CLEARS the
    //    flag (a deliberate act) to resume. This closes the L4 ACT gap — a monitor
    //    that DOES something, not just alerts.
    std::atomic<bool>             external_halt_{false};
    std::string                   halt_flag_path_ = "C:/Omega/control/halt_omega.flag";
    long long                     last_halt_check_ms_{0};  // guarded by mtx_ (throttle)
    void check_external_halt_() {
        long long now_ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (now_ms - last_halt_check_ms_ < 1000) return;   // throttle to ~1/sec
            last_halt_check_ms_ = now_ms;
        }
        // Halt on EITHER the sentinel flag OR the operator's manual KILL_SWITCH.lock.
        // 2026-07-24b (audit gap #4): the KILL_SWITCH used to be checked ONLY inside
        // PortfolioGuard::can_open_new_position, which the live equity/index books never
        // call -> the panic button did NOT stop the live stock trades. Checking it HERE,
        // at the exec chokepoint, makes it halt EVERY order regardless of engine.
        std::FILE* f  = std::fopen(halt_flag_path_.c_str(), "r");
        std::FILE* ks = std::fopen("C:/Omega/KILL_SWITCH.lock", "r");
        const bool halt_now = (f != nullptr) || (ks != nullptr);
        if (f)  std::fclose(f);
        if (ks) std::fclose(ks);
        if (halt_now) {
            if (!external_halt_.exchange(true)) {
                std::printf("[IBKR-EXEC] *** EXTERNAL HALT *** (%s) -- blocking ALL new orders "
                            "at the exec chokepoint until cleared\n",
                            ks ? "KILL_SWITCH.lock" : "sentinel halt_omega.flag");
                std::fflush(stdout);
            }
        } else if (external_halt_.exchange(false)) {
            std::printf("[IBKR-EXEC] halt cleared -- new orders re-enabled\n");
            std::fflush(stdout);
        }
    }

    void trip_circuit_(const std::string& why) {           // call under mtx_
        if (circuit_tripped_.exchange(true)) return;       // one-shot
        enabled.store(false);                              // hard halt
        std::printf("[IBKR-EXEC] *** CIRCUIT-BREAKER TRIPPED *** %s -- ALL ORDERS HALTED "
                    "(exec disabled; restart + investigate the runaway before re-enabling)\n",
                    why.c_str());
        std::fflush(stdout);
    }

    IbkrExecutionEngine() {
        client_ = std::make_unique<EClientSocket>(this, &signal_);
        init_specs();
    }
    ~IbkrExecutionEngine() { stop_watchdog(); stop(); }

    void init_specs() {
        // MICRO contracts (operator real-money cutover order 2026-07-18: flatten,
        // start from zero, ALL sizes at minimum — supersedes the 2026-06-16
        // full-size GC/ES/NQ/YM decision, which was taken for paper).
        // NOTE: XAUUSD and XAUUSD.M now BOTH resolve to MGC; ibkr_to_omega_ is
        // last-add-wins (MGC -> XAUUSD.M) but fill->ledger reconciliation is
        // keyed by clOrdId (g_live_orders), so attribution stays per-engine.
        auto add = [&](const char* om, IbkrContractSpec s) {
            spec_[om] = s; ibkr_to_omega_[s.ibkr_symbol] = om;
        };
        add("XAUUSD",  {"MGC",    "FUT",  "COMEX",    "USD", 1.0});
        // GOLD = GLD ETF (S-2026-07-22j FINAL: London spot XAUUSD is HARD-BANNED
        // for Australian residents — IBKR product restriction, same class as
        // CFDs; err-460 was permanent, not propagation). GLD under the proven
        // stocks permission tracks gold ~1:1; ~11 shares = the certified 1-oz
        // notional at RT ~5.3bp (cheaper than spot would have been). MGC futures
        // remain the upgrade path once funding covers the margin (probed 30-min).
        add("XAUUSD.S",{"GLD",    "STK",  "SMART",    "USD", 1.0, "ARCA"});
        // CME 1-Ounce Gold future (S-2026-07-23a, operator: "use mgc but minimum
        // lot ... not 10oz" — MGC min IS 10oz; 1OZ is the true minimum futures
        // lot: 1 oz/contract, ~$400 margin, 23h session). Boot preflight
        // verifies the account can trade it before the engine switches to it.
        add("XAUUSD.O",{"1OZ",    "FUT",  "COMEX",    "USD", 1.0});
        // Micro gold (10oz) for the small-notional mimic books (S-2026-07-14 operator
        // sizing: 1 MGC). Distinct omega sym kept for per-book routing/attribution.
        add("XAUUSD.M",{"MGC",    "FUT",  "COMEX",    "USD", 1.0});
        // ── INDEX = ETF PROXIES (S-2026-07-22j FINAL-2, operator: "u can trade
        // smaller amounts on indices — fix this, we need to trade those engines").
        // IBKR-AU: CFDs don't exist for the entity; micro futures margin-wall the
        // account (MNQ NZ$11k / MES 5.8k, broker-signed). The small-size vehicle
        // the account CAN trade under its (proven-VIABLE) stocks permission:
        //   NAS100 -> QQQ (~$560/share)   US500.F -> SPY (~$630/share)
        //   DJ30.F -> DIA (~$430/share)   — 1-share granularity, ~$150-300 margin.
        // Same underlying the engines certified on (tracking corr ~1). PROXY-SCALE
        // NOTE: engines signal in INDEX points; broker fills come back in ETF $ —
        // ledger broker_fill_px columns show ETF scale (cosmetic seam, real cash
        // truth = ibkr_fills.csv + statement; revisit-lot-sizes standing item).
        // USTEC.F stays MNQ futures (explicit futures routing only). GER40 has no
        // AU-tradable small vehicle -> stays DAX FUT (rejects until funded).
        add("US500.F", {"SPY",    "STK",  "SMART",    "USD", 1.0, "ARCA"});
        add("USTEC.F", {"MNQ",    "FUT",  "CME",      "USD", 1.0});
        add("NAS100",  {"QQQ",    "STK",  "SMART",    "USD", 1.0, "NASDAQ"});
        add("DJ30.F",  {"DIA",    "STK",  "SMART",    "USD", 1.0, "ARCA"});
        add("GER40",   {"DAX",    "FUT",  "EUREX",    "EUR", 1.0});
        add("ESTX50",  {"ESTX50", "FUT",  "EUREX",    "EUR", 1.0});
        add("UK100",   {"Z",      "FUT",  "ICEEU",    "GBP", 1.0});
        // FX spot (IDEALPRO): base/quote split handled in base_contract().
        add("EURUSD",  {"EUR",    "CASH", "IDEALPRO", "USD", 1.0});
        add("GBPUSD",  {"GBP",    "CASH", "IDEALPRO", "USD", 1.0});
        add("EURGBP",  {"EUR",    "CASH", "IDEALPRO", "GBP", 1.0});
        add("AUDUSD",  {"AUD",    "CASH", "IDEALPRO", "USD", 1.0});
        add("NZDUSD",  {"NZD",    "CASH", "IDEALPRO", "USD", 1.0});
        add("USDJPY",  {"USD",    "CASH", "IDEALPRO", "JPY", 1.0});

        // ── US single-name equities (STK/SMART) for StockDip + StockTurtle live ──
        // (S-2026-07-19t). Covers the FULL unique roster of BOTH daily-close books
        // (DIP live-11 + DIP ext-11 + TURTLE live-11 + TURTLE ext-14; both families
        // route send_live_order -> ibkr_exec). Symbol == omega sym (US tickers are
        // 1:1). SMART routing + a per-ticker primaryExchange to disambiguate. Shares
        // are integer, no multiplier (qty_per_lot=1). primaryExchange verified at
        // Monday boot via the [IBKR-EXEC] qualified-log line; a name that does NOT
        // qualify is a wrong exchange -> re-map (never fires an order unresolved).
        //
        // NYSE-listed: DELL, TPR, SHOP, NOW, DD, BMY. Everything else NASDAQ.
        auto stk = [&](const char* om, const char* primary) {
            add(om, {om, "STK", "SMART", "USD", 1.0, primary});
        };
        const char* STK_NASDAQ[] = {
            "MU","NVDA","AVGO","CRDO","STX","INTC","AMD","AAPL","MSFT","MRVL",
            "PLTR","META","NFLX","MSTR","AMZN","GOOGL","WDC","SWKS","QCOM","TSLA",
            "CRWD","PANW","ASTS","RKLB" };
        const char* STK_NYSE[] = { "DELL","TPR","SHOP","NOW","DD","BMY" };
        for (const char* om : STK_NASDAQ) stk(om, "NASDAQ");
        for (const char* om : STK_NYSE)   stk(om, "NYSE");
    }

    Contract base_contract(const IbkrContractSpec& s) const {
        Contract c;
        c.symbol   = s.ibkr_symbol;
        c.secType  = s.sec_type;
        c.exchange = s.exchange;
        c.currency = s.currency;
        // STK: disambiguate SMART routing with the primary listing exchange so
        // reqContractDetails resolves to exactly one contract (no first-wins guess).
        if (s.sec_type == "STK" && !s.primary_exchange.empty())
            c.primaryExchange = s.primary_exchange;
        return c;
    }

    // ---- lifecycle ----
    // Self-guarding: a no-op if already fully connected; otherwise tears down any
    // half-open prior session first so a re-connect never move-assigns onto a
    // joinable pump_thread_ (which would std::terminate). Safe to call from the
    // boot path and from the watchdog thread.
    bool connect() {
        if (client_ && client_->isConnected() && connected_.load()) return true;
        if (pump_thread_.joinable() || (client_ && client_->isConnected())) stop();
        if (!client_->eConnect(host.c_str(), port, client_id, false)) {
            std::printf("[IBKR-EXEC] connect FAILED %s:%d cid=%d\n", host.c_str(), port, client_id);
            std::fflush(stdout);
            return false;
        }
        reader_ = std::make_unique<EReader>(client_.get(), &signal_);
        reader_->start();
        run_ = true;
        pump_thread_ = std::thread([this] { pump_loop(); });
        std::printf("[IBKR-EXEC] connected %s:%d cid=%d paper_only=%d\n",
                    host.c_str(), port, client_id, (int)paper_only);
        std::fflush(stdout);
        return true;
    }

    void stop() {
        run_ = false;
        // Disconnect BEFORE joining: eDisconnect closes the socket, the EReader
        // hits EOF and issues the signal, waking a pump parked in waitForSignal.
        // issueSignal() is belt-and-braces in case the socket was already gone.
        // (The old order -- join first -- could hang forever on an idle socket.)
        if (client_ && client_->isConnected()) client_->eDisconnect();
        signal_.issueSignal();
        if (pump_thread_.joinable()) pump_thread_.join();
        connected_ = false;
    }

    // ---- reconnect watchdog ----
    void ensure_watchdog() {
        bool expected = false;
        if (!wd_run_.compare_exchange_strong(expected, true)) return;  // already running
        wd_thread_ = std::thread([this] { watchdog_loop(); });
        std::printf("[IBKR-EXEC] reconnect watchdog started (cadence=%ds)\n", reconnect_secs_);
        std::fflush(stdout);
    }

    void stop_watchdog() {
        wd_run_ = false;
        if (wd_thread_.joinable()) wd_thread_.join();
    }

    void watchdog_loop() {
        while (wd_run_.load()) {
            for (int i = 0; i < reconnect_secs_ && wd_run_.load(); ++i)
                std::this_thread::sleep_for(std::chrono::seconds(1));
            if (!wd_run_.load()) break;
            if (!enabled.load() || connected_.load()) continue;
            std::printf("[IBKR-EXEC] watchdog: exec disconnected -- attempting (re)connect\n");
            std::fflush(stdout);
            connect();  // self-guards teardown of any half-open session
        }
    }

    void pump_loop() {
        while (run_) {
            signal_.waitForSignal();
            if (!run_) break;
            reader_->processMsgs();
        }
    }

    // Request front-month qualification for every spec'd symbol.
    void qualify_all() {
        std::lock_guard<std::mutex> lk(mtx_);
        int rid = 9000;
        for (auto& kv : spec_) {
            cd_req_[rid] = kv.first;
            client_->reqContractDetails(rid++, base_contract(kv.second));
        }
    }

    bool is_resolved(const std::string& omega_sym) {
        std::lock_guard<std::mutex> lk(mtx_);
        return resolved_.count(omega_sym) > 0;
    }

    // ---- order entry (called from order_exec.hpp when execution_broker==IBKR) ----
    // type: "MKT" | "LMT" | "STP".  px: limit (LMT) or stop trigger (STP).
    // Returns the IBKR orderId, or -1 if rejected (disabled / unresolved / blocked).
    long place_order(const std::string& omega_sym, bool is_long, double qty,
                     const std::string& type = "MKT", double px = 0.0) {
        if (!enabled.load()) { return -1; }
        if (circuit_tripped_.load()) { return -1; }   // STICKY hard halt (already logged once)
        check_external_halt_();                        // poll the sentinel control flag (throttled)
        // EXTERNAL HALT blocks new ENTRIES only — a broker-held symbol (exit/manage) always
        // passes so a halt can never strand an open position (risk-reducing orders survive).
        if (external_halt_.load() && !omega::g_broker_confirmed.holds(omega_sym)) {
            std::printf("[IBKR-EXEC] BLOCKED %s -- EXTERNAL HALT active (entries only; held-symbol exits pass)\n",
                        omega_sym.c_str());
            std::fflush(stdout); return -1;
        }
        if (!connected_.load()) {
            std::printf("[IBKR-EXEC] BLOCKED %s -- not connected\n", omega_sym.c_str());
            std::fflush(stdout); return -1;
        }
        if (paper_only && port == 4001) {
            std::printf("[IBKR-EXEC] BLOCKED %s -- paper_only set but on LIVE port 4001\n",
                        omega_sym.c_str());
            std::fflush(stdout); return -1;
        }
        Contract c;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = resolved_.find(omega_sym);
            if (it == resolved_.end()) {
                std::printf("[IBKR-EXEC] BLOCKED %s -- no resolved contract (qualify pending)\n",
                            omega_sym.c_str());
                std::fflush(stdout); return -1;
            }
            c = it->second;
        }
        Order o;
        o.action        = is_long ? "BUY" : "SELL";
        o.orderType     = type;
        // Futures quantity must be a whole number of contracts >= 1. Engine-side
        // "lots" are CFD-convention fractions (0.01 gold etc.); a fractional FUT
        // qty is a silent IBKR reject (REJECTs never reach the fill callback —
        // known thin-interface gap), so the order would vanish without a trace.
        // Operator min-size mandate 2026-07-18: round to integer, floor at 1.
        double send_qty = qty;
        // FUT: whole contracts >=1 (fractional = silent IBKR reject). STK: whole
        // shares >=1 (US equities are integer-share; a fractional qty needs the
        // fractional-shares flag we don't set, so round + floor to 1). Both share
        // the same integer-floor path; CASH (FX) keeps its fractional qty.
        if (c.secType == "FUT" || c.secType == "STK")
            send_qty = std::max(1.0, std::round(qty));
        // ── PER-ORDER SIZE CAP: hard-clamp to the instrument minimum lot (operator
        //    "lowest possible ... lot size unless changed explicitly by me", 2026-07-24).
        //    A mis-sized order (the ~25K last week) is now physically impossible — it
        //    clamps to the minimum and logs loudly so the sizing bug is visible.
        {
            const double cap = max_lot_for(omega_sym, c.secType);
            if (send_qty > cap) {
                std::printf("[SIZE-CAP] %s order lot %.6f CLAMPED to min %.6f "
                            "(lowest-size policy; raise max_lot_override_[%s] explicitly to change)\n",
                            omega_sym.c_str(), send_qty, cap, omega_sym.c_str());
                std::fflush(stdout);
                send_qty = cap;
                if (c.secType == "FUT" || c.secType == "STK")
                    send_qty = std::max(1.0, std::round(send_qty));  // keep integer floor after clamp
            }
        }
        o.totalQuantity = DecimalFunctions::doubleToDecimal(send_qty);
        if (type == "LMT") o.lmtPrice = px;
        if (type == "STP") o.auxPrice = px;

        // ── CIRCUIT-BREAKER: hard rate + per-symbol-unfilled cap (2026-07-24).
        //    Caps a runaway at ~MAX_ORDERS_PER_SEC instead of 24,776. A real FILL
        //    resets unfilled_by_sym_[sym], so legit filling trades never trip.
        {
            std::lock_guard<std::mutex> lk(mtx_);
            long long now_ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            if (now_ms - rate_win_start_ms_ >= 1000) { rate_win_start_ms_ = now_ms; rate_win_count_ = 0; }
            if (++rate_win_count_ > MAX_ORDERS_PER_SEC) {
                trip_circuit_("global rate > " + std::to_string(MAX_ORDERS_PER_SEC) + " orders/sec");
                return -1;
            }
            // B1 fix: window-decay so slow legit rejects don't accumulate a lifetime
            // trip. Reset this symbol's counter if its last unfilled send aged out.
            long long& wstart = unfilled_win_start_ms_[omega_sym];
            if (now_ms - wstart >= UNFILLED_WINDOW_MS) { wstart = now_ms; unfilled_by_sym_[omega_sym] = 0; }
            if (++unfilled_by_sym_[omega_sym] > MAX_UNFILLED_PER_SYM) {
                trip_circuit_(omega_sym + ": " + std::to_string(unfilled_by_sym_[omega_sym]) +
                              " orders in " + std::to_string(UNFILLED_WINDOW_MS/1000) +
                              "s with 0 fills (runaway loop)");
                return -1;
            }
        }
        // ── BOOK-WIDE EXPOSURE CAP (gap 4): refuse a new-symbol entry once the broker
        //    already holds max_book_positions_ distinct positions. An order for a
        //    symbol already held (manage/close) always passes. Uses broker TRUTH
        //    (g_broker_confirmed), so it bounds REAL exposure, not intent (the storm
        //    breaker bounds intent). Fail-safe: 0 confirmed positions => never blocks.
        if (!omega::g_broker_confirmed.holds(omega_sym) &&
            omega::g_broker_confirmed.count() >= (size_t)max_book_positions_) {
            std::printf("[BOOK-CAP] BLOCKED %s -- book already holds %zu positions (cap %d)\n",
                        omega_sym.c_str(), omega::g_broker_confirmed.count(), max_book_positions_);
            std::fflush(stdout);
            return -1;
        }
        // ── PER-ORDER $ NOTIONAL CAP (gap 8): bound DOLLARS, not just contract count.
        //    notional = send_qty * ref_px * multiplier. ref_px = order px (LMT/STP) or
        //    the last traded px. FAILS SAFE: if the ceiling is armed but no price is
        //    known yet, REFUSE (a blind order at unknown notional is exactly the class
        //    that let the 25K through). A close/manage order is NOT exempt -- a $-blowout
        //    on any side is a mapping bug.
        if (max_order_notional_usd_ > 0.0) {
            double ref_px = (px > 0.0) ? px : last_price(omega_sym);
            double mult   = 1.0;
            if (!c.multiplier.empty()) { double m = std::atof(c.multiplier.c_str()); if (m > 0.0) mult = m; }
            if (ref_px <= 0.0) {
                std::printf("[NOTIONAL-CAP] BLOCKED %s -- no price known, cannot bound "
                            "notional (fail-safe)\n", omega_sym.c_str());
                std::fflush(stdout);
                return -1;
            }
            const double notional = send_qty * ref_px * mult;
            if (notional > max_order_notional_usd_) {
                std::printf("[NOTIONAL-CAP] BLOCKED %s -- order notional $%.0f > cap $%.0f "
                            "(qty=%.4f px=%.2f mult=%.0f); raise max_order_notional_usd_ "
                            "explicitly to change\n",
                            omega_sym.c_str(), notional, max_order_notional_usd_,
                            send_qty, ref_px, mult);
                std::fflush(stdout);
                return -1;
            }
        }
        long oid = next_id_.fetch_add(1);
        { std::lock_guard<std::mutex> lk(rej_mtx_);            // gap 3: for reject reconcile
          oid_to_sym_[oid] = omega_sym;
          if (oid_to_sym_.size() > 512) oid_to_sym_.erase(oid_to_sym_.begin()); }
        client_->placeOrder(oid, c, o);
        std::printf("[IBKR-EXEC] %s %s %s qty=%.0f (eng=%.2f) type=%s px=%.2f oid=%ld%s\n",
                    paper_only ? "PAPER" : "LIVE", o.action.c_str(), omega_sym.c_str(),
                    send_qty, qty, type.c_str(), px, oid, paper_only ? " [SHADOW]" : "");
        std::fflush(stdout);
        return oid;
    }

    // ---- EWrapper callbacks (signatures matched to vendored EWrapper_prototypes.h) ----
    void nextValidId(OrderId id) override {
        next_id_ = id;
        connected_ = true;
        std::printf("[IBKR-EXEC] nextValidId=%ld -- qualifying contracts\n", (long)id);
        std::fflush(stdout);
        qualify_all();
        refresh_positions();   // gap 1: reconcile broker truth on every (re)connect
    }

    // ── BROKER POSITION PARITY (gap 1, 2026-07-24): reqPositions() -> position()* ->
    //    positionEnd() rebuilds the AUTHORITATIVE broker holdings into
    //    g_broker_confirmed. This is the truth source the display gate + phantom
    //    parity read — books/desk reflect what the broker ACTUALLY holds, not engine
    //    intent. Kicked on connect (nextValidId) and on demand (refresh_positions()).
    void refresh_positions() {
        if (!client_ || !connected_.load()) return;
        omega::g_broker_confirmed.begin_snapshot();
        client_->reqPositions();
    }
    void position(const std::string& /*account*/, const Contract& contract,
                  Decimal position, double avgCost) override {
        const double qty = DecimalFunctions::decimalToDouble(position);
        std::string om;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = ibkr_to_omega_.find(contract.symbol);
            om = (it != ibkr_to_omega_.end()) ? it->second : contract.symbol;
            if (std::fabs(qty) > 1e-9) {            // capture entry + contract for stop placement
                broker_avgcost_[om]  = avgCost;
                broker_contract_[om] = contract;
            }
        }
        omega::g_broker_confirmed.stage(om, qty);
    }

    // Place a resting native disaster stop for a held LONG that has none. Uses the
    // broker's avg entry. Direct placeOrder (bypasses size cap; stop matches the qty).
    // Place a resting native disaster stop for a held position that has none. Handles
    // BOTH sides (2026-07-24b, audit gap: was long-only -> live shorts were naked):
    //   long  (qty>0) -> SELL STP at entry*(1-pct)   below the market
    //   short (qty<0) -> BUY  STP at entry*(1+pct)   above the market
    void ensure_native_stop_(const std::string& om, double qty) {
        if (!native_stops_enabled_ || std::fabs(qty) < 1e-9) return;
        double entry = 0.0; Contract c;
        { std::lock_guard<std::mutex> lk(mtx_);
          auto a = broker_avgcost_.find(om); auto k = broker_contract_.find(om);
          if (a == broker_avgcost_.end() || k == broker_contract_.end() || a->second <= 0.0) return;
          entry = a->second; c = k->second; }
        { std::lock_guard<std::mutex> lk(rej_mtx_); if (resting_stop_oid_.count(om)) return; }
        const bool is_long = qty > 0.0;
        Order so;
        so.action        = is_long ? "SELL" : "BUY";                 // close side
        so.orderType     = "STP";
        so.auxPrice      = is_long ? entry * (1.0 - native_disaster_stop_pct_ / 100.0)
                                   : entry * (1.0 + native_disaster_stop_pct_ / 100.0);
        // ── SELF-TRIGGER CLAMP (2026-07-24, both-systems parity with Chimera's
        //    pre-boot seed clamp). A held position that moved hard AGAINST the entry
        //    while Omega was down (a long >pct% underwater / a short >pct% up) yields
        //    an entry-anchored stop on the WRONG side of the market — placed, it fires
        //    IMMEDIATELY on arming and force-closes a hold the operator may want to
        //    keep. If a live/delayed last is known, re-anchor the trigger to market so
        //    the stop always rests pct% away on the protective side (never through it).
        //    last_price==0 (not ticked yet at boot) => keep the entry anchor unchanged.
        double mkt = last_price(om);
        if (mkt > 0.0) {
            if (is_long  && so.auxPrice >= mkt) so.auxPrice = mkt * (1.0 - native_disaster_stop_pct_ / 100.0);
            if (!is_long && so.auxPrice <= mkt) so.auxPrice = mkt * (1.0 + native_disaster_stop_pct_ / 100.0);
        }
        so.auxPrice      = snap_stop_px_(om, so.auxPrice, is_long);   // conform to minTick (err 110)
        so.totalQuantity = DecimalFunctions::doubleToDecimal(std::fabs(qty));
        so.tif           = "GTC";
        long soid = next_id_.fetch_add(1);
        client_->placeOrder(soid, c, so);
        { std::lock_guard<std::mutex> lk(rej_mtx_);
          resting_stop_oid_[om] = soid; stop_contract_[om] = c; }
        std::printf("[IBKR-EXEC] NATIVE STOP (held) %s %s STP @ %.2f qty=%.0f oid=%ld "
                    "(broker-side GTC, survives Omega death)\n",
                    om.c_str(), so.action.c_str(), so.auxPrice, std::fabs(qty), soid);
        std::fflush(stdout);
    }
    void positionEnd() override {
        omega::g_broker_confirmed.commit_snapshot();
        // ── PROTECT EVERY HELD POSITION: place a broker-side disaster stop on any held
        //    LONG that has none (covers positions that filled BEFORE the on-fill path
        //    existed, and re-arms if a stop was lost). This is what makes "protections
        //    survive Omega death" true for ALL open trades, not just future fills.
        for (const auto& kv : omega::g_broker_confirmed.all())
            if (std::fabs(kv.second) > 1e-9) ensure_native_stop_(kv.first, kv.second);  // long AND short
        // ── CANCEL NATIVE STOP ON FLAT: for any symbol we placed a resting stop on but
        //    the broker NO LONGER holds (position closed), cancel the stop so a stale
        //    resting order can't fire later into a reverse (naked) position.
        std::vector<std::string> flat_syms;
        {
            std::lock_guard<std::mutex> lk(rej_mtx_);
            for (auto& kv : resting_stop_oid_)
                if (!omega::g_broker_confirmed.holds(kv.first)) flat_syms.push_back(kv.first);
        }
        for (const auto& sym : flat_syms) {
            long oid;
            { std::lock_guard<std::mutex> lk(rej_mtx_);
              oid = resting_stop_oid_[sym];
              resting_stop_oid_.erase(sym); stop_contract_.erase(sym); }
            if (client_) client_->cancelOrder(oid, OrderCancel());
            std::printf("[IBKR-EXEC] native stop cancelled %s oid=%ld (broker flat)\n", sym.c_str(), oid);
            std::fflush(stdout);
        }
        std::printf("[IBKR-EXEC] reqPositions parity: broker holds %zu confirmed position(s)\n",
                    omega::g_broker_confirmed.count());
        std::fflush(stdout);
    }

    // ── SAFE TARGETED CLOSE (2026-07-24): close EXACTLY what the broker holds for one
    //    symbol — never naked. SELL a long / BUY a short of the broker-confirmed qty.
    //    Cancels its resting native stop first (avoid the stop firing on the close).
    //    Returns the close orderId, or -1 if the broker holds nothing.
    long close_broker_position(const std::string& omega_sym) {
        double net = omega::g_broker_confirmed.net_qty(omega_sym);
        if (std::fabs(net) < 1e-9) {
            std::printf("[IBKR-EXEC] close %s -- broker holds nothing, nothing to close\n", omega_sym.c_str());
            std::fflush(stdout); return -1;
        }
        Contract c;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto sc = stop_contract_.find(omega_sym);
            auto rc = resolved_.find(omega_sym);
            if (sc != stop_contract_.end())      c = sc->second;   // exact contract we traded
            else if (rc != resolved_.end())      c = rc->second;
            else { std::printf("[IBKR-EXEC] close %s -- no resolved contract\n", omega_sym.c_str());
                   std::fflush(stdout); return -1; }
        }
        // cancel the resting stop first so it can't also fire on the flatten
        { std::lock_guard<std::mutex> lk(rej_mtx_);
          auto it = resting_stop_oid_.find(omega_sym);
          if (it != resting_stop_oid_.end()) { if (client_) client_->cancelOrder(it->second, OrderCancel());
                                               resting_stop_oid_.erase(it); stop_contract_.erase(omega_sym); } }
        Order o;
        o.action        = (net > 0.0) ? "SELL" : "BUY";
        o.orderType     = "MKT";
        o.totalQuantity = DecimalFunctions::doubleToDecimal(std::fabs(net));
        long oid = next_id_.fetch_add(1);
        client_->placeOrder(oid, c, o);
        std::printf("[IBKR-EXEC] CLOSE %s %s qty=%.0f oid=%ld (exact broker qty, not naked)\n",
                    omega_sym.c_str(), o.action.c_str(), std::fabs(net), oid);
        std::fflush(stdout);
        return oid;
    }

    void contractDetails(int reqId, const ContractDetails& cd) override {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = cd_req_.find(reqId);
        if (it == cd_req_.end()) return;
        const std::string& om = it->second;
        // Front-month: keep the contract with the earliest non-empty expiry seen.
        auto existing = resolved_.find(om);
        const std::string& exp = cd.contract.lastTradeDateOrContractMonth;
        if (existing == resolved_.end() ||
            (!exp.empty() && exp < existing->second.lastTradeDateOrContractMonth)) {
            resolved_[om] = cd.contract;
        }
        if (cd.minTick > 0.0) contract_min_tick_[om] = cd.minTick;  // for stop-price tick-snap
    }

    void contractDetailsEnd(int reqId) override {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = cd_req_.find(reqId);
        if (it != cd_req_.end()) {
            auto r = resolved_.find(it->second);
            std::printf("[IBKR-EXEC] qualified %s -> %s %s\n", it->second.c_str(),
                        r != resolved_.end() ? r->second.symbol.c_str() : "?",
                        r != resolved_.end() ? r->second.lastTradeDateOrContractMonth.c_str() : "");
            std::fflush(stdout);
            cd_req_.erase(it);
        }
    }

    void orderStatus(OrderId orderId, const std::string& status, Decimal filled,
                     Decimal remaining, double avgFillPrice, int permId, int parentId,
                     double lastFillPrice, int clientId, const std::string& whyHeld,
                     double mktCapPrice) override {
        std::printf("[IBKR-EXEC] orderStatus oid=%ld %s filled=%.0f rem=%.0f avg=%.2f\n",
                    (long)orderId, status.c_str(),
                    DecimalFunctions::decimalToDouble(filled),
                    DecimalFunctions::decimalToDouble(remaining), avgFillPrice);
        std::fflush(stdout);
    }

    // ---- open-order inventory + global cancel (S-2026-07-22i: 7 orphan
    // PreSubmitted orders found on the live account during the EXEC-SMOKE
    // margin reject -- they reserve commodities margin; operator ordered
    // identify + delete via API) ----
    void openOrder(OrderId orderId, const Contract& c, const Order& o,
                   const OrderState& st) override {
        if (o.whatIf) {
            // [EXEC-PREFLIGHT] S-22j: whatIf response — margin/commission verdict,
            // NOTHING was executed. This is the per-symbol "can we actually trade
            // it" check (permission errors arrive via error() with this oid).
            std::printf("[EXEC-PREFLIGHT] %s %s VIABLE marginChange=%s commission=%.2f%s%s\n",
                        c.symbol.c_str(), o.action.c_str(),
                        st.initMarginChange.c_str(),
                        st.commission == UNSET_DOUBLE ? 0.0 : st.commission,
                        st.warningText.empty() ? "" : " warn=",
                        st.warningText.c_str());
            std::fflush(stdout);
            return;
        }
        std::printf("[IBKR-EXEC] OPEN-ORDER oid=%ld %s %s %s qty=%.2f type=%s lmt=%.2f aux=%.2f status=%s\n",
                    (long)orderId, o.action.c_str(), c.symbol.c_str(), c.secType.c_str(),
                    DecimalFunctions::decimalToDouble(o.totalQuantity),
                    o.orderType.c_str(), o.lmtPrice, o.auxPrice, st.status.c_str());
        std::fflush(stdout);
    }

    // [EXEC-PREFLIGHT] whatIf order — IBKR evaluates margin + permissions and
    // answers via openOrder(whatIf)/error; NO order is executed. The proper
    // end-to-end account check (operator 2026-07-22: "there are mechanisms to
    // test these end to end, use them").
    long preflight(const std::string& omega_sym, bool is_long, double qty) {
        if (!enabled.load() || !connected_.load()) return -1;
        Contract c;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = resolved_.find(omega_sym);
            if (it == resolved_.end()) {
                std::printf("[EXEC-PREFLIGHT] %s SKIP -- contract not qualified\n", omega_sym.c_str());
                std::fflush(stdout);
                return -1;
            }
            c = it->second;
        }
        Order o;
        o.action = is_long ? "BUY" : "SELL";
        o.orderType = "MKT";
        double send_qty = qty;
        if (c.secType == "FUT" || c.secType == "STK") send_qty = std::max(1.0, std::round(qty));
        o.totalQuantity = DecimalFunctions::doubleToDecimal(send_qty);
        o.whatIf = true;
        long oid = next_id_.fetch_add(1);
        client_->placeOrder(oid, c, o);
        std::printf("[EXEC-PREFLIGHT] %s %s qty=%.0f whatIf sent oid=%ld (verdict follows; nothing executes)\n",
                    omega_sym.c_str(), o.action.c_str(), send_qty, oid);
        std::fflush(stdout);
        return oid;
    }
    void openOrderEnd() override {
        std::printf("[IBKR-EXEC] OPEN-ORDER-END (inventory complete)\n");
        std::fflush(stdout);
    }
    void req_open_orders() { if (client_ && connected_.load()) client_->reqAllOpenOrders(); }
    void global_cancel()   { if (client_ && connected_.load()) client_->reqGlobalCancel(); }

    // ── live position marks (S-2026-07-23a: stock books are daily-close driven,
    // so open positions showed frozen now==entry / $0 PnL intraday; stream a
    // last-price per open-position symbol through this exec connection) ────────
    std::map<std::string, double> last_px_;        // omega sym -> live/delayed last
    std::map<long, std::string>   md_req_;         // tickerId -> omega sym
    std::map<std::string, long>   md_by_sym_;      // omega sym -> tickerId
    long md_next_id_ = 60000;
    void ensure_mktdata(const std::string& omega_sym) {
        if (!client_ || !connected_.load()) return;
        Contract c;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (md_by_sym_.count(omega_sym)) return;      // already streaming
            auto it = resolved_.find(omega_sym);
            if (it == resolved_.end()) return;
            c = it->second;
            const long tid = md_next_id_++;
            md_req_[tid] = omega_sym; md_by_sym_[omega_sym] = tid;
            client_->reqMarketDataType(3);                // delayed OK (falls back to live if subscribed)
            client_->reqMktData(tid, c, "", false, false, TagValueListSPtr());
        }
        std::printf("[IBKR-EXEC] mktdata stream armed %s\n", omega_sym.c_str());
        std::fflush(stdout);
    }
    double last_price(const std::string& omega_sym) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = last_px_.find(omega_sym);
        return it == last_px_.end() ? 0.0 : it->second;
    }
    void tickPrice(TickerId tickerId, TickType field, double price, const TickAttrib&) override {
        // LAST=4, DELAYED_LAST=68, CLOSE=9 (fallback), DELAYED_CLOSE=75
        if (price <= 0.0) return;
        if (field == 4 || field == 68 || field == 9 || field == 75) {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = md_req_.find((long)tickerId);
            if (it != md_req_.end()) {
                // never let a CLOSE tick overwrite a fresher LAST
                if (field == 9 || field == 75) { last_px_.emplace(it->second, price); }
                else                            { last_px_[it->second] = price; }
            }
        }
    }

    // Authoritative fill event -> reconcile to ledger via on_fill.
    void execDetails(int /*reqId*/, const Contract& contract, const Execution& execution) override {
        IbkrFill f;
        f.ts_unix = (long)std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();   // S-23a: fills carried ts=0
        f.ibkr_symbol = contract.symbol;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = ibkr_to_omega_.find(contract.symbol);
            f.omega_symbol = (it != ibkr_to_omega_.end()) ? it->second : contract.symbol;
        }
        f.side     = execution.side;   // "BOT"/"SLD"
        f.qty      = DecimalFunctions::decimalToDouble(execution.shares);
        f.price    = execution.price;
        f.order_id = execution.orderId;
        f.exec_id  = execution.execId;
        std::printf("[IBKR-EXEC] FILL %s(%s) %s qty=%.0f px=%.2f oid=%ld exec=%s\n",
                    f.omega_symbol.c_str(), f.ibkr_symbol.c_str(), f.side.c_str(),
                    f.qty, f.price, f.order_id, f.exec_id.c_str());
        std::fflush(stdout);
        // BROKER-FILL GATE: record this REAL fill so the live-trades display can show
        // ONLY broker-confirmed positions (never engine intent). This is the truth
        // source that ends the phantom-position display.
        omega::g_broker_confirmed.on_fill(f.omega_symbol, f.side, f.qty);
        // ── NATIVE STOP-ON-FILL: a fresh opening fill gets a RESTING broker STP (GTC).
        //    BOTH sides (2026-07-24b): long (BOT) -> SELL STP at fill*(1-pct) below;
        //    short (SLD) -> BUY STP at fill*(1+pct) above. Placed DIRECTLY (bypasses the
        //    size cap — the stop must match the position qty, and must place even during a
        //    halt: it is protective). The broker holds it -> fires even if Omega dies.
        //    Cancelled on flat in positionEnd.
        if (native_stops_enabled_ && (f.side == "BOT" || f.side == "SLD") && f.price > 0.0 &&
            omega::g_broker_confirmed.holds(f.omega_symbol)) {
            bool have;
            { std::lock_guard<std::mutex> lk(rej_mtx_); have = resting_stop_oid_.count(f.omega_symbol) > 0; }
            if (!have) {
                const bool is_long = (f.side == "BOT");
                Order so;
                so.action        = is_long ? "SELL" : "BUY";
                so.orderType     = "STP";
                so.auxPrice      = is_long ? f.price * (1.0 - native_disaster_stop_pct_ / 100.0)
                                           : f.price * (1.0 + native_disaster_stop_pct_ / 100.0);
                so.auxPrice      = snap_stop_px_(f.omega_symbol, so.auxPrice, is_long);  // minTick (err 110)
                so.totalQuantity = execution.shares;
                so.tif           = "GTC";     // survive across sessions until hit or cancelled
                long soid = next_id_.fetch_add(1);
                client_->placeOrder(soid, contract, so);
                { std::lock_guard<std::mutex> lk(rej_mtx_);
                  resting_stop_oid_[f.omega_symbol] = soid;
                  stop_contract_[f.omega_symbol]    = contract; }
                std::printf("[IBKR-EXEC] NATIVE STOP %s %s STP @ %.2f qty=%.0f oid=%ld "
                            "(broker-side GTC, survives Omega death)\n",
                            f.omega_symbol.c_str(), so.action.c_str(), so.auxPrice, f.qty, soid);
                std::fflush(stdout);
            }
        }
        // CIRCUIT-BREAKER: a real fill clears this symbol's unfilled counter so
        // normal filling trades never approach the per-symbol cap.
        { std::lock_guard<std::mutex> lk(mtx_); unfilled_by_sym_[f.omega_symbol] = 0; }
        { std::lock_guard<std::mutex> lk(rej_mtx_); oid_to_sym_.erase(f.order_id); }  // gap 3: filled, drop oid
        refresh_positions();   // gap 1: a fill changed holdings -> re-sync authoritative broker truth
        if (on_fill) on_fill(f);
    }

    // Socket is gone -- flip connected_ so place_order BLOCKS (was a latent bug:
    // connected_ never went false on drop, so orders fired onto a dead socket)
    // and the watchdog re-establishes.
    void connectionClosed() override {
        connected_ = false;
        std::printf("[IBKR-EXEC] connectionClosed -- socket down, watchdog will reconnect\n");
        std::fflush(stdout);
    }

    // S-2026-07-22j: per-oid reject tracking so the GOLD-PROBE can distinguish a
    // clean whatIf (margin fits, permission ok) from one followed by err 201/460.
    std::mutex rej_mtx_;
    std::map<long, bool> rejected_oids_;
    std::map<long, std::string> oid_to_sym_;   // gap 3: orderId -> omega sym, for reject reconcile
    bool preflight_rejected(long oid) {
        std::lock_guard<std::mutex> lk(rej_mtx_);
        auto it = rejected_oids_.find(oid);
        return it != rejected_oids_.end() && it->second;
    }

    void error(int id, int code, const std::string& msg, const std::string& /*adv*/) override {
        if (code == 201 || code == 460 || code == 10289) {
            // ── REJECT RECONCILE (gap 3, 2026-07-24): the thin TWS interface never
            //    routes a reject to the fill callback, so a rejected entry would
            //    linger as phantom "in position". Map the oid back to its symbol and
            //    fire on_reject so the engine CLEARS its intent for that order.
            std::string rsym;
            {
                std::lock_guard<std::mutex> lk(rej_mtx_);
                rejected_oids_[(long)id] = true;
                if (rejected_oids_.size() > 512) rejected_oids_.erase(rejected_oids_.begin());
                auto it = oid_to_sym_.find((long)id);
                if (it != oid_to_sym_.end()) { rsym = it->second; oid_to_sym_.erase(it); }
            }
            if (!rsym.empty()) {
                std::printf("[IBKR-EXEC] REJECT oid=%d code=%d sym=%s -> clearing engine intent\n",
                            id, code, rsym.c_str());
                std::fflush(stdout);
                if (on_reject) on_reject(rsym);
            }
        }
        // Hard socket-level failures mean the connection is dead: mark it so
        // place_order blocks and the watchdog reconnects. (1100 = upstream IB
        // link only, socket still alive -- deliberately NOT treated as a drop.)
        if (code == 504 || code == 509 || code == 1300 || code == 502) {
            connected_ = false;
        }
        // Suppress benign farm-status codes (same set the ibkr/ engines ignore).
        if (code != 2104 && code != 2106 && code != 2158 && code != 2150 &&
            code != 162  && code != 200) {
            std::printf("[IBKR-EXEC] err %d: %s\n", code, msg.c_str());
            std::fflush(stdout);
        }
    }
};

} // namespace omega
