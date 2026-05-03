// HomePanel — landing tile grid grouped by category, with a live status
// strip at the top.
//
// Step 7 update:
//   - Status strip (4 cards) added above the section grid: engines online,
//     open positions, closed trades, last activity. Backed by the same
//     /engines, /positions, /ledger routes the deeper panels use, polled
//     every 10 s. Cards never block the tile grid — they render skeleton
//     dashes while loading and graceful "—" on error so Home is always
//     reachable as a router even if the engine link is down.
//   - All 24 panels are now live (Step 7 = full surface). The legacy
//     dim-and-tag-with-"step N" logic that made every non-Step-1 tile
//     look "not yet shipped" has been retired. Tiles render in normal
//     amber across the board.
//   - MARKET section subtitle updated from "OpenBB-derived data + screening"
//     to "Live market data via Yahoo + FRED" to match the MarketDataProxy
//     substrate that replaced OpenBb in Step 7.
//
// Each tile is a button that routes the active workspace to that code.

import { useCallback } from 'react';
import { getEngines, getLedger, getPositions } from '@/api/omega';
import type { Engine, LedgerEntry, Position } from '@/api/types';
import { usePanelData } from '@/hooks/usePanelData';
import { PANEL_REGISTRY, panelsByGroup } from '@/router/functionCodes';
import type { FunctionCode, PanelDescriptor } from '@/types';

interface Props {
  onSelect: (code: FunctionCode) => void;
}

const STATUS_POLL_MS = 10000;

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

        <StatusStrip />

        <Section
          title="Omega"
          subtitle="Engine, positions, ledger"
          tiles={groups.omega}
          onSelect={onSelect}
        />
        <Section
          title="Market"
          subtitle="Live market data via Yahoo + FRED"
          tiles={groups.market}
          onSelect={onSelect}
        />

        <p className="mt-12 text-center text-[10px] uppercase tracking-widest text-amber-700">
          Tip: type a code in the command bar (Ctrl+K), then Enter
        </p>
      </div>
    </section>
  );
}

/* ──────────────────────────────────────────────────────────────────────── */
/*  Live status strip                                                       */
/* ──────────────────────────────────────────────────────────────────────── */

function StatusStrip() {
  const enginesFetcher = useCallback(
    (s: AbortSignal) => getEngines({ signal: s }),
    [],
  );
  const positionsFetcher = useCallback(
    (s: AbortSignal) => getPositions({ signal: s }),
    [],
  );
  // Ask for a small page — we only need the count + the most recent row.
  const ledgerFetcher = useCallback(
    (s: AbortSignal) => getLedger({ limit: 200 }, { signal: s }),
    [],
  );

  const engines = usePanelData<Engine[]>(enginesFetcher, [], {
    pollMs: STATUS_POLL_MS,
  });
  const positions = usePanelData<Position[]>(positionsFetcher, [], {
    pollMs: STATUS_POLL_MS,
  });
  const ledger = usePanelData<LedgerEntry[]>(ledgerFetcher, [], {
    pollMs: STATUS_POLL_MS,
  });

  const engineRows = readData(engines.state.data) ?? [];
  const enginesOnline = engineRows.filter((e) => e.enabled).length;
  const enginesTotal = engineRows.length;
  const enginesLabel =
    engines.state.status === 'loading' && engineRows.length === 0
      ? '—'
      : engines.state.status === 'err' && engineRows.length === 0
        ? '—'
        : `${enginesOnline}/${enginesTotal}`;

  const positionRows = readData(positions.state.data) ?? [];
  const positionsLabel =
    positions.state.status === 'loading' && positionRows.length === 0
      ? '—'
      : positions.state.status === 'err' && positionRows.length === 0
        ? '—'
        : String(positionRows.length);

  const ledgerRows = readData(ledger.state.data) ?? [];
  const ledgerLabel =
    ledger.state.status === 'loading' && ledgerRows.length === 0
      ? '—'
      : ledger.state.status === 'err' && ledgerRows.length === 0
        ? '—'
        : String(ledgerRows.length);
  const lastTradeTs = mostRecentExitTs(ledgerRows);
  const lastActivityLabel =
    lastTradeTs > 0 ? formatRelative(lastTradeTs) : '—';

  return (
    <div className="mt-8 grid grid-cols-2 gap-3 sm:grid-cols-4">
      <StatusCard
        label="Engines online"
        value={enginesLabel}
        sub={enginesTotal > 0 ? `${enginesTotal} registered` : 'no data'}
      />
      <StatusCard
        label="Open positions"
        value={positionsLabel}
        sub={positionsLabel === '—' ? 'no data' : 'live snapshot'}
      />
      <StatusCard
        label="Closed trades"
        value={ledgerLabel}
        sub={ledgerLabel === '—' ? 'no data' : 'last 200'}
      />
      <StatusCard
        label="Last activity"
        value={lastActivityLabel}
        sub={lastTradeTs > 0 ? formatUtcShort(lastTradeTs) : 'no trades'}
      />
    </div>
  );
}

