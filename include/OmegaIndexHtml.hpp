#pragma once
// AUTO-GENERATED from src/gui/www/omega_index.html
// Split into 3 chunks to stay under MSVC 16KB string literal limit.
namespace omega_gui {
static const char* INDEX_HTML =
R"OMEGA0(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Omega — Trading Desk</title>
    <link rel="icon" type="image/png" href="/chimera_logo.png">
    <link rel="preconnect" href="https://fonts.googleapis.com">
    <link href="https://fonts.googleapis.com/css2?family=Space+Mono:wght@400;700&family=DM+Sans:wght@300;400;500;600;700&display=swap" rel="stylesheet">
    <style>
        *, *::before, *::after { margin:0; padding:0; box-sizing:border-box; }
        :root {
            --bg0:#080b10; --bg1:#0e1218; --bg2:#131820;
            --glass:rgba(18,24,34,0.82); --border:rgba(255,255,255,0.07);
            --t1:#dde2ea; --t2:#6b748a; --t3:#3d4455;
            --gold:#f0c040; --gold2:#ffd966; --silver:#b0c4d8;
            --green:#00e87a; --red:#ff3d5a; --amber:#ff9500;
            --blue:#3db8ff; --purple:#9d7fff; --cyan:#00d4ff;
        }
        body { font-family:'DM Sans',sans-serif; background:var(--bg0); color:var(--t1); overflow-x:hidden; min-height:100vh; }
        body::after { content:''; position:fixed; inset:0; pointer-events:none; z-index:9999;
            background:repeating-linear-gradient(0deg,transparent,transparent 2px,rgba(0,0,0,0.025) 2px,rgba(0,0,0,0.025) 4px); }
        .desk { display:grid; grid-template-rows:auto 1fr; height:100vh; padding:10px; gap:10px; }
        header { background:var(--glass); border:1px solid var(--border); border-radius:12px; padding:10px 20px;
            display:flex; justify-content:space-between; align-items:center; backdrop-filter:blur(20px);
            box-shadow:0 4px 24px rgba(0,0,0,0.4); }
        .logo { display:flex; align-items:center; gap:12px; }
        .logo-img { width:36px; height:36px; background:url('/chimera_logo.png') center/contain no-repeat;
            filter:drop-shadow(0 0 8px rgba(61,184,255,0.4)); }
        .logo-name { font-family:'Space Mono',monospace; font-size:15px; font-weight:700; letter-spacing:1px;
            background:linear-gradient(120deg,var(--cyan) 0%,var(--blue) 60%,var(--purple) 100%);
            -webkit-background-clip:text; -webkit-text-fill-color:transparent; }
        .logo-sub { font-size:9px; color:var(--t2); letter-spacing:2px; text-transform:uppercase; margin-top:1px; }
        .hbar { display:flex; align-items:center; gap:8px; flex-wrap:nowrap; overflow:visible; }
        .badge { padding:4px 9px; border-radius:6px; font-size:11px; font-weight:600; letter-spacing:0.5px;
            border:1px solid var(--border); background:rgba(255,255,255,0.04); white-space:nowrap; }
        .badge.mode-shadow { color:var(--amber); border-color:var(--amber); background:rgba(255,149,0,0.1); }
        .badge.mode-live   { color:var(--green); border-color:var(--green); background:rgba(0,232,122,0.1); }
        .conn-dot { width:9px; height:9px; border-radius:50%; display:inline-block; margin-right:6px; }
        .conn-dot.ok  { background:var(--green); animation:sonar 2s infinite; }
        .conn-dot.bad { background:var(--red); }
        @keyframes sonar { 0%{box-shadow:0 0 0 0 rgba(0,232,122,0.6);}70%{box-shadow:0 0 0 8px rgba(0,232,122,0);}100%{box-shadow:0 0 0 0 rgba(0,232,122,0);} }
        .mono { font-family:'Space Mono',monospace; font-size:13px; color:var(--t2); }
        .main { display:grid; grid-template-columns:300px 1fr 280px; grid-template-rows:auto auto 1fr; gap:10px; overflow:hidden; }
        .card { background:var(--glass); border:1px solid var(--border); border-radius:12px; padding:14px 16px;
            backdrop-filter:blur(16px); position:relative; overflow:hidden; }
        .card::before { content:''; position:absolute; top:0; left:0; right:0; height:2px;
            background:linear-gradient(90deg,var(--cyan) 0%,transparent 100%); opacity:0.5; }
        .card-hd { font-size:9px; font-weight:700; text-transform:uppercase; letter-spacing:2px; color:var(--t2);
            margin-bottom:12px; display:flex; align-items:center; gap:8px; }
        .card-hd .dot { width:5px; height:5px; border-radius:50%; background:var(--cyan); flex-shrink:0; }
        .col-left { grid-column:1; display:flex; flex-direction:column; gap:10px; }
        .sym-group-label { font-size:8px; color:var(--t2); text-transform:uppercase; letter-spacing:2px; margin:6px 0 4px; }
        .sym-row { display:flex; justify-content:space-between; align-items:center; padding:8px 10px;
            border-radius:8px; background:rgba(255,255,255,0.03); border:1px solid rgba(255,255,255,0.05); margin-bottom:4px; }
        .sym-row.primary-sym { border-color:rgba(61,184,255,0.18); background:rgba(61,184,255,0.04); }
        .sym-label { font-weight:700; font-size:11px; color:var(--t2); letter-spacing:1px; min-width:60px; }
        .sym-label.primary { color:var(--blue); }
        .px-pair { display:flex; gap:10px; align-items:center; }
        .bid { font-family:'Space Mono',monospace; font-size:13px; font-weight:700; color:var(--green); }
        .ask { font-family:'Space Mono',monospace; font-size:13px; font-weight:700; color:var(--red); }
        .sym-spread { font-family:'Space Mono',monospace; font-size:9px; color:var(--t2); }
        .eng-state-grid { display:grid; grid-template-columns:1fr 1fr 1fr; gap:6px; }
        .eng-state-card { border-radius:8px; padding:10px 8px; border:1px solid var(--border);
            background:rgba(255,255,255,0.02); text-align:center; position:relative; overflow:hidden; transition:all 0.3s; }
        .eng-state-card.phase-0 { background:rgba(10,16,28,0.6); border-color:var(--t3); }
        .eng-state-card.phase-1 { background:rgba(40,28,4,0.8); border-color:rgba(200,120,0,0.4); animation:eng-pulse-amber 1.5s ease-in-out infinite; }
        .eng-state-card.phase-2 { background:rgba(0,40,20,0.95); border-color:var(--green); animation:eng-pulse-green 0.8s ease-in-out infinite; }
        @keyframes eng-pulse-amber { 0%,100%{box-shadow:0 0 0 0 rgba(255,149,0,0);} 50%{box-shadow:0 0 12px 3px rgba(255,149,0,0.2);} }
        @keyframes eng-pulse-green { 0%,100%{box-shadow:0 0 0 0 rgba(0,232,122,0);} 50%{box-shadow:0 0 16px 4px rgba(0,232,122,0.3);} }
        .eng-sym { font-size:10px; font-weight:700; color:var(--blue); letter-spacing:0.5px; }
        .eng-phase-badge { font-size:8px; text-transform:uppercase; letter-spacing:1px; margin-top:4px; padding:2px 6px;
            border-radius:4px; display:inline-block; }
        .phase-badge-flat { background:rgba(255,255,255,0.06); color:var(--t2); }
        .phase-badge-comp { background:rgba(255,149,0,0.15); color:var(--amber); }
        .phase-badge-brk  { background:rgba(0,232,122,0.15); color:var(--green); }
        .eng-vol { font-family:'Space Mono',monospace; font-size:9px; color:var(--t2); margin-top:4px; }
        .eng-signals { font-family:'Space Mono',monospace; font-size:11px; color:var(--gold); margin-top:4px; }
        .regime-strip { display:flex; gap:8px; align-items:center; padding:10px 12px; border-radius:8px;
            background:rgba(255,255,255,0.02); border:1px solid var(--border); margin-top:0; }
        .regime-label { font-size:9px; color:var(--t2); text-transform:uppercase; letter-spacing:1.5px; }
        .regime-val { font-family:'Space Mono',monospace; font-size:13px; font-weight:700; margin-left:auto; }
        .regime-risk-on  { color:var(--green); }
        .regime-risk-off { color:var(--red); }
        .regime-neutral  { color:var(--amber); }
        .col-centre { grid-column:2; display:flex; flex-direction:column; gap:10px; }
        .stats-row { display:grid; grid-template-columns:180px 1fr 1fr 1fr 1fr; gap:8px; }
        .pnl-val { font-family:'Space Mono',monospace; font-size:34px; font-weight:700; letter-spacing:-1px; transition:color 0.3s; text-shadow:0 0 20px currentColor; }
        .pnl-pos { color:var(--green); } .pnl-neg { color:var(--red); }
        .pnl-sub { font-size:10px; color:var(--t2); margin-top:4px; }
        .stat-card { text-align:center; padding:14px 10px; }
        .stat-val { font-family:'Space Mono',monospace; font-size:22px; font-weight:700; color:var(--blue); text-shadow:0 0 12px rgba(61,184,255,0.3); }
        .stat-lbl { font-size:9px; color:var(--t2); text-transform:uppercase; letter-spacing:1px; margin-top:4px; }
        .conf-grid { display:grid; grid-template-columns:repeat(5,1fr); gap:6px; }
        .conf-card { border-radius:8px; padding:8px; border:1px solid var(--border);
            background:rgba(255,255,255,0.02); text-align:center; }
        .conf-sym { font-size:9px; font-weight:700; color:var(--t2); letter-spacing:1px; }
        .conf-price { font-family:'Space Mono',monospace; font-size:11px; color:var(--t1); margin-top:3px; }
        .conf-chg { font-family:'Space Mono',monospace; font-size:9px; margin-top:2px; }
        .conf-up { color:var(--green); } .conf-dn { color:var(--red); } .conf-flat { color:var(--t2); }
        .sig-panel { padding:12px 14px; border-radius:8px; border:1px solid rgba(61,184,255,0.15);
            background:rgba(61,184,255,0.04); }
        .no-data { text-align:center; color:var(--t2); padding:20px; font-size:12px; }
        table { width:100%; border-collapse:collapse; font-size:11px; }
        th { text-align:left; padding:7px 8px; color:var(--t2); font-size:9px; text-transform:uppercase;
            letter-spacing:1.5px; font-weight:600; border-bottom:1px solid rgba(255,255,255,0.06); }
        td { padding:8px 8px; border-bottom:1px solid rgba(255,255,255,0.03); }
        tr:hover td { background:rgba(255,255,255,0.02); }
        .col-right { grid-column:3; display:flex; flex-direction:column; gap:10px; }
        .lat-grid { display:grid; grid-template-columns:1fr 1fr; gap:8px; }
        .lat-item { text-align:center; padding:10px 8px; background:rgba(255,255,255,0.02); border-radius:8px; border:1px solid var(--border); }
        .lat-val { font-family:'Space Mono',monospace; font-size:20px; font-weight:700; color:var(--blue); }
        .lat-lbl { font-size:9px; color:var(--t2); text-transform:uppercase; letter-spacing:1px; margin-top:3px; }
        .gov-row { display:flex; justify-content:space-between; align-items:center; font-size:10px; padding:5px 0;
            border-bottom:1px solid rgba(255,255,255,0.03); }
        .gov-row:last-child { border-bottom:none; }
        .gov-reason { color:var(--t2); min-width:100px; font-size:10px; }
        .gov-bar-bg { flex:1; height:4px; background:var(--t3); margin:0 8px; border-radius:2px; overflow:hidden; }
        .gov-bar-fill { height:100%; border-radius:2px; background:var(--amber); transition:width 0.5s; }
        .gov-n { font-family:'Space Mono',monospace; font-weight:700; color:var(--amber); min-width:24px; text-align:right; }
        .fix-row { display:flex; justify-content:space-between; align-items:center; padding:7px 10px;
            border-radius:8px; background:rgba(255,255,255,0.02); border:1px solid var(--border); margin-bottom:5px; font-size:11px; }
        .fix-label { color:var(--t2); font-size:10px; text-transform:uppercase; letter-spacing:1px; }
        .fix-val { font-family:'Space Mono',monospace; font-weight:700; font-size:12px; }
        .fix-ok { color:var(--green); } .fix-bad { color:var(--red); }
        .bottom-row { grid-column:1 / -1; display:grid; grid-template-columns:1fr 1fr; gap:10px; }
        ::-webkit-scrollbar { width:4px; height:4px; }
        ::-webkit-scrollbar-track { background:transparent; }
        ::-webkit-scrollbar-thumb { background:rgba(255,255,255,0.1); border-radius:2px; }
    </style>
</head>
<body>
<div class="desk">
    <header>
        <div class="logo">
            <div class="logo-img"></div>
            <div>
                <div class="logo-name">Omega</div>
                <div class="logo-sub">Commodities &amp; Indices — Breakout System</div>
            </div>
        </div>
        <div class="hbar">
            <span id="modeBadge" class="badge mode-shadow">SHADOW</span>
            <span id="buildBadge" class="badge" style="color:var(--amber);font-size:9px;font-weight:700;letter-spacing:1.5px" title="Git hash — built version">⬡ <span id="buildVersion">...</span></span>
            <span id="uptimeBadge" class="badge" style="color:var(--t2);font-size:10px;font-family:monospace">UP 00:00:00</span>
            <span id="sessionBadge" class="badge" style="color:var(--t2);font-size:10px;font-weight:600">── UTC</span>
            <span class="badge"><span class="conn-dot bad" id="connDot"></span><span id="connText">Connecting...</span></span>
            <span class="badge" id="fixQuoteHdr" style="color:var(--red)">QUOTE: --</span>
            <span class="mono" id="clock">--:--:-- UTC</span>
        </div>
    </header>

