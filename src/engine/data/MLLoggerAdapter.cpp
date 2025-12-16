#include "MLLoggerAdapter.hpp"

namespace Omega {

MLLoggerAdapter::MLLoggerAdapter() : ok(false) {}

bool MLLoggerAdapter::init(const std::string& path)
{
    ok = logger.open(path);
    return ok;
}

void MLLoggerAdapter::dump(const SymbolThreadState& S)
{
    if(!ok) return;

    logger.write(
        S.tick,
        S.book,
        S.micro,
        S.strategy,
        S.engine
    );
}

}