function StatusCard({
  label,
  value,
  sub,
}: {
  label: string;
  value: string;
  sub: string;
}) {
  return (
    <div className="border border-amber-800/50 bg-black/40 px-4 py-3">
      <div className="font-mono text-[10px] uppercase tracking-widest text-amber-600">
        {label}
      </div>
      <div className="mt-1 font-mono text-xl font-bold text-amber-300">
        {value}
      </div>
      <div className="mt-0.5 font-mono text-[10px] uppercase tracking-widest text-amber-700">
        {sub}
      </div>
    </div>
  );
}

/** Pull T out of the discriminated union safely; returns null on err. */
function readData<T>(d: T | null | undefined): T | null {
  return d ?? null;
}

function mostRecentExitTs(rows: LedgerEntry[]): number {
  let best = 0;
  for (const r of rows) {
    if (r.exit_ts > best) best = r.exit_ts;
  }
  return best;
}

/* ──────────────────────────────────────────────────────────────────────── */
/*  Tile grid                                                               */
/* ──────────────────────────────────────────────────────────────────────── */

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
  // Step 7: every panel in PANEL_REGISTRY ships in this build. The legacy
  // `live = (p.step === 1)` flag that dimmed not-yet-shipped tiles is
  // retired — keeping it would mark every non-HOME/HELP tile as pending
  // even though they all run live data.
  return (
    <button
      type="button"
      onClick={() => onSelect(p.code)}
      className="flex w-full items-baseline gap-3 rounded border border-amber-700/60 bg-amber-950/40 px-3 py-2.5 text-left transition-colors hover:border-amber-500 hover:bg-amber-900/40 focus:outline-none focus:ring-1 focus:ring-amber-400"
    >
      <span className="w-14 font-mono text-sm font-bold text-amber-300">
        {p.code}
      </span>
      <span className="font-mono text-xs text-amber-400">{p.title}</span>
    </button>
  );
}

/* ──────────────────────────────────────────────────────────────────────── */
/*  Formatters                                                              */
/* ──────────────────────────────────────────────────────────────────────── */

function formatRelative(unixMs: number): string {
  if (!unixMs || unixMs <= 0) return '—';
  const ageMs = Date.now() - unixMs;
  if (ageMs < 0) return 'just now';
  const sec = Math.floor(ageMs / 1000);
  if (sec < 60) return `${sec}s ago`;
  const min = Math.floor(sec / 60);
  if (min < 60) return `${min}m ago`;
  const hr = Math.floor(min / 60);
  if (hr < 24) return `${hr}h ago`;
  const day = Math.floor(hr / 24);
  if (day < 30) return `${day}d ago`;
  return formatUtcShort(unixMs);
}

function formatUtcShort(unixMs: number): string {
  if (!unixMs || unixMs <= 0) return '—';
  const d = new Date(unixMs);
  const yyyy = d.getUTCFullYear();
  const mm = String(d.getUTCMonth() + 1).padStart(2, '0');
  const dd = String(d.getUTCDate()).padStart(2, '0');
  return `${yyyy}-${mm}-${dd}`;
}
