#!/usr/bin/env bash
set -euo pipefail

# -------- user-tunable defaults --------
DUR=${DUR:-10}          # seconds to sample
K=${K:-1.0}             # weight for Harm_Score
OUTDIR="./noisy_results_$(date +%Y%m%d_%H%M%S)"
PQOS_BIN=${PQOS_BIN:-pqos}
# ---------------------------------------

LOGFILE=""  # will be set after sampling
mkdir -p "$OUTDIR"

########################
# 1) Get core→proc map #
########################
read -rp "Cores for process1 [default 24]: " P1C; P1C=${P1C:-24}
read -rp "Cores for process2 [default 25-32]: " P2C; P2C=${P2C:-25-32}

# You can extend easily: add more "procX:cores" chunks below.
MAP_STR="proc1:$P1C proc2:$P2C"

# Build union of cores for pqos command
get_cores () {
  local spec="$1"
  local out=""
  IFS=',' read -ra parts <<< "$spec"
  for seg in "${parts[@]}"; do
    if [[ "$seg" =~ ^([0-9]+)-([0-9]+)$ ]]; then
      for ((c=${BASH_REMATCH[1]}; c<=${BASH_REMATCH[2]}; c++)); do out+="${c},"; done
    else
      out+="${seg},"
    fi
  done
  echo "${out%,}"
}
ALL_CORES=$(get_cores "$P1C"),$(get_cores "$P2C")
ALL_CORES=${ALL_CORES%,}   # trim trailing comma

#############################
# 2) Sample with pqos -m    #
#############################
RAW="$OUTDIR/pqos.log"
echo "[*] Sampling pqos for $DUR s on cores: $ALL_CORES"
sudo "$PQOS_BIN" -m "all:${ALL_CORES}" -t "$DUR" > "$RAW"
LOGFILE="$RAW"

#########################################
# 3) Save map file for the awk stage    #
#########################################
MAPFILE="$OUTDIR/core_map.csv"
echo "$MAP_STR" | awk '{
  for(i=1;i<=NF;i++){ split($i,a,":"); print a[1]","a[2] }
}' > "$MAPFILE"

#########################################
# 4) Parse + score (per your algorithm) #
#########################################
OUT="$OUTDIR/proc_scores.csv"
VERDICT="$OUTDIR/culprits.log"
: > "$VERDICT"

awk -v K="$K" -v MAP="$MAPFILE" -v VER="$VERDICT" '
BEGIN{
  OFS=",";
  print "time,process,IPC_sum,MISSES_sum,LLC_KB_sum,MBL_sum,MBR_sum," \
        "norm_bw,norm_llc_occ,norm_llc_miss,Occ_Score,Harm_Score,Noise_Score";

  # load core→proc map
  while ((getline line < MAP) > 0) {
    gsub(/^[ \t]+|[ \t]+$/, "", line);
    if(line=="" || line ~ /^#/) continue;
    split(line,a,/[ \t]*,[ \t]*/);
    proc=a[1]; corespec=a[2];
    n=split(corespec, parts, /,/);
    for(i=1;i<=n;i++){
      seg=parts[i];
      if(seg ~ /-/){
        split(seg,r,"-");
        for(c=r[1]; c<=r[2]; c++) core2proc[c]=proc;
      } else {
        core2proc[seg+0]=proc;
      }
    }
  }
}
# TIME line
$1=="TIME"{
  if(have_block){ flush_block(); reset_block(); }
  time=$2" "$3; next;
}
# data lines: leading spaces then core#
/^[[:space:]]*[0-9]+[[:space:]]+/{
  core_id = $1+0;
  ipc     = $2+0;
  mis     = $3;
  llc     = $4+0;
  mbl     = $5+0;
  mbr     = $6+0;

  gsub(/k$/,"000",mis); gsub(/m$/,"000000",mis);
  gsub(/K$/,"000",mis); gsub(/M$/,"000000",mis);
  mis+=0;

  p = (core_id in core2proc) ? core2proc[core_id] : "unmapped";

  core[idx]=core_id; PROC[idx]=p;
  IPC[idx]=ipc; MIS[idx]=mis; LLC[idx]=llc; MBL[idx]=mbl; MBR[idx]=mbr;
  idx++; have_block=1; next;
}
function reset_block(){
  delete core; delete PROC; delete IPC; delete MIS; delete LLC; delete MBL; delete MBR;
  delete P_IPC; delete P_MIS; delete P_LLC; delete P_MBL; delete P_MBR;
  idx=0;
}
function flush_block(){
  for(i=0;i<idx;i++){
    p=PROC[i];
    P_IPC[p]+=IPC[i];
    P_MIS[p]+=MIS[i];
    P_LLC[p]+=LLC[i];
    P_MBL[p]+=MBL[i];
    P_MBR[p]+=MBR[i];
  }
  sum_bw=sum_occ=sum_mis=0;
  for(p in P_IPC){
    bw=P_MBL[p]+P_MBR[p];
    sum_bw+=bw; sum_occ+=P_LLC[p]; sum_mis+=P_MIS[p];
  }
  if(sum_bw==0)  sum_bw=1;
  if(sum_occ==0) sum_occ=1;
  if(sum_mis==0) sum_mis=1;

  maxN=-1; culprit="";
  for(p in P_IPC){
    bw    = P_MBL[p]+P_MBR[p];
    nbw   = bw       / sum_bw;
    nocc  = P_LLC[p] / sum_occ;
    nharm = P_MIS[p] / sum_mis;

    occ   = nbw + nocc;
    harm  = nharm;
    noise = occ + K*harm;

    printf "%s,%s,%.2f,%d,%.1f,%.1f,%.1f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n", \
           time,p,P_IPC[p],P_MIS[p],P_LLC[p],P_MBL[p],P_MBR[p], \
           nbw,nocc,nharm,occ,harm,noise;

    if(noise>maxN){ maxN=noise; culprit=p; }
  }
  printf "# %s NOISY_NEIGHBOUR=%s noise=%.4f\n", time, culprit, maxN >> VER;
}
END{
  if(have_block) flush_block();
}
' "$LOGFILE" > "$OUT"

echo "[*] Done."
echo "  Raw pqos log      : $RAW"
echo "  Scores per process: $OUT"
echo "  Verdicts per time : $VERDICT"
[[ -s "$VERDICT" ]] && { echo; cat "$VERDICT"; }
