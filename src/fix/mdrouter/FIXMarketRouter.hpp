#pragma once
#include <functional>
#include <string>
#include <unordered_map>
#include <mutex>
#include "../md/FIXMDDecoder.hpp"
#include "../../market/Tick.hpp"
#include "../../market/OrderBook.hpp"

namespace Omega {

class FIXMarketRouter {
public:
    FIXMarketRouter();

    void setTickCallback(std::function<void(const std::string&,const Tick&)> cb);
    void setBookCallback(std::function<void(const std::string&,const OrderBook&)> cb);

    void update(const std::string& symbol,
                const std::vector<FIXMDEntry>& entries,
                const FIXMDEntry& tobBid,
                const FIXMDEntry& tobAsk,
                bool hasTOB);

private:
    std::function<void(const std::string&,const Tick&)> tcb;
    std::function<void(const std::string&,const OrderBook&)> bcb;

    struct BookWrap {
        OrderBook ob;
    };

    std::unordered_map<std::string,BookWrap> bookMap;
    std::mutex lock;
};

}
