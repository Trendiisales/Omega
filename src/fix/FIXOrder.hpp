#pragma once
#include <string>
#include <unordered_map>

namespace Omega {

class FIXOrder {
public:
    static std::unordered_map<int,std::string> newOrder(
        const std::string& sender,
        const std::string& target,
        int seq,
        const std::string& clOrdId,
        const std::string& symbol,
        double qty,
        double px,
        char side,
        char type
    );

    static std::unordered_map<int,std::string> cancel(
        const std::string& sender,
        const std::string& target,
        int seq,
        const std::string& origClOrdID,
        const std::string& clOrdId,
        const std::string& symbol,
        char side
    );
};

}
