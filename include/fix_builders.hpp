#pragma once
// fix_builders.hpp -- extracted from main.cpp
// Section: fix_builders (original lines 1872-2132)
// SINGLE-TRANSLATION-UNIT include -- only include from main.cpp

static std::string extract_tag(const std::string& msg, const char* tag) {
    const std::string pat = std::string(tag) + '=';
    size_t pos = 0;
    while (true) {
        pos = msg.find(pat, pos);
        if (pos == std::string::npos) return {};
        // Tag must be at start of message or preceded by SOH delimiter
        if (pos == 0 || msg[pos - 1] == '\x01') break;
        pos += pat.size(); // false match inside a value -- skip
    }
    const size_t s = pos + pat.size();
    const size_t e = msg.find('\x01', s);
    if (e == std::string::npos) return {};
    return msg.substr(s, e - s);
}

static std::string compute_checksum(const std::string& body) {
    unsigned int sum = 0;
    for (unsigned char c : body) sum += c;
    sum %= 256u;
    char buf[4]; snprintf(buf, sizeof(buf), "%03u", sum);
    return buf;
}

static std::string wrap_fix(const std::string& body) {
    const std::string with_l = "8=FIX.4.4\x01" "9=" + std::to_string(body.size()) + "\x01" + body;
    return with_l + "10=" + compute_checksum(with_l) + "\x01";
}

static int g_quote_seq = 1;

// ??????????????????????????????????????????????????????????????????????????????
// IMMUTABLE FIX SECTION -- DO NOT MODIFY
// BlackBull cTrader FIX 4.4 message builders.
// These functions encode the ONLY valid parameter combinations for this broker.
// Changing any tag value here will break the quote session.
// See OmegaFIX.hpp header and FIX_BLACKBULL_CONSTRAINTS.md for constraints.
// ??????????????????????????????????????????????????????????????????????????????

