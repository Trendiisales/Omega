#pragma once
// AUTO-GENERATED -- split into chunks to stay under MSVC 16KB string literal limit.
namespace omega_gui {
static const char* INDEX_HTML =
R"OMEGA0(


<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="Cache-Control" content="no-store,no-cache,must-revalidate">
<title>Omega -- Trading Desk</title>
<link rel="icon" type="image/svg+xml" href="data:image/svg+xml;base64,PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHZpZXdCb3g9IjAgMCAzMiAzMiI+CiAgPHJlY3Qgd2lkdGg9IjMyIiBoZWlnaHQ9IjMyIiByeD0iNiIgZmlsbD0iIzA1MDgwZCIvPgogIDx0ZXh0IHg9IjE2IiB5PSIyMyIgZm9udC1mYW1pbHk9InNlcmlmIiBmb250LXNpemU9IjIwIiBmb250LXdlaWdodD0iYm9sZCIgZmlsbD0iI2Y1Yzg0MiIgdGV4dC1hbmNob3I9Im1pZGRsZSI+zqk8L3RleHQ+Cjwvc3ZnPg==">
<link rel="preconnect" href="https://fonts.googleapis.com">
<link href="https://fonts.googleapis.com/css2?family=IBM+Plex+Mono:wght@400;600;700&family=Syne:wght@400;600;700;800&display=swap" rel="stylesheet">
<style>
*,*::before,*::after{margin:0;padding:0;box-sizing:border-box}
:root{
  --bg0:#05080d;--bg1:#090d14;--bg2:#0d1219;--bg3:#111720;
  --glass:rgba(11,16,26,0.88);--border:rgba(255,255,255,0.06);--border2:rgba(255,255,255,0.1);
  --t1:#e8edf5;--t2:#8a9ab8;--t3:#4a5878;
  --gold:#f5c842;--gold2:#ffe680;--gold-dim:rgba(245,200,66,0.12);
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

/* ?? LAYOUT ?? */
.desk{display:grid;grid-template-rows:52px 1fr;height:100vh;gap:0;padding:8px;gap:8px;}
.main{display:grid;grid-template-columns:320px 1fr 300px;gap:8px;overflow:hidden;min-height:0;}

/* ?? CARD ?? */
.card{background:var(--glass);border:1px solid var(--border);border-radius:10px;
  backdrop-filter:blur(20px);position:relative;overflow:hidden;}
.card-hd{font-size:10px;font-weight:700;text-transform:uppercase;letter-spacing:2.5px;
  color:var(--t2);padding:10px 12px 6px;display:flex;align-items:center;gap:6px;border-bottom:1px solid var(--border);}
.card-hd .dot{width:4px;height:4px;border-radius:50%;flex-shrink:0;}
.card-body{padding:10px 12px;}

/* ?? HEADER ?? */
header{background:var(--glass);border:1px solid var(--border);border-radius:10px;
  display:grid;grid-template-columns:auto 1fr auto;align-items:center;gap:12px;
  padding:0 14px;backdrop-filter:blur(24px);box-shadow:0 2px 20px rgba(0,0,0,0.5);}
.logo{display:flex;align-items:center;gap:9px;flex-shrink:0;}
.logo-img{width:28px;height:28px;background:url('data:image/svg+xml;base64,PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHZpZXdCb3g9IjAgMCAzMiAzMiI+CiAgPHJlY3Qgd2lkdGg9IjMyIiBoZWlnaHQ9IjMyIiByeD0iNiIgZmlsbD0iIzA1MDgwZCIvPgogIDx0ZXh0IHg9IjE2IiB5PSIyMyIgZm9udC1mYW1pbHk9InNlcmlmIiBmb250LXNpemU9IjIwIiBmb250LXdlaWdodD0iYm9sZCIgZmlsbD0iI2Y1Yzg0MiIgdGV4dC1hbmNob3I9Im1pZGRsZSI+zqk8L3RleHQ+Cjwvc3ZnPg==') center/contain no-repeat;
  filter:drop-shadow(0 0 6px rgba(46,168,255,0.5));}
.logo-name{font-family:'IBM Plex Mono',monospace;font-size:13px;font-weight:700;
  letter-spacing:2px;background:linear-gradient(120deg,var(--cyan),var(--blue) 50%,var(--purple));
  -webkit-background-clip:text;-webkit-text-fill-color:transparent;}
.logo-sub{font-size:9px;color:var(--t2);letter-spacing:3px;text-transform:uppercase;margin-top:1px;}

/* ?? HEADER TICKERS ?? */
.hdr-tickers{display:flex;align-items:center;gap:4px;justify-content:center;overflow:hidden;}
.htk{display:flex;align-items:center;gap:4px;padding:4px 8px;border-radius:6px;
  border:1px solid var(--border);background:rgba(255,255,255,0.02);white-space:nowrap;cursor:default;}
.htk.gold{border-color:rgba(245,200,66,0.3);background:var(--gold-dim);}
.htk-sym{font-size:10px;font-weight:700;letter-spacing:1px;color:var(--t2);}
.htk.gold .htk-sym{color:var(--gold);}
.htk-b{font-family:'IBM Plex Mono',monospace;font-size:13px;font-weight:700;color:var(--green);}
.htk-a{font-family:'IBM Plex Mono',monospace;font-size:13px;font-weight:700;color:var(--red);}
.htk-sep{color:var(--t3);font-size:11px;}
.htk-ph{font-size:9px;font-weight:700;letter-spacing:0.5px;padding:1px 4px;border-radius:3px;margin-left:2px;}
.ph-flat{background:rgba(255,255,255,0.05);color:var(--t2);}
.ph-comp{background:var(--amber-dim);color:var(--amber);}
.ph-brk{background:var(--green-dim);color:var(--green);animation:brk-blink 0.7s ease-in-out infinite;}
@keyframes brk-blink{0%,100%{opacity:1}50%{opacity:0.5}}
.pnl-live-dot{display:inline-block;width:7px;height:7px;border-radius:50%;background:var(--green);margin-right:5px;animation:pnl-pulse 1s ease-in-out infinite;}
.pnl-live-dot.no-pos{background:var(--t3);animation:none;}
@keyframes pnl-pulse{0%,100%{opacity:1;transform:scale(1)}50%{opacity:0.4;transform:scale(0.7)}}
.pnl-float-row{display:flex;gap:12px;margin-top:4px;font-size:11px;}
.pnl-float-item{display:flex;flex-direction:column;align-items:center;}
.pnl-float-lbl{font-size:9px;color:var(--t3);text-transform:uppercase;letter-spacing:1px;}
.pnl-float-val{font-size:13px;font-weight:600;font-family:'IBM Plex Mono',monospace;}
.vd{width:1px;height:24px;background:var(--border);margin:0 3px;flex-shrink:0;}

/* ?? HEADER RIGHT ?? */
.hbar{display:flex;align-items:center;gap:6px;flex-shrink:0;}
.badge{padding:3px 8px;border-radius:5px;font-family:'IBM Plex Mono',monospace;font-size:10px;
  font-weight:600;border:1px solid var(--border);background:rgba(255,255,255,0.03);white-space:nowrap;}
.badge.shadow{color:var(--amber);border-color:rgba(255,136,0,0.4);background:var(--amber-dim);}
.badge.live{color:var(--green);border-color:rgba(0,217,126,0.4);background:var(--green-dim);}
.dot-conn{width:7px;height:7px;border-radius:50%;display:inline-block;margin-right:5px;}
.dot-ok{background:var(--green);box-shadow:0 0 6px var(--green);animation:sonar 2s infinite;}
.dot-bad{background:var(--red);}
@keyframes sonar{0%{box-shadow:0 0 0 0 rgba(0,217,126,0.6)}70%{box-shadow:0 0 0 6px rgba(0,217,126,0)}100%{box-shadow:0 0 0 0 rgba(0,217,126,0)}}

/* ?? LEFT COLUMN ?? */
.col-left{grid-column:1;display:flex;flex-direction:column;gap:8px;overflow:hidden;min-height:0;}

/* Market data table -- compact two-column grid */
.mkt-card{flex:1;overflow:hidden;display:flex;flex-direction:column;min-height:0;}
.mkt-body{flex:1;overflow-y:auto;padding:6px 8px;}
.mkt-body::-webkit-scrollbar{width:3px;}
.mkt-body::-webkit-scrollbar-thumb{background:rgba(255,255,255,0.08);border-radius:2px;}
.sym-section-label{font-size:10px;color:var(--t2);text-transform:uppercase;letter-spacing:2px;
  padding:5px 4px 3px;opacity:0.7;}
.sym-row{display:grid;grid-template-columns:80px 1fr 38px;align-items:center;gap:0;
  padding:5px 6px;border-radius:6px;margin-bottom:2px;border:1px solid transparent;transition:background 0.15s;}
.sym-row:hover{background:rgba(255,255,255,0.025);}
.sym-row.r-gold{border-color:rgba(245,200,66,0.15);background:rgba(245,200,66,0.04);}
.sym-row.r-primary{border-color:rgba(46,168,255,0.1);}
.sym-row.r-fx{border-color:rgba(0,200,240,0.1);}
.sym-row.r-asia{border-color:rgba(0,224,176,0.1);}
.sym-row.r-eu{border-color:rgba(157,127,255,0.1);}
.sym-nm{font-size:10px;font-weight:700;letter-spacing:0.5px;}
.c-gold{color:var(--gold)}.c-blue{color:var(--blue)}
.c-cyan{color:var(--cyan)}.c-teal{color:var(--teal)}.c-purple{color:var(--purple)}.c-t2{color:var(--t2)}
.px-pair{display:grid;grid-template-columns:1fr 8px 1fr;align-items:center;gap:0;}
.bid{font-family:'IBM Plex Mono',monospace;font-size:11px;font-weight:700;color:var(--green);
  text-align:right;white-space:nowrap;}
.ask{font-family:'IBM Plex Mono',monospace;font-size:11px;font-weight:700;color:var(--red);
  text-align:left;white-space:nowrap;padding-left:3px;}
.px-sep{color:var(--t2);font-size:11px;text-align:center;}
.sprd{font-family:'IBM Plex Mono',monospace;font-size:10px;color:var(--t2);
  text-align:right;white-space:nowrap;}
/* ?? Direction arrow -- shown next to each symbol name ?? */
.dir-arrow{font-size:10px;font-weight:700;margin-left:3px;opacity:0;transition:opacity 0.4s;}
.dir-arrow.up{color:var(--green);opacity:1;}
.dir-arrow.down{color:var(--red);opacity:1;}

/* Regime card */
.regime-card{flex-shrink:0;}
.regime-grid{display:grid;grid-template-columns:1fr 1fr;gap:6px;padding:8px 10px;}
.rg-item{background:rgba(255,255,255,0.02);border-radius:6px;padding:7px 8px;text-align:center;}
.rg-lbl{font-size:10px;color:var(--t2);text-transform:uppercase;letter-spacing:1.5px;margin-bottom:3px;}
.rg-val{font-family:'IBM Plex Mono',monospace;font-size:15px;font-weight:700;color:var(--t1);}
)OMEGA0"
R"OMEGA1(

/* ?? CENTRE COLUMN ?? */
.col-centre{grid-column:2;display:flex;flex-direction:column;gap:8px;overflow:hidden;min-height:0;}

/* Stats row */
.stats-bar{display:grid;grid-template-columns:200px repeat(4,1fr);gap:8px;flex-shrink:0;}
.pnl-card{background:var(--glass);border:1px solid var(--border);border-radius:10px;
  padding:10px 14px;position:relative;overflow:hidden;}
.pnl-card::before{content:'';position:absolute;top:0;left:0;right:0;height:2px;
  background:linear-gradient(90deg,var(--cyan),var(--blue) 50%,transparent);}
.pnl-num{font-family:'IBM Plex Mono',monospace;font-size:28px;font-weight:700;
  letter-spacing:-0.5px;text-shadow:0 0 20px currentColor;line-height:1;margin:4px 0 2px;}
.pnl-pos{color:var(--green)}.pnl-neg{color:var(--red)}
.pnl-sub{font-size:11px;color:var(--t2);}
.stat-card{background:var(--glass);border:1px solid var(--border);border-radius:8px;
  display:flex;flex-direction:row;align-items:center;gap:8px;padding:5px 10px;min-width:0;}
.stat-n{font-family:'IBM Plex Mono',monospace;font-size:15px;font-weight:700;color:var(--blue);}
.stat-l{font-size:9px;color:var(--t3);text-transform:uppercase;letter-spacing:1px;}
/* Live trades panel */
.live-trade-row{display:flex;align-items:center;gap:8px;padding:3px 8px;border-radius:5px;
  background:rgba(255,255,255,0.025);border:1px solid var(--border);font-size:11px;font-family:'IBM Plex Mono',monospace;}
.lt-sym{color:var(--amber);font-weight:700;min-width:52px;}
.lt-side-long{color:var(--green);}
.lt-side-short{color:var(--red);}
.lt-pnl-pos{color:var(--green);font-weight:700;}
.lt-pnl-neg{color:var(--red);font-weight:700;}
.lt-meta{color:var(--t3);font-size:10px;}
/* Daily limit bar */
.limit-bar-wrap{flex:1;height:4px;background:rgba(255,255,255,0.07);border-radius:2px;overflow:hidden;}
.limit-bar-fill{height:100%;border-radius:2px;transition:width 0.5s;}
/* L2 status dot */
.l2-dot{width:6px;height:6px;border-radius:50%;display:inline-block;margin-right:3px;}
.l2-dot-live{background:var(--green);}
.l2-dot-dead{background:var(--t3);}

/* Engine grid -- all 15 engines in a responsive grid */
.eng-section{flex-shrink:0;}
.eng-section-label{font-size:10px;color:var(--t2);text-transform:uppercase;letter-spacing:2px;padding:0 2px 5px;display:flex;align-items:center;gap:0;}
.eng-grid{display:grid;grid-template-columns:repeat(5,1fr);gap:5px;}
.eng-grid.eng-grid-3{grid-template-columns:repeat(3,1fr);}
.eng-grid.eng-grid-6{grid-template-columns:repeat(6,1fr);}
.eng-cell{border-radius:7px;padding:6px 5px;border:1px solid var(--border);
  background:rgba(255,255,255,0.015);text-align:center;cursor:default;transition:all 0.25s;}
.eng-cell.ph1{background:rgba(30,18,0,0.85);border-color:rgba(255,136,0,0.55);
  box-shadow:0 0 8px rgba(255,136,0,0.15);animation:eng-amber 1.8s ease-in-out infinite;}
.eng-cell.ph2{background:rgba(0,30,15,0.95);border-color:rgba(0,217,126,0.7);
  box-shadow:0 0 14px rgba(0,217,126,0.35);animation:eng-green 0.9s ease-in-out infinite;}
.eng-cell.ph-live{background:rgba(0,40,20,0.98);border-color:rgba(0,217,126,1.0);
  box-shadow:0 0 20px rgba(0,217,126,0.6);animation:eng-live 0.4s ease-in-out infinite;}
@keyframes eng-amber{0%,100%{box-shadow:0 0 8px rgba(255,136,0,0.1)}50%{box-shadow:0 0 16px rgba(255,136,0,0.3)}}
@keyframes eng-green{0%,100%{box-shadow:0 0 10px rgba(0,217,126,0.2)}50%{box-shadow:0 0 22px rgba(0,217,126,0.5)}}
@keyframes eng-live{0%,100%{box-shadow:0 0 16px rgba(0,217,126,0.4)}50%{box-shadow:0 0 30px rgba(0,217,126,0.9)}}
.eng-sym{font-size:12px;font-weight:700;color:var(--blue);letter-spacing:0.5px;line-height:1;}
.eng-ph{font-size:10px;font-weight:700;text-transform:uppercase;letter-spacing:0.5px;margin-top:3px;padding:2px 5px;
  border-radius:3px;display:inline-block;}
.eph-flat{background:rgba(255,255,255,0.05);color:var(--t2);}
.eph-comp{background:var(--amber-dim);color:var(--amber);}
.eph-brk{background:var(--green-dim);color:var(--green);}
.eph-live{background:rgba(0,217,126,0.25);color:#fff;font-weight:700;}
.eng-px{font-family:'IBM Plex Mono',monospace;font-size:12px;font-weight:700;margin-top:3px;
  display:flex;justify-content:center;gap:3px;align-items:center;line-height:1;}
.eng-bid{color:var(--green);}.eng-ask{color:var(--red);}.eng-sep{color:var(--t2);font-size:11px;}
.eng-vol{font-family:'IBM Plex Mono',monospace;font-size:10px;color:#a8bbd4;margin-top:2px;line-height:1.3;}
/* Proximity bar -- shows how close price is to compression boundary */
.eng-prox{width:calc(100% - 8px);display:flex;align-items:center;gap:4px;margin:3px 4px 0;}
.eng-prox-track{flex:1;height:3px;background:rgba(255,255,255,0.06);border-radius:2px;overflow:hidden;}
.eng-prox-fill{height:100%;border-radius:2px;transition:width 0.3s,background 0.3s;}
.eng-l2{position:relative;height:3px;background:rgba(255,255,255,0.06);border-radius:2px;overflow:hidden;margin-top:3px;}
.eng-l2-fill{position:absolute;top:0;height:100%;border-radius:2px;transition:width 0.25s,background 0.25s,left 0.25s;}
/* ?? L2 DEPTH PANEL ?? */
.depth-wrap{display:grid;grid-template-columns:1fr 1fr;gap:4px;padding:6px 8px;}
.depth-side{display:flex;flex-direction:column;gap:2px;}
.depth-side-hd{font-size:9px;font-weight:700;letter-spacing:1.5px;text-transform:uppercase;text-align:center;padding:2px 0 4px;}
.depth-side-hd.bid{color:var(--green);}.depth-side-hd.ask{color:var(--red);}
.depth-row{display:grid;grid-template-columns:1fr auto;align-items:center;gap:4px;font-size:10px;font-family:'IBM Plex Mono',monospace;padding:1px 4px;border-radius:3px;position:relative;overflow:hidden;}
.depth-row .depth-bg{position:absolute;top:0;bottom:0;right:0;border-radius:3px;transition:width 0.25s;}
.depth-row.bid .depth-bg{background:rgba(0,217,126,0.10);}.depth-row.ask .depth-bg{background:rgba(255,51,85,0.10);}
.depth-px{position:relative;z-index:1;color:var(--t1);font-weight:600;}
.depth-sz{position:relative;z-index:1;color:var(--t3);font-size:9px;text-align:right;}
.depth-row.bid .depth-px{color:var(--green);}.depth-row.ask .depth-px{color:var(--red);}
.depth-imb-bar{height:4px;background:rgba(255,255,255,0.05);border-radius:2px;margin:3px 8px 2px;position:relative;overflow:hidden;}
.depth-imb-fill{position:absolute;top:0;height:100%;transition:all 0.3s;border-radius:2px;}
.depth-sym-sel{display:flex;gap:4px;padding:6px 8px 2px;flex-wrap:wrap;}
.depth-sym-btn{font-size:9px;font-weight:700;letter-spacing:1px;padding:2px 7px;border-radius:4px;border:1px solid var(--border);background:rgba(255,255,255,0.03);color:var(--t2);cursor:pointer;transition:all 0.15s;text-transform:uppercase;}
.depth-sym-btn.active{border-color:rgba(245,200,66,0.5);background:rgba(245,200,66,0.10);color:var(--gold);}
.depth-sym-btn:hover:not(.active){border-color:var(--border2);color:var(--t1);}
.depth-empty{text-align:center;color:var(--t3);font-size:11px;padding:14px 8px;}
.eng-prox-pct{font-family:'IBM Plex Mono',monospace;font-size:10px;color:var(--t2);
  min-width:26px;text-align:right;transition:color 0.3s;flex-shrink:0;}
/* Signal count badge */
.eng-sigs{font-size:11px;font-weight:700;color:var(--gold);margin-top:2px;
  padding:1px 4px;border-radius:3px;background:rgba(245,200,66,0.1);}

/* Signal + trade area */
.sig-trade-area{flex:1;display:flex;flex-direction:column;gap:6px;min-height:0;overflow:hidden;}
.last-sig{flex-shrink:0;background:var(--glass);border:1px solid var(--border);border-radius:8px;padding:6px 12px;}
.sig-row{display:grid;grid-template-columns:repeat(4,1fr);gap:10px;align-items:center;}
.sig-field-lbl{font-size:10px;color:var(--t2);text-transform:uppercase;letter-spacing:1.5px;margin-bottom:3px;}
.sig-field-val{font-family:'IBM Plex Mono',monospace;font-size:18px;font-weight:700;}

.trades-card{flex:1;min-height:260px;background:var(--glass);border:1px solid var(--border);border-radius:10px;
  display:flex;flex-direction:column;overflow:hidden;}
.trades-scroll{flex:1;overflow-y:auto;}
.trades-scroll::-webkit-scrollbar{width:3px;}
.trades-scroll::-webkit-scrollbar-thumb{background:rgba(255,255,255,0.08);border-radius:2px;}
table{width:100%;border-collapse:collapse;font-size:13px;}
th{padding:7px 10px;color:var(--t2);font-size:10px;text-transform:uppercase;letter-spacing:1.5px;
  font-weight:700;border-bottom:1px solid var(--border);white-space:nowrap;background:var(--bg1);}
td{padding:7px 10px;border-bottom:1px solid rgba(255,255,255,0.025);white-space:nowrap;
  font-family:'IBM Plex Mono',monospace;font-size:13px;font-weight:500;}
.no-data{text-align:center;color:var(--t2);padding:24px;font-size:13px;}

/* ?? RIGHT COLUMN ?? */
.col-right{grid-column:3;display:flex;flex-direction:column;gap:8px;overflow-y:auto;}
.col-right::-webkit-scrollbar{width:3px;}
.col-right::-webkit-scrollbar-thumb{background:rgba(255,255,255,0.08);border-radius:2px;}

/* Latency */
.lat-grid{display:grid;grid-template-columns:1fr 1fr;gap:6px;padding:8px 10px;}
.lat-item{background:rgba(255,255,255,0.02);border-radius:6px;padding:8px 6px;text-align:center;}
.lat-val{font-family:'IBM Plex Mono',monospace;font-size:18px;font-weight:700;color:var(--blue);}
.lat-lbl{font-size:9px;color:var(--t2);text-transform:uppercase;letter-spacing:1px;margin-top:3px;}


/* Governor */
.gov-item{display:flex;justify-content:space-between;align-items:center;padding:5px 10px;
  border-bottom:1px solid var(--border);}
.gov-item:last-of-type{border-bottom:none;}
.gov-lbl{font-size:11px;color:var(--t2);}
.gov-bar{flex:1;height:3px;background:rgba(255,255,255,0.06);margin:0 8px;border-radius:2px;overflow:hidden;}
.gov-fill{height:100%;border-radius:2px;background:var(--amber);transition:width 0.5s;}
.gov-n{font-family:'IBM Plex Mono',monospace;font-size:12px;font-weight:700;color:var(--amber);min-width:22px;text-align:right;}

/* Engine Attribution */
.eng-row{display:flex;align-items:center;padding:3px 0;border-bottom:1px solid rgba(255,255,255,0.04);}
.eng-lbl{font-size:10px;color:var(--t2);width:90px;flex-shrink:0;letter-spacing:0.3px;}
.eng-pnl{font-family:'IBM Plex Mono',monospace;font-size:11px;font-weight:700;flex:1;text-align:right;}
.eng-cnt{font-size:9px;color:var(--t3);width:32px;text-align:right;margin-left:8px;}

/* Cluster Exposure */
.exp-row{display:flex;align-items:center;margin:3px 0;gap:6px;}
.exp-lbl{font-size:10px;color:var(--t2);width:82px;flex-shrink:0;letter-spacing:0.3px;}
.exp-track{flex:1;height:6px;background:rgba(255,255,255,0.06);border-radius:3px;overflow:hidden;position:relative;}
.exp-bar{height:100%;border-radius:3px;transition:width 0.4s,background 0.3s;position:absolute;top:0;}
.exp-bar.long{background:var(--green);left:50%;}
.exp-bar.short{background:var(--red);right:50%;}
.exp-val{font-family:'IBM Plex Mono',monospace;font-size:10px;font-weight:600;min-width:52px;text-align:right;}

/* FIX session */
.fix-item{display:flex;justify-content:space-between;align-items:center;padding:6px 10px;
  border-bottom:1px solid var(--border);}
.fix-item:last-of-type{border-bottom:none;}
.fix-lbl{font-size:11px;color:var(--t2);text-transform:uppercase;letter-spacing:1px;}
.fix-val{font-family:'IBM Plex Mono',monospace;font-size:13px;font-weight:700;}
.fix-ok{color:var(--green)}.fix-bad{color:var(--red)}


/* Comp ranges */
.comp-grid{display:grid;grid-template-columns:1fr 1fr 1fr;gap:6px;padding:8px 10px;}
.comp-item{background:rgba(255,255,255,0.02);border-radius:6px;padding:7px 6px;text-align:center;}
.comp-sym{font-size:11px;font-weight:700;color:var(--blue);margin-bottom:3px;}
.comp-ph{font-size:10px;font-weight:700;margin-bottom:3px;}
.comp-detail{font-family:'IBM Plex Mono',monospace;font-size:10px;color:#a8bbd4;}
</style>)OMEGA1"
R"OMEGA2(

</head>
<body>
<div class="desk">

<!-- ?? HEADER ?? -->
<header>
  <div class="logo">
    <div class="logo-img"></div>
    <div>
      <div class="logo-name">Omega</div>

      <div class="logo-sub">Commodities &amp; Indices</div>
    </div>
  </div>

  <div style="display:flex;align-items:center;gap:18px;padding:6px 18px;border-radius:8px;background:rgba(255,255,255,0.03);border:1px solid var(--border);">
    <div style="text-align:center;">
      <div style="font-size:10px;color:var(--t2);text-transform:uppercase;letter-spacing:1.5px;">VIX</div>

      <div style="font-family:'IBM Plex Mono',monospace;font-size:18px;font-weight:700;" id="vixLevelHdr">--</div>
    </div>
    <div style="width:1px;height:28px;background:var(--border);"></div>
    <div style="text-align:center;">
      <div style="font-size:10px;color:var(--t2);text-transform:uppercase;letter-spacing:1.5px;">Regime</div>
      <div style="font-family:'IBM Plex Mono',monospace;font-size:18px;font-weight:700;" id="macroRegimeHdr">--</div>
    </div>
    <div style="width:1px;height:28px;background:var(--border);"></div>
    <div style="text-align:center;">
      <div style="font-size:10px;color:var(--t2);text-transform:uppercase;letter-spacing:1.5px;">Session</div>

      <div style="font-family:'IBM Plex Mono',monospace;font-size:18px;font-weight:700;" id="sessionValHdr">--</div>
    </div>
    <div style="width:1px;height:28px;background:var(--border);"></div>
    <div style="text-align:center;">
      <div style="font-size:10px;color:var(--t2);text-transform:uppercase;letter-spacing:1.5px;">ES?NQ</div>
      <div style="font-family:'IBM Plex Mono',monospace;font-size:14px;font-weight:700;color:var(--blue);" id="esNqDivHdr">--</div>
    </div>
  </div>

  <div class="hbar">
    <span id="modeBadge" class="badge shadow">SHADOW</span>
    <span class="badge" id="uptimeBadge" style="color:var(--t2)">UP 00:00:00</span>
    <span class="badge" id="sessionBadge" style="color:var(--t2)">?? UTC</span>
    <span class="badge"><span class="dot-conn dot-bad" id="connDot"></span><span id="connText">Connecting</span></span>
    <span class="badge" id="fixQuoteHdr" style="color:var(--red)">Q:--</span>
    <span class="badge" id="ctL2Badge" style="color:var(--t2);font-family:'IBM Plex Mono',monospace;font-size:10px;">L2 ?</span>
    <span id="buildBadge" class="badge" style="color:var(--amber);font-size:9px;font-weight:700;letter-spacing:1.5px" title="Git hash -- built version">? <span id="buildVersion">...</span></span>
    <span style="font-family:'IBM Plex Mono',monospace;font-size:13px;color:var(--t2)" id="clock">--:--:-- UTC</span>
  </div>
</header>)OMEGA2"
R"OMEGA3(


<!-- ?? MAIN ?? -->
<div class="main">

  <!-- ?? LEFT: Market Data + Regime ?? -->
  <div class="col-left">

    <div class="card mkt-card">
      <div class="card-hd"><span class="dot" style="background:var(--cyan)"></span>Market Data</div>
      <div class="mkt-body">

        <div class="sym-section-label">? Precious Metals</div>
        <div class="sym-row r-gold">
          <span class="sym-nm c-gold">XAUUSD<span class="dir-arrow" id="dirGold"></span></span>
          <div class="px-pair"><span class="bid" id="goldBid">----</span><span class="px-sep">|</span><span class="ask" id="goldAsk">----</span></div>
          <span class="sprd" id="goldSpread">--</span>
        </div>
        <div class="sym-section-label">? US Indices &amp; Oil</div>
        <div class="sym-row r-primary">
          <span class="sym-nm c-blue">US500.F<span class="dir-arrow" id="dirSp"></span></span>
          <div class="px-pair"><span class="bid" id="spBid">----</span><span class="px-sep">|</span><span class="ask" id="spAsk">----</span></div>
          <span class="sprd" id="spSpread">--</span>
        </div>
        <div class="sym-row r-primary">
          <span class="sym-nm c-blue">USTEC.F<span class="dir-arrow" id="dirNq"></span></span>
          <div class="px-pair"><span class="bid" id="nqBid">----</span><span class="px-sep">|</span><span class="ask" id="nqAsk">----</span></div>
          <span class="sprd" id="nqSpread">--</span>
        </div>
        <div class="sym-row r-primary">
          <span class="sym-nm c-blue">DJ30.F<span class="dir-arrow" id="dirDj"></span></span>
          <div class="px-pair"><span class="bid" id="djBid">----</span><span class="px-sep">|</span><span class="ask" id="djAsk">----</span></div>
          <span class="sprd" id="djSpread">--</span>
        </div>
        <div class="sym-row r-primary">
          <span class="sym-nm c-blue">NAS100<span class="dir-arrow" id="dirNas"></span></span>
          <div class="px-pair"><span class="bid" id="nasBid">----</span><span class="px-sep">|</span><span class="ask" id="nasAsk">----</span></div>
          <span class="sprd" id="nasSpread">--</span>
        </div>
        <div class="sym-row r-primary">
          <span class="sym-nm c-blue">USOIL.F<span class="dir-arrow" id="dirCl"></span></span>
          <div class="px-pair"><span class="bid" id="clBid">----</span><span class="px-sep">|</span><span class="ask" id="clAsk">----</span></div>
          <span class="sprd" id="clSpread">--</span>
        </div>
        <div class="sym-row r-primary">
          <span class="sym-nm c-blue">UKBRENT<span class="dir-arrow" id="dirBrent"></span></span>
          <div class="px-pair"><span class="bid" id="brentBid">----</span><span class="px-sep">|</span><span class="ask" id="brentAsk">----</span></div>
          <span class="sprd" id="brentSpread">--</span>
        </div>

        <div class="sym-section-label">? EU Indices</div>
        <div class="sym-row r-eu">
          <span class="sym-nm c-purple">GER40<span class="dir-arrow" id="dirGer"></span></span>
          <div class="px-pair"><span class="bid" id="ger30Bid">----</span><span class="px-sep">|</span><span class="ask" id="ger30Ask">----</span></div>
          <span class="sprd" id="ger30Spread">--</span>
        </div>
        <div class="sym-row r-eu">
          <span class="sym-nm c-purple">UK100<span class="dir-arrow" id="dirUk"></span></span>
          <div class="px-pair"><span class="bid" id="uk100Bid">----</span><span class="px-sep">|</span><span class="ask" id="uk100Ask">----</span></div>
          <span class="sprd" id="uk100Spread">--</span>
        </div>
        <div class="sym-row r-eu">
          <span class="sym-nm c-purple">ESTX50<span class="dir-arrow" id="dirEstx"></span></span>
          <div class="px-pair"><span class="bid" id="estx50Bid">----</span><span class="px-sep">|</span><span class="ask" id="estx50Ask">----</span></div>
          <span class="sprd" id="estx50Spread">--</span>
        </div>

        <div class="sym-section-label">? FX Majors</div>
        <div class="sym-row r-fx">
          <span class="sym-nm c-cyan">EURUSD<span class="dir-arrow" id="dirEur"></span></span>
          <div class="px-pair"><span class="bid" id="eurBid">----</span><span class="px-sep">|</span><span class="ask" id="eurAsk">----</span></div>
          <span class="sprd" id="eurSpread">--</span>
        </div>
        <div class="sym-row r-fx" style="border-color:rgba(0,200,240,0.18);background:rgba(0,200,240,0.04);">
          <span class="sym-nm c-cyan">GBPUSD<span class="dir-arrow" id="dirGbp"></span></span>
          <div class="px-pair"><span class="bid" id="gbpBid">----</span><span class="px-sep">|</span><span class="ask" id="gbpAsk">----</span></div>
          <span class="sprd" id="gbpSpread">--</span>
        </div>
        <div class="sym-section-label">? Asia FX</div>
        <div class="sym-row r-asia">
          <span class="sym-nm c-teal">AUDUSD<span class="dir-arrow" id="dirAud"></span></span>
          <div class="px-pair"><span class="bid" id="audBid">----</span><span class="px-sep">|</span><span class="ask" id="audAsk">----</span></div>
          <span class="sprd" id="audSpread">--</span>
        </div>
        <div class="sym-row r-asia">
          <span class="sym-nm c-teal">NZDUSD<span class="dir-arrow" id="dirNzd"></span></span>
          <div class="px-pair"><span class="bid" id="nzdBid">----</span><span class="px-sep">|</span><span class="ask" id="nzdAsk">----</span></div>
          <span class="sprd" id="nzdSpread">--</span>
        </div>
        <div class="sym-row r-asia">
          <span class="sym-nm" style="color:var(--purple)">USDJPY<span class="dir-arrow" id="dirJpy"></span></span>
          <div class="px-pair"><span class="bid" id="jpyBid">----</span><span class="px-sep">|</span><span class="ask" id="jpyAsk">----</span></div>
          <span class="sprd" id="jpySpread">--</span>
        </div>

        <div class="sym-section-label">? Confirmation</div>
        <div class="sym-row">
          <span class="sym-nm c-t2">VIX.F<span class="dir-arrow" id="dirVix"></span></span>
          <div class="px-pair"><span class="bid" id="vixBid">--</span><span class="px-sep">|</span><span class="ask" id="vixAsk">--</span></div>
          <span class="sprd" id="vixSpread">--</span>
        </div>
        <div class="sym-row">
          <span class="sym-nm c-t2">DX.F<span class="dir-arrow" id="dirDx"></span></span>
          <div class="px-pair"><span class="bid" id="dxBid">--</span><span class="px-sep">|</span><span class="ask" id="dxAsk">--</span></div>
          <span class="sprd" id="dxSpread">--</span>
        </div>
        <div class="sym-row">
          <span class="sym-nm c-t2">NGAS.F<span class="dir-arrow" id="dirNgas"></span></span>
          <div class="px-pair"><span class="bid" id="ngasBid">--</span><span class="px-sep">|</span><span class="ask" id="ngasAsk">--</span></div>
          <span class="sprd" id="ngasSpread">--</span>
        </div>
      </div>
    </div>

    <!-- ?? Cross-Asset Engines Panel ?? -->
    <div class="eng-section" style="margin-top:4px;" id="caEngSection">
      <div class="eng-section-label" style="display:flex;align-items:center;gap:6px;">
        ? Cross-Asset
        <span id="caBlockedBadge" style="font-size:10px;padding:1px 6px;border-radius:3px;border:1px solid rgba(255,136,0,0.3);color:var(--amber);font-family:'IBM Plex Mono',monospace;display:none;">0 blocked</span>
        <span id="caActiveBadge" style="font-size:10px;padding:1px 6px;border-radius:3px;border:1px solid rgba(0,217,126,0.3);color:var(--green);font-family:'IBM Plex Mono',monospace;margin-left:auto;display:none;">? LIVE</span>
      </div>
      <div id="caEngGrid" style="display:grid;grid-template-columns:1fr 1fr;gap:3px;margin-top:4px;"></div>
    </div>

  </div>

  <!-- ?? CENTRE ?? -->
  <div class="col-centre">

    <!-- Stats bar -->
    <div class="stats-bar">
      <!-- ?? Compact stat bar ????????????????????????????????????????? -->
      <div class="pnl-card">
        <div style="display:flex;align-items:center;gap:6px;">
          <span class="pnl-live-dot no-pos" id="pnlLiveDot"></span>
          <span style="font-size:9px;color:var(--t2);text-transform:uppercase;letter-spacing:2px;">Daily P&amp;L</span>
          <span class="pnl-num pnl-pos" id="pnlVal" style="font-size:18px;margin-left:2px;">+$0.00</span>
          <span style="font-size:10px;color:var(--amber);margin-left:2px;" id="pnlNzd">NZ$0</span>
        </div>
        <div style="display:flex;gap:10px;margin-top:3px;font-size:10px;font-family:'IBM Plex Mono',monospace;">
          <span style="color:var(--t3)">C:<span id="pnlClosed" style="color:var(--t1)">$0</span></span>
          <span style="color:var(--t3)">F:<span id="pnlFloating" style="color:var(--t2)">--</span></span>
          <span style="color:var(--t3)" id="pnlSub">0T</span>
        </div>
        <!-- Daily loss limit progress bar -->
        <div style="display:flex;align-items:center;gap:5px;margin-top:4px;">
          <span style="font-size:9px;color:var(--t3);">LIMIT</span>
          <div class="limit-bar-wrap"><div class="limit-bar-fill" id="limitBarFill" style="width:0%;background:var(--green)"></div></div>
          <span style="font-size:9px;color:var(--t3);" id="limitPct">0%</span>
        </div>
        <div style="font-size:9px;color:var(--t3);margin-top:2px;" id="pnlGrossSub"></div>
      </div>
      <!-- Compact stats -->
      <div class="stat-card"><div class="stat-l">W</div><div class="stat-n" id="statWins" style="color:var(--green)">0</div></div>
      <div class="stat-card"><div class="stat-l">L</div><div class="stat-n" id="statLosses" style="color:var(--red)">0</div></div>
      <div class="stat-card" title="Avg win / Avg loss"><div class="stat-l">AVG W</div><div class="stat-n" id="statAvgWin" style="color:var(--teal)">$0</div></div>
      <div class="stat-card" title="Average loss per losing trade"><div class="stat-l">AVG L</div><div class="stat-n" id="statAvgLoss" style="color:var(--red)">$0</div></div>
      <div class="stat-card" title="Max intraday drawdown from peak PnL"><div class="stat-l">DD</div><div class="stat-n" id="statMaxDD" style="color:var(--red)">$0</div></div>
      <!-- L2 status indicator -->
      <div class="stat-card" title="cTrader L2 depth feed status">
        <div class="stat-l">L2</div>
        <div style="display:flex;flex-direction:column;gap:2px;">
          <div style="font-size:9px;"><span class="l2-dot l2-dot-dead" id="l2DotGold"></span><span style="color:var(--t2)" id="l2LblGold">GOLD</span></div>
          <div style="font-size:9px;"><span class="l2-dot l2-dot-dead" id="l2DotCt"></span><span style="color:var(--t2)" id="l2LblCt">CT</span></div>
        </div>
      </div>
    </div>
    <!-- Live open trades panel -- full width below stats bar, prominent when positions open -->
    <div id="liveTradesPanelOuter" style="border:1px solid rgba(255,255,255,0.08);border-radius:6px;padding:4px 6px;transition:border-color 0.3s,box-shadow 0.3s;flex-shrink:0;">
      <div id="liveTradesPanel" style="display:flex;flex-direction:column;gap:2px;"></div>
    </div>

)OMEGA3"
R"OMEGA4(

