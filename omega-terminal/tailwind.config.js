/** @type {import('tailwindcss').Config} */
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
        // Tailwind already ships an "amber" palette. We extend with two
        // semantic aliases so panel code can read intent rather than hex.
        terminal: {
          bg: '#000000',
          surface: '#0a0a0a',
          border: '#3f2c00',
          text: '#fbbf24', // amber-400
          dim: '#92400e',  // amber-800
          accent: '#fcd34d', // amber-300
        },
      },
      boxShadow: {
        glow: '0 0 0 1px rgba(252,211,77,0.25), 0 0 12px rgba(252,211,77,0.15)',
      },
    },
  },
  plugins: [],
};
