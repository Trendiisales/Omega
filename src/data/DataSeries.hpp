#pragma once
#include <deque>
#include <cstdint>
#include <mutex>
#include <stdexcept>

namespace Omega {

template<typename T>
class DataSeries {
public:
    DataSeries() : maxSize(5000) {}
    
    void setMax(size_t n) { 
        std::lock_guard<std::mutex> g(lock);
        maxSize = n; 
    }

    void add(const T& v) {
        std::lock_guard<std::mutex> g(lock);
        series.push_back(v);
        while (series.size() > maxSize) {
            series.pop_front();
        }
    }

    const std::deque<T>& all() const { 
        return series; 
    }
    
    const T& last() const { 
        if (series.empty()) throw std::runtime_error("Empty series");
        return series.back(); 
    }
    
    const T& first() const {
        if (series.empty()) throw std::runtime_error("Empty series");
        return series.front();
    }
    
    const T& at(size_t i) const {
        return series.at(i);
    }
    
    size_t size() const { return series.size(); }
    bool empty() const { return series.empty(); }
    
    void clear() {
        std::lock_guard<std::mutex> g(lock);
        series.clear();
    }

private:
    mutable std::mutex lock;
    size_t maxSize;
    std::deque<T> series;
};

} // namespace Omega
