#include "FIXSupervisor.hpp"
#include <chrono>

namespace Omega {

FIXSupervisor::FIXSupervisor(FIXSession* s)
: sess(s), running(false) {}

FIXSupervisor::~FIXSupervisor(){ stop(); }

void FIXSupervisor::start(){
    running=true;
    th = std::thread(&FIXSupervisor::loop,this);
}

void FIXSupervisor::stop(){
    running=false;
    if(th.joinable()) th.join();
}

void FIXSupervisor::loop(){
    using namespace std::chrono;
    while(running){
        std::this_thread::sleep_for(seconds(25));
        FIXMessage hb;
        hb.set(35,"0");
        hb.set(49,"SUP");
        hb.set(56,"MD");
        hb.setInt(34,1);
        sess->sendMessage(hb);
    }
}

}
