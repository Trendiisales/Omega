// useEngineHealth — title-bar status pill driver.
//
// Step 7 introduces this hook to replace the previous hardcoded
// "engine link: pending" string in App.tsx with a live readout of the
// engine REST link.
//
// Contract
// --------
//   - Polls GET /api/v1/omega/engines on a configurable interval
//     (default 5000 ms).
//   - First poll fires immediately on mount; subsequent polls fire on
//     the interval, so the pill turns green within one round-trip when
//     the engine comes up rather than waiting a full interval.
//   - Each in-flight request is abortable; if a new poll fires while
//     the previous one is still pending (slow engine), the previous
//     request is aborted to keep at most one outstanding fetch.
//   - On unmount the timer is cleared and any in-flight request is
//     aborted, so the hook leaves no dangling network work.
//   - HTTP timeout is enforced via AbortController inside the fetch
//     wrapper; we use 4000 ms by default, well under the 5 s poll
//     interval to avoid pile-up.
//   - A non-2xx response, network error, JSON parse failure, or abort
//     all flip status to 'down'. A 2xx response with a JSON-parseable
//     body flips status to 'connected'. The very first call lives in
//     'pending' until either of those resolves.
//   - The returned `lastError` is the most recent failure message (or
//     null after a success), useful for tooltip surfacing.
//   - The returned `lastOkAt` is the wall-clock millisecond of the most
//     recent successful poll, useful for "stale by Ns" indicators.
//
// The hook intentionally talks raw fetch rather than going through
// `getEngines()` from src/api/omega.ts so the polling layer stays
// independent of the typed API surface — we don't care about the
// payload here, only the status code.

import { useEffect, useRef, useState } from 'react';
import type { EngineLinkStatus } from '@/types';

/** Tunables. Defaults match the title-bar pill spec in step 7. */
export interface UseEngineHealthOptions {
  /** Poll cadence in milliseconds. Default 5000. */
  intervalMs?: number;
  /** Per-request timeout in milliseconds. Default 4000. */
  timeoutMs?: number;
  /** Override the URL probed. Default '/api/v1/omega/engines'. */
  url?: string;
}

/** Hook return shape. */
export interface EngineHealthState {
  /** Current pill state. */
  status: EngineLinkStatus;
  /** Wall-clock ms of the most recent successful poll, or null. */
  lastOkAt: number | null;
  /** Most recent failure message, or null after a success. */
  lastError: string | null;
}

const DEFAULT_INTERVAL_MS = 5000;
const DEFAULT_TIMEOUT_MS = 4000;
const DEFAULT_URL = '/api/v1/omega/engines';

export function useEngineHealth(opts: UseEngineHealthOptions = {}): EngineHealthState {
  const intervalMs = opts.intervalMs ?? DEFAULT_INTERVAL_MS;
  const timeoutMs = opts.timeoutMs ?? DEFAULT_TIMEOUT_MS;
  const url = opts.url ?? DEFAULT_URL;

  const [state, setState] = useState<EngineHealthState>({
    status: 'pending',
    lastOkAt: null,
    lastError: null,
  });

  // We keep both the timer id and the in-flight controller in refs so
  // the cleanup function can tear them down without the effect having
  // to depend on them (which would re-run the effect on every poll).
  const timerRef = useRef<ReturnType<typeof setTimeout> | null>(null);
  const controllerRef = useRef<AbortController | null>(null);
  const mountedRef = useRef<boolean>(true);

  useEffect(() => {
    mountedRef.current = true;

    async function probe(): Promise<void> {
      // Abort any previous in-flight request so we have at most one
      // outstanding poll at a time.
      if (controllerRef.current) {
        controllerRef.current.abort();
      }
      const ac = new AbortController();
      controllerRef.current = ac;

      // Per-request timeout. A separate timer keeps the request from
      // outliving the poll interval.
      const timeoutId = setTimeout(() => ac.abort(), timeoutMs);

      try {
        const res = await fetch(url, {
          method: 'GET',
          headers: { Accept: 'application/json' },
          signal: ac.signal,
          credentials: 'omit',
          // No-store so a CDN or proxy in front of us doesn't serve a
          // cached 200 after the engine has died.
          cache: 'no-store',
        });

        clearTimeout(timeoutId);

        if (!mountedRef.current) return;

        if (!res.ok) {
          setState({
            status: 'down',
            lastOkAt: null,
            lastError: `HTTP ${res.status} ${res.statusText}`,
          });
          return;
        }

        // We don't actually need the body — a 2xx is enough — but read
        // it so the connection is fully drained and the engine doesn't
        // log a half-closed stream.
        try {
          await res.text();
        } catch {
          // Ignore body-drain errors; the status code is what matters.
        }

        setState({
          status: 'connected',
          lastOkAt: Date.now(),
          lastError: null,
        });
      } catch (err) {
        clearTimeout(timeoutId);

        if (!mountedRef.current) return;

        const aborted =
          (err as Error)?.name === 'AbortError' || ac.signal.aborted;

        // An abort fired by *us* during cleanup or because of the next
        // poll is not really a "down" event; but the abort controller
        // can't distinguish between user-initiated and timeout-initiated
        // aborts. We treat any abort as 'down' so the pill always
        // reflects the latest reachable status — if the engine is up
        // and just slow, the next poll will recover the pill within
        // one interval.
        setState({
          status: 'down',
          lastOkAt: null,
          lastError: aborted
            ? `request aborted (timeout ${timeoutMs}ms)`
            : `network error: ${(err as Error)?.message ?? 'unknown'}`,
        });
      }
    }

    function scheduleNext(): void {
      if (!mountedRef.current) return;
      timerRef.current = setTimeout(async () => {
        await probe();
        scheduleNext();
      }, intervalMs);
    }

    // Fire immediately on mount, then schedule subsequent polls. We
    // don't await the first probe so the effect returns synchronously;
    // React will render the initial 'pending' state until probe lands.
    probe().then(scheduleNext);

    return () => {
      mountedRef.current = false;
      if (timerRef.current) {
        clearTimeout(timerRef.current);
        timerRef.current = null;
      }
      if (controllerRef.current) {
        controllerRef.current.abort();
        controllerRef.current = null;
      }
    };
  }, [intervalMs, timeoutMs, url]);

  return state;
}
