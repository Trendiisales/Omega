#pragma once

#include <string>
#include <iostream>

namespace Chimera {

class ChimeraHttpServer {
public:
    ChimeraHttpServer() = default;
    ~ChimeraHttpServer() = default;

    bool start(int port, const std::string&) {
        std::cout << "[HTTP] started on port " << port << std::endl;
        return true;
    }

    void stop() {
        std::cout << "[HTTP] stopped" << std::endl;
    }
};

} // namespace Chimera
