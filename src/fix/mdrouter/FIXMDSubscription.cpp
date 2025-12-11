#include "FIXMDSubscription.hpp"
#include <chrono>
#include <sstream>

namespace Omega {

FIXMDSubscription::FIXMDSubscription(FIXSession* s)
: sess(s) {}

std::string FIXMDSubscription::newID(){
    using namespace std::chrono;
    long ms = duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
    std::ostringstream o;
    o<<"SUB"<<ms;
    return o.str();
}

bool FIXMDSubscription::subscribe(const std::string& sym){
    std::lock_guard<std::mutex> g(lock);

    FIXMessage m;
    m.set(35,"V");
    m.set(262,newID());
    m.set(263,"1");
    m.set(264,"1");
    m.set(146,"1");
    m.set(55,sym);

    bool ok = sess->sendMessage(m);
    if(ok) subs.insert(sym);
    return ok;
}

bool FIXMDSubscription::unsubscribe(const std::string& sym){
    std::lock_guard<std::mutex> g(lock);

    FIXMessage m;
    m.set(35,"V");
    m.set(263,"2");
    m.set(55,sym);

    bool ok = sess->sendMessage(m);
    if(ok) subs.erase(sym);
    return ok;
}

std::unordered_set<std::string> FIXMDSubscription::list(){
    std::lock_guard<std::mutex> g(lock);
    return subs;
}

}
