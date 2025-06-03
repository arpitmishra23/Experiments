#!/usr/bin/env python3
"""
noisy_vm_detector.py

Combines “peer‐degradation” with a fair‐share LLC check, using ±5% thresholds.

Reads four “mon‐core” text files:
  • VM1_baseline.txt      – VM1 running alone
  • VM1_contention.txt    – VM1 running alongside VM2
  • VM2_baseline.txt      – VM2 running alone
  • VM2_contention.txt    – VM2 running alongside VM1

For each VM, computes the per-second averages of:
  – LLC[KB]
  – MISSES/sec
  – IPC

Then:
  1) Identify which VM’s *peer* degraded noticeably (LLC drop ≥5%, misses rise ≥5%, IPC drop ≥5%).
  2) Check that that “noisy” VM’s own avg LLC under contention > (total_LLC/2).

Usage:
  $ python3 noisy_vm_detector.py
  Total LLC size (KB): ...
  VM1 UUID: ...
  VM2 UUID: ...
"""
import sys
import re

def parse_three_metrics(filename):
    """
    Parse a “mon-core” text file and return three lists:
      • misses_list (floats, MISSES/sec)
      • llc_list    (floats, LLC[KB])
      • ipc_list    (floats, IPC)

    Each data line looks like:
      TIME 2025-06-02 11:12:29
           20-27    0.27      53k      9520     2.21       0.00

    We skip lines starting with “TIME,” then for each core‐range line:
      parts[1] = IPC,
      parts[2] = MISSES (e.g. “53k” → 53000),
      parts[3] = LLC[KB].
    """
    misses, llc, ipc = [], [], []
    try:
        with open(filename, 'r') as f:
            for line in f:
                s = line.strip()
                if not s or s.startswith("TIME"):
                    continue
                parts = s.split()
                # Expect at least [core-range, ipc, misses, llc, mbl, mbr]
                if len(parts) < 4:
                    continue
                if re.match(r'^\d+-\d+$', parts[0]):
                    # IPC
                    try:
                        ipc_val = float(parts[1])
                    except ValueError:
                        continue
                    # MISSES (e.g. "53k")
                    ms = parts[2].lower()
                    try:
                        if ms.endswith('k'):
                            miss_val = float(ms[:-1]) * 1000.0
                        else:
                            miss_val = float(ms)
                    except ValueError:
                        continue
                    # LLC[KB]
                    try:
                        llc_val = int(parts[3])
                    except ValueError:
                        continue

                    ipc.append(ipc_val)
                    misses.append(miss_val)
                    llc.append(llc_val)
    except FileNotFoundError:
        print(f"Error: cannot open {filename}", file=sys.stderr)
        sys.exit(1)
    except Exception as e:
        print(f"Error reading {filename}: {e}", file=sys.stderr)
        sys.exit(1)

    if not misses or not llc or not ipc:
        print(f"Warning: no valid data in {filename}", file=sys.stderr)
    return misses, llc, ipc

def avg(lst):
    return sum(lst)/len(lst) if lst else 0.0

