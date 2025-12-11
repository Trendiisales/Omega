#include "MicroEngineBase.hpp"

namespace Omega {

// Legacy implementation for backward compatibility
MicroEngineBaseLegacy::MicroEngineBaseLegacy() : enabled_(true) {}
MicroEngineBaseLegacy::~MicroEngineBaseLegacy() {}

void MicroEngineBaseLegacy::setSymbol(const std::string& s) {
    sym = s;
}

const std::string& MicroEngineBaseLegacy::symbol() const {
    return sym;
}

} // namespace Omega
