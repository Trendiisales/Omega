// App — top-level shell.
//
// Layout (top to bottom):
//   1. Title bar with back/forward chevrons and live engine-link pill.
//   2. Command bar with autocomplete.
//   3. WorkspaceTabs.
//   4. Active workspace panel (rendered via PanelHost).
//   5. Footer status line (back/forward hint added in step 7).
//
// Step 3 update:
//   - Workspaces now carry an `args: string[]` list parsed from the command
//     bar (e.g. "POS HBG" -> args: ["HBG"]). The active workspace's args are
//     forwarded to PanelHost so the live panel can use them for filters.
//   - HomePanel tile clicks dispatch with empty args (no command-bar typing).
//
// Step 4 update:
//   - `navigate` now accepts a raw target string (e.g. "LDG HybridGold" or
//     "TRADE 12345") so panel-internal navigation (ENG row click -> LDG,
//     POS row click -> LDG, LDG row click -> TRADE) can carry arguments.
//     The router's resolveCode already parses positional args; we just
//     widen the input type from FunctionCode to string and let the router
//     do the work.
//
// Step 7 update (this commit):
//   - Per-workspace back/forward history. Each workspace carries its own
//     `history: HistoryEntry[]` + `historyIdx: number`. Forward navigation
//     (command bar, panel row click, HOME tile) appends to the stack and
//     trims any forward-tail. Back/forward move `historyIdx` within the
//     existing stack so the panel state at each step is recoverable.
//   - Two new buttons in the title bar (◂ / ▸) and four new keyboard
//     shortcuts (Alt+Left, Alt+Right, Cmd/Ctrl+[, Cmd/Ctrl+]) drive
//     back/forward.
//   - Hardcoded "engine link: pending" string replaced by a live status
//     pill driven by the new useEngineHealth hook (see
//     src/hooks/useEngineHealth.ts). Pill colour: green=connected,
//     amber=pending, red=down. The pill carries an accessible title
//     attribute with the most recent error message so hover tells the
//     operator what failed.
//   - Header label bumped to "step 7".

import { useCallback, useEffect, useMemo, useState } from 'react';
import { CommandBar } from '@/components/CommandBar';
import { WorkspaceTabs } from '@/components/WorkspaceTabs';
import { PanelHost } from '@/panels/PanelHost';
import { resolveCode } from '@/router/functionCodes';
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
 * Push a new history entry onto a workspace, trimming any forward-tail
 * (so navigating after a back-step branches the history) and bounding
 * the stack to MAX_HISTORY entries (oldest dropped first). Returns a
 * new Workspace; never mutates `ws`.
 *
 * If the new entry is byte-identical to the current entry (same code +
 * same args in the same order), this is a no-op — we skip the push so
 * a user repeatedly hitting Enter on the same code doesn't bloat the
 * stack.
 */
function pushHistory(ws: Workspace, entry: HistoryEntry): Workspace {
  const cur = ws.history[ws.historyIdx];
  if (
    cur &&
    cur.code === entry.code &&
    cur.args.length === entry.args.length &&
    cur.args.every((a, i) => a === entry.args[i])
  ) {
    // No-op duplicate; just sync the live fields in case they drifted.
    return { ...ws, code: entry.code, args: entry.args };
  }

  // Trim forward-tail if we navigated after a back-step.
  const head = ws.history.slice(0, ws.historyIdx + 1);
  const next = [...head, entry];

  // Bound the stack from the head end so we keep recent history.
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

/**
 * Move the workspace's history pointer by `delta` (-1 = back, +1 = fwd).
 * Returns the workspace unchanged when the move would fall outside the
 * bounds [0, history.length - 1].
 */
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

export default function App() {
  const [workspaces, setWorkspaces] = useState<Workspace[]>(INITIAL_WORKSPACES);
  const [activeId, setActiveId] = useState<string>(INITIAL_WORKSPACES[0]!.id);

  const activeWorkspace = useMemo(
    () => workspaces.find((w) => w.id === activeId) ?? workspaces[0]!,
    [workspaces, activeId]
  );

  // Live engine link status for the title-bar pill.
  const engineHealth = useEngineHealth();

  // Route the active workspace to a new code via the CommandBar's full
  // RouteResult (carries parsed args). Pushes onto the active tab's
  // history stack so back/forward can return here.
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

  // Navigate by raw target string. Accepts either a bare code ("HOME",
  // "LDG") or a code-with-args string ("LDG HybridGold", "TRADE 12345"),
  // matching the same surface CommandBar dispatches through. The router
  // parses head + args identically in both paths, keeping the contract
  // single-sourced.
  const navigate = useCallback(
    (target: string) => {
      const result = resolveCode(target);
      dispatchResult(result);
    },
    [dispatchResult]
  );

  // Back/forward operate on the active tab only.
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

  // Keyboard shortcuts:
  //   Ctrl/Cmd+T  -> new tab
  //   Ctrl/Cmd+W  -> close tab
  //   Alt+Left    -> back        (matches Chrome/Firefox)
  //   Alt+Right   -> forward     (matches Chrome/Firefox)
  //   Ctrl/Cmd+[  -> back        (matches Safari)
  //   Ctrl/Cmd+]  -> forward     (matches Safari)
  //
  // We deliberately swallow the keystroke (preventDefault) so the
  // browser's own history doesn't fight us. The CommandBar listens for
  // Ctrl/Cmd+K independently.
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

      // Alt+Arrow: matches Chrome/Firefox back/forward muscle memory.
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

      // Cmd/Ctrl+[ / ]: matches Safari back/forward muscle memory.
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

  // Pill colour + label keyed off live engine status.
  const pill = engineLinkPill(engineHealth.status);
  const pillTitle =
    engineHealth.status === 'connected'
      ? engineHealth.lastOkAt
        ? `Last OK ${formatAge(engineHealth.lastOkAt)}`
        : 'Engine link OK'
      : engineHealth.lastError
        ? `Engine link ${engineHealth.status}: ${engineHealth.lastError}`
        : `Engine link ${engineHealth.status}`;

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
          {/* Back / forward chevrons. Disabled state is rendered as a
              dimmed glyph so the operator can see the buttons exist
              even when the active tab has nothing to walk back to. */}
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

          {/* Live engine link pill. */}
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

/** Map an engine link status to the pill's display attributes. */
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

/** Format a wall-clock ms as a human "Ns ago" / "Nm ago" string. */
function formatAge(ts: number): string {
  const ageMs = Math.max(0, Date.now() - ts);
  const sec = Math.round(ageMs / 1000);
  if (sec < 60) return `${sec}s ago`;
  const min = Math.round(sec / 60);
  if (min < 60) return `${min}m ago`;
  const hr = Math.round(min / 60);
  return `${hr}h ago`;
}
