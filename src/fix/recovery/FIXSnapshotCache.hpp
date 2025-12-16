#pragma once
#include <unordered_map>
#include <mutex>
#include "../FIXMessage.hpp"

namespace Omega {

class FIXSnapshotCache {
public:
    FIXSnapshotCache();

    void store(const std::string&,const FIXMessage&);
    bool get(const std::string&,FIXMessage&);

private:
    std::mutex lock;
    std::unordered_map<std::string,FIXMessage> map;
};

}
