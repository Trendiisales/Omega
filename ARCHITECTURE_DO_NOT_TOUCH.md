# CHIMERA_HFT — DO NOT TOUCH ARCHITECTURE

## Purpose
This document defines components that MUST NOT be redesigned,
stubbed, deleted, or replaced without a full version bump.

## Frozen Data Flow

```
Tick → TickPipelineExt → MicroMetrics
     → RegimeClassifier
     → StrategyFusion
     → ExecutionSupervisor
     → OrderIntent → Execution
```

## Locked Interfaces

The following are **ARCHITECTURAL CONTRACTS**:

| File | API |
|------|-----|
| `pipeline/MicroMetrics.hpp` | POD struct with shockFlag, trendScore, volRatio |
| `pipeline/TickPipelineExt.{hpp,cpp}` | init(), pushTick(), compute() |
| `risk/RegimeClassifier.{hpp,cpp}` | classify(MicroMetrics&) |
| `risk/KillSwitch.{hpp,cpp}` | trigger(), clear(), isTriggered() |
| `supervisor/ExecutionSupervisor.{hpp,cpp}` | approve(), route(), onExecution() |
| `positions/PnLTracker.{hpp,cpp}` | onExec(), realized(), fees() |

If code depends on these, **THEY MUST EXIST**.

## Forbidden Actions

❌ Replacing real logic with stubs  
❌ Deleting headers without removing all consumers  
❌ Changing function signatures without audit  
❌ Introducing alternative versions (v2, _new, _alt)  
❌ "Temporary" implementations  
❌ Changing namespaces (must remain `Chimera::`)

## Required Process for Change

1. Update `src/api/CHIMERA_API_LOCK.hpp`
2. Update `src/api/CHIMERA_API_ASSERTS.hpp`
3. Update this document
4. Bump version
5. Regenerate audit report
6. Full rebuild to verify static_asserts pass

Failure to follow this process **WILL** break the system.

## Historical Note

v1.0 broke because:
- `pipeline/` and `supervisor/` directories were deleted
- Consumers were not updated
- Real logic was replaced with stubs
- API signatures drifted without audit

This document exists to ensure that **never happens again**.

## Version History

| Version | Date | Changes |
|---------|------|---------|
| v1.0 | 2024-12-18 | Initial release (broken) |
| v1.1 | 2024-12-18 | Restored pipeline/supervisor |
| v1.1.1 | 2024-12-18 | Locked API surface |