    <!-- Engine state -- ALL 15 engines in 3 groups -->
    <div class="eng-section">
      <div class="eng-section-label">? US / Oil Engines</div>
      <div class="eng-grid eng-grid-5">
        <div class="eng-cell" id="engSP"><div class="eng-sym c-blue">US500</div><div class="eng-ph eph-flat" id="engSPPhase">FLAT</div><div class="eng-px"><span class="eng-bid" id="engSPBid">--</span><span class="eng-sep">|</span><span class="eng-ask" id="engSPAsk">--</span></div><div class="eng-vol" id="engSPVol">--</div><div class="eng-sigs" id="engSPSig">0 signals</div><div class="eng-prox"><div class="eng-prox-track"><div class="eng-prox-fill" id="engSPProx" style="width:0%;background:var(--t3)"></div></div><span class="eng-prox-pct" id="engSPPct"></span></div><div class="eng-l2" id="engSPL2"><div class="eng-l2-fill" id="engSPL2F" style="width:50%;background:var(--t3)"></div></div></div>
        <div class="eng-cell" id="engNQ"><div class="eng-sym c-blue">USTEC</div><div class="eng-ph eph-flat" id="engNQPhase">FLAT</div><div class="eng-px"><span class="eng-bid" id="engNQBid">--</span><span class="eng-sep">|</span><span class="eng-ask" id="engNQAsk">--</span></div><div class="eng-vol" id="engNQVol">--</div><div class="eng-sigs" id="engNQSig">0 signals</div><div class="eng-prox"><div class="eng-prox-track"><div class="eng-prox-fill" id="engNQProx" style="width:0%;background:var(--t3)"></div></div><span class="eng-prox-pct" id="engNQPct"></span></div><div class="eng-l2" id="engNQL2"><div class="eng-l2-fill" id="engNQL2F" style="width:50%;background:var(--t3)"></div></div></div>
        <div class="eng-cell" id="engUS30"><div class="eng-sym c-blue">DJ30</div><div class="eng-ph eph-flat" id="engUS30Phase">FLAT</div><div class="eng-px"><span class="eng-bid" id="engUS30Bid">--</span><span class="eng-sep">|</span><span class="eng-ask" id="engUS30Ask">--</span></div><div class="eng-vol" id="engUS30Vol">--</div><div class="eng-sigs" id="engUS30Sig">0 signals</div><div class="eng-prox"><div class="eng-prox-track"><div class="eng-prox-fill" id="engUS30Prox" style="width:0%;background:var(--t3)"></div></div><span class="eng-prox-pct" id="engUS30Pct"></span></div><div class="eng-l2" id="engUS30L2"><div class="eng-l2-fill" id="engUS30L2F" style="width:50%;background:var(--t3)"></div></div></div>
        <div class="eng-cell" id="engNAS"><div class="eng-sym c-blue">NAS100</div><div class="eng-ph eph-flat" id="engNASPhase">FLAT</div><div class="eng-px"><span class="eng-bid" id="engNASBid">--</span><span class="eng-sep">|</span><span class="eng-ask" id="engNASAsk">--</span></div><div class="eng-vol" id="engNASVol">--</div><div class="eng-sigs" id="engNASSig">0 signals</div><div class="eng-prox"><div class="eng-prox-track"><div class="eng-prox-fill" id="engNASProx" style="width:0%;background:var(--t3)"></div></div><span class="eng-prox-pct" id="engNASPct"></span></div><div class="eng-l2" id="engNASL2"><div class="eng-l2-fill" id="engNASL2F" style="width:50%;background:var(--t3)"></div></div></div>
        <div class="eng-cell" id="engCL"><div class="eng-sym" style="color:var(--amber)">USOIL</div><div class="eng-ph eph-flat" id="engCLPhase">FLAT</div><div class="eng-px"><span class="eng-bid" id="engCLBid">--</span><span class="eng-sep">|</span><span class="eng-ask" id="engCLAsk">--</span></div><div class="eng-vol" id="engCLVol">--</div><div class="eng-sigs" id="engCLSig">0 signals</div><div class="eng-prox"><div class="eng-prox-track"><div class="eng-prox-fill" id="engCLProx" style="width:0%;background:var(--t3)"></div></div><span class="eng-prox-pct" id="engCLPct"></span></div><div class="eng-l2" id="engCLL2"><div class="eng-l2-fill" id="engCLL2F" style="width:50%;background:var(--t3)"></div></div></div>
      </div>
    </div>



