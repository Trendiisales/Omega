// CellPanel — per-cell drill across Tsmom / Donchian / EmaPullback / TrendRider.
//
// Step 4 deliverable, with a deliberate caveat:
//   STEP4_OPENER §B.1 makes the engine-side `/api/v1/omega/cells` route an
//   item that REQUIRES explicit user approval before any C++ edit (the
//   "engine-side C++ edits require explicit user approval" rule). The
//   route therefore is NOT shipping in this UI cut. Until the engine
//   side lands a `CellSummaryRegistry` + the route + a JSON shape, this
//   panel cannot render real data.
//
// Rather than ship nothing, this panel renders:
//   1. The panel chrome (header, args echo, layout shell) so the UI
//      surface is in place.
//   2. A clear "endpoint pending" notice explaining exactly which
//      engine-side work unlocks it. The wording matches the OPENER's
//      §B.1 text so the gap is documented in two places.
//   3. A preview of the eventual table layout (column headers + a row
//      of em-dashes) so the user can see what the live panel will look
//      like once the engine ships the route.
//
// When the route lands, the panel changes are minimal:
//   - Add a `Cell` interface to `omega-terminal/src/api/types.ts` mirroring
//     the JSON shape the engine settles on.
//   - Add `getCells()` to `omega-terminal/src/api/omega.ts`.
//   - Replace the placeholder body below with a usePanelData fetch + a
//     real table (pattern-matched off LdgPanel/EngPanel).
// The router entry, PanelHost wiring, and command-bar surface are
// already in place, so the engine-side commit is the only thing
// blocking this panel from going live.

interface Props {
  args: string[];
  onNavigate?: (target: string) => void;
}

export function CellPanel({ args }: Props) {
  const cellFilter = (args[0] ?? '').toUpperCase();

  return (
    <section
      className="flex h-full w-full flex-col overflow-y-auto p-6 gap-4"
      aria-label="Cell grid"
    >
      <header>
        <div className="font-mono text-xs uppercase tracking-[0.4em] text-amber-600">
          CELL
        </div>
        <h2 className="mt-1 font-mono text-2xl font-bold text-amber-300">
          Cell Grid
        </h2>
        <p className="mt-2 font-mono text-xs text-amber-500/80">
          Per-cell summary across Tsmom / Donchian / EmaPullback / TrendRider.
          {cellFilter && (
            <>
              {' '}&middot; cell:{' '}
              <span className="text-amber-300">{cellFilter}</span>
            </>
          )}
        </p>
      </header>

      <PendingBanner />

      <PreviewTable />
    </section>
  );
}

/* ──────────────────────────────────────────────────────────────────────── */
/*  Sub-views                                                               */
/* ──────────────────────────────────────────────────────────────────────── */

function PendingBanner() {
  return (
    <div className="border border-amber-800/50 bg-amber-950/20 px-4 py-3">
      <div className="font-mono text-[10px] uppercase tracking-widest text-amber-600">
        Endpoint pending
      </div>
      <p className="mt-1 font-mono text-xs leading-5 text-amber-400">
        The C++ <span className="text-amber-300">/api/v1/omega/cells</span>{' '}
        route is not yet wired. Per STEP4_OPENER §B.1, engine-side work
        requires explicit user approval before any edit. The plan to
        surface for approval is a small{' '}
        <span className="text-amber-300">CellSummaryRegistry</span>{' '}
        mirroring{' '}
        <span className="text-amber-300">OpenPositionRegistry</span>{' '}
        — each engine registers a snapshotter that returns its cells.
      </p>
      <p className="mt-2 font-mono text-xs leading-5 text-amber-500/80">
        Once the route lands, this panel switches from this placeholder
        to a live, sortable, args-filtered table (pattern matches
        LdgPanel). No further UI commit is required at that point — the
        router, PanelHost wiring, and command-bar surface are already in
        place.
      </p>
    </div>
  );
}

function PreviewTable() {
  return (
    <div className="border border-amber-800/50 bg-black/40">
      <div className="flex items-baseline justify-between border-b border-amber-800/40 px-4 py-2">
        <h3 className="font-mono text-xs uppercase tracking-[0.3em] text-amber-500">
          Preview (no live data)
        </h3>
        <span className="font-mono text-[10px] uppercase tracking-widest text-amber-700">
          awaiting /cells route
        </span>
      </div>
      <table className="w-full border-collapse font-mono text-xs">
        <thead>
          <tr className="border-b border-amber-900/60 text-left text-amber-500">
            <th className="px-3 py-2">Engine</th>
            <th className="px-3 py-2">Cell</th>
            <th className="px-3 py-2">Symbol</th>
            <th className="px-3 py-2">TF</th>
            <th className="px-3 py-2 text-right">Open</th>
            <th className="px-3 py-2 text-right">Trades</th>
            <th className="px-3 py-2 text-right">Win Rate</th>
            <th className="px-3 py-2 text-right">P&amp;L</th>
            <th className="px-3 py-2 text-right">Last Signal</th>
          </tr>
        </thead>
        <tbody>
          {Array.from({ length: 4 }).map((_, i) => (
            <tr key={`prev-${i}`} className="border-b border-amber-900/40">
              {Array.from({ length: 9 }).map((__, j) => (
                <td key={j} className="px-3 py-2 text-amber-700">
                  —
                </td>
              ))}
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}
