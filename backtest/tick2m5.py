import sys
inf, outf = sys.argv[1], sys.argv[2]
W=300  # 5-min buckets (seconds)
with open(inf) as f, open(outf,'w') as o:
    o.write("ts,open,high,low,close\n")
    hdr=f.readline().lower().split(',')
    # detect ts (ms) col0; price = mid of next two cols
    cur=-1; O=H=L=C=0.0; n=0
    for ln in f:
        p=ln.split(',')
        if len(p)<3: continue
        try:
            tms=int(p[0]); a=float(p[1]); b=float(p[2])
        except: continue
        ts=tms//1000
        mid=(a+b)/2.0
        g=(ts//W)*W
        if g!=cur:
            if cur>=0: o.write(f"{cur},{O},{H},{L},{C}\n"); n+=1
            cur=g; O=H=L=C=mid
        else:
            if mid>H:H=mid
            if mid<L:L=mid
            C=mid
    if cur>=0: o.write(f"{cur},{O},{H},{L},{C}\n"); n+=1
print(f"{outf}: {n} m5 bars")
