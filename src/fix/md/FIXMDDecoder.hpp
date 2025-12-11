#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include "../FIXMessage.hpp"
#include "../../market/Tick.hpp"
#include "../../market/OrderBook.hpp"

namespace Omega {

struct FIXMDEntry {
    int type = -1;     // 269
    double px = 0;     // 270
    double qty = 0;    // 271
    int level = -1;    // MDEntryPositionNo if available
};

class FIXMDDecoder {
public:
    FIXMDDecoder();

    // Snapshot (35=W)
    bool decodeSnapshot(const FIXMessage& msg,
                        std::vector<FIXMDEntry>& entries);

    // Incremental (35=X)
    bool decodeIncremental(const FIXMessage& msg,
                           std::vector<FIXMDEntry>& entries);

    // Top-of-book update (optional)
    bool decodeTop(const FIXMessage& msg, FIXMDEntry& bid, FIXMDEntry& ask);

    // Universal dispatcher
    bool decode(const FIXMessage& msg,
                std::vector<FIXMDEntry>& entries,
                FIXMDEntry& tobBid,
                FIXMDEntry& tobAsk,
                bool& hasTOB,
                bool& isSnapshot,
                bool& isIncremental);

private:
    void parseRepeatingGroup(const FIXMessage& msg,
                             std::vector<FIXMDEntry>& out);
};

}
