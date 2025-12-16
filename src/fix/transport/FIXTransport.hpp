#pragma once
#include <string>
#include <functional>
#include <vector>
#include <cstdint>

namespace Omega {

class FIXTransport {
public:
    using RxCallback = std::function<void(const std::string&)>;
    using StateCallback = std::function<void(bool)>;

    FIXTransport();
    virtual ~FIXTransport();

    // lifecycle
    virtual bool connect(const std::string& host, int port) = 0;
    virtual void disconnect() = 0;

    // sending
    virtual bool sendRaw(const std::string& msg) = 0;

    // receiving
    void setRxCallback(RxCallback cb);
    void setStateCallback(StateCallback cb);

protected:
    void emitRx(const std::string& msg);
    void emitState(bool up);

private:
    RxCallback onRx;
    StateCallback onState;
};

} // namespace Omega
