#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <functional>
#include <mutex>
#include <algorithm>

namespace Omega {

class EventBus {
public:
    using Handler = std::function<void()>;

    EventBus();

    int  subscribe(const std::string& topic, Handler h);
    void unsubscribe(const std::string& topic, int id);
    void publish(const std::string& topic);

private:
    struct Entry {
        int id;
        Handler h;
    };

    std::mutex lock;
    std::unordered_map<std::string, std::vector<Entry>> map;
    int nextId = 1;
};

} // namespace Omega
