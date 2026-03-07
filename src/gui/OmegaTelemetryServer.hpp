#pragma once
// ==============================================================================
// OmegaTelemetryServer — HTTP :7779 + WebSocket :7780
// Reads OmegaTelemetrySharedMemory and serves GUI + JSON API.
// ==============================================================================
#include <atomic>
#include <thread>
#include <vector>
#include <mutex>
#include <string>
#include "OmegaTelemetryWriter.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#endif

namespace omega {

class OmegaTelemetryServer
{
public:
    OmegaTelemetryServer();
    ~OmegaTelemetryServer();

    void start(int http_port, int ws_port, OmegaTelemetrySnapshot* snap = nullptr);
    void stop();

private:
    void run(int port);
    void wsBroadcastLoop();

    static std::string wsHandshakeResponse(const std::string& request);
    static std::string wsBuildFrame(const std::string& payload);
    static bool        wsSendFrame(SOCKET s, const std::string& payload);

    std::atomic<bool> running_;
    std::thread       thread_;
    std::thread       ws_thread_;

    SOCKET server_fd_;
    SOCKET ws_fd_;
    int    ws_port_;

    std::vector<SOCKET> ws_clients_;
    std::mutex          ws_mutex_;

    HANDLE                  hMap_;
    OmegaTelemetrySnapshot* snap_;
};

} // namespace omega
