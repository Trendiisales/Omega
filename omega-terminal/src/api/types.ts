// omega-terminal — API types
//
// These interfaces mirror the JSON shapes returned by the C++
// OmegaApiServer (src/api/OmegaApiServer.{hpp,cpp}, Step 2 engine-side
// work). Field names are byte-identical to the JSON keys — do NOT rename
// fields here without renaming on the engine side at the same time.
//
// Reference: docs/omega_terminal/STEP2_OPENER.md § "Step 2 scope" §B.3.
//
// Layout intent:
//
//   - Engine endpoint           -> Engine[]
//     GET /api/v1/omega/engines
//   - Positions endpoint        -> Position[]
//     GET /api/v1/omega/positions
//   - Ledger endpoint           -> LedgerEntry[]
//     GET /api/v1/omega/ledger?from=<iso>&to=<iso>&engine=<name>&limit=<n>
//   - Equity endpoint           -> EquityPoint[]
//     GET /api/v1/omega/equity?from=<iso>&to=<iso>&interval=<1m|1h|1d>
//
// Step 5 additions (OpenBB-backed market routes; engine forwards via
// src/api/OpenBbProxy):
//   - INTEL screener            -> OpenBbEnvelope<IntelArticle>
//     GET /api/v1/omega/intel?screen=<id>&limit=<n>
//   - CURV yield curves         -> OpenBbEnvelope<CurvPoint>
//     GET /api/v1/omega/curv?region=US|EU|JP
//   - WEI world equity indices  -> OpenBbEnvelope<WeiQuote>
//     GET /api/v1/omega/wei?region=US|EU|ASIA|WORLD|<symbol-list>
//   - MOV movers                -> OpenBbEnvelope<MovRow>
//     GET /api/v1/omega/mov?universe=active|gainers|losers
//
// Step 6 additions (BB function suite, all OpenBB-backed via OpenBbProxy
// except WATCH which is engine-side scheduled):
//   - OMON   options chain      -> OpenBbEnvelope<OptionsRow>
//     GET /api/v1/omega/omon?symbol=<sym>&expiry=<iso>
//   - FA     financial analysis -> FaEnvelope (income/balance/cash merged)
//     GET /api/v1/omega/fa?symbol=<sym>
//   - KEY    key stats          -> KeyEnvelope (key_metrics + multiples)
//     GET /api/v1/omega/key?symbol=<sym>
//   - DVD    dividends          -> OpenBbEnvelope<Dividend>
//     GET /api/v1/omega/dvd?symbol=<sym>
//   - EE     earnings estimates -> EeEnvelope (consensus + surprise)
//     GET /api/v1/omega/ee?symbol=<sym>
//   - NI     news (per-symbol)  -> OpenBbEnvelope<IntelArticle>
//     GET /api/v1/omega/ni?symbol=<sym>&limit=<n>
//   - GP     graph price        -> OpenBbEnvelope<HistoricalBar>
//     GET /api/v1/omega/gp?symbol=<sym>&interval=<1m|1h|1d>
//   - QR     quote recap        -> OpenBbEnvelope<QuoteRow>
//     GET /api/v1/omega/qr?symbols=<list>
//   - HP     historical price   -> OpenBbEnvelope<HistoricalBar>
//     GET /api/v1/omega/hp?symbol=<sym>&interval=<1m|1h|1d>
//     (shares cache slot with /gp on identical (symbol, interval))
//   - DES    description        -> OpenBbEnvelope<CompanyProfile>
//     GET /api/v1/omega/des?symbol=<sym>
//   - FXC    fx cross           -> OpenBbEnvelope<FxQuote>
//     GET /api/v1/omega/fxc?pair=<base>/<quote>|<region>
//   - CRYPTO crypto             -> OpenBbEnvelope<CryptoQuote>
//     GET /api/v1/omega/crypto?symbols=<list>
//   - WATCH  nightly screener   -> WatchEnvelope (engine-cron-driven)
//     GET /api/v1/omega/watch?universe=SP500|NDX|ALL
//
// All timestamps are unix-epoch milliseconds (int64) on the wire,
// represented as `number` in TS. The engine guarantees JS-safe range
// (< 2^53) for all timestamps it produces.
//
// OpenBB note: the Step-5 + Step-6 single-call routes pass the OpenBB
// OBBject envelope through verbatim (no JSON parsing in C++). The UI
// dereferences `.results` to get the row array. The Step-6 multi-call
// merged routes (FA / KEY / EE / WATCH) wrap multiple OpenBB calls into
// one merged envelope with explicitly-named sub-arrays.

