# AUDIT PROBE SAFETY — hard rule (added 2026-07-12 after a read-only audit fired a live kill)

An audit / selftest / health check is READ-ONLY. It may ONLY issue:
  - HTTP GET / HEAD to endpoints
  - ssh read commands (cat, journalctl, git rev-parse, reg query, netstat, dir, Get-*)
  - file reads

It MUST NEVER, under any framing ("just to verify the 401", "confirm auth enforced"):
  - POST/PUT/DELETE to ANY endpoint, especially mutating ones (/api/kill, /api/session_reset,
    /api/flatten, /api/reset, /api/close, /api/halt) — a probe of a mutating endpoint IS the mutation.
  - restart/stop/start a service, edit any file on a live box, run any write command.

To test that an auth gate REJECTS a tokenless request WITHOUT triggering the action:
  - Check the CONFIG (nginx conf, the endpoint's auth guard in source) statically, OR
  - Probe a NON-mutating tokenless endpoint and confirm 401, OR
  - Inspect the code path — do NOT fire the live action to observe the response code.

History: 2026-07-12 a "complete system audit" (spec'd read-only) POSTed a tokenless /api/kill to the
live chimera box "expecting 401". nginx injects the token on proxied /api/, so it returned 200 and
FIRED — flattened 2 open shadow legs + latched halt. Shadow only, no real money, but a read-only
audit mutated live state. This file is the standing guard: any audit spec that includes a mutating
probe is malformed.