    <div class="eng-section" style="margin-top:6px;">
      <div class="eng-section-label">? EU Indices + Brent</div>
      <div class="eng-grid" style="grid-template-columns:repeat(4,1fr)">
        <div class="eng-cell" id="engGER"><div class="eng-sym c-purple">GER40</div><div class="eng-ph eph-flat" id="engGERPhase">FLAT</div><div class="eng-px"><span class="eng-bid" id="engGERBid">--</span><span class="eng-sep">|</span><span class="eng-ask" id="engGERAsk">--</span></div><div class="eng-vol" id="engGERVol">--</div><div class="eng-sigs" id="engGERSig">0 signals</div><div class="eng-prox"><div class="eng-prox-track"><div class="eng-prox-fill" id="engGERProx" style="width:0%;background:var(--t3)"></div></div><span class="eng-prox-pct" id="engGERPct"></span></div><div class="eng-l2" id="engGERL2"><div class="eng-l2-fill" id="engGERL2F" style="width:50%;background:var(--t3)"></div></div></div>
        <div class="eng-cell" id="engUK"><div class="eng-sym c-purple">UK100</div><div class="eng-ph eph-flat" id="engUKPhase">FLAT</div><div class="eng-px"><span class="eng-bid" id="engUKBid">--</span><span class="eng-sep">|</span><span class="eng-ask" id="engUKAsk">--</span></div><div class="eng-vol" id="engUKVol">--</div><div class="eng-sigs" id="engUKSig">0 signals</div><div class="eng-prox"><div class="eng-prox-track"><div class="eng-prox-fill" id="engUKProx" style="width:0%;background:var(--t3)"></div></div><span class="eng-prox-pct" id="engUKPct"></span></div><div class="eng-l2" id="engUKL2"><div class="eng-l2-fill" id="engUKL2F" style="width:50%;background:var(--t3)"></div></div></div>

        <div class="eng-cell" id="engESTX"><div class="eng-sym c-purple">ESTX50</div><div class="eng-ph eph-flat" id="engESTXPhase">FLAT</div><div class="eng-px"><span class="eng-bid" id="engESTXBid">--</span><span class="eng-sep">|</span><span class="eng-ask" id="engESTXAsk">--</span></div><div class="eng-vol" id="engESTXVol">--</div><div class="eng-sigs" id="engESTXSig">0 signals</div><div class="eng-prox"><div class="eng-prox-track"><div class="eng-prox-fill" id="engESTXProx" style="width:0%;background:var(--t3)"></div></div><span class="eng-prox-pct" id="engESTXPct"></span></div><div class="eng-l2" id="engESTXL2"><div class="eng-l2-fill" id="engESTXL2F" style="width:50%;background:var(--t3)"></div></div></div>
        <div class="eng-cell" id="engBRENT"><div class="eng-sym" style="color:var(--amber)">BRENT</div><div class="eng-ph eph-flat" id="engBRENTPhase">FLAT</div><div class="eng-px"><span class="eng-bid" id="engBRENTBid">--</span><span class="eng-sep">|</span><span class="eng-ask" id="engBRENTAsk">--</span></div><div class="eng-vol" id="engBRENTVol">--</div><div class="eng-sigs" id="engBRENTSig">0 signals</div><div class="eng-prox"><div class="eng-prox-track"><div class="eng-prox-fill" id="engBRENTProx" style="width:0%;background:var(--t3)"></div></div><span class="eng-prox-pct" id="engBRENTPct"></span></div><div class="eng-l2" id="engBRENTL2"><div class="eng-l2-fill" id="engBRENTL2F" style="width:50%;background:var(--t3)"></div></div></div>
      </div>
    </div>


    <div class="eng-section" style="margin-top:6px;">
      <div class="eng-section-label">? FX + Asia Engines <span id="asiaGateBadge" style="font-size:10px;margin-left:8px;padding:1px 7px;border-radius:3px;border:1px solid rgba(255,255,255,0.15);color:var(--t2);white-space:nowrap;flex-shrink:0;font-family:'IBM Plex Mono',monospace;letter-spacing:0.5px;">ASIA FX: --</span></div>
      <div class="eng-grid" style="grid-template-columns:repeat(5,1fr)">
        <div class="eng-cell" id="engEUR"><div class="eng-sym c-cyan">EURUSD</div><div class="eng-ph eph-flat" id="engEURPhase">FLAT</div><div class="eng-px"><span class="eng-bid" id="engEURBid">--</span><span class="eng-sep">|</span><span class="eng-ask" id="engEURAsk">--</span></div><div class="eng-vol" id="engEURVol">--</div><div class="eng-sigs" id="engEURSig">0 signals</div><div class="eng-prox"><div class="eng-prox-track"><div class="eng-prox-fill" id="engEURProx" style="width:0%;background:var(--t3)"></div></div><span class="eng-prox-pct" id="engEURPct"></span></div><div class="eng-l2" id="engEURL2"><div class="eng-l2-fill" id="engEURL2F" style="width:50%;background:var(--t3)"></div></div></div>
        <div class="eng-cell" id="engGBP"><div class="eng-sym c-cyan">GBPUSD</div><div class="eng-ph eph-flat" id="engGBPPhase">FLAT</div><div class="eng-px"><span class="eng-bid" id="engGBPBid">--</span><span class="eng-sep">|</span><span class="eng-ask" id="engGBPAsk">--</span></div><div class="eng-vol" id="engGBPVol">--</div><div class="eng-sigs" id="engGBPSig">0 signals</div><div class="eng-prox"><div class="eng-prox-track"><div class="eng-prox-fill" id="engGBPProx" style="width:0%;background:var(--t3)"></div></div><span class="eng-prox-pct" id="engGBPPct"></span></div><div class="eng-l2" id="engGBPL2"><div class="eng-l2-fill" id="engGBPL2F" style="width:50%;background:var(--t3)"></div></div></div>
        <div class="eng-cell" id="engAUD"><div class="eng-sym c-teal">AUDUSD</div><div class="eng-ph eph-flat" id="engAUDPhase">FLAT</div><div class="eng-px"><span class="eng-bid" id="engAUDBid">--</span><span class="eng-sep">|</span><span class="eng-ask" id="engAUDAsk">--</span></div><div class="eng-vol" id="engAUDVol">--</div><div class="eng-sigs" id="engAUDSig">0 signals</div><div class="eng-prox"><div class="eng-prox-track"><div class="eng-prox-fill" id="engAUDProx" style="width:0%;background:var(--t3)"></div></div><span class="eng-prox-pct" id="engAUDPct"></span></div><div class="eng-l2" id="engAUDL2"><div class="eng-l2-fill" id="engAUDL2F" style="width:50%;background:var(--t3)"></div></div></div>
        <div class="eng-cell" id="engNZD"><div class="eng-sym c-teal">NZDUSD</div><div class="eng-ph eph-flat" id="engNZDPhase">FLAT</div><div class="eng-px"><span class="eng-bid" id="engNZDBid">--</span><span class="eng-sep">|</span><span class="eng-ask" id="engNZDAsk">--</span></div><div class="eng-vol" id="engNZDVol">--</div><div class="eng-sigs" id="engNZDSig">0 signals</div><div class="eng-prox"><div class="eng-prox-track"><div class="eng-prox-fill" id="engNZDProx" style="width:0%;background:var(--t3)"></div></div><span class="eng-prox-pct" id="engNZDPct"></span></div><div class="eng-l2" id="engNZDL2"><div class="eng-l2-fill" id="engNZDL2F" style="width:50%;background:var(--t3)"></div></div></div>
        <div class="eng-cell" id="engJPY"><div class="eng-sym" style="color:var(--purple)">USDJPY</div><div class="eng-ph eph-flat" id="engJPYPhase">FLAT</div><div class="eng-px"><span class="eng-bid" id="engJPYBid">--</span><span class="eng-sep">|</span><span class="eng-ask" id="engJPYAsk">--</span></div><div class="eng-vol" id="engJPYVol">--</div><div class="eng-sigs" id="engJPYSig">0 signals</div><div class="eng-prox"><div class="eng-prox-track"><div class="eng-prox-fill" id="engJPYProx" style="width:0%;background:var(--t3)"></div></div><span class="eng-prox-pct" id="engJPYPct"></span></div><div class="eng-l2" id="engJPYL2"><div class="eng-l2-fill" id="engJPYL2F" style="width:50%;background:var(--t3)"></div></div></div>
      </div>
    </div>

