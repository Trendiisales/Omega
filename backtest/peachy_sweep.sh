#!/usr/bin/env bash
# Full single-lever sweep around the best NAS her-config. One lever varied per row.
# Reports n, ALL/H1/H2 PF + ALL net. Want: H1 & H2 both PF>1 (WF-robust).
set -u
BIN=./peachy_orb
F="${1:-/tmp/nas_5m_proxy.csv}"
# base config (index 1..22 after the file arg)
#      OR sess flat body volLB retr tpR cost LO maxStop closeBuf volRatio trendEMA minStop maxTrade BE trail recency dow pushWick atrMaxP
BASE=(15 1330 1600 0.7 2 0.4 2.0 1.0 1 1.0 0.3 1.0 0 0.0 1 0.0 0.0 0 127 0.40 0.0)
LABELS=(OR sess flat body volLB retr tpR cost LO maxStop closeBuf volRatio trendEMA minStop maxTrade BE trail recency dow pushWick atrMaxP)

run(){ # $1=idx(1-based into BASE) $2=value $3=rowlabel
  local a=("${BASE[@]}"); a[$(($1-1))]="$2"
  "$BIN" "$F" "${a[@]}" "$3" | awk -v L="$3" '
    $1=="ALL"{an=$3; ap=$5; anet=$7} $1=="H1"{h1=$5} $1=="H2"{h2=$5}
    END{gsub("PF=","",ap);gsub("PF=","",h1);gsub("PF=","",h2);gsub("net=","",anet);
        flag=(h1+0>1.0 && h2+0>1.0)?"  <== both>1":"";
        printf "  %-16s n=%-4s ALL=%-5s H1=%-5s H2=%-5s net=%-9s%s\n",L,an,ap,h1,h2,anet,flag}'
}
sweep(){ # $1=idx $2=label $3..=values
  local idx=$1 lab=$2; shift 2; echo "-- $lab (idx $idx) --"
  for v in "$@"; do run "$idx" "$v" "$lab=$v"; done
}

echo "### BASELINE ###"; run 1 15 "BASELINE"
echo "### ENTRY / SETUP LEVERS ###"
sweep 1  OR_min      5 15 30 60
sweep 2  sess_start  1300 1330 1400 1430
sweep 4  body        0.5 0.6 0.7 0.8
sweep 6  retr        0.3 0.4 0.5 0.6 0.75
sweep 10 maxStop     0.75 1.0 1.25 1.5 2.0
sweep 14 minStop     0.0 0.25 0.5 0.75
sweep 11 closeBuf    0.0 0.15 0.3 0.5
sweep 12 volRatio    1.0 1.2 1.3 1.5
sweep 13 trendEMA    0 20 50 100
sweep 20 pushWick    0.30 0.40 0.50 0.60
sweep 18 recency     0 1 2 3 6
sweep 19 dow         127 62 65 28 96   # all / Tue-Fri / Mon+Fri / Tue-Thu / Thu-Fri
echo "### EXIT / MANAGEMENT LEVERS ###"
sweep 3  flat        1500 1530 1600 1700 1900
sweep 7  tpR         1.0 1.5 2.0 2.5 3.0
sweep 16 BE_R        0.0 0.5 1.0 1.5
sweep 17 trailATR    0.0 1.0 1.5 2.0 3.0
sweep 21 atrMaxP     0 30 50 80
echo "### FREQUENCY ###"
sweep 15 maxTrade    1 2 3
echo "### COST STRESS (best cfg) ###"
sweep 8  cost        1.0 2.0 3.0
