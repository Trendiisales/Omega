#pragma once
#include <string>
#include "../FIXMessage.hpp"

namespace Omega {

class FIXResend {
public:
    FIXResend();

    bool isResendRequest(const FIXMessage&);
    bool buildResendResponse(const FIXMessage& req,
                             FIXMessage& outGapFill,
                             FIXMessage& outSeqReset,
                             int lastSent);

private:
    FIXMessage buildGapFill(int begin, int end);
    FIXMessage buildSeqReset(int newSeq);
};

}
