// omega/include/omega/CsvLoader.h
// Minimal, dependency-free CSV reader for OHLCV data.
// Expected header (case-insensitive), extra columns ignored:
//   date,open,high,low,close,volume
#ifndef OMEGA_CSVLOADER_H
#define OMEGA_CSVLOADER_H

#include <string>
#include <vector>
#include "omega/Bar.h"

namespace omega {

// Load a single CSV file into a Series. The symbol is derived from the file
// name stem unless overridden. Throws std::runtime_error on failure.
Series loadCsv(const std::string& path, const std::string& symbolOverride = "");

// Load every *.csv file in a directory into one Series per file.
std::vector<Series> loadCsvDir(const std::string& dir);

} // namespace omega

#endif // OMEGA_CSVLOADER_H
