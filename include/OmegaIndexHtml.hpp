#pragma once
// AUTO-GENERATED — split into chunks to stay under MSVC 16KB string literal limit.
namespace omega_gui {
static const char* INDEX_HTML =
R"OMEGA0(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="Cache-Control" content="no-store,no-cache,must-revalidate">
<title>Omega — Trading Desk</title>
<link rel="icon" type="image/png" href="/chimera_logo.png">
<link rel="preconnect" href="https://fonts.googleapis.com">
<link href="https://fonts.googleapis.com/css2?family=IBM+Plex+Mono:wght@400;600;700&family=Syne:wght@400;600;700;800&display=swap" rel="stylesheet">
<style>
*,*::before,*::after{margin:0;padding:0;box-sizing:border-box}
:root{
  --bg0:#05080d;--bg1:#090d14;--bg2:#0d1219;--bg3:#111720;
  --glass:rgba(11,16,26,0.88);--border:rgba(255,255,255,0.06);--border2:rgba(255,255,255,0.1);
  --t1:#cdd5e0;--t2:#5a6478;--t3:#2e3545;
  --gold:#f5c842;--gold2:#ffe680;--gold-dim:rgba(245,200,66,0.12);
  --silver:#9ab4cc;--silver-dim:rgba(154,180,204,0.1);
  --green:#00d97e;--red:#ff3355;--amber:#ff8800;--blue:#2ea8ff;
  --purple:#a47fff;--cyan:#00c8f0;--teal:#00e0b0;
  --green-dim:rgba(0,217,126,0.12);--red-dim:rgba(255,51,85,0.1);
  --amber-dim:rgba(255,136,0,0.1);--blue-dim:rgba(46,168,255,0.1);
}
html,body{height:100%;overflow:hidden}
body{font-family:'Syne',sans-serif;background:var(--bg0);color:var(--t1);
  background-image:radial-gradient(ellipse 80% 50% at 50% -20%,rgba(46,168,255,0.06),transparent),
                   radial-gradient(ellipse 60% 40% at 80% 80%,rgba(245,200,66,0.04),transparent);}
body::after{content:'';position:fixed;inset:0;pointer-events:none;z-index:9999;
  background:repeating-linear-gradient(0deg,transparent,transparent 3px,rgba(0,0,0,0.018) 3px,rgba(0,0,0,0.018) 4px);}

/* ── LAYOUT ── */
.desk{display:grid;grid-template-rows:52px 1fr;height:100vh;gap:0;padding:8px;gap:8px;}
.main{display:grid;grid-template-columns:220px 1fr 240px;gap:8px;overflow:hidden;min-height:0;}

/* ── CARD ── */
.card{background:var(--glass);border:1px solid var(--border);border-radius:10px;
  backdrop-filter:blur(20px);position:relative;overflow:hidden;}
.card-hd{font-size:8px;font-weight:700;text-transform:uppercase;letter-spacing:2.5px;
  color:var(--t2);padding:10px 12px 6px;display:flex;align-items:center;gap:6px;border-bottom:1px solid var(--border);}
.card-hd .dot{width:4px;height:4px;border-radius:50%;flex-shrink:0;}
.card-body{padding:10px 12px;}

/* ── HEADER ── */
header{background:var(--glass);border:1px solid var(--border);border-radius:10px;
  display:grid;grid-template-columns:auto 1fr auto;align-items:center;gap:12px;
  padding:0 14px;backdrop-filter:blur(24px);box-shadow:0 2px 20px rgba(0,0,0,0.5);}
.logo{display:flex;align-items:center;gap:9px;flex-shrink:0;}
.logo-img{width:28px;height:28px;background:url('/chimera_logo.png') center/contain no-repeat;
  filter:drop-shadow(0 0 6px rgba(46,168,255,0.5));}
.logo-name{font-family:'IBM Plex Mono',monospace;font-size:13px;font-weight:700;
  letter-spacing:2px;background:linear-gradient(120deg,var(--cyan),var(--blue) 50%,var(--purple));
  -webkit-background-clip:text;-webkit-text-fill-color:transparent;}
.logo-sub{font-size:7px;color:var(--t2);letter-spacing:3px;text-transform:uppercase;margin-top:1px;}

/* ── HEADER TICKERS ── */
.hdr-tickers{display:flex;align-items:center;gap:4px;justify-content:center;overflow:hidden;}
.htk{display:flex;align-items:center;gap:4px;padding:4px 8px;border-radius:6px;
  border:1px solid var(--border);background:rgba(255,255,255,0.02);white-space:nowrap;cursor:default;}
.htk.gold{border-color:rgba(245,200,66,0.3);background:var(--gold-dim);}
.htk.silver{border-color:rgba(154,180,204,0.25);background:var(--silver-dim);}
.htk-sym{font-size:8px;font-weight:700;letter-spacing:1px;color:var(--t2);}
.htk.gold .htk-sym{color:var(--gold);}
.htk.silver .htk-sym{color:var(--silver);}
.htk-b{font-family:'IBM Plex Mono',monospace;font-size:12px;font-weight:700;color:var(--green);}
.htk-a{font-family:'IBM Plex Mono',monospace;font-size:12px;font-weight:700;color:var(--red);}
.htk-sep{color:var(--t3);font-size:9px;}
.htk-ph{font-size:7px;font-weight:700;letter-spacing:0.5px;padding:1px 4px;border-radius:3px;margin-left:2px;}
.ph-flat{background:rgba(255,255,255,0.05);color:var(--t2);}
.ph-comp{background:var(--amber-dim);color:var(--amber);}
.ph-brk{background:var(--green-dim);color:var(--green);animation:brk-blink 0.7s ease-in-out infinite;}
@keyframes brk-blink{0%,100%{opacity:1}50%{opacity:0.5}}
.vd{width:1px;height:24px;background:var(--border);margin:0 3px;flex-shrink:0;}

/* ── HEADER RIGHT ── */
.hbar{display:flex;align-items:center;gap:6px;flex-shrink:0;}
.badge{padding:3px 8px;border-radius:5px;font-family:'IBM Plex Mono',monospace;font-size:10px;
  font-weight:600;border:1px solid var(--border);background:rgba(255,255,255,0.03);white-space:nowrap;}
.badge.shadow{color:var(--amber);border-color:rgba(255,136,0,0.4);background:var(--amber-dim);}
.badge.live{color:var(--green);border-color:rgba(0,217,126,0.4);background:var(--green-dim);}
.dot-conn{width:7px;height:7px;border-radius:50%;display:inline-block;margin-right:5px;}
.dot-ok{background:var(--green);box-shadow:0 0 6px var(--green);animation:sonar 2s infinite;}
.dot-bad{background:var(--red);}
@keyframes sonar{0%{box-shadow:0 0 0 0 rgba(0,217,126,0.6)}70%{box-shadow:0 0 0 6px rgba(0,217,126,0)}100%{box-shadow:0 0 0 0 rgba(0,217,126,0)}}

/* ── LEFT COLUMN ── */
.col-left{grid-column:1;display:flex;flex-direction:column;gap:8px;overflow:hidden;min-height:0;}

/* Market data table — compact two-column grid */
.mkt-card{flex:1;overflow:hidden;display:flex;flex-direction:column;min-height:0;}
.mkt-body{flex:1;overflow-y:auto;padding:6px 8px;}
.mkt-body::-webkit-scrollbar{width:3px;}
.mkt-body::-webkit-scrollbar-thumb{background:rgba(255,255,255,0.08);border-radius:2px;}
.sym-section-label{font-size:7px;color:var(--t2);text-transform:uppercase;letter-spacing:2px;
  padding:5px 4px 3px;opacity:0.7;}
.sym-row{display:grid;grid-template-columns:62px 1fr auto;align-items:center;gap:6px;
  padding:5px 6px;border-radius:6px;margin-bottom:2px;border:1px solid transparent;transition:background 0.15s;}
.sym-row:hover{background:rgba(255,255,255,0.025);}
.sym-row.r-gold{border-color:rgba(245,200,66,0.15);background:rgba(245,200,66,0.04);}
.sym-row.r-silver{border-color:rgba(154,180,204,0.12);background:rgba(154,180,204,0.03);}
.sym-row.r-primary{border-color:rgba(46,168,255,0.1);}
.sym-row.r-fx{border-color:rgba(0,200,240,0.1);}
.sym-row.r-asia{border-color:rgba(0,224,176,0.1);}
.sym-row.r-eu{border-color:rgba(157,127,255,0.1);}
.sym-nm{font-size:10px;font-weight:700;letter-spacing:0.5px;}
.c-gold{color:var(--gold)}.c-silver{color:var(--silver)}.c-blue{color:var(--blue)}
.c-cyan{color:var(--cyan)}.c-teal{color:var(--teal)}.c-purple{color:var(--purple)}.c-t2{color:var(--t2)}
.px-pair{display:flex;gap:5px;align-items:center;}
.bid{font-family:'IBM Plex Mono',monospace;font-size:11px;font-weight:700;color:var(--green);}
.ask{font-family:'IBM Plex Mono',monospace;font-size:11px;font-weight:700;color:var(--red);}
.sprd{font-family:'IBM Plex Mono',monospace;font-size:8px;color:var(--t2);}

/* Regime card */
.regime-card{flex-shrink:0;}
.regime-grid{display:grid;grid-template-columns:1fr 1fr;gap:6px;padding:8px 10px;}
.rg-item{background:rgba(255,255,255,0.02);border-radius:6px;padding:7px 8px;text-align:center;}
.rg-lbl{font-size:7px;color:var(--t2);text-transform:uppercase;letter-spacing:1.5px;margin-bottom:3px;}
.rg-val{font-family:'IBM Plex Mono',monospace;font-size:13px;font-weight:700;color:var(--t1);}

/* ── CENTRE COLUMN ── */
.col-centre{grid-column:2;display:flex;flex-direction:column;gap:8px;overflow:hidden;min-height:0;}

/* Stats row */
.stats-bar{display:grid;grid-template-columns:160px repeat(4,1fr);gap:8px;flex-shrink:0;}
.pnl-card{background:var(--glass);border:1px solid var(--border);border-radius:10px;
  padding:10px 14px;position:relative;overflow:hidden;}
.pnl-card::before{content:'';position:absolute;top:0;left:0;right:0;height:2px;
  background:linear-gradient(90deg,var(--cyan),var(--blue) 50%,transparent);}
.pnl-num{font-family:'IBM Plex Mono',monospace;font-size:28px;font-weight:700;
  letter-spacing:-0.5px;text-shadow:0 0 20px currentColor;line-height:1;margin:4px 0 2px;}
.pnl-pos{color:var(--green)}.pnl-neg{color:var(--red)}
.pnl-sub{font-size:9px;color:var(--t2);}
.stat-card{background:var(--glass);border:1px solid var(--border);border-radius:10px;
  display:flex;flex-direction:column;align-items:center;justify-content:center;padding:8px 6px;}
.stat-n{font-family:'IBM Plex Mono',monospace;font-size:20px;font-weight:700;color:var(--blue);}
.stat-l{font-size:8px;color:var(--t2);text-transform:uppercase;letter-spacing:1px;margin-top:3px;}

/* Engine grid — all 15 engines in a responsive grid */
.eng-section{flex-shrink:0;}
.eng-section-label{font-size:8px;color:var(--t2);text-transform:uppercase;letter-spacing:2px;padding:0 2px 5px;}
.eng-grid{display:grid;grid-template-columns:repeat(5,1fr);gap:5px;}
.eng-grid.eng-grid-3{grid-template-columns:repeat(3,1fr);}
.eng-grid.eng-grid-6{grid-template-columns:repeat(6,1fr);}
.eng-cell{border-radius:7px;padding:7px 5px;border:1px solid var(--border);
  background:rgba(255,255,255,0.015);text-align:center;cursor:default;transition:all 0.25s;}
.eng-cell.ph1{background:rgba(30,18,0,0.85);border-color:rgba(255,136,0,0.45);
  box-shadow:0 0 8px rgba(255,136,0,0.12);animation:eng-amber 1.8s ease-in-out infinite;}
.eng-cell.ph2{background:rgba(0,30,15,0.95);border-color:rgba(0,217,126,0.6);
  box-shadow:0 0 12px rgba(0,217,126,0.25);animation:eng-green 0.9s ease-in-out infinite;}
@keyframes eng-amber{0%,100%{box-shadow:0 0 8px rgba(255,136,0,0.1)}50%{box-shadow:0 0 14px rgba(255,136,0,0.25)}}
@keyframes eng-green{0%,100%{box-shadow:0 0 10px rgba(0,217,126,0.15)}50%{box-shadow:0 0 20px rgba(0,217,126,0.4)}}
.eng-sym{font-size:9px;font-weight:700;color:var(--blue);letter-spacing:0.3px;line-height:1;}
.eng-ph{font-size:7px;text-transform:uppercase;letter-spacing:0.5px;margin-top:3px;padding:1px 4px;
  border-radius:3px;display:inline-block;}
.eph-flat{background:rgba(255,255,255,0.05);color:var(--t2);}
.eph-comp{background:var(--amber-dim);color:var(--amber);}
.eph-brk{background:var(--green-dim);color:var(--green);}
.eng-vol{font-family:'IBM Plex Mono',monospace;font-size:7px;color:var(--t3);margin-top:3px;line-height:1.3;}
.eng-sigs{font-size:8px;color:var(--gold);margin-top:2px;}

/* Signal + trade area */
.sig-trade-area{flex:1;display:grid;grid-template-rows:auto 1fr;gap:8px;min-height:0;overflow:hidden;}
.last-sig{flex-shrink:0;background:var(--glass);border:1px solid var(--border);border-radius:10px;padding:10px 14px;}
.sig-row{display:grid;grid-template-columns:repeat(4,1fr);gap:10px;align-items:center;}
.sig-field-lbl{font-size:7px;color:var(--t2);text-transform:uppercase;letter-spacing:1.5px;margin-bottom:3px;}
.sig-field-val{font-family:'IBM Plex Mono',monospace;font-size:16px;font-weight:700;}

.trades-card{flex:1;background:var(--glass);border:1px solid var(--border);border-radius:10px;
  display:flex;flex-direction:column;overflow:hidden;min-height:0;}
.trades-scroll{flex:1;overflow-y:auto;}
.trades-scroll::-webkit-scrollbar{width:3px;}
.trades-scroll::-webkit-scrollbar-thumb{background:rgba(255,255,255,0.08);border-radius:2px;}
table{width:100%;border-collapse:collapse;font-size:12px;}
th{padding:6px 8px;color:var(--t2);font-size:8px;text-transform:uppercase;letter-spacing:1.5px;
  font-weight:700;border-bottom:1px solid var(--border);white-space:nowrap;background:var(--bg1);}
td{padding:6px 8px;border-bottom:1px solid rgba(255,255,255,0.025);white-space:nowrap;}
.no-data{text-align:center;color:var(--t2);padding:24px;font-size:11px;}

/* ── RIGHT COLUMN ── */
.col-right{grid-column:3;display:flex;flex-direction:column;gap:8px;overflow-y:auto;}
.col-right::-webkit-scrollbar{width:3px;}
.col-right::-webkit-scrollbar-thumb{background:rgba(255,255,255,0.08);border-radius:2px;}

/* Latency */
.lat-grid{display:grid;grid-template-columns:1fr 1fr;gap:6px;padding:8px 10px;}
.lat-item{background:rgba(255,255,255,0.02);border-radius:6px;padding:8px 6px;text-align:center;}
.lat-val{font-family:'IBM Plex Mono',monospace;font-size:18px;font-weight:700;color:var(--blue);}
.lat-lbl{font-size:7px;color:var(--t2);text-transform:uppercase;letter-spacing:1px;margin-top:3px;}

/* Governor */
.gov-item{display:flex;justify-content:space-between;align-items:center;padding:5px 10px;
  border-bottom:1px solid var(--border);}
.gov-item:last-of-type{border-bottom:none;}
.gov-lbl{font-size:9px;color:var(--t2);}
.gov-bar{flex:1;height:3px;background:rgba(255,255,255,0.06);margin:0 8px;border-radius:2px;overflow:hidden;}
.gov-fill{height:100%;border-radius:2px;background:var(--amber);transition:width 0.5s;}
.gov-n{font-family:'IBM Plex Mono',monospace;font-size:11px;font-weight:700;color:var(--amber);min-width:22px;text-align:right;}

/* FIX session */
.fix-item{display:flex;justify-content:space-between;align-items:center;padding:6px 10px;
  border-bottom:1px solid var(--border);}
.fix-item:last-of-type{border-bottom:none;}
.fix-lbl{font-size:9px;color:var(--t2);text-transform:uppercase;letter-spacing:1px;}
.fix-val{font-family:'IBM Plex Mono',monospace;font-size:12px;font-weight:700;}
.fix-ok{color:var(--green)}.fix-bad{color:var(--red)}

/* Comp ranges */
.comp-grid{display:grid;grid-template-columns:1fr 1fr 1fr;gap:6px;padding:8px 10px;}
.comp-item{background:rgba(255,255,255,0.02);border-radius:6px;padding:7px 6px;text-align:center;}
.comp-sym{font-size:9px;font-weight:700;color:var(--blue);margin-bottom:3px;}
.comp-ph{font-size:8px;font-weight:700;margin-bottom:3px;}
.comp-detail{font-family:'IBM Plex Mono',monospace;font-size:8px;color:var(--t2);}
</style>
</head>
<body>
<div class="desk">

<!-- ══ HEADER ══ -->
<header>
  <div class="logo">
    <div class="logo-img"></div>
    <div>
      <div class="logo-name">Omega</div>
      <div class="logo-sub">Commodities &amp; Indices</div>
    </div>
  </div>