/* ============================================================ */
/* Shared primitives                                            */
/* ============================================================ */

/**
 * Engine operating mode. Mirrors the C++ side, where this is derived
 * from `kShadowDefault = (g_cfg.mode != "LIVE")` per STEP2_OPENER §
 * Constraints. Read-only on the wire — flipping mode is not a UI action
 * in step 2.
 */
export type EngineMode = 'LIVE' | 'SHADOW';

/**
 * Engine runtime state. RUNNING = healthy, processing ticks. IDLE =
 * registered but no recent activity (e.g. paused, market closed). ERR =
 * error state — engine has surfaced a fault and stopped processing.
 */
export type EngineState = 'RUNNING' | 'IDLE' | 'ERR';

/** Trade side, as serialized by the engine ledger. */
export type Side = 'LONG' | 'SHORT';

/**
 * Equity time-series interval. Server-side enum keeping aggregation
 * options bounded. If you add a value here, also add it on the C++
 * side at the same time.
 */
export type EquityInterval = '1m' | '1h' | '1d';

/**
 * Historical-bar interval shared by GP and HP. The engine forwards the
 * literal value to OpenBB's `/equity/price/historical?interval=` param.
 * Common values: 1m, 5m, 15m, 30m, 1h, 4h, 1d, 1W, 1M.
 */
export type BarInterval = '1m' | '5m' | '15m' | '30m' | '1h' | '4h' | '1d' | '1W' | '1M';

/* ============================================================ */
/* GET /api/v1/omega/engines                                    */
/* ============================================================ */

/**
 * One engine snapshot. Matches the C++ `EngineSnapshot` struct in
 * `include/globals.hpp` (Step 2 addition).
 *
 *   name             unique engine identifier (e.g. "HybridGold")
 *   enabled          whether the engine is currently enabled
 *   mode             "LIVE" or "SHADOW"
 *   state            "RUNNING" | "IDLE" | "ERR"
 *   last_signal_ts   unix ms of last signal emitted; 0 = never
 *   last_pnl         most recent realized pnl contribution
 */
export interface Engine {
  name: string;
  enabled: boolean;
  mode: EngineMode;
  state: EngineState;
  last_signal_ts: number;
  last_pnl: number;
}

/* ============================================================ */
/* GET /api/v1/omega/positions                                  */
/* ============================================================ */

/**
 * One open position row. Matches the JSON shape produced by the
 * positions route on the C++ side.
 *
 *   symbol           e.g. "EURUSD" / "XAUUSD" / "BTCUSD"
 *   side             "LONG" | "SHORT"
 *   size             contract / lot size (engine-native units)
 *   entry            entry price
 *   current          current mid (or last) price
 *   unrealized_pnl   unrealized P&L in account currency
 *   mfe              max favorable excursion observed since entry
 *   mae              max adverse excursion observed since entry
 *   engine           name of the engine that opened the position
 */
export interface Position {
  symbol: string;
  side: Side;
  size: number;
  entry: number;
  current: number;
  unrealized_pnl: number;
  mfe: number;
  mae: number;
  engine: string;
}

/* ============================================================ */
/* GET /api/v1/omega/ledger                                     */
/* ============================================================ */

