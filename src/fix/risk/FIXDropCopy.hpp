#pragma once
#include <functional>
#include "../FIXMessage.hpp"

namespace Omega {

class FIXDropCopy {
public:
    FIXDropCopy();

    void setCallback(std::function<void(const FIXMessage&)>);
    void onFIX(const FIXMessage&);

private:
    std::function<void(const FIXMessage&)> cb;
};

}
