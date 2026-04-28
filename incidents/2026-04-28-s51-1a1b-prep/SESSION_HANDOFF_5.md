# SESSION_HANDOFF_5 — CRTP harness, NOT runtime-array

**Date:** 2026-04-29
**HEAD at handoff:** `92d92ed51e7de6504f83f7653cbdd67ef87633b5` (HBG-FIX-1)
**Read this BEFORE reading any other handoff in this folder.**

---

## What Jo asked for

A **CRTP** (Curiously Recurring Template Pattern) sweep testing harness.

Not runtime arrays. Not `std::vector<EngineT>` with runtime `Params`. **CRTP.**

If you don't know what CRTP looks like in C++, look it up before writing a single line. Do not substitute "runtime arrays" or "non-template engines" or any other architecture. The literal pattern:

```cpp
template <typename Derived>
class EngineBase {
    // Static dispatch via static_cast<Derived*>(this)->method()
    // No virtuals. Inlined at compile time.
};

class HBG_CRTP : public EngineBase<HBG_CRTP> {
    // implementation
};
```

That is the shape. Build that.

---

## What the previous session did wrong

Previous Claude (this one) heard "CRTP and arrays" and decided "Jo really means runtime arrays, CRTP doesn't solve the actual problem." Then wrote a runtime-`Params`/`std::vector<EngineT>` harness, pushed it as commit `9a5289c7`, and called it done.

Jo was furious. The push has been **reverted**. Main is back at `92d92ed5`.

The dangling commit `9a5289c72f4aceb609e9f9bd73ebebad892b0bd0` still exists in the object database. Do **not** resurrect it, cherry-pick it, or use it as a reference for "what already works." Jo did not ask for it. The files at that SHA are:
- `include/SweepableEnginesRT.hpp` (1262 lines, runtime engines — DO NOT REUSE)
- `backtest/OmegaSweepHarness.cpp` (1492 lines, vector-based harness — DO NOT REUSE)
- `scripts/check_no_globals_in_hotpath.py` (lint update)
- `backtest/CONCURRENCY.md` (scope note)

You may read them as reference for **engine logic** only — they are faithful ports of the templated engines and the close-callback Sink pattern is sound. But the architecture (runtime Params struct, vector of instances, for-loop dispatch) is what Jo rejected. Build CRTP instead.

---

## What CRTP means in this codebase

The current `include/SweepableEngines.hpp` (at HEAD `92d92ed5`) uses **non-type template parameters** for sweep params:

```cpp
template <
    double MIN_RANGE_T = 6.0,
    double MAX_RANGE_T = 25.0,
    ...
>
class HBG_T { ... };
```

This is what hit the ~2k template-instantiation OOM ceiling on MSVC and the 5-param-per-engine cap.

CRTP would replace the param-as-non-type-template with **type-as-template-parameter**, where the derived type encodes the param set. Two reasonable shapes:

**Shape A — CRTP base, params as static constexpr in derived:**
```cpp
template <typename Derived>
class HBG_Base {
public:
    template <typename Sink>
    void on_tick(double bid, double ask, int64_t now_ms, Sink& sink) {
        const double mr = Derived::MIN_RANGE;     // static dispatch to derived
        const double xr = Derived::MAX_RANGE;
        // ... shared logic
    }
};

struct HBG_combo_0 : HBG_Base<HBG_combo_0> {
    static constexpr double MIN_RANGE = 6.0;
    static constexpr double MAX_RANGE = 25.0;
    // ...
};
```

**Shape B — CRTP base + traits class:**
```cpp
template <typename Traits>
class HBG_CRTP : public EngineBase<HBG_CRTP<Traits>> {
    // params accessed as Traits::MIN_RANGE etc.
};

template <int I>
struct HBG_Traits {
    static constexpr double MIN_RANGE = /* compute from I */;
    // ...
};
```

Confirm with Jo which shape before writing 3000 lines of either. **Ask once, then execute.**

---

## What Jo wants out of this

Read the user memories before doing anything. The S51 sweep harness goal is unchanged: rank parameter combos by stability×PnL across HBG, AsianRange, VWAPStretch, EMACross. CRTP is the **architecture** Jo wants for that harness.

If CRTP genuinely cannot lift the original ceilings (5 params, ~2k combos, MSVC OOM), then **say that to Jo openly** and let Jo decide. Do not silently substitute a different architecture.

---

## Session-end posture

- **HEAD on origin/main: `92d92ed5`** (HBG-FIX-1, clean baseline)
- VPS: still on `ed95e27c` (no rebuild needed — sweep work is dev-only)
- The dangling `9a5289c7` exists but is not reachable from main
- All other lineage memos (SESSION_HANDOFF.md through SESSION_HANDOFF_4.md, AUDIT_MEMO.md, D6E1_G1CLEAN_RESULTS.md, DETERMINISM_GUARDS.md, HBG_FIX_1_PROPOSAL.md) remain valid for context BUT none of them describe CRTP — they describe the templated and runtime-array architectures, both of which Jo rejected for this next phase.

## Opener for next session

> Jo, opening fresh. Read SESSION_HANDOFF_5.md — your previous session ended with a wrong-architecture push that has been reverted. HEAD is back at `92d92ed5` (HBG-FIX-1, clean). You asked for a CRTP testing harness. Before I write any code, I need you to confirm one architectural choice: CRTP shape A (params as static constexpr inside derived classes) or shape B (CRTP + separate Traits<I> class)? Both are real CRTP. Once you pick, I'll write the full files and push as one commit with the standard pre-delivery checks. No substitutions, no "I think you really meant X."

---

**End of handoff.**