  <div class="hdr-tickers">
    <div class="htk gold">
      <span class="htk-sym">GOLD</span>
      <span class="htk-b" id="hdrGoldBid">----</span>
      <span class="htk-sep">|</span>
      <span class="htk-a" id="hdrGoldAsk">----</span>
      <span class="htk-ph ph-flat" id="hdrGoldPhase">--</span>
    </div>
    <div class="htk silver">
      <span class="htk-sym">SILVER</span>
      <span class="htk-b" id="hdrXagBid">----</span>
      <span class="htk-sep">|</span>
      <span class="htk-a" id="hdrXagAsk">----</span>

)OMEGA0"

R"OMEGA1(
    </div>
    <div class="vd"></div>
    <div class="htk"><span class="htk-sym">US500</span><span class="htk-b" id="hdrSpBid" style="font-size:10px">----</span><span class="htk-sep">|</span><span class="htk-a" id="hdrSpAsk" style="font-size:10px">----</span></div>
    <div class="htk"><span class="htk-sym">USTEC</span><span class="htk-b" id="hdrNqBid" style="font-size:10px">----</span><span class="htk-sep">|</span><span class="htk-a" id="hdrNqAsk" style="font-size:10px">----</span></div>
    <div class="htk"><span class="htk-sym">OIL</span><span class="htk-b" id="hdrClBid" style="font-size:10px">----</span><span class="htk-sep">|</span><span class="htk-a" id="hdrClAsk" style="font-size:10px">----</span></div>
    <div class="htk"><span class="htk-sym">BRENT</span><span class="htk-b" id="hdrBrentBid" style="font-size:10px">----</span><span class="htk-sep">|</span><span class="htk-a" id="hdrBrentAsk" style="font-size:10px">----</span></div>
    <div class="vd"></div>
    <div class="htk"><span class="htk-sym">EUR</span><span class="htk-b" id="hdrEurBid" style="font-size:10px">----</span><span class="htk-sep">|</span><span class="htk-a" id="hdrEurAsk" style="font-size:10px">----</span></div>
    <div class="htk"><span class="htk-sym">GBP</span><span class="htk-b" id="hdrGbpBid" style="font-size:10px">----</span><span class="htk-sep">|</span><span class="htk-a" id="hdrGbpAsk" style="font-size:10px">----</span></div>
    <div class="htk" style="border-color:rgba(0,200,240,0.2)"><span class="htk-sym" style="color:var(--cyan)">AUD</span><span class="htk-b" id="hdrAudBid" style="font-size:10px">----</span><span class="htk-sep">|</span><span class="htk-a" id="hdrAudAsk" style="font-size:10px">----</span></div>
    <div class="htk" style="border-color:rgba(0,200,240,0.2)"><span class="htk-sym" style="color:var(--cyan)">NZD</span><span class="htk-b" id="hdrNzdBid" style="font-size:10px">----</span><span class="htk-sep">|</span><span class="htk-a" id="hdrNzdAsk" style="font-size:10px">----</span></div>
    <div class="htk" style="border-color:rgba(164,127,255,0.2)"><span class="htk-sym" style="color:var(--purple)">JPY</span><span class="htk-b" id="hdrJpyBid" style="font-size:10px">----</span><span class="htk-sep">|</span><span class="htk-a" id="hdrJpyAsk" style="font-size:10px">----</span></div>
  </div>

  <div class="hbar">
    <span id="modeBadge" class="badge shadow">SHADOW</span>
    <span class="badge" id="uptimeBadge" style="color:var(--t2)">UP 00:00:00</span>
    <span class="badge" id="sessionBadge" style="color:var(--t2)">── UTC</span>
    <span class="badge"><span class="dot-conn dot-bad" id="connDot"></span><span id="connText">Connecting</span></span>
    <span class="badge" id="fixQuoteHdr" style="color:var(--red)">Q:--</span>
    <span style="font-family:'IBM Plex Mono',monospace;font-size:11px;color:var(--t2)" id="clock">--:--:-- UTC</span>
  </div>
