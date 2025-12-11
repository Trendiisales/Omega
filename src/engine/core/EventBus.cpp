#include "EventBus.hpp"

namespace Omega {

EventBus::EventBus() {}

int EventBus::subscribe(const std::string& topic, Handler h) {
    std::lock_guard<std::mutex> g(lock);
    int id = nextId++;
    map[topic].push_back(Entry{id, h});
    return id;
}

void EventBus::unsubscribe(const std::string& topic, int id) {
    std::lock_guard<std::mutex> g(lock);
    auto it = map.find(topic);
    if (it == map.end()) return;
    auto& v = it->second;
    v.erase(
        std::remove_if(v.begin(), v.end(),
            [id](const Entry& e){ return e.id == id; }),
        v.end()
    );
}

void EventBus::publish(const std::string& topic) {
    std::vector<Handler> local;
    {
        std::lock_guard<std::mutex> g(lock);
        auto it = map.find(topic);
        if (it == map.end()) return;
        for (const auto& e : it->second)
            local.push_back(e.h);
    }
    for (auto& h : local) h();
}

} // namespace Omega
