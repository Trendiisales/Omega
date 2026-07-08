#!/usr/bin/env python3
"""Fetch histdata.com XAGUSD tick-month zips headless (cookie-session + tk token recipe,
proven for XAUUSD 2026-07-08). Saves to /Users/jo/Tick/XAGUSD/."""
import re, sys, time, urllib.request, urllib.parse, http.cookiejar, pathlib

OUT = pathlib.Path("/Users/jo/Tick/XAGUSD")
OUT.mkdir(exist_ok=True)
UA = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0 Safari/537.36"

def fetch_month(year, month):
    tag = f"{year}{month:02d}"
    dest = OUT / f"HISTDATA_COM_ASCII_XAGUSD_T{tag}.zip"
    if dest.exists() and dest.stat().st_size > 100000:
        print(f"{tag}: already have ({dest.stat().st_size} bytes)")
        return True
    cj = http.cookiejar.CookieJar()
    op = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(cj))
    op.addheaders = [("User-Agent", UA)]
    page_url = f"https://www.histdata.com/download-free-forex-historical-data/?/ascii/tick-data-quotes/xagusd/{year}/{month}"
    try:
        html = op.open(page_url, timeout=60).read().decode("utf-8", "replace")
    except Exception as e:
        print(f"{tag}: page GET failed: {e}")
        return False
    m = re.search(r'id="tk"[^>]*value="([0-9a-f]{32})"', html)
    if not m:
        m = re.search(r'name="tk"[^>]*value="([0-9a-f]{32})"', html)
    if not m:
        print(f"{tag}: no tk token (month likely absent on site)")
        return False
    tk = m.group(1)
    data = urllib.parse.urlencode({
        "tk": tk, "date": str(year), "datemonth": tag,
        "platform": "ASCII", "timeframe": "T", "fxpair": "XAGUSD",
    }).encode()
    req = urllib.request.Request("https://www.histdata.com/get.php", data=data,
                                 headers={"User-Agent": UA, "Referer": page_url,
                                          "Content-Type": "application/x-www-form-urlencoded"})
    try:
        blob = op.open(req, timeout=300).read()
    except Exception as e:
        print(f"{tag}: POST failed: {e}")
        return False
    if len(blob) < 100000 or not blob[:2] == b"PK":
        print(f"{tag}: bad payload ({len(blob)} bytes, head={blob[:40]!r})")
        return False
    dest.write_bytes(blob)
    print(f"{tag}: OK {len(blob)} bytes")
    return True

if __name__ == "__main__":
    months = []
    for y in (2022, 2023):
        months += [(y, m) for m in range(1, 13)]
    months += [(2024, 1)]
    ok = fail = 0
    for y, m in months:
        if fetch_month(y, m): ok += 1
        else: fail += 1
        time.sleep(2)
    print(f"done: {ok} ok, {fail} fail")