</header>

<!-- ══ MAIN ══ -->
<div class="main">

  <!-- ── LEFT: Market Data + Regime ── -->
  <div class="col-left">

    <div class="card mkt-card">
      <div class="card-hd"><span class="dot" style="background:var(--cyan)"></span>Market Data</div>
      <div class="mkt-body">

        <div class="sym-section-label">★ Precious Metals</div>
        <div class="sym-row r-gold">
          <span class="sym-nm c-gold">GOLD.F</span>
          <div class="px-pair"><span class="bid" id="goldBid">----</span><span class="c-t2" style="font-size:9px">|</span><span class="ask" id="goldAsk">----</span></div>
          <span class="sprd" id="goldSpread">--</span>
        </div>
        <div class="sym-row r-silver">
          <span class="sym-nm c-silver">XAGUSD</span>
          <div class="px-pair"><span class="bid" id="xagBid">----</span><span class="c-t2" style="font-size:9px">|</span><span class="ask" id="xagAsk">----</span></div>
          <span class="sprd" id="xagSpread">--</span>
        </div>

        <div class="sym-section-label">▶ US Indices &amp; Oil</div>
        <div class="sym-row r-primary">
          <span class="sym-nm c-blue">US500.F</span>
          <div class="px-pair"><span class="bid" id="spBid">----</span><span class="c-t2" style="font-size:9px">|</span><span class="ask" id="spAsk">----</span></div>
          <span class="sprd" id="spSpread">--</span>
        </div>
        <div class="sym-row r-primary">
          <span class="sym-nm c-blue">USTEC.F</span>
          <div class="px-pair"><span class="bid" id="nqBid">----</span><span class="c-t2" style="font-size:9px">|</span><span class="ask" id="nqAsk">----</span></div>
          <span class="sprd" id="nqSpread">--</span>
        </div>
        <div class="sym-row r-primary">
          <span class="sym-nm c-blue">DJ30.F</span>
          <div class="px-pair"><span class="bid" id="djBid" style="font-size:10px">----</span><span class="c-t2" style="font-size:9px">|</span><span class="ask" id="djAsk" style="font-size:10px">----</span></div>
          <span class="sprd"></span>
        </div>
        <div class="sym-row r-primary">
          <span class="sym-nm c-blue">NAS100</span>
          <div class="px-pair"><span class="bid" id="nasBid" style="font-size:10px">----</span><span class="c-t2" style="font-size:9px">|</span><span class="ask" id="nasAsk" style="font-size:10px">----</span></div>
          <span class="sprd"></span>
        </div>
        <div class="sym-row r-primary">
          <span class="sym-nm c-blue">USOIL.F</span>
          <div class="px-pair"><span class="bid" id="clBid">----</span><span class="c-t2" style="font-size:9px">|</span><span class="ask" id="clAsk">----</span></div>
          <span class="sprd" id="clSpread">--</span>
        </div>
        <div class="sym-row r-primary">
          <span class="sym-nm c-blue">UKBRENT</span>
          <div class="px-pair"><span class="bid" id="brentBid">----</span><span class="c-t2" style="font-size:9px">|</span><span class="ask" id="brentAsk">----</span></div>
          <span class="sprd" id="brentSpread">--</span>
        </div>

        <div class="sym-section-label">◈ EU Indices</div>
        <div class="sym-row r-eu">
          <span class="sym-nm c-purple">GER30</span>
          <div class="px-pair"><span class="bid" id="ger30Bid" style="font-size:10px">----</span><span class="c-t2" style="font-size:9px">|</span><span class="ask" id="ger30Ask" style="font-size:10px">----</span></div>
          <span class="sprd"></span>
        </div>
        <div class="sym-row r-eu">
          <span class="sym-nm c-purple">UK100</span>
          <div class="px-pair"><span class="bid" id="uk100Bid" style="font-size:10px">----</span><span class="c-t2" style="font-size:9px">|</span><span class="ask" id="uk100Ask" style="font-size:10px">----</span></div>
          <span class="sprd"></span>
        </div>
        <div class="sym-row r-eu">
          <span class="sym-nm c-purple">ESTX50</span>
          <div class="px-pair"><span class="bid" id="estx50Bid" style="font-size:10px">----</span><span class="c-t2" style="font-size:9px">|</span><span class="ask" id="estx50Ask" style="font-size:10px">----</span></div>
          <span class="sprd"></span>
        </div>

        <div class="sym-section-label">⬡ FX Majors</div>
        <div class="sym-row r-fx">
          <span class="sym-nm c-cyan">EURUSD</span>
          <div class="px-pair"><span class="bid" id="eurBid">----</span><span class="c-t2" style="font-size:9px">|</span><span class="ask" id="eurAsk">----</span></div>
          <span class="sprd" id="eurSpread">--</span>
        </div>
        <div class="sym-row r-fx">
          <span class="sym-nm c-cyan">GBPUSD</span>
          <div class="px-pair"><span class="bid" id="gbpBid">----</span><span class="c-t2" style="font-size:9px">|</span><span class="ask" id="gbpAsk">----</span></div>
          <span class="sprd" id="gbpSpread">--</span>
        </div>

        <div class="sym-section-label">🌏 Asia FX</div>
        <div class="sym-row r-asia">
          <span class="sym-nm c-teal">AUDUSD</span>
          <div class="px-pair"><span class="bid" id="audBid">----</span><span class="c-t2" style="font-size:9px">|</span><span class="ask" id="audAsk">----</span></div>
          <span class="sprd" id="audSpread">--</span>
        </div>
        <div class="sym-row r-asia">
          <span class="sym-nm c-teal">NZDUSD</span>
          <div class="px-pair"><span class="bid" id="nzdBid">----</span><span class="c-t2" style="font-size:9px">|</span><span class="ask" id="nzdAsk">----</span></div>
          <span class="sprd" id="nzdSpread">--</span>
        </div>
        <div class="sym-row r-asia">
          <span class="sym-nm" style="color:var(--purple)">USDJPY</span>
          <div class="px-pair"><span class="bid" id="jpyBid">----</span><span class="c-t2" style="font-size:9px">|</span><span class="ask" id="jpyAsk">----</span></div>
          <span class="sprd" id="jpySpread">--</span>
        </div>

        <div class="sym-section-label">◈ Confirmation</div>
        <div class="sym-row">
          <span class="sym-nm c-t2">VIX.F</span>
          <div class="px-pair"><span class="bid" id="vixBid" style="font-size:10px">--</span><span class="c-t2" style="font-size:9px">|</span><span class="ask" id="vixAsk" style="font-size:10px">--</span></div>
          <span class="sprd"></span>
        </div>
        <div class="sym-row">
          <span class="sym-nm c-t2">DX.F</span>
          <div class="px-pair"><span class="bid" id="dxBid" style="font-size:10px">--</span><span class="c-t2" style="font-size:9px">|</span><span class="ask" id="dxAsk" style="font-size:10px">--</span></div>
          <span class="sprd"></span>
        </div>
        <div class="sym-row">
          <span class="sym-nm c-t2">NGAS.F</span>
          <div class="px-pair"><span class="bid" id="ngasBid" style="font-size:10px">--</span><span class="c-t2" style="font-size:9px">|</span><span class="ask" id="ngasAsk" style="font-size:10px">--</span></div>
          <span class="sprd"></span>
        </div>
      </div>
    </div>

    <!-- Regime -->
    <div class="card regime-card">
      <div class="card-hd"><span class="dot" style="background:var(--gold)"></span>Macro Regime</div>
      <div class="regime-grid">
        <div class="rg-item">
          <div class="rg-lbl">VIX</div>
          <div class="rg-val" id="vixLevel" style="color:var(--t1)">--</div>
        </div>
        <div class="rg-item">
          <div class="rg-lbl">Regime</div>
          <div class="rg-val" id="macroRegime">--</div>
        </div>
        <div class="rg-item">
          <div class="rg-lbl">ES/NQ Div</div>
          <div class="rg-val" id="esNqDiv" style="color:var(--blue)">--</div>
        </div>
        <div class="rg-item">
          <div class="rg-lbl">Session</div>
          <div class="rg-val" id="sessionVal" style="color:var(--t1)">--</div>
        </div>
      </div>
    </div>
  </div>

  <!-- ── CENTRE ── -->
  <div class="col-centre">

    <!-- Stats bar -->
    <div class="stats-bar">
      <div class="pnl-card">
        <div style="font-size:8px;color:var(--t2);text-transform:uppercase;letter-spacing:2px;">Daily P&amp;L</div>
        <div class="pnl-num pnl-pos" id="pnlVal">+$0.00</div>
        <div class="pnl-sub" id="pnlSub">0 trades · 0.0% win</div>
        <div style="font-size:8px;color:var(--t2);margin-top:2px;" id="pnlGrossSub"></div>
      </div>
      <div class="stat-card"><div class="stat-n" id="statWins" style="color:var(--green)">0</div><div class="stat-l">Wins</div></div>
      <div class="stat-card"><div class="stat-n" id="statLosses" style="color:var(--red)">0</div><div class="stat-l">Losses</div></div>
      <div class="stat-card"><div class="stat-n" id="statAvgWin" style="color:var(--teal)">$0</div><div class="stat-l">Avg Win</div></div>
      <div class="stat-card"><div class="stat-n" id="statMaxDD" style="color:var(--red)">$0</div><div class="stat-l">Max DD</div></div>
    </div>

    <!-- Engine state — ALL 15 engines in 3 groups -->
    <div class="eng-section">
      <div class="eng-section-label">⚡ US / Oil Engines</div>
      <div class="eng-grid eng-grid-5">
        <div class="eng-cell" id="engSP"><div class="eng-sym c-blue">US500</div><div class="eng-ph eph-flat" id="engSPPhase">FLAT</div><div class="eng-vol" id="engSPVol">--</div><div class="eng-sigs" id="engSPSig">0</div></div>
        <div class="eng-cell" id="engNQ"><div class="eng-sym c-blue">USTEC</div><div class="eng-ph eph-flat" id="engNQPhase">FLAT</div><div class="eng-vol" id="engNQVol">--</div><div class="eng-sigs" id="engNQSig">0</div></div>
        <div class="eng-cell" id="engUS30"><div class="eng-sym c-blue">DJ30</div><div class="eng-ph eph-flat" id="engUS30Phase">FLAT</div><div class="eng-vol" id="engUS30Vol">--</div><div class="eng-sigs" id="engUS30Sig">0</div></div>
        <div class="eng-cell" id="engNAS"><div class="eng-sym c-blue">NAS100</div><div class="eng-ph eph-flat" id="engNASPhase">FLAT</div><div class="eng-vol" id="engNASVol">--</div><div class="eng-sigs" id="engNASSig">0</div></div>
        <div class="eng-cell" id="engCL"><div class="eng-sym c-amber" style="color:var(--amber)">USOIL</div><div class="eng-ph eph-flat" id="engCLPhase">FLAT</div><div class="eng-vol" id="engCLVol">--</div><div class="eng-sigs" id="engCLSig">0</div></div>
      </div>
    </div>

    <div class="eng-section" style="margin-top:6px;">
      <div class="eng-section-label">◈ EU Indices + Brent</div>
      <div class="eng-grid" style="grid-template-columns:repeat(4,1fr)">
        <div class="eng-cell" id="engGER"><div class="eng-sym c-purple">GER30</div><div class="eng-ph eph-flat" id="engGERPhase">FLAT</div><div class="eng-vol" id="engGERVol">--</div><div class="eng-sigs" id="engGERSig">0</div></div>
        <div class="eng-cell" id="engUK"><div class="eng-sym c-purple">UK100</div><div class="eng-ph eph-flat" id="engUKPhase">FLAT</div><div class="eng-vol" id="engUKVol">--</div><div class="eng-sigs" id="engUKSig">0</div></div>
        <div class="eng-cell" id="engESTX"><div class="eng-sym c-purple">ESTX50</div><div class="eng-ph eph-flat" id="engESTXPhase">FLAT</div><div class="eng-vol" id="engESTXVol">--</div><div class="eng-sigs" id="engESTXSig">0</div></div>
        <div class="eng-cell" id="engBRENT"><div class="eng-sym" style="color:var(--amber)">BRENT</div><div class="eng-ph eph-flat" id="engBRENTPhase">FLAT</div><div class="eng-vol" id="engBRENTVol">--</div><div class="eng-sigs" id="engBRENTSig">0</div></div>
      </div>
    </div>

)OMEGA1"

