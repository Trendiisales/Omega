#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include "../fix/marketdata/FIXMDOrderBook.hpp"
#include "../fix/marketdata/FIXMDNormalizer.hpp"
#include "../data/UnifiedTick.hpp"

namespace Omega {

class BookPumper {
public:
    BookPumper();
    ~BookPumper();

    void attachBook(FIXMDOrderBook* b);
    void setSymbol(const std::string& s);
    void setCallback(std::function<void(const UnifiedTick&)> cb);

    void start();
    void stop();

private:
    void loop();

private:
    FIXMDOrderBook* book;
    FIXMDNormalizer normalizer;
    std::string symbol;

    std::function<void(const UnifiedTick&)> onTick;

    std::atomic<bool> running;
    std::thread worker;
};

} // namespace Omega
