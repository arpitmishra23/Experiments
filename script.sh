#!/usr/bin/env bash
set -euo pipefail

SSH_KEY=~/.ssh/id_rsa_vm_rdt
PQOS=/usr/local/bin/pqos

### --- USER INPUT --- ###
read -rp "Victim VM domain name (virsh): " VM1_DOM
read -rp "Victim VM IP (ssh root@IP): "     VM1_IP
read -rp "Adversary VM domain name (virsh): " VM2_DOM
read -rp "Adversary VM IP (ssh root@IP): "     VM2_IP

read -rp "Victim vCPU count [8]: "    N1; N1=${N1:-8}
read -rp "Adversary vCPU count [8]: " N2; N2=${N2:-8}
read -rp "Duration per phase (sec) [30]: " DUR; DUR=${DUR:-30}
read -rp "Host core start for victim [20]: "    C1; C1=${C1:-20}
read -rp "Host core start for adversary [28]: " C2; C2=${C2:-28}

R1_END=$((C1 + N1 - 1))
R2_END=$((C2 + N2 - 1))
V_RANGE="0-$((N1-1))"
A_RANGE="0-$((N2-1))"

echo "[HOST] Resetting RDT state…"
sudo "$PQOS" -R

echo "[HOST] Pinning $VM1_DOM vCPUs → cores ${C1}-${R1_END}"
for v in $(seq 0 $((N1-1))); do
  sudo virsh vcpupin "$VM1_DOM" "$v" "${C1}-${R1_END}"
done

echo "[HOST] Pinning $VM2_DOM vCPUs → cores ${C2}-${R2_END}"
for v in $(seq 0 $((N2-1))); do
  sudo virsh vcpupin "$VM2_DOM" "$v" "${C2}-${R2_END}"
done

echo "[HOST] Launching victim on $VM1_IP…"
ssh -i "$SSH_KEY" -o BatchMode=yes -o StrictHostKeyChecking=no root@"$VM1_IP" \
  "pkill victim 2>/dev/null || true; \
   gcc -std=gnu99 -O2 -pthread /root/victim.c -o /root/victim; \
   taskset -c $V_RANGE /root/victim $N1 & echo \$! > /root/victim.pid"

#sleep 2

echo "[HOST] Locating victim QEMU PID…"
VQEMU=$(pgrep -f "qemu-kvm.*${VM1_DOM}")
echo "[HOST] Victim QEMU PID = $VQEMU"

echo "[HOST] Phase 1 – victim alone for ${DUR}s"
sudo "$PQOS" -I -p all:"$VQEMU" -t "$DUR" --mon-file=victim_before.txt

echo "[HOST] Launching adversary on $VM2_IP…"
ssh -i "$SSH_KEY" -o BatchMode=yes -o StrictHostKeyChecking=no root@"$VM2_IP" \
  "pkill adversary 2>/dev/null || true; \
   gcc -std=gnu99 -O3 -march=native -fopenmp /root/adversary.c -o /root/adversary; \
   export OMP_NUM_THREADS=$N2; \
   taskset -c $A_RANGE /root/adversary $N2 & echo \$! > /root/adversary.pid"

#sleep 2

echo "[HOST] Phase 2 – victim + adversary for ${DUR}s"
sudo "$PQOS" -I -p all:"$VQEMU" -t "$DUR" --mon-file=victim_during.txt

echo "[HOST] Tearing down adversary…"
ssh -i "$SSH_KEY" -o BatchMode=yes -o StrictHostKeyChecking=no root@"$VM2_IP" \
  "kill \$(cat /root/adversary.pid) 2>/dev/null || true"

echo "[HOST] Phase 3 – victim alone post-adversary for ${DUR}s"
sudo "$PQOS" -I -p all:"$VQEMU" -t "$DUR" --mon-file=victim_after.txt

echo "[HOST] Cleaning up victim…"
ssh -i "$SSH_KEY" -o BatchMode=yes -o StrictHostKeyChecking=no root@"$VM1_IP" \
  "kill \$(cat /root/victim.pid) 2>/dev/null || true"

echo "[HOST] Aggregating JSON results…"
jq -Rn \
  --rawfile b victim_before.txt \
  --rawfile d victim_during.txt \
  --rawfile a victim_after.txt \
  '{before: $b, during: $d, after: $a}' \
  > results.json

echo
echo "[HOST] All done!"
echo "  • Raw logs:     victim_before.txt, victim_during.txt, victim_after.txt"
echo "  • Consolidated: results.json"
