// Omega Terminal — function-code router
//
// Step 1 contract (this file is the canonical surface):
//   - The router is a pure lookup table over FunctionCode -> PanelDescriptor.
//   - Resolution is case-insensitive and trims surrounding whitespace.
//   - Aliases route to the same panel (e.g. POSITIONS -> POS).
//   - Unknown codes resolve to HELP with `matched: false`, which the UI
//     surfaces as a "did you mean…" hint.
//
// Code surface is locked to docs/SESSION_2026-05-01_HANDOFF.md, steps 1-6:
//   - HOME, HELP                          (shell, step 1, shipped)
//   - CC, ENG, POS                        (omega,  step 3, ComingSoon)
//   - LDG, TRADE, CELL                    (omega,  step 4, ComingSoon)
//   - INTEL, CURV, WEI, MOV               (market, step 5, ComingSoon)
//   - OMON, FA, KEY, DVD, EE, NI, GP, QR,
//     HP, DES, FXC, CRYPTO, WATCH         (market, step 6, ComingSoon)
//
// Step 2 will add: arg parsing (e.g. "CC EURUSD"), per-panel state, a
// pluggable registry so panels can self-register, and step 2 itself
// also adds Vite proxy + IPC contract — but the registry already lists
// every code so autocomplete shows the full surface from day 1.

import type { FunctionCode, PanelDescriptor, RouteResult } from '@/types';