    <div class="main">
        <!-- LEFT COLUMN -->
        <div class="col-left">
            <div class="card">
                <div class="card-hd"><span class="dot"></span>Market Data</div>

                <div class="sym-group-label">▶ Primary — Trading</div>
                <div class="sym-row primary-sym">
                    <span class="sym-label primary">US500.F</span>
                    <div class="px-pair"><span class="bid" id="spBid">----</span><span style="color:var(--t3)">|</span><span class="ask" id="spAsk">----</span></div>
                    <span class="sym-spread" id="spSpread">--</span>
                </div>
                <div class="sym-row primary-sym">
                    <span class="sym-label primary">USTEC.F</span>
                    <div class="px-pair"><span class="bid" id="nqBid">----</span><span style="color:var(--t3)">|</span><span class="ask" id="nqAsk">----</span></div>
                    <span class="sym-spread" id="nqSpread">--</span>
                </div>
                <div class="sym-row primary-sym">
                    <span class="sym-label primary">USOIL.F</span>
                    <div class="px-pair"><span class="bid" id="clBid">----</span><span style="color:var(--t3)">|</span><span class="ask" id="clAsk">----</span></div>
                    <span class="sym-spread" id="clSpread">--</span>
                </div>

                <div class="sym-group-label" style="margin-top:8px;">◈ Confirmation</div>
                <div class="sym-row">
                    <span class="sym-label">VIX.F</span>
                    <div class="px-pair"><span class="bid" id="vixBid" style="font-size:11px">--</span><span style="color:var(--t3)">|</span><span class="ask" id="vixAsk" style="font-size:11px">--</span></div>
                </div>
                <div class="sym-row">
                    <span class="sym-label">DX.F</span>
                    <div class="px-pair"><span class="bid" id="dxBid" style="font-size:11px">--</span><span style="color:var(--t3)">|</span><span class="ask" id="dxAsk" style="font-size:11px">--</span></div>
                </div>
                <div class="sym-row">
                    <span class="sym-label">DJ30.F</span>
                    <div class="px-pair"><span class="bid" id="djBid" style="font-size:11px">--</span><span style="color:var(--t3)">|</span><span class="ask" id="djAsk" style="font-size:11px">--</span></div>
                </div>
                <div class="sym-row">
                    <span class="sym-label">NAS100</span>
)OMEGA0"
R"OMEGA1(
                    <div class="px-pair"><span class="bid" id="nasBid" style="font-size:11px">--</span><span style="color:var(--t3)">|</span><span class="ask" id="nasAsk" style="font-size:11px">--</span></div>
                </div>
                <div class="sym-row">
                    <span class="sym-label">GOLD.F</span>
                    <div class="px-pair"><span class="bid" id="goldBid" style="font-size:11px">--</span><span style="color:var(--t3)">|</span><span class="ask" id="goldAsk" style="font-size:11px">--</span></div>
                </div>
                <div class="sym-row">
                    <span class="sym-label">NGAS.F</span>
                    <div class="px-pair"><span class="bid" id="ngasBid" style="font-size:11px">--</span><span style="color:var(--t3)">|</span><span class="ask" id="ngasAsk" style="font-size:11px">--</span></div>
                </div>
            </div>

            <div class="card">
                <div class="card-hd"><span class="dot" style="background:var(--gold)"></span>Macro Regime</div>
                <div class="regime-strip">
                    <span class="regime-label">VIX</span>
                    <span class="regime-val" id="vixLevel" style="color:var(--t1)">--</span>
                    <span class="regime-label" style="margin-left:12px;">Regime</span>
                    <span class="regime-val" id="macroRegime">--</span>
                </div>
                <div class="regime-strip" style="margin-top:6px;">
                    <span class="regime-label">ES vs NQ</span>
                    <span class="regime-val" id="esNqDiv" style="color:var(--blue)">--</span>
                    <span class="regime-label" style="margin-left:12px;">Session</span>
                    <span class="regime-val" id="sessionVal" style="color:var(--t1)">--</span>
                </div>
            </div>
        </div>

        <!-- CENTRE COLUMN -->
        <div class="col-centre">
            <!-- Engine Phase Cards -->
            <div>
                <div style="font-size:9px;color:var(--t2);text-transform:uppercase;letter-spacing:2px;margin-bottom:8px;">⚡ Breakout Engine State</div>
                <div class="eng-state-grid">
                    <div class="eng-state-card phase-0" id="engSP">
                        <div class="eng-sym">US500.F</div>
                        <div class="eng-phase-badge phase-badge-flat" id="engSPPhase">FLAT</div>
                        <div class="eng-vol" id="engSPVol">rv -- bv --</div>
                        <div class="eng-signals" id="engSPSig">0 signals</div>
                    </div>
                    <div class="eng-state-card phase-0" id="engNQ">
                        <div class="eng-sym">USTEC.F</div>
                        <div class="eng-phase-badge phase-badge-flat" id="engNQPhase">FLAT</div>
                        <div class="eng-vol" id="engNQVol">rv -- bv --</div>
                        <div class="eng-signals" id="engNQSig">0 signals</div>
                    </div>
                    <div class="eng-state-card phase-0" id="engCL">
                        <div class="eng-sym">USOIL.F</div>
                        <div class="eng-phase-badge phase-badge-flat" id="engCLPhase">FLAT</div>
                        <div class="eng-vol" id="engCLVol">rv -- bv --</div>
                        <div class="eng-signals" id="engCLSig">0 signals</div>
                    </div>
                </div>
            </div>

            <!-- Stats -->
            <div class="stats-row">
                <div class="card" style="padding:14px 16px;">
                    <div class="card-hd"><span class="dot"></span>Daily P&amp;L</div>
                    <div class="pnl-val pnl-pos" id="pnlVal">+$0.00</div>
                    <div class="pnl-sub" id="pnlSub">0 trades &middot; 0.0% win</div>
                    <div style="font-size:9px;color:var(--t2);margin-top:3px;" id="pnlGrossSub"></div>
                </div>
                <div class="card stat-card"><div class="stat-val" id="statWins">0</div><div class="stat-lbl">Wins</div></div>
                <div class="card stat-card"><div class="stat-val" id="statLosses">0</div><div class="stat-lbl">Losses</div></div>
                <div class="card stat-card"><div class="stat-val" id="statAvgWin" style="color:var(--green)">$0</div><div class="stat-lbl">Avg Win</div></div>
                <div class="card stat-card"><div class="stat-val" id="statMaxDD" style="color:var(--red)">$0</div><div class="stat-lbl">Max DD</div></div>
            </div>

            <!-- Last Signal -->
            <div class="card">
                <div class="card-hd"><span class="dot" style="background:var(--amber)"></span>Last Signal</div>
                <div id="lastSignalDetail">
                    <span style="color:var(--t2);font-size:12px;">Waiting for first signal...</span>
                </div>
            </div>

            <!-- Recent Trades -->
            <div class="card" style="flex:1;overflow:hidden;">
                <div class="card-hd">
                    <span class="dot" style="background:var(--green)"></span>
                    Recent Trades
                    <span id="tradeCount" style="font-family:'Space Mono',monospace;color:var(--t2);margin-left:8px;font-size:10px;"></span>
                    <button id="bellBtn" onclick="toggleBell()" title="Click to enable trade bell (required by browser)"
                        style="margin-left:auto;background:rgba(255,214,0,.1);border:1px solid #ffd600;border-radius:4px;padding:2px 8px;cursor:pointer;font-size:11px;color:#ffd600;">🔔 ARM BELL</button>
                </div>
                <div style="overflow-y:auto;max-height:300px;">
                    <table>
                        <thead><tr>
                            <th>Time</th><th>Symbol</th><th>Side</th><th>Entry</th><th>Exit</th>
                            <th>TP</th><th>SL</th><th>Held</th><th>Result</th><th>Gross</th><th>Slip</th><th>Net P&amp;L</th>
                        </tr></thead>
                        <tbody id="tradesBody"><tr><td colspan="12" class="no-data">No trades yet</td></tr></tbody>
                    </table>
                </div>
            </div>
        </div>

        <!-- RIGHT COLUMN -->
        <div class="col-right">
            <div class="card">
                <div class="card-hd"><span class="dot" style="background:var(--blue)"></span>FIX Latency</div>
                <div class="lat-grid">
                    <div class="lat-item"><div class="lat-val" id="rttLast">--</div><div class="lat-lbl">Last ms</div></div>
                    <div class="lat-item"><div class="lat-val" id="rttP50">--</div><div class="lat-lbl">P50 ms</div></div>
                    <div class="lat-item"><div class="lat-val" id="rttP95">--</div><div class="lat-lbl">P95 ms</div></div>
                    <div class="lat-item"><div class="lat-val" id="msgRate">--</div><div class="lat-lbl">msg/s</div></div>
                </div>
            </div>

            <div class="card">
                <div class="card-hd"><span class="dot" style="background:var(--amber)"></span>Governor Blocks</div>
                <div class="gov-row"><span class="gov-reason">SPREAD_WIDE</span><div class="gov-bar-bg"><div class="gov-bar-fill" id="gbarSpread" style="width:0%"></div></div><span class="gov-n" id="gnSpread">0</span></div>
                <div class="gov-row"><span class="gov-reason">LATENCY_HIGH</span><div class="gov-bar-bg"><div class="gov-bar-fill" id="gbarLat" style="width:0%"></div></div><span class="gov-n" id="gnLat">0</span></div>
                <div class="gov-row"><span class="gov-reason">PNL_LIMIT</span><div class="gov-bar-bg"><div class="gov-bar-fill" id="gbarPnl" style="width:0%"></div></div><span class="gov-n" id="gnPnl">0</span></div>
                <div class="gov-row"><span class="gov-reason">CONSEC_LOSS</span><div class="gov-bar-bg"><div class="gov-bar-fill" id="gbarConsec" style="width:0%"></div></div><span class="gov-n" id="gnConsec">0</span></div>
                <div style="font-size:9px;color:var(--t2);margin-top:8px;text-align:right;" id="govTotal">Total: 0</div>
            </div>

            <div class="card">
                <div class="card-hd"><span class="dot" style="background:var(--purple)"></span>FIX Session</div>
                <div class="fix-row"><span class="fix-label">Quote</span><span class="fix-val fix-bad" id="fixQStatus">--</span></div>
                <div class="fix-row"><span class="fix-label">Seq Gaps</span><span class="fix-val" id="fixGaps" style="color:var(--t1)">0</span></div>
                <div class="fix-row"><span class="fix-label">Orders</span><span class="fix-val" id="fixOrders" style="color:var(--t1)">0</span></div>
                <div class="fix-row"><span class="fix-label">Fills</span><span class="fix-val" id="fixFills" style="color:var(--t1)">0</span></div>
            </div>

            <!-- Breadth confirmation -->
            <div class="card">
                <div class="card-hd"><span class="dot" style="background:var(--silver)"></span>Breadth</div>
                <div class="sym-row" style="margin-bottom:4px;">
                    <span class="sym-label">ES</span>
                    <div class="px-pair"><span class="bid" id="esBid" style="font-size:11px">--</span><span style="color:var(--t3)">|</span><span class="ask" id="esAsk" style="font-size:11px">--</span></div>
                </div>
                <div class="sym-row">
                    <span class="sym-label">DX</span>
                    <div class="px-pair"><span class="bid" id="dxCashBid" style="font-size:11px">--</span><span style="color:var(--t3)">|</span><span class="ask" id="dxCashAsk" style="font-size:11px">--</span></div>
                </div>
                <div style="font-size:9px;color:var(--t2);margin-top:8px;text-align:center;">
                    ES + DX confirm broad market moves
                </div>
            </div>
        </div>
)OMEGA1"
R"OMEGA1B(
        <!-- BOTTOM ROW -->
        <div class="bottom-row">
            <div class="card">
                <div class="card-hd"><span class="dot" style="background:var(--cyan)"></span>Compression Ranges</div>
                <div id="compRanges" style="font-size:11px;color:var(--t2);">Waiting for compression...</div>
            </div>
            <div class="card">
                <div class="card-hd"><span class="dot" style="background:var(--gold)"></span>System Log</div>
                <div id="sysLog" style="font-size:10px;color:var(--t2);font-family:'Space Mono',monospace;max-height:80px;overflow-y:auto;">
                    Ready.
                </div>
            </div>
        </div>
    </div>
