#pragma once
#include <string>
#include <unordered_set>
#include <mutex>
#include "../FIXSession.hpp"

namespace Omega {

class FIXMDSubscription {
public:
    FIXMDSubscription(FIXSession*);

    bool subscribe(const std::string& symbol);
    bool unsubscribe(const std::string& symbol);

    std::unordered_set<std::string> list();

private:
    FIXSession* sess;
    std::unordered_set<std::string> subs;
    std::mutex lock;

    std::string newID();
};

}
