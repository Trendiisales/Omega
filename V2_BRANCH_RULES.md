# CHIMERA v2 Branch Rules

## Branch Layout

```
main                  -> v1.5.1_ACTIVE (locked)
v2-control-plane      -> new arbiter intelligence
v2-fix-oms            -> FIX OMS rehydration
v2-ml-offline         -> ML replay + training
v2-latency-risk       -> latency-aware risk
```

---

## Rule 1: Archived Code is Read-Only

`src/archived` is **never** included directly.

Any rehydration must:
1. Copy files into `src/active`
2. Pass `VERIFY_ACTIVE.py`
3. Be minimal

---

## Rule 2: Rehydrate by Capability, Not by Directory

### Example: FIX OMS (v2-fix-oms)

**Allowed rehydration:**
```
fix/FIXOrder.hpp
fix/FIXOrderState.hpp
fix/FIXOrderStateMachine.hpp
fix/FIXOMSStateSync.hpp
fix/FIXOrderRouter.hpp
```

**Forbidden:**
```
fix/FIXMotherEngineLink.hpp
fix/FIXEngineV1.hpp
fix/FIXSupervisor.hpp
```

**Reason:** OMS is execution-local. Engine orchestration is obsolete.

---

## Rule 3: ML Stays Offline

**Allowed rehydration:**
```
logging/UnifiedRecord.hpp
logging/BinaryLog.hpp
engine/MLLogger.hpp
engine/MLLoggerAdapter.hpp
```

**Never rehydrate:**
- Online inference
- Adaptive live weights
- Model loading in hot path

**ML consumes logs, not ticks.**

---

## Rule 4: Latency Risk is Advisory Only

**Allowed:**
```
risk/LatencyTracker.hpp
risk/LatencyRegime.hpp
risk/VenueLatencyProfile.hpp
```

**Forbidden:**
```
risk/LatencyCancelScaler.hpp
risk/LatencyOrderScaler.hpp
```

**Arbiter owns throttling.**

---

## Rule 5: Every v2 Branch Must Pass VERIFY_ACTIVE

This is **non-negotiable**.

Add to every branch:
```bash
python3 tools/VERIFY_ACTIVE.py
```

If it fails, the branch is invalid.

---

## Rehydration Checklist

Before merging any v2 branch:

- [ ] `python3 tools/VERIFY_ACTIVE.py` passes
- [ ] No includes resolve outside `src/active`
- [ ] No unused files in `src/active`
- [ ] Rehydrated files are minimal and justified
- [ ] No legacy engine orchestration code
- [ ] No online ML inference
- [ ] Arbiter remains the sole throttling authority

---

## File Counts (v1.5.1 Baseline)

| Location | .cpp | .hpp/.h |
|----------|------|---------|
| src/active | 2 | 49 |
| src/archived | 130 | 150 |

**Total active: 51 files**
**Total archived: 280 files**
