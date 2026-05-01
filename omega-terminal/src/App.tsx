// App — top-level shell.
//
// Layout (top to bottom):
//   1. Title bar with status pill (placeholder — engine link wired in step 2).
//   2. Command bar with autocomplete.
//   3. WorkspaceTabs.
//   4. Active workspace panel (rendered via PanelHost).
//   5. Footer status line.
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
//   - Header bumped to "step 4".

import { useCallback, useEffect, useMemo, useState } from 'react';
import { CommandBar } from '@/components/CommandBar';
import { WorkspaceTabs } from '@/components/WorkspaceTabs';
import { PanelHost } from '@/panels/PanelHost';
import { resolveCode } from '@/router/functionCodes';
import type { RouteResult, Workspace } from '@/types';

function makeId(): string {
  return `ws-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 7)}`;
}

const INITIAL_WORKSPACES: Workspace[] = [
  { id: makeId(), code: 'HOME', args: [] },
];

export default function App() {
  const [workspaces, setWorkspaces] = useState<Workspace[]>(INITIAL_WORKSPACES);
  const [activeId, setActiveId] = useState<string>(INITIAL_WORKSPACES[0]!.id);

  const activeWorkspace = useMemo(
    () => workspaces.find((w) => w.id === activeId) ?? workspaces[0]!,
    [workspaces, activeId]
  );

  // Route the active workspace to a new code via the CommandBar's full
  // RouteResult (carries parsed args).
  const dispatchResult = useCallback(
    (result: RouteResult) => {
      setWorkspaces((prev) =>
        prev.map((w) =>
          w.id === activeId
            ? { ...w, code: result.code, args: result.args }
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

  const addWorkspace = useCallback(() => {
    const ws: Workspace = { id: makeId(), code: 'HOME', args: [] };
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

  // Ctrl/Cmd+T new tab, Ctrl/Cmd+W close tab.
  useEffect(() => {
    function onKey(e: KeyboardEvent) {
      const mod = e.ctrlKey || e.metaKey;
      if (!mod) return;
      const key = e.key.toLowerCase();
      if (key === 't') {
        e.preventDefault();
        addWorkspace();
      } else if (key === 'w') {
        e.preventDefault();
        closeWorkspace(activeId);
      }
    }
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [addWorkspace, closeWorkspace, activeId]);

  return (
    <div className="flex h-full w-full flex-col bg-black text-amber-400">
      {/* Title bar */}
      <header className="flex items-center justify-between border-b border-amber-700/60 bg-black px-4 py-2">
        <div className="flex items-baseline gap-3">
          <span className="font-mono text-sm font-bold tracking-[0.3em] text-amber-300">
            OMEGA
          </span>
          <span className="font-mono text-[10px] uppercase tracking-widest text-amber-600">
            terminal &middot; v0.1 &middot; step 4
          </span>
        </div>
        <div className="flex items-center gap-3 font-mono text-[10px] uppercase tracking-widest">
          <span className="flex items-center gap-1.5">
            <span
              aria-hidden="true"
              className="inline-block h-2 w-2 rounded-full bg-amber-500"
            />
            <span className="text-amber-500">engine link: pending</span>
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
        </span>
        <span>Ctrl+K cmd  &middot;  Ctrl+T new  &middot;  Ctrl+W close</span>
      </footer>
    </div>
  );
}
