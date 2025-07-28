#!/usr/bin/env bash
set -euo pipefail

# --------- defaults ----------
DUR=60          # duration in seconds
INT=1000        # interval between samples in milliseconds
OUTDIR=./contention_log
PERF=perf
# -----------------------------

usage() {
  echo "Usage: $0 [-d sec] [-i ms] [-o outdir]"
  exit 1
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -d) DUR="$2"; shift 2;;
    -i) INT="$2"; shift 2;;
    -o) OUTDIR="$2"; shift 2;;
    -h|--help) usage;;
    *) echo "Unknown arg: $1"; usage;;
  esac
done

mkdir -p "$OUTDIR"
RAW="$OUTDIR/imc_raw.csv"
FULL="$OUTDIR/imc_full.csv"

# Define events: read/write CAS counts, per-channel RPQ & WPQ occupancy, and DRAM clocks
EVENTS="uncore_imc/cas_count_read/,uncore_imc/cas_count_write/,uncore_imc/unc_m_rpq_occupancy_pch0/,uncore_imc/unc_m_rpq_occupancy_pch1/,uncore_imc/unc_m_wpq_occupancy_pch0/,uncore_imc/unc_m_wpq_occupancy_pch1/,uncore_imc_free_running/dclk/"

echo "[*] Collecting perf stats for $DUR seconds @ ${INT}ms interval..."
$PERF stat -a -I "$INT" -x, -e "$EVENTS" -- sleep "$DUR" 2> "$RAW"

# AWK script to fuse raw data and compute avg_rpq_occupancy and avg_wpq_occupancy
cat > "$OUTDIR/fuse.awk" << 'AWK'
BEGIN {
  FS = ","; OFS = ",";
  print "time_s,cas_rd,cas_wr,dclk,rpq_occ_pc0,rpq_occ_pc1,wpq_occ_pc0,wpq_occ_pc1,total_cas,avg_rpq_occupancy,avg_wpq_occupancy,time_enabled,time_running"
}
{
  t     = $1;
  val   = $2;
  ev    = $4;
  te    = $5;
  tr    = $6;

  if      (ev == "uncore_imc/cas_count_read/")          rd   = val;
  else if (ev == "uncore_imc/cas_count_write/")         wr   = val;
  else if (ev == "uncore_imc/unc_m_rpq_occupancy_pch0/")  rpq0 = val;
  else if (ev == "uncore_imc/unc_m_rpq_occupancy_pch1/")  rpq1 = val;
  else if (ev == "uncore_imc/unc_m_wpq_occupancy_pch0/")  wpq0 = val;
  else if (ev == "uncore_imc/unc_m_wpq_occupancy_pch1/")  wpq1 = val;
  else if (ev == "uncore_imc_free_running/dclk/")       {
    dclk     = val;
    time_en  = te;
    time_run = tr;
  }
}
$4 == "uncore_imc_free_running/dclk/" {
  total       = rd + wr;
  avg_rpq     = (dclk > 0) ? (rpq0 + rpq1) / dclk : 0;
  avg_wpq     = (dclk > 0) ? (wpq0 + wpq1) / dclk : 0;
  printf "% .3f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.6f,%.6f,%s,%s\n", \
         t, rd, wr, dclk, rpq0, rpq1, wpq0, wpq1, total, avg_rpq, avg_wpq, time_en, time_run;

  # reset for next interval
  rd=wr=dclk=rpq0=rpq1=wpq0=wpq1=0;
}
AWK

# Run AWK to produce the full CSV
awk -f "$OUTDIR/fuse.awk" "$RAW" > "$FULL"

echo "[*] Done. Outputs:"
echo "  Raw : $RAW"
echo "  Full: $FULL"
head -n 6 "$FULL"