</div>

<script>
let wsConnected = false;
let lastData = {};
let _bellEnabled = false;
let _lastTradeCount = 0;
let _bellBootCount = -1; // set to trade count on first data load — prevents page-refresh re-ring
// AudioContext created ONCE on user gesture (toggleBell click) and reused.
// Chrome blocks AudioContext creation outside of user gestures.
// Creating a new AudioContext() inside the poll/fetch callback = BLOCKED.
let _audioCtx = null;

function safe(v,d=0){const n=Number(v);return isNaN(n)?d:n;}
function fmtUTC(ts){if(!ts)return '--';const d=new Date(ts*1000);return d.toUTCString().slice(17,25);}

function toggleBell(){
    _bellEnabled=!_bellEnabled;
    const b=document.getElementById('bellBtn');
    if(b){
        if(_bellEnabled){
            b.textContent='🔔 BELL: ARMED';
            b.style.color='var(--green)';b.style.borderColor='var(--green)';
            b.style.background='rgba(0,230,118,.1)';
        } else {
            b.textContent='🔔 ARM BELL';
            b.style.color='#ffd600';b.style.borderColor='#ffd600';
            b.style.background='rgba(255,214,0,.1)';
        }
    }
    // Create AudioContext on first user click — Chrome requires user gesture
    if(_bellEnabled && !_audioCtx){
        try{
            _audioCtx=new (window.AudioContext||window.webkitAudioContext)();
            // Resume if suspended (autoplay policy)
            if(_audioCtx.state==='suspended') _audioCtx.resume();
        }catch(e){console.warn('AudioContext init failed:',e);}
    }
    if(_bellEnabled && _audioCtx){ _playTestBell(); }
}

