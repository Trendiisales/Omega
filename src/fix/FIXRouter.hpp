#pragma once
#include <unordered_map>
#include <functional>
#include <string>

namespace Omega {

class FIXRouter {
public:
    void addHandler(int msgType, std::function<void(const std::unordered_map<int,std::string>&)> h);
    void route(const std::unordered_map<int,std::string>& msg);

private:
    std::unordered_map<int,std::function<void(const std::unordered_map<int,std::string>&)>> handlers;
};

}
