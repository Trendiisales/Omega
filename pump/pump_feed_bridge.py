#!/usr/bin/env python3
"""
pump_feed_bridge — IBKR -> stdout feed for the C++ pump_shadow runner.

DATA ONLY. No trading logic, no orders. This is the thin Python pipe the C++
engine needs because the IBKR API is Python (same pattern as every Omega IBKR
feed: ibkr_dom_bridge.py, OmegaMgcLiveBars). All decisions live in the C++
PumpScalpManager downstream.

What it does, every 5s:
  - (every 30s) scan IBKR for top % gainers; subscribe streaming quotes for names
    already up >= PREFILTER_PCT from the day open (cap MAX_SYMBOLS).
  - on first subscribe, SEED today's 3m history (5m kept for diag) so the engine is warm.
  - emit a price tick and roll 3m+5m bars (engine eats 3m only) from the streaming last price.

Feed protocol (stdout, line-buffered):
  B,SYM,TF_SEC,o,h,l,c,v,ts_ms     one CLOSED timeframe bar
  P,SYM,px,ts_ms                   one price tick

Run:  python pump_feed_bridge.py | pump_shadow.exe --gate 100
"""
import sys, time, math, socket, threading, json, os
from http.server import BaseHTTPRequestHandler, HTTPServer
from ib_async import IB, Stock, ScannerSubscription, util

# Restart persistence: candidates + tracked symbols survive a bridge restart so
# the scanner page is never blank and tracked pumps are re-subscribed instantly.
STATE_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "pump_state.json")

def _utc_day():
    return time.strftime("%Y%m%d", time.gmtime())

def save_state(subs):
    try:
        tmp = STATE_PATH + ".tmp"
        with open(tmp, "w") as f:
            json.dump({"date": _utc_day(), "candidates": _candidates,
                       "subs": {s: sub.day_open for s, sub in subs.items()}}, f)
        os.replace(tmp, STATE_PATH)
    except Exception:
        pass

def load_state():
    """Return {sym: day_open} of same-day tracked symbols; preload candidates."""
    try:
        with open(STATE_PATH) as f:
            st = json.load(f)
        if st.get("date") != _utc_day():
            return {}                      # stale day -> start fresh
        _candidates.update(st.get("candidates", {}))
        return dict(st.get("subs", {}))
    except Exception:
        return {}

IB_HOST, IB_PORT, IB_CID = "127.0.0.1", 4002, 33   # paper; distinct clientId
PREFILTER_PCT = 40.0      # only subscribe names already this far up from open (engine gate is 100)
MAX_SYMBOLS   = 12
TFS           = [180, 300]        # S-2026-06-11 trio->3m-only: 180 = the ONLY engine
                                  # feed (PumpScalpManager drops tf!=180); 300 kept
                                  # purely for the _diag_5m /api mirror. 3m edge:
                                  # pump_tf_bt.py n=42 PF 36.4/20.7 @1%/2% slip.
SCAN_EVERY    = 30
TICK_EVERY    = 5
SERVE_PORT    = 7782      # TCP server for the in-Omega PumpFeedConsumer (--serve)

# In server mode, feed lines stream to the connected Omega consumer; else stdout
# (standalone pump_shadow.exe). A background thread owns the accept loop.
_conn = None
_serve = False
_need_reseed = False   # set by accept loop on (re)connect; consumed by main loop
_candidates = {}          # sym -> dict(px, day_open, up, ts) for the scanner web page
SCANNER_HTTP_PORT = 7783  # "separate address": http://<vps>:7783 shows live pump candidates

