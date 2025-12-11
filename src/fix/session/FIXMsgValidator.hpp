#pragma once
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace Omega {

struct ValidationResult {
    bool valid = false;
    std::string error;
};

class FIXMsgValidator {
public:
    FIXMsgValidator();
    ~FIXMsgValidator();

    ValidationResult validate(const std::unordered_map<std::string,std::string>& tags) const;
    bool validateBasic(const std::unordered_map<std::string,std::string>& tags) const;
    
    void setRequiredTags(const std::unordered_set<std::string>& tags);
    void addRequiredTag(const std::string& tag);

private:
    bool has(const std::unordered_map<std::string,std::string>& t,
             const std::string& key) const;

private:
    std::unordered_set<std::string> required;
};

} // namespace Omega
