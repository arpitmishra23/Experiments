#!/usr/bin/env bash
set -euo pipefail

# Usage: sudo ./noisy_detector.sh [DURATION_SEC] [TOP_N] [K_WEIGHT]
DUR=${1:-10}
TOPN=${2:-5}
K=${3:-1.0}

echo "[*] 1) Sample LLC-loads & misses for ${DUR}s"
sudo perf record -a \
    -e LLC-loads:u,LLC-load-misses:u \
    -c1 -o /tmp/perf_llc.data -- sleep "$DUR"

echo "[*] 2) Dump events to /tmp/llc.events.txt"
sudo perf script -i /tmp/perf_llc.data -F pid,comm,event \
    > /tmp/llc.events.txt
rm -f /tmp/perf_llc.data

echo "[*] 3) Pick top ${TOPN} PIDs by LLC-load-misses"
mapfile -t TOP_PIDS < <(
  awk '$3~/^LLC-load-misses/ { print $1 }' /tmp/llc.events.txt \
    | sort | uniq -c \
    | sort -nr \
    | head -n "$TOPN" \
    | awk '{print $2}'
)
if [ ${#TOP_PIDS[@]} -eq 0 ]; then
  echo "  No misses found; exiting."
  exit 1
fi
echo "    → PIDs: ${TOP_PIDS[*]}"

declare -a PID_LISTS

for proc_name in "${TOP_PIDS[@]}"; do
  # Use pgrep to get PID(s) of process name, exact match (-x)
  # If multiple processes exist, all PIDs will be returned separated by newline
  pids=$(pgrep -x "$proc_name" || true)
  
  if [[ -z "$pids" ]]; then
    echo "No PID found for process: $proc_name"
    PID_LISTS+=("")
  else
    echo "Process: $proc_name, PID(s): $pids"
    # Join multiple PIDs into a single space-separated string
    pid_line=$(echo "$pids" | tr '\n' ' ')
    PID_LISTS+=("$pid_line")
  fi
done

# Now PID_LISTS[i] contains the PIDs for process TOP_PIDS[i]

# Example: print mapping
for i in "${!TOP_PIDS[@]}"; do
  echo "Process '${TOP_PIDS[i]}' has PID(s): '${PID_LISTS[i]}'"
done

# --------------------------------------------------
# 4) For each of our top commands, call pqos per-PID and aggregate
# --------------------------------------------------
AGG=/tmp/noisy_agg.csv
echo "proc,mis_sum,llc_kb_sum,bw_sum" > "$AGG"

for idx in "${!TOP_PIDS[@]}"; do
  proc="${TOP_PIDS[idx]}"
  pid_line="${PID_LISTS[idx]}"
  [[ -z "$pid_line" ]] && continue

  echo "[*] PQoS for $proc (PIDs: $pid_line) for ${DUR}s"

  # initialize sums
  mis_sum=0
  llc_kb_sum=0
  bw_sum=0

  for pid in $pid_line; do
    # inside your for pid in $pid_line loop…
LOG=$(mktemp)
sudo pqos -I -p "all:$pid" -t "$DUR" >"$LOG" 2>/dev/null || true

# tail the last block and parse cols 4 (MISSES), 5 (LLC[KB]), and 6+7 (BW)
read miss_val llc_val bw_val < <(
  tail -n +$(( $(grep -n '^TIME' "$LOG" | tail -1 | cut -d: -f1) + 1 )) "$LOG" \
    | awk '
      /^[[:space:]]*[0-9]/ {
        # strip suffix k/M on MISSES
        gsub(/k$/,"000",$4)
        gsub(/m$/,"000000",$4)
        sum_miss += $4+0

        sum_llc  += $5
        sum_bw   += ($6 + $7)
      }
      END { printf("%d %d %d\n", sum_miss, sum_llc, sum_bw) }
    '
)
rm -f "$LOG"

mis_sum=$(( mis_sum     + miss_val ))
llc_kb_sum=$(( llc_kb_sum  + llc_val ))
bw_sum=$(( bw_sum      + bw_val  ))

  done

  # 4.3) append this process's aggregated metrics
  printf "%s,%d,%d,%d\n" \
    "$proc" "$mis_sum" "$llc_kb_sum" "$bw_sum" \
    >> "$AGG"
done
    

# --------------------------------------------------
# 5) Compute Occ_Score, Harm_Score & Noise_Score
# --------------------------------------------------
echo
echo "[*] 5) Scoring with K=${K}"
awk -F, -v K="$K" '
NR>1 {
  p=$1; mis=$2; llc=$3; bw=$4
  MIS[p]=mis; LLC[p]=llc; BW[p]=bw
  sum_mis += mis; sum_llc += llc; sum_bw += bw
  procs[++n]=p
}
END {
  # avoid div/0
  sum_mis = sum_mis?sum_mis:1
  sum_llc = sum_llc?sum_llc:1
  sum_bw  = sum_bw? sum_bw:1

  print "process,Occ_Score,Harm_Score,Noise_Score"
  maxN=-1
  for(i=1;i<=n;i++){
    p=procs[i]
    occ  = (BW[p]/sum_bw) + (LLC[p]/sum_llc)
    harm = MIS[p]/sum_mis
    noise = occ + K*harm
    printf "%s,%.6f,%.6f,%.6f\n", p, occ, harm, noise
    if(noise>maxN){ maxN=noise; noisy=p }
  }
  print ""
  print " Noisiest neighbor:", noisy, "(score=" sprintf("%.6f",maxN) ")"
}
' "$AGG"
