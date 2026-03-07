// ==============================================================================
// OMEGA — Commodities & Indices Trading System
// Strategy: Compression Breakout (CRTP engine, zero virtual dispatch)
// Broker: BlackBull Markets — identical FIX stack to ChimeraMetals
// Primary: MES · MNQ · MCL  |  Confirmation: ES NQ CL VIX DX ZN YM RTY
// GUI: HTTP :7779 / WebSocket :7780
// ==============================================================================

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mstcpip.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#pragma comment(lib, "ws2_32.lib")

#include <iostream>
#include <atomic>
#include <string>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <fstream>
#include <chrono>
#include <mutex>
#include <algorithm>
#include <unordered_map>
#include <vector>
#include <deque>
#include <cmath>
#include <csignal>
#include <functional>
#include <cstdint>
#include <cstring>

// ── Omega headers (flat — all files in same directory on VPS) ────────────────
#include "OmegaTelemetryWriter.hpp"
#include "OmegaTradeLedger.hpp"
#include "BreakoutEngine.hpp"
#include "MacroRegimeDetector.hpp"
#include "OmegaTelemetryServer.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Singleton
// ─────────────────────────────────────────────────────────────────────────────
static HANDLE g_singleton_mutex = NULL;

// ─────────────────────────────────────────────────────────────────────────────
// Config
// ─────────────────────────────────────────────────────────────────────────────
struct OmegaConfig {
    // FIX — identical to ChimeraMetals
    std::string host       = "live-uk-eqx-02.p.c-trader.com";
    int         port       = 5211;
    std::string sender     = "live.blackbull.8077780";
    std::string target     = "cServer";
    std::string username   = "8077780";
    std::string password   = "Bowen6feb";
    int         heartbeat  = 30;

    std::string mode       = "SHADOW";

    // Breakout params
    double vol_thresh_pct        = 0.050;
    double tp_pct                = 0.400;
    double sl_pct                = 2.000;
    int    compression_lookback  = 50;
    int    baseline_lookback     = 200;
    double compression_threshold = 0.80;
    int    max_hold_sec          = 1500;
    int    min_entry_gap_sec     = 180;
    double max_spread_pct        = 0.05;
    double max_latency_ms        = 15.0;

    // Risk
    double daily_loss_limit  = 200.0;
    int    max_consec_losses = 3;
    int    loss_pause_sec    = 300;

    // Session UTC
    int session_start_utc = 7;
    int session_end_utc   = 21;

    // GUI
    int         gui_port   = 7779;
    int         ws_port    = 7780;
    std::string shadow_csv = "omega_shadow.csv";
};

static OmegaConfig         g_cfg;
static std::atomic<bool>   g_running(true);

// ─────────────────────────────────────────────────────────────────────────────
// Globals
// ─────────────────────────────────────────────────────────────────────────────
static OmegaTelemetryWriter      g_telemetry;
omega::OmegaTradeLedger          g_omegaLedger;      // extern in TelemetryServer.cpp
static omega::MacroRegimeDetector g_macroDetector;

// CRTP breakout engines — one per primary symbol
static omega::BreakoutEngine g_eng_mes("MES");
static omega::BreakoutEngine g_eng_mnq("MNQ");
static omega::BreakoutEngine g_eng_mcl("MCL");

// Book
static std::mutex                              g_book_mtx;
static std::unordered_map<std::string,double>  g_bids;
static std::unordered_map<std::string,double>  g_asks;

// RTT
static double              g_rtt_last = 0.0, g_rtt_p50 = 0.0, g_rtt_p95 = 0.0;
static std::deque<double>  g_rtts;
static int64_t             g_rtt_pending_ts = 0;
static std::string         g_rtt_pending_id;

// Governor counters
static int     g_gov_spread  = 0;
static int     g_gov_lat     = 0;
static int     g_gov_pnl     = 0;
static int     g_gov_consec  = 0;
static int     g_consec_losses = 0;
static bool    g_loss_pause    = false;
static int64_t g_loss_pause_until = 0;

// Shadow CSV
static std::ofstream g_shadow_csv;

