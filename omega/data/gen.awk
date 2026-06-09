# data/gen.awk  --  deterministic synthetic OHLCV fixtures for Omega demos.
# Usage: awk -v mode=TREND -f gen.awk dates.txt > TREND.csv
BEGIN { print "date,open,high,low,close,volume" }
{
  i = NR - 1
  date = $0
  d = 0
  if (mode == "TREND") {
    if (i < 250)      { base = 50 + 0.5*sin(i/8.0);                 d = 0;  vol = 1000000 + 50000*sin(i) }
    else if (i < 430) { base = 50 + 0.40*(i-250) + 1.5*sin(i/6.0);  d = 1;  vol = 3200000 + 400000*sin(i/3.0) }
    else if (i < 470) { base = 122 + 1.8*(i-430);                   d = 1;  vol = 6000000 + 300000*sin(i/2.0) }
    else              { base = 194 - 1.1*(i-470) + 1.5*sin(i/5.0);  d = -1; vol = 5000000 + 300000*sin(i/2.0) }
  } else if (mode == "CHOP") {
    base = 30 + 3*sin(i/5.0); d = 0; vol = 600000 + 40000*sin(i)
  } else if (mode == "SPY") {
    base = 400 + 0.10*i + 3*sin(i/15.0); d = 1; vol = 80000000 + 2000000*sin(i/10.0)
  }
  high = base + 0.6; low = base - 0.6
  if (d > 0)      { cl = high - 0.10; op = base - 0.20 }
  else if (d < 0) { cl = low + 0.10;  op = base + 0.20 }
  else            { cl = (high+low)/2; op = base - 0.05*sin(i) }
  if (op > high) high = op + 0.05
  if (op < low)  low  = op - 0.05
  if (vol < 0) vol = -vol
  printf "%s,%.2f,%.2f,%.2f,%.2f,%d\n", date, op, high, low, cl, int(vol)
}
