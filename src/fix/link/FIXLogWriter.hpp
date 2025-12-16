#pragma once
#include <string>
#include <fstream>
#include <mutex>

namespace Omega {

class FIXLogWriter {
public:
    FIXLogWriter();
    ~FIXLogWriter();

    bool open(const std::string& path);
    void close();

    void write(const std::string& dir,const std::string& raw);

private:
    std::ofstream f;
    std::mutex lock;
    bool opened=false;
};

}
