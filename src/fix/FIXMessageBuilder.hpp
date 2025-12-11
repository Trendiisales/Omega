#pragma once
#include <cstdint>
#include <string>
#include <sstream>
#include <iomanip>

namespace Omega {

class FIXMessageBuilder {
public:
    FIXMessageBuilder(const std::string& senderCompID,
                      const std::string& targetCompID);

    void reset();
    void setType(const std::string& msgType);
    void add(const std::string& tag, const std::string& value);
    void add(const std::string& tag, int value);
    void add(const std::string& tag, double value);

    std::string build(uint64_t seq);

private:
    std::string sender;
    std::string target;

    std::ostringstream body;
    std::string type;

    std::string checksum(const std::string& msg);
};

}
