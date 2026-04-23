#pragma once
// order_exec.hpp -- extracted from main.cpp
// Section: order_exec (original lines 2134-2855)
// SINGLE-TRANSLATION-UNIT include -- only include from main.cpp

static std::string send_limit_order(const std::string& symbol, bool is_long,
                                    double qty, double limit_px) {
    if (g_cfg.mode != "LIVE") return {};
    if (!g_trade_ready.load()) {
        std::cerr << "[LIMIT-ORDER] BLOCKED -- trade session not ready\n";
        return {};
    }
    const int sym_id = symbol_name_to_id(symbol);
    if (sym_id <= 0) {
        std::cerr << "[LIMIT-ORDER] BLOCKED -- no numeric ID for " << symbol << "\n";
        return {};
    }
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string clOrdId = "OML-" + std::to_string(nowSec())
                               + "-" + std::to_string(g_order_id_counter++);
    std::string msg;
    {
        std::lock_guard<std::mutex> lk(g_trade_mtx);
        if (!g_trade_ssl) {
            std::cerr << "[LIMIT-ORDER] BLOCKED -- trade SSL null\n";
            return {};
        }
        msg = build_limit_order_single(g_trade_seq++, clOrdId, sym_id, is_long, qty, limit_px);
        const int w = SSL_write(g_trade_ssl, msg.c_str(), static_cast<int>(msg.size()));
        if (w <= 0) {
            std::cerr << "[LIMIT-ORDER] SSL_write failed for " << symbol << "\n";
            return {};
        }
    }
    // Track for cancel fallback
    {
        std::lock_guard<std::mutex> lk(g_pending_limits_mtx);
        PendingLimitOrder plo;
        plo.symbol    = symbol;
        plo.is_long   = is_long;
        plo.qty       = qty;
        plo.limit_px  = limit_px;
        plo.sent_ms   = now_ms;
        plo.expire_ms = now_ms + LIMIT_ORDER_TIMEOUT_MS;
        g_pending_limits[clOrdId] = plo;
    }
    // Also register in g_live_orders for ACK tracking
    {
        std::lock_guard<std::mutex> lk(g_live_orders_mtx);
        LiveOrderRecord rec;
        rec.clOrdId = clOrdId;
        rec.symbol  = symbol;
        rec.side    = is_long ? "LONG" : "SHORT";
        rec.qty     = qty;
        rec.price   = limit_px;
        rec.ts      = nowSec();
        g_live_orders[clOrdId] = rec;
    }
    std::printf("[LIMIT-SENT] %s %s qty=%.2f limit=%.5f clOrdId=%s\n",
                symbol.c_str(), is_long ? "BUY" : "SELL",
                qty, limit_px, clOrdId.c_str());
    std::fflush(stdout);
    return clOrdId;
}

// Send a live market order. Does nothing in SHADOW mode.
// Returns clOrdId on success, empty string on failure/shadow.
static std::string send_live_order(const std::string& symbol, bool is_long,
                                   double qty, double mid_price) {
    // Hard SHADOW gate -- never send in shadow regardless of anything else
    if (g_cfg.mode != "LIVE") return {};

    if (!g_trade_ready.load()) {
        std::cerr << "[ORDER] BLOCKED -- trade session not ready\n";
        return {};
    }

    const int sym_id = symbol_name_to_id(symbol);
    if (sym_id <= 0) {
        std::cerr << "[ORDER] BLOCKED -- no numeric ID for symbol " << symbol << "\n";
        return {};
    }

    const std::string clOrdId = "OM-" + std::to_string(nowSec())
                               + "-" + std::to_string(g_order_id_counter++);

    std::string msg;
    {
        std::lock_guard<std::mutex> lk(g_trade_mtx);
        if (!g_trade_ssl) {
            std::cerr << "[ORDER] BLOCKED -- trade SSL null\n";
            return {};
        }
        msg = build_new_order_single(g_trade_seq++, clOrdId, sym_id, is_long, qty);
        const int w = SSL_write(g_trade_ssl, msg.c_str(), static_cast<int>(msg.size()));
        if (w <= 0) {
            std::cerr << "[ORDER] SSL_write failed for " << symbol << "\n";
            return {};
        }
    }

    // Record for ACK tracking
    {
        std::lock_guard<std::mutex> lk(g_live_orders_mtx);
        LiveOrderRecord rec;
        rec.clOrdId = clOrdId;
        rec.symbol  = symbol;
        rec.side    = is_long ? "LONG" : "SHORT";
        rec.qty     = qty;
        rec.price   = mid_price;
        rec.ts      = nowSec();
        g_live_orders[clOrdId] = rec;
    }

    std::cout << "\033[1;33m[ORDER-SENT] " << symbol
              << " " << (is_long ? "BUY" : "SELL")
              << " qty=" << qty
              << " mid=" << std::fixed << std::setprecision(4) << mid_price
              << " clOrdId=" << clOrdId
              << "\033[0m\n";
    std::cout.flush();

    // Note: fill quality is recorded in handle_execution_report when the actual
    // fill price arrives via ExecutionReport (tag 31 LastPx). Not recorded here.

    return clOrdId;
}

