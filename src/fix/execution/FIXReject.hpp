#pragma once
#include <string>
#include <functional>
#include "../FIXMessage.hpp"

namespace Omega {

struct FIXRejectInfo {
    std::string refID;
    std::string reason;
    int code=0;
};

class FIXReject {
public:
    FIXReject();

    bool parse(const FIXMessage& msg, FIXRejectInfo& out);

    void setCallback(std::function<void(const FIXRejectInfo&)> cb);

private:
    std::function<void(const FIXRejectInfo&)> callback;
};

}