/**
 * One closed-trade ledger entry.
 *
 *   id               server-issued trade id (stable across reloads)
 *   engine           originating engine name
 *   symbol           instrument symbol
 *   side             "LONG" | "SHORT"
 *   entry_ts         unix ms entry timestamp
 *   exit_ts          unix ms exit timestamp
 *   entry            entry price
 *   exit             exit price
 *   pnl              realized P&L in account currency
 *   reason           free-form exit reason (e.g. "TP", "SL", "TRAIL")
 */
export interface LedgerEntry {
  id: string;
  engine: string;
  symbol: string;
  side: Side;
  entry_ts: number;
  exit_ts: number;
  entry: number;
  exit: number;
  pnl: number;
  reason: string;
}

/**
 * Query parameters for the ledger endpoint. All fields are optional;
 * omitting them returns the most recent `limit` trades across all
 * engines and symbols.
 */
export interface LedgerQuery {
  /** ISO-8601 lower bound (inclusive) on `entry_ts`. */
  from?: string;
  /** ISO-8601 upper bound (exclusive) on `entry_ts`. */
  to?: string;
  /** Engine name filter (exact match). */
  engine?: string;
  /** Hard limit on returned rows. Server clamps to a sane max. */
  limit?: number;
}

/* ============================================================ */
/* GET /api/v1/omega/equity                                     */
/* ============================================================ */

/**
 * One equity time-series point.
 *
 *   ts        unix ms timestamp at the close of the interval bucket
 *   equity    account equity value at `ts`, in account currency
 */
export interface EquityPoint {
  ts: number;
  equity: number;
}

/**
 * Query parameters for the equity endpoint.
 */
export interface EquityQuery {
  /** ISO-8601 lower bound (inclusive). */
  from?: string;
  /** ISO-8601 upper bound (exclusive). */
  to?: string;
  /** Bucket size for aggregation. */
  interval?: EquityInterval;
}

/* ============================================================ */
/* OpenBB envelope (Step 5)                                     */
/* ============================================================ */

/**
 * Generic OpenBB OBBject wrapper. Every OpenBB Hub response — and
 * every Step-5 mock response from OpenBbProxy — comes back in this
 * shape:
 *
 *   {
 *     "results":  [...],
 *     "provider": "<provider-id>",
 *     "warnings": null | [{ "message": string, ... }],
 *     "chart":    null,
 *     "extra":    { ... }
 *   }
 *
 * The Step-5 panels read `.results` and surface `provider` / `warnings`
 * in a corner badge. `chart` is currently unused on the UI side and
 * `extra` holds OpenBB-internal diagnostics (and `{mock:true}` when the
 * proxy is in mock mode).
 */
export interface OpenBbEnvelope<T> {
  results: T[];
  provider?: string;
  warnings?: Array<{ message: string; [k: string]: unknown }> | null;
  chart?: unknown;
  extra?: Record<string, unknown>;
}

/* ============================================================ */
/* GET /api/v1/omega/intel                                      */
/* ============================================================ */

/**
 * One news / intel article. Matches the OpenBB `/news/world` row shape
 * on the most common providers (benzinga, biztoc, fmp, intrinio,
 * tiingo). Field availability varies by provider; only `title` and
 * `date` are guaranteed.
 */
export interface IntelArticle {
  title: string;
  /** ISO-8601 string from OpenBB. May or may not include a Z suffix. */
  date: string;
  text?: string;
  url?: string;
  source?: string;
  symbols?: string[];
  images?: Array<{ url: string }>;
  [k: string]: unknown;
}

/**
 * Query parameters for the intel endpoint. The screen-id parameter is
 * accepted by the engine route but currently maps every value to the
 * default OpenBB news call -- Step 6 will branch into sector/macro
 * screens based on the id.
 */
export interface IntelQuery {
  /** Screener identifier. Default "TOP" (OpenBB /news/world). */
  screen?: string;
  /** Number of articles to request. Server clamps to [1, 50]. */
  limit?: number;
}