// Cancel an open order by clOrdId (FIX 35=F OrderCancelRequest).
// Used by bracket engine to cancel the unfilled leg after the other fills.
static void send_cancel_order(const std::string& clOrdId) {
    if (g_cfg.mode != "LIVE") return;
    if (clOrdId.empty()) return;

    // Collect order info under g_live_orders_mtx, then release before acquiring g_trade_mtx
    // to avoid lock-order inversion (send_live_order takes g_trade_mtx then g_live_orders_mtx).
    std::string cancelSymbol, cancelSide;
    double cancelQty = 0.0;
    int sym_id = 0;
    {
        std::lock_guard<std::mutex> lk(g_live_orders_mtx);
        auto it = g_live_orders.find(clOrdId);
        if (it == g_live_orders.end()) return;
        if (it->second.acked || it->second.rejected) return; // already done
        cancelSymbol = it->second.symbol;
        cancelSide   = it->second.side;
        cancelQty    = it->second.qty;
        sym_id       = symbol_name_to_id(cancelSymbol);
    }
    if (sym_id <= 0) return;

    const std::string cancelClOrdId = "CX-" + std::to_string(nowSec())
                                    + "-" + std::to_string(g_order_id_counter++);

    // Build and send cancel under g_trade_mtx -- protects g_trade_seq and g_trade_ssl
    {
        std::lock_guard<std::mutex> lk(g_trade_mtx);
        if (!g_trade_ssl) return;

        std::ostringstream b;
        b << "35=F\x01"
          << "49=" << g_cfg.sender << "\x01"
          << "56=" << g_cfg.target << "\x01"
          << "50=TRADE\x01" << "57=TRADE\x01"
          << "34=" << g_trade_seq++ << "\x01"
          << "52=" << timestamp() << "\x01"
          << "41=" << clOrdId << "\x01"              // OrigClOrdID
          << "11=" << cancelClOrdId << "\x01"        // new ClOrdID for the cancel
          << "55=" << sym_id << "\x01"
          << "54=" << (cancelSide == "LONG" ? "1" : "2") << "\x01"
          << "38=" << std::fixed << std::setprecision(2) << cancelQty << "\x01"
          << "60=" << timestamp() << "\x01";
        const std::string msg = wrap_fix(b.str());

        SSL_write(g_trade_ssl, msg.c_str(), static_cast<int>(msg.size()));
    }
    std::cout << "[ORDER-CANCEL] clOrdId=" << clOrdId
              << " sym=" << cancelSymbol
              << " side=" << cancelSide << "\n";
    std::cout.flush();
}

// =============================================================================
//  send_hard_stop_order() -- Phase 1 hard stop implementation
//
//  Design:
//    Sends a LIMIT order at hard_sl_px for qty lots on the CLOSING side.
//    For a LONG position: sends a SELL LIMIT at hard_sl_px.
//    For a SHORT position: sends a BUY  LIMIT at hard_sl_px.
//
//    Tombstone guard: once fired for a given position (keyed by symbol + entry_ts),
//    it NEVER fires again for the same position. This prevents:
//      1. Re-firing on every tick after first send
//      2. Re-firing after a partial close changes pos.size
//
//    STAIR size tracking: uses the CURRENT pos.size (remaining after partials).
//    A hard stop placed on the full original size after step 1 would over-close.
//    Caller passes current_qty so the hard stop matches the live remaining exposure.
//
//    SHADOW mode: sends nothing. Logs [HARD-STOP-SHADOW] to confirm the logic
//    would have fired and with what parameters.
//
//    Returns clOrdId on success, empty on shadow/error.
//
//  Usage (call from manage_position or a dedicated risk watchdog):
//    send_hard_stop_order("XAUUSD", false, pos.size, pos.entry - hard_sl_pts, pos.entry_ts);
//    // is_long=false for SHORT position: closing side is BUY
// =============================================================================