// FIX recv buffer
static std::string g_recv_buf;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static int64_t nowSec() noexcept {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

static std::string timestamp() {
    const auto tp = std::chrono::system_clock::now();
    const auto t  = std::chrono::system_clock::to_time_t(tp);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        tp.time_since_epoch()) % 1000;
    struct tm ti; gmtime_s(&ti, &t);
    std::ostringstream o;
    o << std::put_time(&ti, "%Y%m%d-%H:%M:%S")
      << '.' << std::setw(3) << std::setfill('0') << ms.count();
    return o.str();
}

static std::string extract_tag(const std::string& msg, const char* tag) {
    const std::string pat = std::string(tag) + '=';
    const size_t pos = msg.find(pat);
    if (pos == std::string::npos) return {};
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

static std::string build_logon(int seq, const char* subID) {
    std::ostringstream b;
    b << "35=A\x01" << "49=" << g_cfg.sender << "\x01" << "56=" << g_cfg.target << "\x01"
      << "50=" << subID << "\x01" << "57=" << subID << "\x01" << "34=" << seq << "\x01"
      << "52=" << timestamp() << "\x01" << "98=0\x01" << "108=" << g_cfg.heartbeat << "\x01"
      << "141=Y\x01" << "553=" << g_cfg.username << "\x01" << "554=" << g_cfg.password << "\x01";
    return wrap_fix(b.str());
}

static std::string build_marketdata_req(int seq) {
    static const char* syms[] = { "MES","MNQ","MCL","ES","NQ","CL","VIX","DX","ZN","YM","RTY" };
    static const int nsyms = 11;
    std::ostringstream b;
    b << "35=V\x01"
      << "49=" << g_cfg.sender << "\x01" << "56=" << g_cfg.target << "\x01"
      << "50=QUOTE\x01" << "57=QUOTE\x01"
      << "34=" << seq << "\x01" << "52=" << timestamp() << "\x01"
      << "262=OMEGA-MD-001\x01" << "263=1\x01" << "264=0\x01" << "265=0\x01"
      << "146=" << nsyms << "\x01";
    for (int i = 0; i < nsyms; ++i) b << "55=" << syms[i] << "\x01";
    b << "267=2\x01" << "269=0\x01" << "269=1\x01";
    return wrap_fix(b.str());
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

// ─────────────────────────────────────────────────────────────────────────────
// SSL connect (identical to ChimeraMetals — untouched)
// ─────────────────────────────────────────────────────────────────────────────
static SSL* connect_ssl(const std::string& host, int port, int& sock_out) {
    struct addrinfo hints{}, *result = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &result) != 0)
        return nullptr;
    SOCKET sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (sock == INVALID_SOCKET) { freeaddrinfo(result); return nullptr; }
    if (connect(sock, result->ai_addr, static_cast<int>(result->ai_addrlen)) != 0) {
        freeaddrinfo(result); closesocket(sock); return nullptr;
    }
    freeaddrinfo(result);
    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&flag), sizeof(flag));
    setsockopt(sock, SOL_SOCKET,  SO_KEEPALIVE, reinterpret_cast<const char*>(&flag), sizeof(flag));
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) { closesocket(sock); return nullptr; }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL* ssl = SSL_new(ctx);
    if (!ssl) { SSL_CTX_free(ctx); closesocket(sock); return nullptr; }
    SSL_set_fd(ssl, static_cast<int>(sock));
    if (SSL_connect(ssl) <= 0) {
        ERR_print_errors_fp(stderr);
        SSL_free(ssl); SSL_CTX_free(ctx); closesocket(sock); return nullptr;
    }
    sock_out = static_cast<int>(sock);
    return ssl;
}

// ─────────────────────────────────────────────────────────────────────────────
// RTT
// ─────────────────────────────────────────────────────────────────────────────
static void rtt_record(double ms) {
    g_rtt_last = ms;
    g_rtts.push_back(ms);
    if (g_rtts.size() > 200u) g_rtts.pop_front();
    std::vector<double> v(g_rtts.begin(), g_rtts.end());
    std::sort(v.begin(), v.end());
    g_rtt_p50 = v[static_cast<size_t>(v.size() * 0.50)];
    g_rtt_p95 = v[static_cast<size_t>(v.size() * 0.95)];
}

