#include "FIXLatencyMonitor.hpp"
#include <algorithm>

namespace Omega {

FIXLatencyMonitor::FIXLatencyMonitor(){}

void FIXLatencyMonitor::recordSend(long ts){
    std::lock_guard<std::mutex> g(lock);
    sendQueue.push_back(ts);
    if(sendQueue.size()>5000) sendQueue.pop_front();
}

void FIXLatencyMonitor::recordRecv(long ts){
    std::lock_guard<std::mutex> g(lock);
    if(sendQueue.empty()) return;
    long s = sendQueue.front();
    sendQueue.pop_front();
    samples.push_back(ts - s);
    if(samples.size()>5000) samples.pop_front();
}

double FIXLatencyMonitor::avgLatencyMs(){
    std::lock_guard<std::mutex> g(lock);
    if(samples.empty()) return 0;
    double sum=0;
    for(long x:samples) sum+=x;
    return sum / samples.size();
}

double FIXLatencyMonitor::p95(){
    std::lock_guard<std::mutex> g(lock);
    if(samples.empty()) return 0;
    auto v=samples;
    std::sort(v.begin(),v.end());
    int idx = v.size()*0.95;
    if(idx>=(int)v.size()) idx=v.size()-1;
    return v[idx];
}

double FIXLatencyMonitor::p99(){
    std::lock_guard<std::mutex> g(lock);
    if(samples.empty()) return 0;
    auto v=samples;
    std::sort(v.begin(),v.end());
    int idx = v.size()*0.99;
    if(idx>=(int)v.size()) idx=v.size()-1;
    return v[idx];
}

}