// Tombstone: maps "SYMBOL:entry_ts_sec" -> true if hard stop already sent for this position.
// Prevents double-firing across ticks. Cleared on position close (see clear_hard_stop_tombstone).
static std::mutex                          g_hard_stop_mtx;
static std::unordered_map<std::string, bool> g_hard_stop_tombstone;

static std::string make_hard_stop_key(const std::string& symbol, int64_t entry_ts_sec) {
    return symbol + ":" + std::to_string(entry_ts_sec);
}

// clear_hard_stop_tombstone() -- call when a position closes to free the tombstone entry.
// Prevents the map from growing unbounded across many trades.
static void clear_hard_stop_tombstone(const std::string& symbol, int64_t entry_ts_sec) {
    std::lock_guard<std::mutex> lk(g_hard_stop_mtx);
    g_hard_stop_tombstone.erase(make_hard_stop_key(symbol, entry_ts_sec));
}

// send_hard_stop_order():
//   symbol       - trading symbol (e.g. "XAUUSD")
//   pos_is_long  - true = position is LONG (closing order is SELL), false = SHORT (BUY)
//   current_qty  - CURRENT remaining position size after any partials (use pos.size)
//   hard_sl_px   - absolute price level of the hard stop
//   entry_ts_sec - position entry timestamp in seconds (tombstone key)
//
// Returns clOrdId on LIVE send, "[SHADOW]" string on shadow fire, "" on tombstone block.
static std::string send_hard_stop_order(const std::string& symbol, bool pos_is_long,
                                        double current_qty, double hard_sl_px,
                                        int64_t entry_ts_sec) {
    // Validate inputs
    if (current_qty <= 0.0 || hard_sl_px <= 0.0) {
        printf("[HARD-STOP] BLOCKED -- invalid qty=%.4f sl_px=%.4f\n",
               current_qty, hard_sl_px);
        fflush(stdout);
        return {};
    }

    // ?? Tombstone guard: never fire twice for the same position ???????????????
    const std::string key = make_hard_stop_key(symbol, entry_ts_sec);
    {
        std::lock_guard<std::mutex> lk(g_hard_stop_mtx);
        if (g_hard_stop_tombstone.count(key)) {
            // Already sent for this position -- silent return (fires every tick otherwise)
            return {};
        }
        g_hard_stop_tombstone[key] = true;
    }

    // Closing side: LONG position closes with SELL, SHORT position closes with BUY
    const bool closing_is_buy = !pos_is_long;

    // SHADOW mode: log and return without sending
    if (g_cfg.mode != "LIVE") {
        printf("[HARD-STOP-SHADOW] %s %s qty=%.4f hard_sl=%.4f entry_ts=%lld -- would send %s LIMIT\n",
               symbol.c_str(), pos_is_long ? "LONG" : "SHORT",
               current_qty, hard_sl_px, (long long)entry_ts_sec,
               closing_is_buy ? "BUY" : "SELL");
        fflush(stdout);
        return "[SHADOW]";
    }

    // LIVE: trade session must be ready
    if (!g_trade_ready.load()) {
        printf("[HARD-STOP] BLOCKED -- trade session not ready (symbol=%s)\n", symbol.c_str());
        fflush(stdout);
        // Un-tombstone so it can retry next tick when session recovers
        std::lock_guard<std::mutex> lk(g_hard_stop_mtx);
        g_hard_stop_tombstone.erase(key);
        return {};
    }

    const int sym_id = symbol_name_to_id(symbol);
    if (sym_id <= 0) {
        printf("[HARD-STOP] BLOCKED -- no numeric ID for symbol %s\n", symbol.c_str());
        fflush(stdout);
        std::lock_guard<std::mutex> lk(g_hard_stop_mtx);
        g_hard_stop_tombstone.erase(key);
        return {};
    }

    const std::string clOrdId = "HS-" + std::to_string(nowSec())
                               + "-" + std::to_string(g_order_id_counter++);

    std::string msg;
    {
        std::lock_guard<std::mutex> lk(g_trade_mtx);
        if (!g_trade_ssl) {
            printf("[HARD-STOP] BLOCKED -- trade SSL null (symbol=%s)\n", symbol.c_str());
            fflush(stdout);
            std::lock_guard<std::mutex> lk2(g_hard_stop_mtx);
            g_hard_stop_tombstone.erase(key);
            return {};
        }
        // Use build_limit_order_single: LIMIT order at hard_sl_px on the closing side
        msg = build_limit_order_single(g_trade_seq++, clOrdId, sym_id,
                                       closing_is_buy, current_qty, hard_sl_px);
        const int w = SSL_write(g_trade_ssl, msg.c_str(), static_cast<int>(msg.size()));
        if (w <= 0) {
            printf("[HARD-STOP] SSL_write failed for %s\n", symbol.c_str());
            fflush(stdout);
            // Un-tombstone -- send failed, allow retry
            std::lock_guard<std::mutex> lk2(g_hard_stop_mtx);
            g_hard_stop_tombstone.erase(key);
            return {};
        }
    }

    // Register in live orders for ACK tracking
    {
        std::lock_guard<std::mutex> lk(g_live_orders_mtx);
        LiveOrderRecord rec;
        rec.clOrdId = clOrdId;
        rec.symbol  = symbol;
        rec.side    = closing_is_buy ? "LONG" : "SHORT";  // closing side
        rec.qty     = current_qty;
        rec.price   = hard_sl_px;
        rec.ts      = nowSec();
        g_live_orders[clOrdId] = rec;
    }

    printf("\033[1;31m[HARD-STOP] SENT %s %s qty=%.4f LIMIT@%.4f clOrdId=%s entry_ts=%lld\033[0m\n",
           symbol.c_str(), pos_is_long ? "LONG(sell-stop)" : "SHORT(buy-stop)",
           current_qty, hard_sl_px, clOrdId.c_str(), (long long)entry_ts_sec);
    fflush(stdout);
    return clOrdId;
}

