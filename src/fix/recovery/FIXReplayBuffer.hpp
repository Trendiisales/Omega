#pragma once
#include <string>
#include <deque>
#include <vector>
#include "../FIXMessage.hpp"
#include <mutex>

namespace Omega {

class FIXReplayBuffer {
public:
    FIXReplayBuffer();

    void push(const FIXMessage&);
    bool getRange(int begin,int end,std::vector<FIXMessage>& out);

private:
    std::mutex lock;
    std::deque<FIXMessage> buf;
};

}
