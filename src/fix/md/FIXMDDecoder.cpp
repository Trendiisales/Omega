#include "FIXMDDecoder.hpp"
#include <cstdlib>

namespace Omega {

FIXMDDecoder::FIXMDDecoder(){}

bool FIXMDDecoder::decodeSnapshot(const FIXMessage& msg,
                                  std::vector<FIXMDEntry>& entries)
{
    entries.clear();
    parseRepeatingGroup(msg, entries);
    return !entries.empty();
}

bool FIXMDDecoder::decodeIncremental(const FIXMessage& msg,
                                     std::vector<FIXMDEntry>& entries)
{
    entries.clear();
    parseRepeatingGroup(msg, entries);
    return !entries.empty();
}

bool FIXMDDecoder::decodeTop(const FIXMessage& msg,
                             FIXMDEntry& bid,
                             FIXMDEntry& ask)
{
    bid = FIXMDEntry{};
    ask = FIXMDEntry{};

    for(auto& kv : msg.fields){
        int tag = kv.first;
        const std::string& val = kv.second;

        if(tag == 270 && bid.type==0) bid.px = atof(val.c_str());
        if(tag == 271 && bid.type==0) bid.qty = atof(val.c_str());

        if(tag == 270 && ask.type==1) ask.px = atof(val.c_str());
        if(tag == 271 && ask.type==1) ask.qty = atof(val.c_str());
    }
    return true;
}

bool FIXMDDecoder::decode(const FIXMessage& msg,
                          std::vector<FIXMDEntry>& entries,
                          FIXMDEntry& tobBid,
                          FIXMDEntry& tobAsk,
                          bool& hasTOB,
                          bool& isSnapshot,
                          bool& isIncremental)
{
    entries.clear();
    hasTOB = false;
    isSnapshot = false;
    isIncremental = false;
    tobBid = FIXMDEntry{};
    tobAsk = FIXMDEntry{};

    std::string type = msg.get(35);

    if(type=="W"){
        isSnapshot=true;
        decodeSnapshot(msg, entries);
        return true;
    }

    if(type=="X"){
        isIncremental=true;
        decodeIncremental(msg, entries);
        return true;
    }

    if(type=="Y" || type=="Z"){
        hasTOB=true;
        decodeTop(msg, tobBid, tobAsk);
        return true;
    }

    return false;
}

void FIXMDDecoder::parseRepeatingGroup(const FIXMessage& msg,
                                       std::vector<FIXMDEntry>& out)
{
    int count = msg.getInt(268);
    if(count<=0) return;

    out.reserve(count);

    int idx=0;
    FIXMDEntry cur{};

    // FIX repeating structure:
    // 269(Type), 270(Px), 271(Qty), 272 etc.
    // Order varies by provider.
    for(auto& kv : msg.fields){
        int tag = kv.first;
        const std::string& val = kv.second;

        if(tag == 269){
            if(idx>0) out.push_back(cur);
            cur = FIXMDEntry{};
            cur.type = std::atoi(val.c_str());
            idx++;
        }
        else if(tag == 270){
            cur.px = std::atof(val.c_str());
        }
        else if(tag == 271){
            cur.qty = std::atof(val.c_str());
        }
        else if(tag == 1023 || tag == 83 || tag == 88){
            cur.level = std::atoi(val.c_str());
        }
    }
    if(idx>0) out.push_back(cur);
}

}