// Check pending limit orders and cancel any that have exceeded LIMIT_ORDER_TIMEOUT_MS.
// Call this every tick from the main tick handler.
static void check_pending_limits() {
    if (g_cfg.mode != "LIVE") return;
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::vector<std::string> to_cancel;
    {
        std::lock_guard<std::mutex> lk(g_pending_limits_mtx);
        for (auto& kv : g_pending_limits) {
            if (kv.second.filled || kv.second.cancelled) continue;
            if (now_ms >= kv.second.expire_ms) {
                to_cancel.push_back(kv.first);
            }
        }
    }
    for (const auto& clOrdId : to_cancel) {
        std::printf("[LIMIT-CANCEL] %s timeout=%lldms -- sending cancel\n",
                    clOrdId.c_str(), (long long)LIMIT_ORDER_TIMEOUT_MS);
        std::fflush(stdout);
        send_cancel_order(clOrdId);
        {
            std::lock_guard<std::mutex> lk(g_pending_limits_mtx);
            auto it = g_pending_limits.find(clOrdId);
            if (it != g_pending_limits.end()) it->second.cancelled = true;
        }
    }
    // Prune old filled/cancelled entries older than 60s
    {
        std::lock_guard<std::mutex> lk(g_pending_limits_mtx);
        for (auto it = g_pending_limits.begin(); it != g_pending_limits.end(); ) {
            if ((it->second.filled || it->second.cancelled)
                && now_ms - it->second.sent_ms > 60000)
                it = g_pending_limits.erase(it);
            else
                ++it;
        }
    }
}

// Mark a pending limit order as filled (call from handle_execution_report).
static void pending_limit_filled(const std::string& clOrdId) {
    std::lock_guard<std::mutex> lk(g_pending_limits_mtx);
    auto it = g_pending_limits.find(clOrdId);
    if (it != g_pending_limits.end()) {
        it->second.filled = true;
        std::printf("[LIMIT-FILLED] %s\n", clOrdId.c_str());
        std::fflush(stdout);
    }
}


