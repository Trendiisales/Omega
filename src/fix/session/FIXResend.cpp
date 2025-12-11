#include "FIXResend.hpp"
#include <cstdlib>

namespace Omega {

FIXResend::FIXResend(){}

bool FIXResend::isResendRequest(const FIXMessage& m){
    return m.get(35)=="2"; // 35=2 Resend Request
}

bool FIXResend::buildResendResponse(const FIXMessage& req,
                                    FIXMessage& outGapFill,
                                    FIXMessage& outSeqReset,
                                    int lastSent)
{
    int begin = std::atoi(req.get(7).c_str());
    int end   = std::atoi(req.get(16).c_str());

    if(end==0) end = lastSent;

    outGapFill = buildGapFill(begin,end);
    outSeqReset= buildSeqReset(end+1);
    return true;
}

FIXMessage FIXResend::buildGapFill(int begin,int end){
    FIXMessage m;
    m.set(35,"4");
    m.setInt(34,begin);
    m.setInt(43,1);
    m.set(122,"");
    m.setInt(36,end+1);
    return m;
}

FIXMessage FIXResend::buildSeqReset(int newSeq){
    FIXMessage m;
    m.set(35,"4");
    m.setInt(36,newSeq);
    m.setInt(34,newSeq);
    m.setInt(123,0);
    return m;
}

}
