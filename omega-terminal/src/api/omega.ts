// omega-terminal — typed fetch wrappers for the OmegaApiServer.
//
// Reference: docs/omega_terminal/STEP2_OPENER.md § "Step 2 scope" §B.2.
//
// Design goals (per STEP2_OPENER):
//   - All public functions return Promise<T> of a strongly-typed shape.
//   - Cancellation: every call accepts an optional AbortSignal and
//     supports a default 5 s timeout that is independent of the caller's
//     own signal. The two abort sources are linked via a small helper.
//   - Errors: any non-2xx status, network failure, abort, or JSON parse
//     failure is normalized to a thrown `OmegaApiError`. Consumers can
//     `instanceof OmegaApiError` to branch on the structured fields.
//   - No retries here. Step 2 is the wire layer; retry policy belongs
//     in the panels (Step 3+) where it can be UI-aware.
//
// Step 5 additions: `getIntel`, `getCurv`, `getWei`, `getMov` — typed
// wrappers around the OpenBB-backed routes added in OmegaApiServer.cpp.
// All four return `OpenBbEnvelope<T>` (the OpenBB OBBject wrapper passed
// through verbatim by OpenBbProxy). The Step-5 panels read `.results`
// off the envelope and surface `provider` / `warnings` in a corner badge.
//
// Per-call timeouts are bumped to 30 s for the OpenBB routes because
// OpenBB Hub free-tier endpoints can take 5-10 s on cold starts. The
// engine-side proxy enforces its own 30 s libcurl timeout, so a UI
// timeout shorter than that would just race the proxy's response.
//
// Endpoints are reached via the Vite dev proxy in development:
//   GET /api/v1/omega/<route>  ->  http://127.0.0.1:7781/api/v1/omega/<route>
//
// In a production bundle the same relative paths work as long as the
// hosting layer proxies /api/v1/omega/* to the engine's OmegaApiServer.

import {
  Engine,
  Position,
  LedgerEntry,
  EquityPoint,
  LedgerQuery,
  EquityQuery,
  IntelArticle,
  IntelQuery,
  CurvPoint,
  CurvQuery,
  WeiQuote,
  WeiQuery,
  MovRow,
  MovQuery,
  OpenBbEnvelope,
  OmegaApiError,
} from '@/api/types';

/* ============================================================ */
/* Configuration                                                 */
/* ============================================================ */

/**
 * Base path for all Omega API calls. Kept relative so the same code
 * path works behind the Vite dev proxy and behind a production
 * reverse proxy. Override only via `setOmegaApiBase` in tests.
 */
let API_BASE = '/api/v1/omega';

/**
 * Default per-request timeout in ms. Caller can override via the
 * `timeoutMs` option on each call.
 */
const DEFAULT_TIMEOUT_MS = 5000;

/**
 * Step-5 OpenBB routes can take longer than the engine-internal routes
 * because the proxy is round-tripping over the public internet. Bumped
 * to 30 s to match the libcurl timeout enforced server-side.
 */
const OPENBB_TIMEOUT_MS = 30000;

/**
 * Override the API base. Intended for tests only — production code
 * should rely on the default and the Vite proxy.
 */
export function setOmegaApiBase(base: string): void {
  // Strip a trailing slash so callers do not have to think about it.
  API_BASE = base.replace(/\/+$/, '');
}

/** Read-only view of the current base. Mostly for debugging. */
export function getOmegaApiBase(): string {
  return API_BASE;
}

/* ============================================================ */
/* Internal helpers                                              */
/* ============================================================ */

/** Options shared by every request wrapper. */
export interface OmegaCallOptions {
  /** Caller-owned abort signal. Linked with the timeout signal. */
  signal?: AbortSignal;
  /** Override the default 5000 ms timeout. */
  timeoutMs?: number;
}

/**
 * Build a `URLSearchParams` object from a record, dropping
 * undefined/null fields. Numeric values are coerced via String().
 */
function buildQuery(params: Record<string, string | number | undefined | null>): string {
  const sp = new URLSearchParams();
  for (const [k, v] of Object.entries(params)) {
    if (v === undefined || v === null) continue;
    sp.set(k, String(v));
  }
  const q = sp.toString();
  return q.length === 0 ? '' : `?${q}`;
}