def main():
    print()
    try:
        total_llc = float(input("Total LLC size (KB): ").strip())
    except ValueError:
        print("Invalid total LLC size.", file=sys.stderr)
        sys.exit(1)

    vm1_uuid = input("VM1 UUID: ").strip()
    vm2_uuid = input("VM2 UUID: ").strip()
    print()

    # Filenames in current directory
    f1_base = "VM1_baseline.txt"
    f1_cont = "VM1_contention.txt"
    f2_base = "VM2_baseline.txt"
    f2_cont = "VM2_contention.txt"

    # Parse each file
    m1b, l1b, i1b = parse_three_metrics(f1_base)
    m1c, l1c, i1c = parse_three_metrics(f1_cont)

    m2b, l2b, i2b = parse_three_metrics(f2_base)
    m2c, l2c, i2c = parse_three_metrics(f2_cont)

    # Compute averages
    vm1_base_llc = avg(l1b)
    vm1_cont_llc = avg(l1c)
    vm1_base_miss = avg(m1b)
    vm1_cont_miss = avg(m1c)
    vm1_base_ipc = avg(i1b)
    vm1_cont_ipc = avg(i1c)

    vm2_base_llc = avg(l2b)
    vm2_cont_llc = avg(l2c)
    vm2_base_miss = avg(m2b)
    vm2_cont_miss = avg(m2c)
    vm2_base_ipc = avg(i2b)
    vm2_cont_ipc = avg(i2c)

    # Compute “ratios” (contention vs. baseline)
    def safe_div(n, d):
        return (n/d) if (d > 0) else 1.0

    llc_ratio_1  = safe_div(vm1_cont_llc, vm1_base_llc)   # <1 if LLC fell
    miss_ratio_1 = safe_div(vm1_cont_miss, vm1_base_miss) # >1 if misses rose
    ipc_ratio_1  = safe_div(vm1_cont_ipc, vm1_base_ipc)   # <1 if IPC fell

    llc_ratio_2  = safe_div(vm2_cont_llc, vm2_base_llc)
    miss_ratio_2 = safe_div(vm2_cont_miss, vm2_base_miss)
    ipc_ratio_2  = safe_div(vm2_cont_ipc, vm2_base_ipc)

    # Print summary
    print(f"== VM1 ({vm1_uuid}) ==")
    print(f"  LLC  (base/cont)  = {vm1_base_llc:.1f} / {vm1_cont_llc:.1f} KB   → ratio {llc_ratio_1:.2f}")
    print(f"  MISS (base/cont)  = {vm1_base_miss:.1f} / {vm1_cont_miss:.1f}     → ratio {miss_ratio_1:.2f}")
    print(f"  IPC  (base/cont)  = {vm1_base_ipc:.2f} / {vm1_cont_ipc:.2f}     → ratio {ipc_ratio_1:.2f}\n")

    print(f"== VM2 ({vm2_uuid}) ==")
    print(f"  LLC  (base/cont)  = {vm2_base_llc:.1f} / {vm2_cont_llc:.1f} KB   → ratio {llc_ratio_2:.2f}")
    print(f"  MISS (base/cont)  = {vm2_base_miss:.1f} / {vm2_cont_miss:.1f}     → ratio {miss_ratio_2:.2f}")
    print(f"  IPC  (base/cont)  = {vm2_base_ipc:.2f} / {vm2_cont_ipc:.2f}     → ratio {ipc_ratio_2:.2f}\n")

    fair_share = total_llc / 2.0
    print(f"Fair share of LLC (each) = {fair_share:.1f} KB\n")

    # Use ±5% thresholds:
    LLC_DROP_THRESH   = 0.95  # “fell by ≥5%” → ratio ≤ 0.95
    MISS_RISE_THRESH  = 1.05  # “rose by ≥5%” → ratio ≥ 1.05
    IPC_DROP_THRESH   = 0.95  # “fell by ≥5%” → ratio ≤ 0.95
    FLAT_LOW, FLAT_HIGH = 0.95, 1.05  # “flat” within ±5%

    noisy_vm = None

    # Case A: VM1 degraded noticeably while VM2 stayed flat?
    victim1 = (
        llc_ratio_1  <= LLC_DROP_THRESH and
        miss_ratio_1 >= MISS_RISE_THRESH and
        ipc_ratio_1  <= IPC_DROP_THRESH
    )
    flat2 = (
        FLAT_LOW <= llc_ratio_2  <= FLAT_HIGH and
        FLAT_LOW <= miss_ratio_2 <= FLAT_HIGH and
        FLAT_LOW <= ipc_ratio_2  <= FLAT_HIGH
    )

    if victim1 and flat2:
        # VM2’s metrics are flat → VM2 is candidate noisy neighbor if it holds > fair_share
        if vm2_cont_llc > fair_share:
            noisy_vm = vm2_uuid

    # Case B: VM2 degraded noticeably while VM1 stayed flat?
    victim2 = (
        llc_ratio_2  <= LLC_DROP_THRESH and
        miss_ratio_2 >= MISS_RISE_THRESH and
        ipc_ratio_2  <= IPC_DROP_THRESH
    )
    flat1 = (
        FLAT_LOW <= llc_ratio_1  <= FLAT_HIGH and
        FLAT_LOW <= miss_ratio_1 <= FLAT_HIGH and
        FLAT_LOW <= ipc_ratio_1  <= FLAT_HIGH
    )

    if victim2 and flat1:
        if vm1_cont_llc > fair_share:
            noisy_vm = vm1_uuid

    if noisy_vm:
        print(f"Detected noisy VM: {noisy_vm}")
    else:
        print("No noisy VM detected under combined peer-degradation + fair-share checks.")

if __name__ == "__main__":
    main()
