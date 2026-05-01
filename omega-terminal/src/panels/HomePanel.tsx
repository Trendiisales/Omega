// HomePanel — landing tile grid grouped by category.
//
// Each tile is a button that routes the active workspace to that code.
// `step === 1` codes render in normal amber; not-yet-shipped codes are
// dimmed and tagged with their target step so the user immediately
// sees the build trajectory.

import { PANEL_REGISTRY, panelsByGroup } from '@/router/functionCodes';
import type { FunctionCode, PanelDescriptor } from '@/types';

interface Props {
  onSelect: (code: FunctionCode) => void;
}

export function HomePanel({ onSelect }: Props) {
  const desc = PANEL_REGISTRY.HOME;
  const groups = panelsByGroup();

  return (
    <section
      className="flex h-full w-full justify-center overflow-y-auto p-8"
      aria-label="Home panel"
    >
      <div className="w-full max-w-5xl">
        <div className="text-center">
          <div className="font-mono text-xs uppercase tracking-[0.4em] text-amber-600">
            {desc.code}
          </div>
          <h2 className="mt-2 font-mono text-3xl font-bold text-amber-300">
            {desc.title}
          </h2>
          <p className="mx-auto mt-4 max-w-xl text-sm leading-6 text-amber-500/80">
            {desc.description}
          </p>
        </div>

        <Section title="Omega" subtitle="Engine, positions, ledger" tiles={groups.omega} onSelect={onSelect} />
        <Section title="Market" subtitle="OpenBB-derived data + screening" tiles={groups.market} onSelect={onSelect} />

        <p className="mt-12 text-center text-[10px] uppercase tracking-widest text-amber-700">
          Tip: type a code in the command bar (Ctrl+K), then Enter
        </p>
      </div>
    </section>
  );
}

interface SectionProps {
  title: string;
  subtitle: string;
  tiles: PanelDescriptor[];
  onSelect: (code: FunctionCode) => void;
}

function Section({ title, subtitle, tiles, onSelect }: SectionProps) {
  return (
    <div className="mt-10">
      <div className="flex items-baseline justify-between border-b border-amber-700/40 pb-2">
        <h3 className="font-mono text-sm font-bold uppercase tracking-[0.3em] text-amber-300">
          {title}
        </h3>
        <span className="font-mono text-[10px] uppercase tracking-widest text-amber-600">
          {subtitle}
        </span>
      </div>
      <ul className="mt-4 grid grid-cols-1 gap-2 sm:grid-cols-2 lg:grid-cols-3">
        {tiles.map((p) => (
          <li key={p.code}>
            <Tile descriptor={p} onSelect={onSelect} />
          </li>
        ))}
      </ul>
    </div>
  );
}

interface TileProps {
  descriptor: PanelDescriptor;
  onSelect: (code: FunctionCode) => void;
}

function Tile({ descriptor: p, onSelect }: TileProps) {
  const live = p.step === 1;
  return (
    <button
      type="button"
      onClick={() => onSelect(p.code)}
      className={`flex w-full items-baseline gap-3 rounded border px-3 py-2.5 text-left transition-colors focus:outline-none focus:ring-1 focus:ring-amber-400 ${
        live
          ? 'border-amber-700/60 bg-amber-950/40 hover:border-amber-500 hover:bg-amber-900/40'
          : 'border-amber-900/40 bg-black hover:border-amber-700 hover:bg-amber-950/30'
      }`}
    >
      <span
        className={`w-14 font-mono text-sm font-bold ${
          live ? 'text-amber-300' : 'text-amber-500/70'
        }`}
      >
        {p.code}
      </span>
      <span
        className={`font-mono text-xs ${
          live ? 'text-amber-400' : 'text-amber-500/60'
        }`}
      >
        {p.title}
      </span>
      {!live && (
        <span className="ml-auto rounded border border-amber-800/50 px-1.5 py-0.5 font-mono text-[9px] uppercase tracking-widest text-amber-600">
          step {p.step}
        </span>
      )}
    </button>
  );
}
