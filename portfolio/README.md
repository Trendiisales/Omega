# Omega portfolio allocator

Blends per-engine trade streams into one vol-targeted, correlation-capped book.

## Run
```
python3 allocator.py            # uses book.json
python3 allocator.py mybook.json
```
Outputs per-engine Sharpe, the correlation matrix, auto-detected correlated
clusters (each cluster = ~1 bet, down-weighted), the combined Sharpe (full +
overlap), and writes `book_result.json` (weights + equity curve).

## Adding an engine
1. Run its backtest harness with `PORT_DUMP=streams/<Name>_trades.txt` set
   (each harness emits `epoch_seconds,pnl` per closed trade when PORT_DUMP is set).
2. Add `"<Name>"` to `book` in book.json.
3. Re-run. The allocator measures its correlation and down-weights it if it
   duplicates an existing bet.

## Honest caveats (read before sizing)
- The gold engines are mutually correlated (~one bet, riding the 2024-26 gold
  bull). The allocator caps them, but the book still leans long-gold.
- The non-gold half (fx_xrev_eurgbp + the 3 index engines) is the durable,
  gold-independent core (Sharpe ~1.1 over 7 years).
- Streams have different history lengths -> trust the OVERLAP Sharpe over the
  full-span number.
- Some single-engine Sharpes are regime-inflated (gold bull). Re-validate on a
  bear/chop tape before live sizing. See ../CLAUDE.md "never deploy without backtest".
