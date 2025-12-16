#include "DataLogger.hpp"

namespace Omega {

DataLogger::DataLogger(){}
DataLogger::~DataLogger(){ close(); }

void DataLogger::setPath(const std::string& p){
    path=p;
}

bool DataLogger::open() {
    std::lock_guard<std::mutex> g(lock);
    if(opened) return true;

    tickW.open(path + "/ticks.csv",
        TickCSV::header());

    bookW.open(path + "/orderbook.csv",
        OrderBookCSV::header());

    microW.open(path + "/micro.csv",
        MicroCSV::header());

    uniW.open(path + "/unified.csv",
        UnifiedRecord::header());

    opened=true;
    return true;
}

void DataLogger::close(){
    std::lock_guard<std::mutex> g(lock);
    if(!opened) return;
    tickW.close();
    bookW.close();
    microW.close();
    uniW.close();
    opened=false;
}

void DataLogger::flush(){
    std::lock_guard<std::mutex> g(lock);
    tickW.flush();
    bookW.flush();
    microW.flush();
    uniW.flush();
}

void DataLogger::writeTick(const Tick& t){
    std::lock_guard<std::mutex> g(lock);
    tickW.writeRow(TickCSV::encode(t));
}

void DataLogger::writeBook(const OrderBook& ob){
    std::lock_guard<std::mutex> g(lock);
    bookW.writeRow(OrderBookCSV::encode(ob));
}

void DataLogger::writeMicro(const MicroStructureState& m){
    std::lock_guard<std::mutex> g(lock);
    microW.writeRow(MicroCSV::encode(m));
}

void DataLogger::writeUnified(const UnifiedRecord& u){
    std::lock_guard<std::mutex> g(lock);
    uniW.writeRow(UnifiedRecord::encode(u));
}

}
