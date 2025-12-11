#include "FIXOrder.hpp"
#include <sstream>

namespace Omega {

std::unordered_map<int,std::string>
FIXOrder::newOrder(const std::string& sender,const std::string& target,int seq,
                   const std::string& clOrdId,const std::string& sym,
                   double qty,double px,char side,char type)
{
    std::ostringstream oq, op;
    oq << qty;
    op << px;

    return {
        {8,"FIX.4.4"},
        {35,"D"},
        {49,sender},
        {56,target},
        {34,std::to_string(seq)},
        {11,clOrdId},
        {55,sym},
        {54,std::string(1,side)},
        {38,oq.str()},
        {40,std::string(1,type)},
        {44,op.str()}
    };
}

std::unordered_map<int,std::string>
FIXOrder::cancel(const std::string& sender,const std::string& target,int seq,
                 const std::string& origClOrdID,const std::string& clOrdId,
                 const std::string& sym,char side)
{
    return {
        {8,"FIX.4.4"},
        {35,"F"},
        {49,sender},
        {56,target},
        {34,std::to_string(seq)},
        {41,origClOrdID},
        {11,clOrdId},
        {55,sym},
        {54,std::string(1,side)}
    };
}

}