/**
 * Compose a caller-supplied AbortSignal with an internal timeout.
 * Returns the linked signal plus a `cancel` cleanup that the caller
 * should run in `finally` to clear the timeout.
 */
function linkSignals(
  caller: AbortSignal | undefined,
  timeoutMs: number,
): { signal: AbortSignal; cancel: () => void; timedOut: () => boolean } {
  const ac = new AbortController();
  let timedOut = false;

  const onCallerAbort = () => ac.abort(caller?.reason);
  if (caller) {
    if (caller.aborted) {
      ac.abort(caller.reason);
    } else {
      caller.addEventListener('abort', onCallerAbort, { once: true });
    }
  }

  const timer = setTimeout(() => {
    timedOut = true;
    ac.abort(new DOMException('OmegaApi request timed out', 'AbortError'));
  }, timeoutMs);

  return {
    signal: ac.signal,
    cancel: () => {
      clearTimeout(timer);
      if (caller) caller.removeEventListener('abort', onCallerAbort);
    },
    timedOut: () => timedOut,
  };
}

/**
 * Fetch a JSON body from the engine, or throw `OmegaApiError`.
 * Generic over the response payload `T`. The body is parsed once and
 * narrowed by the caller-supplied type — there is no runtime schema
 * check, so any drift between the C++ side and `types.ts` is a
 * compile-time concern.
 */
async function getJson<T>(path: string, opts: OmegaCallOptions = {}): Promise<T> {
  const url = `${API_BASE}${path}`;
  const timeoutMs = opts.timeoutMs ?? DEFAULT_TIMEOUT_MS;
  const linked = linkSignals(opts.signal, timeoutMs);

  let res: Response;
  try {
    res = await fetch(url, {
      method: 'GET',
      headers: { Accept: 'application/json' },
      signal: linked.signal,
      // No credentials for now — the server is bound to 127.0.0.1 only.
      credentials: 'omit',
    });
  } catch (err) {
    // Network failure or abort. Distinguish the two so consumers can
    // surface a sensible message.
    const aborted =
      (err as Error)?.name === 'AbortError' ||
      linked.signal.aborted ||
      linked.timedOut();
    linked.cancel();
    throw new OmegaApiError({
      message: aborted
        ? linked.timedOut()
          ? `OmegaApi request timed out after ${timeoutMs} ms (${url})`
          : `OmegaApi request aborted (${url})`
        : `OmegaApi network error: ${(err as Error)?.message ?? 'unknown'} (${url})`,
      status: 0,
      url,
      aborted,
    });
  }

  // We have an HTTP response; success path needs the body cleared too,
  // failure path reads the body to surface server diagnostics.
  if (!res.ok) {
    let body: string | undefined;
    try {
      body = await res.text();
    } catch {
      body = undefined;
    }
    linked.cancel();
    throw new OmegaApiError({
      message: `OmegaApi HTTP ${res.status} ${res.statusText} (${url})`,
      status: res.status,
      url,
      body,
      aborted: false,
    });
  }

  let parsed: T;
  try {
    parsed = (await res.json()) as T;
  } catch (err) {
    linked.cancel();
    throw new OmegaApiError({
      message: `OmegaApi JSON parse failed: ${(err as Error)?.message ?? 'unknown'} (${url})`,
      status: res.status,
      url,
      aborted: false,
    });
  }

  linked.cancel();
  return parsed;
}

/* ============================================================ */
/* Public API                                                    */
/* ============================================================ */

/**
 * GET /api/v1/omega/engines
 *
 * Returns the list of engines registered with `g_engines` on the C++
 * side, with their current snapshot.
 */
export function getEngines(opts: OmegaCallOptions = {}): Promise<Engine[]> {
  return getJson<Engine[]>('/engines', opts);
}

/**
 * GET /api/v1/omega/positions
 *
 * Returns the list of open positions across all engines, keyed by
 * symbol. Closed positions appear in the ledger, not here.
 */
export function getPositions(opts: OmegaCallOptions = {}): Promise<Position[]> {
  return getJson<Position[]>('/positions', opts);
}

