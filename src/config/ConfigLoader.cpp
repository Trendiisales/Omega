#include "ConfigLoader.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>

namespace Omega {

ConfigLoader::ConfigLoader() {}
ConfigLoader::~ConfigLoader() {}

bool ConfigLoader::load(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return false;

    kv.clear();

    std::string line;
    std::string currentSection;
    
    while (std::getline(f, line)) {
        // Trim whitespace
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        line = line.substr(start);
        
        size_t end = line.find_last_not_of(" \t\r\n");
        if (end != std::string::npos) line = line.substr(0, end + 1);
        
        if (line.empty()) continue;
        if (line[0] == '#') continue;
        if (line[0] == ';') continue;
        
        // Section header [section]
        if (line[0] == '[' && line.back() == ']') {
            currentSection = line.substr(1, line.size() - 2);
            continue;
        }

        auto pos = line.find('=');
        if (pos == std::string::npos) continue;

        std::string key = line.substr(0, pos);
        std::string val = line.substr(pos + 1);

        // Trim key and value
        key.erase(std::remove_if(key.begin(), key.end(), ::isspace), key.end());
        
        // For value, only trim leading/trailing spaces but preserve internal spaces
        size_t valStart = val.find_first_not_of(" \t");
        if (valStart != std::string::npos) {
            size_t valEnd = val.find_last_not_of(" \t");
            val = val.substr(valStart, valEnd - valStart + 1);
        }

        // Prepend section if exists
        if (!currentSection.empty()) {
            key = currentSection + "." + key;
        }

        kv[key] = val;
    }

    return true;
}

bool ConfigLoader::save(const std::string& path) const {
    std::ofstream f(path);
    if (!f.is_open()) return false;
    
    for (const auto& pair : kv) {
        f << pair.first << " = " << pair.second << "\n";
    }
    
    return true;
}

std::string ConfigLoader::getString(const std::string& key,
                                    const std::string& def) const
{
    auto it = kv.find(key);
    return it == kv.end() ? def : it->second;
}

int ConfigLoader::getInt(const std::string& key, int def) const {
    auto it = kv.find(key);
    if (it == kv.end()) return def;
    try {
        return std::stoi(it->second);
    } catch (...) {
        return def;
    }
}

double ConfigLoader::getDouble(const std::string& key, double def) const {
    auto it = kv.find(key);
    if (it == kv.end()) return def;
    try {
        return std::stod(it->second);
    } catch (...) {
        return def;
    }
}

bool ConfigLoader::getBool(const std::string& key, bool def) const {
    auto it = kv.find(key);
    if (it == kv.end()) return def;
    std::string v = it->second;
    std::transform(v.begin(), v.end(), v.begin(), ::tolower);
    return (v == "1" || v == "true" || v == "yes" || v == "on");
}

void ConfigLoader::set(const std::string& key, const std::string& value) {
    kv[key] = value;
}

void ConfigLoader::setInt(const std::string& key, int value) {
    kv[key] = std::to_string(value);
}

void ConfigLoader::setDouble(const std::string& key, double value) {
    kv[key] = std::to_string(value);
}

void ConfigLoader::setBool(const std::string& key, bool value) {
    kv[key] = value ? "true" : "false";
}

bool ConfigLoader::hasKey(const std::string& key) const {
    return kv.find(key) != kv.end();
}

std::vector<std::string> ConfigLoader::keys() const {
    std::vector<std::string> result;
    result.reserve(kv.size());
    for (const auto& pair : kv) {
        result.push_back(pair.first);
    }
    return result;
}

} // namespace Omega
