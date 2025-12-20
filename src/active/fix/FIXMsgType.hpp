#pragma once

namespace Chimera {

enum class FIXMsgType {
    LOGON,
    LOGOUT,
    HEARTBEAT,
    MARKET_DATA_SNAPSHOT,
    MARKET_DATA_INCREMENTAL,
    SECURITY_LIST,
    EXECUTION_REPORT,
    ORDER_CANCEL_REJECT
};

} // namespace Chimera