/**
 * GET /api/v1/omega/ledger
 *
 * Returns closed trades, optionally filtered by date range, engine,
 * and limit. The server clamps `limit` to a sane maximum (~10 000) to
 * keep the JSON small.
 */
export function getLedger(
  query: LedgerQuery = {},
  opts: OmegaCallOptions = {},
): Promise<LedgerEntry[]> {
  const qs = buildQuery({
    from: query.from,
    to: query.to,
    engine: query.engine,
    limit: query.limit,
  });
  return getJson<LedgerEntry[]>(`/ledger${qs}`, opts);
}

/**
 * GET /api/v1/omega/equity
 *
 * Returns the equity time-series, bucketed at the requested interval.
 * Defaults are server-side: omitting `interval` yields the engine's
 * current preferred bucket (typically 1h for live, 1m for backtest
 * replay).
 */
export function getEquity(
  query: EquityQuery = {},
  opts: OmegaCallOptions = {},
): Promise<EquityPoint[]> {
  const qs = buildQuery({
    from: query.from,
    to: query.to,
    interval: query.interval,
  });
  return getJson<EquityPoint[]>(`/equity${qs}`, opts);
}

/* ============================================================ */
/* Step 5: OpenBB-backed market routes                          */
/* ============================================================ */

/**
 * GET /api/v1/omega/intel
 *
 * Returns an OpenBB news envelope (results: IntelArticle[]). The screen
 * argument is currently advisory -- the server route always calls
 * `/news/world` regardless. Step 6 will branch on the screen-id.
 */
export function getIntel(
  query: IntelQuery = {},
  opts: OmegaCallOptions = {},
): Promise<OpenBbEnvelope<IntelArticle>> {
  const qs = buildQuery({
    screen: query.screen,
    limit: query.limit,
  });
  return getJson<OpenBbEnvelope<IntelArticle>>(`/intel${qs}`, {
    timeoutMs: OPENBB_TIMEOUT_MS,
    ...opts,
  });
}

/**
 * GET /api/v1/omega/curv
 *
 * Returns an OpenBB treasury_rates envelope (results: CurvPoint[]). US
 * is fully wired via the federal_reserve provider; EU/JP return an
 * empty results array with a `warnings` entry until Step 6 wires those
 * providers.
 */
export function getCurv(
  query: CurvQuery = {},
  opts: OmegaCallOptions = {},
): Promise<OpenBbEnvelope<CurvPoint>> {
  const qs = buildQuery({
    region: query.region,
  });
  return getJson<OpenBbEnvelope<CurvPoint>>(`/curv${qs}`, {
    timeoutMs: OPENBB_TIMEOUT_MS,
    ...opts,
  });
}

/**
 * GET /api/v1/omega/wei
 *
 * Returns an OpenBB equity quote envelope (results: WeiQuote[]). The
 * region argument selects a curated symbol list (US/EU/ASIA/WORLD) on
 * the server; passing a literal comma-separated symbol list also works.
 */
export function getWei(
  query: WeiQuery = {},
  opts: OmegaCallOptions = {},
): Promise<OpenBbEnvelope<WeiQuote>> {
  const qs = buildQuery({
    region: query.region,
    provider: query.provider,
  });
  return getJson<OpenBbEnvelope<WeiQuote>>(`/wei${qs}`, {
    timeoutMs: OPENBB_TIMEOUT_MS,
    ...opts,
  });
}

/**
 * GET /api/v1/omega/mov
 *
 * Returns an OpenBB discovery envelope (results: MovRow[]). The
 * universe argument selects the OpenBB sub-route
 * /equity/discovery/{active|gainers|losers}; anything else is coerced
 * to "active".
 */
export function getMov(
  query: MovQuery = {},
  opts: OmegaCallOptions = {},
): Promise<OpenBbEnvelope<MovRow>> {
  const qs = buildQuery({
    universe: query.universe,
    provider: query.provider,
  });
  return getJson<OpenBbEnvelope<MovRow>>(`/mov${qs}`, {
    timeoutMs: OPENBB_TIMEOUT_MS,
    ...opts,
  });
}
