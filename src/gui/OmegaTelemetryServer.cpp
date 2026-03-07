// ==============================================================================
// OmegaTelemetryServer.cpp
// HTTP :7779 (GUI + REST) + WebSocket :7780 (250ms push)
// All MSVC /W4 /WX issues resolved:
//   - WS frame bytes use static_cast<char> from uint8_t constants
//   - No truncation warnings
// ==============================================================================
#include "OmegaTelemetryServer.hpp"
#include "OmegaTradeLedger.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

#include <openssl/sha.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>

#include <fstream>
#include <sstream>
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <chrono>
#include <thread>
#include <cstdint>
#include <cstdio>

extern omega::OmegaTradeLedger g_omegaLedger;

namespace omega {

// ─────────────────────────────────────────────────────────────────────────────
// Utilities
// ─────────────────────────────────────────────────────────────────────────────

static std::string loadFile(const std::string& p)
{
    static const char* bases[] = {
        "", "C:\\Omega\\", "..\\", ".\\"
    };
    for (const char* base : bases) {
        std::string full = std::string(base) + p;
        std::ifstream f(full, std::ios::binary);
        if (f.is_open()) {
            std::ostringstream ss; ss << f.rdbuf();
            return ss.str();
        }
    }
    return "";
}

static std::string base64Encode(const unsigned char* data, size_t len)
{
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO* mem = BIO_new(BIO_s_mem());
    b64 = BIO_push(b64, mem);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(b64, data, static_cast<int>(len));
    BIO_flush(b64);
    BUF_MEM* bptr; BIO_get_mem_ptr(b64, &bptr);
    std::string r(bptr->data, bptr->length);
    BIO_free_all(b64);
    return r;
}

static std::string extractHeader(const std::string& req, const std::string& name)
{
    std::string lr = req, ln = name;
    std::transform(lr.begin(), lr.end(), lr.begin(), ::tolower);
    std::transform(ln.begin(), ln.end(), ln.begin(), ::tolower);
    size_t pos = lr.find(ln + ":");
    if (pos == std::string::npos) return "";
    pos += ln.size() + 1;
    while (pos < req.size() && (req[pos] == ' ' || req[pos] == '\t')) pos++;
    size_t end = req.find("\r\n", pos);
    if (end == std::string::npos) end = req.find('\n', pos);
    if (end == std::string::npos) end = req.size();
    return req.substr(pos, end - pos);
}

// ─────────────────────────────────────────────────────────────────────────────
// JSON builders
// ─────────────────────────────────────────────────────────────────────────────

static std::string buildTelemetryJson(const OmegaTelemetrySnapshot* s)
{
    if (!s) return "{}";
    const int    trades = s->total_trades;
    const int    wins   = s->wins;
    const double wr     = (trades > 0) ? (100.0 * wins / trades) : 0.0;
    char buf[8192];
    snprintf(buf, sizeof(buf),
        "{"
        "\"mes_bid\":%.4f,\"mes_ask\":%.4f,"
        "\"mnq_bid\":%.4f,\"mnq_ask\":%.4f,"
        "\"mcl_bid\":%.4f,\"mcl_ask\":%.4f,"
        "\"es_bid\":%.4f,\"es_ask\":%.4f,"
        "\"nq_bid\":%.4f,\"nq_ask\":%.4f,"
        "\"cl_bid\":%.4f,\"cl_ask\":%.4f,"
        "\"vix_bid\":%.4f,\"vix_ask\":%.4f,"
        "\"dx_bid\":%.4f,\"dx_ask\":%.4f,"
        "\"zn_bid\":%.4f,\"zn_ask\":%.4f,"
        "\"ym_bid\":%.4f,\"ym_ask\":%.4f,"
        "\"rty_bid\":%.4f,\"rty_ask\":%.4f,"
        "\"daily_pnl\":%.2f,\"max_drawdown\":%.2f,"
        "\"fix_rtt_last\":%.2f,\"fix_rtt_p50\":%.2f,\"fix_rtt_p95\":%.2f,"
        "\"total_trades\":%d,\"wins\":%d,\"losses\":%d,\"win_rate\":%.1f,"
        "\"avg_win\":%.2f,\"avg_loss\":%.2f,"
        "\"total_orders\":%d,\"total_fills\":%d,"
        "\"fix_quote_status\":\"%s\",\"fix_trade_status\":\"%s\","
        "\"quote_msg_rate\":%d,\"sequence_gaps\":%d,"
        "\"mode\":\"%s\","
        "\"session_name\":\"%s\",\"session_tradeable\":%d,"
        "\"mes_phase\":%d,\"mes_comp_high\":%.4f,\"mes_comp_low\":%.4f,"
        "\"mes_recent_vol_pct\":%.4f,\"mes_baseline_vol_pct\":%.4f,\"mes_signals\":%d,"
        "\"mnq_phase\":%d,\"mnq_comp_high\":%.4f,\"mnq_comp_low\":%.4f,"
        "\"mnq_recent_vol_pct\":%.4f,\"mnq_baseline_vol_pct\":%.4f,\"mnq_signals\":%d,"
        "\"mcl_phase\":%d,\"mcl_comp_high\":%.4f,\"mcl_comp_low\":%.4f,"
        "\"mcl_recent_vol_pct\":%.4f,\"mcl_baseline_vol_pct\":%.4f,\"mcl_signals\":%d,"
        "\"last_signal_symbol\":\"%s\",\"last_signal_side\":\"%s\","
        "\"last_signal_price\":%.4f,\"last_signal_reason\":\"%s\","
        "\"vix_level\":%.2f,\"macro_regime\":\"%s\",\"es_nq_divergence\":%.6f,"
        "\"gov_spread\":%d,\"gov_latency\":%d,\"gov_pnl\":%d,"
        "\"gov_positions\":%d,\"gov_consec_loss\":%d"
        "}",
        s->mes_bid, s->mes_ask, s->mnq_bid, s->mnq_ask,
        s->mcl_bid, s->mcl_ask, s->es_bid,  s->es_ask,
        s->nq_bid,  s->nq_ask,  s->cl_bid,  s->cl_ask,
        s->vix_bid, s->vix_ask, s->dx_bid,  s->dx_ask,
        s->zn_bid,  s->zn_ask,  s->ym_bid,  s->ym_ask,
        s->rty_bid, s->rty_ask,
        s->daily_pnl, s->max_drawdown,
        s->fix_rtt_last, s->fix_rtt_p50, s->fix_rtt_p95,
        trades, wins, s->losses, wr,
        s->avg_win, s->avg_loss,
        s->total_orders, s->total_fills,
        s->fix_quote_status, s->fix_trade_status,
        s->quote_msg_rate, s->sequence_gaps,
        s->mode,
        s->session_name, s->session_tradeable,
        s->mes_phase, s->mes_comp_high, s->mes_comp_low,
        s->mes_recent_vol_pct, s->mes_baseline_vol_pct, s->mes_signals,
        s->mnq_phase, s->mnq_comp_high, s->mnq_comp_low,
        s->mnq_recent_vol_pct, s->mnq_baseline_vol_pct, s->mnq_signals,
        s->mcl_phase, s->mcl_comp_high, s->mcl_comp_low,
        s->mcl_recent_vol_pct, s->mcl_baseline_vol_pct, s->mcl_signals,
        s->last_signal_symbol, s->last_signal_side,
        s->last_signal_price, s->last_signal_reason,
        s->vix_level, s->macro_regime, s->es_nq_divergence,
        s->gov_spread, s->gov_latency, s->gov_pnl,
        s->gov_positions, s->gov_consec_loss
    );
    return buf;
}

static std::string buildTradesJson()
{
    const auto trades = g_omegaLedger.snapshot();
    std::string out;
    out.reserve(4096);
    out += '[';
    bool first = true;
    for (auto it = trades.crbegin(); it != trades.crend(); ++it) {
        const TradeRecord& t = *it;
        if (!first) out += ',';
        first = false;
        char row[512];
        snprintf(row, sizeof(row),
            "{\"id\":%d,\"symbol\":\"%s\",\"side\":\"%s\","
            "\"price\":%.4f,\"exitPrice\":%.4f,\"tp\":%.4f,\"sl\":%.4f,"
            "\"pnl\":%.4f,\"mfe\":%.4f,\"mae\":%.4f,"
            "\"entryTs\":%lld,\"exitTs\":%lld,"
            "\"exitReason\":\"%s\",\"engine\":\"%s\",\"regime\":\"%s\"}",
            t.id, t.symbol.c_str(), t.side.c_str(),
            t.entryPrice, t.exitPrice, t.tp, t.sl,
            t.pnl, t.mfe, t.mae,
            static_cast<long long>(t.entryTs), static_cast<long long>(t.exitTs),
            t.exitReason.c_str(), t.engine.c_str(), t.regime.c_str());
        out += row;
    }
    out += ']';
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// OmegaTelemetryServer ctor / dtor / start / stop
// ─────────────────────────────────────────────────────────────────────────────

OmegaTelemetryServer::OmegaTelemetryServer()
    : running_(false)
    , server_fd_(INVALID_SOCKET)
    , ws_fd_(INVALID_SOCKET)
    , ws_port_(7780)
    , hMap_(NULL)
    , snap_(nullptr)
{}

OmegaTelemetryServer::~OmegaTelemetryServer() { stop(); }

void OmegaTelemetryServer::start(int http_port, int ws_port)
{
    if (running_.load()) return;
    ws_port_ = ws_port;
    hMap_ = OpenFileMappingA(FILE_MAP_READ, FALSE, "Global\\OmegaTelemetrySharedMemory");
    if (hMap_)
        snap_ = static_cast<OmegaTelemetrySnapshot*>(
                    MapViewOfFile(hMap_, FILE_MAP_READ, 0, 0, sizeof(OmegaTelemetrySnapshot)));
    running_.store(true);
    thread_    = std::thread(&OmegaTelemetryServer::run,             this, http_port);
    ws_thread_ = std::thread(&OmegaTelemetryServer::wsBroadcastLoop, this);
}

void OmegaTelemetryServer::stop()
{
    if (!running_.load()) return;
    running_.store(false);
    if (server_fd_ != INVALID_SOCKET) closesocket(server_fd_);
    if (ws_fd_     != INVALID_SOCKET) closesocket(ws_fd_);
    if (thread_.joinable())    thread_.join();
    if (ws_thread_.joinable()) ws_thread_.join();
    if (snap_) UnmapViewOfFile(snap_);
    if (hMap_) CloseHandle(hMap_);
}

// ─────────────────────────────────────────────────────────────────────────────
// WebSocket helpers
// ─────────────────────────────────────────────────────────────────────────────

std::string OmegaTelemetryServer::wsHandshakeResponse(const std::string& request)
{
    const std::string key = extractHeader(request, "Sec-WebSocket-Key");
    if (key.empty()) return "";
    const std::string magic = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char sha1[20];
    SHA1(reinterpret_cast<const unsigned char*>(magic.c_str()), magic.size(), sha1);
    const std::string accept = base64Encode(sha1, 20);
    return "HTTP/1.1 101 Switching Protocols\r\n"
           "Upgrade: websocket\r\nConnection: Upgrade\r\n"
           "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
}

std::string OmegaTelemetryServer::wsBuildFrame(const std::string& payload)
{
    // Use uint8_t constants to avoid MSVC C4309 truncation warning,
    // then narrow to char via static_cast for std::string::push_back.
    std::string frame;
    frame.reserve(payload.size() + 10);

    // FIN=1, opcode=1 (text) => 0x81
    frame.push_back(static_cast<char>(static_cast<uint8_t>(0x81u)));

    const size_t len = payload.size();
    if (len < 126u) {
        frame.push_back(static_cast<char>(static_cast<uint8_t>(len)));
    } else if (len < 65536u) {
        frame.push_back(static_cast<char>(static_cast<uint8_t>(126u)));
        frame.push_back(static_cast<char>(static_cast<uint8_t>((len >> 8u) & 0xFFu)));
        frame.push_back(static_cast<char>(static_cast<uint8_t>( len        & 0xFFu)));
    } else {
        frame.push_back(static_cast<char>(static_cast<uint8_t>(127u)));
        for (int i = 7; i >= 0; --i)
            frame.push_back(static_cast<char>(static_cast<uint8_t>((len >> (8u * static_cast<unsigned>(i))) & 0xFFu)));
    }
    frame += payload;
    return frame;
}

bool OmegaTelemetryServer::wsSendFrame(SOCKET s, const std::string& payload)
{
    const std::string frame = wsBuildFrame(payload);
    return send(s, frame.c_str(), static_cast<int>(frame.size()), 0) > 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// WebSocket broadcast loop — 250ms push
// ─────────────────────────────────────────────────────────────────────────────

void OmegaTelemetryServer::wsBroadcastLoop()
{
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);

    ws_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(ws_fd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(static_cast<u_short>(ws_port_));

    if (bind(ws_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
        listen(ws_fd_, 8) == SOCKET_ERROR) {
        std::cerr << "[OmegaWS] bind/listen failed\n"; return;
    }

    u_long nb = 1;
    ioctlsocket(ws_fd_, FIONBIO, &nb);

    std::cout << "[OmegaWS] WebSocket port " << ws_port_ << "\n";

    auto last_broadcast = std::chrono::steady_clock::now();

    while (running_.load()) {
        sockaddr_in ca{}; int cl = sizeof(ca);
        SOCKET c = accept(ws_fd_, reinterpret_cast<sockaddr*>(&ca), &cl);
        if (c != INVALID_SOCKET) {
            char buf[1024];
            const int n = recv(c, buf, static_cast<int>(sizeof(buf)) - 1, 0);
            if (n > 0) {
                buf[n] = '\0';
                if (strstr(buf, "Upgrade: websocket") || strstr(buf, "Upgrade: WebSocket")) {
                    const std::string resp = wsHandshakeResponse(std::string(buf, static_cast<size_t>(n)));
                    if (!resp.empty()) {
                        send(c, resp.c_str(), static_cast<int>(resp.size()), 0);
                        std::lock_guard<std::mutex> lk(ws_mutex_);
                        ws_clients_.push_back(c);
                    } else { closesocket(c); }
                } else { closesocket(c); }
            } else { closesocket(c); }
        }

        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_broadcast).count() >= 250) {
            last_broadcast = now;
            std::lock_guard<std::mutex> lk(ws_mutex_);
            if (!ws_clients_.empty()) {
                const std::string payload = buildTelemetryJson(snap_);
                std::vector<SOCKET> alive;
                alive.reserve(ws_clients_.size());
                for (SOCKET s : ws_clients_) {
                    if (wsSendFrame(s, payload)) alive.push_back(s);
                    else closesocket(s);
                }
                ws_clients_ = std::move(alive);
            }
        }
        Sleep(10);
    }
    closesocket(ws_fd_);
    ws_fd_ = INVALID_SOCKET;
}

// ─────────────────────────────────────────────────────────────────────────────
// HTTP handler
// ─────────────────────────────────────────────────────────────────────────────

void OmegaTelemetryServer::run(int port)
{
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);

    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(static_cast<u_short>(port));

    if (bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
        listen(server_fd_, 16) == SOCKET_ERROR) {
        std::cerr << "[OmegaHTTP] bind/listen failed\n"; return;
    }

    std::cout << "[OmegaHTTP] port " << port << "\n";

    while (running_.load()) {
        sockaddr_in ca{}; int cl = sizeof(ca);
        SOCKET c = accept(server_fd_, reinterpret_cast<sockaddr*>(&ca), &cl);
        if (c == INVALID_SOCKET) { if (!running_.load()) break; Sleep(5); continue; }

        char buf[2048];
        const int n = recv(c, buf, static_cast<int>(sizeof(buf)) - 1, 0);
        if (n <= 0) { closesocket(c); continue; }
        buf[n] = '\0';

        std::string body, ct = "text/html";
        int status = 200;

        if (strstr(buf, "GET /api/telemetry"))    { ct = "application/json"; body = buildTelemetryJson(snap_); }
        else if (strstr(buf, "GET /api/trades"))  { ct = "application/json"; body = buildTradesJson(); }
        else if (strstr(buf, "GET /chimera_logo.png")) { ct = "image/png"; body = loadFile("chimera_logo.png"); }
        else if (strstr(buf, "GET / ") || strstr(buf, "GET /index.html")) {
            ct = "text/html"; body = loadFile("omega_index.html");
            if (body.empty()) { body = "<h1>Omega GUI not found</h1>"; status = 404; }
        }
        else { status = 404; body = "Not Found"; }

        char hdr[256];
        snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 %d OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
            "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
            status, ct.c_str(), body.size());
        send(c, hdr,        static_cast<int>(strlen(hdr)),     0);
        send(c, body.c_str(), static_cast<int>(body.size()),   0);
        closesocket(c);
    }
    closesocket(server_fd_);
    server_fd_ = INVALID_SOCKET;
}

} // namespace omega