export const PANEL_REGISTRY: Record<FunctionCode, PanelDescriptor> = {
  // -------------------------------------------------------------- shell
  HOME: {
    code: 'HOME',
    title: 'Home',
    description: 'Workspace landing page. Type a function code or pick a tile.',
    step: 1,
    group: 'shell',
    aliases: ['START', 'MENU'],
  },
  HELP: {
    code: 'HELP',
    title: 'Help',
    description: 'Cheat-sheet of every function code, alias, and keyboard shortcut.',
    step: 1,
    group: 'help',
    aliases: ['?', 'H'],
  },

  // -------------------------------------------------------- omega step 3
  CC: {
    code: 'CC',
    title: 'Command Center',
    description: 'Live engine status, signals, positions overview, equity strip.',
    step: 3,
    group: 'omega',
    aliases: ['DASH', 'DASHBOARD'],
  },
  ENG: {
    code: 'ENG',
    title: 'Engines',
    description: 'Per-engine drill: status, last signal, P&L attribution.',
    step: 3,
    group: 'omega',
    aliases: ['ENGINE', 'ENGINES'],
  },
  POS: {
    code: 'POS',
    title: 'Positions',
    description: 'Open positions table: size, entry, MFE, MAE, unrealized P&L, exposure.',
    step: 3,
    group: 'omega',
    aliases: ['POSITIONS', 'PORT', 'PORTFOLIO'],
  },

  // -------------------------------------------------------- omega step 4
  LDG: {
    code: 'LDG',
    title: 'Trade Ledger',
    description: 'Filterable trade ledger across all engines and symbols.',
    step: 4,
    group: 'omega',
    aliases: ['LEDGER', 'TRADES'],
  },
  TRADE: {
    code: 'TRADE',
    title: 'Trade Drill-Down',
    description: 'Per-trade timeline: entry, fills, MFE/MAE, exit reason, signal context.',
    step: 4,
    group: 'omega',
    aliases: ['TRD'],
  },
  CELL: {
    code: 'CELL',
    title: 'Cell Grid',
    description: 'Per-cell drill across Tsmom, Donchian, EmaPullback, TrendRider.',
    step: 4,
    group: 'omega',
    aliases: ['CELLS', 'GRID'],
  },

  // ------------------------------------------------------- market step 5
  INTEL: {
    code: 'INTEL',
    title: 'Intelligence Screener',
    description: 'Composite screener over fundamentals + technicals + sentiment.',
    step: 5,
    group: 'market',
    aliases: ['SCREEN', 'SCREENER'],
  },
  CURV: {
    code: 'CURV',
    title: 'Curves',
    description: 'Yield curves, term structure, butterflies, real-vs-nominal.',
    step: 5,
    group: 'market',
    aliases: ['CURVE', 'YIELDS'],
  },
  WEI: {
    code: 'WEI',
    title: 'World Equity Indices',
    description: 'Global index dashboard with returns, sectors, correlations.',
    step: 5,
    group: 'market',
    aliases: ['INDICES', 'WORLD'],
  },
  MOV: {
    code: 'MOV',
    title: 'Movers',
    description: 'Top gainers / losers / unusual volume across selected universes.',
    step: 5,
    group: 'market',
    aliases: ['MOVERS', 'GAINERS'],
  },

  // ------------------------------------------------------- market step 6
  OMON: {
    code: 'OMON',
    title: 'Options Monitor',
    description: 'Option chain, IV surface, skew, term structure, flow.',
    step: 6,
    group: 'market',
    aliases: ['OPT', 'OPTIONS'],
  },
  FA: {
    code: 'FA',
    title: 'Financial Analysis',
    description: 'Income / balance / cash-flow trends with peer comparison.',
    step: 6,
    group: 'market',
    aliases: ['FUNDAMENTALS'],
  },
  KEY: {
    code: 'KEY',
    title: 'Key Stats',
    description: 'Key ratios, valuation multiples, growth, profitability snapshot.',
    step: 6,
    group: 'market',
    aliases: ['STATS'],
  },
  DVD: {
    code: 'DVD',
    title: 'Dividends',
    description: 'Dividend history, ex-dates, yield, payout sustainability.',
    step: 6,
    group: 'market',
    aliases: ['DIV', 'DIVIDEND'],
  },
  EE: {
    code: 'EE',
    title: 'Earnings Estimates',
    description: 'Consensus EPS / revenue, surprise history, calendar.',
    step: 6,
    group: 'market',
    aliases: ['EARNINGS'],
  },
  NI: {
    code: 'NI',
    title: 'News',
    description: 'News stream filtered by symbol, sector, or topic.',
    step: 6,
    group: 'market',
    aliases: ['NEWS'],
  },
  GP: {
    code: 'GP',
    title: 'Graph Price',
    description: 'Interactive price chart with overlays, indicators, comparisons.',
    step: 6,
    group: 'market',
    aliases: ['GRAPH', 'CHART'],
  },
  QR: {
    code: 'QR',
    title: 'Quote Recap',
    description: 'Multi-quote tape: bid/ask/last, day range, %chg across a list.',
    step: 6,
    group: 'market',
    aliases: ['QUOTE'],
  },
  HP: {
    code: 'HP',
    title: 'Historical Price',
    description: 'OHLCV table with adjustable interval and date range.',
    step: 6,
    group: 'market',
    aliases: ['HISTORY', 'HIST'],
  },
  DES: {
    code: 'DES',
    title: 'Description',
    description: 'Company description, business segments, key people, addresses.',
    step: 6,
    group: 'market',
    aliases: ['DESC'],
  },
  FXC: {
    code: 'FXC',
    title: 'FX Cross',
    description: 'Currency cross rates, majors / minors, vol & carry tables.',
    step: 6,
    group: 'market',
    aliases: ['FX', 'FOREX'],
  },
  CRYPTO: {
    code: 'CRYPTO',
    title: 'Crypto',
    description: 'Crypto majors snapshot, dominance, funding, liquidations.',
    step: 6,
    group: 'market',
    aliases: ['BTC', 'COIN'],
  },
  WATCH: {
    code: 'WATCH',
    title: 'Watch List',
    description: 'INTEL rules run nightly across S&P 500 + NDX components; flagged candidates.',
    step: 6,
    group: 'market',
    aliases: ['WATCHLIST'],
  },
};

/**
 * Ordered list — used by autocomplete + HelpPanel.
 * Order: HOME, HELP, then omega (step 3 -> 4), then market (step 5 -> 6).
 */
