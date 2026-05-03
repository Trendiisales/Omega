// App — top-level shell.
//
// Layout (top to bottom):
//   1. Title bar with back/forward chevrons and live engine-link pill.
//   2. Command bar with autocomplete.
//   3. WorkspaceTabs.
//   4. Persistent back/forward breadcrumb bar (Step 7 — surfaces history
//      navigation as a primary, always-visible affordance instead of the
//      easy-to-miss header chevrons).
//   5. Active workspace panel (rendered via PanelHost).
//   6. Footer status line.
//
// Step 3 update:
//   - Workspaces now carry an `args: string[]` list parsed from the command
//     bar (e.g. "POS HBG" -> args: ["HBG"]). The active workspace's args are
//     forwarded to PanelHost so the live panel can use them for filters.
//
// Step 4 update:
//   - `navigate` accepts a raw target string ("LDG HybridGold", "TRADE 12345")
//     so panel-internal navigation can carry args through resolveCode.
//
// Step 7 update:
//   - Per-workspace back/forward history stack with Alt+←/→ + Cmd/Ctrl+[/]
//     and header chevron buttons. Forward navigation pushes onto the stack
//     and trims any forward-tail.
//   - **Breadcrumb back bar** added between WorkspaceTabs and the panel.
//     Always rendered so users on any screen can see their navigation
//     state and click a single button to go back. Shows the literal panel
//     title + args of the previous (or next) entry so the destination is
//     unambiguous before clicking. Hidden only on a fresh tab where there
//     is no history to traverse — keeps the interior visual quiet.
//   - Hardcoded "engine link: pending" string replaced by a live status
//     pill driven by useEngineHealth. Pill colour: green=connected,
//     amber=pending, red=down. Pill carries an accessible title attribute
//     with the most recent error message.

import { useCallback, useEffect, useMemo, useState } from 'react';
import { CommandBar } from '@/components/CommandBar';
import { WorkspaceTabs } from '@/components/WorkspaceTabs';
import { PanelHost } from '@/panels/PanelHost';
import { PANEL_REGISTRY, resolveCode } from '@/router/functionCodes';
import { useEngineHealth } from '@/hooks/useEngineHealth';
import type {
  EngineLinkStatus,
  HistoryEntry,
  RouteResult,
  Workspace,
} from '@/types';

/** Cap a workspace history list to this many entries. */
const MAX_HISTORY = 50;

function makeId(): string {
  return `ws-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 7)}`;
}

/** Build the initial workspace shape with a one-entry history stack. */
function makeWorkspace(): Workspace {
  const entry: HistoryEntry = { code: 'HOME', args: [] };
  return {
    id: makeId(),
    code: entry.code,
    args: entry.args,
    history: [entry],
    historyIdx: 0,
  };
}

const INITIAL_WORKSPACES: Workspace[] = [makeWorkspace()];

/**
 * Push a new history entry onto a workspace, trimming forward-tail and
 * bounding the stack. No-op when the new entry equals the current.
 */
function pushHistory(ws: Workspace, entry: HistoryEntry): Workspace {
  const cur = ws.history[ws.historyIdx];
  if (
    cur &&
    cur.code === entry.code &&
    cur.args.length === entry.args.length &&
    cur.args.every((a, i) => a === entry.args[i])
  ) {
    return { ...ws, code: entry.code, args: entry.args };
  }

  const head = ws.history.slice(0, ws.historyIdx + 1);
  const next = [...head, entry];
  const trimmed =
    next.length > MAX_HISTORY ? next.slice(next.length - MAX_HISTORY) : next;

  return {
    ...ws,
    code: entry.code,
    args: entry.args,
    history: trimmed,
    historyIdx: trimmed.length - 1,
  };
}

