Daily-loop design (GapShortEngineDaily.cpp):
 connect -> managedAccounts() captures account -> reqPnL(account) [live daily PnL]
 ET-gate: after 09:35 run ENTRY once; poll every 30s; at 15:55 COVER all.
 ENTRY: scan -> histData(gap) -> reqMktData(236) locate -> shortable? -> risk.allow_entry -> short+stop.
 MONITOR: pnl() -> risk.on_close(daily) -> if kill -> cancel all + market-flatten + stop entries.
 COVER: reqPositions -> buy-to-cover all shorts + cancel resting stops.