static void handle_execution_report(const std::string& msg) {
    const std::string clOrdId  = extract_tag(msg, "11");
    const std::string ordStatus= extract_tag(msg, "39"); // 0=New,1=PartFill,2=Fill,8=Rejected
    const std::string execType = extract_tag(msg, "150");
    const std::string text     = extract_tag(msg, "58");
    const std::string symbol   = extract_tag(msg, "55");
    const std::string side     = extract_tag(msg, "54");
    const std::string lastPx   = extract_tag(msg, "31");
    const std::string lastQty  = extract_tag(msg, "32");

    std::cout << "[ORDER-ACK] clOrdId=" << clOrdId
              << " status=" << ordStatus
              << " execType=" << execType
              << " sym=" << symbol
              << " side=" << side
              << " lastPx=" << lastPx
              << " lastQty=" << lastQty
              << (text.empty() ? "" : " text=" + text) << "\n";
    std::cout.flush();

    if (!clOrdId.empty()) {
        std::lock_guard<std::mutex> lk(g_live_orders_mtx);
        auto it = g_live_orders.find(clOrdId);
        if (it != g_live_orders.end()) {
            if (ordStatus == "8") {
                it->second.rejected = true;
                std::cerr << "[ORDER-REJECT] " << it->second.symbol
                          << " " << it->second.side
                          << " REJECTED text=" << text << "\n";
                std::cerr.flush();
                // CRITICAL FIX: notify bracket engines of rejection so they
                // don't stay stuck in PENDING with no open broker position.
                if (it->second.symbol == "XAUUSD")   g_bracket_gold.on_reject();
                if (it->second.symbol == "US500.F")  g_bracket_sp.on_reject();
                if (it->second.symbol == "USTEC.F")  g_bracket_nq.on_reject();
                if (it->second.symbol == "DJ30.F")   g_bracket_us30.on_reject();
                if (it->second.symbol == "NAS100")   g_bracket_nas100.on_reject();
                if (it->second.symbol == "GER40")    g_bracket_ger30.on_reject();
                if (it->second.symbol == "UK100")    g_bracket_uk100.on_reject();
                if (it->second.symbol == "ESTX50")   g_bracket_estx50.on_reject();
                if (it->second.symbol == "BRENT")  g_bracket_brent.on_reject();
                if (it->second.symbol == "EURUSD")   g_bracket_eurusd.on_reject();
                if (it->second.symbol == "GBPUSD")   g_bracket_gbpusd.on_reject();
                if (it->second.symbol == "AUDUSD")   g_bracket_audusd.on_reject();
                if (it->second.symbol == "NZDUSD")   g_bracket_nzdusd.on_reject();
                if (it->second.symbol == "USDJPY")   g_bracket_usdjpy.on_reject();
            } else if (ordStatus == "0" || ordStatus == "1" || ordStatus == "2") {
                it->second.acked = true;
                // Mark limit order as filled so cancel fallback does not fire
                if (ordStatus == "2" || ordStatus == "1") pending_limit_filled(clOrdId);
                if (!lastPx.empty() && !lastQty.empty()) {
                    try {
                        const double fill_px  = std::stod(lastPx);
                        const double fill_qty = std::stod(lastQty);
                        if (fill_px > 0.0 && fill_qty > 0.0) {
                            const bool is_long_fill = (it->second.side == "LONG");
                            if (it->second.symbol == "XAUUSD") {
                                g_bracket_gold.confirm_fill(is_long_fill, fill_px, fill_qty);
                                // Cancel other leg -- engine fires cancel_order_fn internally
                                const std::string& cancel_id = is_long_fill
                                    ? g_bracket_gold.pending_short_clOrdId
                                    : g_bracket_gold.pending_long_clOrdId;
                                if (!cancel_id.empty()) send_cancel_order(cancel_id);
                            }
                            auto fill_bracket = [&](auto& beng) {
                                beng.confirm_fill(is_long_fill, fill_px, fill_qty);
                                const std::string& cid = is_long_fill
                                    ? beng.pending_short_clOrdId : beng.pending_long_clOrdId;
                                if (!cid.empty()) send_cancel_order(cid);
                            };
                            if (it->second.symbol == "US500.F")  fill_bracket(g_bracket_sp);
                            if (it->second.symbol == "USTEC.F")  fill_bracket(g_bracket_nq);
                            if (it->second.symbol == "DJ30.F")   fill_bracket(g_bracket_us30);
                            if (it->second.symbol == "NAS100")   fill_bracket(g_bracket_nas100);
                            if (it->second.symbol == "GER40")    fill_bracket(g_bracket_ger30);
                            if (it->second.symbol == "UK100")    fill_bracket(g_bracket_uk100);
                            if (it->second.symbol == "ESTX50")   fill_bracket(g_bracket_estx50);
                            if (it->second.symbol == "BRENT")  fill_bracket(g_bracket_brent);
                            if (it->second.symbol == "EURUSD")   fill_bracket(g_bracket_eurusd);
                            if (it->second.symbol == "GBPUSD")   fill_bracket(g_bracket_gbpusd);
                            if (it->second.symbol == "AUDUSD")   fill_bracket(g_bracket_audusd);
                            if (it->second.symbol == "NZDUSD")   fill_bracket(g_bracket_nzdusd);
                            if (it->second.symbol == "USDJPY")   fill_bracket(g_bracket_usdjpy);

                            // ?? Fill quality: update with actual fill price ???
                            // Compares fill_px to the signal mid recorded at send_live_order.
                            // Detects adverse selection when fills consistently exceed mid.
                            g_edges.fill_quality.record_fill(
                                it->second.symbol,
                                it->second.price,  // signal mid at order send time
                                fill_px,           // actual fill from ExecutionReport
                                (it->second.side == "LONG"),
                                nowSec());
                        }
                    } catch (...) {}
                }
            }
        }
    }
}

