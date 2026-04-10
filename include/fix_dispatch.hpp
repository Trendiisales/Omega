#pragma once
// fix_dispatch.hpp -- extracted from main.cpp
// Section: fix_dispatch (original lines 7285-7565)
// SINGLE-TRANSLATION-UNIT include -- only include from main.cpp
//
// FIX CONNECTION ROLE: ORDER EXECUTION ONLY.
// Market data (W/X messages) from BlackBull are IGNORED entirely.
// All price + L2 data comes exclusively from the cTrader Open API depth feed.
// FIX is used only for: session management, order entry/exit, fill confirmation,
// RTT measurement, and heartbeats.

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
        g_orb_us.reset_range();    g_orb_ger30.reset_range();
        g_orb_uk100.reset_range(); g_orb_estx50.reset_range();
        g_orb_silver.reset_range();
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

    // ?? Market data W/X -- LATENCY MEASUREMENT ONLY ??????????????????????????????????
    // ALL price and L2 data comes from cTrader Open API. FIX W/X is ignored for
    // trading decisions. We only extract tag 52 (SendingTime) for RTT diagnostics.
    // No L2 parsing, no g_l2_books writes, no AtomicL2 writes, no on_tick() calls.
    if (type == "W" || type == "X") {
        // Measure latency from broker tag 52 (SendingTime) -- feed lag diagnostic only.
        // NOT fed into rtt_record() -- broker clock vs our clock may differ 10-20ms.
        // Displayed separately as feed latency indicator on GUI.
        const std::string send_ts = extract_tag(msg, "52");
        if (!send_ts.empty()) {
            const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            const int64_t sent_us = parse_fix_time_us(send_ts);
            if (sent_us > 0 && now_us > sent_us) {
                const double tick_lat_ms = static_cast<double>(now_us - sent_us) / 1000.0;
                if (tick_lat_ms > 0.0 && tick_lat_ms < 5000.0) {
                    static int64_t s_last_lat_push_us = 0;
                    if (now_us - s_last_lat_push_us >= 200000LL) {
                        s_last_lat_push_us = now_us;
                        g_telemetry.UpdateLatency(tick_lat_ms, g_rtt_p50, g_rtt_p95);
                    }
                }
            }
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
    // NOTE: even with fallback active, FIX W/X prices are still ignored.
    // The fallback only affects which FIX subscription format we send -- we still
    // rely exclusively on cTrader for all price and L2 data.
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