R"OMEGA2(

    <div class="eng-section" style="margin-top:6px;">
      <div class="eng-section-label">⬡ FX + Asia Engines</div>
      <div class="eng-grid" style="grid-template-columns:repeat(5,1fr)">
        <div class="eng-cell" id="engEUR"><div class="eng-sym c-cyan">EURUSD</div><div class="eng-ph eph-flat" id="engEURPhase">FLAT</div><div class="eng-vol" id="engEURVol">--</div><div class="eng-sigs" id="engEURSig">0</div></div>
        <div class="eng-cell" id="engGBP"><div class="eng-sym c-cyan">GBPUSD</div><div class="eng-ph eph-flat" id="engGBPPhase">FLAT</div><div class="eng-vol" id="engGBPVol">--</div><div class="eng-sigs" id="engGBPSig">0</div></div>
        <div class="eng-cell" id="engAUD"><div class="eng-sym c-teal">AUDUSD</div><div class="eng-ph eph-flat" id="engAUDPhase">FLAT</div><div class="eng-vol" id="engAUDVol">--</div><div class="eng-sigs" id="engAUDSig">0</div></div>
        <div class="eng-cell" id="engNZD"><div class="eng-sym c-teal">NZDUSD</div><div class="eng-ph eph-flat" id="engNZDPhase">FLAT</div><div class="eng-vol" id="engNZDVol">--</div><div class="eng-sigs" id="engNZDSig">0</div></div>
        <div class="eng-cell" id="engJPY"><div class="eng-sym" style="color:var(--purple)">USDJPY</div><div class="eng-ph eph-flat" id="engJPYPhase">FLAT</div><div class="eng-vol" id="engJPYVol">--</div><div class="eng-sigs" id="engJPYSig">0</div></div>
      </div>
    </div>

    <!-- Last signal + trades -->
    <div class="sig-trade-area">
      <div class="last-sig">
        <div style="font-size:8px;color:var(--t2);text-transform:uppercase;letter-spacing:2px;margin-bottom:6px;display:flex;align-items:center;gap:6px;">
          <span style="width:4px;height:4px;border-radius:50%;background:var(--amber);flex-shrink:0;display:inline-block;"></span>Last Signal
          <button id="bellBtn" onclick="toggleBell()" style="margin-left:auto;background:rgba(255,214,0,.1);border:1px solid rgba(255,214,0,0.4);border-radius:4px;padding:1px 7px;cursor:pointer;font-size:9px;color:#ffd600;font-family:inherit;font-weight:700;">🔔 ARM BELL</button>
        </div>
        <div id="lastSignalDetail"><span style="color:var(--t2);font-size:11px;">Waiting for first signal…</span></div>
      </div>

      <div class="trades-card">
        <div class="card-hd" style="flex-shrink:0;">
          <span class="dot" style="background:var(--green)"></span>Recent Trades
          <span id="tradeCount" style="font-family:'IBM Plex Mono',monospace;color:var(--t2);margin-left:6px;font-size:9px;"></span>
        </div>
        <div class="trades-scroll">
          <table>
            <thead><tr>
              <th>Time</th><th>Symbol</th><th>Side</th><th>Entry</th><th>Exit</th>
              <th>Held</th><th>Result</th><th>Gross</th><th>Slip</th><th>Net</th>
            </tr></thead>
            <tbody id="tradesBody"><tr><td colspan="10" class="no-data">No trades yet</td></tr></tbody>
          </table>
        </div>
      </div>
    </div>
  </div>

  <!-- ── RIGHT COLUMN ── -->
  <div class="col-right">

    <!-- FIX Latency -->
    <div class="card">
      <div class="card-hd"><span class="dot" style="background:var(--blue)"></span>FIX Latency</div>
      <div class="lat-grid">
        <div class="lat-item"><div class="lat-val" id="rttLast">--</div><div class="lat-lbl">Last ms</div></div>
        <div class="lat-item"><div class="lat-val" id="rttP50">--</div><div class="lat-lbl">P50 ms</div></div>
        <div class="lat-item"><div class="lat-val" id="rttP95">--</div><div class="lat-lbl">P95 ms</div></div>
        <div class="lat-item"><div class="lat-val" id="msgRate">--</div><div class="lat-lbl">msg/s</div></div>
      </div>
    </div>

    <!-- FIX Session -->
    <div class="card">
      <div class="card-hd"><span class="dot" style="background:var(--purple)"></span>FIX Session</div>
      <div class="fix-item"><span class="fix-lbl">Quote</span><span class="fix-val fix-bad" id="fixQStatus">--</span></div>
      <div class="fix-item"><span class="fix-lbl">Mode</span><span class="fix-val" id="fixModeRight" style="color:var(--amber)">--</span></div>
      <div class="fix-item"><span class="fix-lbl">Seq Gaps</span><span class="fix-val" id="fixGaps" style="color:var(--t1)">0</span></div>
      <div class="fix-item"><span class="fix-lbl">Orders</span><span class="fix-val" id="fixOrders" style="color:var(--t1)">0</span></div>
      <div class="fix-item"><span class="fix-lbl">Fills</span><span class="fix-val" id="fixFills" style="color:var(--t1)">0</span></div>
      <div class="fix-item"><span class="fix-lbl">Build</span><span class="fix-val" id="buildVersion" style="color:var(--t2);font-size:9px">...</span></div>
    </div>

    <!-- Governor Blocks -->
    <div class="card">
      <div class="card-hd"><span class="dot" style="background:var(--amber)"></span>Governor Blocks</div>
      <div class="gov-item"><span class="gov-lbl">SPREAD</span><div class="gov-bar"><div class="gov-fill" id="gbarSpread" style="width:0%"></div></div><span class="gov-n" id="gnSpread">0</span></div>
      <div class="gov-item"><span class="gov-lbl">LATENCY</span><div class="gov-bar"><div class="gov-fill" id="gbarLat" style="width:0%"></div></div><span class="gov-n" id="gnLat">0</span></div>
      <div class="gov-item"><span class="gov-lbl">PNL LIM</span><div class="gov-bar"><div class="gov-fill" id="gbarPnl" style="width:0%"></div></div><span class="gov-n" id="gnPnl">0</span></div>
      <div class="gov-item"><span class="gov-lbl">POSITION</span><div class="gov-bar"><div class="gov-fill" id="gbarPos" style="width:0%"></div></div><span class="gov-n" id="gnPos">0</span></div>
      <div class="gov-item"><span class="gov-lbl">CONSEC</span><div class="gov-bar"><div class="gov-fill" id="gbarConsec" style="width:0%"></div></div><span class="gov-n" id="gnConsec">0</span></div>
      <div style="font-size:8px;color:var(--t2);padding:5px 10px;text-align:right;" id="govTotal">Total: 0</div>
    </div>

    <!-- Compression Ranges (SP/NQ/CL) -->
    <div class="card">
      <div class="card-hd"><span class="dot" style="background:var(--cyan)"></span>Compression Ranges</div>
      <div class="comp-grid">
        <div class="comp-item"><div class="comp-sym">US500</div><div class="comp-ph" id="compSPPh" style="color:var(--t2)">FLAT</div><div class="comp-detail" id="compSPDet">--</div></div>
        <div class="comp-item"><div class="comp-sym">USTEC</div><div class="comp-ph" id="compNQPh" style="color:var(--t2)">FLAT</div><div class="comp-detail" id="compNQDet">--</div></div>
        <div class="comp-item"><div class="comp-sym">USOIL</div><div class="comp-ph" id="compCLPh" style="color:var(--t2)">FLAT</div><div class="comp-detail" id="compCLDet">--</div></div>
      </div>
    </div>

    <!-- Gold engine state summary -->
    <div class="card">
      <div class="card-hd"><span class="dot" style="background:var(--gold)"></span>Gold Stack</div>
      <div style="padding:8px 10px;display:grid;grid-template-columns:1fr 1fr;gap:6px;">
        <div class="rg-item"><div class="rg-lbl">Phase</div><div class="rg-val" id="goldPhaseRight" style="font-size:11px;color:var(--gold)">--</div></div>
        <div class="rg-item"><div class="rg-lbl">Signals</div><div class="rg-val" id="goldSigsRight" style="color:var(--gold)">0</div></div>
        <div class="rg-item"><div class="rg-lbl">rv%</div><div class="rg-val" id="goldRvRight" style="color:var(--t1);font-size:11px">--</div></div>
        <div class="rg-item"><div class="rg-lbl">bv%</div><div class="rg-val" id="goldBvRight" style="color:var(--t1);font-size:11px">--</div></div>
      </div>
    </div>

    <!-- Silver bracket engine -->
    <div class="card">
      <div class="card-hd"><span class="dot" style="background:var(--silver)"></span>Silver Bracket</div>
      <div style="padding:8px 10px;display:grid;grid-template-columns:1fr 1fr;gap:6px;">
        <div class="rg-item"><div class="rg-lbl">Phase</div><div class="rg-val" id="xagPhaseRight" style="font-size:11px;color:var(--silver)">--</div></div>
        <div class="rg-item"><div class="rg-lbl">Signals</div><div class="rg-val" id="xagSigsRight" style="color:var(--silver)">0</div></div>
      </div>
    </div>

  </div><!-- /col-right -->

