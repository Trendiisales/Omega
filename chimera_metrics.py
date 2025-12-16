#!/usr/bin/env python3
"""
Chimera Metrics Server
Parses Chimera console output and serves Prometheus-format metrics for the dashboard.

Usage:
    Terminal 1: cd ~/Chimera/build && ./chimera 2>&1 | python3 ../chimera_metrics.py
    Terminal 2: cd ~/Chimera && python3 -m http.server 8081
    Browser: http://localhost:8081/chimera_dashboard_v2.html
"""

import re
import sys
import threading
import time
from http.server import HTTPServer, BaseHTTPRequestHandler
import json

# Global state
state = {
    'binance_connected': 0,
    'binance_ticks': 0,
    'binance_latency_us': 200,
    'fix_quote_connected': 0,
    'fix_trade_connected': 0,
    'fix_ticks': 0,
    'fix_messages': 0,
    'fix_heartbeats': 0,
    'fix_errors': 0,
    'fix_quote_latency_us': 0,
    'fix_trade_latency_us': 0,
    # Metals
    'xauusd_bid': 0, 'xauusd_ask': 0,
    'xagusd_bid': 0, 'xagusd_ask': 0,
    # Forex
    'eurusd_bid': 0, 'eurusd_ask': 0,
    'gbpusd_bid': 0, 'gbpusd_ask': 0,
    'usdjpy_bid': 0, 'usdjpy_ask': 0,
    'audusd_bid': 0, 'audusd_ask': 0,
    'usdcad_bid': 0, 'usdcad_ask': 0,
    'nzdusd_bid': 0, 'nzdusd_ask': 0,
    'usdchf_bid': 0, 'usdchf_ask': 0,
    # Crypto (from Binance)
    'btcusdt_bid': 0, 'btcusdt_ask': 0,
    'ethusdt_bid': 0, 'ethusdt_ask': 0,
    'solusdt_bid': 0, 'solusdt_ask': 0,
    # Engine
    'engine_loop_us': 50,
    'queue_depth': 0,
    'heartbeat': 0,
}

# Parse patterns
STATS_PATTERN = re.compile(r'\[STATS\] bn=(\d+) fx=(\d+) \| ws=(\w+) quote=(\w+) trade=(\w+) \| msgs=(\d+) hb=(\d+)')
PRICES_PATTERN = re.compile(r'\[PRICES\] XAUUSD: ([\d.]+)/([\d.]+) \| EURUSD: ([\d.]+)/([\d.]+)')
FIX_MD_PATTERN = re.compile(r'\[FIX-MD\] (\w+) bid=([\d.]+) ask=([\d.]+)')
BINANCE_TICK_PATTERN = re.compile(r'\[BN\] (\w+) ([\d.]+)/([\d.]+)')

def parse_line(line):
    """Parse a line from Chimera output and update state."""
    global state
    
    # Parse STATS line
    m = STATS_PATTERN.search(line)
    if m:
        state['binance_ticks'] = int(m.group(1))
        state['fix_ticks'] = int(m.group(2))
        state['binance_connected'] = 1 if m.group(3) == 'OK' else 0
        state['fix_quote_connected'] = 1 if m.group(4) == 'OK' else 0
        state['fix_trade_connected'] = 1 if m.group(5) == 'OK' else 0
        state['fix_messages'] = int(m.group(6))
        state['fix_heartbeats'] = int(m.group(7))
        return
    
    # Parse PRICES line
    m = PRICES_PATTERN.search(line)
    if m:
        state['xauusd_bid'] = float(m.group(1))
        state['xauusd_ask'] = float(m.group(2))
        state['eurusd_bid'] = float(m.group(3))
        state['eurusd_ask'] = float(m.group(4))
        return
    
    # Parse FIX market data
    m = FIX_MD_PATTERN.search(line)
    if m:
        symbol = m.group(1).upper()
        bid = float(m.group(2))
        ask = float(m.group(3))
        
        # Map symbol to state key
        symbol_map = {
            'XAUUSD': ('xauusd_bid', 'xauusd_ask'),
            'XAGUSD': ('xagusd_bid', 'xagusd_ask'),
            'EURUSD': ('eurusd_bid', 'eurusd_ask'),
            'GBPUSD': ('gbpusd_bid', 'gbpusd_ask'),
            'USDJPY': ('usdjpy_bid', 'usdjpy_ask'),
            'AUDUSD': ('audusd_bid', 'audusd_ask'),
            'USDCAD': ('usdcad_bid', 'usdcad_ask'),
            'NZDUSD': ('nzdusd_bid', 'nzdusd_ask'),
            'USDCHF': ('usdchf_bid', 'usdchf_ask'),
        }
        
        if symbol in symbol_map:
            bid_key, ask_key = symbol_map[symbol]
            if bid > 0:
                state[bid_key] = bid
            if ask > 0:
                state[ask_key] = ask
        return
    
    # Parse Binance ticks (if we add this output)
    m = BINANCE_TICK_PATTERN.search(line)
    if m:
        symbol = m.group(1).upper()
        bid = float(m.group(2))
        ask = float(m.group(3))
        
        if symbol == 'BTCUSDT':
            state['btcusdt_bid'] = bid
            state['btcusdt_ask'] = ask
        elif symbol == 'ETHUSDT':
            state['ethusdt_bid'] = bid
            state['ethusdt_ask'] = ask
        elif symbol == 'SOLUSDT':
            state['solusdt_bid'] = bid
            state['solusdt_ask'] = ask
        return
    
    # Parse connection events
    if '[BINANCE] Feed started' in line:
        state['binance_connected'] = 1
    elif '[FIX-QUOTE] Logon successful' in line or '[FIX-QUOTE] Logon OK' in line:
        state['fix_quote_connected'] = 1
    elif '[FIX-TRADE] Logon successful' in line or '[FIX-TRADE] Logon OK' in line:
        state['fix_trade_connected'] = 1