/** Move the workspace's history pointer by `delta`. */
function stepHistory(ws: Workspace, delta: number): Workspace {
  const target = ws.historyIdx + delta;
  if (target < 0 || target >= ws.history.length) return ws;
  const entry = ws.history[target]!;
  return {
    ...ws,
    code: entry.code,
    args: entry.args,
    historyIdx: target,
  };
}

/** Format a HistoryEntry as a human label: "TRADE Drill-Down · 12345". */
function describeEntry(entry: HistoryEntry): string {
  const desc = PANEL_REGISTRY[entry.code];
  const title = desc ? desc.title : entry.code;
  if (entry.args.length === 0) return title;
  return `${title} · ${entry.args.join(' ')}`;
}

export default function App() {
  const [workspaces, setWorkspaces] = useState<Workspace[]>(INITIAL_WORKSPACES);
  const [activeId, setActiveId] = useState<string>(INITIAL_WORKSPACES[0]!.id);

  const activeWorkspace = useMemo(
    () => workspaces.find((w) => w.id === activeId) ?? workspaces[0]!,
    [workspaces, activeId]
  );

  const engineHealth = useEngineHealth();

  const dispatchResult = useCallback(
    (result: RouteResult) => {
      setWorkspaces((prev) =>
        prev.map((w) =>
          w.id === activeId
            ? pushHistory(w, { code: result.code, args: result.args })
            : w
        )
      );
    },
    [activeId]
  );

  const navigate = useCallback(
    (target: string) => {
      const result = resolveCode(target);
      dispatchResult(result);
    },
    [dispatchResult]
  );

  const goBack = useCallback(() => {
    setWorkspaces((prev) =>
      prev.map((w) => (w.id === activeId ? stepHistory(w, -1) : w))
    );
  }, [activeId]);

  const goForward = useCallback(() => {
    setWorkspaces((prev) =>
      prev.map((w) => (w.id === activeId ? stepHistory(w, +1) : w))
    );
  }, [activeId]);

  const canGoBack = activeWorkspace.historyIdx > 0;
  const canGoForward =
    activeWorkspace.historyIdx < activeWorkspace.history.length - 1;
  const prevEntry = canGoBack
    ? activeWorkspace.history[activeWorkspace.historyIdx - 1]
    : null;
  const nextEntry = canGoForward
    ? activeWorkspace.history[activeWorkspace.historyIdx + 1]
    : null;

  const addWorkspace = useCallback(() => {
    const ws = makeWorkspace();
    setWorkspaces((prev) => [...prev, ws]);
    setActiveId(ws.id);
  }, []);

  const closeWorkspace = useCallback(
    (id: string) => {
      setWorkspaces((prev) => {
        if (prev.length <= 1) return prev;
        const idx = prev.findIndex((w) => w.id === id);
        const next = prev.filter((w) => w.id !== id);
        if (id === activeId) {
          const fallback = next[Math.max(0, idx - 1)] ?? next[0]!;
          setActiveId(fallback.id);
        }
        return next;
      });
    },
    [activeId]
  );

  // Keyboard shortcuts. The command-bar input does NOT preventDefault on
  // Alt+arrows, so the window listener still fires when the input is
  // focused. We swallow the keystroke after dispatching to keep the
  // browser's text-edit interpretation from racing the navigation.
  useEffect(() => {
    function onKey(e: KeyboardEvent) {
      const mod = e.ctrlKey || e.metaKey;
      const key = e.key.toLowerCase();

      if (mod && key === 't') {
        e.preventDefault();
        addWorkspace();
        return;
      }
      if (mod && key === 'w') {
        e.preventDefault();
        closeWorkspace(activeId);
        return;
      }
      if (e.altKey && !mod && e.key === 'ArrowLeft') {
        e.preventDefault();
        goBack();
        return;
      }
      if (e.altKey && !mod && e.key === 'ArrowRight') {
        e.preventDefault();
        goForward();
        return;
      }
      if (mod && e.key === '[') {
        e.preventDefault();
        goBack();
        return;
      }
      if (mod && e.key === ']') {
        e.preventDefault();
        goForward();
        return;
      }
    }
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [addWorkspace, closeWorkspace, activeId, goBack, goForward]);

  const pill = engineLinkPill(engineHealth.status);
  const pillTitle =
    engineHealth.status === 'connected'
      ? engineHealth.lastOkAt
        ? `Last OK ${formatAge(engineHealth.lastOkAt)}`
        : 'Engine link OK'
      : engineHealth.lastError
        ? `Engine link ${engineHealth.status}: ${engineHealth.lastError}`
        : `Engine link ${engineHealth.status}`;

  // The breadcrumb bar is suppressed on a fresh, single-entry HOME tab so
  // the interior is visually quiet on first load. Any forward navigation
  // (or even a HOME->HOME re-dispatch with args) creates a 2nd entry and
  // the bar appears with a clearly visible Back affordance.
  const showBreadcrumbBar =
    activeWorkspace.history.length > 1 || canGoBack || canGoForward;

  return (
    <div className="flex h-full w-full flex-col bg-black text-amber-400">
      {/* Title bar */}
      <header className="flex items-center justify-between border-b border-amber-700/60 bg-black px-4 py-2">
        <div className="flex items-baseline gap-3">
          <span className="font-mono text-sm font-bold tracking-[0.3em] text-amber-300">
            OMEGA
          </span>
          <span className="font-mono text-[10px] uppercase tracking-widest text-amber-600">
            terminal &middot; v0.1 &middot; step 7
          </span>
        </div>

        <div className="flex items-center gap-3 font-mono text-[10px] uppercase tracking-widest">
          <div className="flex items-center gap-1">
            <button
              type="button"
              onClick={goBack}
              disabled={!canGoBack}
              aria-label="Back (Alt+Left)"
              title="Back (Alt+Left)"
              className={
                'rounded px-1.5 py-0.5 font-mono text-xs leading-none ' +
                (canGoBack
                  ? 'text-amber-300 hover:bg-amber-900/30'
                  : 'cursor-not-allowed text-amber-800')
              }
            >
              {'◂'}
            </button>
            <button
              type="button"
              onClick={goForward}
              disabled={!canGoForward}
              aria-label="Forward (Alt+Right)"
              title="Forward (Alt+Right)"
              className={
                'rounded px-1.5 py-0.5 font-mono text-xs leading-none ' +
                (canGoForward
                  ? 'text-amber-300 hover:bg-amber-900/30'
                  : 'cursor-not-allowed text-amber-800')
              }
            >
              {'▸'}
            </button>
          </div>

          <span className="flex items-center gap-1.5" title={pillTitle}>
            <span
              aria-hidden="true"
              className={`inline-block h-2 w-2 rounded-full ${pill.dotClass}`}
            />
            <span className={pill.textClass}>engine link: {pill.label}</span>
          </span>
        </div>
      </header>

      {/* Command bar */}
      <CommandBar onDispatch={dispatchResult} />

      {/* Workspace tabs */}
      <WorkspaceTabs
        workspaces={workspaces}
        activeId={activeWorkspace.id}
        onActivate={setActiveId}
        onClose={closeWorkspace}
        onAdd={addWorkspace}
      />

      {/* Persistent back/forward breadcrumb bar (Step 7).
          Always rendered when the active tab has any history depth, so
          back navigation is a primary affordance on every screen. */}
      {showBreadcrumbBar && (
        <nav
          aria-label="Back / forward navigation"
          className="flex items-center justify-between border-b border-amber-800/50 bg-amber-950/20 px-4 py-1.5"
        >
          {/* Left: prominent Back button. */}
          <button
            type="button"
            onClick={goBack}
            disabled={!canGoBack}
            className={
              'flex items-center gap-2 rounded border px-3 py-1 font-mono text-xs uppercase tracking-widest transition-colors ' +
              (canGoBack
                ? 'border-amber-600 bg-amber-900/40 text-amber-200 hover:border-amber-400 hover:bg-amber-800/60'
                : 'cursor-not-allowed border-amber-900/40 bg-black text-amber-800')
            }
            title={canGoBack ? 'Alt+Left or Cmd/Ctrl+[' : 'No previous panel'}
          >
            <span className="text-base leading-none">{'◀'}</span>
            <span>
              Back
              {prevEntry && (
                <>
                  {' '}
                  <span className="text-amber-400/80">
                    to {describeEntry(prevEntry)}
                  </span>
                </>
              )}
            </span>
          </button>

          {/* Middle: history depth indicator. Tiny but informative. */}
          <span className="font-mono text-[10px] uppercase tracking-widest text-amber-600">
            history {activeWorkspace.historyIdx + 1} / {activeWorkspace.history.length}
          </span>

          {/* Right: forward button (only when meaningful). */}
          <button
            type="button"
            onClick={goForward}
            disabled={!canGoForward}
            className={
              'flex items-center gap-2 rounded border px-3 py-1 font-mono text-xs uppercase tracking-widest transition-colors ' +
              (canGoForward
                ? 'border-amber-600 bg-amber-900/40 text-amber-200 hover:border-amber-400 hover:bg-amber-800/60'
                : 'cursor-not-allowed border-amber-900/40 bg-black text-amber-800')
            }
            title={canGoForward ? 'Alt+Right or Cmd/Ctrl+]' : 'No forward panel'}
          >
            <span>
              {nextEntry && (
                <>
                  <span className="text-amber-400/80">
                    {describeEntry(nextEntry)}
                  </span>{' '}
                </>
              )}
              Forward
            </span>
            <span className="text-base leading-none">{'▶'}</span>
          </button>
        </nav>
      )}

      {/* Active panel */}
      <main className="flex-1 overflow-hidden">
        <PanelHost
          code={activeWorkspace.code}
          args={activeWorkspace.args}
          onNavigate={navigate}
        />
      </main>

      {/* Footer */}
      <footer className="flex items-center justify-between border-t border-amber-700/40 bg-black px-4 py-1.5 font-mono text-[10px] uppercase tracking-widest text-amber-600">
        <span>
          {workspaces.length} workspace{workspaces.length === 1 ? '' : 's'}
          {' '}&middot;{' '}active: {activeWorkspace.code}
          {activeWorkspace.args.length > 0 && (
            <> &middot; args: {activeWorkspace.args.join(' ')}</>
          )}
          {activeWorkspace.history.length > 1 && (
            <>
              {' '}&middot; hist: {activeWorkspace.historyIdx + 1}/
              {activeWorkspace.history.length}
            </>
          )}
        </span>
        <span>
          Ctrl+K cmd  &middot;  Ctrl+T new  &middot;  Ctrl+W close
          {'  '}&middot;{'  '}Alt+&larr;/&rarr; back/fwd
        </span>
      </footer>
    </div>
  );
}

function engineLinkPill(status: EngineLinkStatus): {
  label: string;
  dotClass: string;
  textClass: string;
} {
  switch (status) {
    case 'connected':
      return {
        label: 'connected',
        dotClass: 'bg-emerald-400',
        textClass: 'text-emerald-400',
      };
    case 'down':
      return {
        label: 'down',
        dotClass: 'bg-red-500',
        textClass: 'text-red-500',
      };
    case 'pending':
    default:
      return {
        label: 'pending',
        dotClass: 'bg-amber-500',
        textClass: 'text-amber-500',
      };
  }
}

function formatAge(ts: number): string {
  const ageMs = Math.max(0, Date.now() - ts);
  const sec = Math.round(ageMs / 1000);
  if (sec < 60) return `${sec}s ago`;
  const min = Math.round(sec / 60);
  if (min < 60) return `${min}m ago`;
  const hr = Math.round(min / 60);
  return `${hr}h ago`;
}