# Chimera brand mark (same PNG as the Omega dashboard header) inlined as base64 so
# the scanner page is self-contained; recoloured BLUE via a CSS hue-rotate filter.
LOGO_B64 = "iVBORw0KGgoAAAANSUhEUgAAAEAAAABACAYAAACqaXHeAAAI80lEQVR4nO2bCYydVRXHf++bN+2bGVpsaW2xLiylBRREDdJFqxEFl7YCCmKxGo0gcU8UFPeFADECplpRg2jFCKIGBSvurSJVUalaQEUtFLGbdpkuM11m3jM3+d3k5stbZ6YB/fwnkzfzvnvvu+ec/1nuuW9KB1fNrFFgZBQcGQVHRsGRUXBkFBxloETBFVCjwCgXnQEZBUeZxwZmAluA3R3O6wMmA3OAxwH/Bm79b1LAW4G5wA+BXwLjFaIZDgPOAuYDU4AJwOOBAWAd8ArgJmDlY10BHwDOBN4AzAIuB56iIN8Frq4zZwbwCeA4/644/l/AkAwKYz4InAB80nHTgc31NlE6uCqwryPMMHbsAl4p/X4PLO9gjWOA7wCfBoaBS4D7gVuAnVoxWHNZbt4NCrYX+CfwG+AkYB5wAOgHrgMm6RIbgacDfwRuHAsGLNVi+9X20SojWHAxsA1Y40aDZRrhXGBQQSdJ/7cBe3z+feB1+vd231usUJuA9cC7fT/MP8fXEBOeBfwDuEP3CJ/TEOUOFfAtYBqwBOhRCcFiP5EZZ0jLXwG/bbDGq4BXa624wXHAh4EnOO9aYEVu3jNdO+Bryfs7gC+22PfEsUiDE4AXAqdp3ZobX6EiQuD5uv4WfO+jWiSP50n7sMaDiUvMNwgGetdDn8+79fdOENx11Az4rP50UOtl/n6xLvB54D4tvEfKBhd5Z0JjpHEXcK8R/zgVmKm04Pv10J+w5jz9etTI2hgTNP8uKfht4DKtFCrIqiwIAenJwCLjQ5dCj3dexAuAY6VtSH9n6wKZc0KEf2qDfaxS0UMG3pBFjhytAsotnr8EuBB4kgKFNHW4/p/SMAh9skqJQSdQdV/OoktlTdWfA7pSzTUf1M+n+Znh/Ucsku4yPQbFPSTDgiLWmoUe0hCbx1IBZX/2uvGjtfigAkSUFCZFEP5pwNuB9xs7gqW3Ake4xnBSzX3ZKu71prUeFRA++25T5nuBjxtsq64x3TQ6oOuFLPFT4HvtVJZZi+eHG5lLbrK3jvBRASXXi7+XpfoCg9xf3eR0BQtKuUifvhn4MfAlYKHs2a8Sx/leiEFTLXLeokuEcUfpPjEjnWqKvF62jEoB90nBbRYlX9DSWXKMjgLXO1VGap+mC12ipYNyHnDt27T8FQq73XlRociaYxQcU+VlBuBPmfODMk5UcT9zfnh2+mhcYK0FyWxpiCxY6kbrKTAqIgpRSYJVoOjvfD+mphD4PqJ1dyd7KiWv47Xu8w2if/fZZmv+lbrVfGuUU/2sAdnwJ6vCEZfCV2rJAa1ZkZ6NLB83fkA3eFgGDec2MsnK8pwkG0SkDMO5h2ndEAuaFT2XS//gVqf4evVo6oBpBqZN0u8kmdCsIKmqtA9J9RAL3mjZO2g9MMu1d+nHzdBlQHyO9cIKS+I8dinsDZbFQWnHN1q03KYCgn9eBXxFn23nqFmTukEJFwDP1k9fpKWrsmhPrjXXrEMV0+wZusPPga8Cf8mN2+C4PrNXLKFHrIC/efLDmv0IN97VwPdrrt2tXx9rENtmkEr9O2sgeCnXryzlmNBlnRKYdTvwDQVHt5rhwWxqcsgasQIiFpmSdjSZW5N2d1jqlkx9Kc3zwnWKuE6/v58PvFhX6zF1bzd4TzXIjokCXiNtY75PUUteu9X+DzwbzEjOD6NB/jOjK+1UlhMNvPuk/UQLolvHQgFz1OhWBWy0Xsjl95iibnLsYBsUr4d2OtYlWTBkepzkPjbIwhAjGAsFDBtdz1W7Qzm/77ZDM88qbLGl8A4D0Uja7zGeNEu3cW+horzGOqOSxIOmyDrYTBButRpOhUcL7DHXL9MPF8qW4VHePYx3n8OJQvKIBjjZg1NbwjMCn+yrcw6oqvF7rQ1uN1enAa9T1JIT43qjfm+dz45jY28inFc6Qtbh+I0Gs3ze7vU4Osda//gkWNbbLHXOEWnFV/YgNkUGLDeujM8xIQ28kQWHVAG7PB9kRtsuX0P6eaLPZ5v28n4fW2ixhE6pXFWwcVqxXyaFI/Cvdbtljmu057j+IVVAwDetCtdazgaB3mMz4oCxIAauiKrus9qqraLAQ47tNWU9otDv8/BVMcXtdP27DXZhboqSCp/hWm2jTOdY589aKT9FJaxxkzsthCIDqlLzYdvdH3N8jwLut6NzmwedvfYKLkiUGaj/I1Pac5PjeFRyZOR0q8PQvW4LGSPDBM/Z7wD+4IFmuZu70+epf/aZRWZaFpeMJ7d4H3CxBUvsCG9R+K26w1Y70rNkyOQkUEaUnH+RBVto3x2yq7EJ3uvdZdMhWn2WUbs7V+9HFgTWXCqDQrOlEVaaRifLkLKsWWh/oseTZeYRfUi3yPx5E/BmC7GgsDFnwALnztPyF8qEK/TDmPujEgaSK7SbWwiPx+5LtX4lCZKRVdfLvjVSv6JRyo7d69hntBIkG6ECFrmhPa6xxACHDYj0WqyiIOu8V2gXD9jY6FKR+7xDCDfDmB3CncNrDcI3etqsuKchmTLmClhgno9aDh/yZ7PD7OSGN66/w9fPyZJOcH9yioz1QegnpNisK15nPNlhOoyF0ZFjrYAlajemtgN2aOKtbkxxB1XOJju5YWOd4nwLon0Jkz7TZPwmb5173VevFzZjpoCzpXi/i/cbcNYZ4c/ULUr65Gqpek+z+7kGeKmNjdgxqlofhAZnM9yZMLDb9t2ossBcg0nVk+CA1lhvvp5vipprvh5U+xssXEKqaxcvV3EVL0gGpX+3bfBGN854/phoRYrzulqlw3KLDZ3iEbgnCXpDbjD43Mv0u90KvTe5ObqyxXcE8jhPIQ8aWyKTem2s/qLJ3KPMQL26yz6Fr/n1mxErYKnCbtcKXf69ReHOMggN+7w7GbepWS8uhz6/M7A7yftlBbq2lRCycKJflSnr2hWVFlyiIUot7gVmKlBa1cXiY5v0Suvy2AyNtXsnOCF3wiu5drhSa4UpNmpjn6KmETa2MkJpBN8R+p9CRsGRUXBkFBwZBUdGwZFRcJT//w8TFPsfJspFZ0BGwZFRcGQUHBkFR/Zob+DRxn8AM8dvx2O9DVIAAAAASUVORK5CYII="  # noqa: E501
LOGO_BLUE_B64 = "iVBORw0KGgoAAAANSUhEUgAAAEAAAABACAYAAACqaXHeAAAJgUlEQVR42u2be5BWdRnHP+/77gu77EFgvSGYeEHwMe/WsIrRTKJmKWGlKUVMJNqkljNJiWhoOugw3gZSyiHM0QkzHPOWjlnqIBQXBUV8SvMa97jqby/ssvv2R9/TnDnz7vvuLrviDO+ZOQPvOef3O7/n+3yf6+9sZvjy1gL78JFlHz8qAFQAqACwbx9VHiyzLwOQ2dfD4D7PgIoP+DQswiIfDmzyYB93cVwtUAfUAwOBLR7ssS75gNYXhu9Nwa8ETgOeA/4GbPNgW8qMiYDxwGjgAKA/cBDQCKzW7wUe7OlPNQAW+fXAOcBkYAQwCRgmQZ7yYHcUGTMUmAUcrUvVen4nsBtYAxwjQBZ6sNs1brAH29gjAGgRWeAj4Jui3yoPdk8X5jgSeByYA7QBU4E3gUeAHcA3gNUebHZq3HzAgAZgHbAcOB44HWgREHOBQTKJ9cCJwOse7ME99gEW+URpbBfwMXCEwBhhkY8DtgJLgPkerLHEVBcCTRJ0kOh/lQcLuv+sRT7JIq/zYNv07nESagPwrge7RtcHAV/XPLXAqcC/gWdkDk096QQfBQ4GJgA1AmEH8BdgKHC2aPl3YEUHIH4LuETaihfYB5hhkQ8BVniwuzzYA6mhp2hugN/FFz3YduA3ZRS33x6HQYu8PzAWGCW7K2jhDwiIBcDvgcHA7Rb5TfLS6eOLon0j8J6uHSmn1lf0LnbU6n5e9t7pw4N91BN5wL3ALVpsi661Aj8AZgDnyo776v44YK5FXpeaZyCQA94AXheTGmVWN3mw+zp4/06dABd9YomQRV5rkf9EFPwjME1aygDtYsE64DDgfAmSA7YJjFMSc30JOArYrvB3gUwgqzGzLPLPdrCUFxQtdgP1Fvn1FvkhvZoIWeTnAlOAz0igYcAAaS1Jw13ACQIldjp5oFmxOT4mijXtOltkSgXN+R5QbZEfrHcWgLUebJMHW2yRPyXg3hfD6i3ylcAqXVvXUbjrrhOs0tmghR8hjTdJgP+H04RZxEczcBzwI2C6RT5Kmt4M7K852mTbdcBvgceA7yms1QiABot8GTDHg11rkd8sZ9uuOQYrjDYCwSLfAPwV+FNnMstyJjAAGCIB64B+RYSPAchovvj/VaL6GMX9t7XIwRJsOnCZIsHDwPPA/cB5Ys8ugdhH1+61yA/0YDcAV8gk8sDhMp84In0euAaYZ5GftqcArAHWKr7PBu6TprMSIil8saoypvYoxfOp0nQV8JYHWws8Ic3PlLDbNC6TWN9mOd8r5NVXeLBpcsB3K+bngWMF3Esaf7dFfuYepcIW+QBgpAdbpt9Xy5a3CXlKCN8ujc/3YHdq/KnA5zzYr/W7H3Aj8AVpsSo1Rzx3u+5934O9U2Sd+yuUTgAOAYIY26wx67udClvktybC1SglJM0lNJ/0C9uBD8WgtuRClMVNVibXlGJkIQVuGxABL3mwa8skPbfIWT4PnAQ8X6y26AoA8+WYNijDO17o7i4BQEG0nCozGgNcqrS3SfnACGWWHyXY1BEAMQuqlXk+4MHe7WC9w4D5CdDWeLApe5IIzVTicpcHm6qJyx0F5QE1wLeV9+eBs1TOjhEIIUX7TAm/EofZs4H7LfKbLfKRRTK/D/RcXtGreo9qAQ/2L1V+WOQzFMZCSmuZhOAFzZ2XXR8lJ7ZVTiqTGJPtwI9kEixI3s8pLOeUfY6xyJ8E/iDBscgnqzZZAhyotdIT1eD5CknbS4wtiHbPAFu08MEpmqeF6+qRS6THOeBi4MsW+Voxboic9EgBcGNP9QS/I+eXLaKxQuLMA0s82BxpfqjMppTT7FQDJzU+K7+wQwo5Vu/aJfM7CHimVJusqgvarxeimyVgR/P1AV4FNlrkC/RsUyco3pHAnQElJ4f8jvoCVcAHEv6hnuoHtAF3qJlRnYoAsdaXK1rMU65+nMyltZuaLyRygUyZtfUH7gReAapjf9BjbXEPthx4UQinw19Ojma2zhr5is0J6nf36Kt1tiUA6SjknqDC6QN6aV+gtkgdEMfmN5QbPAmcUSZL7Izm44rxXXn9fkXeHT+bFcvqentjZL08b1UqUemncrReuf4xCWdZbLGlYn6b5h+gtndf4B75lb4pJhRSppLvVQDUWlqpcS3ScosyvUMV6kYq7KXtPm6hNRdxgO0SrI+0uFNMmgMsldnN1nPZEqzp80lsjS0EbhMQAyXQz9QVapEvKBRJYWvlQ/4pk2mTLymIQfsJyDnAdcAyPbdD5zpd618kE80I8KEqrnpva8yDrQZWqxNTL5o2K+tap8VGCQa0i5ofAs8Cv9DzNRJwlzo6T6jQabDIpyt9DomU+s/AQ6oasymQY0YOVnb4aK/uDapDfCbwY+ByFTSzFCYXqWm5NUH1WoXI4UqL35c/WaFK7ZXUKzZJ+M0JWo8VA+ao4bE15VMycpaXWeQ1wKLORIPubo72B64EFqshEWt9hLx2PpXvxyxYCfxUuz5rSsz/tMJonRhSJdac58EmScBLJXyjTKlNv7NSyg8t8gXKRnt8e3yMxp4uDz0FeM2Dzewg7W0E6j3YKg/2cBnh8WAbBNROmUl7wvvjweaJfUtE/WoppUrPNujZk3vr+4DztaCgOSbIwaEGRHJbrFqCrLbIT+yCr3lLjY2cgGwGjrbIx+v+Ug92NfBdOeEHZRbVWtNu+ZmeBcAiH6M435Do+f0DWKjafFgCgKxS4SzwKw/2Whdf92aiiozzg7NSQG30YIs92FzgKr2vT5wYlds76A4DJgjdOLS1aEcn3tWNQ1yrwNkA3KCFdfW4WAlRc4JJvyxjOo8rrLbo38N6DACL/AJRfKcm3wlc7sFW6yuPc2QWGdnki8BSD/Zqqf25Dt71FfULQ8K2r/NgXmboogQD82rfdT8KqLd+shZwoSavlrefDoy2yMeqCdlXpW+LytFlHuyRLgj9NQFXrQ2SJtE/D0zzYCtKjD1DydShiQItJ5Ps9tbYSYrtNQmnt1sLnAt8VXb3sYRuSOwc3ZpyhuWEv0j7jq3yLSFRZ/zcg71cYuzh6lvGbfBmCV/gf5/fdJsBEyXsNmkhp9+bJNx4YKPAySfOHLAh8cFDZz52ukRA7kpsyfVTI/a5MlOMlvb/o3FZrfNlD7ao2xsjsut8quqKk4+toldbqiCpAnZ4sHVdtHlLVXgZ7SO83YmxB6hRuzuRfeaA9eWUsFe/EqPyoWQFgAoAFQAqAFQAoPIHE1T+YKLCgIoPqABQAaACQAWAvXD8FxeNf73qd8diAAAAAElFTkSuQmCC"  # pixel-recolored blue (favicon + header; CSS filters cannot recolor favicons)  # noqa: E501