function _playTestBell(){
    // Short quiet test tone on arm so user knows it's working
    try{
        const ctx=_audioCtx; if(!ctx) return;
        if(ctx.state==='suspended') ctx.resume();
        const now=ctx.currentTime;
        const o=ctx.createOscillator(),g=ctx.createGain(),c=ctx.createDynamicsCompressor();
        c.threshold.value=-6;c.ratio.value=4;
        o.connect(g);g.connect(c);c.connect(ctx.destination);
        o.type='sine';o.frequency.value=880;
        g.gain.setValueAtTime(0,now);
        g.gain.linearRampToValueAtTime(0.4,now+0.01);
        g.gain.exponentialRampToValueAtTime(0.001,now+0.35);
        o.start(now);o.stop(now+0.4);
    }catch(e){}
}

// playWinBell — loud two-tone ascending chime with compressor
function _playWinBell(){
    if(!_bellEnabled||!_audioCtx) return;
    try{
        const ctx=_audioCtx;
        if(ctx.state==='suspended') ctx.resume();
        const now=ctx.currentTime;
        // Two chimes: 880Hz then 1100Hz, louder than old 0.3 gain
        [[0,880,1040],[0.22,1100,1320]].forEach(([t,f1,f2])=>{
            [f1,f2].forEach((freq,i)=>{
                const o=ctx.createOscillator(),g=ctx.createGain(),c=ctx.createDynamicsCompressor();
                c.threshold.value=-3;c.ratio.value=4;c.attack.value=0.003;c.release.value=0.1;
                o.connect(g);g.connect(c);c.connect(ctx.destination);
                o.type='sine';o.frequency.setValueAtTime(freq,now+t);
                g.gain.setValueAtTime(0,now+t);
                g.gain.linearRampToValueAtTime(i===0?1.8:0.9,now+t+0.008); // 1.8 vs old 1.2 — louder
                g.gain.exponentialRampToValueAtTime(0.3,now+t+0.1);
                g.gain.exponentialRampToValueAtTime(0.001,now+t+1.4);
                o.start(now+t);o.stop(now+t+1.5);
            });
        });
    }catch(e){}
}