export const PANEL_LIST: PanelDescriptor[] = [
  PANEL_REGISTRY.HOME,
  PANEL_REGISTRY.HELP,
  // omega step 3
  PANEL_REGISTRY.CC,
  PANEL_REGISTRY.ENG,
  PANEL_REGISTRY.POS,
  // omega step 4
  PANEL_REGISTRY.LDG,
  PANEL_REGISTRY.TRADE,
  PANEL_REGISTRY.CELL,
  // market step 5
  PANEL_REGISTRY.INTEL,
  PANEL_REGISTRY.CURV,
  PANEL_REGISTRY.WEI,
  PANEL_REGISTRY.MOV,
  // market step 6
  PANEL_REGISTRY.OMON,
  PANEL_REGISTRY.FA,
  PANEL_REGISTRY.KEY,
  PANEL_REGISTRY.DVD,
  PANEL_REGISTRY.EE,
  PANEL_REGISTRY.NI,
  PANEL_REGISTRY.GP,
  PANEL_REGISTRY.QR,
  PANEL_REGISTRY.HP,
  PANEL_REGISTRY.DES,
  PANEL_REGISTRY.FXC,
  PANEL_REGISTRY.CRYPTO,
  PANEL_REGISTRY.WATCH,
];

/** Build an alias -> code map at module load. */
const ALIAS_INDEX: Map<string, FunctionCode> = (() => {
  const m = new Map<string, FunctionCode>();
  for (const desc of PANEL_LIST) {
    m.set(desc.code.toUpperCase(), desc.code);
    for (const a of desc.aliases ?? []) {
      m.set(a.toUpperCase(), desc.code);
    }
  }
  return m;
})();

/** Resolve a raw user-typed string to a panel descriptor. */
export function resolveCode(raw: string): RouteResult {
  const key = raw.trim().toUpperCase();
  const code = ALIAS_INDEX.get(key);
  if (code) {
    return { matched: true, code, descriptor: PANEL_REGISTRY[code] };
  }
  return {
    matched: false,
    code: 'HELP',
    descriptor: PANEL_REGISTRY.HELP,
  };
}

/**
 * Suggestion list for autocomplete given a partial query.
 *
 * Scoring:
 *   - 0: prefix match on canonical code
 *   - 1: prefix match on any alias
 *   - 2: substring match in title
 * Within a tier, codes that are already shipped (step 1) sort before
 * not-yet-shipped codes; ties broken alphabetically.
 */
export function suggestCodes(query: string, limit = 8): PanelDescriptor[] {
  const q = query.trim().toUpperCase();
  if (q.length === 0) return PANEL_LIST.slice(0, limit);

  const scored = PANEL_LIST.map((desc): { desc: PanelDescriptor; score: number } => {
    const codeUp = desc.code.toUpperCase();
    const titleUp = desc.title.toUpperCase();
    let score = 999;
    if (codeUp.startsWith(q)) score = 0;
    else if ((desc.aliases ?? []).some((a) => a.toUpperCase().startsWith(q))) score = 1;
    else if (titleUp.includes(q)) score = 2;
    return { desc, score };
  })
    .filter((row) => row.score < 999)
    .sort((a, b) => {
      if (a.score !== b.score) return a.score - b.score;
      const aShipped = a.desc.step === 1 ? 0 : 1;
      const bShipped = b.desc.step === 1 ? 0 : 1;
      if (aShipped !== bShipped) return aShipped - bShipped;
      return a.desc.code.localeCompare(b.desc.code);
    });

  return scored.slice(0, limit).map((row) => row.desc);
}

/** Group codes for HomePanel sectioning. Stable order within each group. */
export function panelsByGroup(): Record<'omega' | 'market', PanelDescriptor[]> {
  return {
    omega: PANEL_LIST.filter((p) => p.group === 'omega'),
    market: PANEL_LIST.filter((p) => p.group === 'market'),
  };
}
