#pragma once
#include <string>
#include <unordered_map>
#include <vector>

namespace Omega {

class ConfigLoader {
public:
    ConfigLoader();
    ~ConfigLoader();

    bool load(const std::string& path);
    bool save(const std::string& path) const;

    std::string getString(const std::string& key,
                          const std::string& def = "") const;

    int getInt(const std::string& key,
               int def = 0) const;

    double getDouble(const std::string& key,
                     double def = 0.0) const;

    bool getBool(const std::string& key,
                 bool def = false) const;
    
    void set(const std::string& key, const std::string& value);
    void setInt(const std::string& key, int value);
    void setDouble(const std::string& key, double value);
    void setBool(const std::string& key, bool value);
    
    bool hasKey(const std::string& key) const;
    std::vector<std::string> keys() const;

private:
    std::unordered_map<std::string, std::string> kv;
};

} // namespace Omega
