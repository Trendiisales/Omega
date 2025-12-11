#pragma once
#include <string>
#include <unordered_map>
#include <vector>

namespace Omega {

class FIXMessage {
public:
    FIXMessage();
    void clear();

    void set(int tag,const std::string& v);
    void setInt(int tag,int v);

    std::string get(int tag) const;
    int getInt(int tag) const;

    std::string encode() const;
    bool decode(const std::string& raw);

    std::unordered_map<int,std::string> fields;

private:
    static std::string computeBodyLength(const std::string&);
    static std::string computeChecksum(const std::string&);
};

}