// playLossBell — low descending thud
function _playLossBell(){
    if(!_bellEnabled||!_audioCtx) return;
    try{
        const ctx=_audioCtx;
        if(ctx.state==='suspended') ctx.resume();
        const now=ctx.currentTime;
        const o=ctx.createOscillator(),g=ctx.createGain(),c=ctx.createDynamicsCompressor();
        c.threshold.value=-6;c.ratio.value=4;
        o.connect(g);g.connect(c);c.connect(ctx.destination);
        o.type='sawtooth';
        o.frequency.setValueAtTime(280,now);
        o.frequency.linearRampToValueAtTime(130,now+0.3);
        g.gain.setValueAtTime(0,now);
        g.gain.linearRampToValueAtTime(0.9,now+0.01); // 0.9 vs old 0.25 — much louder
        g.gain.exponentialRampToValueAtTime(0.001,now+0.5);
        o.start(now);o.stop(now+0.55);
    }catch(e){}
}

function setPrice(id, val, dec) {
    const el = document.getElementById(id);
    if (!el) return;
    const v = safe(val);
    if (v > 0) el.textContent = v.toFixed(dec || 2);
}

function setSpread(id, bid, ask) {
    const el = document.getElementById(id);
    if (!el) return;
    const b = safe(bid), a = safe(ask);
    if (b > 0 && a > 0) el.textContent = (a - b).toFixed(2);
}

function updateEngineCard(prefix, phase, compHi, compLo, rv, bv, sigs) {
    const card  = document.getElementById('eng' + prefix);
    const badge = document.getElementById('eng' + prefix + 'Phase');
    const vol   = document.getElementById('eng' + prefix + 'Vol');
    const sig   = document.getElementById('eng' + prefix + 'Sig');
    if (!card) return;
    const p = safe(phase);
    card.className = 'eng-state-card phase-' + p;
    if (p === 0) { badge.className='eng-phase-badge phase-badge-flat'; badge.textContent='FLAT'; }
    else if (p === 1) { badge.className='eng-phase-badge phase-badge-comp'; badge.textContent='COMPRESSION'; }
    else { badge.className='eng-phase-badge phase-badge-brk'; badge.textContent='BREAKOUT ⚡'; }
    if (p === 1 && safe(compHi) > 0) {
        if (vol) vol.textContent = 'Hi=' + safe(compHi).toFixed(2) + ' Lo=' + safe(compLo).toFixed(2);
    } else {
        if (vol) vol.textContent = 'rv ' + safe(rv).toFixed(3) + '% bv ' + safe(bv).toFixed(3) + '%';
    }
    if (sig) sig.textContent = sigs + ' signals';
}

function updateMacroRegime(d) {
    const vix = safe(d.vix_level);
    const reg = d.macro_regime || 'NEUTRAL';
    const div = safe(d.es_nq_divergence);
    const vE = document.getElementById('vixLevel');
    if (vE) { vE.textContent = vix > 0 ? vix.toFixed(1) : '--';
        vE.style.color = vix >= 25 ? 'var(--red)' : vix <= 15 ? 'var(--green)' : 'var(--amber)'; }
    const rE = document.getElementById('macroRegime');
    if (rE) { rE.textContent = reg;
        rE.className = 'regime-val regime-' + reg.toLowerCase().replace('_','-'); }
    const dE = document.getElementById('esNqDiv');
    if (dE) { dE.textContent = (div >= 0 ? '+' : '') + (div * 100).toFixed(3) + '%';
        dE.style.color = Math.abs(div) < 0.0002 ? 'var(--t2)' : div > 0 ? 'var(--green)' : 'var(--red)'; }
    const sE = document.getElementById('sessionVal');
    if (sE) { const t = safe(d.session_tradeable);
        sE.textContent = t ? (d.session_name || 'ACTIVE') : 'CLOSED';
        sE.style.color = t ? 'var(--green)' : 'var(--t2)'; }
}