/* ============================================================ */
/* GET /api/v1/omega/curv                                       */
/* ============================================================ */

/**
 * One yield-curve point. OpenBB
 * `/fixedincome/government/treasury_rates` returns one row per (date,
 * maturity) pair when `provider=federal_reserve`. The Step-5 CurvPanel
 * uses the most recent date and groups by maturity to draw the curve.
 *
 * Maturity strings on the federal_reserve provider use the shape
 * "month_<n>" or "year_<n>" -- e.g. "month_3", "year_10". The CurvPanel
 * has a small parser that converts these into years for the X axis.
 */
export interface CurvPoint {
  date: string;
  maturity: string;
  rate: number;
  [k: string]: unknown;
}

/** Query parameters for the curv endpoint. */
export interface CurvQuery {
  /**
   * Curve region. "US" (default) is fully wired via the federal_reserve
   * OpenBB provider. "EU" and "JP" return a 200 response with an empty
   * `results` array and a warning -- Step 6 wires those providers.
   */
  region?: 'US' | 'EU' | 'JP' | string;
}

/* ============================================================ */
/* GET /api/v1/omega/wei                                        */
/* ============================================================ */

/**
 * One world-equity-index quote row. Shape matches OpenBB
 * `/equity/price/quote` on the yfinance provider. Some fields may be
 * missing on other providers; the WeiPanel guards every cell.
 */
export interface WeiQuote {
  symbol: string;
  name?: string;
  last_price?: number;
  change?: number;
  change_percent?: number;
  volume?: number;
  prev_close?: number;
  open?: number;
  high?: number;
  low?: number;
  [k: string]: unknown;
}

/** Query parameters for the wei endpoint. */
export interface WeiQuery {
  /**
   * Region preset. Recognised values:
   *   "US"    SPY,QQQ,DIA,IWM,VTI
   *   "EU"    VGK,EZU,FEZ,EWG,EWU
   *   "ASIA"  EWJ,FXI,EWY,EWT,EWA
   *   "WORLD" VT,ACWI,VEA,VWO,EFA
   *
   * Anything else is forwarded as a literal comma-separated symbol list,
   * so power users can type `WEI AAPL,MSFT,GOOGL` from the command bar.
   */
  region?: string;
  /** Override the OpenBB provider (default "yfinance"). */
  provider?: string;
}

/* ============================================================ */
/* GET /api/v1/omega/mov                                        */
/* ============================================================ */

/**
 * One movers row. Shape matches OpenBB `/equity/discovery/<universe>`
 * on the yfinance provider. The `percent_change` field name varies by
 * provider; we include both `percent_change` and `change_percent` for
 * resilience.
 */
export interface MovRow {
  symbol: string;
  name?: string;
  price?: number;
  change?: number;
  percent_change?: number;
  change_percent?: number;
  volume?: number;
  [k: string]: unknown;
}

/** Query parameters for the mov endpoint. */
export interface MovQuery {
  /** "active" (default) | "gainers" | "losers". Anything else -> "active". */
  universe?: 'active' | 'gainers' | 'losers' | string;
  /** Override the OpenBB provider (default "yfinance"). */
  provider?: string;
}

/* ============================================================ */
/* GET /api/v1/omega/omon  (Step 6 - options chain)             */
/* ============================================================ */

/**
 * One option-chain row. Shape mirrors OpenBB
 * `/derivatives/options/chains?symbol=<sym>`. Fields vary across
 * providers (cboe, intrinio, tradier); we model the common subset.
 *
 * The OMON panel computes its summary header (ATM IV, term structure,
 * put/call ratio) client-side from the array because the engine-side
 * "no third-party JSON libs" stance precludes parsing this in C++.
 */