static std::vector<std::pair<int, std::string>> parse_security_list_entries(const std::string& msg) {
    std::vector<std::pair<int, std::string>> out;
    int current_id = 0;
    size_t pos = 0;
    while (pos < msg.size()) {
        const size_t eq = msg.find('=', pos);
        if (eq == std::string::npos) break;
        const size_t soh = msg.find('\x01', eq + 1);
        if (soh == std::string::npos) break;
        const std::string tag = msg.substr(pos, eq - pos);
        const std::string val = msg.substr(eq + 1, soh - (eq + 1));
        if (tag == "55") {
            try { current_id = std::stoi(val); } catch (...) { current_id = 0; }
        } else if (tag == "1007" && current_id > 0 && !val.empty()) {
            out.emplace_back(current_id, val);
            current_id = 0;
        }
        pos = soh + 1;
    }
    return out;
}

static bool apply_security_list_symbol_map(const std::vector<std::pair<int, std::string>>& entries) {
    bool ext_changed = false;
    std::lock_guard<std::mutex> lk(g_symbol_map_mtx);
    for (const auto& entry : entries) {
        const int id = entry.first;
        const std::string& name = entry.second;
        if (id <= 0 || name.empty()) continue;

        // Special handling for XAUUSD: we subscribe to ID 41 (XAUUSD spot) and
        // route it internally as "XAUUSD". The SecurityList also contains:
        //   ID 41   -> "XAUUSD"  (spot  -- this is what we want, must map to "XAUUSD")
        //   ID 2660 -> "XAUUSD"  (futures, ~$25 above spot -- must NOT route to engines)
        // Without intervention, SecurityList would overwrite g_id_to_sym[41]="XAUUSD"
        // (breaking spot routing) and g_id_to_sym[2660]="XAUUSD" (injecting futures
        // price into gold engines whenever a 2660 tick arrives).
        if (id == 41) {
            // Force spot ID 41 to always resolve to internal name "XAUUSD"
            g_id_to_sym[41] = "XAUUSD";
        } else if (name == "XAUUSD") {
            // Block stray futures ticks from broker FIX feed (ID 2660) -- routed to dead key
            g_id_to_sym[id] = "XAUUSD.FUTURES.IGNORED";
        } else {
            g_id_to_sym[id] = name;
        }

        for (int i = 0; i < OMEGA_NSYMS; ++i) {
            if (name == OMEGA_SYMS[i].name && OMEGA_SYMS[i].id != id) {
                // XAUUSD is pinned to ID 41 (XAUUSD spot). The SecurityList also
                // contains ID 2660 named "XAUUSD" (futures, ~$25 above spot).
                // Block that from overwriting our intentional spot subscription.
                if (std::string(OMEGA_SYMS[i].name) == "XAUUSD") {
                    std::cout << "[OMEGA-SECURITY] XAUUSD pin: keeping id="
                              << OMEGA_SYMS[i].id << " (XAUUSD spot), blocking id="
                              << id << " (futures)\n";
                    std::cout.flush();
                    continue;
                }
                std::cout << "[OMEGA-SECURITY] primary id update " << name
                          << " " << OMEGA_SYMS[i].id << " -> " << id << "\n";
                OMEGA_SYMS[i].id = id;
            }
        }

        bool matched_ext = false;
        for (size_t i = 0; i < g_ext_syms.size(); ++i) {
            auto& ext = g_ext_syms[i];
            if (name != ext.name) continue;
            matched_ext = true;
            if (ext.id == id) break;

            std::cout << "[OMEGA-SECURITY] learned ext id " << name
                      << " -> " << id << "\n";
            ext.id = id;
            ext_changed = true;
            switch (i) {
                case 0: g_cfg.ext_ger30_id = id; break;
                case 1: g_cfg.ext_uk100_id = id; break;
                case 2: g_cfg.ext_estx50_id = id; break;
                case 3: break;  // index 3 slot reserved; id stored in g_ext_syms[3].id above; no g_cfg mirror
                case 4: g_cfg.ext_eurusd_id = id; break;
                case 5: g_cfg.ext_ukbrent_id = id; break;
                case 6: g_cfg.ext_gbpusd_id = id; break;
                case 7: g_cfg.ext_audusd_id = id; break;
                case 8: g_cfg.ext_nzdusd_id = id; break;
                case 9: g_cfg.ext_usdjpy_id = id; break;
                default: break;
            }
            break;
        }
        // Log unmatched broker symbols that contain keywords we care about --
        // catches broker renames like GER40?GER40, UK100?UK100.F etc.
        if (!matched_ext) {
            bool is_primary = false;
            for (int i = 0; i < OMEGA_NSYMS; ++i)
                if (name == OMEGA_SYMS[i].name) { is_primary = true; break; }
            if (!is_primary) {
                // Broker alias matching for ext symbols -- BlackBull FIX may send
                // alternate names. Maps broker FIX name -> g_ext_syms index.
                struct AliasMap { const char* broker; size_t ext_idx; };
                static const AliasMap aliases[] = {
                    // Brent oil: index 5
                    {"UKBRENT",   5}, {"BRENT.F",   5},
                    // GER40: index 0
                    {"GER30",     0}, {"GER40",     0}, {"DAX",       0}, {"DAX40",    0},
                    // UK100: index 1
                    {"UK100",     1}, {"FTSE",      1}, {"FTSE100",   1},
                    // ESTX50: index 2
                    {"ESTX50",    2}, {"STOXX50",   2}, {"SX5E",      2}, {"EUSTX50",  2},
                };
                bool alias_matched = false;
                for (const auto& a : aliases) {
                    if (name == a.broker && a.ext_idx < g_ext_syms.size() && g_ext_syms[a.ext_idx].id == 0) {
                        g_ext_syms[a.ext_idx].id = id;
                        g_id_to_sym[id] = g_ext_syms[a.ext_idx].name;
                        std::cout << "[OMEGA-SECURITY] alias matched '" << name
                                  << "' -> '" << g_ext_syms[a.ext_idx].name
                                  << "' id=" << id << "\n";
                        alias_matched = true;
                        break;
                    }
                }
                // Log anything with a keyword we care about so we can see broker names
                if (!alias_matched) {
                    static const char* hints[] = {"GER","UK1","EST","BRENT","DAX","FTSE","STOXX"};
                    for (const char* h : hints) {
                        if (name.find(h) != std::string::npos) {
                            std::cout << "[OMEGA-SECURITY] UNMATCHED: '" << name << "' id=" << id << "\n";
                            break;
                        }
                    }
                }
            }
        }
    }
    // Log ext symbols that are still unresolved after processing
    for (const auto& ext : g_ext_syms) {
        if (ext.id == 0) {
            std::cout << "[OMEGA-SECURITY] WARNING: ext symbol '" << ext.name
                      << "' still has id=0 -- broker SecurityList did not contain a match\n";
        }
    }
    std::cout.flush();
    return ext_changed;
}

