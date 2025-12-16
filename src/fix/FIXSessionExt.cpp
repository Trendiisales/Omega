#include "FIXSessionExt.hpp"
#include <unordered_map>

namespace Omega {

bool FIXSessionExt::sendResendRequest(int beginSeq, int endSeq) {
    std::unordered_map<int,std::string> f{
        {8,"FIX.4.4"},
        {35,"2"},
        {7,std::to_string(beginSeq)},
        {16,std::to_string(endSeq)}
    };
    return sendMessage(f);
}

bool FIXSessionExt::sendGapFill(int seq, int newSeq) {
    std::unordered_map<int,std::string> f{
        {8,"FIX.4.4"},
        {35,"4"},
        {34,std::to_string(seq)},
        {123,std::to_string(newSeq)},  // NewSeqNo
        {122,"Y"}                      // GapFillFlag
    };
    return sendMessage(f);
}

bool FIXSessionExt::sendTestRequest(const std::string& id) {
    std::unordered_map<int,std::string> f{
        {8,"FIX.4.4"},
        {35,"1"},
        {112,id}
    };
    return sendMessage(f);
}

}