class _ScanHandler(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        rows = sorted(_candidates.values(), key=lambda c: -c["up"])
        if self.path.startswith("/api"):
            body = json.dumps(rows).encode(); ctype = "application/json"
        else:
            # styled to match the Omega dashboard SCREENER panel (dark/mono/teal).
            # NO meta-refresh: a tiny script polls /api every 5s and rewrites the
            # table in place — no full reload, no favicon flash, no tab spinner.
            def row(c):
                arrow = '<span style="color:#3ddc97">&#9650;</span>'   # up-mover
                gate = '<span style="color:#3ddc97">TRADE</span>' if c["up"] >= 100 else \
                       '<span style="color:#6b6b6b">watch</span>'
                return (f"<tr><td style='color:#d8d8d8'>{c['sym']}</td>"
                        f"<td>{c['px']:.3f}</td><td>{arrow}</td>"
                        f"<td style='color:#e6a23c'>+{c['up']:.0f}%</td>"
                        f"<td>{c['day_open']:.3f}</td><td>{gate}</td></tr>")
            trs = "".join(row(c) for c in rows) or \
                  "<tr><td colspan=6 style='color:#6b6b6b'>no live movers — waiting for a pump</td></tr>"
            n_trade = sum(1 for c in rows if c["up"] >= 100)
            script = (
                "<script>\n"
                "async function rf(){try{\n"
                " const r=await fetch('/api',{cache:'no-store'}); const d=await r.json();\n"
                " d.sort((a,b)=>b.up-a.up);\n"
                " const esc=s=>String(s).replace(/[&<>]/g,m=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[m]));\n"
                " let h='';\n"
                " for(const c of d){\n"
                "  const gate=c.up>=100?'<span style=\"color:#3ddc97\">TRADE</span>':'<span style=\"color:#6b6b6b\">watch</span>';\n"
                "  h+='<tr><td style=\"color:#d8d8d8\">'+esc(c.sym)+'</td><td>'+c.px.toFixed(3)+'</td>'\n"
                "    +'<td><span style=\"color:#3ddc97\">&#9650;</span></td>'\n"
                "    +'<td style=\"color:#e6a23c\">+'+Math.round(c.up)+'%</td>'\n"
                "    +'<td>'+c.day_open.toFixed(3)+'</td><td>'+gate+'</td></tr>';\n"
                " }\n"
                " if(!h) h='<tr><td colspan=6 style=\"color:#6b6b6b\">no live movers — waiting for a pump</td></tr>';\n"
                " document.getElementById('rows').innerHTML=h;\n"
                " const ng=d.filter(c=>c.up>=100).length;\n"
                " document.getElementById('hdr').textContent='PUMP SCANNER · '+d.length+' live movers · '+ng+' at gate (≥100%)';\n"
                "}catch(e){}}\n"
                "setInterval(rf,5000); rf();\n"
                "</script>")
            body = (
                "<html><head><meta charset=utf-8><title>PUMP SCANNER</title>"
                f"<link rel=icon type=image/png href='data:image/png;base64,{LOGO_BLUE_B64}'>"
                "<style>"
                "body{background:#0a0a0a;color:#c8c8c8;font:13px/1.5 'SF Mono',Menlo,monospace;margin:0;padding:18px}"
                ".panel{border:1px solid #1d1d1d;border-radius:6px;padding:14px 16px;max-width:720px}"
                ".brand{display:flex;align-items:center;gap:10px;margin-bottom:12px}"
                ".brand img{width:26px;height:26px;border-radius:4px}"
                ".brand .t{color:#5fd3e0;letter-spacing:3px;font-weight:bold;font-size:14px}"
                ".brand .v{color:#3a5b62;letter-spacing:2px;font-size:10px;text-transform:uppercase}"
                ".hdr{color:#9aa0a6;letter-spacing:.5px;font-size:12px;margin-bottom:10px}"
                "table{border-collapse:collapse;width:100%}"
                "th{text-align:left;color:#5fd3e0;font-weight:normal;padding:4px 16px 8px 0;border-bottom:1px solid #1d1d1d}"
                "td{padding:5px 16px 5px 0;white-space:nowrap}"
                ".foot{color:#6b6b6b;font-size:11px;margin-top:12px}"
                "</style></head><body><div class=panel>"
                f"<div class=brand><img src='data:image/png;base64,{LOGO_BLUE_B64}' alt='Chimera'>"
                "<span class=t>CHIMERA</span><span class=v>pump scalp</span></div>"
                f"<div class=hdr id=hdr>PUMP SCANNER &middot; {len(rows)} live movers &middot; {n_trade} at gate (&ge;100%)</div>"
                "<table><tr><th>symbol</th><th>price</th><th>trend</th><th>up from open</th>"
                "<th>day open</th><th>status</th></tr>"
                f"<tbody id=rows>{trs}</tbody></table>"
                "<div class=foot>up = % above today's open &middot; TRADE = past the 100% gate the 3m "
                "engines act on &middot; live pump trades show in the dashboard RUNNING TRADES panel</div>"
                f"</div>{script}</body></html>").encode()
            ctype = "text/html; charset=utf-8"
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Access-Control-Allow-Origin", "*")   # so the dashboard could embed it later
        self.send_header("Content-Length", str(len(body))); self.end_headers()
        self.wfile.write(body)


def _http_thread(port):
    try:
        HTTPServer(("0.0.0.0", port), _ScanHandler).serve_forever()
    except Exception as e:
        sys.stderr.write(f"[pump_bridge] scanner http error: {e}\n")


def _serve_thread(port):
    global _conn, _need_reseed
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port)); srv.listen(1)
    sys.stderr.write(f"[pump_bridge] serving on 127.0.0.1:{port}\n"); sys.stderr.flush()
    while True:
        c, _ = srv.accept()
        sys.stderr.write("[pump_bridge] consumer connected\n"); sys.stderr.flush()
        _conn = c
        # New consumer = fresh (or restarted) Omega: its engines are COLD with a
        # re-anchored day_open -> gate reads ~0% on a +200% name and the LB+21
        # bar warmup blocks entries for hours. Flag a full re-seed; the MAIN
        # loop does the actual ib calls (ib_async is not thread-safe here).
        _need_reseed = True