    <div class="eng-section" style="margin-top:6px;">
      <div class="eng-section-label">? Metals Engines</div>
      <div class="eng-grid" style="grid-template-columns:repeat(1,1fr)">
        <div class="eng-cell" id="engXAU" style="border-color:rgba(245,200,66,0.2);background:rgba(245,200,66,0.04);"><div class="eng-sym" style="color:var(--gold)">XAUUSD</div><div class="eng-ph eph-flat" id="engXAUPhase">FLAT</div><div class="eng-px"><span class="eng-bid" id="engXAUBid" style="color:var(--gold)">--</span><span class="eng-sep">|</span><span class="eng-ask" id="engXAUAsk" style="color:var(--red)">--</span></div><div class="eng-vol" id="engXAUVol">--</div><div class="eng-sigs" id="engXAUSig">0 signals</div><div class="eng-prox"><div class="eng-prox-track"><div class="eng-prox-fill" id="engXAUProx" style="width:0%;background:var(--t3)"></div></div><span class="eng-prox-pct" id="engXAUPct"></span></div><div class="eng-l2" id="engXAUL2"><div class="eng-l2-fill" id="engXAUL2F" style="width:50%;background:var(--t3)"></div></div></div>
      </div>
    </div>
)OMEGA4"
R"OMEGA5(

    <!-- Watchdog banner -- shown when session is active but no trade in 20min -->
    <div id="watchdogBanner" style="display:none;margin:0 0 6px 0;padding:7px 12px;background:rgba(255,136,0,0.10);border:1px solid rgba(255,136,0,0.45);border-radius:7px;align-items:center;gap:10px;">
      <span style="font-size:13px;color:var(--amber)">?</span>
      <div style="flex:1">
        <span style="font-size:11px;font-weight:700;color:var(--amber);text-transform:uppercase;letter-spacing:1px;">No trades firing</span>
        <span id="watchdogMsg" style="font-size:11px;color:var(--t2);margin-left:6px;"></span>
      </div>
      <span id="watchdogAge" style="font-family:'IBM Plex Mono',monospace;font-size:12px;color:var(--amber);font-weight:700;white-space:nowrap;"></span>
    </div>

    <!-- Last signal + trades -->
    <div class="sig-trade-area">
      <div class="last-sig">
        <div style="display:flex;align-items:center;gap:6px;margin-bottom:3px;">
          <span style="font-size:9px;color:var(--t2);text-transform:uppercase;letter-spacing:1.5px;">Last Signals</span>
          <button id="bellBtn" onclick="toggleBell()" style="margin-left:auto;background:rgba(255,214,0,.1);border:1px solid rgba(255,214,0,0.3);border-radius:3px;padding:0 5px;cursor:pointer;font-size:9px;color:#ffd600;font-family:inherit;line-height:18px;">BELL</button>
        </div>
        <div id="lastSignalDetail" style="display:flex;flex-direction:column;gap:1px;"><span style="color:var(--t2);font-size:11px;">Waiting for first signal...</span></div>
      </div>

      <!-- SL Cooldown Panel -->
      <div id="slCooldownSection" style="display:none;margin:0 0 8px 0;padding:6px 10px;background:rgba(255,51,85,0.07);border:1px solid rgba(255,51,85,0.2);border-radius:6px;">
        <div style="font-size:10px;color:var(--red);text-transform:uppercase;letter-spacing:1px;margin-bottom:5px;font-weight:700;">? SL Cooldown Active</div>
        <div id="slCooldownPanel"></div>
      </div>

      <div class="trades-card">
        <div class="card-hd" style="flex-shrink:0;">
          <span class="dot" style="background:var(--green)"></span>Recent Trades
          <span id="tradeCount" style="font-family:'IBM Plex Mono',monospace;color:var(--t2);margin-left:6px;font-size:11px;"></span>
        </div>
        <div class="trades-scroll">
          <table>
            <thead><tr>
              <th>Time</th><th>Symbol</th><th>Side</th><th>Entry</th><th>Exit</th>
              <th>Held</th><th>Result</th><th>Engine / Reason</th><th>Gross</th><th>Slip</th><th style="color:var(--green)">Net $$</th>
            </tr></thead>
            <tbody id="tradesBody"><tr><td colspan="11" class="no-data">No trades yet</td></tr></tbody>

          </table>
        </div>
      </div>
    </div>
  </div>

  <!-- ?? RIGHT COLUMN ?? -->
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
      <div class="fix-item"><span class="fix-lbl">Seq Gaps <span style="font-size:9px;color:var(--t3);font-weight:400;text-transform:none;letter-spacing:0">missed msgs</span></span><span class="fix-val" id="fixGaps" style="color:var(--t1)">0</span></div>
      <div class="fix-item"><span class="fix-lbl">Orders <span style="font-size:9px;color:var(--t3);font-weight:400;text-transform:none;letter-spacing:0">sent</span></span><span class="fix-val" id="fixOrders" style="color:var(--t1)">0</span></div>
      <div class="fix-item"><span class="fix-lbl">Fills <span style="font-size:9px;color:var(--t3);font-weight:400;text-transform:none;letter-spacing:0">executions</span></span><span class="fix-val" id="fixFills" style="color:var(--t1)">0</span></div>
      <div class="fix-item"><span class="fix-lbl">BUILD <span style="font-size:9px;color:var(--t3);font-weight:400;text-transform:none;letter-spacing:0">git hash</span></span><span class="fix-val" id="buildVersionRight" style="color:var(--t2);font-size:9px">...</span></div>
    </div>

    <!-- Governor Blocks -->
    <div class="card">
      <div class="card-hd"><span class="dot" style="background:var(--amber)"></span>Governor Blocks</div>
      <div class="gov-item" title="Trades blocked because spread is too wide -- entry cost exceeds edge threshold"><span class="gov-lbl">SPREAD <span style="font-size:9px;color:var(--t3)">wide spread</span></span><div class="gov-bar"><div class="gov-fill" id="gbarSpread" style="width:0%"></div></div><span class="gov-n" id="gnSpread">0</span></div>
      <div class="gov-item" title="Trades blocked because FIX round-trip latency exceeded the configured cap -- stale prices"><span class="gov-lbl">LATENCY <span style="font-size:9px;color:var(--t3)">rtt cap</span></span><div class="gov-bar"><div class="gov-fill" id="gbarLat" style="width:0%"></div></div><span class="gov-n" id="gnLat">0</span></div>
      <div class="gov-item" title="Trades blocked because daily P&amp;L loss limit was hit -- engine shut down for the day"><span class="gov-lbl">P&amp;L LIMIT <span style="font-size:9px;color:var(--t3)">daily cap</span></span><div class="gov-bar"><div class="gov-fill" id="gbarPnl" style="width:0%"></div></div><span class="gov-n" id="gnPnl">0</span></div>
      <div class="gov-item" title="Trades blocked because max open positions reached -- waiting for existing trades to close"><span class="gov-lbl">POSITIONS <span style="font-size:9px;color:var(--t3)">cap reached</span></span><div class="gov-bar"><div class="gov-fill" id="gbarPos" style="width:0%"></div></div><span class="gov-n" id="gnPos">0</span></div>
      <div class="gov-item" title="Trades blocked due to consecutive losses -- cooling off after a losing streak"><span class="gov-lbl">CONSEC LOSS <span style="font-size:9px;color:var(--t3)">streak block</span></span><div class="gov-bar"><div class="gov-fill" id="gbarConsec" style="width:0%"></div></div><span class="gov-n" id="gnConsec">0</span></div>
      <div style="font-size:10px;color:var(--t2);padding:5px 10px;text-align:right;" id="govTotal">Total: 0</div>
    </div>

    <!-- Compression Ranges (SP/NQ/CL) -->
    <div class="card">
      <div class="card-hd"><span class="dot" style="background:var(--cyan)"></span>Compression Ranges<span style="font-size:9px;font-weight:400;color:var(--t3);letter-spacing:0;text-transform:none;margin-left:6px;">how close to breakout</span></div>
      <div class="comp-grid">
        <div class="comp-item"><div class="comp-sym">US500</div><div class="comp-ph" id="compSPPh" style="color:var(--t2)">FLAT</div><div class="comp-detail" id="compSPDet">--</div></div>
        <div class="comp-item"><div class="comp-sym">USTEC</div><div class="comp-ph" id="compNQPh" style="color:var(--t2)">FLAT</div><div class="comp-detail" id="compNQDet">--</div></div>
        <div class="comp-item"><div class="comp-sym">USOIL</div><div class="comp-ph" id="compCLPh" style="color:var(--t2)">FLAT</div><div class="comp-detail" id="compCLDet">--</div></div>
      </div>
    </div>



    <!-- L2 Order Book Depth Panel -->
    <div class="card" id="depthCard">
      <div class="card-hd" style="justify-content:space-between;">
        <div style="display:flex;align-items:center;gap:6px;">
          <span class="dot" style="background:var(--gold)"></span>L2 Order Book
          <span style="font-size:9px;font-weight:400;color:var(--t3);letter-spacing:0;text-transform:none;">depth levels</span>
        </div>
        <span id="depthImbBadge" style="font-family:'IBM Plex Mono',monospace;font-size:10px;font-weight:700;color:var(--t2);">IMB --</span>
      </div>
      <div class="depth-sym-sel">
        <button class="depth-sym-btn active" onclick="setDepthSym('gold')" id="dbtngold">GOLD</button>
        <button class="depth-sym-btn" onclick="setDepthSym('sp')" id="dbtnsp">SP500</button>
        <button class="depth-sym-btn" onclick="setDepthSym('eur')" id="dbtnneur">EUR</button>
      </div>
      <!-- imbalance bar: bid pressure left (green), ask pressure right (red) -->
      <div class="depth-imb-bar">
        <div class="depth-imb-fill" id="depthImbFill" style="width:2px;left:50%;background:var(--t3);"></div>
      </div>
      <div class="depth-wrap">
        <div class="depth-side">
          <div class="depth-side-hd bid">&#9650; BIDS</div>
          <div id="depthBidRows"></div>
        </div>
        <div class="depth-side">
          <div class="depth-side-hd ask">&#9660; ASKS</div>
          <div id="depthAskRows"></div>
        </div>
      </div>
    </div>

    <!-- Per-Engine P&L Attribution -->
    <div class="card" id="engAttribCard">
      <div class="card-hd"><span class="dot" style="background:var(--green)"></span>Engine Attribution <span style="font-size:9px;font-weight:400;color:var(--t3);letter-spacing:0;text-transform:none;margin-left:6px;">session P&amp;L by engine type</span></div>
      <div style="padding:6px 10px 4px;">
        <div class="eng-row"><span class="eng-lbl">BREAKOUT</span><span class="eng-pnl" id="engPnlBreakout">$0</span><span class="eng-cnt" id="engCntBreakout">0</span></div>
        <div class="eng-row"><span class="eng-lbl">BRACKET</span><span class="eng-pnl" id="engPnlBracket">$0</span><span class="eng-cnt" id="engCntBracket">0</span></div>
        <div class="eng-row"><span class="eng-lbl">MEAN REV</span><span class="eng-pnl" id="engPnlMeanRev">$0</span><span class="eng-cnt" id="engCntMeanRev">0</span></div>
        <div class="eng-row"><span class="eng-lbl">GOLD STACK</span><span class="eng-pnl" id="engPnlGoldStack">$0</span><span class="eng-cnt" id="engCntGoldStack">0</span></div>
        <div class="eng-row"><span class="eng-lbl">GOLD FLOW</span><span class="eng-pnl" id="engPnlGoldFlow">$0</span><span class="eng-cnt" id="engCntGoldFlow">0</span></div>
        <div id="gfPyramidRow" style="display:none;margin:2px 0 4px;padding:4px 8px;border-radius:6px;background:rgba(0,255,150,0.08);border:1px solid rgba(0,255,150,0.3);">
          <span style="font-size:9px;font-weight:700;color:var(--green);letter-spacing:0.8px;">&#9650; STACK UNLOCKED</span>
          <span id="gfPyramidStage" style="font-size:9px;color:var(--t2);margin-left:6px;">stage 0</span>
          <span id="gfPyramidProfit" style="font-size:9px;color:var(--green);margin-left:6px;font-weight:700;">$0</span>
        </div>
        <div class="eng-row"><span class="eng-lbl">CROSS-ASSET</span><span class="eng-pnl" id="engPnlCross">$0</span><span class="eng-cnt" id="engCntCross">0</span></div>
        <div class="eng-row"><span class="eng-lbl">LATENCY</span><span class="eng-pnl" id="engPnlLatency">$0</span><span class="eng-cnt" id="engCntLatency">0</span></div>
      </div>
    </div>

    <!-- Cluster Exposure + MultiDay Throttle -->
    <div class="card" id="exposureCard">
      <div class="card-hd"><span class="dot" style="background:var(--cyan)"></span>Live Cluster Exposure
        <span id="mdThrottleBadge" style="display:none;margin-left:8px;padding:2px 8px;border-radius:10px;font-size:9px;font-weight:700;background:rgba(255,80,80,0.2);color:var(--red);border:1px solid var(--red);letter-spacing:0.5px;text-transform:uppercase">&#9888; SIZES HALVED</span>
      </div>
      <div style="padding:4px 10px 0;font-size:9px;color:var(--t3);letter-spacing:0.4px;">USD notional per cluster (1 lot units) &bull; direction: green=long red=short</div>

      <div style="padding:8px 10px 2px;">
        <div class="exp-row" id="expUS">    <span class="exp-lbl">US EQUITY</span>  <div class="exp-track"><div class="exp-bar" id="expBarUS"></div></div>    <span class="exp-val" id="expValUS">$0</span></div>
        <div class="exp-row" id="expEU">    <span class="exp-lbl">EU EQUITY</span>  <div class="exp-track"><div class="exp-bar" id="expBarEU"></div></div>    <span class="exp-val" id="expValEU">$0</span></div>
        <div class="exp-row" id="expOIL">   <span class="exp-lbl">OIL</span>         <div class="exp-track"><div class="exp-bar" id="expBarOIL"></div></div>   <span class="exp-val" id="expValOIL">$0</span></div>
        <div class="exp-row" id="expMET">   <span class="exp-lbl">METALS</span>      <div class="exp-track"><div class="exp-bar" id="expBarMET"></div></div>   <span class="exp-val" id="expValMET">$0</span></div>
        <div class="exp-row" id="expJPY">   <span class="exp-lbl">JPY/AUD/NZD</span><div class="exp-track"><div class="exp-bar" id="expBarJPY"></div></div>   <span class="exp-val" id="expValJPY">$0</span></div>
        <div class="exp-row" id="expEGB">   <span class="exp-lbl">EUR/GBP</span>     <div class="exp-track"><div class="exp-bar" id="expBarEGB"></div></div>   <span class="exp-val" id="expValEGB">$0</span></div>
      </div>
      <div style="padding:4px 10px 8px;display:flex;justify-content:space-between;align-items:center;">
        <div style="font-size:10px;color:var(--t2);">TOTAL <span id="expTotal" style="color:var(--t1);font-weight:700;margin-left:4px;">$0</span></div>
        <div style="font-size:9px;color:var(--t3);">Losing days: <span id="mdConsecDays" style="color:var(--t2);font-weight:600;">0</span> &nbsp; Scale: <span id="mdScale" style="color:var(--t2);font-weight:600;">100%</span></div>
      </div>
    </div>

  </div><!-- /col-right -->

</div><!-- /main -->
</div><!-- /desk -->
)OMEGA5"
R"OMEGA6(

<script>
'use strict';
let wsConnected=false,lastData={},_bellEnabled=false,_lastTradeCount=0,_bellBootCount=-1,_audioCtx=null,_lastOpenCount=0,_openBootDone=false,_ltFingerprint='';

function safe(v,d=0){const n=Number(v);return isNaN(n)?d:n;})OMEGA6"
R"OMEGA7(

function fmtUTC(ts){if(!ts)return '--';return new Date(ts*1000).toUTCString().slice(17,25);}
)OMEGA7"
R"OMEGA8(

function toggleBell(){
  _bellEnabled=!_bellEnabled;
  const b=document.getElementById('bellBtn');
  if(b){b.textContent=_bellEnabled?'? ARMED':'? ARM BELL';b.style.color=_bellEnabled?'var(--green)':'#ffd600';b.style.borderColor=_bellEnabled?'rgba(0,217,126,0.4)':'rgba(255,214,0,0.4)';}
  if(_bellEnabled&&!_audioCtx){try{_audioCtx=new(window.AudioContext||window.webkitAudioContext)();if(_audioCtx.state==='suspended')_audioCtx.resume();}catch(e){}}
  if(_bellEnabled&&_audioCtx)_playTestBell();
})OMEGA8"
R"OMEGA9(

function _playTestBell(){try{const ctx=_audioCtx;if(!ctx)return;if(ctx.state==='suspended')ctx.resume();const t=ctx.currentTime,o=ctx.createOscillator(),g=ctx.createGain();o.connect(g);g.connect(ctx.destination);o.type='sine';o.frequency.value=880;g.gain.setValueAtTime(0,t);g.gain.linearRampToValueAtTime(0.35,t+0.01);g.gain.exponentialRampToValueAtTime(0.001,t+0.35);o.start(t);o.stop(t+0.4);}catch(e){}})OMEGA9"
R"OMEGA10(

function _playWinBell(){if(!_bellEnabled||!_audioCtx)return;try{const ctx=_audioCtx;if(ctx.state==='suspended')ctx.resume();const t=ctx.currentTime;[[0,880,1040],[0.2,1100,1320]].forEach(([dt,f1,f2])=>{[f1,f2].forEach((freq,i)=>{const o=ctx.createOscillator(),g=ctx.createGain();o.connect(g);g.connect(ctx.destination);o.type='sine';o.frequency.value=freq;g.gain.setValueAtTime(0,t+dt);g.gain.linearRampToValueAtTime(i?0.8:1.6,t+dt+0.008);g.gain.exponentialRampToValueAtTime(0.001,t+dt+1.2);o.start(t+dt);o.stop(t+dt+1.3);});});}catch(e){}})OMEGA10"
R"OMEGA11(

function _playLossBell(){if(!_bellEnabled||!_audioCtx)return;try{const ctx=_audioCtx;if(ctx.state==='suspended')ctx.resume();const t=ctx.currentTime,o=ctx.createOscillator(),g=ctx.createGain();o.connect(g);g.connect(ctx.destination);o.type='sawtooth';o.frequency.setValueAtTime(280,t);o.frequency.linearRampToValueAtTime(130,t+0.3);g.gain.setValueAtTime(0,t);g.gain.linearRampToValueAtTime(0.8,t+0.01);g.gain.exponentialRampToValueAtTime(0.001,t+0.5);o.start(t);o.stop(t+0.55);}catch(e){}}
function _playEntryBell(){if(!_bellEnabled||!_audioCtx)return;try{const ctx=_audioCtx;if(ctx.state==='suspended')ctx.resume();const t=ctx.currentTime;[[0,600,900],[0.12,750,1100]].forEach(([dt,f1,f2])=>{[f1,f2].forEach((freq,i)=>{const o=ctx.createOscillator(),g=ctx.createGain();o.connect(g);g.connect(ctx.destination);o.type='sine';o.frequency.value=freq;g.gain.setValueAtTime(0,t+dt);g.gain.linearRampToValueAtTime(i?0.6:1.2,t+dt+0.005);g.gain.exponentialRampToValueAtTime(0.001,t+dt+0.25);o.start(t+dt);o.stop(t+dt+0.3);});});}catch(e){}}
)OMEGA11"
R"OMEGA12(

// _pxCache: avoids DOM writes when price hasn't changed (eliminates L2 flicker)
// _midCache: tracks previous mid-price per symbol for direction arrows
const _pxCache={};
const _midCache={};

function px(id,val,dec){
  const el=document.getElementById(id);if(!el)return;
  const v=safe(val);if(v<=0)return;
  const s=v.toFixed(dec||2);
  if(_pxCache[id]===s)return;   // value unchanged -- skip DOM write entirely
  _pxCache[id]=s;
  el.textContent=s;
}

// pxDir: updates the direction arrow for a symbol given its current bid+ask.
// dirId  -- the id of the <span class="dir-arrow"> element
// bid/ask -- raw values from telemetry
// threshold -- minimum mid movement to flip arrow (default 0.0001, caller can override for indices)
function pxDir(dirId,bid,ask,threshold){
  const el=document.getElementById(dirId);if(!el)return;
  const b=safe(bid),a=safe(ask);
  if(b<=0||a<=0)return;
  const mid=(b+a)/2;
  const thr=threshold||0.0001;
  const prev=_midCache[dirId];
  _midCache[dirId]=mid;
  if(!prev||prev<=0)return;          // first tick -- no direction yet
  if(mid>prev+thr){
    if(el.textContent!=='?'||el.className!=='dir-arrow up'){
      el.textContent='?';el.className='dir-arrow up';
    }
  } else if(mid<prev-thr){
    if(el.textContent!=='?'||el.className!=='dir-arrow down'){
      el.textContent='?';el.className='dir-arrow down';
    }
  }
  // if within threshold: leave arrow as-is (last known direction stays visible)
})OMEGA12"
R"OMEGA13(

function sprd(id,bid,ask){const el=document.getElementById(id);if(!el)return;const b=safe(bid),a=safe(ask);if(b>0&&a>0)el.textContent=(a-b).toFixed(2);})OMEGA13"
R"OMEGA14(

function txt(id,v){const el=document.getElementById(id);if(el)el.textContent=v;}
)OMEGA14"
R"OMEGA15(

function setHdrPhase(id,phase){
  const el=document.getElementById(id);if(!el)return;
  const p=safe(phase);
  if(p===0){el.className='htk-ph ph-flat';el.textContent='FLAT';}
  else if(p===1){el.className='htk-ph ph-comp';el.textContent='COMP';}
  else{el.className='htk-ph ph-brk';el.textContent='BRK ?';}
}
)OMEGA15"
R"OMEGA16(

function updateL2Bar(cellId, imb, active) {
  const f = document.getElementById(cellId + 'L2F');
  if (!f) return;
  const v = (imb == null || !active) ? 0.5 : Math.max(0, Math.min(1, imb));
  // Bar is centered at 50% -- left half = ask pressure (red), right half = bid pressure (green)
  // imb=0.5 (neutral): small grey pip at center
  // imb>0.5 (bid heavy): green bar grows right from center
  // imb<0.5 (ask heavy): red bar grows left from center
  if (!active) {
    f.style.width = '2px'; f.style.left = '50%'; f.style.background = 'var(--t3)';
    return;
  }
  const dev = v - 0.5;
  if (Math.abs(dev) < 0.05) {
    f.style.width = '2px'; f.style.left = '50%'; f.style.background = 'var(--t2)';
  } else if (dev > 0) {
    // bid heavy -- green bar from center rightward
    const pct = Math.min(50, dev * 200);
    f.style.width = pct + '%'; f.style.left = '50%'; f.style.background = 'var(--green)';
  } else {
    // ask heavy -- red bar from center leftward
    const pct = Math.min(50, -dev * 200);
    f.style.left = (50 - pct) + '%'; f.style.width = pct + '%'; f.style.background = 'var(--red)';
  }
}

