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
// All timestamps are unix-epoch milliseconds (int64) on the wire,
// represented as `number` in TS. The engine guarantees JS-safe range
// (< 2^53) for all timestamps it produces.
//
// OpenBB note: the Step-5 routes pass the OpenBB OBBject envelope
// through verbatim (no JSON parsing in C++). The UI dereferences
// `.results` to get the row array; the rest of the wrapper (provider,
// warnings, chart, extra) is informational. When OMEGA_OPENBB_TOKEN is
// not set on the server, the proxy returns HTTP 503 with a structured
// error body — surfacing through OmegaApiError exactly like any other
// transport error.

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
