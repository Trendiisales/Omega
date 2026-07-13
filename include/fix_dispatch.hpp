#pragma once
// fix_dispatch.hpp -- extracted from main.cpp
// Section: fix_dispatch (original lines 7285-7565)
// SINGLE-TRANSLATION-UNIT include -- only include from main.cpp

// 2026-05-01 race fix: forward-declare the engine dispatch post function so
// fix_dispatch.hpp can compile even though it is included (via on_tick.hpp)
// before engine_dispatch.hpp. The actual definition is in engine_dispatch.hpp,
// also single-TU, so the linker has no work to do across translation units.
inline void engine_dispatch_post_tick(const std::string& sym, double bid, double ask);

static void dispatch_fix(const std::string& msg, SSL* ssl) {
    const std::string type = extract_tag(msg, "35");

    if (type == "A") {
        std::cout << "[OMEGA] LOGON ACCEPTED\n";
        g_quote_ready.store(true);
        g_connected_since.store(nowSec());
        // Reset spread gate + CVD on reconnect.
        // spread_gate: stale Asia medians would block valid London entries.
        // cvd: stale prev_bid/prev_ask causes phantom delta on price-gapped reconnect.
        g_edges.spread_gate.reset_all();
        g_edges.cvd.reset_all();  // clears prev_bid/prev_ask to prevent gap-spike CVD
        // Reset ORB range state -- partial ranges built before disconnect
        // would fire immediately on the first qualifying tick post-reconnect.
        g_md_subscribed.store(false);  // clear -- fresh session, not yet subscribed
        g_telemetry.UpdateFixStatus("CONNECTED", "CONNECTED", 0, 0);
        const std::string md = fix_build_md_subscribe_all(g_quote_seq++);
        SSL_write(ssl, md.c_str(), static_cast<int>(md.size()));
        g_md_subscribed.store(true);
        std::cout << "[OMEGA] Subscribed ALL symbols (OMEGA-MD-ALL)\n";
        return;
    }

    if (type == "0") {
        const std::string trid = extract_tag(msg, "112");
        if (!trid.empty() && trid == g_rtt_pending_id && g_rtt_pending_ts > 0) {
            const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            rtt_record(static_cast<double>(now_us - g_rtt_pending_ts) / 1000.0);
            g_rtt_pending_ts = 0;
            g_telemetry.UpdateLatency(g_rtt_last, g_rtt_p50, g_rtt_p95);
        }
        return;
    }

    if (type == "1") {
        const std::string trid = extract_tag(msg, "112");
        const std::string hb   = build_heartbeat(g_quote_seq++, "QUOTE", trid.c_str());
        SSL_write(ssl, hb.c_str(), static_cast<int>(hb.size()));
        return;
    }

    if (type == "5") {
        // Server sent Logout -- ghost session or forced disconnect.
        // Set flag so quote_loop breaks out and reconnects with a delay.
        std::cout << "[OMEGA] Logout received from server (ghost session or forced)\n";
        g_quote_ready.store(false);
        g_quote_logout_received.store(true);
        return;
    }

    // ?? Unknown / unexpected message types -- log everything for diagnostics ??
    if (type != "W" && type != "X" && type != "A" && type != "0" && type != "1" && type != "3" && type != "j") {
        std::string readable = msg.substr(0, std::min(msg.size(), size_t(300)));
        for (char& c : readable) if (c == '\x01') c = '|';
        std::cerr << "[OMEGA-RAW] type=" << type << " msg=" << readable << "\n";
        std::cerr.flush();
    }

    // ?? Market data ???????????????????????????????????????????????????????????
    if (type == "W" || type == "X") {
        const std::string sym_raw = extract_tag(msg, "55");
        if (sym_raw.empty()) {
            std::cerr << "[OMEGA-MD] W/X msg missing tag 55 -- raw: ";
            std::string r = msg.substr(0, 200); for (char& c : r) if (c=='\x01') c='|';
            std::cerr << r << "\n"; std::cerr.flush();
            return;
        }
        std::string sym;
        // Try numeric ID first (normal case), then string name fallback
        try {
            const int id = std::stoi(sym_raw);
            std::lock_guard<std::mutex> lk(g_symbol_map_mtx);
            const auto it = g_id_to_sym.find(id);
            if (it == g_id_to_sym.end()) {
                std::cerr << "[OMEGA-MD] Unknown numeric ID " << id << " in tag55\n";
                std::cerr.flush();
                return;
            }
            sym = it->second;
        } catch (...) {
            // Broker sent string name in 55= (e.g. "XAUUSD") -- look up directly
            for (int i = 0; i < OMEGA_NSYMS; ++i) {
                if (sym_raw == OMEGA_SYMS[i].name) { sym = OMEGA_SYMS[i].name; break; }
            }
            if (sym.empty() && g_cfg.enable_extended_symbols) {
                std::lock_guard<std::mutex> lk(g_symbol_map_mtx);
                for (const auto& e : g_ext_syms) {
                    if (sym_raw == e.name) { sym = e.name; break; }
                }
            }
            if (sym.empty()) {
                std::cerr << "[OMEGA-MD] Unknown string symbol '" << sym_raw << "' in tag55\n";
                std::cerr.flush();
                return;
            }
        }
        double bid = 0.0, ask = 0.0;
        // ?? L2 depth parsing -- tag 269=side, 270=price, 271=size ?????????????
        // Parse all depth levels into g_l2_books, extract best bid/ask for on_tick.
        {
            L2Book book;
            size_t pos = 0u;
            while ((pos = msg.find("269=", pos)) != std::string::npos) {
                const char et = msg[pos + 4u];
                const size_t soh = msg.find('\x01', pos);
                if (soh == std::string::npos) break;
                const size_t px_pos = msg.find("270=", pos);
                if (px_pos == std::string::npos || px_pos > soh + 200) { pos = soh; continue; }
                const size_t px_end = msg.find('\x01', px_pos + 4u);
                if (px_end == std::string::npos) break;
                const double price = std::stod(msg.substr(px_pos + 4u, px_end - (px_pos + 4u)));
                // Tag 271 (MDEntrySize) -- may or may not be present
                double size = 0.0;
                const size_t sz_pos = msg.find("271=", px_pos);
                if (sz_pos != std::string::npos && sz_pos < px_end + 30) {
                    const size_t sz_end = msg.find('\x01', sz_pos + 4u);
                    if (sz_end != std::string::npos)
                        size = std::stod(msg.substr(sz_pos + 4u, sz_end - (sz_pos + 4u)));
                }
                if (et == '0') { // bid
                    if (price > bid) bid = price;
                    if (book.bid_count < 5) {
                        book.bids[book.bid_count++] = {price, size};
                    }
                } else if (et == '1') { // ask
                    if (ask <= 0.0 || price < ask) ask = price;
                    if (book.ask_count < 5) {
                        book.asks[book.ask_count++] = {price, size};
                    }
                }
                pos = soh;
            }
            // Store L2 book + write atomics. FIX is the sole L2 source as of
            // 2026-05-06 (cTrader Open API and OmegaDomStreamer cBot culled).
            // Unconditional write per W/X message -- no staleness gate.
            if (book.bid_count > 0 || book.ask_count > 0) {
                double imb = 0.5;
                double mp_bias = 0.0;
                bool   hd  = false;
                {
                    std::lock_guard<std::mutex> lk(g_l2_mtx);
                    auto& stored = g_l2_books[sym];
                    if (book.bid_count > 0) {
                        stored.bid_count = book.bid_count;
                        for (int i = 0; i < book.bid_count; ++i) stored.bids[i] = book.bids[i];
                    }
                    if (book.ask_count > 0) {
                        stored.ask_count = book.ask_count;
                        for (int i = 0; i < book.ask_count; ++i) stored.asks[i] = book.asks[i];
                    }
                    imb = stored.imbalance();
                    hd  = stored.has_data();
                    // ── Microprice bias (size-weighted mid - mid) ──────────────
                    // microprice = (bid_size*ask_price + ask_size*bid_price)
                    //              / (bid_size + ask_size)
                    // Used by entry_score_l2() as a direction tiebreaker (+/-1).
                    // Falls to 0 if broker omits tag 271 (sizes=0).
                    if (stored.bid_count > 0 && stored.ask_count > 0) {
                        const double bp = stored.bids[0].price;
                        const double ap = stored.asks[0].price;
                        const double bs = stored.bids[0].size;
                        const double as_ = stored.asks[0].size;
                        const double total = bs + as_;
                        if (total > 0.0 && bp > 0.0 && ap > 0.0) {
                            const double micro = (bs * ap + as_ * bp) / total;
                            const double mid   = (bp + ap) * 0.5;
                            mp_bias = micro - mid;
                        }
                    }
                }
                AtomicL2* al = get_atomic_l2(sym);
                if (al) {
                    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    al->imbalance.store(imb, std::memory_order_relaxed);
                    al->microprice_bias.store(mp_bias, std::memory_order_relaxed);
                    al->has_data.store(hd,  std::memory_order_relaxed);
                    al->last_update_ms.store(now_ms, std::memory_order_release);
                }
            }
        }
        // Measure latency from broker tag 52 (SendingTime) on every quote
        // Provides sub-second RTT samples vs 5s heartbeat ping
        const std::string send_ts = extract_tag(msg, "52");
        if (!send_ts.empty()) {
            const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            const int64_t sent_us = parse_fix_time_us(send_ts);
            if (sent_us > 0 && now_us > sent_us) {
                const double tick_lat_ms = static_cast<double>(now_us - sent_us) / 1000.0;
                if (tick_lat_ms > 0.0 && tick_lat_ms < 5000.0) {
                    // Do NOT feed tag52 delta into rtt_record -- broker clock vs our clock
                    // may differ by 10-20ms even on co-located hardware (NTP drift).
                    // rtt_record() feeds the lat_ok gate -- only use true TestRequest RTT.
                    // tag52 delta is displayed separately as feed latency indicator only.
                    static int64_t s_last_lat_push_us = 0;
                    if (now_us - s_last_lat_push_us >= 200000LL) {
                        s_last_lat_push_us = now_us;
                        g_telemetry.UpdateLatency(tick_lat_ms, g_rtt_p50, g_rtt_p95);
                    }
                }
            }
        }
        // Passive L2 routing disabled -- all symbols go through on_tick.

        // ── IBKR venue routing (S-2026-07-06 FX; S-2026-07-09 XAUUSD/NAS100) ──
        // BlackBull FIX delivers FX majors as ~30s-frozen snapshots and, per the
        // 2026-07-09 feed-migration directive, the whole book is moving off
        // BlackBull onto IBKR wherever an IBKR contract exists on the bridge. When
        // the matching IBKR slot is FRESH, IBKR is the authoritative source -- drop
        // the BlackBull quote here so a stale/CFD-scale price never overwrites the
        // live IBKR price. Bridge down -> slot goes stale within 5s -> BlackBull
        // resumes as fallback (no blackout for any gated symbol).
        //   FX majors (is_fx_major)        -> IBKR IDEALPRO L1.
        //   is_ibkr_primary_index -> the COMPLETE-migration set (S-2026-07-09):
        //     XAUUSD/NAS100 (depth) + US500.F/USTEC.F/GER40/UK100/ESTX50/USOIL.F/
        //     XAGUSD/VIX.F/DX.F/NGAS.F/BRENT (L1). Each is posted to on_tick from the
        //     bridge on_book callback in omega_main. This gate sits AFTER the L2-book
        //     parse/store above, so the BlackBull AtomicL2 depth is still cached
        //     (imbalance / microprice stay populated); only the price TICK source is
        //     switched. Bridge down or missing IBKR entitlement (Error 354) -> slot
        //     stale >5s -> BlackBull automatically resumes as fallback (no blackout).
        // After this migration NO symbol is BlackBull-primary while its IBKR slot is
        // fresh -- the zero-BlackBull goal. See outputs/FEED_AUDIT_2026-07-09.md.
        // S-2026-07-09b HARD-DROP set (operator: "STOP BLACKBULL NOW"): symbols where
        // IBKR is entitled + actively flowing NEVER take a BlackBull tick. Root cause of
        // the NAS100 spike-war: the depth-slot freshness check was unreliable, so BlackBull
        // (CFD, ~200pt off the IBKR future) leaked in alongside the IBKR NQ feed -> every
        // other tick looked like a 200pt spike -> spike filter rejected them -> the mark
        // froze/flickered between two scales. Unconditional drop kills the dual feed. These
        // symbols go QUIET (not BlackBull) if IBKR ever dies -- accepted per the zero-BlackBull
        // directive; a dark tile is loud/honest vs a silently wrong CFD price.
        auto is_ibkr_hard = [](const char* s) noexcept {
            return std::strcmp(s,"XAUUSD")==0 || std::strcmp(s,"NAS100")==0
                || std::strcmp(s,"USTEC.F")==0 || std::strcmp(s,"US500.F")==0
                || std::strcmp(s,"XAGUSD")==0 || std::strcmp(s,"GER40")==0
                || std::strcmp(s,"ESTX50")==0;
        };
        if (is_ibkr_hard(sym.c_str())) return;   // never BlackBull, no freshness dependency
        // S-2026-07-13 (operator: "no blackbull fallback we are not using it for now"):
        // FX majors JOIN the hard-IBKR group -- NEVER take a BlackBull tick, no freshness
        // fallback. If the IBKR IDEALPRO slot dies, FX goes QUIET (dark tile) rather than
        // riding a ~30s-frozen BlackBull CFD snapshot. A dark FX tile is honest; a
        // wrong-venue/stale price silently feeding the FX books is not.
        if (omega::ibkr::is_fx_major(sym.c_str())) return;
        // The remaining UNENTITLED index/energy set (UK100/USOIL/VIX/DX/NGAS/BRENT) keep
        // the freshness fallback so they aren't dark when IBKR has no data for them at all.
        if (omega::ibkr::is_ibkr_primary_index(sym.c_str())) {
            const int64_t ib_now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            const omega::ibkr::L2Slot* ibslot = g_ibkr_l2.lookup(sym.c_str());
            if (ibslot && ibslot->fresh(ib_now_ms, 5000)) return;
        }

        // Seed cache with whatever side(s) we just parsed -- must happen BEFORE
        // the fallback read below, otherwise first-ever X (single-sided) drops silently.
        {
            std::lock_guard<std::mutex> lk(g_book_mtx);
            if (bid > 0.0) g_bids[sym] = bid;
            if (ask > 0.0) g_asks[sym] = ask;
        }

        // Merge incremental update with cached book.
        // BlackBull type=X sends only ONE side (bid OR ask).
        // Fill missing side from last known book so on_tick always gets valid bid+ask.
        if (bid <= 0.0 || ask <= 0.0) {
            std::lock_guard<std::mutex> lk(g_book_mtx);
            if (bid <= 0.0) { const auto it = g_bids.find(sym); if (it != g_bids.end()) bid = it->second; }
            if (ask <= 0.0) { const auto it = g_asks.find(sym); if (it != g_asks.end()) ask = it->second; }
        }
        if (bid > 0.0 && ask > 0.0) {
            // Post to the engine dispatch queue (single-writer pattern: only the
            // dispatch worker ever enters on_tick()). See engine_dispatch.hpp.
            // NOTE (S-2026-07-09 COMPLETE migration): BlackBull FIX is no longer the
            // canonical price source for ANY migrated symbol. For every symbol in
            // is_fx_major + is_ibkr_primary_index (now the whole traded book) this
            // line is reached ONLY when the IBKR slot is stale (the fresh-slot gate
            // above returned early) -- i.e. BlackBull is purely the FALLBACK, and
            // IBKR (posted via the bridge on_book callback) is canonical. A symbol
            // falls back to FIX here only when its IBKR slot is stale (bridge down or
            // no market-data entitlement). See outputs/FEED_AUDIT_2026-07-09.md.
            engine_dispatch_post_tick(sym, bid, ask);
        }
        return;
    }

    if (type == "3" || type == "j") {
        std::string r = msg.substr(0, 400); for (char& c : r) if (c=='\x01') c='|';
        std::cerr << "[OMEGA] FIX REJECT type=" << type
                  << " text=" << extract_tag(msg, "58")
                  << " refMsgType=" << extract_tag(msg, "372")
                  << " full=" << r << "\n";
        std::cerr.flush();
    }

    // ?? MarketDataRequestReject (35=Y) -- depth fallback ??????????????????????
    // If broker rejects our 264=5 subscription, fall back to 264=1 (top-of-book).
    // This fires at most once per session. g_md_depth_fallback prevents re-loop.
    if (type == "Y") {
        const std::string rej_reason = extract_tag(msg, "281"); // MDReqRejReason
        const std::string req_id     = extract_tag(msg, "262"); // MDReqID
        std::cerr << "[OMEGA-DEPTH] MarketDataRequestReject (35=Y)"
                  << " req=" << req_id
                  << " reason=" << rej_reason
                  << " -- broker rejected 264=5, falling back to 264=1 (top-of-book)\n";
        std::cerr.flush();

        if (!g_md_depth_fallback.load()) {
            g_md_depth_fallback.store(true);
            g_md_depth_ok.store(false);
            std::cout << "[OMEGA-DEPTH] Depth fallback active -- re-subscribing at 264=1 (top-of-book).\n";
            std::cout.flush();
            // Immediately unsub the rejected 264=5 request and re-sub at 264=1.
            // ssl is in scope here (dispatch_fix parameter) -- no need to signal the loop.
            // g_md_depth_fallback=true means fix_build_md_subscribe_all() now emits 264=1.
            const std::string unsub = fix_build_md_unsub_all(g_quote_seq++);
            if (!unsub.empty())
                SSL_write(ssl, unsub.c_str(), static_cast<int>(unsub.size()));
            Sleep(80);  // brief gap -- let broker process unsub before resub
            const std::string resub = fix_build_md_subscribe_all(g_quote_seq++);
            if (!resub.empty()) {
                SSL_write(ssl, resub.c_str(), static_cast<int>(resub.size()));
                g_md_subscribed.store(true);
                std::cout << "[OMEGA-DEPTH] Re-subscribed at 264=1 -- data should resume.\n";
                std::cout.flush();
            }
        }
    }

} // end dispatch_fix

// ?????????????????????????????????????????????????????????????????????????????
// Quote loop
// ?????????????????????????????????????????????????????????????????????????????
// ?????????????????????????????????????????????????????????????????????????????
// trade_loop  -- FIX session on port 5212 (order management)
// Runs in its own thread. In SHADOW mode: connects, logs on, keeps alive.
// In LIVE mode: NewOrderSingle messages are sent via g_trade_ssl.
// ?????????????????????????????????????????????????????????????????????????????