static std::string build_heartbeat(int seq, const char* subID, const char* trid = nullptr) {
    std::ostringstream b;
    b << "35=0\x01" << "49=" << g_cfg.sender << "\x01" << "56=" << g_cfg.target << "\x01"
      << "50=" << subID << "\x01" << "57=" << subID << "\x01"
      << "34=" << seq << "\x01" << "52=" << timestamp() << "\x01";
    if (trid && *trid) b << "112=" << trid << "\x01";
    return wrap_fix(b.str());
}

static std::string build_test_request(int seq, const char* subID, const std::string& trid) {
    std::ostringstream b;
    b << "35=1\x01" << "49=" << g_cfg.sender << "\x01" << "56=" << g_cfg.target << "\x01"
      << "50=" << subID << "\x01" << "57=" << subID << "\x01"
      << "34=" << seq << "\x01" << "52=" << timestamp() << "\x01"
      << "112=" << trid << "\x01";
    return wrap_fix(b.str());
}

// ?????????????????????????????????????????????????????????????????????????????
// SSL connect (identical to ChimeraMetals -- untouched)
// ?????????????????????????????????????????????????????????????????????????????
static SSL* connect_ssl(const std::string& host, int port, int& sock_out) {
    struct addrinfo hints{}, *result = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &result) != 0)
        return nullptr;
    SOCKET sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (sock == INVALID_SOCKET) { freeaddrinfo(result); return nullptr; }

    // Non-blocking connect with 5s timeout -- prevents connect() blocking for
    // the full OS TCP timeout (~20s on Windows) during shutdown or server outage.
    u_long nb = 1; ioctlsocket(sock, FIONBIO, &nb);
    connect(sock, result->ai_addr, static_cast<int>(result->ai_addrlen));
    freeaddrinfo(result);
    fd_set wset; FD_ZERO(&wset); FD_SET(sock, &wset);
    struct timeval tv{5, 0};  // 5 second connect timeout
    int sel = select(0, nullptr, &wset, nullptr, &tv);
    if (sel <= 0) { closesocket(sock); return nullptr; }
    // Check for connect error (select returns writable even on error)
    int err = 0; int elen = sizeof(err);
    getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &elen);
    if (err != 0) { closesocket(sock); return nullptr; }
    // Restore blocking mode for normal operation
    nb = 0; ioctlsocket(sock, FIONBIO, &nb);

    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,  reinterpret_cast<const char*>(&flag), sizeof(flag));
    setsockopt(sock, SOL_SOCKET,  SO_KEEPALIVE, reinterpret_cast<const char*>(&flag), sizeof(flag));
    // 200ms receive timeout -- SSL_read unblocks within 200ms so g_running=false exits promptly
    DWORD recv_timeout_ms = 200;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&recv_timeout_ms), sizeof(recv_timeout_ms));
    // 500ms send timeout -- caps SSL_write on logout/heartbeat during shutdown
    DWORD send_timeout_ms = 500;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&send_timeout_ms), sizeof(send_timeout_ms));
    // Reuse a single SSL_CTX across all connections -- avoids creating/destroying
    // a fresh context on every reconnect (TLS handshake params don't change).
    static SSL_CTX* s_ctx = nullptr;
    if (!s_ctx) {
        s_ctx = SSL_CTX_new(TLS_client_method());
        if (!s_ctx) { closesocket(sock); return nullptr; }
        SSL_CTX_set_min_proto_version(s_ctx, TLS1_2_VERSION);
        // Quiet shutdown: SSL_free() will NOT send/wait for TLS close-notify.
        // Without this, SSL_free() performs a bidirectional TLS handshake that can
        // block for several seconds waiting for the server's close-notify response,
        // even with SO_SNDTIMEO set. This was the remaining shutdown hang.
        SSL_CTX_set_quiet_shutdown(s_ctx, 1);
    }
    SSL* ssl = SSL_new(s_ctx);
    if (!ssl) { closesocket(sock); return nullptr; }
    SSL_set_fd(ssl, static_cast<int>(sock));
    if (SSL_connect(ssl) <= 0) {
        ERR_print_errors_fp(stderr);
        closesocket(sock); SSL_free(ssl); return nullptr;
    }
    sock_out = static_cast<int>(sock);
    return ssl;
}

// ?????????????????????????????????????????????????????????????????????????????
// RTT