function renderLastSignal(d) {
    const el = document.getElementById('lastSignalDetail');
    if (!el) return;
    if (!d.last_signal_side || d.last_signal_side === 'NONE' || d.last_signal_side === 'CLOSED' || d.last_signal_side === '') {
        el.innerHTML = '<span style="color:var(--t2);font-size:12px;">Waiting for first signal...</span>';
        return;
    }
    const sc = d.last_signal_side === 'LONG' ? 'var(--green)' : 'var(--red)';
    el.innerHTML =
        '<div style="display:grid;grid-template-columns:1fr 1fr 1fr;gap:10px;">' +
        '<div style="text-align:center;"><div style="font-size:9px;color:var(--t2);text-transform:uppercase;letter-spacing:1px;margin-bottom:4px;">Symbol</div>' +
        '<div style="font-family:Space Mono,monospace;font-size:18px;font-weight:700;color:var(--blue);">' + (d.last_signal_symbol || '--') + '</div></div>' +
        '<div style="text-align:center;"><div style="font-size:9px;color:var(--t2);text-transform:uppercase;letter-spacing:1px;margin-bottom:4px;">Direction</div>' +
)OMEGA1B"
R"OMEGA2(
        '<div style="font-family:Space Mono,monospace;font-size:18px;font-weight:700;color:' + sc + ';">' + d.last_signal_side + '</div></div>' +
        '<div style="text-align:center;"><div style="font-size:9px;color:var(--t2);text-transform:uppercase;letter-spacing:1px;margin-bottom:4px;">Price</div>' +
        '<div style="font-family:Space Mono,monospace;font-size:18px;font-weight:700;color:var(--t1);">' + safe(d.last_signal_price).toFixed(2) + '</div></div>' +
        '</div>' +
        '<div style="margin-top:8px;font-size:10px;color:var(--t2);">Reason: <span style="color:var(--gold);font-weight:600;">' + (d.last_signal_reason || '--') + '</span>' +
        ' &nbsp;|&nbsp; Regime: <span style="color:var(--amber);">' + (d.macro_regime || '--') + '</span></div>';
}

function renderCompRanges(d) {
    const el = document.getElementById('compRanges');
    if (!el) return;
    const syms = [
        { sym:'US500.F', phase:d.sp_phase,  hi:d.sp_comp_high,  lo:d.sp_comp_low,  rv:d.sp_recent_vol_pct,  bv:d.sp_baseline_vol_pct  },
        { sym:'USTEC.F', phase:d.nq_phase,  hi:d.nq_comp_high,  lo:d.nq_comp_low,  rv:d.nq_recent_vol_pct,  bv:d.nq_baseline_vol_pct  },
        { sym:'USOIL.F', phase:d.cl_phase,  hi:d.cl_comp_high,  lo:d.cl_comp_low,  rv:d.cl_recent_vol_pct,  bv:d.cl_baseline_vol_pct  },
    ];
    let html = '<div style="display:grid;grid-template-columns:1fr 1fr 1fr;gap:10px;">';
    syms.forEach(s => {
        const p = safe(s.phase);
        const col = p === 1 ? 'var(--amber)' : p === 2 ? 'var(--green)' : 'var(--t3)';
        const phase = p === 0 ? 'FLAT' : p === 1 ? 'COMPRESSING' : 'BREAKOUT';
        html += '<div style="text-align:center;">' +
            '<div style="font-size:10px;font-weight:700;color:var(--blue);">' + s.sym + '</div>' +
            '<div style="font-size:9px;color:' + col + ';font-weight:600;margin-top:2px;">' + phase + '</div>' +
            (p === 1 && safe(s.hi) > 0
                ? '<div style="font-family:Space Mono,monospace;font-size:9px;color:var(--t2);margin-top:2px;">' +
                  safe(s.hi).toFixed(2) + ' ↑ ' + safe(s.lo).toFixed(2) + '</div>'
                : '<div style="font-size:9px;color:var(--t3);margin-top:2px;">rv=' + safe(s.rv).toFixed(3) + '%</div>') +
        '</div>';
    });
    html += '</div>';
    el.innerHTML = html;
}

