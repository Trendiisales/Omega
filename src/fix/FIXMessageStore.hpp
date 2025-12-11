#pragma once
#include <unordered_map>
#include <string>
#include <mutex>

// Minimal local store for messages required for resend / gap fill

namespace Omega {

class FIXMessageStore {
public:
    void save(int seq, const std::string& raw);
    bool get(int seq, std::string& raw);
    void clearBelow(int seq);

private:
    std::unordered_map<int,std::string> store;
    std::mutex m;
};

}
