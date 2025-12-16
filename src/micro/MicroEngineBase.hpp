#pragma once
#include <string>
#include <cstdint>
#include "../data/UnifiedTick.hpp"

namespace Omega {

struct MicroSignal {
    double value = 0.0;
    double confidence = 0.0;
    uint64_t ts = 0;
    
    inline bool isPositive() const { return value > 0; }
    inline bool isNegative() const { return value < 0; }
    inline bool isStrong() const { return confidence > 0.5; }
};

// =============================================================================
// CRTP Base - Zero virtual function overhead
// All method calls resolved at compile-time for HFT speed
// =============================================================================
template<typename Derived>
class MicroEngineBase {
public:
    MicroEngineBase() : enabled_(true) {}
    ~MicroEngineBase() = default;

    // CRTP dispatch - NO virtual overhead, resolved at compile-time
    inline void onTick(const UnifiedTick& t) {
        if (enabled_) {
            static_cast<Derived*>(this)->onTickImpl(t);
        }
    }
    
    inline MicroSignal compute() const {
        return static_cast<const Derived*>(this)->computeImpl();
    }
    
    inline void reset() {
        static_cast<Derived*>(this)->resetImpl();
    }

    inline void setSymbol(const std::string& s) { sym = s; }
    inline const std::string& symbol() const { return sym; }
    
    inline void enable(bool e) { enabled_ = e; }
    inline bool isEnabled() const { return enabled_; }

protected:
    std::string sym;
    bool enabled_ = true;
    
    // Default implementations
    void resetImpl() {}
};

// =============================================================================
// Legacy compatibility - keeps old code working
// =============================================================================
class MicroEngineBaseLegacy {
public:
    MicroEngineBaseLegacy();
    virtual ~MicroEngineBaseLegacy();

    virtual void onTick(const UnifiedTick& t) = 0;
    virtual MicroSignal compute() const = 0;
    virtual void reset() {}

    void setSymbol(const std::string& s);
    const std::string& symbol() const;
    
    void enable(bool e) { enabled_ = e; }
    bool isEnabled() const { return enabled_; }

protected:
    std::string sym;
    bool enabled_ = true;
};

} // namespace Omega
