#!/usr/bin/env python3
"""Locked deployable champion configs (regime-gated). Single source of truth."""
import sys; sys.path.insert(0,'/tmp')
import luke_matrix as M
STOCK=dict(entry_modes=['A','C'],adr_min=4.0,max_stopw=0.06,cost_bps=2,stop_slip_bps=5,
           regime_gate=True,time_stop=20,risk_pct=0.0075,trail_ema=True,be_after_partial=True)
CRYPTO=dict(entry_modes=['C','B'],adr_min=3.0,max_stopw=0.06,cost_bps=6,stop_slip_bps=10,
            regime_gate=True,trail_ema=False,max_partials=0,be_after_partial=False,risk_pct=0.01)
print('OMEGA stocks champion (SPY>200MA gated):'); M.pr(M.run('/tmp/luke_data/*.csv',STOCK,'stocks A+C gated'))
print('CHIMERA crypto champion (BTC>200MA gated):'); M.pr(M.run('/tmp/luke_crypto/*.csv',CRYPTO,'crypto C+B ride-wide gated'))
