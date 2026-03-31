# BlackBull cTrader FIX Constraints
# READ THIS BEFORE CHANGING ANY FIX SUBSCRIPTION TAGS

## Market Data Subscription (35=V)
- 264 (MarketDepth): **ONLY 0 or 1 supported**. Any other value = INVALID_REQUEST + ghost session
- 263 (SubscriptionRequestType): 1=subscribe, 2=unsubscribe only
- 265 (MDUpdateType): 0=full refresh only
- 267 (NoMDEntryTypes): bid(0) and ask(1) only
- 269 (MDEntryType): 0=bid, 1=ask ONLY -- no trades, no open interest, no L2 size

## Ghost Session -- Root Cause
When BlackBull rejects a subscription (bad params), it keeps the server-side
session alive. Omega times out and reconnects but the server still has the old
session. Result: 30s timeout loop until server eventually clears it.

## Fix (84aba86)
Every reconnect now sends unsub+logout BEFORE the real logon to clear any ghost.
This is permanent -- do not remove this logic.

## Orders
- Only NewOrderSingle (35=D) OrdType=1 (Market) confirmed working
- No native bracket/OCO -- TP/SL managed in Omega code only
