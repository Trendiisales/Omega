#include "FIXMessage.hpp"
#include <sstream>
#include <iomanip>
#include <cstdlib>

namespace Omega {

FIXMessage::FIXMessage(){}

void FIXMessage::clear(){ fields.clear(); }

void FIXMessage::set(int tag,const std::string& v){
    fields[tag]=v;
}
void FIXMessage::setInt(int tag,int v){
    fields[tag]=std::to_string(v);
}

std::string FIXMessage::get(int tag) const {
    auto it=fields.find(tag);
    return it==fields.end() ? "" : it->second;
}
int FIXMessage::getInt(int tag) const {
    auto it=fields.find(tag);
    return it==fields.end() ? 0 : std::atoi(it->second.c_str());
}

std::string FIXMessage::encode() const {
    std::ostringstream o;

    o<<"8=FIX.4.4"<<char(1);
    std::ostringstream body;

    for(auto& kv : fields){
        body<<kv.first<<"="<<kv.second<<char(1);
    }

    std::string b = body.str();
    o<<"9="<<b.size()<<char(1);
    o<<b;
    std::string full = o.str();

    int sum=0;
    for(unsigned char c:full) sum+=c;
    int cks = sum % 256;

    std::ostringstream ck;
    ck<<"10="<<std::setw(3)<<std::setfill('0')<<cks<<char(1);
    return full + ck.str();
}

bool FIXMessage::decode(const std::string& raw)
{
    fields.clear();
    size_t i=0;
    while(i < raw.size()){
        size_t eq = raw.find('=',i);
        size_t soh = raw.find(char(1), eq);
        if(eq==std::string::npos || soh==std::string::npos) break;

        int tag = std::atoi(raw.substr(i,eq-i).c_str());
        std::string val = raw.substr(eq+1,soh-eq-1);

        fields[tag]=val;
        i = soh+1;
    }
    return true;
}

}
