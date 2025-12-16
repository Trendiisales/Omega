#pragma once
#include <string>
#include <mutex>
#include <vector>
#include <atomic>
#include "../market/Tick.hpp"
#include "../market/OrderBook.hpp"
#include "../micro/MicroStructureState.hpp"
#include "CSVWriter.hpp"
#include "TickCSV.hpp"
#include "OrderBookCSV.hpp"
#include "MicroCSV.hpp"
#include "UnifiedRecord.hpp"

namespace Omega {

class DataLogger {
public:
    DataLogger();
    ~DataLogger();

    void setPath(const std::string& p);
    bool open();

    void writeTick(const Tick&);
    void writeBook(const OrderBook&);
    void writeMicro(const MicroStructureState&);
    void writeUnified(const UnifiedRecord&);

    void flush();
    void close();

private:
    std::string path;
    CSVWriter tickW;
    CSVWriter bookW;
    CSVWriter microW;
    CSVWriter uniW;

    std::mutex lock;
    bool opened=false;
};

}