function renderTrades(trades) {
    const el = document.getElementById('tradesBody');
    const cE = document.getElementById('tradeCount');
    if (!trades || trades.length === 0) {
        el.innerHTML = '<tr><td colspan="12" class="no-data">No trades yet</td></tr>';
        if (cE) cE.textContent = ''; return;
    }
    const closed = trades.filter(t => t.exitReason && t.exitReason !== '');
    // On first data load after page refresh, set boot baseline — never ring for pre-existing trades
    if (_bellBootCount < 0) { _bellBootCount = closed.length; _lastTradeCount = closed.length; }
    if (_bellEnabled && closed.length > _lastTradeCount && _lastTradeCount >= _bellBootCount) {
        const pnl = safe(closed[0].net_pnl);
        if (pnl > 0) { _playWinBell(); } else { _playLossBell(); }
    }
    _lastTradeCount = closed.length;
    const wins = closed.filter(t => safe(t.net_pnl) > 0).length;
    const losses = closed.filter(t => safe(t.net_pnl) < 0).length;
    const totalNet = closed.reduce((s,t) => s + safe(t.net_pnl), 0);
    if (cE) cE.textContent = closed.length + ' closed · ' + wins + 'W/' + losses + 'L · ' +
        (totalNet >= 0 ? '+' : '') + '$' + Math.abs(totalNet).toFixed(2) + ' net';
    const now = Math.floor(Date.now() / 1000);
    el.innerHTML = trades.slice(0, 50).map(t => {
        const isOpen = !t.exitReason || t.exitReason === '';
        const netPnl  = safe(t.net_pnl);
        const grossPnl = safe(t.pnl);
        const slip    = safe(t.slippage_entry) + safe(t.slippage_exit);
        const isWin = netPnl > 0, isLoss = netPnl < 0;
        const rowBg = isOpen ? 'rgba(61,184,255,0.07)' : isWin ? 'rgba(0,200,100,0.07)' : isLoss ? 'rgba(220,50,50,0.07)' : '';
        const rowBorder = isOpen ? '1px solid rgba(61,184,255,0.25)' : '1px solid rgba(255,255,255,0.04)';
        const pnlC = isWin ? 'var(--green)' : isLoss ? 'var(--red)' : 'var(--t2)';
        const sideC = t.side === 'LONG' ? 'var(--green)' : 'var(--red)';
        const reason = t.exitReason || '';
        const result = isOpen ? '🔵 OPEN'
            : reason === 'TP_HIT'  ? '✅ TP'
            : reason === 'SL_HIT'  ? '❌ SL'
            : reason === 'TIMEOUT' ? '⏱ TO' : '⚡ FC';
        const resultC = isOpen ? 'var(--blue)'
            : reason === 'TP_HIT' ? 'var(--green)' : reason === 'SL_HIT' ? 'var(--red)' : 'var(--t2)';
        let heldStr = '--';
        if (isOpen && safe(t.entryTs) > 0) {
            const es = now - safe(t.entryTs); heldStr = es >= 60 ? Math.floor(es/60)+'m'+(es%60)+'s' : es+'s';
        } else if (safe(t.entryTs) > 0 && safe(t.exitTs) > 0) {
            const hs = safe(t.exitTs) - safe(t.entryTs); heldStr = hs >= 60 ? Math.floor(hs/60)+'m'+(hs%60)+'s' : hs+'s';
        }
        const grossDisplay = isOpen ? '' : (grossPnl >= 0 ? '+' : '') + '$' + Math.abs(grossPnl).toFixed(2);
        const slipDisplay  = isOpen ? '' : slip > 0 ? '-$' + slip.toFixed(2) : '--';
        const netDisplay   = isOpen ? '<span style="color:var(--t2);font-size:9px;">live</span>'
            : (netPnl >= 0 ? '+' : '') + '$' + Math.abs(netPnl).toFixed(2);
        return `<tr style="background:${rowBg};border-bottom:${rowBorder};">
            <td style="padding:4px 8px;color:var(--t2);font-size:9px;">${fmtUTC(safe(t.entryTs))}</td>
            <td style="padding:4px 8px;color:var(--blue);font-weight:700;">${t.symbol||'--'}</td>
            <td style="padding:4px 8px;color:${sideC};font-weight:700;">${t.side||'--'}</td>
            <td style="padding:4px 8px;font-family:'Space Mono',monospace;">${safe(t.price)>0?safe(t.price).toFixed(2):'--'}</td>
            <td style="padding:4px 8px;font-family:'Space Mono',monospace;color:var(--t2);">${isOpen?'<span style="color:var(--blue);font-size:9px;">open</span>':safe(t.exitPrice)>0?safe(t.exitPrice).toFixed(2):'--'}</td>
            <td style="padding:4px 8px;font-family:'Space Mono',monospace;color:var(--green);font-size:9px;">${safe(t.tp)>0?safe(t.tp).toFixed(1):'--'}</td>
            <td style="padding:4px 8px;font-family:'Space Mono',monospace;color:var(--red);font-size:9px;">${safe(t.sl)>0?safe(t.sl).toFixed(1):'--'}</td>
            <td style="padding:4px 8px;color:var(--t2);font-size:9px;">${heldStr}</td>
            <td style="padding:4px 8px;font-weight:700;color:${resultC};">${result}</td>
            <td style="padding:4px 8px;font-family:'Space Mono',monospace;color:var(--t2);font-size:10px;">${grossDisplay}</td>
            <td style="padding:4px 8px;font-family:'Space Mono',monospace;color:var(--red);font-size:10px;">${slipDisplay}</td>
            <td style="padding:4px 8px;font-family:'Space Mono',monospace;color:${pnlC};font-weight:700;">${netDisplay}</td>
        </tr>`;
    }).join('');
}