// ?? L2 Depth Panel ?????????????????????????????????????????????????????????
// Renders top-N bid/ask levels for the selected symbol.
// Symbol key maps to JSON fields emitted by appendBook() in OmegaTelemetryServer.cpp:
//   gold ? gold_bids / gold_asks
//   sp   ? sp_bids   / sp_asks
//   eur  ? eur_bids  / eur_asks
// Each level: { p: price, s: size }
// imbalance drawn as a centred bar: bid vol left (green), ask vol right (red).
let _depthSym = 'gold';   // currently selected symbol key
const _depthSymL2Key = { gold:'l2_gold', sp:'l2_sp', eur:'l2_eur' };
const _depthMaxRows = 5;  // levels to show per side

// Last best bid/ask seen per symbol -- used to gate repaints.
// WS pushes at 1000ms but the book only needs repainting when a price level
// actually changes. Sizes-only changes (volume shifting at same price) are
// allowed through at most once per 500ms to avoid thrashing on busy tape.
const _depthLastBest = {};   // key -> { fp: string } -- fingerprint of all visible price levels
let   _depthLastSizeUpdate = 0;  // timestamp of last size-only repaint

function setDepthSym(sym) {
  _depthSym = sym;
  // Force full repaint on symbol switch -- clear price cache AND size throttle
  delete _depthLastBest[sym];
  _depthLastSizeUpdate = 0;   // new symbol should show sizes immediately
  // Update button active states
  ['gold','sp','eur'].forEach(s => {
    const b = document.getElementById('dbtn' + (s === 'eur' ? 'neur' : s));
    if (b) b.className = 'depth-sym-btn' + (s === sym ? ' active' : '');
  });
}

// ?? Depth panel row pool ????????????????????????????????????????????????????
// Pre-built DOM rows. On first call we create _depthMaxRows rows per side and
// keep them alive forever. Each tick we update only the cells whose values
// actually changed -- zero innerHTML rewrites during normal operation.
const _depthRows = { bid: [], ask: [] };
let   _depthRowsReady = false;

function _ensureDepthRows() {
  if (_depthRowsReady) return;
  ['bid','ask'].forEach(side => {
    const container = document.getElementById('depth' + (side==='bid'?'Bid':'Ask') + 'Rows');
    if (!container) return;
    container.innerHTML = '';
    for (let i = 0; i < _depthMaxRows; i++) {
      const row  = document.createElement('div');
      row.className = 'depth-row ' + side;
      // Rows are always visible -- blank when no data, never toggled hidden
      const bg   = document.createElement('div');  bg.className  = 'depth-bg';
      const px   = document.createElement('span'); px.className  = 'depth-px';
      const sz   = document.createElement('span'); sz.className  = 'depth-sz';
      row.appendChild(bg); row.appendChild(px); row.appendChild(sz);
      container.appendChild(row);
      _depthRows[side].push({ row, bg, px, sz, lastP: null, lastS: null, lastBar: null });
    }
  });
  _depthRowsReady = true;
}

function updateDepthPanel(d) {
  _ensureDepthRows();
  const sym   = _depthSym;
  const bids  = d[sym + '_bids'];
  const asks  = d[sym + '_asks'];
  const imbEl = document.getElementById('depthImbFill');
  const badge = document.getElementById('depthImbBadge');

  // ?? No data path ?????????????????????????????????????????????????????????
  if (!bids || !bids.length || !asks || !asks.length) {
    ['bid','ask'].forEach(side => {
      _depthRows[side].forEach(r => {
        // Keep rows visible but blank -- no display toggle
        if (r.row.style.display === 'none') r.row.style.display = '';
        if (r.lastP !== '')  { r.px.textContent = ''; r.lastP = ''; }
        if (r.lastS !== '')  { r.sz.textContent = ''; r.lastS = ''; }
        if (r.lastBar !== 0) { r.bg.style.width = '0%'; r.lastBar = 0; }
      });
    });
    if (imbEl) { imbEl.style.width='2px'; imbEl.style.left='50%'; imbEl.style.background='var(--t3)'; }
    if (badge) { badge.textContent = 'IMB --'; badge.style.color = 'var(--t2)'; }
    delete _depthLastBest[sym];
    return;
  }

  // ?? Sort once -- used for both gate and render ?????????????????????????????
  const sortedBids = [...bids].sort((a,b) => b.p - a.p).slice(0, _depthMaxRows);
  const sortedAsks = [...asks].sort((a,b) => a.p - b.p).slice(0, _depthMaxRows);

  const refPx = sortedBids[0] ? sortedBids[0].p : (sortedAsks[0] ? sortedAsks[0].p : 1);
  const dec   = refPx > 100 ? 2 : (refPx > 1 ? 4 : 5);

  // ?? Book fingerprint gate ?????????????????????????????????????????????????
  // Build a compact string of all visible price levels. patchRows already
  // diffs per-cell, but we skip even calling it when nothing changed at all.
  // Sizes are intentionally excluded from the fingerprint -- they shift every
  // tick on a live book and would defeat gating entirely. Size bar widths are
  // throttled separately via _depthLastSizeUpdate.
  const priceFp = sortedBids.map(l=>l.p.toFixed(dec)).join(',')
                + '|'
                + sortedAsks.map(l=>l.p.toFixed(dec)).join(',');

  const now = Date.now();
  const cached = _depthLastBest[sym];
  const priceChanged = !cached || cached.fp !== priceFp;
  const sizeUpdateDue = (now - _depthLastSizeUpdate) >= 500;

  // Always update imbalance badge -- it's a cheap text compare, no DOM writes unless changed.
  // Row repaint only happens when price levels change OR size throttle fires.
  const totalBidVol = sortedBids.reduce((s,r) => s + r.s, 0);
  const totalAskVol = sortedAsks.reduce((s,r) => s + r.s, 0);
  const imb       = totalBidVol / (totalBidVol + totalAskVol + 0.0001);
  const scalarImb = safe(d[_depthSymL2Key[sym]], 0.5);
  const dispImb   = (scalarImb !== 0.5) ? scalarImb : imb;
  const dev       = dispImb - 0.5;

  if (imbEl) {
    // Quantise to 1% steps to avoid style writes on micro-drift
    const newW  = Math.abs(dev) < 0.03 ? '2px' : Math.min(50, Math.abs(dev) * 200).toFixed(0)+'%';
    const newL  = dev <= 0 ? (50 - Math.min(50, -dev * 200)).toFixed(0)+'%' : '50%';
    const newBg = Math.abs(dev) < 0.03 ? 'var(--t2)' : dev > 0 ? 'var(--green)' : 'var(--red)';
    if (imbEl._w !== newW || imbEl._l !== newL || imbEl._bg !== newBg) {
      imbEl.style.width = newW; imbEl.style.left = newL; imbEl.style.background = newBg;
      imbEl._w = newW; imbEl._l = newL; imbEl._bg = newBg;
    }
  }
  if (badge) {
    const imbLabel = (dispImb * 100).toFixed(0);
    const bidDom = dispImb > 0.55, askDom = dispImb < 0.45;
    const newTxt = 'IMB ' + imbLabel + '%' + (bidDom ? ' B?' : askDom ? ' A?' : '');
    if (badge.textContent !== newTxt) {
      badge.textContent = newTxt;
      badge.style.color = bidDom ? 'var(--green)' : askDom ? 'var(--red)' : 'var(--t2)';
    }
  }

  // Skip row repaint entirely if neither price levels nor size throttle fired
  if (!priceChanged && !sizeUpdateDue) return;

  _depthLastBest[sym] = { fp: priceFp };
  if (!priceChanged) _depthLastSizeUpdate = now;  // size-only update -- record time

  // ?? Surgical per-cell row update ??????????????????????????????????????????
  const maxVol = Math.max(totalBidVol, totalAskVol, 0.001);

  function patchRows(levels, side) {
    const pool = _depthRows[side];
    for (let i = 0; i < _depthMaxRows; i++) {
      const cell = pool[i];
      const lvl  = i < levels.length ? levels[i] : null;
      // Zero-price = padding level (server always sends 5, pads with p=0/s=0 when book shallow)
      const hasData = lvl && lvl.p > 0 && lvl.s > 0;

      // Always keep row visible -- never toggle display. Blank rows are invisible
      // due to empty text + zero-width bar. Toggling display causes the jump.
      if (cell.row.style.display === 'none') cell.row.style.display = '';

      if (!hasData) {
        if (cell.lastP !== '')  { cell.px.textContent = ''; cell.lastP = ''; }
        if (cell.lastS !== '')  { cell.sz.textContent = ''; cell.lastS = ''; }
        if (cell.lastBar !== 0) { cell.bg.style.width = '0%'; cell.lastBar = 0; }
        continue;
      }

      const pStr   = lvl.p.toFixed(dec);
      const sStr   = lvl.s >= 1000 ? (lvl.s/1000).toFixed(1)+'k' : lvl.s.toFixed(lvl.s < 1 ? 3 : 2);
      const barPct = Math.round(Math.min(100, (lvl.s / maxVol) * 100));

      if (cell.lastP !== pStr)   { cell.px.textContent = pStr;  cell.lastP = pStr; }
      if (cell.lastS !== sStr)   { cell.sz.textContent = sStr;  cell.lastS = sStr; }
      if (cell.lastBar !== barPct) { cell.bg.style.width = barPct+'%'; cell.lastBar = barPct; }
    }
  }

  patchRows(sortedBids, 'bid');
  patchRows(sortedAsks, 'ask');
}

)OMEGA16"
R"OMEGA17(

function updateEngCell(cellId,phaseId,volId,sigId,phase,rv,bv,sigs,hi,lo,bid,ask,dec,isLive,bkt){
  const cell=document.getElementById(cellId),ph=document.getElementById(phaseId),
        vol=document.getElementById(volId),sig=document.getElementById(sigId);
  if(!cell)return;
  const p=safe(phase),d=dec!=null?dec:2;
  const bktPhase=bkt?safe(bkt.phase):0;
  const bktActive=bkt&&bktPhase>=1&&bktPhase<=3&&safe(bkt.hi)>0;
  if(isLive)cell.className='eng-cell ph-live';
  else if(bktPhase===2)cell.className='eng-cell ph2';  // PENDING = orange
  else if(bktPhase===1||bktPhase===3)cell.className='eng-cell ph2'; // ARMED/LIVE bracket
  else cell.className='eng-cell'+(p===1?' ph1':p===2?' ph2':'');
  if(ph){
    if(isLive){ph.className='eng-ph eph-live';ph.textContent='LIVE ?';}
    else if(bktPhase===3){ph.className='eng-ph eph-live';ph.textContent='BKT LIVE ?';}
    else if(bktPhase===2){ph.className='eng-ph eph-brk';ph.textContent='BKT PENDING ?';}
    else if(bktPhase===1){ph.className='eng-ph eph-comp';ph.textContent='BKT ARMED ?';}
    else if(p===0){ph.className='eng-ph eph-flat';ph.textContent='FLAT';}
    else if(p===1){ph.className='eng-ph eph-comp';ph.textContent='COMPRESSING';}
    else{ph.className='eng-ph eph-brk';ph.textContent='BREAKOUT ?';}
  }
  if(vol){
    if(bktActive){
      // Show bracket levels prominently
      const bhi=safe(bkt.hi).toFixed(d),blo=safe(bkt.lo).toFixed(d);
      vol.innerHTML='<span style="color:var(--green)">?'+bhi+'</span> <span style="color:var(--red)">?'+blo+'</span>';
    } else if(p===1&&safe(hi)>0){
      vol.textContent='range: '+safe(hi).toFixed(d)+'?'+safe(lo).toFixed(d);
    } else if(safe(rv)>0){
      vol.textContent='vol: '+safe(rv).toFixed(2)+'% / '+safe(bv).toFixed(2)+'%';
    } else vol.innerHTML='';
  }
  // Signal count
  if(sig)sig.textContent=sigs>0?sigs+' signal'+(sigs>1?'s':''):'';
  // Prices
  const bidEl=document.getElementById(cellId+'Bid'),askEl=document.getElementById(cellId+'Ask');
  if(bidEl&&safe(bid)>0)bidEl.textContent=safe(bid).toFixed(d);
  if(askEl&&safe(ask)>0)askEl.textContent=safe(ask).toFixed(d);

  // Proximity bar -- how close is price to breaking the compression boundary?
  // p=0(FLAT): empty. p=1(COMP): fill = how tight compression is (rv/bv inverted).
  // p=2(BREAKOUT_WATCH): fill = how far price has moved through range toward boundary.
  const prox=document.getElementById(cellId+'Prox');
  const pctEl=document.getElementById(cellId+'Pct');
  if(prox){
    if(p===0||!isFinite(safe(rv))){
      prox.style.width='0%';prox.style.background='var(--t3)';
      if(pctEl){pctEl.textContent='';pctEl.style.color='var(--t2)';}
    }
    else if(p===1){
      // Compression tightness: lower rv/bv = tighter = closer to breakout
      // Shows how compressed vol is relative to baseline -- higher % = tighter compression
      const ratio=safe(bv)>0?safe(rv)/safe(bv):1;
      const pct=Math.max(0,Math.min(100,(1-ratio)*200));
      prox.style.width=pct+'%';
      const col=pct>70?'var(--green)':pct>40?'var(--amber)':'var(--t2)';
      prox.style.background=col;
      if(pctEl){pctEl.textContent=Math.round(pct)+'%';pctEl.style.color=col;}
    } else if(p===2){
      // Breakout watch: how close is price to the compression boundary?
      // 100% = price is AT the boundary (about to trigger)
      const mid=safe(bid)>0&&safe(ask)>0?(safe(bid)+safe(ask))/2:0;
      const range=safe(hi)-safe(lo);
      if(mid>0&&range>0){
        const distLong=Math.abs(mid-safe(hi))/range;
        const distShort=Math.abs(mid-safe(lo))/range;
        const proximity=Math.max(0,Math.min(100,(1-Math.min(distLong,distShort))*100));
        prox.style.width=proximity+'%';
        prox.style.background='var(--green)';
        if(pctEl){pctEl.textContent=Math.round(proximity)+'%';pctEl.style.color='var(--green)';}
      }
    } else if(isLive){
      prox.style.width='100%';prox.style.background='var(--green)';
      if(pctEl){pctEl.textContent='LIVE';pctEl.style.color='var(--green)';}
    }
  }
}
)OMEGA17"
R"OMEGA18(

function renderLastSignal(d){
  const el=document.getElementById('lastSignalDetail');if(!el)return;
  const hist=(d.signal_history||[]).slice(0,3);
  if(hist.length===0){el.innerHTML='<span style="color:var(--t2);font-size:11px;">Waiting for first signal...</span>';return;}
  const latest=hist[0];
  const rows=hist.map((s,i)=>{
    const isBracket=s.side==='BRACKET';
    const sc=isBracket?'var(--amber)':s.side==='LONG'?'var(--green)':'var(--red)';
    const eng=s.engine||'';
    const dec=(s.price||0)>100?2:(s.price||0)>1?4:5;
    const entry=s.price||0;
    const tp=s.tp||0; const sl=s.sl||0;
    const isLong=s.side==='LONG';
    const tpDist=tp>0&&entry>0?Math.abs(tp-entry):0;
    const slDist=sl>0&&entry>0?Math.abs(sl-entry):0;
    const rr=slDist>0?(tpDist/slDist):0;
    const rrCol=rr>=2?'var(--green)':rr>=1.5?'var(--amber)':'var(--red)';
    const rrStr=rr>0?`<span style="color:${rrCol};font-family:'IBM Plex Mono',monospace;font-size:10px;font-weight:700">${rr.toFixed(1)}R</span>`:'';
    const tpStr=tp>0?`<span style="color:var(--green);font-family:'IBM Plex Mono',monospace;font-size:10px">tp:${tp.toFixed(dec)}</span>`:'';
    const slStr=sl>0?`<span style="color:var(--red);font-family:'IBM Plex Mono',monospace;font-size:10px">sl:${sl.toFixed(dec)}</span>`:'';
    const latest_badge=i===0?'<span style="font-size:9px;color:var(--gold);padding:0 4px;background:rgba(255,214,0,0.15);border-radius:2px;border:1px solid rgba(255,214,0,0.3)">LATEST</span>':'';
    const engBadge=eng?`<span style="font-size:9px;color:var(--blue);padding:0 4px;background:rgba(0,136,255,0.1);border-radius:2px">${eng}</span>`:'';
    const rowBg=i===0?'rgba(255,214,0,0.04)':'';
    return `<div style="display:flex;align-items:center;gap:5px;padding:2px 0;border-bottom:1px solid rgba(255,255,255,0.04);background:${rowBg};overflow:hidden">
      ${latest_badge}
      <span style="color:var(--blue);font-weight:700;font-size:11px;min-width:62px">${s.symbol||'--'}</span>
      <span style="color:${sc};font-weight:700;font-size:11px;min-width:40px">${isBracket?'BKT':s.side}</span>
      <span style="color:var(--amber);font-family:'IBM Plex Mono',monospace;font-size:11px">${entry>0?entry.toFixed(dec):'--'}</span>
      ${tpStr}${slStr}${rrStr}
      <span style="color:var(--t2);font-size:10px;flex:1;text-align:right;overflow:hidden;text-overflow:ellipsis;white-space:nowrap">${s.reason||''}</span>
      ${engBadge}
    </div>`;
  }).join('');
  el.innerHTML=rows;
}
)OMEGA18"
R"OMEGA19(

function renderSLCooldowns(d){
  const el=document.getElementById('slCooldownPanel');if(!el)return;
  const cds=d.sl_cooldowns||[];
  const count=safe(d.sl_cooldown_count);
  if(count===0||cds.length===0){el.innerHTML='<span style="color:var(--t2);font-size:11px">None</span>';el.parentElement&&(el.parentElement.style.display='none');return;}
  el.parentElement&&(el.parentElement.style.display='block');
  el.innerHTML=cds.map(c=>{
    const pct=Math.min(100,Math.round(safe(c.secs_remaining)/120*100));
    return `<div style="display:inline-flex;align-items:center;gap:5px;margin-right:8px;margin-bottom:4px;background:rgba(255,51,85,0.12);border:1px solid rgba(255,51,85,0.35);border-radius:4px;padding:2px 8px">
      <span style="color:var(--red);font-weight:700;font-size:12px">${c.symbol}</span>
      <span style="color:var(--t2);font-size:11px">SL COOL</span>
      <span style="color:var(--red);font-family:'IBM Plex Mono',monospace;font-size:11px">${safe(c.secs_remaining)}s</span>
      <div style="width:32px;height:4px;background:rgba(255,255,255,0.1);border-radius:2px;overflow:hidden">
        <div style="width:${pct}%;height:100%;background:var(--red);border-radius:2px;transition:width 1s linear"></div>
      </div>
    </div>`;
  }).join('');
}
)OMEGA19"
R"OMEGA20(

function renderAsiaGate(d){
  const el=document.getElementById('asiaGateBadge');if(!el)return;
  const open=safe(d.asia_fx_gate_open);
  el.textContent=open?'ASIA FX: OPEN':'ASIA FX: CLOSED';
  el.style.color=open?'var(--green)':'var(--t2)';
  el.style.borderColor=open?'rgba(0,217,126,0.3)':'rgba(255,255,255,0.1)';
}


)OMEGA20"
R"OMEGA21(