</div><!-- /main -->
</div><!-- /desk -->

<script>
'use strict';
let wsConnected=false,lastData={},_bellEnabled=false,_lastTradeCount=0,_bellBootCount=-1,_audioCtx=null;

function safe(v,d=0){const n=Number(v);return isNaN(n)?d:n;}
function fmtUTC(ts){if(!ts)return '--';return new Date(ts*1000).toUTCString().slice(17,25);}

function toggleBell(){
  _bellEnabled=!_bellEnabled;
  const b=document.getElementById('bellBtn');
  if(b){b.textContent=_bellEnabled?'🔔 ARMED':'🔔 ARM BELL';b.style.color=_bellEnabled?'var(--green)':'#ffd600';b.style.borderColor=_bellEnabled?'rgba(0,217,126,0.4)':'rgba(255,214,0,0.4)';}
  if(_bellEnabled&&!_audioCtx){try{_audioCtx=new(window.AudioContext||window.webkitAudioContext)();if(_audioCtx.state==='suspended')_audioCtx.resume();}catch(e){}}
  if(_bellEnabled&&_audioCtx)_playTestBell();
}
function _playTestBell(){try{const ctx=_audioCtx;if(!ctx)return;if(ctx.state==='suspended')ctx.resume();const t=ctx.currentTime,o=ctx.createOscillator(),g=ctx.createGain();o.connect(g);g.connect(ctx.destination);o.type='sine';o.frequency.value=880;g.gain.setValueAtTime(0,t);g.gain.linearRampToValueAtTime(0.35,t+0.01);g.gain.exponentialRampToValueAtTime(0.001,t+0.35);o.start(t);o.stop(t+0.4);}catch(e){}}
function _playWinBell(){if(!_bellEnabled||!_audioCtx)return;try{const ctx=_audioCtx;if(ctx.state==='suspended')ctx.resume();const t=ctx.currentTime;[[0,880,1040],[0.2,1100,1320]].forEach(([dt,f1,f2])=>{[f1,f2].forEach((freq,i)=>{const o=ctx.createOscillator(),g=ctx.createGain();o.connect(g);g.connect(ctx.destination);o.type='sine';o.frequency.value=freq;g.gain.setValueAtTime(0,t+dt);g.gain.linearRampToValueAtTime(i?0.8:1.6,t+dt+0.008);g.gain.exponentialRampToValueAtTime(0.001,t+dt+1.2);o.start(t+dt);o.stop(t+dt+1.3);});});}catch(e){}}
function _playLossBell(){if(!_bellEnabled||!_audioCtx)return;try{const ctx=_audioCtx;if(ctx.state==='suspended')ctx.resume();const t=ctx.currentTime,o=ctx.createOscillator(),g=ctx.createGain();o.connect(g);g.connect(ctx.destination);o.type='sawtooth';o.frequency.setValueAtTime(280,t);o.frequency.linearRampToValueAtTime(130,t+0.3);g.gain.setValueAtTime(0,t);g.gain.linearRampToValueAtTime(0.8,t+0.01);g.gain.exponentialRampToValueAtTime(0.001,t+0.5);o.start(t);o.stop(t+0.55);}catch(e){}}

