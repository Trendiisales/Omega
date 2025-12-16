#include "FIXMessageBuilder.hpp"

namespace Omega {

FIXMessageBuilder::FIXMessageBuilder(
        const std::string& s,
        const std::string& t)
    : sender(s), target(t)
{
    reset();
}

void FIXMessageBuilder::reset()
{
    body.str("");
    body.clear();
    type.clear();
}

void FIXMessageBuilder::setType(const std::string& msgType)
{
    type = msgType;
}

void FIXMessageBuilder::add(const std::string& tag, const std::string& value)
{
    body<<tag<<"="<<value<<"\x01";
}

void FIXMessageBuilder::add(const std::string& tag, int value)
{
    body<<tag<<"="<<value<<"\x01";
}

void FIXMessageBuilder::add(const std::string& tag, double value)
{
    body<<tag<<"="<<value<<"\x01";
}

std::string FIXMessageBuilder::checksum(const std::string& msg)
{
    int sum = 0;
    for(unsigned char c : msg) sum += c;
    int cks = sum % 256;
    std::ostringstream oss;
    oss<<std::setw(3)<<std::setfill('0')<<cks;
    return oss.str();
}

std::string FIXMessageBuilder::build(uint64_t seq)
{
    std::ostringstream header;
    std::string bodyStr = body.str();

    header<<"8=FIX.4.4\x01"
          <<"49="<<sender<<"\x01"
          <<"56="<<target<<"\x01"
          <<"34="<<seq<<"\x01"
          <<"52=20250101-00:00:00\x01"
          <<"35="<<type<<"\x01";

    std::string full = header.str() + bodyStr;
    int sum = 0;
    for(unsigned char c: full) sum+=c;
    int cks = sum % 256;

    std::ostringstream final;
    final<<full<<"10="<<std::setw(3)<<std::setfill('0')<<cks<<"\x01";

    return final.str();
}

}
