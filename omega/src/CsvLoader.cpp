// omega/src/CsvLoader.cpp
#include "omega/CsvLoader.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

#if defined(__cpp_lib_filesystem) || (defined(__has_include) && __has_include(<filesystem>))
#include <filesystem>
#define OMEGA_HAS_FILESYSTEM 1
#endif

namespace omega {

namespace {

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& s) {
    std::size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    std::size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::vector<std::string> splitCsv(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    std::stringstream ss(line);
    while (std::getline(ss, cur, ',')) out.push_back(trim(cur));
    return out;
}

std::string stemOf(const std::string& path) {
    std::size_t slash = path.find_last_of("/\\");
    std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
    std::size_t dot = name.find_last_of('.');
    return (dot == std::string::npos) ? name : name.substr(0, dot);
}

} // namespace

Series loadCsv(const std::string& path, const std::string& symbolOverride) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("Cannot open CSV file: " + path);

    Series series;
    series.symbol = symbolOverride.empty() ? stemOf(path) : symbolOverride;

    std::string line;
    if (!std::getline(in, line)) throw std::runtime_error("Empty CSV file: " + path);

    // Map header names to column indices.
    std::vector<std::string> header = splitCsv(line);
    std::map<std::string, int> idx;
    for (int i = 0; i < static_cast<int>(header.size()); ++i) idx[toLower(header[i])] = i;

    auto need = [&](const char* key) -> int {
        auto it = idx.find(key);
        if (it == idx.end())
            throw std::runtime_error(std::string("CSV missing '") + key + "' column: " + path);
        return it->second;
    };

    const int cDate = need("date");
    const int cOpen = need("open");
    const int cHigh = need("high");
    const int cLow  = need("low");
    const int cClose = need("close");
    const int cVol = need("volume");

    int lineNo = 1;
    while (std::getline(in, line)) {
        ++lineNo;
        if (trim(line).empty()) continue;
        std::vector<std::string> f = splitCsv(line);
        int maxNeeded = std::max({cDate, cOpen, cHigh, cLow, cClose, cVol});
        if (static_cast<int>(f.size()) <= maxNeeded)
            throw std::runtime_error("Malformed CSV row " + std::to_string(lineNo) + " in " + path);

        try {
            Bar b;
            b.date   = f[cDate];
            b.open   = std::stod(f[cOpen]);
            b.high   = std::stod(f[cHigh]);
            b.low    = std::stod(f[cLow]);
            b.close  = std::stod(f[cClose]);
            b.volume = static_cast<long long>(std::stod(f[cVol]));
            series.bars.push_back(b);
        } catch (const std::exception&) {
            throw std::runtime_error("Bad numeric value on CSV row " +
                                     std::to_string(lineNo) + " in " + path);
        }
    }
    return series;
}

std::vector<Series> loadCsvDir(const std::string& dir) {
    std::vector<Series> out;
#ifdef OMEGA_HAS_FILESYSTEM
    namespace fs = std::filesystem;
    std::vector<std::string> paths;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        std::string p = entry.path().string();
        if (toLower(p).size() >= 4 && toLower(p).substr(p.size() - 4) == ".csv")
            paths.push_back(p);
    }
    std::sort(paths.begin(), paths.end());  // deterministic ordering
    for (const std::string& p : paths) out.push_back(loadCsv(p));
#else
    (void)dir;
    throw std::runtime_error("loadCsvDir requires <filesystem> support");
#endif
    return out;
}

} // namespace omega