function px(id,val,dec){const el=document.getElementById(id);if(!el)return;const v=safe(val);if(v>0)el.textContent=v.toFixed(dec||2);}
function sprd(id,bid,ask){const el=document.getElementById(id);if(!el)return;const b=safe(bid),a=safe(ask);if(b>0&&a>0)el.textContent=(a-b).toFixed(2);}
function txt(id,v){const el=document.getElementById(id);if(el)el.textContent=v;}

function setHdrPhase(id,phase){
  const el=document.getElementById(id);if(!el)return;
  const p=safe(phase);
  if(p===0){el.className='htk-ph ph-flat';el.textContent='FLAT';}
  else if(p===1){el.className='htk-ph ph-comp';el.textContent='COMP';}
  else{el.className='htk-ph ph-brk';el.textContent='BRK ⚡';}
}

function updateEngCell(cellId,phaseId,volId,sigId,phase,rv,bv,sigs,hi,lo){
  const cell=document.getElementById(cellId),ph=document.getElementById(phaseId),vol=document.getElementById(volId),sig=document.getElementById(sigId);
  if(!cell)return;
  const p=safe(phase);
  cell.className='eng-cell'+(p===1?' ph1':p===2?' ph2':'');
  if(ph){
    if(p===0){ph.className='eng-ph eph-flat';ph.textContent='FLAT';}
    else if(p===1){ph.className='eng-ph eph-comp';ph.textContent='COMP';}
    else{ph.className='eng-ph eph-brk';ph.textContent='BRK⚡';}
  }
  if(vol){
    if(p===1&&safe(hi)>0) vol.textContent=safe(hi).toFixed(1)+'↑'+safe(lo).toFixed(1);
    else vol.textContent='r'+safe(rv).toFixed(2)+'%';
  }
  if(sig)sig.textContent=sigs+'sig';
}

function renderLastSignal(d){
  const el=document.getElementById('lastSignalDetail');if(!el)return;
  if(!d.last_signal_side||d.last_signal_side==='NONE'||d.last_signal_side==='CLOSED'||!d.last_signal_side){
    el.innerHTML='<span style="color:var(--t2);font-size:11px;">Waiting for first signal…</span>';return;}
  const sc=d.last_signal_side==='LONG'?'var(--green)':'var(--red)';
  el.innerHTML=`<div class="sig-row">
    <div><div class="sig-field-lbl">Symbol</div><div class="sig-field-val" style="color:var(--blue)">${d.last_signal_symbol||'--'}</div></div>
    <div><div class="sig-field-lbl">Direction</div><div class="sig-field-val" style="color:${sc}">${d.last_signal_side}</div></div>
    <div><div class="sig-field-lbl">Price</div><div class="sig-field-val" style="color:var(--t1)">${safe(d.last_signal_price).toFixed(2)}</div></div>
    <div><div class="sig-field-lbl">Reason</div><div style="font-family:'IBM Plex Mono',monospace;font-size:11px;color:var(--gold);margin-top:4px;">${d.last_signal_reason||'--'}</div></div>
  </div>`;
}

