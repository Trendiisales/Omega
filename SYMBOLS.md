# Omega Symbol Reference
## Single source of truth for all instrument names, IDs, and mappings

Last verified: 2026-04-01  
Broker: BlackBull Markets  
FIX endpoint: `live.blackbull.8077780` (port 4199 SSL)  
cTrader endpoint: `live.ctraderapi.com:5035`  
cTrader Account ID: 44937427  

---

## Primary Symbols (FIX primary, cTrader depth)

| Internal Name | FIX ID | cTrader Name(s)           | Asset Class   | Tick Value | Verified Price (2026-04-01) | Notes |
|--------------|--------|---------------------------|---------------|------------|---------------------------|-------|
| `XAUUSD`     | 41     | `XAUUSD`                  | Gold spot     | $100/pt/lot | ~$4,670/oz               | id=41 HARDCODED. id=2660 = GOLD.F futures, BLOCKED |
| `US500.F`    | 2642   | `US500.F`, `US500`, `SP500`| S&P 500 CFD  | $50/pt/lot  | ~$6,570                  | ✅ Confirmed correct |
| `USTEC.F`    | 2643   | `USTEC.F`, `USTEC`, `NASDAQ`, `TECH100` | Nasdaq 100 CFD | $20/pt/lot | ~$23,900 | ✅ Confirmed correct |
| `DJ30.F`     | 2637   | `DJ30.F`, `DJ30`, `US30`, `DOWJONES` | Dow Jones CFD | $5/pt/lot | ~$46,600 | ✅ Confirmed correct |
| `USOIL.F`    | 2632   | `USOIL.F`, `USOIL`, `WTI`, `CRUDE` | WTI Crude CFD | $1000/pt/lot | ~$101-102/bbl | ⚠️ Price at $101 = geopolitical spike (Iran/Hormuz). Normal range $65-85. Correct instrument |
| `VIX.F`      | 4462   | `VIX.F`, `VIX`, `VOLX`    | VIX index     | $1/pt/lot   | ~$25                     | Read-only for macro context |
| `DX.F`       | 2638   | `DX.F`                    | USD index     | $1/pt/lot   | ~$99.6                   | Read-only for macro context |
| `NGAS.F`     | 2631   | `NGAS.F`, `NGAS`, `NATGAS`| Natural Gas   | $1000/pt/lot | ~$2.87                  | Not actively traded |

---

## Extended Symbols (FIX ID learned dynamically from SecurityList)

| Internal Name | FIX ID   | cTrader Name(s)                      | Asset Class    | Tick Value  | Verified Price (2026-04-01) | Notes |
|--------------|----------|--------------------------------------|----------------|-------------|---------------------------|-------|
| `GER40`      | dynamic  | `GER40`, `GER30`, `DAX`, `DAX40`     | DAX 40 CFD     | €1/pt → ~$1.10/pt/lot | ~$23,000 | ✅ GER40 confirmed correct name |
| `UK100`      | dynamic  | `UK100`, `FTSE`, `FTSE100`           | FTSE 100 CFD   | £1/pt → ~$1.33/pt/lot | ~$10,277 | ✅ Confirmed |
| `ESTX50`     | dynamic  | `ESTX50`, `STOXX50`, `SX5E`, `EUSTX50` | Euro Stoxx 50 CFD | €1/pt → ~$1.10/pt/lot | ~$5,651 | ✅ Confirmed |
| `XAGUSD`     | dynamic  | `XAGUSD`, `SILVER`                   | Silver spot    | $5000/pt/lot | ~$75/oz                  | ✅ Confirmed |
| `EURUSD`     | dynamic  | `EURUSD`, `EUR/USD`                  | FX             | $100,000/pt/lot | ~$1.156              | ✅ Confirmed |
| `BRENT`      | dynamic  | `BRENT`, `UKBRENT`, `BRENT.F`        | Brent Crude CFD | $1000/pt/lot | ~$102-103/bbl          | ⚠️ Same spike as USOIL.F (Iran/Hormuz). Correct instrument |
| `GBPUSD`     | dynamic  | `GBPUSD`, `GBP/USD`                  | FX             | $100,000/pt/lot | ~$1.323              | ✅ Confirmed |
| `AUDUSD`     | dynamic  | `AUDUSD`, `AUD/USD`                  | FX             | $100,000/pt/lot | ~$0.690              | ✅ Confirmed |
| `NZDUSD`     | dynamic  | `NZDUSD`, `NZD/USD`                  | FX             | $100,000/pt/lot | ~$0.574              | ✅ Confirmed |
| `USDJPY`     | dynamic  | `USDJPY`, `USD/JPY`                  | FX (JPY-quoted) | $100,000/rate/lot | ~$158.7           | ✅ Tick value = 100,000/rate (dynamic) |

---

## Removed / Blocked Symbols

| Symbol   | Why Removed |
|----------|-------------|
| `NAS100` (id=110) | **Duplicate of USTEC.F** — cash CFD vs futures CFD, both Nasdaq 100. USTEC.F has lower spread and better liquidity. Removed from FIX subscription 2026-04-01 |
| `GOLD.F` (id=2660) | **Blocked** — BlackBull futures contract priced ~$25 above XAUUSD spot. Any tick from id=2660 is silently dropped. XAUUSD (id=41) is the correct instrument |

---

## Oil Price Note (2026-04-01)

USOIL.F and BRENT both showing ~$101-103. This is **correct** — WTI surged from ~$65 in early 2026 to $100+ due to:
- Iran/Strait of Hormuz closure (March 2026)
- OPEC+ cuts
- Iran tanker attack (March 31 2026)

Both instruments are correctly named. The BrentWTI spread engine is disabled — spread is only ~$1 currently (near historic low) so no edge.

---

## cTrader Name Resolution

The `name_alias` map in `src/main.cpp` translates broker cTrader names → internal names.  
If broker sends `US500` → routes as `US500.F` internally.  
If broker sends `DAX` → routes as `GER40` internally.  

**To verify live:** `curl http://185.167.119.59:7779/api/symbols` (after rebuild)  
**To audit at startup:** look for `[CTRADER-AUDIT]` lines in `C:\Omega\logs\omega_YYYY-MM-DD.log`  

---

## How to Update This File

When a new instrument is added or a broker name is discovered:
1. Update this table  
2. Update `name_alias` in `src/main.cpp` if broker cTrader name differs from internal name  
3. Update `OMEGA_SYMS[]` in `include/OmegaFIX.hpp` if it's a primary FIX symbol  
4. Update `g_ext_syms` in `include/OmegaFIX.hpp` if it's an extended symbol  
5. Update `tick_value_multiplier()` in `src/main.cpp` with correct contract size  
6. Commit with hash in this file  