export interface OptionsRow {
  /** Underlying symbol, e.g. "AAPL". */
  underlying_symbol?: string;
  /** Per-contract symbol, e.g. "AAPL250620C00200000". */
  contract_symbol?: string;
  /** Expiry date (ISO). Some providers use `expiration`. */
  expiration?: string;
  /** Strike price. */
  strike?: number;
  /** "call" | "put". Some providers use uppercase. */
  option_type?: string;
  bid?: number;
  ask?: number;
  /** Mid / last traded price; provider-dependent. */
  last_trade_price?: number;
  /** Implied volatility, decimal (e.g. 0.32 for 32 %). */
  implied_volatility?: number;
  open_interest?: number;
  volume?: number;
  delta?: number;
  gamma?: number;
  theta?: number;
  vega?: number;
  [k: string]: unknown;
}

/** Query parameters for the omon endpoint. */
export interface OmonQuery {
  /** Underlying symbol. Required. */
  symbol: string;
  /**
   * Optional expiry filter (ISO date). When omitted, the engine returns
   * the full chain across expiries; the panel filters client-side.
   */
  expiry?: string;
}

/* ============================================================ */
/* GET /api/v1/omega/fa  (Step 6 - financial analysis)          */
/* ============================================================ */

/**
 * One row of an income statement. Field set is provider-dependent
 * (fmp / polygon / intrinio); the FaPanel renders whichever fields are
 * non-null per row.
 */
export interface IncomeStatement {
  period_ending?: string;
  fiscal_period?: string;
  revenue?: number;
  cost_of_revenue?: number;
  gross_profit?: number;
  operating_income?: number;
  net_income?: number;
  ebitda?: number;
  eps_basic?: number;
  eps_diluted?: number;
  [k: string]: unknown;
}

/** One row of a balance sheet. */
export interface BalanceSheet {
  period_ending?: string;
  fiscal_period?: string;
  total_assets?: number;
  total_current_assets?: number;
  total_liabilities?: number;
  total_current_liabilities?: number;
  total_equity?: number;
  cash_and_short_term_investments?: number;
  long_term_debt?: number;
  short_term_debt?: number;
  [k: string]: unknown;
}

/** One row of a cash-flow statement. */
export interface CashFlow {
  period_ending?: string;
  fiscal_period?: string;
  cash_from_operating_activities?: number;
  cash_from_investing_activities?: number;
  cash_from_financing_activities?: number;
  capital_expenditure?: number;
  free_cash_flow?: number;
  [k: string]: unknown;
}

/**
 * Merged envelope produced by the engine FA route. Three OpenBB calls
 * (`/equity/fundamental/{income,balance,cash}`) are stitched into one
 * top-level object so the UI gets one round-trip per panel mount.
 */
export interface FaEnvelope {
  income: IncomeStatement[];
  balance: BalanceSheet[];
  cash: CashFlow[];
  /** Provider id of the underlying OpenBB calls (typically the same for all three). */
  provider?: string;
  warnings?: Array<{ message: string; [k: string]: unknown }> | null;
  /** Mirrors OpenBbEnvelope.extra; engine sets `{mock:true}` in mock mode. */
  extra?: Record<string, unknown>;
}

/** Query parameters for the fa endpoint. */
export interface FaQuery {
  /** Symbol. Required. */
  symbol: string;
}

/* ============================================================ */
/* GET /api/v1/omega/key  (Step 6 - key stats)                  */
/* ============================================================ */

/**
 * Key-metrics row from OpenBB `/equity/fundamental/key_metrics`.
 * Provider-dependent field set; the KeyPanel renders what's present.
 */
export interface KeyMetricsRow {
  market_cap?: number;
  enterprise_value?: number;
  pe_ratio?: number;
  forward_pe?: number;
  peg_ratio?: number;
  price_to_book?: number;
  price_to_sales?: number;
  ev_to_sales?: number;
  ev_to_ebitda?: number;
  dividend_yield?: number;
  payout_ratio?: number;
  beta?: number;
  return_on_equity?: number;
  return_on_assets?: number;
  debt_to_equity?: number;
  current_ratio?: number;
  quick_ratio?: number;
  profit_margin?: number;
  operating_margin?: number;
  [k: string]: unknown;
}

