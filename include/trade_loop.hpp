#pragma once
// trade_loop.hpp -- extracted from main.cpp
// SINGLE-TRANSLATION-UNIT include -- only include from main.cpp

static void trade_loop() {
    int backoff_ms = 1000;
    const int max_backoff = 30000;

    while (g_running.load()) {
        std::cout << "[OMEGA-TRADE] Connecting " << g_cfg.host << ":" << g_cfg.trade_port << "\n";

        int sock = -1;
        SSL* ssl = connect_ssl(g_cfg.host, g_cfg.trade_port, sock);
        if (!ssl) {
            std::cerr << "[OMEGA-TRADE] Connect failed -- retry " << backoff_ms << "ms\n";
            for (int i = 0; i < backoff_ms / 10 && g_running.load(); ++i) Sleep(10);
            backoff_ms = std::min(backoff_ms * 2, max_backoff);
            continue;
        }

        backoff_ms = 1000;
        g_trade_seq = 1;
        g_trade_ready.store(false);  // clear before logon -- previous session may have left it true

        // Send trade logon
        const std::string logon = fix_build_logon(g_trade_seq++, "TRADE");
        SSL_write(ssl, logon.c_str(), static_cast<int>(logon.size()));
        std::cout << "[OMEGA-TRADE] Logon sent\n";

        // Store globally for order submission
        {
            std::lock_guard<std::mutex> lk(g_trade_mtx);
            g_trade_ssl  = ssl;
            g_trade_sock = sock;
        }

        // Read loop -- heartbeats + logon ACK only on trade session
        std::string trade_recv_buf;
        auto last_ping      = std::chrono::steady_clock::now();
        auto logon_sent_at  = std::chrono::steady_clock::now();


        while (g_running.load()) {
            const auto now = std::chrono::steady_clock::now();

            // Logon timeout: if no LOGON ACCEPTED within 10s, drop and reconnect
            if (!g_trade_ready.load() &&
                std::chrono::duration_cast<std::chrono::seconds>(now - logon_sent_at).count() >= 10) {
                std::cerr << "[OMEGA-TRADE] Logon timeout (10s) -- reconnecting\n";
                break;
            }

            // Proactive 13min reconnect REMOVED.
            // Was added to handle "32 drops at 15min intervals" -- those drops
            // were caused by d7a0a16's L2 auto-restart, not a broker timeout.
            // With L2 restart removed, FIX session stays up indefinitely.

            // Heartbeat every 30s
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_ping).count() >= g_cfg.heartbeat) {
                last_ping = now;
                const std::string hb = build_heartbeat(g_trade_seq++, "TRADE");
                std::lock_guard<std::mutex> lk(g_trade_mtx);
                if (SSL_write(ssl, hb.c_str(), static_cast<int>(hb.size())) <= 0) {
                    // DIAGNOSTIC (Session 14): previously a silent break -- root cause of
                    // apparent "phantom" disconnects where neither SSL_read error nor
                    // Logout received preceded the reconnect. SSL_write failure here means
                    // broker has silently closed/half-closed the TCP connection; our write
                    // cannot complete. Log before break so post-mortem can distinguish
                    // this path from the SSL_read error path at L88 and Logout at L142.
                    const int wsa = WSAGetLastError();
                    std::cerr << "[OMEGA-TRADE] Heartbeat write failed (WSA=" << wsa
                              << ") -- reconnecting\n";
                    std::cerr.flush();
                    break;
                }
            }

            char buf[4096];
            const int n = SSL_read(ssl, buf, static_cast<int>(sizeof(buf)) - 1);
            if (n <= 0) {
                const int err = SSL_get_error(ssl, n);
                if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                    Sleep(1); continue;
                }
                // SO_RCVTIMEO timeout fires as SSL_ERROR_SYSCALL + WSAETIMEDOUT --
                // not a real disconnect, just no data in 200ms window
                if (err == SSL_ERROR_SYSCALL && WSAGetLastError() == WSAETIMEDOUT) {
                    continue;
                }
                std::cerr << "[OMEGA-TRADE] SSL error " << err << " -- reconnecting\n";
                break;
            }
            trade_recv_buf.append(buf, static_cast<size_t>(n));

            // Parse messages from trade session
            while (true) {
                const size_t bs = trade_recv_buf.find("8=FIX");
                if (bs == std::string::npos) { trade_recv_buf.clear(); break; }
                if (bs > 0) trade_recv_buf = trade_recv_buf.substr(bs);
                const size_t bl_pos = trade_recv_buf.find("\x01" "9=");
                if (bl_pos == std::string::npos) break;
                const size_t bl_start = bl_pos + 3u;
                const size_t bl_end   = trade_recv_buf.find('\x01', bl_start);
                if (bl_end == std::string::npos) break;
                int body_len = 0;
                try { body_len = std::stoi(trade_recv_buf.substr(bl_start, bl_end - bl_start)); }
                catch (...) { trade_recv_buf = trade_recv_buf.substr(bl_end); continue; }
                const size_t hdr_end = bl_end + 1u;
                const size_t msg_end = hdr_end + static_cast<size_t>(body_len) + 7u;
                if (msg_end > trade_recv_buf.size()) break;
                const std::string tmsg = trade_recv_buf.substr(0u, msg_end);
                trade_recv_buf = trade_recv_buf.substr(msg_end);

                const std::string ttype = extract_tag(tmsg, "35");
                if (ttype == "A") {
                    g_trade_ready.store(true);
                    std::cout << "[OMEGA-TRADE] LOGON ACCEPTED\n";
                    const std::string req_id = "omega-sec-" + std::to_string(nowSec());
                    const std::string sec_req = fix_build_security_list_request(g_trade_seq++, req_id);
                    SSL_write(ssl, sec_req.c_str(), static_cast<int>(sec_req.size()));
                    std::cout << "[OMEGA-TRADE] SecurityListRequest sent req_id=" << req_id << "\n";
                } else if (ttype == "8") {
                    // ExecutionReport -- order ACK / fill / reject
                    handle_execution_report(tmsg);
                } else if (ttype == "y") {
                    const auto entries = parse_security_list_entries(tmsg);
                    if (!entries.empty()) {
                        const bool ext_changed = apply_security_list_symbol_map(entries);
                        const std::string req_id = extract_tag(tmsg, "320");
                        std::cout << "[OMEGA-TRADE] SecurityList received req_id="
                                  << (req_id.empty() ? "?" : req_id)
                                  << " entries=" << entries.size();
                        if (ext_changed) std::cout << " (ext IDs updated -- will re-subscribe)";
                        std::cout << "\n";
                        std::cout.flush();
                        if (ext_changed) g_ext_md_refresh_needed.store(true);
                    }
                } else if (ttype == "5") {
                    std::cout << "[OMEGA-TRADE] Logout received\n";
                    break;
                } else if (ttype == "1") {
                    // TestRequest (35=1) -- FIX spec requires response with a Heartbeat
                    // containing the received TestReqID (tag 112). Without this response,
                    // the broker marks the session dead and terminates it silently.
                    //
                    // Root cause of Session 13's "4 unplanned disconnects" (05:53, 06:38,
                    // 08:20, 08:35 UTC on 2026-04-23). Quote session handles this correctly
                    // in fix_dispatch.hpp:43-48; trade session previously "silently absorbed"
                    // TestRequests per L135 comment, causing the trade-only drops observed.
                    const std::string trid = extract_tag(tmsg, "112");
                    const std::string hb   = build_heartbeat(g_trade_seq++, "TRADE", trid.c_str());
                    {
                        std::lock_guard<std::mutex> lk(g_trade_mtx);
                        if (SSL_write(ssl, hb.c_str(), static_cast<int>(hb.size())) <= 0) {
                            const int wsa = WSAGetLastError();
                            std::cerr << "[OMEGA-TRADE] TestReq-Heartbeat write failed (WSA="
                                      << wsa << ") -- reconnecting\n";
                            std::cerr.flush();
                            // Cannot break from inner while here without losing state; set a
                            // condition the outer loop will detect on its next iteration.
                            // Simplest: break inner, let outer SSL_read hit error and break.
                            break;
                        }
                    }
                    std::cout << "[OMEGA-TRADE] TestRequest -> Heartbeat reid=" << trid << "\n";
                } else if (ttype == "3" || ttype == "j") {
                    std::string r = tmsg.substr(0, 300);
                    for (char& c : r) if (c == '\x01') c = '|';
                    std::cerr << "[OMEGA-TRADE] REJECT type=" << ttype
                              << " text=" << extract_tag(tmsg, "58") << "\n";
                }
                // Heartbeats (type=0) silently absorbed -- they are server-side ACKs of
                // our own heartbeats or of TestRequests we sent; no response required.
            }
        }

        // Tear down: SO_SNDTIMEO (500ms, set in connect_ssl) ensures SSL_write
        // returns within 500ms max even if the server is slow to ACK.
        // DO NOT set FIONBIO here -- non-blocking mode causes SSL_write to return
        // WANT_WRITE immediately (0 bytes sent) which silently drops the logout.
        // SO_SNDTIMEO is the correct mechanism; FIONBIO defeats it.
        if (g_trade_ready.load()) {
            const std::string tlo = fix_build_logout(g_trade_seq++, "TRADE");
            SSL_write(ssl, tlo.c_str(), static_cast<int>(tlo.size()));
            std::cout << "[OMEGA-TRADE] Logout sent\n";
        }
        g_trade_ready.store(false);
        {
            std::lock_guard<std::mutex> lk(g_trade_mtx);
            g_trade_ssl  = nullptr;
            g_trade_sock = -1;
        }
        Sleep(150);  // let kernel flush logout to wire before closing
        // Close the socket FIRST -- SSL_free then finds a dead socket and
        // returns immediately rather than attempting any I/O.
        if (sock >= 0) closesocket(static_cast<SOCKET>(sock));
        SSL_free(ssl);

        // Reconnect after any disconnect
        std::cerr << "[OMEGA-TRADE] Disconnected -- reconnecting\n";
        for (int i = 0; i < 200 && g_running.load(); ++i) Sleep(10);
    }
    g_trade_thread_done.store(true);  // signal main() that trade_loop has fully exited
}

#include "quote_loop.hpp"