// ─────────────────────────────────────────────────────────────────────────────
// Shadow CSV
// ─────────────────────────────────────────────────────────────────────────────
static void write_shadow_csv(const omega::TradeRecord& tr) {
    if (!g_shadow_csv.is_open()) return;
    g_shadow_csv << tr.entryTs << ',' << tr.symbol << ',' << tr.side
                 << ',' << tr.entryPrice << ',' << tr.exitPrice
                 << ',' << tr.pnl << ',' << tr.mfe << ',' << tr.mae
                 << ',' << (tr.exitTs - tr.entryTs)
                 << ',' << tr.exitReason
                 << ',' << tr.spreadAtEntry
                 << ',' << tr.latencyMs
                 << ',' << tr.regime << '\n';
    g_shadow_csv.flush();
}

// ─────────────────────────────────────────────────────────────────────────────
// Session
// ─────────────────────────────────────────────────────────────────────────────
static bool session_tradeable() noexcept {
    const auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    struct tm ti; gmtime_s(&ti, &t);
    return (ti.tm_hour >= g_cfg.session_start_utc && ti.tm_hour < g_cfg.session_end_utc);
}

// ─────────────────────────────────────────────────────────────────────────────
// Apply config to engines
// ─────────────────────────────────────────────────────────────────────────────
static void apply_engine_config(omega::BreakoutEngine& eng) noexcept {
    eng.VOL_THRESH_PCT        = g_cfg.vol_thresh_pct;
    eng.TP_PCT                = g_cfg.tp_pct;
    eng.SL_PCT                = g_cfg.sl_pct;
    eng.COMPRESSION_LOOKBACK  = g_cfg.compression_lookback;
    eng.BASELINE_LOOKBACK     = g_cfg.baseline_lookback;
    eng.COMPRESSION_THRESHOLD = g_cfg.compression_threshold;
    eng.MAX_HOLD_SEC          = g_cfg.max_hold_sec;
    eng.MIN_GAP_SEC           = g_cfg.min_entry_gap_sec;
    eng.MAX_SPREAD_PCT        = g_cfg.max_spread_pct;
}

// ─────────────────────────────────────────────────────────────────────────────
// Config loader
// ─────────────────────────────────────────────────────────────────────────────
static void load_config(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) { std::cout << "[CONFIG] Using defaults\n"; return; }
    std::string line, section;
    auto trim = [](std::string& s) {
        while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
        while (!s.empty() && s.front() == ' ') s = s.substr(1);
    };
    while (std::getline(f, line)) {
        trim(line);
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '[') { section = line.substr(1, line.find(']') - 1); continue; }
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        trim(k); trim(v);
        const auto cm = v.find('#');
        if (cm != std::string::npos) v = v.substr(0, cm);
        trim(v);
        if (v.empty()) continue;

        if (section == "fix") {
            if (k=="host")               g_cfg.host      = v;
            if (k=="port")               g_cfg.port      = std::stoi(v);
            if (k=="sender_comp_id")     g_cfg.sender    = v;
            if (k=="target_comp_id")     g_cfg.target    = v;
            if (k=="username")           g_cfg.username  = v;
            if (k=="password")           g_cfg.password  = v;
            if (k=="heartbeat_interval") g_cfg.heartbeat = std::stoi(v);
        }
        if (section == "mode"     && k=="mode")         g_cfg.mode           = v;
        if (section == "breakout") {
            if (k=="vol_thresh_pct")        g_cfg.vol_thresh_pct        = std::stod(v);
            if (k=="tp_pct")                g_cfg.tp_pct                = std::stod(v);
            if (k=="sl_pct")                g_cfg.sl_pct                = std::stod(v);
            if (k=="compression_lookback")  g_cfg.compression_lookback  = std::stoi(v);
            if (k=="baseline_lookback")     g_cfg.baseline_lookback     = std::stoi(v);
            if (k=="compression_threshold") g_cfg.compression_threshold = std::stod(v);
            if (k=="max_hold_sec")          g_cfg.max_hold_sec          = std::stoi(v);
            if (k=="min_entry_gap_sec")     g_cfg.min_entry_gap_sec     = std::stoi(v);
            if (k=="max_spread_entry_pct")  g_cfg.max_spread_pct        = std::stod(v);
        }
        if (section == "risk") {
            if (k=="daily_loss_limit")  g_cfg.daily_loss_limit  = std::stod(v);
            if (k=="max_consec_losses") g_cfg.max_consec_losses = std::stoi(v);
            if (k=="loss_pause_sec")    g_cfg.loss_pause_sec    = std::stoi(v);
        }
        if (section == "session") {
            if (k=="session_start_utc") g_cfg.session_start_utc = std::stoi(v);
            if (k=="session_end_utc")   g_cfg.session_end_utc   = std::stoi(v);
        }
        if (section == "telemetry") {
            if (k=="gui_port")   g_cfg.gui_port   = std::stoi(v);
            if (k=="shadow_csv") g_cfg.shadow_csv = v;
        }
    }
    std::cout << "[CONFIG] mode=" << g_cfg.mode
              << " vol=" << g_cfg.vol_thresh_pct
              << "% tp=" << g_cfg.tp_pct
              << "% sl=" << g_cfg.sl_pct
              << "% maxhold=" << g_cfg.max_hold_sec << "s\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Signal handler
