#include "FIXLogWriter.hpp"

namespace Omega {

FIXLogWriter::FIXLogWriter(){}
FIXLogWriter::~FIXLogWriter(){ close(); }

bool FIXLogWriter::open(const std::string& p){
    std::lock_guard<std::mutex> g(lock);
    f.open(p,std::ios::out|std::ios::app);
    opened = f.is_open();
    return opened;
}

void FIXLogWriter::close(){
    std::lock_guard<std::mutex> g(lock);
    if(opened){
        f.close();
        opened=false;
    }
}

void FIXLogWriter::write(const std::string& dir,const std::string& raw){
    if(!opened) return;
    std::lock_guard<std::mutex> g(lock);
    f<<dir<<"|"<<raw<<"\n";
}

}