/**
 * Multiples row from OpenBB `/equity/fundamental/multiples`. Some
 * providers fold these into key_metrics; the merged envelope handles
 * either layout.
 */
export interface MultiplesRow {
  pe_ratio_ttm?: number;
  ev_to_ebitda_ttm?: number;
  price_to_sales_ttm?: number;
  price_to_book_quarterly?: number;
  earnings_yield_ttm?: number;
  free_cash_flow_yield_ttm?: number;
  [k: string]: unknown;
}

/** Merged envelope for KEY: key_metrics + multiples. */
export interface KeyEnvelope {
  key_metrics: KeyMetricsRow[];
  multiples: MultiplesRow[];
  provider?: string;
  warnings?: Array<{ message: string; [k: string]: unknown }> | null;
  extra?: Record<string, unknown>;
}

export interface KeyQuery {
  symbol: string;
}

/* ============================================================ */
/* GET /api/v1/omega/dvd  (Step 6 - dividends)                  */
/* ============================================================ */

/**
 * One dividend payment. Shape matches OpenBB
 * `/equity/fundamental/dividends`.
 */
export interface Dividend {
  ex_dividend_date?: string;
  amount?: number;
  /** Some providers expose record / payment / declaration dates. */
  record_date?: string;
  payment_date?: string;
  declaration_date?: string;
  /** ISO date label most providers expose; alias for ex_dividend_date. */
  date?: string;
  [k: string]: unknown;
}

export interface DvdQuery {
  symbol: string;
}

/* ============================================================ */
/* GET /api/v1/omega/ee  (Step 6 - earnings estimates)          */
/* ============================================================ */

/** One consensus row from OpenBB `/equity/estimates/consensus`. */
export interface EpsConsensus {
  symbol?: string;
  fiscal_period?: string;
  fiscal_year?: number;
  eps_avg?: number;
  eps_high?: number;
  eps_low?: number;
  revenue_avg?: number;
  revenue_high?: number;
  revenue_low?: number;
  number_of_analysts?: number;
  [k: string]: unknown;
}

/** One surprise row from OpenBB `/equity/estimates/surprise`. */
export interface EpsSurprise {
  symbol?: string;
  date?: string;
  fiscal_period?: string;
  fiscal_year?: number;
  eps_actual?: number;
  eps_estimate?: number;
  eps_surprise?: number;
  surprise_percent?: number;
  [k: string]: unknown;
}

/** Merged envelope for EE: consensus + surprise. */
export interface EeEnvelope {
  consensus: EpsConsensus[];
  surprise: EpsSurprise[];
  provider?: string;
  warnings?: Array<{ message: string; [k: string]: unknown }> | null;
  extra?: Record<string, unknown>;
}

export interface EeQuery {
  symbol: string;
}

/* ============================================================ */
/* GET /api/v1/omega/ni  (Step 6 - per-symbol news)             */
/* ============================================================ */

/**
 * NI uses the same row shape as INTEL (IntelArticle) — both come from
 * OpenBB news endpoints. The only difference is the route:
 *   INTEL -> /news/world
 *   NI    -> /news/company?symbol=<sym>
 */
export interface NiQuery {
  /** Symbol. Required. */
  symbol: string;
  /** Number of articles. Server clamps to [1, 50]. */
  limit?: number;
}

/* ============================================================ */
/* GET /api/v1/omega/gp + /hp  (Step 6 - historical bars)       */
/* ============================================================ */

/**
 * One historical OHLCV bar. Shape matches OpenBB
 * `/equity/price/historical`. Adjusted vs unadjusted prices are provider-
 * dependent — the panels render whichever set is present.
 */