// ─────────────────────────────────────────────────────────────────────────────
static void sig_handler(int) noexcept { g_running.store(false); }

// ─────────────────────────────────────────────────────────────────────────────
// Tick handler — called for every bid/ask update
// ─────────────────────────────────────────────────────────────────────────────
static void on_tick(const std::string& sym, double bid, double ask) {
    { std::lock_guard<std::mutex> lk(g_book_mtx); g_bids[sym] = bid; g_asks[sym] = ask; }

    const double mid = (bid + ask) * 0.5;
    if (sym == "VIX") g_macroDetector.updateVIX(mid);
    if (sym == "ES")  g_macroDetector.updateES(mid);
    if (sym == "NQ")  g_macroDetector.updateNQ(mid);

    g_telemetry.UpdatePrice(sym.c_str(), bid, ask);

    const std::string regime = g_macroDetector.regime();
    g_telemetry.UpdateMacroRegime(
        g_macroDetector.vixLevel(), regime.c_str(), g_macroDetector.esNqDivergence());

    const bool tradeable = session_tradeable();
    g_telemetry.UpdateSession(tradeable ? "ACTIVE" : "CLOSED", tradeable ? 1 : 0);

    // ── Governor gates ────────────────────────────────────────────────────────
    if (g_omegaLedger.dailyPnl() < -g_cfg.daily_loss_limit) {
        ++g_gov_pnl;
        g_telemetry.UpdateGovernor(g_gov_spread, g_gov_lat, g_gov_pnl, 0, g_gov_consec);
        return;
    }
    if (g_loss_pause) {
        if (nowSec() < g_loss_pause_until) {
            ++g_gov_consec;
            g_telemetry.UpdateGovernor(g_gov_spread, g_gov_lat, g_gov_pnl, 0, g_gov_consec);
            return;
        }
        g_loss_pause = false;
    }
    if (!tradeable) {
        g_telemetry.UpdateGovernor(g_gov_spread, g_gov_lat, g_gov_pnl, 0, g_gov_consec);
        return;
    }
    if (g_rtt_last > 0.0 && g_rtt_last > g_cfg.max_latency_ms) {
        ++g_gov_lat;
        g_telemetry.UpdateGovernor(g_gov_spread, g_gov_lat, g_gov_pnl, 0, g_gov_consec);
        return;
    }

    // ── Route to engine ───────────────────────────────────────────────────────
    omega::BreakoutEngine* eng = nullptr;
    if      (sym == "MES") eng = &g_eng_mes;
    else if (sym == "MNQ") eng = &g_eng_mnq;
    else if (sym == "MCL") eng = &g_eng_mcl;
    else {
        g_telemetry.UpdateGovernor(g_gov_spread, g_gov_lat, g_gov_pnl, 0, g_gov_consec);
        return;
    }

    auto on_close = [&](const omega::TradeRecord& tr) {
        g_omegaLedger.record(tr);
        write_shadow_csv(tr);
        if (tr.pnl <= 0.0) {
            if (++g_consec_losses >= g_cfg.max_consec_losses) {
                g_loss_pause       = true;
                g_loss_pause_until = nowSec() + g_cfg.loss_pause_sec;
                std::cout << "[OMEGA-RISK] " << g_cfg.max_consec_losses
                          << " consecutive losses — pause " << g_cfg.loss_pause_sec << "s\n";
            }
        } else { g_consec_losses = 0; }
        g_telemetry.UpdateStats(
            g_omegaLedger.dailyPnl(), g_omegaLedger.maxDD(),
            g_omegaLedger.total(), g_omegaLedger.wins(), g_omegaLedger.losses(),
            g_omegaLedger.winRate(), g_omegaLedger.avgWin(), g_omegaLedger.avgLoss(), 0, 0);
        g_telemetry.UpdateLastSignal(tr.symbol.c_str(), "CLOSED", tr.exitPrice, tr.exitReason.c_str());
    };

    const auto sig = eng->update(bid, ask, g_rtt_last, regime.c_str(), on_close);

    g_telemetry.UpdateEngineState(sym.c_str(),
        static_cast<int>(eng->phase), eng->comp_high, eng->comp_low,
        eng->recent_vol_pct, eng->base_vol_pct, eng->signal_count);

    if (sig.valid) {
        g_telemetry.UpdateLastSignal(sym.c_str(),
            sig.is_long ? "LONG" : "SHORT", sig.entry, sig.reason);
        std::cout << "\033[1;" << (sig.is_long ? "32" : "31") << "m"
                  << "[OMEGA] " << sym << " " << (sig.is_long ? "LONG" : "SHORT")
                  << " entry=" << sig.entry << " tp=" << sig.tp << " sl=" << sig.sl
                  << " regime=" << regime << "\033[0m\n";
    }

    g_telemetry.UpdateGovernor(g_gov_spread, g_gov_lat, g_gov_pnl, 0, g_gov_consec);
}