def generate_metrics():
    """Generate Prometheus-format metrics."""
    lines = [
        '# HELP chimera_binance_connected Binance WebSocket connection status',
        '# TYPE chimera_binance_connected gauge',
        f'chimera_binance_connected {state["binance_connected"]}',
        f'chimera_fix_quote_connected {state["fix_quote_connected"]}',
        f'chimera_fix_trade_connected {state["fix_trade_connected"]}',
        '',
        '# HELP chimera_binance_ticks Total Binance ticks received',
        '# TYPE chimera_binance_ticks counter',
        f'chimera_binance_ticks {state["binance_ticks"]}',
        f'chimera_fix_ticks {state["fix_ticks"]}',
        f'chimera_fix_messages {state["fix_messages"]}',
        f'chimera_fix_heartbeats {state["fix_heartbeats"]}',
        f'chimera_fix_errors {state["fix_errors"]}',
        '',
        '# HELP chimera_binance_latency_us Latency in microseconds',
        '# TYPE chimera_binance_latency_us gauge',
        f'chimera_binance_latency_us {state["binance_latency_us"]}',
        f'chimera_fix_quote_latency_us {state["fix_quote_latency_us"]}',
        f'chimera_fix_trade_latency_us {state["fix_trade_latency_us"]}',
        '',
        '# HELP chimera_engine_loop_us Engine loop time',
        '# TYPE chimera_engine_loop_us gauge',
        f'chimera_engine_loop_us {state["engine_loop_us"]}',
        f'chimera_queue_depth {state["queue_depth"]}',
        f'chimera_heartbeat {state["heartbeat"]}',
        '',
        '# Metals',
        f'chimera_xauusd_bid {state["xauusd_bid"]}',
        f'chimera_xauusd_ask {state["xauusd_ask"]}',
        f'chimera_xagusd_bid {state["xagusd_bid"]}',
        f'chimera_xagusd_ask {state["xagusd_ask"]}',
        '',
        '# Forex majors',
        f'chimera_eurusd_bid {state["eurusd_bid"]}',
        f'chimera_eurusd_ask {state["eurusd_ask"]}',
        f'chimera_gbpusd_bid {state["gbpusd_bid"]}',
        f'chimera_gbpusd_ask {state["gbpusd_ask"]}',
        f'chimera_usdjpy_bid {state["usdjpy_bid"]}',
        f'chimera_usdjpy_ask {state["usdjpy_ask"]}',
        f'chimera_audusd_bid {state["audusd_bid"]}',
        f'chimera_audusd_ask {state["audusd_ask"]}',
        f'chimera_usdcad_bid {state["usdcad_bid"]}',
        f'chimera_usdcad_ask {state["usdcad_ask"]}',
        f'chimera_nzdusd_bid {state["nzdusd_bid"]}',
        f'chimera_nzdusd_ask {state["nzdusd_ask"]}',
        f'chimera_usdchf_bid {state["usdchf_bid"]}',
        f'chimera_usdchf_ask {state["usdchf_ask"]}',
        '',
        '# Crypto',
        f'chimera_btcusdt_bid {state["btcusdt_bid"]}',
        f'chimera_btcusdt_ask {state["btcusdt_ask"]}',
        f'chimera_ethusdt_bid {state["ethusdt_bid"]}',
        f'chimera_ethusdt_ask {state["ethusdt_ask"]}',
        f'chimera_solusdt_bid {state["solusdt_bid"]}',
        f'chimera_solusdt_ask {state["solusdt_ask"]}',
    ]
    return '\n'.join(lines) + '\n'

class MetricsHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == '/metrics':
            content = generate_metrics()
            self.send_response(200)
            self.send_header('Content-Type', 'text/plain; charset=utf-8')
            self.send_header('Access-Control-Allow-Origin', '*')
            self.send_header('Content-Length', len(content))
            self.end_headers()
            self.wfile.write(content.encode('utf-8'))
        else:
            self.send_error(404)
    
    def log_message(self, format, *args):
        pass  # Suppress HTTP logs

def run_server(port=9002):
    """Run the metrics HTTP server."""
    server = HTTPServer(('0.0.0.0', port), MetricsHandler)
    print(f'[METRICS] Server listening on port {port}', file=sys.stderr)
    server.serve_forever()

def read_stdin():
    """Read lines from stdin and parse them."""
    for line in sys.stdin:
        line = line.strip()
        if line:
            print(line)  # Pass through to stdout
            parse_line(line)

def main():
    # Start HTTP server in background thread
    server_thread = threading.Thread(target=run_server, daemon=True)
    server_thread.start()
    
    print('[METRICS] Chimera Metrics Server started', file=sys.stderr)
    print('[METRICS] Pipe Chimera output: ./chimera 2>&1 | python3 chimera_metrics.py', file=sys.stderr)
    
    # Read and parse stdin
    try:
        read_stdin()
    except KeyboardInterrupt:
        print('\n[METRICS] Shutting down', file=sys.stderr)

if __name__ == '__main__':
    main()
