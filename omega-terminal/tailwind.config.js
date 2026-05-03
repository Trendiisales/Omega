/** @type {import('tailwindcss').Config} */
//
// Tailwind config — Omega palette remap.
//
// The Step-1 commit kept Tailwind's stock amber palette, which renders as
// bright orange-yellow ("terminal amber"). The 2026-05-01 redesign of
// `src/index.css` documented an intent to replace that with the Bloomberg-
// style palette from the existing Omega GUI on :7779
// (source: include/OmegaIndexHtml.hpp), but the remap itself never landed
// in the Tailwind config. As a result every panel that wrote
// `text-amber-300`, `border-amber-700/40`, `bg-amber-950/40`, etc., still
// rendered in stock amber.
//
// This config performs that remap centrally so panel components don't need
// to change. Each `amber-X` step is replaced with the closest semantic
// position in the Omega palette:
//
//   amber-200 -> #ffe680  gold2  (brightest accent)
//   amber-300 -> #f5c842  gold   (primary accent: panel titles, big P&L
//                                  numbers, engine names, active values)
//   amber-400 -> #e8edf5  t1     (off-white primary text — most cells)
//   amber-500 -> #8a9ab8  t2     (muted blue-gray — column headers, labels)
//   amber-600 -> #6a7898  ----   (intermediate dim — uppercase mini labels)
//   amber-700 -> #4a5878  t3     (very dim — status pills, "step N" chips)
//   amber-800 -> #2a3548  ----   (dim borders)
//   amber-900 -> #1a2332  ----   (raised surface / hover background)
//   amber-950 -> #0d1219  bg2    (panel surface)
//
// Tailwind v3's opacity modifier (`bg-amber-950/40`, `border-amber-700/60`,
// `hover:bg-amber-900/40`) keeps working with hex inputs — the engine
// applies the opacity to whichever hex sits at that palette step.
//
// The semantic CSS classes in `src/index.css` (.up / .down / .gold / .dim)
// reference the same hex values via CSS variables, so a developer can use
// either approach interchangeably and they will look identical.
//
// If a future panel needs the original stock-Tailwind amber for some
// specific visual effect (it shouldn't), it can write the hex directly via
// arbitrary-value syntax: `text-[#fcd34d]`.

const omegaPalette = {
  // Step-aligned amber palette — REMAPPED to Omega palette.
  amber: {
    200: '#ffe680', // gold2
    300: '#f5c842', // gold (primary accent)
    400: '#e8edf5', // t1 (primary text)
    500: '#8a9ab8', // t2 (muted secondary)
    600: '#6a7898', // intermediate dim
    700: '#4a5878', // t3 (very dim)
    800: '#2a3548', // dim border
    900: '#1a2332', // raised / hover surface
    950: '#0d1219', // bg2 (panel surface)
  },
  // Semantic aliases used directly by some components.
  terminal: {
    bg:      '#05080d', // bg0
    surface: '#0d1219', // bg2
    border:  '#2a3548',
    text:    '#e8edf5', // t1
    dim:     '#8a9ab8', // t2
    accent:  '#f5c842', // gold
  },
  // Up / down convenience for arbitrary uses (the .up / .down CSS classes
  // in index.css are the canonical surface; these are extras for places
  // where a Tailwind class is more ergonomic).
  up:   '#00d97e', // green
  down: '#ff3355', // red
  gold: '#f5c842',
  dim:  '#8a9ab8',
};

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
      colors: omegaPalette,
      boxShadow: {
        // Gold-accent glow for focused rows / hover halos. Replaces the
        // amber glow used in the Step-1 config.
        glow: '0 0 0 1px rgba(245,200,66,0.25), 0 0 12px rgba(245,200,66,0.15)',
      },
    },
  },
  plugins: [],
};