function renderTrades(trades){
  const el=document.getElementById('tradesBody'),cE=document.getElementById('tradeCount');
  if(!trades||trades.length===0){el.innerHTML='<tr><td colspan="10" class="no-data">No trades yet</td></tr>';if(cE)cE.textContent='';return;}
  const closed=trades.filter(t=>t.exitReason&&t.exitReason!=='');
  if(_bellBootCount<0){_bellBootCount=closed.length;_lastTradeCount=closed.length;}
  if(_bellEnabled&&closed.length>_lastTradeCount&&_lastTradeCount>=_bellBootCount){const pnl=safe(closed[0].net_pnl);pnl>0?_playWinBell():_playLossBell();}
  _lastTradeCount=closed.length;
  const wins=closed.filter(t=>safe(t.net_pnl)>0).length,losses=closed.filter(t=>safe(t.net_pnl)<0).length,totalNet=closed.reduce((s,t)=>s+safe(t.net_pnl),0);
  // BUG FIX 1: trade count header — was (totalNet>=0?'+':'') now (totalNet>=0?'+':'-')
  if(cE)cE.textContent=closed.length+' closed · '+wins+'W/'+losses+'L · '+(totalNet>=0?'+':'-')+'$'+Math.abs(totalNet).toFixed(2);
  const now=Math.floor(Date.now()/1000);
  el.innerHTML=trades.slice(0,60).map(t=>{
    const isOpen=!t.exitReason||t.exitReason==='',net=safe(t.net_pnl),gross=safe(t.pnl),slip=safe(t.slippage_entry)+safe(t.slippage_exit);
    const win=net>0,loss=net<0,sc=t.side==='LONG'?'var(--green)':'var(--red)';
    const reason=t.exitReason||'',result=isOpen?'🔵':reason==='TP_HIT'?'✅TP':reason==='SL_HIT'?'❌SL':reason==='TIMEOUT'?'⏱TO':'⚡FC';
    const rc=isOpen?'var(--blue)':win?'var(--green)':loss?'var(--red)':'var(--t2)';
    const netC=win?'var(--green)':loss?'var(--red)':'var(--t2)';
    let heldStr='--';
    if(isOpen&&safe(t.entryTs)>0){const s=now-safe(t.entryTs);heldStr=s>=60?Math.floor(s/60)+'m'+(s%60)+'s':s+'s';}

)OMEGA2"

R"OMEGA3(
    else if(safe(t.entryTs)>0&&safe(t.exitTs)>0){const s=safe(t.exitTs)-safe(t.entryTs);heldStr=s>=60?Math.floor(s/60)+'m'+(s%60)+'s':s+'s';}
    const rowBg=isOpen?'rgba(46,168,255,0.06)':win?'rgba(0,217,126,0.05)':loss?'rgba(255,51,85,0.05)':'';
    // BUG FIX 2: gross column — was (gross>=0?'+':'') now (gross>=0?'+':'-')
    const grossD=isOpen?'':(gross>=0?'+':'-')+'$'+Math.abs(gross).toFixed(2);
    const slipD=isOpen?'':slip>0?'-$'+slip.toFixed(2):'--';
    // BUG FIX 3: net column — was (net>=0?'+':'') now (net>=0?'+':'-')
    const netD=isOpen?'<span style="color:var(--t2);font-size:10px">live</span>':(net>=0?'+':'-')+'$'+Math.abs(net).toFixed(2);
    return `<tr style="background:${rowBg}">
      <td style="color:var(--t2);font-size:10px">${fmtUTC(safe(t.entryTs))}</td>
      <td style="color:var(--blue);font-weight:700">${t.symbol||'--'}</td>
      <td style="color:${sc};font-weight:700">${t.side||'--'}</td>
      <td style="font-family:'IBM Plex Mono',monospace;font-size:11px">${safe(t.price)>0?safe(t.price).toFixed(2):'--'}</td>
      <td style="font-family:'IBM Plex Mono',monospace;color:var(--t2);font-size:11px">${isOpen?'<span style="color:var(--blue);font-size:10px">open</span>':safe(t.exitPrice)>0?safe(t.exitPrice).toFixed(2):'--'}</td>
      <td style="color:var(--t2);font-size:10px">${heldStr}</td>
      <td style="font-weight:700;color:${rc}">${result}</td>
      <td style="font-family:'IBM Plex Mono',monospace;color:${gross>=0?'var(--green)':'var(--red)'};font-size:11px">${grossD}</td>
      <td style="font-family:'IBM Plex Mono',monospace;color:var(--red);font-size:10px">${slipD}</td>
      <td style="font-family:'IBM Plex Mono',monospace;color:${netC};font-weight:700">${netD}</td>
    </tr>`;
  }).join('');
}

