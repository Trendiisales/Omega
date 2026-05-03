import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import path from 'node:path';

// Omega Terminal — Vite config
//
// Step 2 update (2026-05-01): adds the dev proxy for the new
// OmegaApiServer endpoints. Every request to /api/v1/omega/* hitting the
// Vite dev server (default 127.0.0.1:5173) is forwarded to the C++
// OmegaApiServer at 127.0.0.1:7781.
//
// Port note: the original STEP2_OPENER.md called for :7779, but
// src/gui/OmegaTelemetryServer is already bound to :7779 (HTTP) + :7780
// (WebSocket). OmegaApiServer therefore moves to :7781 so both servers
// can run in parallel until the Step 7 cutover. This is the canonical
// port from now on; STEP2_OPENER's :7779 reference is superseded by
// docs/SESSION_2026-05-01_HANDOFF.md § "OmegaTelemetryServer already on
// :7779".
//
// `changeOrigin: true` keeps the upstream Host header consistent with
// the OmegaApiServer's bind address, which matters once we add basic
// origin checks server-side. `secure: false` is irrelevant for plain
// HTTP but flagged here to make the intent explicit if anyone later
// switches to https://127.0.0.1:7781.
//
// `rewrite` is intentionally a no-op identity transform: Vite forwards
// the path verbatim. We keep the function in place so future versioned
// routes (/api/v2/...) are easy to remap without restructuring the
// proxy block.

const OMEGA_API_TARGET = 'http://127.0.0.1:7781';

export default defineConfig({
  plugins: [react()],
  resolve: {
    alias: {
      '@': path.resolve(__dirname, 'src'),
    },
  },
  server: {
    port: 5173,
    strictPort: true,
    host: '127.0.0.1',
    proxy: {
      '/api/v1/omega': {
        target: OMEGA_API_TARGET,
        changeOrigin: true,
        secure: false,
        rewrite: (p) => p,
      },
    },
  },
  build: {
    target: 'es2022',
    sourcemap: true,
    outDir: 'dist',
    emptyOutDir: true,
  },
});
