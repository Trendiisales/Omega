// Omega Terminal — shared types
//
// FunctionCode    : the short, all-caps mnemonic the user types in the
//                   command bar. Bloomberg-style. The full set is locked
//                   to the surface enumerated in
//                   docs/SESSION_2026-05-01_HANDOFF.md (steps 1-6).
// PanelDescriptor : metadata that the router uses to render a panel and
//                   that autocomplete uses to surface suggestions. The
//                   `step` field marks which build step makes the panel
//                   live (1 = shipped now; >1 = ComingSoon placeholder).
// PanelGroup      : how HomePanel groups codes for visual scanning.
// Workspace       : a single named tab in WorkspaceTabs. Step 3 adds
//                   `args` so commands like `POS HBG` carry positional
//                   arguments into the active panel.
// RouteResult     : router output for a given user-typed string. Step 3
//                   adds `args` (the trailing tokens after the code) so
//                   the dispatcher can plumb them through to the panel.
//
// Step 7 update:
//   - Workspace gains `history: HistoryEntry[]` and `historyIdx: number`
//     so each tab keeps its own back/forward stack. The router never
//     mutates `history` directly; the dispatcher in App.tsx pushes a new
//     entry whenever a non-history navigation lands and trims any
//     forward-tail when the user navigates after going back.
//   - New `EngineLinkStatus` enumerates the live-status pill states
//     surfaced by the title bar (replaces the previous hardcoded
//     "engine link: pending" string).

export type FunctionCode =
  // Shell + meta
  | 'HOME'
  | 'HELP'
  // Omega — engine, positions, ledger (steps 3-4)
  | 'CC'
  | 'ENG'
  | 'POS'
  | 'LDG'
  | 'TRADE'
  | 'CELL'
  // Market data (BB-derived) — steps 5-6
  | 'INTEL'
  | 'CURV'
  | 'WEI'
  | 'MOV'
  | 'OMON'
  | 'FA'
  | 'KEY'
  | 'DVD'
  | 'EE'
  | 'NI'
  | 'GP'
  | 'QR'
  | 'HP'
  | 'DES'
  | 'FXC'
  | 'CRYPTO'
  | 'WATCH';

/** Build step at which a panel becomes live. 1 = shipped in this commit. */
export type BuildStep = 1 | 3 | 4 | 5 | 6;

/** Coarse group used for HomePanel visual sectioning and HelpPanel ordering. */
export type PanelGroup = 'shell' | 'omega' | 'market' | 'help';

export interface PanelDescriptor {
  /** Canonical, all-caps function code. */
  code: FunctionCode;
  /** Short human title shown in tabs and autocomplete. */
  title: string;
  /** One-line description shown in autocomplete and HELP. */
  description: string;
  /** Build step that makes this panel live. */
  step: BuildStep;
  /** Group used for HomePanel sectioning. */
  group: PanelGroup;
  /** Optional aliases (also all-caps) the user can type to reach this panel. */
  aliases?: string[];
}

/**
 * One entry in a workspace's back/forward history.
 *
 * Step 7 introduces this so the user can navigate ENG -> LDG -> TRADE
 * and walk back to ENG without losing other tabs' state. The shape is
 * intentionally identical to the per-workspace `code`/`args` pair so
 * pushing the current location onto the stack is a structural copy.
 */
export interface HistoryEntry {
  code: FunctionCode;
  args: string[];
}

export interface Workspace {
  /** Stable id for React keys. Generated when a tab is opened. */
  id: string;
  /** The currently routed function code for this tab. */
  code: FunctionCode;
  /**
   * Positional arguments parsed from the command bar string.
   * Step 3 introduces this so commands like `POS HBG` or `CC EURUSD`
   * route to the same panel but carry context through to it.
   * Empty array when the user just typed the code.
   */
  args: string[];
  /**
   * Per-tab back/forward stack. The entry at `history[historyIdx]`
   * always equals the current `{code, args}` pair. `historyIdx` > 0
   * means a back-step is available; `historyIdx < history.length - 1`
   * means a forward-step is available. Bounded to 50 entries to keep
   * the in-memory shape predictable.
   */
  history: HistoryEntry[];
  /** Index of the current location within `history`. */
  historyIdx: number;
  /** Optional override label; falls back to the panel's title. */
  label?: string;
}

/**
 * Result returned by the router for a given user-typed string.
 * `args` are the remaining whitespace-separated tokens after the code,
 * preserved in their original (uppercase) form so panels can pattern-match
 * (engine names, symbols, etc.).
 */
export interface RouteResult {
  matched: boolean;
  code: FunctionCode;
  descriptor: PanelDescriptor;
  args: string[];
}

/**
 * Live status of the engine REST link, surfaced in the title bar pill.
 *
 *   - 'connected' : a recent /engines call returned 2xx within the
 *                   poll window. Pill is green.
 *   - 'pending'   : no successful response yet (cold start, or the
 *                   first poll is in flight). Pill is amber. This is
 *                   the initial state and also the state during a
 *                   transient retry.
 *   - 'down'      : the most recent poll failed (HTTP error, network
 *                   error, timeout). Pill is red. Polling continues
 *                   so the pill auto-recovers when the engine returns.
 */
export type EngineLinkStatus = 'connected' | 'pending' | 'down';