// 35=A Logon
static std::string fix_build_logon(int seq, const char* subID) {
    std::ostringstream b;
    b << "35=A\x01" << "49=" << g_cfg.sender << "\x01" << "56=" << g_cfg.target << "\x01"
      << "50=" << subID << "\x01" << "57=" << subID << "\x01" << "34=" << seq << "\x01"
      << "52=" << timestamp() << "\x01" << "98=0\x01" << "108=" << g_cfg.heartbeat << "\x01"
      << "141=Y\x01" << "553=" << g_cfg.username << "\x01" << "554=" << g_cfg.password << "\x01";
    return wrap_fix(b.str());
}
// 35=5 Logout
static std::string fix_build_logout(int seq, const char* subID) {
    std::ostringstream b;
    b << "35=5\x01" << "49=" << g_cfg.sender << "\x01" << "56=" << g_cfg.target << "\x01"
      << "50=" << subID << "\x01" << "57=" << subID << "\x01"
      << "34=" << seq << "\x01" << "52=" << timestamp() << "\x01";
    return wrap_fix(b.str());
}
// ?? SINGLE subscription covering ALL symbols (primary + extended) ??????????
// ONE fixed req ID = OMEGA-MD-ALL. Unsub always matches. No ghost possible.
//
// ── DEPTH SELECTION (2026-04-20 upgrade) ────────────────────────────────────
// Primary path:  264=0 (FULL BOOK) -- delivers REAL asymmetric L2 sizes.
// Fallback path: 264=1 (top-of-book) -- used only if broker rejects 264=0.
//
// BlackBull cTrader ONLY accepts MarketDepth values of 0 or 1 (confirmed by
// "INVALID_REQUEST: MarketDepth should be either 0 or 1" 35=Y rejections).
// Historically this code sent 264=1, which returns a single level per side
// with MDEntrySize=0 -- producing the "imbalance always 0.500" problem that
// forced the team to route L2 via the OmegaDomStreamer cBot instead. A FIX
// probe on 2026-04-20 confirmed that 264=0 returns genuine multi-level book
// data with real MDEntrySize values (5 bids / 6 asks / imb5=0.8079 on first
// snapshot). Switching to 264=0 eliminates the need for the cBot path.
//
// The fallback flag g_md_depth_fallback is declared in OmegaFIX.hpp and is
// set to true by the 35=Y handler in fix_dispatch.hpp. When true, this
// builder emits 264=1 instead of 264=0, which prevents a rejection loop.
// The flag starts false, so the first subscribe always tries 264=0.
// ????????????????????????????????????????????????????????????????????????????
static std::string fix_build_md_subscribe_all(int seq) {
    // Collect all symbol IDs: primary + extended + passive L2 observers
    std::vector<int> ids;
    for (int i = 0; i < OMEGA_NSYMS; ++i) ids.push_back(OMEGA_SYMS[i].id);
    {
        std::lock_guard<std::mutex> lk(g_symbol_map_mtx);
        for (const auto& e : g_ext_syms) if (e.id > 0) ids.push_back(e.id);
    }
    // Passive L2 cross-pairs disabled -- not subscribing, not needed for price feed.
    // Depth: 0 = full book (primary); 1 = top-of-book (fallback after 35=Y reject).
    const int depth_val = g_md_depth_fallback.load(std::memory_order_acquire) ? 1 : 0;
    std::ostringstream b;
    b << "35=V\x01" << "49=" << g_cfg.sender << "\x01" << "56=" << g_cfg.target << "\x01"
      << "50=QUOTE\x01" << "57=QUOTE\x01" << "34=" << seq << "\x01" << "52=" << timestamp() << "\x01"
      << "262=OMEGA-MD-ALL\x01" << "263=1\x01" << "264=" << depth_val << "\x01" << "265=0\x01"
      << "146=" << ids.size() << "\x01";
    for (int id : ids) b << "55=" << id << "\x01";
    b << "267=2\x01" << "269=0\x01" << "269=1\x01";
    return wrap_fix(b.str());
}
static std::string fix_build_md_unsub_all(int seq) {
    std::vector<int> ids;
    for (int i = 0; i < OMEGA_NSYMS; ++i) ids.push_back(OMEGA_SYMS[i].id);
    {
        std::lock_guard<std::mutex> lk(g_symbol_map_mtx);
        for (const auto& e : g_ext_syms) if (e.id > 0) ids.push_back(e.id);
    }
    // Unsub depth must match the sub depth for cTrader to match the request.
    const int depth_val = g_md_depth_fallback.load(std::memory_order_acquire) ? 1 : 0;
    std::ostringstream b;
    b << "35=V\x01" << "49=" << g_cfg.sender << "\x01" << "56=" << g_cfg.target << "\x01"
      << "50=QUOTE\x01" << "57=QUOTE\x01" << "34=" << seq << "\x01" << "52=" << timestamp() << "\x01"
      << "262=OMEGA-MD-ALL\x01" << "263=2\x01" << "264=" << depth_val << "\x01" << "265=0\x01"
      << "146=" << ids.size() << "\x01";
    for (int id : ids) b << "55=" << id << "\x01";
    b << "267=2\x01" << "269=0\x01" << "269=1\x01";
    return wrap_fix(b.str());
}
// Legacy individual builders kept for ghost cleanup of old session IDs.
// These are NOT used by the primary subscribe path (fix_build_md_subscribe_all
// is the live path). They're retained to cancel any orphan session subscriptions
// that may linger at broker side from older builds. Left at 264=1 intentionally
// -- these paths don't need the upgraded depth signal.
static std::string fix_build_md_subscribe(int seq) {
    std::ostringstream b;
    b << "35=V\x01" << "49=" << g_cfg.sender << "\x01" << "56=" << g_cfg.target << "\x01"
      << "50=QUOTE\x01" << "57=QUOTE\x01" << "34=" << seq << "\x01" << "52=" << timestamp() << "\x01"
      << "262=OMEGA-MD-001\x01" << "263=1\x01" << "264=1\x01" << "265=0\x01"
      << "146=" << OMEGA_NSYMS << "\x01";
    for (int i = 0; i < OMEGA_NSYMS; ++i) b << "55=" << OMEGA_SYMS[i].id << "\x01";
    b << "267=2\x01" << "269=0\x01" << "269=1\x01";
    return wrap_fix(b.str());
}
static std::string fix_build_md_subscribe_ext(int seq) {
    std::vector<int> ids;
    { std::lock_guard<std::mutex> lk(g_symbol_map_mtx);
      for (const auto& e : g_ext_syms) if (e.id > 0) ids.push_back(e.id); }
    if (ids.empty()) return {};
    std::ostringstream b;
    b << "35=V\x01" << "49=" << g_cfg.sender << "\x01" << "56=" << g_cfg.target << "\x01"
      << "50=QUOTE\x01" << "57=QUOTE\x01" << "34=" << seq << "\x01" << "52=" << timestamp() << "\x01"
      << "262=OMEGA-MD-EXT\x01" << "263=1\x01" << "264=1\x01" << "265=0\x01"
      << "146=" << ids.size() << "\x01";
    for (int id : ids) b << "55=" << id << "\x01";
    b << "267=2\x01" << "269=0\x01" << "269=1\x01";
    return wrap_fix(b.str());
}
static std::string fix_build_md_unsub(int seq) {
    std::ostringstream b;
    b << "35=V\x01" << "49=" << g_cfg.sender << "\x01" << "56=" << g_cfg.target << "\x01"
      << "50=QUOTE\x01" << "57=QUOTE\x01" << "34=" << seq << "\x01" << "52=" << timestamp() << "\x01"
      << "262=OMEGA-MD-001\x01" << "263=2\x01" << "264=1\x01" << "265=0\x01"
      << "146=" << OMEGA_NSYMS << "\x01";
    for (int i = 0; i < OMEGA_NSYMS; ++i) b << "55=" << OMEGA_SYMS[i].id << "\x01";
    b << "267=2\x01" << "269=0\x01" << "269=1\x01";
    return wrap_fix(b.str());
}
static std::string fix_build_md_unsub_ext(int seq) {
    std::vector<int> ids;
    { std::lock_guard<std::mutex> lk(g_symbol_map_mtx);
      for (const auto& e : g_ext_syms) if (e.id > 0) ids.push_back(e.id); }
    if (ids.empty()) return {};
    std::ostringstream b;
    b << "35=V\x01" << "49=" << g_cfg.sender << "\x01" << "56=" << g_cfg.target << "\x01"
      << "50=QUOTE\x01" << "57=QUOTE\x01" << "34=" << seq << "\x01" << "52=" << timestamp() << "\x01"
      << "262=OMEGA-MD-EXT\x01" << "263=2\x01" << "264=1\x01" << "265=0\x01"
      << "146=" << ids.size() << "\x01";
    for (int id : ids) b << "55=" << id << "\x01";
    b << "267=2\x01" << "269=0\x01" << "269=1\x01";
    return wrap_fix(b.str());
}
// 35=x SecurityListRequest
static std::string fix_build_security_list_request(int seq, const std::string& req_id) {
    std::ostringstream b;
    b << "35=x\x01" << "49=" << g_cfg.sender << "\x01" << "56=" << g_cfg.target << "\x01"
      << "50=TRADE\x01" << "57=TRADE\x01" << "34=" << seq << "\x01" << "52=" << timestamp() << "\x01"
      << "320=" << req_id << "\x01" << "559=0\x01";
    return wrap_fix(b.str());
}
// ??????????????????????????????????????????????????????????????????????????????
// END IMMUTABLE FIX SECTION
// ??????????????????????????????????????????????????????????????????????????????

