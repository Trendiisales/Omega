// Loopback test for IbkrDomConsumer.hpp. Connects to 127.0.0.1:9701,
// drains the bridge for N seconds, prints per-symbol stats. Used to
// validate end-to-end Python TCP -> C++ pipe without building full Omega.

#include "IbkrDomConsumer.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

int main(int argc, char** argv) {
    int duration_sec = (argc > 1) ? std::atoi(argv[1]) : 15;

    omega::ibkr::L2Bus         bus;
    omega::ibkr::ConsumerStats stats;
    std::atomic<bool>          stop_flag{false};

    std::thread th([&]{
        omega::ibkr::run_consumer(bus, stats, stop_flag, "127.0.0.1", 9701);
    });

    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(duration_sec);
    while (std::chrono::steady_clock::now() < end) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::printf(
            "t=%lds msgs=%lld errs=%lld reconn=%lld conn=%d  "
            "XAU imb=%.3f bv=%.0f av=%.0f fresh=%d  "
            "XAG imb=%.3f bv=%.0f av=%.0f fresh=%d\n",
            (long)std::chrono::duration_cast<std::chrono::seconds>(
                end - std::chrono::steady_clock::now()).count(),
            (long long)stats.msgs_total.load(),
            (long long)stats.parse_errors.load(),
            (long long)stats.reconnects.load(),
            stats.connected.load() ? 1 : 0,
            bus.xau.imb.load(), bus.xau.bid_vol.load(), bus.xau.ask_vol.load(),
            bus.xau.fresh(now) ? 1 : 0,
            bus.xag.imb.load(), bus.xag.bid_vol.load(), bus.xag.ask_vol.load(),
            bus.xag.fresh(now) ? 1 : 0);
        std::fflush(stdout);
    }
    stop_flag.store(true);
    // give thread a moment to notice
    std::this_thread::sleep_for(std::chrono::seconds(3));
    th.detach();
    return 0;
}
