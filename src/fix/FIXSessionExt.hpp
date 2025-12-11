#pragma once
#include "FIXSession.hpp"

namespace Omega {

// Extensions: resend request, gap fill, test request cycle
class FIXSessionExt : public FIXSession {
public:
    bool sendResendRequest(int beginSeq, int endSeq);
    bool sendGapFill(int seq, int newSeq);

    bool sendTestRequest(const std::string& reqID);
};

}
