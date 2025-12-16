#pragma once
#include <string>
#include <unordered_map>
#include <mutex>

namespace Omega {

class FIXSymbolMap {
public:
    FIXSymbolMap();

    void add(const std::string& fixSym,const std::string& omegaSym);
    std::string resolve(const std::string& fixSym);

private:
    std::unordered_map<std::string,std::string> map;
    std::mutex lock;
};

}
