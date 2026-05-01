// usePanelData — generic data-fetching hook used by Step 3 panels (CC, ENG,
// POS) and reusable for the Step 4+ panels (LDG, TRADE, CELL).
//
// Responsibilities:
//   - Fire a fetch on mount, retry whenever any value in `deps` changes.
//   - Optional polling: if `pollMs` is provided and > 0, refetch on that
//     interval. The polling timer pauses when the document is hidden
//     (visibilitychange) so background tabs don't keep hammering the API.
//   - AbortController lifecycle: every fetch carries a fresh signal that is
//     aborted on unmount and on dep change. This is what STEP3_OPENER means
//     by "AbortController unmount cleanup -- no zombie fetches that outlive
//     the panel mount".
//   - Discriminated union state (`{status: 'loading'|'ok'|'err'}`) so panel
//     code can switch on it cleanly.
//   - Manual refetch via the returned `refetch()` function. Used by the
//     red error banner's "retry" button.
//
// Why we own the AbortController here and not inside omega.ts:
//   omega.ts already accepts an external signal -- we just feed it ours
//   from the hook. Centralising the lifecycle in one place lets every panel
//   share the exact same teardown semantics.
//
// Generic over T. Caller supplies a `fetcher: (signal: AbortSignal) => Promise<T>`
// that wraps one of getEngines / getPositions / getLedger / getEquity from
// src/api/omega.ts.

import { useCallback, useEffect, useRef, useState } from 'react';
import { OmegaApiError } from '@/api/types';

/**
 * Discriminated union describing one async fetch's lifecycle. Panels switch
 * on `status` to decide what to render.
 */
export type PanelData<T> =
  | { status: 'loading'; data: T | null }
  | { status: 'ok';      data: T }
  | { status: 'err';     error: OmegaApiError; data: T | null };

export interface UsePanelDataOptions {
  /** Polling cadence in ms. 0 or undefined disables polling. */
  pollMs?: number;
  /**
   * Whether to retain the previously-loaded data while a refetch is in
   * flight (so the UI doesn't flicker between "ok -> loading -> ok" on
   * every poll). Defaults to true.
   */
  keepPreviousOnRefetch?: boolean;
}

/**
 * Standard data hook. Returns the current state plus a manual `refetch`
 * callback that aborts any in-flight request and starts a new one.
 *
 * `deps` are forwarded to React's dep-array machinery: changes invalidate
 * the in-flight request and trigger a fresh fetch with a new signal.
 */
export function usePanelData<T>(
  fetcher: (signal: AbortSignal) => Promise<T>,
  deps: React.DependencyList,
  opts: UsePanelDataOptions = {}
): { state: PanelData<T>; refetch: () => void } {
  const { pollMs, keepPreviousOnRefetch = true } = opts;

  const [state, setState] = useState<PanelData<T>>({ status: 'loading', data: null });

  // Hold the latest controller in a ref so unmount/refetch can abort it.
  const controllerRef = useRef<AbortController | null>(null);
  // Hold the latest data in a ref so the next fetch can preserve it without
  // pulling state into deps (which would re-create the run callback every
  // render).
  const dataRef = useRef<T | null>(null);
  // Track mount status so a late-arriving promise after unmount doesn't
  // setState into a dead component.
  const mountedRef = useRef<boolean>(true);

  const run = useCallback(async () => {
    // Cancel any in-flight request before issuing a new one.
    controllerRef.current?.abort();
    const ac = new AbortController();
    controllerRef.current = ac;

    setState((prev) => {
      if (keepPreviousOnRefetch && (prev.status === 'ok' || prev.status === 'err')) {
        return { status: 'loading', data: prev.data };
      }
      return { status: 'loading', data: null };
    });

    try {
      const data = await fetcher(ac.signal);
      if (!mountedRef.current || ac.signal.aborted) return;
      dataRef.current = data;
      setState({ status: 'ok', data });
    } catch (err) {
      if (!mountedRef.current || ac.signal.aborted) return;
      const apiErr =
        err instanceof OmegaApiError
          ? err
          : new OmegaApiError({
              message: `Unexpected error: ${(err as Error)?.message ?? 'unknown'}`,
              status: 0,
              url: '',
              aborted: false,
            });
      setState({ status: 'err', error: apiErr, data: dataRef.current });
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [fetcher, keepPreviousOnRefetch]);

  // Fire on mount + whenever deps change.
  useEffect(() => {
    mountedRef.current = true;
    run();
    return () => {
      mountedRef.current = false;
      controllerRef.current?.abort();
      controllerRef.current = null;
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, deps);

  // Polling timer. Skipped while document.hidden so background tabs don't
  // hammer the API. We re-check visibility on resume.
  useEffect(() => {
    if (!pollMs || pollMs <= 0) return;
    let timer: ReturnType<typeof setInterval> | null = null;

    const start = () => {
      if (timer) return;
      timer = setInterval(() => {
        if (document.hidden) return;
        run();
      }, pollMs);
    };
    const stop = () => {
      if (timer) {
        clearInterval(timer);
        timer = null;
      }
    };

    const onVisibility = () => {
      if (document.hidden) {
        stop();
      } else {
        // Run immediately on resume to refresh stale data, then resume polling.
        run();
        start();
      }
    };

    if (!document.hidden) start();
    document.addEventListener('visibilitychange', onVisibility);

    return () => {
      stop();
      document.removeEventListener('visibilitychange', onVisibility);
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [pollMs, run]);

  const refetch = useCallback(() => {
    run();
  }, [run]);

  return { state, refetch };
}
