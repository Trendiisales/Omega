#pragma once
#include <string>
#include "../SymbolThreadState.hpp"
#include "MLLogger.hpp"

namespace Omega {

class MLLoggerAdapter {
public:
    MLLoggerAdapter();
    bool init(const std::string& path);

    // Direct dump from symbol thread
    void dump(const SymbolThreadState& S);

private:
    MLLogger logger;
    bool ok;
};

}
