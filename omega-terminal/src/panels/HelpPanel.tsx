// HelpPanel — exhaustive cheat-sheet for every code, alias, and key.

import { PANEL_LIST, PANEL_REGISTRY } from '@/router/functionCodes';
import type { PanelDescriptor } from '@/types';

const GROUP_LABEL: Record<string, string> = {
  shell: 'Shell',
  omega: 'Omega',
  market: 'Market',
  help: 'Help',
};

export function HelpPanel() {
  const desc = PANEL_REGISTRY.HELP;
  // Keep table grouped by section, in PANEL_LIST order.
  const grouped = (() => {
    const buckets: Array<{ group: string; rows: PanelDescriptor[] }> = [];
    for (const p of PANEL_LIST) {
      const last = buckets[buckets.length - 1];
      if (!last || last.group !== p.group) {
        buckets.push({ group: p.group, rows: [p] });
      } else {
        last.rows.push(p);
      }
    }
    return buckets;
  })();

  return (
    <section
      className="flex h-full w-full justify-center overflow-y-auto p-8"
      aria-label="Help panel"
    >
      <div className="w-full max-w-4xl">
        <div className="font-mono text-xs uppercase tracking-[0.4em] text-amber-600">
          {desc.code}
        </div>
        <h2 className="mt-2 font-mono text-3xl font-bold text-amber-300">
          {desc.title}
        </h2>
        <p className="mt-4 text-sm leading-6 text-amber-500/80">
          Type a function code in the command bar at the top, then press Enter
          to route the active workspace to that panel. Use Tab or arrow keys to
          accept an autocomplete suggestion.
        </p>

        <h3 className="mt-8 font-mono text-xs uppercase tracking-[0.3em] text-amber-600">
          Function codes
        </h3>
        <table className="mt-3 w-full border-collapse font-mono text-sm">
          <thead>
            <tr className="border-b border-amber-700/40 text-left text-amber-500">
              <th className="py-2 pr-3">Code</th>
              <th className="py-2 pr-3">Title</th>
              <th className="py-2 pr-3">Step</th>
              <th className="py-2 pr-3">Aliases</th>
              <th className="py-2">Description</th>
            </tr>
          </thead>
          <tbody>
            {grouped.map((bucket) => (
              <FragmentForGroup key={bucket.group} group={bucket.group} rows={bucket.rows} />
            ))}
          </tbody>
        </table>

        <h3 className="mt-8 font-mono text-xs uppercase tracking-[0.3em] text-amber-600">
          Keyboard
        </h3>
        <ul className="mt-3 space-y-1 font-mono text-sm text-amber-400">
          <li>
            <span className="text-amber-300">Ctrl+K</span> &mdash; focus command
            bar
          </li>
          <li>
            <span className="text-amber-300">↑ / ↓</span> &mdash; move through
            autocomplete suggestions
          </li>
          <li>
            <span className="text-amber-300">Tab</span> &mdash; accept highlighted
            suggestion (without firing)
          </li>
          <li>
            <span className="text-amber-300">Enter</span> &mdash; route active
            workspace to selected code
          </li>
          <li>
            <span className="text-amber-300">Esc</span> &mdash; close
            autocomplete
          </li>
          <li>
            <span className="text-amber-300">Ctrl+T</span> &mdash; open new
            workspace tab
          </li>
          <li>
            <span className="text-amber-300">Ctrl+W</span> &mdash; close current
            workspace tab (must keep at least one)
          </li>
        </ul>

        <h3 className="mt-8 font-mono text-xs uppercase tracking-[0.3em] text-amber-600">
          Build steps
        </h3>
        <p className="mt-3 font-mono text-sm text-amber-500/80">
          Step 1 ships the shell only (HOME, HELP). Steps 3-4 light up the
          Omega panels. Steps 5-6 light up the Market panels. Step 7 is the
          single-commit cutover that retires <code>src/gui/</code>.
        </p>
      </div>
    </section>
  );
}

interface FragmentForGroupProps {
  group: string;
  rows: PanelDescriptor[];
}

function FragmentForGroup({ group, rows }: FragmentForGroupProps) {
  return (
    <>
      <tr>
        <td
          colSpan={5}
          className="bg-amber-950/30 py-1.5 px-2 font-mono text-[10px] uppercase tracking-widest text-amber-500"
        >
          {GROUP_LABEL[group] ?? group}
        </td>
      </tr>
      {rows.map((p) => {
        const live = p.step === 1;
        return (
          <tr key={p.code} className="border-b border-amber-900/40">
            <td className={`py-2 pr-3 font-bold ${live ? 'text-amber-300' : 'text-amber-400'}`}>
              {p.code}
            </td>
            <td className="py-2 pr-3 text-amber-400">{p.title}</td>
            <td className="py-2 pr-3 text-amber-500/70">
              {live ? 'live' : `step ${p.step}`}
            </td>
            <td className="py-2 pr-3 text-amber-500/70">
              {(p.aliases ?? []).join(', ') || '—'}
            </td>
            <td className="py-2 text-amber-500/80">{p.description}</td>
          </tr>
        );
      })}
    </>
  );
}