function updateDashboard(d) {
    lastData = d; setConn(true);

    // Primary prices
    setPrice('spBid',  d.sp_bid,  2); setPrice('spAsk',  d.sp_ask,  2); setSpread('spSpread', d.sp_bid, d.sp_ask);
    setPrice('nqBid',  d.nq_bid,  2); setPrice('nqAsk',  d.nq_ask,  2); setSpread('nqSpread', d.nq_bid, d.nq_ask);
    setPrice('clBid',  d.cl_bid,  2); setPrice('clAsk',  d.cl_ask,  2); setSpread('clSpread', d.cl_bid, d.cl_ask);

    // Confirmation prices
    setPrice('vixBid',    d.vix_bid,    2); setPrice('vixAsk',    d.vix_ask,    2);
    setPrice('dxBid',     d.dx_bid,     2); setPrice('dxAsk',     d.dx_ask,     2);
    setPrice('djBid',     d.dj_bid,     2); setPrice('djAsk',     d.dj_ask,     2);
    setPrice('nasBid',    d.nas_bid,    2); setPrice('nasAsk',    d.nas_ask,    2);
    setPrice('goldBid',   d.gold_bid,   2); setPrice('goldAsk',   d.gold_ask,   2);
    setPrice('ngasBid',   d.ngas_bid,   2); setPrice('ngasAsk',   d.ngas_ask,   2);
    setPrice('esBid',     d.es_bid,     2); setPrice('esAsk',     d.es_ask,     2);
    setPrice('dxCashBid', d.dxcash_bid, 2); setPrice('dxCashAsk', d.dxcash_ask, 2);

    // Engine state
    updateEngineCard('SP', d.sp_phase, d.sp_comp_high, d.sp_comp_low, d.sp_recent_vol_pct, d.sp_baseline_vol_pct, d.sp_signals);
    updateEngineCard('NQ', d.nq_phase, d.nq_comp_high, d.nq_comp_low, d.nq_recent_vol_pct, d.nq_baseline_vol_pct, d.nq_signals);
    updateEngineCard('CL', d.cl_phase, d.cl_comp_high, d.cl_comp_low, d.cl_recent_vol_pct, d.cl_baseline_vol_pct, d.cl_signals);

    // PnL — daily_pnl from ledger is already net (after slippage)
    // gross_daily_pnl is the sum before slippage, shown as subtitle for transparency
    const pnl = safe(d.daily_pnl);
    const grossPnl = safe(d.gross_daily_pnl);
    const pE = document.getElementById('pnlVal');
    if (pE) { pE.textContent = (pnl >= 0 ? '+' : '') + '$' + Math.abs(pnl).toFixed(2);
        pE.className = 'pnl-val ' + (pnl >= 0 ? 'pnl-pos' : 'pnl-neg'); }
    const t = safe(d.total_trades), wr = safe(d.win_rate);
    document.getElementById('pnlSub').textContent = t + ' trades · ' + wr.toFixed(1) + '% win';
    const gSub = document.getElementById('pnlGrossSub');
    if (gSub && grossPnl !== 0) {
        const slip = Math.abs(grossPnl - pnl);
        gSub.textContent = 'gross ' + (grossPnl >= 0 ? '+' : '') + '$' + Math.abs(grossPnl).toFixed(2)
            + ' · slip -$' + slip.toFixed(2);
    }
    document.getElementById('statWins').textContent    = safe(d.wins);
    document.getElementById('statLosses').textContent  = safe(d.losses);
    document.getElementById('statAvgWin').textContent  = '$' + safe(d.avg_win).toFixed(0);
    document.getElementById('statMaxDD').textContent   = '$' + safe(d.max_drawdown).toFixed(0);

    // Latency
    const rttL = safe(d.fix_rtt_last), rttP50 = safe(d.fix_rtt_p50), rttP95 = safe(d.fix_rtt_p95);
    document.getElementById('rttLast').textContent = rttL  > 0 ? rttL.toFixed(1)   : '--';
    document.getElementById('rttP50').textContent  = rttP50 > 0 ? rttP50.toFixed(1) : '--';
    document.getElementById('rttP95').textContent  = rttP95 > 0 ? rttP95.toFixed(1) : '--';
    const rttEl = document.getElementById('rttLast');
    if (rttEl) rttEl.style.color = rttL < 10 ? 'var(--green)' : rttL < 30 ? 'var(--amber)' : 'var(--red)';

    // FIX status
    const qOk = (d.fix_quote_status || '').includes('CONNECTED');
    const fq = document.getElementById('fixQStatus');
    if (fq) { fq.textContent = d.fix_quote_status || '--'; fq.className = 'fix-val ' + (qOk ? 'fix-ok' : 'fix-bad'); }
    const hq = document.getElementById('fixQuoteHdr');
    if (hq) { hq.textContent = 'QUOTE: ' + (d.fix_quote_status || '--'); hq.style.color = qOk ? 'var(--green)' : 'var(--red)'; }
    document.getElementById('fixGaps').textContent   = safe(d.sequence_gaps);
    document.getElementById('fixOrders').textContent = safe(d.total_orders);
    document.getElementById('fixFills').textContent  = safe(d.total_fills);

    // Mode badge
    const mb = document.getElementById('modeBadge');
    if (mb) { mb.textContent = d.mode || 'SHADOW'; mb.className = 'badge ' + (d.mode === 'LIVE' ? 'mode-live' : 'mode-shadow'); }
    // Git hash — prominent amber badge
    const bv = document.getElementById('buildVersion');
    if (bv && d.build_version) {
        bv.textContent = d.build_version;
        const bb = document.getElementById('buildBadge');
        if (bb) bb.title = 'Built: ' + (d.build_time || '?');
    }

    // UTC session — computed client-side from current UTC time
    const sb = document.getElementById('sessionBadge');
    if (sb) {
        const now = new Date();
        const mins = now.getUTCHours() * 60 + now.getUTCMinutes();
        let sess, col;
        if      (mins >= 420 && mins < 630)  { sess = 'LONDON';   col = 'var(--green)'; }
        else if (mins >= 630 && mins < 780)  { sess = 'OVERLAP';  col = 'var(--amber)'; }
        else if (mins >= 780 && mins < 1080) { sess = 'NEW YORK'; col = 'var(--green)'; }
        else if (mins >= 300 && mins < 420)  { sess = 'DEAD ZONE'; col = 'var(--red)';  }
        else                                 { sess = 'ASIAN';    col = 'var(--t2)';   }
        sb.textContent = sess + ' SESSION';
        sb.style.color = col;
    }

    // Uptime — from server-side seconds counter
    const ub = document.getElementById('uptimeBadge');
    if (ub && d.uptime_sec != null) {
        const s = d.uptime_sec;
        const h = Math.floor(s / 3600);
        const m = Math.floor((s % 3600) / 60);
        const sc = s % 60;
        ub.textContent = 'UP ' + String(h).padStart(2,'0') + ':' + String(m).padStart(2,'0') + ':' + String(sc).padStart(2,'0');
        ub.style.color = h >= 1 ? 'var(--green)' : 'var(--t2)';
    }

    // Governor
    const gs = safe(d.gov_spread), gl = safe(d.gov_latency), gp = safe(d.gov_pnl), gc = safe(d.gov_consec_loss);
    const maxG = Math.max(1, gs, gl, gp, gc);
    [['Spread',gs],['Lat',gl],['Pnl',gp],['Consec',gc]].forEach(([id,n]) => {
        const b = document.getElementById('gbar'+id); const ni = document.getElementById('gn'+id);
        if (b) b.style.width = (n/maxG*100) + '%'; if (ni) ni.textContent = n;
    });
    document.getElementById('govTotal').textContent = 'Total: ' + (gs+gl+gp+gc);

    updateMacroRegime(d);
    renderLastSignal(d);
    renderCompRanges(d);
}

function setConn(ok) {
    const d = document.getElementById('connDot'); const t = document.getElementById('connText');
    if (d) d.className = 'conn-dot ' + (ok ? 'ok' : 'bad');
    if (t) t.textContent = ok ? (wsConnected ? 'Live (WS)' : 'Connected') : 'Reconnecting...';
}

let wsFailCount = 0;
let wsGiveUp = false;

function connectWS() {
    if (wsGiveUp) return;  // port blocked by firewall — HTTP poll handles it
    const ws = new WebSocket('ws://' + window.location.hostname + ':7780');
    ws.onopen  = () => { wsConnected = true; wsFailCount = 0; setConn(true); };
    ws.onmessage = e => { try { updateDashboard(JSON.parse(e.data)); } catch(_) {} };
    ws.onerror = () => { wsFailCount++; };
    ws.onclose = () => {
        wsConnected = false; setConn(false);
        if (wsFailCount >= 5) {
            // Port likely blocked by firewall — stop hammering, use HTTP poll
            wsGiveUp = true;
            console.log('[Omega] WS port 7780 unreachable after 5 attempts — switching to HTTP poll mode');
            return;
        }
        // Exponential backoff: 2s, 4s, 8s, max 15s
        const delay = Math.min(2000 * Math.pow(2, wsFailCount), 15000);
        setTimeout(connectWS, delay);
    };
}

function httpPoll() {
    if (wsConnected) return;  // WS handles updates when connected
    fetch('/api/telemetry').then(r => r.json()).then(updateDashboard).catch(() => setConn(false));
}

function pollTrades() {
    fetch('/api/trades').then(r => r.json()).then(renderTrades).catch(() => {});
}

setInterval(() => { document.getElementById('clock').textContent = new Date().toUTCString().slice(17,25) + ' UTC'; }, 1000);

connectWS();
setInterval(httpPoll, 1000);   // HTTP fallback when WS unavailable
setInterval(pollTrades, 5000); // trades poll every 5s (was 3s)
pollTrades();
</script>
</body>
</html>

)OMEGA2"
;
} // namespace omega_gui
