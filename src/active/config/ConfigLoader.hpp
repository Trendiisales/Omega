#pragma once

#include <string>

namespace Chimera {

class ConfigLoader {
public:
    ConfigLoader() = default;
    ~ConfigLoader() = default;

    bool load(const std::string&) { return true; }

    int getInt(const std::string&, int def) const { return def; }
    bool getBool(const std::string&, bool def) const { return def; }

    std::string getString(const std::string&, const std::string& def) const {
        return def;
    }
};

} // namespace Chimera
