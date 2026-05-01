// WorkspaceTabs — strip of named workspaces below the command bar.
//
// Each tab represents an independent workspace currently routed to one
// FunctionCode. Step 1 keeps state in the parent (App.tsx) and renders the
// active workspace's panel inside <PanelHost />. The strip itself is purely
// presentational.

import { PANEL_REGISTRY } from '@/router/functionCodes';
import type { Workspace } from '@/types';

interface Props {
  workspaces: Workspace[];
  activeId: string;
  onActivate: (id: string) => void;
  onClose: (id: string) => void;
  onAdd: () => void;
}

export function WorkspaceTabs({
  workspaces,
  activeId,
  onActivate,
  onClose,
  onAdd,
}: Props) {
  const canClose = workspaces.length > 1;
  return (
    <div
      className="flex items-stretch border-b border-amber-700/40 bg-black"
      role="tablist"
      aria-label="Workspaces"
    >
      <ul className="flex flex-1 items-stretch overflow-x-auto">
        {workspaces.map((ws) => {
          const desc = PANEL_REGISTRY[ws.code];
          const label = ws.label ?? desc.title;
          const isActive = ws.id === activeId;
          return (
            <li key={ws.id} className="flex">
              <button
                type="button"
                role="tab"
                aria-selected={isActive}
                onClick={() => onActivate(ws.id)}
                className={`flex items-center gap-2 border-r border-amber-700/40 px-4 py-2 font-mono text-xs uppercase tracking-wider transition-colors ${
                  isActive
                    ? 'bg-amber-950/60 text-amber-300'
                    : 'text-amber-500 hover:bg-amber-950/30 hover:text-amber-400'
                }`}
              >
                <span className="font-bold">{desc.code}</span>
                <span className="text-amber-500/70">{label}</span>
                {canClose && (
                  <span
                    role="button"
                    tabIndex={-1}
                    aria-label={`Close ${label}`}
                    onClick={(e) => {
                      e.stopPropagation();
                      onClose(ws.id);
                    }}
                    className="ml-1 inline-flex h-4 w-4 items-center justify-center rounded text-amber-700 hover:bg-amber-900 hover:text-amber-300"
                  >
                    &times;
                  </span>
                )}
              </button>
            </li>
          );
        })}
      </ul>
      <button
        type="button"
        onClick={onAdd}
        aria-label="New workspace"
        className="border-l border-amber-700/40 px-4 py-2 font-mono text-base font-bold text-amber-500 hover:bg-amber-950/40 hover:text-amber-300"
      >
        +
      </button>
    </div>
  );
}