export interface HistoricalBar {
  /** ISO date or datetime string. */
  date: string;
  open?: number;
  high?: number;
  low?: number;
  close?: number;
  volume?: number;
  /** Some providers include adjusted close separately. */
  adj_close?: number;
  [k: string]: unknown;
}

/** Query parameters for the gp endpoint (chart). */
export interface GpQuery {
  symbol: string;
  /** Bar interval. Default "1d". */
  interval?: BarInterval;
  /** ISO date lower bound. Optional. */
  start_date?: string;
  /** ISO date upper bound. Optional. */
  end_date?: string;
}

/** Query parameters for the hp endpoint (table). */
export interface HpQuery {
  symbol: string;
  /** Bar interval. Default "1d". */
  interval?: BarInterval;
  start_date?: string;
  end_date?: string;
}

/* ============================================================ */
/* GET /api/v1/omega/qr  (Step 6 - multi-symbol quote tape)     */
/* ============================================================ */

/**
 * One quote row. Same shape as WeiQuote — OpenBB
 * `/equity/price/quote` returns the same fields whether called against
 * a single symbol or a comma-separated list.
 */
export interface QuoteRow {
  symbol: string;
  name?: string;
  last_price?: number;
  bid?: number;
  ask?: number;
  change?: number;
  change_percent?: number;
  volume?: number;
  prev_close?: number;
  open?: number;
  high?: number;
  low?: number;
  [k: string]: unknown;
}

/** Query parameters for the qr endpoint. */
export interface QrQuery {
  /** Comma-separated symbol list. Required. */
  symbols: string;
  provider?: string;
}

/* ============================================================ */
/* GET /api/v1/omega/des  (Step 6 - company description)        */
/* ============================================================ */

/**
 * Single-symbol company profile from OpenBB `/equity/profile`. All
 * fields are optional — providers vary in completeness.
 */
export interface CompanyProfile {
  symbol?: string;
  name?: string;
  description?: string;
  industry?: string;
  sector?: string;
  /** ISO date. */
  ipo_date?: string;
  ceo?: string;
  hq_country?: string;
  hq_state?: string;
  hq_city?: string;
  /** Number of employees. */
  employees?: number;
  website?: string;
  exchange?: string;
  currency?: string;
  market_cap?: number;
  [k: string]: unknown;
}

export interface DesQuery {
  symbol: string;
}

/* ============================================================ */
/* GET /api/v1/omega/fxc  (Step 6 - FX cross rates)             */
/* ============================================================ */

/**
 * One FX quote row. Shape matches OpenBB `/currency/price/quote`. Some
 * providers return the symbol as a slash-separated pair, others as the
 * concatenated 6-letter form (EURUSD); FxcPanel renders either.
 */
export interface FxQuote {
  symbol: string;
  name?: string;
  last_price?: number;
  bid?: number;
  ask?: number;
  change?: number;
  change_percent?: number;
  volume?: number;
  [k: string]: unknown;
}

/** Query parameters for the fxc endpoint. */
export interface FxcQuery {
  /**
   * Either a single pair like "EUR/USD" / "EURUSD", or a region preset
   * recognised by the engine:
   *   "MAJORS"  EUR/USD, GBP/USD, USD/JPY, USD/CHF, AUD/USD, USD/CAD, NZD/USD
   *   "EUR"     EUR/USD, EUR/GBP, EUR/JPY, EUR/CHF, EUR/AUD
   *   "ASIA"    USD/JPY, USD/CNH, USD/HKD, USD/SGD, USD/KRW
   *   "EM"      USD/MXN, USD/BRL, USD/ZAR, USD/TRY, USD/INR
   */
  pair?: string;
  provider?: string;
}

/* ============================================================ */
/* GET /api/v1/omega/crypto  (Step 6 - crypto quotes)           */
/* ============================================================ */

/**
 * One crypto quote row. Shape matches OpenBB `/crypto/price/quote`.
 */