function renderTrades(trades){
  const el=document.getElementById('tradesBody'),cE=document.getElementById('tradeCount');
  if(!trades||trades.length===0){el.innerHTML='<tr><td colspan="11" class="no-data">No trades yet</td></tr>';if(cE)cE.textContent='';return;}
  const closed=trades.filter(t=>t.exitReason&&t.exitReason!=='');
  if(_bellBootCount<0){_bellBootCount=closed.length;_lastTradeCount=closed.length;}
  if(_bellEnabled&&closed.length>_lastTradeCount&&_lastTradeCount>=_bellBootCount){const pnl=safe(closed[0].net_pnl);pnl>0?_playWinBell():_playLossBell();}
  _lastTradeCount=closed.length;
  const wins=closed.filter(t=>safe(t.net_pnl)>0).length,losses=closed.filter(t=>safe(t.net_pnl)<0).length,totalNet=closed.reduce((s,t)=>s+safe(t.net_pnl),0);
  // BUG FIX 1: trade count header -- was (totalNet>=0?'+':'') now (totalNet>=0?'+':'-')
  if(cE)cE.textContent=closed.length+' closed ? '+wins+'W/'+losses+'L ? '+(totalNet>=0?'+':'-')+'$'+Math.abs(totalNet).toFixed(2);
  const now=Math.floor(Date.now()/1000);
  el.innerHTML=trades.slice(0,60).map(t=>{
    const isOpen=!t.exitReason||t.exitReason==='',net=safe(t.net_pnl),gross=safe(t.pnl),slip=safe(t.slippage_entry)+safe(t.slippage_exit);
    const win=net>0,loss=net<0,sc=t.side==='LONG'?'var(--green)':'var(--red)';
    const reason=t.exitReason||'',result=isOpen?'⬤':reason==='TP_HIT'?'✓TP':reason==='SL_HIT'?'✗SL':reason==='TRAIL_HIT'?'✓TR':reason==='BE_HIT'?'✓BE':reason==='TIMEOUT'?'✗TO':reason==='PARTIAL_1R'?'$P1':reason==='PARTIAL_2R'?'$P2':'✓FC';
    const rc=isOpen?'var(--blue)':win?'var(--green)':loss?'var(--red)':'var(--t2)';
    const netC=win?'var(--green)':loss?'var(--red)':'var(--t2)';
    let heldStr='--';

    if(isOpen&&safe(t.entryTs)>0){const s=now-safe(t.entryTs);heldStr=s>=60?Math.floor(s/60)+'m'+(s%60)+'s':s+'s';}


    else if(safe(t.entryTs)>0&&safe(t.exitTs)>0){const s=safe(t.exitTs)-safe(t.entryTs);heldStr=s>=60?Math.floor(s/60)+'m'+(s%60)+'s':s+'s';}
    const rowBg=isOpen?'rgba(46,168,255,0.06)':win?'rgba(0,217,126,0.05)':loss?'rgba(255,51,85,0.05)':'';
    // BUG FIX 2: gross column -- was (gross>=0?'+':'') now (gross>=0?'+':'-')
    const grossD=isOpen?'':(gross>=0?'+':'-')+'$'+Math.abs(gross).toFixed(2);
    const slipD=isOpen?'':slip>0?'-$'+slip.toFixed(2):'--';
    // BUG FIX 3: net column -- was (net>=0?'+':'') now (net>=0?'+':'-')
    const netD=isOpen?'<span style="color:var(--t2);font-size:10px">live</span>':(net>=0?'+':'-')+'$'+Math.abs(net).toFixed(2);
    const tReg=t.regime||'';const tEng=t.engine||'';
    const regCol=tReg.includes('EXPANSION')||tReg.includes('TREND')?'var(--green)':tReg.includes('QUIET')?'var(--amber)':'var(--t2)';
    const engBadge=tEng?`<span style="font-size:9px;color:var(--cyan);margin-left:3px">${tEng}</span>`:'';
    const regimeCell=`<span style="color:${regCol};font-size:10px">${tReg||'--'}</span>${engBadge}`;
    // Engine + reason combined cell
    const engName=(t.engine||'').replace('Engine','').replace('GoldFlow','GFlow').replace('WickRejection','WickRej').replace('NoiseBandMomentum','NBM').replace('GoldSilverLeadLag','LeadLag');
    const exitRsnShort=(t.exitReason||'').replace('FORCE_CLOSE','FC').replace('TRAIL_HIT','TRAIL').replace('TP_HIT','TP').replace('SL_HIT','SL').replace('TIMEOUT','T/O').replace('BE_HIT','BE').replace('PARTIAL_1R','P1-BANK').replace('PARTIAL_2R','P2-BANK').replace('MAX_HOLD_TIMEOUT','T/O');
    const engReasonCell=`<span style="color:var(--cyan);font-size:10px">${engName}</span>${!isOpen&&exitRsnShort?`<span style="color:var(--t2);font-size:10px;margin-left:4px">${exitRsnShort}</span>`:''}`;
    return `<tr style="background:${rowBg}">
      <td style="color:var(--t2);font-size:11px">${fmtUTC(safe(t.entryTs))}</td>
      <td style="color:var(--blue);font-weight:700;font-size:13px">${t.symbol||'--'}</td>
      <td style="color:${sc};font-weight:700">${t.side||'--'}</td>
      <td style="font-family:'IBM Plex Mono',monospace;font-size:12px">${safe(t.price)>0?safe(t.price).toFixed(2):'--'}</td>
      <td style="font-family:'IBM Plex Mono',monospace;color:var(--t2);font-size:12px">${isOpen?'<span style="color:var(--blue);font-size:11px">open</span>':safe(t.exitPrice)>0?safe(t.exitPrice).toFixed(2):'--'}</td>
      <td style="color:var(--t2);font-size:11px">${heldStr}</td>
      <td style="font-weight:700;color:${rc};font-size:13px">${result}</td>
      <td style="font-size:11px;max-width:140px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap">${engReasonCell}</td>
      <td style="font-family:'IBM Plex Mono',monospace;color:${gross>=0?'var(--green)':'var(--red)'};font-size:12px">${grossD}</td>
      <td style="font-family:'IBM Plex Mono',monospace;color:var(--red);font-size:11px">${slipD}</td>
      <td style="font-family:'IBM Plex Mono',monospace;color:${netC};font-weight:900;font-size:14px;letter-spacing:-0.3px">${netD}</td>
    </tr>`;
  }).join('');
}
)OMEGA21"
R"OMEGA22(

function renderCrossAsset(d){
  const grid=document.getElementById('caEngGrid');if(!grid)return;
  const engines=d.ca_engines||[];
  const blocked=safe(d.cost_guard_blocked);
  const passed=safe(d.cost_guard_passed);

  // Badge updates
  const bb=document.getElementById('caBlockedBadge');
  if(bb){
    if(blocked>0){bb.textContent=blocked+' blocked';bb.style.display='inline';}
    else bb.style.display='none';
  }
  const anyActive=engines.some(e=>e.active);
  const ab=document.getElementById('caActiveBadge');
  if(ab){
    if(anyActive){ab.style.display='inline';}else ab.style.display='none';
  }

  // Friendly display names
  const NAMES={
    'ORB_US':'ORB US','ORB_GER40':'ORB GER','ORB_UK100':'ORB UK',
    'ORB_ESTX50':'ORB ESTX',
    'VWAP_SP':'VWAP SP','VWAP_NQ':'VWAP NQ','VWAP_GER40':'VWAP GER','VWAP_EUR':'VWAP EUR',
    'TRENDPB_GOLD':'TPB GOLD','TRENDPB_GER':'TPB GER',
    'FXCASC_GBP':'CASCADE','ESNQ_DIV':'ES/NQ DIV','CARRY_UNW':'CARRY UNW'
  };
  // Color theme per engine type
  const TYPE_COL={
    'ORB':'var(--cyan)','VWAP':'var(--purple)','TRENDPB':'var(--gold)',
    'FXCASC':'var(--green)','ESNQ':'var(--blue)','CARRY':'var(--red)'
  };
  function typeOf(name){
    if(name.startsWith('ORB'))return'ORB';
    if(name.startsWith('VWAP'))return'VWAP';
    if(name.startsWith('TRENDPB'))return'TRENDPB';
    if(name.startsWith('FXCASC'))return'FXCASC';
    if(name.startsWith('ESNQ'))return'ESNQ';
    return'CARRY';
  }

  grid.innerHTML=engines.map(e=>{
    const label=NAMES[e.name]||e.name;
    const type=typeOf(e.name);
    const col=TYPE_COL[type]||'var(--t2)';
    const isActive=e.active===1;
    const dir=e.is_long?'? LONG':'? SHORT';
    const dirCol=e.is_long?'var(--green)':'var(--red)';
    const bg=isActive?'rgba(0,217,126,0.07)':'rgba(255,255,255,0.02)';
    const border=isActive?'1px solid rgba(0,217,126,0.25)':'1px solid rgba(255,255,255,0.06)';

    let detail='';
    if(isActive){
      const ep=e.entry>0?e.entry.toFixed(e.entry>100?1:4):'--';
      const tpd=e.tp>0?e.tp.toFixed(e.tp>100?1:4):'--';
      const sld=e.sl>0?e.sl.toFixed(e.sl>100?1:4):'--';
      detail=`<div style="font-size:9px;color:var(--t2);margin-top:2px;font-family:'IBM Plex Mono',monospace;line-height:1.5">
        <span style="color:var(--t2)">@ </span><span style="color:var(--t1)">${ep}</span>
        <span style="margin-left:4px;color:var(--green)">TP${tpd}</span>
        <span style="margin-left:4px;color:var(--red)">SL${sld}</span>
      </div>`;
    } else if(e.ref_price>0){
      const refLabel=type==='ORB'?'rng':type==='VWAP'?'vwap':type==='TRENDPB'?'ema50':'ref';
      const refVal=e.ref_price.toFixed(e.ref_price>100?1:4);
      detail=`<div style="font-size:9px;color:var(--t3);margin-top:2px;font-family:'IBM Plex Mono',monospace">${refLabel}: ${refVal}</div>`;
    }

    return `<div style="background:${bg};border:${border};border-radius:6px;padding:5px 7px;min-width:0">
      <div style="font-size:9px;color:${col};font-weight:700;letter-spacing:0.5px;text-transform:uppercase;white-space:nowrap;overflow:hidden;text-overflow:ellipsis">${label}</div>
      <div style="font-size:9px;color:var(--t2);margin-top:1px">${e.symbol}</div>
      ${isActive
        ? `<div style="font-size:10px;color:${dirCol};font-weight:700;margin-top:2px">${dir}</div>`
        : `<div style="font-size:9px;color:var(--t3);margin-top:2px">IDLE</div>`}
      ${detail}
    </div>`;
  }).join('');
}
)OMEGA22"
R"OMEGA23(