// ─────────────────────────────────────────────────────────────────────────────
// FIX message extraction
// ─────────────────────────────────────────────────────────────────────────────
static std::vector<std::string> extract_messages(const char* data, int n) {
    g_recv_buf.append(data, static_cast<size_t>(n));
    std::vector<std::string> msgs;
    while (true) {
        const size_t bs = g_recv_buf.find("8=FIX");
        if (bs == std::string::npos) { g_recv_buf.clear(); break; }
        if (bs > 0u) g_recv_buf = g_recv_buf.substr(bs);
        const size_t bl_pos = g_recv_buf.find("\x01" "9=");
        if (bl_pos == std::string::npos) break;
        const size_t bl_start = bl_pos + 3u;
        const size_t bl_end   = g_recv_buf.find('\x01', bl_start);
        if (bl_end == std::string::npos) break;
        const int    body_len = std::stoi(g_recv_buf.substr(bl_start, bl_end - bl_start));
        const size_t hdr_end  = bl_end + 1u;
        const size_t msg_end  = hdr_end + static_cast<size_t>(body_len) + 7u;
        if (msg_end > g_recv_buf.size()) break;
        msgs.push_back(g_recv_buf.substr(0u, msg_end));
        g_recv_buf = g_recv_buf.substr(msg_end);
    }
    return msgs;
}

