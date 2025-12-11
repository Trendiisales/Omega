#include "FIXMessageStore.hpp"

namespace Omega {

void FIXMessageStore::save(int seq, const std::string& raw) {
    std::lock_guard<std::mutex> g(m);
    store[seq] = raw;
}

bool FIXMessageStore::get(int seq, std::string& raw) {
    std::lock_guard<std::mutex> g(m);
    auto it = store.find(seq);
    if(it == store.end()) return false;
    raw = it->second;
    return true;
}

void FIXMessageStore::clearBelow(int seq) {
    std::lock_guard<std::mutex> g(m);
    for(auto it=store.begin(); it!=store.end();) {
        if(it->first < seq) it = store.erase(it);
        else ++it;
    }
}

}
