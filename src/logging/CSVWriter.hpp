#pragma once
#include <string>
#include <fstream>
#include <vector>
#include <mutex>

namespace Omega {

class CSVWriter {
public:
    CSVWriter();
    ~CSVWriter();

    bool open(const std::string& file,
              const std::string& header);

    void writeRow(const std::string& row);
    void flush();
    void close();

private:
    std::ofstream f;
    std::mutex lock;
    bool opened=false;
};

}