function updateDashboard(d){
  lastData=d;setConn(true);

  // Left column
  px('goldBid',d.gold_bid,2);px('goldAsk',d.gold_ask,2);sprd('goldSpread',d.gold_bid,d.gold_ask);
  pxDir('dirGold',d.gold_bid,d.gold_ask,0.05);
  px('spBid',d.sp_bid,2);px('spAsk',d.sp_ask,2);sprd('spSpread',d.sp_bid,d.sp_ask);
  pxDir('dirSp',d.sp_bid,d.sp_ask,0.5);
  px('nqBid',d.nq_bid,2);px('nqAsk',d.nq_ask,2);sprd('nqSpread',d.nq_bid,d.nq_ask);
  pxDir('dirNq',d.nq_bid,d.nq_ask,0.5);
  px('djBid',d.dj_bid,2);px('djAsk',d.dj_ask,2);sprd('djSpread',d.dj_bid,d.dj_ask);
  pxDir('dirDj',d.dj_bid,d.dj_ask,1.0);
  px('nasBid',d.nas_bid,2);px('nasAsk',d.nas_ask,2);sprd('nasSpread',d.nas_bid,d.nas_ask);
  pxDir('dirNas',d.nas_bid,d.nas_ask,0.5);
  px('clBid',d.cl_bid,2);px('clAsk',d.cl_ask,2);sprd('clSpread',d.cl_bid,d.cl_ask);
  pxDir('dirCl',d.cl_bid,d.cl_ask,0.02);
  px('brentBid',d.brent_bid,2);px('brentAsk',d.brent_ask,2);sprd('brentSpread',d.brent_bid,d.brent_ask);
  pxDir('dirBrent',d.brent_bid,d.brent_ask,0.02);
  px('ger30Bid',d.ger30_bid,2);px('ger30Ask',d.ger30_ask,2);sprd('ger30Spread',d.ger30_bid,d.ger30_ask);
  pxDir('dirGer',d.ger30_bid,d.ger30_ask,0.5);
  px('uk100Bid',d.uk100_bid,2);px('uk100Ask',d.uk100_ask,2);sprd('uk100Spread',d.uk100_bid,d.uk100_ask);
  pxDir('dirUk',d.uk100_bid,d.uk100_ask,0.5);
  px('estx50Bid',d.estx50_bid,2);px('estx50Ask',d.estx50_ask,2);sprd('estx50Spread',d.estx50_bid,d.estx50_ask);
  pxDir('dirEstx',d.estx50_bid,d.estx50_ask,0.5);
  px('eurBid',d.eurusd_bid,5);px('eurAsk',d.eurusd_ask,5);sprd('eurSpread',d.eurusd_bid,d.eurusd_ask);
  pxDir('dirEur',d.eurusd_bid,d.eurusd_ask,0.00005);
  px('gbpBid',d.gbpusd_bid,5);px('gbpAsk',d.gbpusd_ask,5);sprd('gbpSpread',d.gbpusd_bid,d.gbpusd_ask);
  pxDir('dirGbp',d.gbpusd_bid,d.gbpusd_ask,0.00005);
  px('audBid',d.audusd_bid,5);px('audAsk',d.audusd_ask,5);sprd('audSpread',d.audusd_bid,d.audusd_ask);
  pxDir('dirAud',d.audusd_bid,d.audusd_ask,0.00005);
  px('nzdBid',d.nzdusd_bid,5);px('nzdAsk',d.nzdusd_ask,5);sprd('nzdSpread',d.nzdusd_bid,d.nzdusd_ask);
  pxDir('dirNzd',d.nzdusd_bid,d.nzdusd_ask,0.00005);
  px('jpyBid',d.usdjpy_bid,3);px('jpyAsk',d.usdjpy_ask,3);sprd('jpySpread',d.usdjpy_bid,d.usdjpy_ask);
  pxDir('dirJpy',d.usdjpy_bid,d.usdjpy_ask,0.005);

  px('vixBid',d.vix_bid,2);px('vixAsk',d.vix_ask,2);
  pxDir('dirVix',d.vix_bid,d.vix_ask,0.02);
  px('dxBid',d.dx_bid,2);px('dxAsk',d.dx_ask,2);
  pxDir('dirDx',d.dx_bid,d.dx_ask,0.02);
  px('ngasBid',d.ngas_bid,2);px('ngasAsk',d.ngas_ask,2);
  pxDir('dirNgas',d.ngas_bid,d.ngas_ask,0.005);

  // Engine cells -- US/Oil group (sp_phase etc from telemetry)
  const live=d.open_positions||[];
  const isLive=sym=>Array.isArray(live)&&live.some(p=>p.symbol===sym);
  updateEngCell('engSP','engSPPhase','engSPVol','engSPSig',d.sp_phase,d.sp_recent_vol_pct,d.sp_baseline_vol_pct,d.sp_signals,d.sp_comp_high,d.sp_comp_low,d.sp_bid,d.sp_ask,2,isLive('US500.F'),d.brackets&&d.brackets.sp);
  updateEngCell('engNQ','engNQPhase','engNQVol','engNQSig',d.nq_phase,d.nq_recent_vol_pct,d.nq_baseline_vol_pct,d.nq_signals,d.nq_comp_high,d.nq_comp_low,d.nq_bid,d.nq_ask,2,isLive('USTEC.F'),d.brackets&&d.brackets.nq);
  updateEngCell('engUS30','engUS30Phase','engUS30Vol','engUS30Sig',(d.brackets&&d.brackets.us30?d.brackets.us30.phase:0),0,0,0,(d.brackets&&d.brackets.us30?d.brackets.us30.hi:0),(d.brackets&&d.brackets.us30?d.brackets.us30.lo:0),d.dj_bid,d.dj_ask,2,isLive('DJ30.F'),d.brackets&&d.brackets.us30);
  updateEngCell('engNAS','engNASPhase','engNASVol','engNASSig',(d.brackets&&d.brackets.nas?d.brackets.nas.phase:0),0,0,0,(d.brackets&&d.brackets.nas?d.brackets.nas.hi:0),(d.brackets&&d.brackets.nas?d.brackets.nas.lo:0),d.nas_bid,d.nas_ask,2,isLive('NAS100'),d.brackets&&d.brackets.nas);
  updateEngCell('engCL','engCLPhase','engCLVol','engCLSig',d.cl_phase,d.cl_recent_vol_pct,d.cl_baseline_vol_pct,d.cl_signals,d.cl_comp_high,d.cl_comp_low,d.cl_bid,d.cl_ask,2,isLive('USOIL.F'),null);
  updateEngCell('engGER','engGERPhase','engGERVol','engGERSig',(d.brackets&&d.brackets.ger?d.brackets.ger.phase:0),0,0,0,(d.brackets&&d.brackets.ger?d.brackets.ger.hi:0),(d.brackets&&d.brackets.ger?d.brackets.ger.lo:0),d.ger30_bid,d.ger30_ask,2,isLive('GER40'),d.brackets&&d.brackets.ger);
  updateEngCell('engUK','engUKPhase','engUKVol','engUKSig',(d.brackets&&d.brackets.uk?d.brackets.uk.phase:0),0,0,0,(d.brackets&&d.brackets.uk?d.brackets.uk.hi:0),(d.brackets&&d.brackets.uk?d.brackets.uk.lo:0),d.uk100_bid,d.uk100_ask,2,isLive('UK100'),d.brackets&&d.brackets.uk);
  updateEngCell('engESTX','engESTXPhase','engESTXVol','engESTXSig',(d.brackets&&d.brackets.estx?d.brackets.estx.phase:0),0,0,0,(d.brackets&&d.brackets.estx?d.brackets.estx.hi:0),(d.brackets&&d.brackets.estx?d.brackets.estx.lo:0),d.estx50_bid,d.estx50_ask,2,isLive('ESTX50'),d.brackets&&d.brackets.estx);
  updateEngCell('engBRENT','engBRENTPhase','engBRENTVol','engBRENTSig',d.brent_phase,d.brent_recent_vol_pct,d.brent_baseline_vol_pct,d.brent_signals,d.brent_comp_high,d.brent_comp_low,d.brent_bid,d.brent_ask,2,isLive('BRENT'),d.brackets&&d.brackets.brent);
  updateEngCell('engEUR','engEURPhase','engEURVol','engEURSig',d.eurusd_phase,d.eurusd_recent_vol_pct,d.eurusd_baseline_vol_pct,d.eurusd_signals,d.eurusd_comp_high,d.eurusd_comp_low,d.eurusd_bid,d.eurusd_ask,5,isLive('EURUSD'),d.brackets&&d.brackets.eur);
  updateEngCell('engGBP','engGBPPhase','engGBPVol','engGBPSig',d.gbpusd_phase,d.gbpusd_recent_vol_pct,d.gbpusd_baseline_vol_pct,d.gbpusd_signals,d.gbpusd_comp_high,d.gbpusd_comp_low,d.gbpusd_bid,d.gbpusd_ask,5,isLive('GBPUSD'),d.brackets&&d.brackets.gbp);
  updateEngCell('engAUD','engAUDPhase','engAUDVol','engAUDSig',d.audusd_phase,d.audusd_recent_vol_pct,d.audusd_baseline_vol_pct,d.audusd_signals,d.audusd_comp_high,d.audusd_comp_low,d.audusd_bid,d.audusd_ask,5,isLive('AUDUSD'),null);
  updateEngCell('engNZD','engNZDPhase','engNZDVol','engNZDSig',d.nzdusd_phase,d.nzdusd_recent_vol_pct,d.nzdusd_baseline_vol_pct,d.nzdusd_signals,d.nzdusd_comp_high,d.nzdusd_comp_low,d.nzdusd_bid,d.nzdusd_ask,5,isLive('NZDUSD'),null);
  updateEngCell('engJPY','engJPYPhase','engJPYVol','engJPYSig',d.usdjpy_phase,d.usdjpy_recent_vol_pct,d.usdjpy_baseline_vol_pct,d.usdjpy_signals,d.usdjpy_comp_high,d.usdjpy_comp_low,d.usdjpy_bid,d.usdjpy_ask,3,isLive('USDJPY'),null);
  updateEngCell('engXAU','engXAUPhase','engXAUVol','engXAUSig',safe(d.xau_phase),d.xau_recent_vol_pct,d.xau_baseline_vol_pct,d.xau_signals,0,0,d.gold_bid,d.gold_ask,2,isLive('XAUUSD'),d.brackets&&d.brackets.gold);
  // L2 imbalance bars
  const l2on = d.l2_active === 1;
  updateL2Bar('engSP',d.l2_sp,l2on); updateL2Bar('engNQ',d.l2_nq,l2on);
  updateL2Bar('engUS30',d.l2_dj,l2on); updateL2Bar('engNAS',d.l2_nas,l2on);
  updateL2Bar('engCL',d.l2_cl,l2on); updateL2Bar('engBRENT',d.l2_brent,l2on);
  updateL2Bar('engGER',d.l2_ger,l2on); updateL2Bar('engUK',d.l2_uk,l2on);
  updateL2Bar('engESTX',d.l2_estx,l2on);
  updateL2Bar('engEUR',d.l2_eur,l2on); updateL2Bar('engGBP',d.l2_gbp,l2on);
  updateL2Bar('engAUD',d.l2_aud,l2on); updateL2Bar('engNZD',d.l2_nzd,l2on);
  updateL2Bar('engJPY',d.l2_jpy,l2on);
  updateL2Bar('engXAU',d.l2_gold,l2on);
  // cTrader L2 status indicator in FIX session panel
  const l2badge = document.getElementById('ctL2Badge');
  if(l2badge) { l2badge.textContent = l2on ? 'L2 ?' : 'L2 ?'; l2badge.style.color = l2on ? 'var(--green)' : 'var(--t2)'; }

  // L2 depth panel -- renders order book levels for selected symbol
  updateDepthPanel(d);



  // ?? Compact stat bar + live trades ??????????????????????????????????????
  const pnl=safe(d.daily_pnl),gross=safe(d.gross_daily_pnl);
  const closed=safe(d.closed_pnl), floating=safe(d.open_unrealised_pnl);
  const NZD_RATE=1.66;
  const DAILY_LIMIT=120.0; // must match config daily_loss_limit USD

  // Main P&L value
  const pE=document.getElementById('pnlVal');
  if(pE){pE.textContent=(pnl>=0?'+':'')+pnl.toFixed(2);pE.className='pnl-num '+(pnl>=0?'pnl-pos':'pnl-neg');}

  // Closed / Floating compact
  const cEl=document.getElementById('pnlClosed');
  if(cEl){cEl.textContent=(closed>=0?'+':'')+closed.toFixed(2);cEl.style.color=closed>=0?'var(--green)':'var(--red)';}
  const fEl=document.getElementById('pnlFloating');
  if(fEl){
    const hf=Math.abs(floating)>0.001;
    fEl.textContent=hf?((floating>=0?'+':'')+floating.toFixed(2)):'--';
    fEl.style.color=floating>0.01?'var(--green)':floating<-0.01?'var(--red)':'var(--t2)';
  }

  // NZD equiv
  const nEl=document.getElementById('pnlNzd');
  if(nEl){const nzd=pnl*NZD_RATE;nEl.textContent=(nzd>=0?'+':'')+'NZ$'+Math.abs(nzd).toFixed(0);nEl.style.color=nzd>=0?'var(--amber)':'var(--red)';}

  // Live dot
  const dotEl=document.getElementById('pnlLiveDot');
  const hasOpen=(d.live_trades&&d.live_trades.length>0)||Math.abs(floating)>0.001;
  if(dotEl)dotEl.className='pnl-live-dot'+(hasOpen?'':' no-pos');

  // Compact trade count
  const subEl=document.getElementById('pnlSub');
  if(subEl)subEl.textContent=safe(d.total_trades)+'T '+(safe(d.wins))+'W/'+(safe(d.losses))+'L';

  // Gross/slip sub
  const gSub=document.getElementById('pnlGrossSub');
  if(gSub&&Math.abs(gross)>0.01){const slip=Math.abs(gross-pnl);gSub.textContent='slip -$'+slip.toFixed(2);}

  // Compact stats
  const sw=document.getElementById('statWins'),slE=document.getElementById('statLosses');
  if(sw)sw.textContent=safe(d.wins);if(slE)slE.textContent=safe(d.losses);
  const saw=document.getElementById('statAvgWin'),smd=document.getElementById('statMaxDD');
  const sal=document.getElementById('statAvgLoss');
  if(saw)saw.textContent='$'+safe(d.avg_win).toFixed(0);
  if(sal)sal.textContent='$'+safe(d.avg_loss).toFixed(0);
  if(smd)smd.textContent='$'+safe(d.max_drawdown).toFixed(0);

  // Daily loss limit bar
  const lossUsd=Math.max(0,-closed-floating);
  const limitPct=Math.min(100,lossUsd/DAILY_LIMIT*100);
  const limitFill=document.getElementById('limitBarFill');
  const limitLbl=document.getElementById('limitPct');
  if(limitFill){
    limitFill.style.width=limitPct.toFixed(1)+'%';
    limitFill.style.background=limitPct<50?'var(--green)':limitPct<80?'var(--amber)':'var(--red)';
  }
  if(limitLbl)limitLbl.textContent=limitPct.toFixed(0)+'%';

  // L2 status dots
  const ctLive=!!(d.ctrader_l2_live);
  const goldReal=!!(d.gold_l2_real);
  const l2Dot=document.getElementById('l2DotGold');
  const l2Ct=document.getElementById('l2DotCt');
  const l2LblG=document.getElementById('l2LblGold');
  const l2LblC=document.getElementById('l2LblCt');
  if(l2Dot)l2Dot.className='l2-dot '+(goldReal?'l2-dot-live':'l2-dot-dead');
  if(l2Ct)l2Ct.className='l2-dot '+(ctLive?'l2-dot-live':'l2-dot-dead');
  if(l2LblG)l2LblG.style.color=goldReal?'var(--green)':'var(--t3)';
  if(l2LblC)l2LblC.style.color=ctLive?'var(--green)':'var(--t3)';

)OMEGA23"
R"OMEGA23C(
  // ?? Live open trades panel ??????????????????????????????????????????????
  const ltPanel=document.getElementById('liveTradesPanel');
  if(ltPanel){
    const trades=d.live_trades||[];
    // Fire entry bell when a new position opens
    if(!_openBootDone){_lastOpenCount=trades.length;_openBootDone=true;}
    if(_bellEnabled&&trades.length>_lastOpenCount)_playEntryBell();
    _lastOpenCount=trades.length;
    // Update panel border/glow when positions are open
    const ltOuter=document.getElementById('liveTradesPanelOuter');
    const totalFloat=trades.reduce((s,lt)=>s+lt.live_pnl,0);
    if(ltOuter){
      if(trades.length>0){
        const bc=totalFloat>=0?'rgba(0,217,126,0.35)':'rgba(255,59,59,0.35)';
        ltOuter.style.borderColor=bc;
        ltOuter.style.boxShadow='0 0 8px '+bc;
      } else {
        ltOuter.style.borderColor='rgba(255,255,255,0.08)';
        ltOuter.style.boxShadow='none';
      }
    }
    if(trades.length===0){
      if(ltOuter) ltOuter.style.display='none';
      ltPanel.innerHTML='';
      _ltFingerprint='';
    } else {
      if(ltOuter) ltOuter.style.display='';
      const totalSign=totalFloat>=0?'+':'';
      const totalCol=totalFloat>=0?'var(--green)':'var(--red)';
      const totalNzd=(totalFloat*NZD_RATE);
      const hdr='<div style="display:flex;justify-content:space-between;align-items:center;padding:3px 6px 6px;border-bottom:1px solid rgba(255,255,255,0.07);margin-bottom:4px;">'
        +'<span style="font-size:10px;color:var(--t2);font-weight:700;letter-spacing:1px;text-transform:uppercase;">'+trades.length+' OPEN</span>'
        +'<span style="font-family:IBM Plex Mono,monospace;font-size:13px;font-weight:900;color:'+totalCol+'">'+totalSign+totalFloat.toFixed(2)+'</span>'
        +'<span style="font-size:10px;color:var(--t3);">NZ$'+totalSign+(totalNzd).toFixed(0)+'</span>'
        +'</div>';

      // GoldFlow trail stage from snapshot -- applies to the GoldFlow live trade
      const gfStage    = safe(d.gf_trail_stage);  // 0=initial 1=BE 2=trail1 3=trail2 4=trail3
      const gfAtrEntry = safe(d.gf_atr_at_entry); // ATR at entry -- used to show next stage target
      // Next-stage ATR multipliers matching GoldFlowEngine.hpp constants
      const GF_NEXT_MULT = [1.0, 2.0, 8.0, 15.0]; // stage 0?1, 1?2, 2?3, 3?4
      const GF_NEXT_LABEL = ['1? ATR ? BE', '2? ATR ? Trail', '8? ATR ? Tighten', '15? ATR ? Final', 'MAX'];

      // Fingerprint: rebuild HTML only when pnl shifts >$0.05, SL moves, stage changes, or current price tick changes 2dp
      const ltFp = trades.map(lt=>
        lt.symbol+'|'+lt.side+'|'+lt.engine+'|'+lt.entry.toFixed(2)+'|'
        +lt.current.toFixed(2)+'|'+lt.sl.toFixed(2)+'|'+lt.tp.toFixed(2)+'|'
        +lt.live_pnl.toFixed(1)
      ).join(';') + '|gf'+gfStage+'|atr'+gfAtrEntry.toFixed(2);
      if(ltFp === _ltFingerprint) return;  // nothing meaningful changed -- skip innerHTML rebuild
      _ltFingerprint = ltFp;
      const STAGE_LABEL = ['INITIAL','BE LOCK','TRAIL 1','TRAIL 2','TRAIL 3'];
      const STAGE_COL   = ['var(--t3)','var(--amber)','var(--cyan)','var(--blue)','var(--green)'];

      const maps = trades.map(lt => {
        const isLong  = lt.side === 'LONG';
        const entry   = lt.entry;
        const cur     = lt.current;
        const sl      = lt.sl;
        const tp      = lt.tp;   // may be 0 for GoldFlow
        const pnl     = lt.live_pnl;
        const held    = lt.held_sec < 60 ? lt.held_sec+'s' : Math.floor(lt.held_sec/60)+'m'+(lt.held_sec%60)+'s';
        const dp      = lt.symbol.includes('USD')&&!lt.symbol.includes('GOLD') ? 4 : 2;
        const isGF    = lt.engine === 'GoldFlow';
        const stage   = isGF ? gfStage : 0;
        const pnlCol  = pnl >= 0 ? 'var(--green)' : 'var(--red)';
        const pnlStr  = (pnl >= 0 ? '+' : '') + pnl.toFixed(2);
        const rowBg   = pnl >= 0 ? 'rgba(0,217,126,0.04)' : 'rgba(255,51,85,0.04)';
        // Label for the TP tick: GoldFlow shows next stage target, others show TP
        const tpLabel = isGF && stage < 4 ? GF_NEXT_LABEL[stage] : 'TP';

        // ?? Price ladder geometry ??????????????????????????????????????????
        // Collect all known prices, build a min/max range with 15% padding
        // so labels never clip at edges.
        const pts = [entry, cur, sl > 0 ? sl : null, tp > 0 ? tp : null].filter(x=>x!==null);
        const rawLo = Math.min(...pts);
        const rawHi = Math.max(...pts);
        const pad   = (rawHi - rawLo) * 0.18 || entry * 0.001; // fallback if all same price
        const lo    = rawLo - pad;
        const hi    = rawHi + pad;
        const range = hi - lo;

        // Convert price to % position along the bar (0=left=low, 100=right=high)
        const pct = p => Math.max(1, Math.min(99, ((p - lo) / range) * 100));

        const W = 100; // work in percentage units, rendered as %

        // Key positions
        const xEntry = pct(entry);
        const xCur   = pct(cur);
        const xSl    = sl > 0 ? pct(sl) : null;
        const xTp    = tp > 0 ? pct(tp) : null;
        const xBe    = stage >= 1 ? pct(entry) : null; // BE = entry (SL moved to entry)

        // Zone fills: loss zone (SL?entry) red, profit zone (entry?TP or entry?cur) green
        const zEntryPct  = xEntry;
        const zSlPct     = xSl !== null ? xSl : (isLong ? 1 : 99);
        const lossPctL   = Math.min(zEntryPct, zSlPct);
        const lossPctW   = Math.abs(zEntryPct - zSlPct);
        const profZoneL  = xTp !== null
          ? Math.min(xEntry, xTp)
          : (isLong ? xEntry : xCur);
        const profZoneW  = xTp !== null
          ? Math.abs(xTp - xEntry)
          : Math.abs(xEntry - xCur);

        // Trail stage badge
        const stageBadge = isGF && stage > 0
          ? `<span style="font-size:9px;font-weight:700;padding:1px 5px;border-radius:3px;
              background:rgba(0,0,0,0.3);border:1px solid ${STAGE_COL[stage]};
              color:${STAGE_COL[stage]};margin-left:5px;letter-spacing:0.5px;">${STAGE_LABEL[stage]}</span>`
          : '';

        // Pyramid indicator for GoldFlow stage >= 2
        const pyramidBadge = isGF && stage >= 2
          ? `<span style="font-size:9px;color:var(--gold);margin-left:4px;">?${stage-1}</span>`
          : '';

        // ?? SVG bar ???????????????????????????????????????????????????????
        // Height 28px. Zones behind, tick marks on top, needle for current price.
        const svg = `<svg width="100%" height="28" style="display:block;overflow:visible" preserveAspectRatio="none">
          <defs>
            <linearGradient id="lossGrad${lt.entry.toFixed(0)}" x1="0%" y1="0%" x2="100%" y2="0%">
              <stop offset="0%" stop-color="rgba(255,51,85,0.22)"/>
              <stop offset="100%" stop-color="rgba(255,51,85,0.06)"/>
            </linearGradient>
            <linearGradient id="profGrad${lt.entry.toFixed(0)}" x1="0%" y1="0%" x2="100%" y2="0%">
              <stop offset="0%" stop-color="rgba(0,217,126,0.06)"/>
              <stop offset="100%" stop-color="rgba(0,217,126,0.22)"/>
            </linearGradient>
          </defs>

          <!-- track -->
          <rect x="0%" y="12" width="100%" height="4" rx="2" fill="rgba(255,255,255,0.06)"/>

          <!-- loss zone: SL ? entry -->
          ${xSl !== null ? `<rect x="${lossPctL}%" y="12" width="${lossPctW}%" height="4"
            fill="url(#lossGrad${lt.entry.toFixed(0)})" rx="1"/>` : ''}

          <!-- profit zone: entry ? TP (or entry ? current if no TP) -->
          <rect x="${profZoneL}%" y="12" width="${profZoneW}%" height="4"
            fill="url(#profGrad${lt.entry.toFixed(0)})" rx="1"/>

          <!-- SL tick -->
          ${xSl !== null ? `
          <line x1="${xSl}%" y1="8" x2="${xSl}%" y2="22" stroke="rgba(255,51,85,0.9)" stroke-width="1.5"/>
          <text x="${xSl}%" y="7" text-anchor="middle" fill="rgba(255,51,85,0.9)"
            font-size="8" font-family="IBM Plex Mono,monospace">SL</text>
          <text x="${xSl}%" y="27" text-anchor="middle" fill="rgba(255,51,85,0.7)"
            font-size="7.5" font-family="IBM Plex Mono,monospace">${sl.toFixed(dp)}</text>
          ` : ''}

          <!-- Entry tick -->
          <line x1="${xEntry}%" y1="7" x2="${xEntry}%" y2="23" stroke="rgba(255,255,255,0.5)" stroke-width="1.5" stroke-dasharray="2,2"/>
          <text x="${xEntry}%" y="6" text-anchor="middle" fill="rgba(255,255,255,0.5)"
            font-size="8" font-family="IBM Plex Mono,monospace">IN</text>

          <!-- BE marker (only when stage >= 1 -- SL moved to entry) -->
          ${stage >= 1 ? `
          <circle cx="${xEntry}%" cy="14" r="3" fill="none" stroke="${STAGE_COL[1]}" stroke-width="1.5"/>
          <text x="${xEntry}%" y="27" text-anchor="middle" fill="${STAGE_COL[1]}"
            font-size="7.5" font-family="IBM Plex Mono,monospace">BE</text>
          ` : ''}

          <!-- TP tick -->
          ${xTp !== null ? `
          <line x1="${xTp}%" y1="8" x2="${xTp}%" y2="22" stroke="rgba(0,217,126,0.9)" stroke-width="1.5"/>
          <text x="${xTp}%" y="7" text-anchor="middle" fill="rgba(0,217,126,0.9)"
            font-size="8" font-family="IBM Plex Mono,monospace">TP</text>
          <text x="${xTp}%" y="27" text-anchor="middle" fill="rgba(0,217,126,0.7)"
            font-size="7.5" font-family="IBM Plex Mono,monospace">${tp.toFixed(dp)}</text>
          ` : ''}

          <!-- Current price needle -- diamond shape, glows -->
          <polygon points="${xCur}%,6 ${xCur}%-3,14 ${xCur}%,22 ${xCur}%+3,14"
            fill="${pnl >= 0 ? 'var(--green)' : 'var(--red)'}"
            style="filter:drop-shadow(0 0 3px ${pnl >= 0 ? 'rgba(0,217,126,0.8)' : 'rgba(255,51,85,0.8)'})"
            transform="translate(0,0)"/>
        </svg>`;

)OMEGA23C"
R"OMEGA23D(
        // Polygon points with calc() don't work in SVG -- use viewBox approach instead
        // Rewrite needle as proper SVG with numeric coords via a viewBox
        const VW = 600; // viewBox width units
        const toVB = p => (p / 100) * VW; // % ? viewBox units
        const nx = toVB(xCur);
        const needle = `<polygon points="${nx},4 ${nx-5},14 ${nx},24 ${nx+5},14"
          fill="${pnl >= 0 ? '#00d97e' : '#ff3355'}"
          style="filter:drop-shadow(0 0 4px ${pnl >= 0 ? 'rgba(0,217,126,0.9)' : 'rgba(255,51,85,0.9)'})"/>`;

        const svgFinal = `<svg viewBox="0 0 ${VW} 28" width="100%" height="28"
            style="display:block;overflow:visible" preserveAspectRatio="none">
          <defs>
            <linearGradient id="lg${toVB(xEntry).toFixed(0)}r" x1="0%" y1="0%" x2="100%" y2="0%">
              <stop offset="0%" stop-color="#ff3355" stop-opacity="0.25"/>
              <stop offset="100%" stop-color="#ff3355" stop-opacity="0.06"/>
            </linearGradient>
            <linearGradient id="lg${toVB(xEntry).toFixed(0)}g" x1="0%" y1="0%" x2="100%" y2="0%">
              <stop offset="0%" stop-color="#00d97e" stop-opacity="0.06"/>
              <stop offset="100%" stop-color="#00d97e" stop-opacity="0.25"/>
            </linearGradient>
          </defs>

          <!-- track -->
          <rect x="0" y="12" width="${VW}" height="4" rx="2" fill="rgba(255,255,255,0.06)"/>

          <!-- loss zone -->
          ${xSl !== null ? `<rect x="${toVB(lossPctL)}" y="12"
            width="${toVB(lossPctW)}" height="4" rx="1"
            fill="url(#lg${toVB(xEntry).toFixed(0)}r)"/>` : ''}

          <!-- profit zone -->
          <rect x="${toVB(profZoneL)}" y="12"
            width="${toVB(profZoneW)}" height="4" rx="1"
            fill="url(#lg${toVB(xEntry).toFixed(0)}g)"/>

          <!-- SL -->
          ${xSl !== null ? `
            <line x1="${toVB(xSl)}" y1="8" x2="${toVB(xSl)}" y2="22"
              stroke="#ff3355" stroke-width="1.5" stroke-opacity="0.85"/>
            <text x="${toVB(xSl)}" y="7" text-anchor="middle"
              fill="#ff3355" font-size="7" font-family="IBM Plex Mono,monospace">SL</text>
            <text x="${toVB(xSl)}" y="28" text-anchor="middle"
              fill="#ff3355" font-size="6.5" font-family="IBM Plex Mono,monospace"
              fill-opacity="0.7">${sl.toFixed(dp)}</text>
          ` : ''}

          <!-- Entry -->
          <line x1="${toVB(xEntry)}" y1="7" x2="${toVB(xEntry)}" y2="23"
            stroke="rgba(255,255,255,0.4)" stroke-width="1" stroke-dasharray="3,2"/>
          <text x="${toVB(xEntry)}" y="6" text-anchor="middle"
            fill="rgba(255,255,255,0.45)" font-size="7" font-family="IBM Plex Mono,monospace">IN</text>

          <!-- BE ring (stage >= 1) -->
          ${stage >= 1 ? `
            <circle cx="${toVB(xEntry)}" cy="14" r="4"
              fill="none" stroke="#ff8800" stroke-width="1.5" stroke-opacity="0.9"/>
            <text x="${toVB(xEntry)}" y="28" text-anchor="middle"
              fill="#ff8800" font-size="6.5" font-family="IBM Plex Mono,monospace">BE</text>
          ` : ''}

          <!-- TP -->
          ${xTp !== null ? `
            <line x1="${toVB(xTp)}" y1="8" x2="${toVB(xTp)}" y2="22"
              stroke="#00d97e" stroke-width="1.5" stroke-opacity="0.85"/>
            <text x="${toVB(xTp)}" y="7" text-anchor="middle"
              fill="#00d97e" font-size="7" font-family="IBM Plex Mono,monospace">${tpLabel}</text>
            <text x="${toVB(xTp)}" y="28" text-anchor="middle"
              fill="#00d97e" font-size="6.5" font-family="IBM Plex Mono,monospace"
              fill-opacity="0.7">${tp.toFixed(dp)}</text>
          ` : ''}

          <!-- Trail stop line (stage >= 2 -- SL has moved beyond entry) -->
          ${isGF && stage >= 2 && sl > 0 ? `
            <line x1="${toVB(xSl)}" y1="10" x2="${toVB(xSl)}" y2="20"
              stroke="${STAGE_COL[stage]}" stroke-width="2" stroke-opacity="0.9"/>
            <text x="${toVB(xSl)}" y="6" text-anchor="middle"
              fill="${STAGE_COL[stage]}" font-size="7" font-family="IBM Plex Mono,monospace">TSL</text>
          ` : ''}

          <!-- Current price needle -->
          ${needle}
        </svg>`;

        return `<div style="background:${rowBg};border:1px solid rgba(255,255,255,0.06);
            border-radius:6px;padding:6px 8px 4px;margin-bottom:5px;">
          <!-- Header row -->
          <div style="display:flex;align-items:center;gap:6px;margin-bottom:4px;">
            <span style="font-family:IBM Plex Mono,monospace;font-size:12px;font-weight:700;
              color:${isLong?'var(--gold)':'var(--purple)'};">${lt.symbol}</span>
            <span style="font-size:10px;font-weight:700;color:${isLong?'var(--green)':'var(--red)'};">
              ${isLong?'?':'?'} ${lt.side}</span>
            <span style="font-family:IBM Plex Mono,monospace;font-size:10px;color:var(--t3);">
              @${entry.toFixed(dp)}</span>
            <span style="font-family:IBM Plex Mono,monospace;font-size:11px;font-weight:700;
              color:var(--t2);">? ${cur.toFixed(dp)}</span>
            <span style="font-family:IBM Plex Mono,monospace;font-size:13px;font-weight:900;
              color:${pnlCol};margin-left:auto;">${pnlStr}</span>
            <span style="font-size:10px;color:var(--t3);">${held}</span>
          </div>
          <!-- Engine + stage badges -->
          <div style="display:flex;align-items:center;gap:4px;margin-bottom:5px;">
            <span style="font-size:9px;color:var(--cyan);padding:1px 5px;background:rgba(0,200,240,0.08);
              border-radius:3px;border:1px solid rgba(0,200,240,0.2);">${lt.engine}</span>
            ${stageBadge}${pyramidBadge}
            ${sl > 0 ? `<span style="font-size:9px;color:var(--t3);margin-left:2px;">
              SL dist: <span style="color:var(--red);font-family:IBM Plex Mono,monospace;">
              ${lt.dist_sl!=null?lt.dist_sl.toFixed(2):'?'}</span></span>` : ''}
            ${tp > 0 ? `<span style="font-size:9px;color:var(--t3);">
              TP dist: <span style="color:var(--green);font-family:IBM Plex Mono,monospace;">
              ${lt.dist_tp!=null?lt.dist_tp.toFixed(2):'?'}</span></span>` : ''}
            ${tp > 0 && sl > 0 ? `<span style="font-size:9px;color:var(--t3);">
              RR: <span style="font-family:IBM Plex Mono,monospace;color:${
                (lt.dist_tp/lt.dist_sl)>=2?'var(--green)':(lt.dist_tp/lt.dist_sl)>=1?'var(--amber)':'var(--red)'
              };">${(lt.dist_tp/lt.dist_sl).toFixed(1)}R</span></span>` : ''}
          </div>
          <!-- Price ladder SVG -->
          <div style="padding:0 2px 2px;">${svgFinal}</div>
        </div>`;
      }).join('');

      ltPanel.innerHTML = hdr + maps;
    }
  }

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
  if(bv&&d.build_version){bv.textContent=d.build_version;const bb=document.getElementById('buildBadge');if(bb)bb.title='Built: '+(d.build_time||'?');}
  const bvr=document.getElementById('buildVersionRight');
  if(bvr&&d.build_version)bvr.textContent=d.build_version;

)OMEGA23D"
R"OMEGA23B(
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
  // ?? Watchdog: warn if no trades firing during active session ??????????????
  {
    const banner=document.getElementById('watchdogBanner');
    const msg=document.getElementById('watchdogMsg');
    const age=document.getElementById('watchdogAge');
    if(banner){
      const now=Math.floor(Date.now()/1000);
      const uptime=safe(d.uptime_sec);
      const lastEntry=safe(d.last_entry_ts);
      const lastSignal=safe(d.last_signal_ts);
      const sessionActive=safe(d.session_tradeable)===1;
      // Only warn after 5 min uptime (warmup period) + session must be active
      const warmedUp=uptime>300;
      // Suppress on weekends -- Saturday after 13:00 UTC and all of Sunday
      const nowD=new Date();
      const dow=nowD.getUTCDay(); // 0=Sun,6=Sat
      const isWeekend=(dow===0)||(dow===6&&nowD.getUTCHours()>=13);
      if(sessionActive&&warmedUp&&!isWeekend){
        const sinceEntry=lastEntry>0?(now-lastEntry):uptime;
        const sinceSignal=lastSignal>0?(now-lastSignal):uptime;
        // Warn thresholds: amber at 20min no entry, red at 40min
        const WARN_SECS=1200; const CRIT_SECS=2400;
        if(sinceEntry>=WARN_SECS){
          banner.style.display='flex';
          const mins=Math.floor(sinceEntry/60);
          const isCrit=sinceEntry>=CRIT_SECS;
          banner.style.borderColor=isCrit?'rgba(255,51,85,0.6)':'rgba(255,136,0,0.45)';
          banner.style.background=isCrit?'rgba(255,51,85,0.10)':'rgba(255,136,0,0.10)';
          const col=isCrit?'var(--red)':'var(--amber)';
          banner.querySelector('span').style.color=col;
          if(age) age.style.color=col;
          if(age) age.textContent=mins+'m idle';
          if(msg){
            if(sinceSignal>=WARN_SECS)
              msg.textContent='-- no signals or entries. Check log for blocks.';
            else
              msg.textContent='-- signals generating but entries blocked. Check R:R / spread / gates.';
          }
        } else {
          banner.style.display='none';
        }
      } else {
        banner.style.display='none';
      }
    }
  }

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

  // ?? Cluster Exposure panel ????????????????????????????????????????????????
  {
    const clusters=[
      ['US',  safe(d.exposure_us_equity), 'expBarUS',  'expValUS'],
      ['EU',  safe(d.exposure_eu_equity), 'expBarEU',  'expValEU'],
      ['OIL', safe(d.exposure_oil),       'expBarOIL', 'expValOIL'],
      ['MET', safe(d.exposure_metals),    'expBarMET', 'expValMET'],
      ['JPY', safe(d.exposure_jpy_risk),  'expBarJPY', 'expValJPY'],
      ['EGB', safe(d.exposure_eur_gbp),   'expBarEGB', 'expValEGB'],
    ];
    const maxExp=Math.max(1,...clusters.map(c=>Math.abs(c[1])));
    clusters.forEach(([_,val,barId,valId])=>{
      const bar=document.getElementById(barId);
      const valEl=document.getElementById(valId);
      if(bar){
        const pct=Math.min(100,Math.abs(val)/maxExp*50); // 50% = max half of track
        bar.className='exp-bar '+(val>=0?'long':'short');
        bar.style.width=pct+'%';
      }
      if(valEl){
        valEl.textContent=(val>=0?'+':'')+val.toFixed(0);
        valEl.style.color=val>0?'var(--green)':val<0?'var(--red)':'var(--t2)';
      }
    });
    const tot=safe(d.exposure_total);
    const totEl=document.getElementById('expTotal');
    if(totEl){totEl.textContent='$'+tot.toFixed(0);totEl.style.color=tot>5000?'var(--amber)':'var(--t1)';}
  }

  // ?? Engine P&L attribution ????????????????????????????????????????????????
  if (d.eng_pnl) {
    const ep=d.eng_pnl, et=d.eng_trades||{};
    const pairs=[
      ['Breakout',   ep.breakout,   et.breakout],
      ['Bracket',    ep.bracket,    et.bracket],
      ['MeanRev',    ep.mean_rev,   et.mean_rev],
      ['GoldStack',  ep.gold_stack, et.gold_stack],
      ['GoldFlow',   ep.gold_flow,  et.gold_flow],
      ['Cross',      ep.cross,      et.cross],
      ['Latency',    ep.latency,    et.latency],
    ];
    pairs.forEach(([k,pnl,cnt])=>{
      const p=document.getElementById('engPnl'+k);
      const c=document.getElementById('engCnt'+k);
      const v=safe(pnl);
      if(p){p.textContent=(v>=0?'+':'')+v.toFixed(2);p.style.color=v>0?'var(--green)':v<0?'var(--red)':' var(--t2)';}
      if(c){c.textContent=safe(cnt)+'t';}
    });
    // GoldFlow pyramid indicator
    const gfRow=document.getElementById('gfPyramidRow');
    const gfStage=document.getElementById('gfPyramidStage');
    const gfProfit=document.getElementById('gfPyramidProfit');
    const unlocked=safe(d.gf_stack_unlocked)>0;
    if(gfRow){gfRow.style.display=unlocked?'block':'none';}
    if(unlocked){
      if(gfStage){gfStage.textContent='stage '+safe(d.gf_trail_stage);}
      if(gfProfit){const p=safe(d.gf_profit_usd);gfProfit.textContent=(p>=0?'+$':'$')+p.toFixed(2);}
    }
  }

  // ?? Multi-day throttle badge ??????????????????????????????????????????????
  {
    const streak=safe(d.multiday_consec_loss_days);
    const scale=safe(d.multiday_scale,1);
    const active=safe(d.multiday_throttle_active);
    const badge=document.getElementById('mdThrottleBadge');
    if(badge) badge.style.display=active?'inline-block':'none';
    const dEl=document.getElementById('mdConsecDays');
    if(dEl){dEl.textContent=streak;dEl.style.color=streak>=2?'var(--amber)':streak>=3?'var(--red)':'var(--t2)';}
    const sEl=document.getElementById('mdScale');
    if(sEl){sEl.textContent=Math.round(scale*100)+'%';sEl.style.color=scale<1?'var(--red)':'var(--green)';}
  }

  // Regime -- header strip
  const vix=safe(d.vix_level),reg=d.macro_regime||'NEUTRAL',div=safe(d.es_nq_divergence);
  const vHdr=document.getElementById('vixLevelHdr');

  if(vHdr){vHdr.textContent=vix>0?vix.toFixed(1):'--';vHdr.style.color=vix>=25?'var(--red)':vix<=15?'var(--green)':'var(--amber)';}
  const rHdr=document.getElementById('macroRegimeHdr');
  if(rHdr){rHdr.textContent=reg;rHdr.style.color=reg==='RISK_ON'?'var(--green)':reg==='RISK_OFF'?'var(--red)':'var(--amber)';}
  const dHdr=document.getElementById('esNqDivHdr');
  if(dHdr){dHdr.textContent=(div>=0?'+':'')+(div*100).toFixed(3)+'%';dHdr.style.color=Math.abs(div)<0.0002?'var(--t2)':div>0?'var(--green)':'var(--red)';}
  const sHdr=document.getElementById('sessionValHdr');
  if(sHdr){const trd=safe(d.session_tradeable);sHdr.textContent=trd?(d.session_name||'ACTIVE'):'CLOSED';sHdr.style.color=trd?'var(--green)':'var(--t2)';}
  // Also keep legacy IDs if still referenced
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
    if(dtEl){if(p===1&&safe(s.hi)>0)dtEl.textContent=safe(s.hi).toFixed(2)+'?'+safe(s.lo).toFixed(2);
    else dtEl.textContent='vol '+safe(s.rv).toFixed(3)+'%';}
  });

  renderLastSignal(d);
  renderSLCooldowns(d);
  renderAsiaGate(d);
  renderCrossAsset(d);
}
)OMEGA23B"
R"OMEGA24(

function setConn(ok){
  const d=document.getElementById('connDot'),t=document.getElementById('connText');
  if(d)d.className='dot-conn '+(ok?'dot-ok':'dot-bad');
  if(t)t.textContent=ok?(wsConnected?'Live':'Connected'):'Reconnecting';}

let wsFailCount=0,wsGiveUp=false;)OMEGA24"
R"OMEGA25(

function connectWS(){
  if(wsGiveUp)return;
  const ws=new WebSocket('ws://'+window.location.hostname+':7780');
  ws.onopen=()=>{wsConnected=true;wsFailCount=0;setConn(true);};
  ws.onmessage=e=>{try{updateDashboard(JSON.parse(e.data));}catch(_){}};
  ws.onerror=()=>{wsFailCount++;};
  ws.onclose=()=>{wsConnected=false;setConn(false);if(wsFailCount>=5){wsGiveUp=true;return;}setTimeout(connectWS,Math.min(2000*Math.pow(2,wsFailCount),15000));};
})OMEGA25"
R"OMEGA26(

function httpPoll(){if(wsConnected)return;fetch('/api/telemetry').then(r=>r.json()).then(updateDashboard).catch(()=>setConn(false));})OMEGA26"
R"OMEGA27(


// pollTrades -- reads /api/history which serves from the disk CSV file.
// This survives restarts: yesterday's +$57 shows up immediately on reload.
// Falls back to /api/trades (in-memory) only if /api/history fails.
function pollTrades(){
  fetch('/api/history')
    .then(r=>r.json())
    .then(trades=>{
      renderTrades(trades);
      // Also update daily summary banner from /api/daily
      fetch('/api/daily')
        .then(r=>r.json())
        .then(s=>{
          const el=document.getElementById('tradeCount');
          if(el&&s&&s.total_trades>0){
            const pnl=s.net_pnl||0;
            el.textContent=s.total_trades+' closed ? '+s.wins+'W/'+(s.total_trades-s.wins)+'L ? '+(pnl>=0?'+':'-')+'$'+Math.abs(pnl).toFixed(2)+(s.source==='csv'?' (from disk)':'');
          }
        })
        .catch(()=>{});
    })
    .catch(()=>{
      // Fallback to memory
      fetch('/api/trades').then(r=>r.json()).then(renderTrades).catch(()=>{});
    });
}


setInterval(()=>{const el=document.getElementById('clock');if(el)el.textContent=new Date().toUTCString().slice(17,25)+' UTC';},1000);
connectWS();
setInterval(httpPoll,1000);
setInterval(pollTrades,5000);
pollTrades();
</script>
</body>
</html>

)OMEGA27"

;
} // namespace omega_gui