def emit(s: str):
    global _conn
    if _serve:
        if _conn is not None:
            try:
                _conn.sendall((s + "\n").encode())
            except Exception:
                try: _conn.close()
                except Exception: pass
                _conn = None        # accept loop will pick up the next consumer
    else:
        sys.stdout.write(s + "\n")
        sys.stdout.flush()


class Sub:
    """One subscribed symbol: streaming ticker + per-TF bar aggregation state."""
    def __init__(self, ticker):
        self.ticker = ticker
        self.day_open = 0.0
        self.last_vol = None
        self.bars = {tf: None for tf in TFS}   # tf -> [bucket_ms, o,h,l,c,v]
        self.h5   = []                          # closed 5m bars [o,h,l,c,v] (diag mirror)
        self.diag = ""                          # engine-condition mirror of the last closed 5m bar

    def _diag_5m(self, o, h, l, c, v):
        """Mirror the engine's ignition conditions on the just-closed 5m bar so
        a blocked entry is visible from /api (which condition failed and why)."""
        self.h5.append([o, h, l, c, v])
        if len(self.h5) > 26: self.h5.pop(0)
        if len(self.h5) < 5: self.diag = "warmup"; return
        c_lb  = self.h5[-4][3]                                  # close 3 bars back
        ig    = (c/c_lb - 1.0)*100.0 if c_lb > 0 else 0.0
        prior = [b[4] for b in self.h5[:-1]][-20:]
        avgv  = (sum(prior)/len(prior)) if prior else 0.0
        volx  = (v/avgv) if avgv > 0 else -1.0                  # -1 = no live volume at all
        stren = (c-l)/(h-l) if h > l else 1.0
        self.diag = (f"ig={ig:+.1f}%(need+3) volx={volx:.1f}(need3) "
                     f"str={stren:.2f}(need.60) v={v:.0f} avg={avgv:.0f}")

    def roll(self, sym, px, vol, ts_ms):
        if px is None or px <= 0 or px != px:
            return
        emit(f"P,{sym},{px},{ts_ms}")
        dv = 0.0
        if vol is not None and self.last_vol is not None and vol >= self.last_vol:
            dv = vol - self.last_vol
        if vol is not None:
            self.last_vol = vol
        for tf in TFS:
            bkt = (ts_ms // (tf * 1000)) * (tf * 1000)
            b = self.bars[tf]
            if b is None:
                self.bars[tf] = [bkt, px, px, px, px, dv]
            elif bkt != b[0]:
                emit(f"B,{sym},{tf},{b[1]},{b[2]},{b[3]},{b[4]},{b[5]},{b[0]}")   # closed bar
                if tf == 300: self._diag_5m(b[1], b[2], b[3], b[4], b[5])
                self.bars[tf] = [bkt, px, px, px, px, dv]
            else:
                b[2] = max(b[2], px); b[3] = min(b[3], px); b[4] = px; b[5] += dv


def fetch_seed(ib, sym):
    """Fetch today's 3/5/15m history. Returns (day_open, day_high, seed_lines,
    contract) or None. Caller decides whether to EMIT (emit R first: the consumer
    resets the symbol's engines so a re-seed never double-counts bars/VWAP)."""
    c = Stock(sym, "SMART", "USD")
    try:
        q = ib.qualifyContracts(c)
        if q: c = q[0]
    except Exception:
        return None
    day_open = day_high = None
    lines = []
    for tf in TFS:
        bs = "3 mins" if tf == 180 else ("5 mins" if tf == 300 else "15 mins")
        try:
            bars = ib.reqHistoricalData(c, endDateTime="", durationStr="1 D",
                                        barSizeSetting=bs, whatToShow="TRADES",
                                        useRTH=False, formatDate=1)
        except Exception:
            continue
        for b in bars:
            ts = int(b.date.timestamp() * 1000)
            lines.append(f"S,{sym},{tf},{b.open},{b.high},{b.low},{b.close},{b.volume},{ts}")  # S=seed: warm only
            # day anchor from whichever TF returns bars (was 5m-only; a failed
            # 5m fetch killed the subscribe even when 3m/15m succeeded)
            if day_open is None and b.open > 0:
                day_open = b.open
            day_high = b.high if day_high is None else max(day_high, b.high)
    return (day_open, day_high, lines, c) if day_open else None


def subscribe_symbol(ib, subs, sym, min_move):
    """Qualify + seed + stream one symbol. min_move=0 restores a previously
    tracked name unconditionally (it earned tracking earlier in the day)."""
    if sym in subs or len(subs) >= MAX_SYMBOLS:
        return False
    sd = fetch_seed(ib, sym)
    if not sd:
        return False
    d_open, d_high, lines, c = sd
    move = (d_high / d_open - 1.0) * 100.0 if d_open else 0.0
    if move < min_move:
        return False
    emit(f"R,{sym}")                       # consumer: clean re-warm before seed batch
    for ln in lines:
        emit(ln)
    tk = ib.reqMktData(c, "", False, False)
    ib.sleep(0.5)
    s = Sub(tk); s.day_open = d_open
    subs[sym] = s
    emit(f"# subscribe {sym} day {d_open:.3f}->{d_high:.3f} up={move:.0f}%")
    return True


def main():
    ib = IB()
    ib.connect(IB_HOST, IB_PORT, clientId=IB_CID, timeout=15)
    try:
        ib.reqMarketDataType(3)   # delayed ok in paper if no live entitlement
    except Exception:
        pass
    subs = {}           # sym -> Sub
    last_scan = 0.0
    pending = load_state()    # same-day symbols from a prior run (restart persistence)
    emit(f"# pump_feed_bridge up (restoring {len(pending)} persisted symbols)")

    global _need_reseed
    while True:
        now = time.time()

        # ---- restore persisted same-day symbols (first pass after restart) ----
        if pending:
            for sym in list(pending.keys()):
                subscribe_symbol(ib, subs, sym, min_move=0.0)   # tracked before -> keep
                pending.pop(sym, None)

        # ---- consumer (re)connected: replay today's history for every tracked
        #      symbol so a restarted Omega recovers true day_open + run_high +
        #      full bar warmup instead of re-anchoring at the reconnect price ----
        if _need_reseed and subs:
            _need_reseed = False
            for sym, s in list(subs.items()):
                sd = fetch_seed(ib, sym)
                if sd:
                    emit(f"R,{sym}")               # reset-then-warm: no double-count
                    for ln in sd[2]:
                        emit(ln)
                    if sd[0] > 0:
                        s.day_open = sd[0]
                emit(f"# reseed {sym} day_open={s.day_open:.3f}")
        elif _need_reseed:
            _need_reseed = False                    # nothing tracked yet

        # ---- periodic scan + subscribe ----
        if now - last_scan >= SCAN_EVERY:
            last_scan = now
            try:
                sc = ScannerSubscription(instrument="STK", locationCode="STK.US.MAJOR",
                                         scanCode="TOP_PERC_GAIN")
                sc.abovePrice = 0.3
                rows = ib.reqScannerData(sc)
                for item in rows[:40]:
                    if len(subs) >= MAX_SYMBOLS:
                        break
                    sym = str(item.contractDetails.contract.symbol).upper()
                    subscribe_symbol(ib, subs, sym, min_move=PREFILTER_PCT)
            except Exception as e:
                emit(f"# scan error: {e}")

        # ---- emit ticks + roll bars + scanner candidate for all subs ----
        ts_ms = int(now * 1000)
        for sym, s in list(subs.items()):
            tk = s.ticker
            px = tk.last or tk.marketPrice() or tk.close
            vol = getattr(tk, "volume", None)
            try:
                s.roll(sym, float(px) if px else None, float(vol) if vol else None, ts_ms)
                if px and s.day_open:                              # C = scanner candidate (feed + web page)
                    up = (float(px) / s.day_open - 1.0) * 100.0
                    emit(f"C,{sym},{float(px)},{s.day_open},{up:.1f},{ts_ms}")
                    _candidates[sym] = {"sym": sym, "px": float(px), "day_open": s.day_open,
                                        "up": up, "ts": ts_ms, "diag": s.diag,
                                        "nb5": len(s.h5)}   # closed-5m-bar count (plumbing truth)
            except Exception:
                pass

        save_state(subs)        # restart persistence: candidates + tracked symbols
        ib.sleep(TICK_EVERY)


if __name__ == "__main__":
    if "--serve" in sys.argv:
        _serve = True
        i = sys.argv.index("--serve")
        port = int(sys.argv[i+1]) if i+1 < len(sys.argv) and sys.argv[i+1].isdigit() else SERVE_PORT
        threading.Thread(target=_serve_thread, args=(port,), daemon=True).start()
    threading.Thread(target=_http_thread, args=(SCANNER_HTTP_PORT,), daemon=True).start()  # scanner web page
    try:
        main()
    except KeyboardInterrupt:
        pass
