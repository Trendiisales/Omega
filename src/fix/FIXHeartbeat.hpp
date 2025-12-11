#pragma once
#include <string>
#include <unordered_map>

namespace Omega {

struct FIXHeartbeat {
    static std::unordered_map<int,std::string> make(const std::string& sender,
                                                    const std::string& target,
                                                    int seq);

    static std::unordered_map<int,std::string> testRequest(const std::string& sender,
                                                           const std::string& target,
                                                           int seq,
                                                           const std::string& reqID);
};

}
