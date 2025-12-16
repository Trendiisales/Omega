#include "FIXReplayBuffer.hpp"
#include <cstdlib>

namespace Omega {

FIXReplayBuffer::FIXReplayBuffer(){}

void FIXReplayBuffer::push(const FIXMessage& m){
    std::lock_guard<std::mutex> g(lock);
    buf.push_back(m);
    if(buf.size()>20000) buf.pop_front();
}

bool FIXReplayBuffer::getRange(int begin,int end,std::vector<FIXMessage>& out){
    std::lock_guard<std::mutex> g(lock);
    out.clear();
    for(auto& m: buf){
        int seq = std::atoi(m.get(34).c_str());
        if(seq>=begin && seq<=end){
            out.push_back(m);
        }
    }
    return !out.empty();
}

}
