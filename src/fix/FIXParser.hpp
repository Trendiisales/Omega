#pragma once
#include <string>
#include <unordered_map>
#include <vector>

namespace Omega {

class FIXParser {
public:
    static bool parse(const std::string& raw,
                      std::unordered_map<int,std::string>& fields);

    static std::string get(const std::unordered_map<int,std::string>& m, int tag);
    static int getInt(const std::unordered_map<int,std::string>& m, int tag);
    static double getDouble(const std::unordered_map<int,std::string>& m, int tag);
};

}