// ?????????????????????????????????????????????????????????????????????????????
// Live Order Dispatch -- 35=D NewOrderSingle
// ?????????????????????????????????????????????????????????????????????????????
// Design: SHADOW mode = zero orders sent, full paper audit trail.
//         LIVE mode   = real 35=D sent on trade session (port 5212).
//
// Safety architecture:
//   1. send_live_order() checks mode == "LIVE" before sending -- impossible to
//      accidentally fire in SHADOW mode regardless of any other code path.
//   2. g_trade_ready atomic must be true (trade session logged in).
//   3. g_trade_ssl must be non-null and write must succeed.
//   4. Every order is logged to console AND the order log before sending.
//   5. Order tracking: g_live_orders maps clOrdId -> symbol+side for ACK matching.
//   6. FIX rejects (35=3, 35=j) on trade session are already logged in trade_loop.
//
// BlackBull/cTrader FIX 4.4 NewOrderSingle fields:
//   35=D, 11=clOrdId, 55=symbolId, 54=side(1=Buy/2=Sell),
//   38=qty, 40=ordType(1=Market), 59=timeInForce(3=IOC),
//   60=transactTime
//
// Position management (TP/SL/TIMEOUT) is handled entirely by the engines in
// software -- we do NOT send bracket orders. This is correct for CFD/futures
// market makers like BlackBull where bracket orders are unreliable.
// The engine closes via another Market order when TP/SL triggers.
// ?????????????????????????????????????????????????????????????????????????????

