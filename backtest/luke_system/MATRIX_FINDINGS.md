# Luke System — symbols × timeframes × entries × exits (faithful daily BT, 2026-06-25)

## TIMEFRAME — DAILY ONLY (it's a swing edge)
- Daily control (crypto A+C): PF1.37 +703% (reproduces exactly).
- Crypto 4h: ALL configs PF0.81-0.96, every return negative. No exit variant (wide ATR-trail, longer hold, time-stop) rescues it.
- Crypto 1h: PF0.83-0.91, -100%.
- => intraday = cost+noise wall (consistent with intraday-spot-cfd-cost-wall). Trade DAILY.

## SYMBOLS — governed by ADR + dispersion
- WORKS (high-ADR, multi-name, daily): US high-ADR stocks PF2.43-2.63; crypto PF1.61 (C alone +1108%).
- DEAD: indices NAS100/US500 PF0.42-0.80 (ADR ~1%, no single-name dispersion); single low-ADR instruments; all intraday.
- Tradeable (ADR>=4%): stocks 13 (MSTR COIN SMCI MU ARM MRVL PLTR AMD SHOP ORCL SNOW DELL INTC); crypto 19 (TIA INJ NEAR OP APT ARB SUI SEI DOT ADA ATOM AVAX DOGE LINK SOL ETH XRP LTC BNB).

## ENTRIES — asset-specific (video oversimplified; not universal)
- C (inside-day / micro-VCP breakout) = UNIVERSAL workhorse: stocks +77.5% PF2.43, crypto +1108% PF1.61.
- A (pullback-to-rising-9/21-EMA) = STOCKS-ONLY: stocks PF2.63 (robustness add); crypto DEAD (PF1.27, CY2022 0.05 — crypto dips keep dipping).
- B (anchored-VWAP cluster) = CRYPTO-ONLY: crypto PF1.68 (bull1.81/bear1.56); stocks DUD PF0.94.
- Recipe: STOCKS = A+C ; CRYPTO = C (+ optional B).

## EXITS — asset-specific
- STOCKS: structured — partials@3xADR + trail-to-9EMA + optional 20-bar time-stop = PF2.50 +84% DD16.6%. 
- CRYPTO: RIDE WIDE — exit only on first daily close<9EMA, minimal trimming = PF1.75 +3414% (vs +1108% trimmed). Partials/tight-trail CUT the fat tails. Cost = higher DD (37%) + weaker bear.
- UNIVERSAL: BE ratchet = no-op (don't bother). Tight trailing HURTS (esp crypto).

## NON-NEGOTIABLES
- Regime gate (SPY>200MA stocks / BTC>200MA crypto) MANDATORY — ungated dies in sustained bear (CY2022 PF0.12-0.48).
- SHADOW only — survivorship (30 names) + daily-validated (not 5m), no clean 2022 stock data.

## LIVE-SCANNER RECONFIRM (25-06-2026) — does the edge survive the real universe selection?
The live engine doesn't trade a fixed roster — it scans IBKR TOP_PERC_GAIN (top % gainers, >=$20B, >=$10).
Reconstructed that in the BT: a name is eligible only if it was a top-K daily gainer (>=gate%) within the
last L trading days (rolling watchlist) AND clears ADR>=4% (the new pre-filter). Then the Luke setup fires
within the watchlist. luke_bt.py: use_scanner/scanner_gate_pct/scanner_lookback/scanner_topk.
RESULT (stocks A+C, SPY gate, 0.75% risk): fixed-roster PF2.74 +33.9% DD16.6% -> SCANNER(gate4 K8 L15)
PF2.94 +36.8% DD16.5%. Ungated both-regime: fixed PF2.52/bear2.79 -> SCANNER PF2.80/bear2.93.
=> the edge HOLDS and slightly IMPROVES under realistic live selection (the watchlist drops a few
low-quality entries on names not recently in play). Scanner+setup overlap heavily (high-ADR names dominate
TOP_PERC_GAIN organically). Caveat: only 30 names locally (yfinance throttled the expand); a 200+-name
reconstruction is owed for deploy-grade.

## C++ ADR PRE-FILTER (src/bigcap_momo_ibkr.cpp, behind luke_gate, mac-canary green)
TOP_PERC_GAIN has no ADR scan field -> added a one-shot 30-day daily req per scanned name -> 20-day ADR%
-> Sym.adr_ok gate on entry (drops ADR<luke_adr_min). Mirrors the MKT_REQ daily pattern; rids at ADR_BASE
(800000+). Default OFF (luke_gate=false). MSVC-verify pending (TWS TU, not Mac-compilable).

## DEPLOY-GRADE BT — REAL IBKR top-111 universe (5Y ADJUSTED_LAST, real 2022 bear) — 25-06-2026
Pulled the live scanner universe straight from IBKR (tools/ibkr_*.py, read-only): 188 scan-union
-> 111 stocks, 106 start 2021 (full 2022 bear), 34 clear ADR>=4%. THE HONEST NUMBER (vs the curated
30-name PF2.6 which was survivorship/selection inflation):
- A+C ADR>=4 scanner+regime-gated: PF1.44 +104% DD17.9%.
- A+C **ADR>=6** stopw<=5% scanner+regime-gated = REAL CHAMPION: **PF1.89 +45.9% DD12.6%** (only the
  ~14 genuinely high-ADR names: ASTS/BE/IONQ/RKLB/NBIS/CRWV/ALAB/LITE/COHR/CLS/MRNA/FN/MSTR/CIEN).
- LAW holds harder: tighter ADR floor = higher PF (1.25 raw -> 1.44 @adr4 -> 1.89 @adr6). Trade ONLY
  the genuinely volatile names.
- CY2022 0.15-0.67 = long core LOSES in real bear (survivorship makes it look less-bad than reality);
  regime gate sits it out (CY22 ~0 trades gated) = MANDATORY confirmed on real bear.
- Scanner watchlist + ADR pre-filter is what makes it work (PF1.25->1.44->1.89).
PROVENANCE: faithful daily, real adjusted IBKR data, BUT survivorship (today's scanner survivors) ->
2022 leg optimistic. Forward-logger (tools/ibkr_universe_logger.py) fixes it going forward. SHADOW.
