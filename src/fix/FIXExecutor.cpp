#include "FIXExecutor.hpp"
#include <sstream>

namespace Omega {

FIXExecutor::FIXExecutor() : sess(nullptr), orderCounter(1) {}

bool FIXExecutor::init(FIXSession* s, const std::string& account)
{
    sess = s;
    acct = account;
    return true;
}

std::string FIXExecutor::nextID()
{
    std::ostringstream oss;
    oss<<"OMEGA_"<<orderCounter++;
    return oss.str();
}

bool FIXExecutor::sendNewOrder(const std::string& sym,
                               double qty,
                               double px,
                               bool isBuy)
{
    if(!sess) return false;

    std::string clID = nextID();

    std::ostringstream body;
    body<<"35=D|"
        <<"11="<<clID<<"|"
        <<"55="<<sym<<"|"
        <<"54="<<(isBuy?1:2)<<"|"
        <<"38="<<qty<<"|"
        <<"40=2|"
        <<"44="<<px<<"|"
        <<"59=0|"
        <<"21=1|"
        <<"1="<<acct<<"|";

    return sess->sendMessage(body.str());
}

bool FIXExecutor::sendCancel(const std::string& clOrdId)
{
    if(!sess) return false;

    std::ostringstream body;
    body<<"35=F|"
        <<"11="<<nextID()<<"|"
        <<"41="<<clOrdId<<"|";

    return sess->sendMessage(body.str());
}

}