struct LiveOrderRecord {
    std::string clOrdId;
    std::string symbol;
    std::string side;     // "LONG" / "SHORT"
    double      qty   = 0;
    double      price = 0;  // mid at time of order
    int64_t     ts    = 0;
    bool        acked = false;
    bool        rejected = false;
};

static std::mutex g_live_orders_mtx;
static std::unordered_map<std::string, LiveOrderRecord> g_live_orders;
static std::atomic<int> g_order_id_counter{1};

static std::string build_new_order_single(int seq, const std::string& clOrdId,
                                          int sym_id, bool is_long,
                                          double qty) {
    std::ostringstream b;
    b << "35=D\x01"
      << "49=" << g_cfg.sender << "\x01"
      << "56=" << g_cfg.target << "\x01"
      << "50=TRADE\x01" << "57=TRADE\x01"
      << "34=" << seq << "\x01"
      << "52=" << timestamp() << "\x01"
      << "11=" << clOrdId << "\x01"           // ClOrdID
      << "55=" << sym_id  << "\x01"           // Symbol (numeric ID)
      << "54=" << (is_long ? "1" : "2") << "\x01"  // Side: 1=Buy 2=Sell
      << "38=" << std::fixed << std::setprecision(2) << qty << "\x01"  // OrderQty
      << "40=1\x01"                           // OrdType=Market
      << "59=3\x01"                           // TimeInForce=IOC
      << "60=" << timestamp() << "\x01";      // TransactTime
    return wrap_fix(b.str());
}

// Build a FIX NewOrderSingle as a LIMIT order (OrdType=2, TimeInForce=1=GTC).
// limit_px = mid price at signal time. Passive fill saves ~$0.30/trade vs market.
// FIX additions vs market order: 40=2 (Limit), 44=price, 59=1 (GTC).
static std::string build_limit_order_single(int seq, const std::string& clOrdId,
                                            int sym_id, bool is_long,
                                            double qty, double limit_px) {
    std::ostringstream b;
    b << "35=D\x01"
      << "49=" << g_cfg.sender << "\x01"
      << "56=" << g_cfg.target << "\x01"
      << "50=TRADE\x01" << "57=TRADE\x01"
      << "34=" << seq << "\x01"
      << "52=" << timestamp() << "\x01"
      << "11=" << clOrdId << "\x01"           // ClOrdID
      << "55=" << sym_id  << "\x01"           // Symbol (numeric ID)
      << "54=" << (is_long ? "1" : "2") << "\x01"  // Side: 1=Buy 2=Sell
      << "38=" << std::fixed << std::setprecision(2) << qty << "\x01"  // OrderQty
      << "40=2\x01"                           // OrdType=Limit
      << "44=" << std::fixed << std::setprecision(5) << limit_px << "\x01"  // Price
      << "59=1\x01"                           // TimeInForce=GTC
      << "60=" << timestamp() << "\x01";      // TransactTime
    return wrap_fix(b.str());
}

// ---------------------------------------------------------------------------
//  Pending limit order tracker.
//  send_limit_order() inserts here; check_pending_limits() cancels expired ones.
// ---------------------------------------------------------------------------
struct PendingLimitOrder {
    std::string symbol;
    bool        is_long   = false;
    double      qty       = 0.0;
    double      limit_px  = 0.0;
    int64_t     sent_ms   = 0;    // wall-clock ms at send time
    int64_t     expire_ms = 0;    // cancel fallback deadline (sent_ms + 500)
    bool        filled    = false;
    bool        cancelled = false;
};
static std::mutex g_pending_limits_mtx;
static std::unordered_map<std::string, PendingLimitOrder> g_pending_limits;
static constexpr int64_t LIMIT_ORDER_TIMEOUT_MS = 500;  // cancel after 500ms if unfilled

// Send a live LIMIT order at limit_px. Falls back to market if timeout fires.
