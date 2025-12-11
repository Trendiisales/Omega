#include "FIXHeartbeat.hpp"

namespace Omega {

std::unordered_map<int,std::string>
FIXHeartbeat::make(const std::string& sender,const std::string& target,int seq)
{
    return {
        {8,"FIX.4.4"},
        {35,"0"},
        {49,sender},
        {56,target},
        {34,std::to_string(seq)}
    };
}

std::unordered_map<int,std::string>
FIXHeartbeat::testRequest(const std::string& sender,const std::string& target,int seq,
                          const std::string& reqID)
{
    return {
        {8,"FIX.4.4"},
        {35,"1"},
        {49,sender},
        {56,target},
        {34,std::to_string(seq)},
        {112,reqID}
    };
}

}
