#pragma once
#include <vector>
#include <string>
#include <functional>
#include "FIXTransport.hpp"

namespace Omega {

class FIXTransportMux : public FIXTransport {
public:
    FIXTransportMux();
    ~FIXTransportMux();

    void add(FIXTransport* t);

    bool connect(const std::string&, int) override { return false; }
    void disconnect() override {}
    bool sendRaw(const std::string& msg) override;

private:
    std::vector<FIXTransport*> list;
};

} // namespace Omega
