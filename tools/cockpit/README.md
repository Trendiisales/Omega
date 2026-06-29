# Omega cockpit (:8099)

Single-pane Mac aggregator. Rebuilt 2026-06-30 after a host "reset" deleted the
original `/Users/jo/omega-cockpit/` + `/Users/jo/omega-supervisor/` with no backup
(was never git-tracked). Now lives HERE, in the tracked repo, so a delete is always
`git checkout`-recoverable.

## Surfaces
- Companion book  -> `~/stall-accountant/companion_state.json` (unified omega+crypto clips)
- Crypto book     -> IBKRCrypto `state.json`  (full GUI :8090)
- RD-Agent        -> `~/Omega/data/rdagent/latest.json`  (full GUI :7799)
- Omega desk      -> VPS GUI :7779

## Run / restore
```sh
# launch agent (KeepAlive + RunAtLoad), runs server.py from this dir on :8099
cp tools/cockpit/com.omega.cockpit.plist ~/Library/LaunchAgents/
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.omega.cockpit.plist
open http://127.0.0.1:8099
```
Manual: `python3 tools/cockpit/server.py` (PORT env overrides 8099).
