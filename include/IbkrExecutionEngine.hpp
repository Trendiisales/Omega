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
#include "Execution.h"

#include "IbkrExec.hpp"   // omega::IbkrFill (shared, TWS-free)

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
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
    std::string sec_type;       // "FUT" / "CASH"
    std::string exchange;       // "COMEX","CME","CBOT","EUREX","ICEEU","IDEALPRO"
    std::string currency;       // "USD","EUR","GBP"
    double      qty_per_lot = 1.0;   // Omega lot -> IBKR contracts (1 lot = 1 contract)
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

    IbkrExecutionEngine() {
        client_ = std::make_unique<EClientSocket>(this, &signal_);
        init_specs();
    }
    ~IbkrExecutionEngine() { stop_watchdog(); stop(); }

    void init_specs() {
        // FULL-SIZE contracts (operator decision 2026-06-16).
        auto add = [&](const char* om, IbkrContractSpec s) {
            spec_[om] = s; ibkr_to_omega_[s.ibkr_symbol] = om;
        };
        add("XAUUSD",  {"GC",     "FUT",  "COMEX",    "USD", 1.0});
        // Micro gold (10oz) for the small-notional mimic books (S-2026-07-14 operator
        // sizing: 1 MGC). Distinct omega sym so everything routed on "XAUUSD" stays GC.
        add("XAUUSD.M",{"MGC",    "FUT",  "COMEX",    "USD", 1.0});
        add("US500.F", {"ES",     "FUT",  "CME",      "USD", 1.0});
        add("USTEC.F", {"NQ",     "FUT",  "CME",      "USD", 1.0});
        add("NAS100",  {"NQ",     "FUT",  "CME",      "USD", 1.0});
        add("DJ30.F",  {"YM",     "FUT",  "CBOT",     "USD", 1.0});
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
    }

    Contract base_contract(const IbkrContractSpec& s) const {
        Contract c;
        c.symbol   = s.ibkr_symbol;
        c.secType  = s.sec_type;
        c.exchange = s.exchange;
        c.currency = s.currency;
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
        o.totalQuantity = DecimalFunctions::doubleToDecimal(qty);
        if (type == "LMT") o.lmtPrice = px;
        if (type == "STP") o.auxPrice = px;

        long oid = next_id_.fetch_add(1);
        client_->placeOrder(oid, c, o);
        std::printf("[IBKR-EXEC] %s %s %s qty=%.0f type=%s px=%.2f oid=%ld%s\n",
                    paper_only ? "PAPER" : "LIVE", o.action.c_str(), omega_sym.c_str(),
                    qty, type.c_str(), px, oid, paper_only ? " [SHADOW]" : "");
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

    // Authoritative fill event -> reconcile to ledger via on_fill.
    void execDetails(int /*reqId*/, const Contract& contract, const Execution& execution) override {
        IbkrFill f;
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

    void error(int /*id*/, int code, const std::string& msg, const std::string& /*adv*/) override {
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
