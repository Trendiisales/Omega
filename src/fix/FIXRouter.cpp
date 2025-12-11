#include "FIXRouter.hpp"
#include "FIXParser.hpp"

namespace Omega {

void FIXRouter::addHandler(int msgType, std::function<void(const std::unordered_map<int,std::string>&)> h) {
    handlers[msgType] = h;
}

void FIXRouter::route(const std::unordered_map<int,std::string>& msg) {
    auto it = msg.find(35);
    if(it == msg.end()) return;
    int type = 0;
    try { type = std::stoi(it->second); } catch(...) { return; }
    auto h = handlers.find(type);
    if(h != handlers.end()) {
        h->second(msg);
    }
}

}