export interface CryptoQuote {
  symbol: string;
  name?: string;
  last_price?: number;
  change?: number;
  change_percent?: number;
  volume?: number;
  market_cap?: number;
  [k: string]: unknown;
}

export interface CryptoQuery {
  /**
   * Comma-separated symbol list, or a region preset:
   *   "MAJORS"  BTC-USD, ETH-USD, BNB-USD, XRP-USD, SOL-USD
   *   "DEFI"    UNI-USD, AAVE-USD, MKR-USD, SNX-USD, COMP-USD
   *   "STABLE"  USDT-USD, USDC-USD, DAI-USD, BUSD-USD
   */
  symbols?: string;
  provider?: string;
}

/* ============================================================ */
/* GET /api/v1/omega/watch  (Step 6 - nightly screener)         */
/* ============================================================ */

/**
 * One WATCH hit row, persisted by the engine-side WatchScheduler. The
 * scheduler runs the INTEL screener over the configured universe each
 * night and stores hits in g_watch_hits; the /watch route returns the
 * current registry snapshot.
 */
export interface WatchHit {
  symbol: string;
  /** Engine-assigned screener tag (e.g. "S&P-MOMO", "NDX-MEAN-REV"). */
  signal?: string;
  /** Score / strength of the hit. Provider-dependent meaning. */
  score?: number;
  /** Last-price snapshot when the hit was recorded. */
  last_price?: number;
  change_percent?: number;
  volume?: number;
  /** ISO-8601 timestamp of when this hit was added to the registry. */
  flagged_at?: string;
  /** Optional human-readable rationale. */
  rationale?: string;
  [k: string]: unknown;
}

/**
 * WATCH envelope. Engine-cron-driven; `last_run_ts` and `next_run_ts`
 * surface the scheduler's heartbeat so the UI can show "scanning…" /
 * "next scan in 4h" without a separate status route.
 */
export interface WatchEnvelope {
  hits: WatchHit[];
  /** unix ms of the last completed scheduler run. 0 = never. */
  last_run_ts: number;
  /** unix ms of the next scheduled scan. */
  next_run_ts: number;
  /** True iff a scan is currently in flight. */
  scanning: boolean;
  /** Universe label (SP500 / NDX / ALL). */
  universe: string;
  /** Optional provider id when hits are derived from a specific OpenBB call. */
  provider?: string;
  warnings?: Array<{ message: string; [k: string]: unknown }> | null;
  extra?: Record<string, unknown>;
}

export interface WatchQuery {
  /** Universe to filter / scan. Default "SP500". */
  universe?: 'SP500' | 'NDX' | 'ALL' | string;
}

/* ============================================================ */
/* Errors                                                       */
/* ============================================================ */

/**
 * Typed error for all OmegaApi calls. Carries the HTTP status when
 * available. Network / abort errors surface with `status: 0`.
 *
 * The class form is intentional so consumers can `instanceof` check
 * inside a try/catch without losing structured fields.
 */
export class OmegaApiError extends Error {
  /** HTTP status code, or 0 for network / aborted requests. */
  public readonly status: number;

  /** Endpoint URL the request targeted (relative to the Vite host). */
  public readonly url: string;

  /**
   * Optional response body, raw text. Only populated for HTTP errors
   * where the server returned a body — never set for aborts / network
   * failures (where there is no response).
   */
  public readonly body?: string;

  /** True if the underlying fetch was aborted (timeout or caller). */
  public readonly aborted: boolean;

  constructor(opts: {
    message: string;
    status: number;
    url: string;
    body?: string;
    aborted?: boolean;
  }) {
    super(opts.message);
    this.name = 'OmegaApiError';
    this.status = opts.status;
    this.url = opts.url;
    this.body = opts.body;
    this.aborted = opts.aborted === true;
    // Preserve the prototype chain across transpilation targets.
    Object.setPrototypeOf(this, OmegaApiError.prototype);
  }
}