// ─────────────────────────────────────────────────────────────────────────────
// FIX dispatch
// ─────────────────────────────────────────────────────────────────────────────
static void dispatch_fix(const std::string& msg, SSL* ssl) {
    const std::string type = extract_tag(msg, "35");

    if (type == "A") {
        std::cout << "[OMEGA] LOGON ACCEPTED\n";
        g_telemetry.UpdateFixStatus("CONNECTED", "CONNECTED", 0, 0);
        const std::string md = build_marketdata_req(g_quote_seq++);
        SSL_write(ssl, md.c_str(), static_cast<int>(md.size()));
        std::cout << "[OMEGA] Subscribed: MES MNQ MCL ES NQ CL VIX DX ZN YM RTY\n";
        return;
    }

    if (type == "0") {
        // Heartbeat response — measure RTT
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
        // TestRequest from server — respond
        const std::string trid = extract_tag(msg, "112");
        const std::string hb   = build_heartbeat(g_quote_seq++, "QUOTE", trid.c_str());
        SSL_write(ssl, hb.c_str(), static_cast<int>(hb.size()));
        return;
    }

    if (type == "W" || type == "X") {
        const std::string sym = extract_tag(msg, "55");
        if (sym.empty()) return;
        double bid = 0.0, ask = 0.0;
        size_t pos = 0u;
        while ((pos = msg.find("269=", pos)) != std::string::npos) {
            const char et  = msg[pos + 4u];
            const size_t px  = msg.find("270=", pos);
            if (px == std::string::npos) break;
            const size_t pxe = msg.find('\x01', px);
            if (pxe == std::string::npos) break;
            const double price = std::stod(msg.substr(px + 4u, pxe - (px + 4u)));
            if (et == '0') bid = price;
            else if (et == '1') ask = price;
            pos = pxe;
        }
        if (bid > 0.0 && ask > 0.0) on_tick(sym, bid, ask);
        return;
    }

    if (type == "3" || type == "j") {
        std::cerr << "[OMEGA] FIX REJECT: " << extract_tag(msg, "58") << "\n";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Quote loop
// ─────────────────────────────────────────────────────────────────────────────
static void quote_loop() {
    int backoff_ms = 1000;
    const int max_backoff = 30000;

    while (g_running.load()) {
        std::cout << "[OMEGA] Connecting " << g_cfg.host << ":" << g_cfg.port << "\n";
        g_telemetry.UpdateFixStatus("CONNECTING", "CONNECTING", 0, 0);

        int sock = -1;
        SSL* ssl = connect_ssl(g_cfg.host, g_cfg.port, sock);
        if (!ssl) {
            std::cerr << "[OMEGA] Connect failed — retry " << backoff_ms << "ms\n";
            Sleep(static_cast<DWORD>(backoff_ms));
            backoff_ms = std::min(backoff_ms * 2, max_backoff);
            continue;
        }

        backoff_ms = 1000;
        g_recv_buf.clear();
        g_quote_seq      = 1;
        g_rtt_pending_ts = 0;

        const std::string logon = build_logon(g_quote_seq++, "QUOTE");
        SSL_write(ssl, logon.c_str(), static_cast<int>(logon.size()));
        std::cout << "[OMEGA] Logon sent\n";

        auto last_ping = std::chrono::steady_clock::now();
        auto last_diag = std::chrono::steady_clock::now();

        while (g_running.load()) {
            const auto now = std::chrono::steady_clock::now();

            // RTT ping every 5s
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_ping).count() >= 5) {
                last_ping = now;
                if (g_rtt_pending_ts == 0) {
                    g_rtt_pending_id = "omega-" + std::to_string(nowSec());
                    g_rtt_pending_ts = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                    const std::string tr = build_test_request(g_quote_seq++, "QUOTE", g_rtt_pending_id);
                    SSL_write(ssl, tr.c_str(), static_cast<int>(tr.size()));
                }
            }

            // Diagnostic every 5 min
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_diag).count() >= 300) {
                last_diag = now;
                std::cout << "[OMEGA-DIAG] PnL=" << g_omegaLedger.dailyPnl()
                          << " T=" << g_omegaLedger.total()
                          << " WR=" << g_omegaLedger.winRate() << "%"
                          << " RTTp95=" << g_rtt_p95 << "ms"
                          << " MES=" << static_cast<int>(g_eng_mes.phase)
                          << " MNQ=" << static_cast<int>(g_eng_mnq.phase)
                          << " MCL=" << static_cast<int>(g_eng_mcl.phase) << "\n";
            }

            char buf[8192];
            const int n = SSL_read(ssl, buf, static_cast<int>(sizeof(buf)) - 1);
            if (n <= 0) {
                const int err = SSL_get_error(ssl, n);
                if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                    Sleep(1); continue;
                }
                std::cerr << "[OMEGA] SSL error " << err << " — reconnecting\n";
                break;
            }
            for (const auto& m : extract_messages(buf, n)) dispatch_fix(m, ssl);
        }

        // Force-close on disconnect
        auto fc = [](omega::BreakoutEngine& eng, const char* sym) {
            if (!eng.pos.active) return;
            double bid = 0.0, ask = 0.0;
            { std::lock_guard<std::mutex> lk(g_book_mtx);
              const auto bi = g_bids.find(sym); if (bi != g_bids.end()) bid = bi->second;
              const auto ai = g_asks.find(sym); if (ai != g_asks.end()) ask = ai->second; }
            if (bid > 0.0 && ask > 0.0)
                eng.forceClose(bid, ask, "DISCONNECT", g_rtt_last,
                    g_macroDetector.regime().c_str(),
                    [](const omega::TradeRecord& tr) {
                        g_omegaLedger.record(tr); write_shadow_csv(tr);
                    });
        };
        fc(g_eng_mes, "MES"); fc(g_eng_mnq, "MNQ"); fc(g_eng_mcl, "MCL");

        SSL_shutdown(ssl); SSL_free(ssl); closesocket(static_cast<SOCKET>(sock));
        g_telemetry.UpdateFixStatus("DISCONNECTED", "DISCONNECTED", 0, 0);
        Sleep(static_cast<DWORD>(backoff_ms));
        backoff_ms = std::min(backoff_ms * 2, max_backoff);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[])
{
    g_singleton_mutex = CreateMutexA(NULL, TRUE, "Global\\Omega_Breakout_System");
    if (!g_singleton_mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        std::cerr << "[OMEGA] Already running\n"; return 1;
    }

    SetConsoleOutputCP(CP_UTF8);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dwMode = 0; GetConsoleMode(hOut, &dwMode);
    SetConsoleMode(hOut, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    std::cout << "\033[1;36m"
              << "=======================================================\n"
              << "  OMEGA  |  Commodities & Indices  |  Breakout System  \n"
              << "  Primary: MES  MNQ  MCL  |  Shadow Mode               \n"
              << "=======================================================\n"
              << "\033[0m";

    std::signal(SIGINT,  sig_handler);
    std::signal(SIGTERM, sig_handler);

    const std::string cfg_path = (argc > 1) ? argv[1] : "omega_config.ini";
    load_config(cfg_path);
    apply_engine_config(g_eng_mes);
    apply_engine_config(g_eng_mnq);
    apply_engine_config(g_eng_mcl);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "[OMEGA] WSAStartup failed\n"; return 1;
    }
    SSL_library_init(); SSL_load_error_strings(); OpenSSL_add_all_algorithms();

    if (!g_telemetry.Init()) std::cerr << "[OMEGA] Telemetry init failed\n";
    g_telemetry.SetMode(g_cfg.mode.c_str());

    g_shadow_csv.open(g_cfg.shadow_csv, std::ios::app);
    if (g_shadow_csv.is_open()) {
        g_shadow_csv.seekp(0, std::ios::end);
        if (g_shadow_csv.tellp() == std::streampos(0))
            g_shadow_csv << "ts_unix,symbol,side,entry_px,exit_px,pnl,mfe,mae,"
                            "hold_sec,reason,spread_at_entry,latency_ms,regime\n";
        std::cout << "[OMEGA] Shadow CSV: " << g_cfg.shadow_csv << "\n";
    }

    omega::OmegaTelemetryServer gui_server;
    gui_server.start(g_cfg.gui_port, g_cfg.ws_port, g_telemetry.snap());
    std::cout << "[OMEGA] GUI http://localhost:" << g_cfg.gui_port
              << "  WS:" << g_cfg.ws_port << "\n";

    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    std::cout << "[OMEGA] FIX loop starting — " << g_cfg.mode << " mode\n";
    quote_loop();

    std::cout << "[OMEGA] Shutdown\n";
    gui_server.stop();
    g_shadow_csv.close();
    WSACleanup();
    ReleaseMutex(g_singleton_mutex);
    CloseHandle(g_singleton_mutex);
    return 0;
}
