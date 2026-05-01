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
// Workspace       : a single named tab in WorkspaceTabs.
// RouteResult     : router output for a given user-typed string.

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

export interface Workspace {
  /** Stable id for React keys. Generated when a tab is opened. */
  id: string;
  /** The currently routed function code for this tab. */
  code: FunctionCode;
  /** Optional override label; falls back to the panel's title. */
  label?: string;
}

/** Result returned by the router for a given user-typed string. */
export interface RouteResult {
  matched: boolean;
  code: FunctionCode;
  descriptor: PanelDescriptor;
}
