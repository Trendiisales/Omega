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
// All timestamps are unix-epoch milliseconds (int64) on the wire,
// represented as `number` in TS. The engine guarantees JS-safe range
// (< 2^53) for all timestamps it produces.

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
