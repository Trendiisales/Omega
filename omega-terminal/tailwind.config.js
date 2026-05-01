/** @type {import('tailwindcss').Config} */
//
// 2026-05-01 colour overhaul:
//   The original Step-1 palette was straight Tailwind amber-on-black, which
//   read as "ugly hazard sign" rather than "trading terminal". This config
//   overrides Tailwind's default `amber` scale with a Bloomberg-style palette
//   derived from src/gui (the existing Omega GUI on :7779), so every existing
//   `text-amber-XXX` / `bg-amber-XXX` / `border-amber-XXX` class in the
//   components automatically picks up the new look without component edits.
//
//   Mapping of the amber scale:
//     amber-50 / 100   bright off-white  (rare, for max-contrast overlays)
//     amber-200 / 300  gold accent        (highlights, active states)
//     amber-400        primary text       (off-white, was bright amber)
//     amber-500        secondary text     (muted blue-gray)
//     amber-600        dim labels         (further muted)
//     amber-700        borders            (subtle white tint, low alpha)
//     amber-800        raised borders     (slightly stronger)
//     amber-900        panel bg           (darker than body)
//     amber-950        deepest bg         (page chrome)
//
//   Plus two semantic colours for market-data display in Step 3 panels:
//     up    green  (#00d97e)  bid / profit / LIVE / position long
//     down  red    (#ff3355)  ask / loss / DOWN / position short
//   Use as: text-up, bg-up/10, border-up/40, etc. (Tailwind opacity syntax).
//
//   Source palette: include/OmegaIndexHtml.hpp ::root, lines 21-28.
//
export default {
  content: ['./index.html', './src/**/*.{ts,tsx}'],
  darkMode: 'class',
  theme: {
    extend: {
      fontFamily: {
        // Mono stack used everywhere by default — terminal aesthetic.
        mono: [
          'JetBrains Mono',
          'Fira Code',
          'IBM Plex Mono',
          'Menlo',
          'Consolas',
          'ui-monospace',
          'SFMono-Regular',
          'monospace',
        ],
      },
      colors: {
        // Override Tailwind's amber palette with the Omega trading-terminal
        // colours. Existing components keep their amber-X classes; the visual
        // result is now blue-gray text on near-black with a gold accent.
        amber: {
          50:  '#f5f7fa',
          100: '#e8edf5',
          200: '#ffe680', // gold-2 — bright accent
          300: '#f5c842', // gold   — primary accent (highlights)
          400: '#e8edf5', // t1     — primary text (off-white)
          500: '#8a9ab8', // t2     — secondary text (muted)
          600: '#4a5878', // t3     — dim labels
          700: '#1f2a3d', // border-strong (used for borders + raised chrome)
          800: '#161e2c', // bg3
          900: '#111720', // bg3 (panel bg)
          950: '#0d1219', // bg2 (deepest)
        },

        // Semantic market-data colours (Step 3 panels: CC, ENG, POS).
        // Match Omega's --green / --red exactly.
        up:   '#00d97e',
        down: '#ff3355',

        // Convenience aliases (kept from the original config but pointed at
        // the new palette so direct references like text-terminal-text work).
        terminal: {
          bg:      '#05080d', // --bg0
          surface: '#0d1219', // --bg2
          border:  '#1f2a3d',
          text:    '#e8edf5', // --t1
          dim:     '#4a5878', // --t3
          accent:  '#f5c842', // --gold
        },

        // Direct Omega palette under semantic names. New code (Step 3+)
        // should prefer these over the amber-X overrides.
        omega: {
          bg0:    '#05080d',
          bg1:    '#090d14',
          bg2:    '#0d1219',
          bg3:    '#111720',
          t1:     '#e8edf5',
          t2:     '#8a9ab8',
          t3:     '#4a5878',
          gold:   '#f5c842',
          gold2:  '#ffe680',
          green:  '#00d97e',
          red:    '#ff3355',
          amber:  '#ff8800',
          blue:   '#2ea8ff',
          purple: '#a47fff',
          cyan:   '#00c8f0',
          teal:   '#00e0b0',
        },
      },
      boxShadow: {
        // Was: amber glow. Now: subtle gold halo for active/focus states,
        // matching Omega's accent treatment.
        glow: '0 0 0 1px rgba(245,200,66,0.25), 0 0 12px rgba(245,200,66,0.15)',
        'glow-up':   '0 0 0 1px rgba(0,217,126,0.3), 0 0 10px rgba(0,217,126,0.15)',
        'glow-down': '0 0 0 1px rgba(255,51,85,0.3), 0 0 10px rgba(255,51,85,0.15)',
      },
    },
  },
  plugins: [],
};
