#include "CSVWriter.hpp"

namespace Omega {

CSVWriter::CSVWriter(){}
CSVWriter::~CSVWriter(){ close(); }

bool CSVWriter::open(const std::string& file,const std::string& header){
    std::lock_guard<std::mutex> g(lock);
    f.open(file, std::ios::out | std::ios::trunc);
    if(!f.is_open()) return false;
    f<<header<<"\n";
    opened=true;
    return true;
}

void CSVWriter::writeRow(const std::string& row){
    if(!opened) return;
    std::lock_guard<std::mutex> g(lock);
    f<<row<<"\n";
}

void CSVWriter::flush(){
    std::lock_guard<std::mutex> g(lock);
    if(opened) f.flush();
}

void CSVWriter::close(){
    std::lock_guard<std::mutex> g(lock);
    if(opened) {
        f.close();
        opened=false;
    }
}

}
