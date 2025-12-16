#include "FIXMsgValidator.hpp"

namespace Omega {

FIXMsgValidator::FIXMsgValidator() {
    // Default required tags for all FIX messages
    required = {"35", "34", "49", "56"};
}

FIXMsgValidator::~FIXMsgValidator() {}

bool FIXMsgValidator::has(const std::unordered_map<std::string,std::string>& t,
                          const std::string& key) const
{
    return t.find(key) != t.end();
}

void FIXMsgValidator::setRequiredTags(const std::unordered_set<std::string>& tags) {
    required = tags;
}

void FIXMsgValidator::addRequiredTag(const std::string& tag) {
    required.insert(tag);
}

bool FIXMsgValidator::validateBasic(const std::unordered_map<std::string,std::string>& tags) const {
    if (!has(tags, "35")) return false;
    if (!has(tags, "34")) return false;
    if (!has(tags, "49")) return false;
    if (!has(tags, "56")) return false;
    return true;
}

ValidationResult FIXMsgValidator::validate(const std::unordered_map<std::string,std::string>& tags) const {
    ValidationResult r;
    
    for (const auto& tag : required) {
        if (!has(tags, tag)) {
            r.valid = false;
            r.error = "Missing required tag: " + tag;
            return r;
        }
    }
    
    r.valid = true;
    return r;
}

} // namespace Omega
