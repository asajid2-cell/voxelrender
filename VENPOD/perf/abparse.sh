#!/usr/bin/env bash
# Parse a VENPOD runtime log's PERF frames into a raw-frame-time distribution.
# Usage: abparse.sh <log> [warmup_frames]
# PERF line: "PERF frame=N fps=sm/inst ms=sm/raw ..." -> raw = 2nd ms value.
LOG="$1"; WARM="${2:-120}"
grep -oE "PERF frame=[0-9]+ fps=[0-9.]+/[0-9.]+ ms=[0-9.]+/[0-9.]+" "$LOG" \
| sed -E 's/PERF frame=([0-9]+).*ms=[0-9.]+\/([0-9.]+)/\1 \2/' \
| awk -v warm="$WARM" '($1+0)>=warm{print $2+0}' \
| sort -n \
| awk '
{ v[n++]=$1 }
END {
  if (n==0){print "no frames"; exit}
  p50=v[int(0.50*n)]; p90=v[int(0.90*n)]; p95=v[int(0.95*n)];
  p99=v[int(0.99*n)]; p999=v[int(0.999*n)]; mx=v[n-1];
  for(i=0;i<n;i++){x=v[i];
    if(x>8.33)b833++; if(x>10)b10++; if(x>12.5)b1267++; if(x>16.67)b1667++;
    if(x>25)b25++; if(x>33)b33++; if(x>50)b50++}
  printf "n=%d p50=%.2f(%.0ffps) p90=%.2f p95=%.2f p99=%.2f p99.9=%.2f max=%.2f\n",
    n,p50,1000.0/p50,p90,p95,p99,p999,mx
  printf "   >8.33=%d >10=%d >12.5=%d >16.67=%d >25=%d >33=%d >50=%d\n",
    b833+0,b10+0,b1267+0,b1667+0,b25+0,b33+0,b50+0
}'