function updateDashboard(d){
  lastData=d;setConn(true);

  // Header
  px('hdrGoldBid',d.gold_bid,2);px('hdrGoldAsk',d.gold_ask,2);setHdrPhase('hdrGoldPhase',d.xau_phase);
  px('hdrXagBid',d.xag_bid,3);px('hdrXagAsk',d.xag_ask,3);
  px('hdrSpBid',d.sp_bid,2);px('hdrSpAsk',d.sp_ask,2);
  px('hdrNqBid',d.nq_bid,2);px('hdrNqAsk',d.nq_ask,2);
  px('hdrClBid',d.cl_bid,2);px('hdrClAsk',d.cl_ask,2);
  px('hdrBrentBid',d.brent_bid,2);px('hdrBrentAsk',d.brent_ask,2);
  px('hdrEurBid',d.eurusd_bid,5);px('hdrEurAsk',d.eurusd_ask,5);
  px('hdrGbpBid',d.gbpusd_bid,5);px('hdrGbpAsk',d.gbpusd_ask,5);
  px('hdrAudBid',d.audusd_bid,5);px('hdrAudAsk',d.audusd_ask,5);
  px('hdrNzdBid',d.nzdusd_bid,5);px('hdrNzdAsk',d.nzdusd_ask,5);
  px('hdrJpyBid',d.usdjpy_bid,3);px('hdrJpyAsk',d.usdjpy_ask,3);

  // Left column
  px('goldBid',d.gold_bid,2);px('goldAsk',d.gold_ask,2);sprd('goldSpread',d.gold_bid,d.gold_ask);
  px('xagBid',d.xag_bid,3);px('xagAsk',d.xag_ask,3);sprd('xagSpread',d.xag_bid,d.xag_ask);
  px('spBid',d.sp_bid,2);px('spAsk',d.sp_ask,2);sprd('spSpread',d.sp_bid,d.sp_ask);
  px('nqBid',d.nq_bid,2);px('nqAsk',d.nq_ask,2);sprd('nqSpread',d.nq_bid,d.nq_ask);
  px('djBid',d.dj_bid,2);px('djAsk',d.dj_ask,2);
  px('nasBid',d.nas_bid,2);px('nasAsk',d.nas_ask,2);
  px('clBid',d.cl_bid,2);px('clAsk',d.cl_ask,2);sprd('clSpread',d.cl_bid,d.cl_ask);
  px('brentBid',d.brent_bid,2);px('brentAsk',d.brent_ask,2);sprd('brentSpread',d.brent_bid,d.brent_ask);
  px('ger30Bid',d.ger30_bid,2);px('ger30Ask',d.ger30_ask,2);
  px('uk100Bid',d.uk100_bid,2);px('uk100Ask',d.uk100_ask,2);
  px('estx50Bid',d.estx50_bid,2);px('estx50Ask',d.estx50_ask,2);
  px('eurBid',d.eurusd_bid,5);px('eurAsk',d.eurusd_ask,5);sprd('eurSpread',d.eurusd_bid,d.eurusd_ask);
  px('gbpBid',d.gbpusd_bid,5);px('gbpAsk',d.gbpusd_ask,5);sprd('gbpSpread',d.gbpusd_bid,d.gbpusd_ask);
  px('audBid',d.audusd_bid,5);px('audAsk',d.audusd_ask,5);sprd('audSpread',d.audusd_bid,d.audusd_ask);
  px('nzdBid',d.nzdusd_bid,5);px('nzdAsk',d.nzdusd_ask,5);sprd('nzdSpread',d.nzdusd_bid,d.nzdusd_ask);
  px('jpyBid',d.usdjpy_bid,3);px('jpyAsk',d.usdjpy_ask,3);sprd('jpySpread',d.usdjpy_bid,d.usdjpy_ask);
  px('vixBid',d.vix_bid,2);px('vixAsk',d.vix_ask,2);
  px('dxBid',d.dx_bid,2);px('dxAsk',d.dx_ask,2);
  px('ngasBid',d.ngas_bid,2);px('ngasAsk',d.ngas_ask,2);

  // Engine cells — US/Oil group (sp_phase etc from telemetry)
  updateEngCell('engSP','engSPPhase','engSPVol','engSPSig',d.sp_phase,d.sp_recent_vol_pct,d.sp_baseline_vol_pct,d.sp_signals,d.sp_comp_high,d.sp_comp_low);
  updateEngCell('engNQ','engNQPhase','engNQVol','engNQSig',d.nq_phase,d.nq_recent_vol_pct,d.nq_baseline_vol_pct,d.nq_signals,d.nq_comp_high,d.nq_comp_low);
  updateEngCell('engCL','engCLPhase','engCLVol','engCLSig',d.cl_phase,d.cl_recent_vol_pct,d.cl_baseline_vol_pct,d.cl_signals,d.cl_comp_high,d.cl_comp_low);
  // These engines don't have dedicated phase fields in telemetry yet — show placeholder
  ['engUS30','engNAS','engGER','engUK','engESTX','engBRENT','engEUR','engGBP','engAUD','engNZD','engJPY'].forEach(id=>{
    const cell=document.getElementById(id);if(cell&&cell.className==='eng-cell')cell.className='eng-cell';
  });

  // Gold stack (xau_ fields)
  const xauP=safe(d.xau_phase);
  const xauPh=xauP===0?'FLAT':xauP===1?'COMP':'BRK⚡';
  txt('goldPhaseRight',xauPh);txt('goldSigsRight',d.xau_signals||0);
  txt('goldRvRight',safe(d.xau_recent_vol_pct).toFixed(3)+'%');
  txt('goldBvRight',safe(d.xau_baseline_vol_pct).toFixed(3)+'%');
  // Silver bracket phase not in telemetry — show from xag engine if available
  txt('xagPhaseRight','--');txt('xagSigsRight','--');

  // PnL
  const pnl=safe(d.daily_pnl),gross=safe(d.gross_daily_pnl);
  const pE=document.getElementById('pnlVal');
  if(pE){pE.textContent=(pnl>=0?'+':'-')+'$'+Math.abs(pnl).toFixed(2);pE.className='pnl-num '+(pnl>=0?'pnl-pos':'pnl-neg');}
  txt('pnlSub',safe(d.total_trades)+' trades · '+safe(d.win_rate).toFixed(1)+'% win');
  const gSub=document.getElementById('pnlGrossSub');
  if(gSub&&gross!==0){const slip=Math.abs(gross-pnl);gSub.textContent='gross '+(gross>=0?'+':'-')+'$'+Math.abs(gross).toFixed(2)+' · slip -$'+slip.toFixed(2);}
  const sw=document.getElementById('statWins'),sl=document.getElementById('statLosses');
  if(sw)sw.textContent=safe(d.wins);if(sl)sl.textContent=safe(d.losses);
  const saw=document.getElementById('statAvgWin'),smd=document.getElementById('statMaxDD');
  if(saw)saw.textContent='$'+safe(d.avg_win).toFixed(0);
  if(smd)smd.textContent='$'+safe(d.max_drawdown).toFixed(0);

  // Latency
  const rl=safe(d.fix_rtt_last),r50=safe(d.fix_rtt_p50),r95=safe(d.fix_rtt_p95);
  const rE=document.getElementById('rttLast');
  if(rE){rE.textContent=rl>0?rl.toFixed(1):'--';rE.style.color=rl<10?'var(--green)':rl<30?'var(--amber)':'var(--red)';}
  const r50E=document.getElementById('rttP50');if(r50E)r50E.textContent=r50>0?r50.toFixed(1):'--';
  const r95E=document.getElementById('rttP95');if(r95E)r95E.textContent=r95>0?r95.toFixed(1):'--';
  txt('msgRate',safe(d.quote_msg_rate)||'--');

  // FIX
  const qOk=(d.fix_quote_status||'').includes('CONNECTED');
  const fq=document.getElementById('fixQStatus');
  if(fq){fq.textContent=d.fix_quote_status||'--';fq.className='fix-val '+(qOk?'fix-ok':'fix-bad');}
  const hq=document.getElementById('fixQuoteHdr');
  if(hq){hq.textContent='Q:'+(qOk?'OK':'--');hq.style.color=qOk?'var(--green)':'var(--red)';}
  txt('fixGaps',safe(d.sequence_gaps));txt('fixOrders',safe(d.total_orders));txt('fixFills',safe(d.total_fills));
  const mr=document.getElementById('fixModeRight');if(mr){mr.textContent=d.mode||'--';mr.style.color=d.mode==='LIVE'?'var(--green)':'var(--amber)';}

  // Mode badge
  const mb=document.getElementById('modeBadge');
  if(mb){mb.textContent=d.mode||'SHADOW';mb.className='badge '+(d.mode==='LIVE'?'live':'shadow');}

  // Build version
  const bv=document.getElementById('buildVersion');
  if(bv&&d.build_version)bv.textContent=d.build_version.slice(0,8);

  // Session badge
  const sb=document.getElementById('sessionBadge');
  if(sb){const now=new Date(),m=now.getUTCHours()*60+now.getUTCMinutes();
    let sess,col;
    if(m>=420&&m<630){sess='LONDON';col='var(--green)';}
    else if(m>=630&&m<780){sess='OVERLAP';col='var(--amber)';}
    else if(m>=780&&m<1080){sess='NEW YORK';col='var(--green)';}
    else if(m>=300&&m<420){sess='DEAD ZONE';col='var(--red)';}
    else{sess='ASIAN';col='var(--cyan)';}
    sb.textContent=sess;sb.style.color=col;}

  // Uptime
  const ub=document.getElementById('uptimeBadge');
  if(ub&&d.uptime_sec!=null){const s=d.uptime_sec,h=Math.floor(s/3600),m=Math.floor((s%3600)/60),sc=s%60;
    ub.textContent='UP '+String(h).padStart(2,'0')+':'+String(m).padStart(2,'0')+':'+String(sc).padStart(2,'0');
    ub.style.color=h>=1?'var(--green)':'var(--t2)';}

  // Governor
  const gs=safe(d.gov_spread),gl=safe(d.gov_latency),gp=safe(d.gov_pnl),gpos=safe(d.gov_positions),gc=safe(d.gov_consec_loss);
  const maxG=Math.max(1,gs,gl,gp,gpos,gc);
  [['Spread',gs],['Lat',gl],['Pnl',gp],['Pos',gpos],['Consec',gc]].forEach(([id,n])=>{
    const b=document.getElementById('gbar'+id),ni=document.getElementById('gn'+id);
    if(b)b.style.width=(n/maxG*100)+'%';if(ni)ni.textContent=n;});
  txt('govTotal','Total: '+(gs+gl+gp+gpos+gc));

  // Regime
  const vix=safe(d.vix_level),reg=d.macro_regime||'NEUTRAL',div=safe(d.es_nq_divergence);
  const vE=document.getElementById('vixLevel');
  if(vE){vE.textContent=vix>0?vix.toFixed(1):'--';vE.style.color=vix>=25?'var(--red)':vix<=15?'var(--green)':'var(--amber)';}
  const rE2=document.getElementById('macroRegime');
  if(rE2){rE2.textContent=reg;rE2.style.color=reg==='RISK_ON'?'var(--green)':reg==='RISK_OFF'?'var(--red)':'var(--amber)';}
  const dE=document.getElementById('esNqDiv');
  if(dE){dE.textContent=(div>=0?'+':'')+(div*100).toFixed(3)+'%';dE.style.color=Math.abs(div)<0.0002?'var(--t2)':div>0?'var(--green)':'var(--red)';}
  const sE=document.getElementById('sessionVal');
  if(sE){const trd=safe(d.session_tradeable);sE.textContent=trd?(d.session_name||'ACTIVE'):'CLOSED';sE.style.color=trd?'var(--green)':'var(--t2)';}

  // Comp ranges
  [{id:'SP',phase:d.sp_phase,hi:d.sp_comp_high,lo:d.sp_comp_low,rv:d.sp_recent_vol_pct},
   {id:'NQ',phase:d.nq_phase,hi:d.nq_comp_high,lo:d.nq_comp_low,rv:d.nq_recent_vol_pct},
   {id:'CL',phase:d.cl_phase,hi:d.cl_comp_high,lo:d.cl_comp_low,rv:d.cl_recent_vol_pct}
  ].forEach(s=>{
    const p=safe(s.phase),col=p===1?'var(--amber)':p===2?'var(--green)':'var(--t2)',label=p===0?'FLAT':p===1?'COMP':'BRK';
    const phEl=document.getElementById('comp'+s.id+'Ph');if(phEl){phEl.textContent=label;phEl.style.color=col;}
    const dtEl=document.getElementById('comp'+s.id+'Det');
    if(dtEl){if(p===1&&safe(s.hi)>0)dtEl.textContent=safe(s.hi).toFixed(2)+'↑'+safe(s.lo).toFixed(2);
    else dtEl.textContent='rv='+safe(s.rv).toFixed(3)+'%';}
  });

  renderLastSignal(d);
}

function setConn(ok){
  const d=document.getElementById('connDot'),t=document.getElementById('connText');
  if(d)d.className='dot-conn '+(ok?'dot-ok':'dot-bad');
  if(t)t.textContent=ok?(wsConnected?'Live':'Connected'):'Reconnecting';}

let wsFailCount=0,wsGiveUp=false;
function connectWS(){
  if(wsGiveUp)return;
  const ws=new WebSocket('ws://'+window.location.hostname+':7780');
  ws.onopen=()=>{wsConnected=true;wsFailCount=0;setConn(true);};
  ws.onmessage=e=>{try{updateDashboard(JSON.parse(e.data));}catch(_){}};
  ws.onerror=()=>{wsFailCount++;};
  ws.onclose=()=>{wsConnected=false;setConn(false);if(wsFailCount>=5){wsGiveUp=true;return;}setTimeout(connectWS,Math.min(2000*Math.pow(2,wsFailCount),15000));};
}
function httpPoll(){if(wsConnected)return;fetch('/api/telemetry').then(r=>r.json()).then(updateDashboard).catch(()=>setConn(false));}
function pollTrades(){fetch('/api/trades').then(r=>r.json()).then(renderTrades).catch(()=>{});}

setInterval(()=>{const el=document.getElementById('clock');if(el)el.textContent=new Date().toUTCString().slice(17,25)+' UTC';},1000);
connectWS();
setInterval(httpPoll,1000);
setInterval(pollTrades,5000);
pollTrades();
</script>
</body>
</html>

)OMEGA3"

;
} // namespace omega_gui
