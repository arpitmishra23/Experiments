#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <inttypes.h>
#include <time.h>
#include <string.h>
#include <pqos.h>
static void die(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(EXIT_FAILURE);
}
static void run_cmd(const char *fmt, ...) {
    char cmd[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    if (system(cmd) != 0)
        die("`%s` failed", cmd);
}
/**
 * Monitor the *cores* [start…start+nc-1] for 'duration' seconds,
 * dumping per-second stats exactly as `pqos --mon-core` would.
 */
static void monitor_cores(int start, int nc, int duration, const char *outfile) {
    /* event mask: LLC occupancy, LLC misses, IPC, local & remote MB */
    enum pqos_mon_event ev = PQOS_MON_EVENT_L3_OCCUP
                          | PQOS_PERF_EVENT_LLC_MISS
                          | PQOS_PERF_EVENT_IPC
                          | PQOS_MON_EVENT_LMEM_BW
                          | PQOS_MON_EVENT_RMEM_BW;
    unsigned cores[nc];
    for (int i = 0; i < nc; i++) cores[i] = start + i;
    struct pqos_mon_data *group = NULL;
    if (pqos_mon_start_cores(nc, cores, ev, NULL, &group) != PQOS_RETVAL_OK)
        die("pqos_mon_start_cores");
    FILE *f = fopen(outfile, "w");
    if (!f) die("fopen(%s)", outfile);
    fprintf(f,
      "TIME                CORE    IPC      MISSES   LLC[KB]  MBL[MB/s]  MBR[MB/s]\n");
    for (int s = 0; s < duration; s++) {
        sleep(1);
        time_t t = time(NULL);
        char ts[32];
        strftime(ts, sizeof(ts), "%F %T", localtime(&t));
        if (pqos_mon_poll(&group, 1) != PQOS_RETVAL_OK)
            die("pqos_mon_poll");
        double   ipc  = group->values.ipc;
        uint64_t miss = group->values.llc_misses_delta;
        uint64_t llc  = group->values.llc / 1024ULL;            // KB
        double   mbl  = group->values.mbm_local_delta  / 1e6;   // MB/s
        double   mbr  = group->values.mbm_remote_delta / 1e6;
        fprintf(f,
          "TIME %s\n"
          "     %2d-%2d   %5.2f   %8" PRIu64 "k   %7" PRIu64 "    %6.2f     %6.2f\n",
          ts, start, start+nc-1, ipc, miss/1024, llc, mbl, mbr);
        fflush(f);
    }
    if (pqos_mon_stop(group) != PQOS_RETVAL_OK)
        die("pqos_mon_stop");
    fclose(f);
}
int main(void) {
    char vm_dom[128], vm_ip[64];
    char adv_dom[128], adv_ip[64];
    int N=8, DUR=30, CSTART=20, ASTART=28;
    // Victim VM details
    printf("Victim VM domain name (virsh): ");    scanf("%127s", vm_dom);
    printf("Victim VM IP: ");                    scanf("%63s", vm_ip);
    printf("vCPU count [8]: ");           if(scanf("%d",&N)!=1) N=8;
    printf("Duration [30]: ");            if(scanf("%d",&DUR)!=1) DUR=30;
    printf("Host core start (victim) [20]: "); if(scanf("%d",&CSTART)!=1) CSTART=20;
    // Adversary VM details
    printf("Adversary VM domain name (virsh): "); scanf("%127s", adv_dom);
    printf("Adversary VM IP: ");                 scanf("%63s", adv_ip);
    printf("Host core start (adversary) [28]: "); if(scanf("%d",&ASTART)!=1) ASTART=28;
    /* 1) initialize resctrl (OS) interface */
    struct pqos_config cfg = { .interface = PQOS_INTER_OS_RESCTRL_MON };
    if (pqos_init(&cfg) != PQOS_RETVAL_OK)      die("pqos_init");
    if (pqos_mon_reset() != PQOS_RETVAL_OK)     die("pqos_mon_reset");
    /* 2) pin the victim VM’s vCPUs to your cores */
    int CR = CSTART + N - 1;
    for (int v = 0; v < N; v++)
      run_cmd("virsh vcpupin %s %d %d-%d", vm_dom, v, CSTART, CR);
    /* 2.1) pin the adversary VM’s vCPUs to its cores */
    int ACR = ASTART + N - 1;
    for (int v = 0; v < N; v++)
      run_cmd("virsh vcpupin %s %d %d-%d", adv_dom, v, ASTART, ACR);
    /* 3) launch your victim inside the guest (over ssh) */
    printf("[HOST] Launching victim…\n");
    run_cmd("ssh -i ~/.ssh/id_rsa_vm_rdt -oBatchMode=yes root@%s "
            "\"pkill victim||true; "
             "gcc -std=gnu99 -O2 -pthread /root/victim.c -o /root/victim; "
             "nohup taskset -c 0-%d /root/victim %d &>/root/victim.log & "
             "echo $!>/root/victim.pid\"",
            vm_ip, N-1, N);
    sleep(2);
    /* 4) Phase 1: monitor just those cores */
    printf("[HOST] Phase 1 – victim alone\n");
    pqos_mon_reset();
    monitor_cores(CSTART, N, DUR, "victim_before.txt");
    /* 5) launch the adversary inside its guest */
    printf("[HOST] Launching adversary…\n");
    run_cmd("ssh -i ~/.ssh/id_rsa_vm_rdt -oBatchMode=yes root@%s "
            "\"pkill adversary||true; "
             "gcc -std=gnu99 -O3 -fopenmp /root/adversary.c -o /root/adversary; "
             "export OMP_NUM_THREADS=%d; "
             "nohup taskset -c 0-%d /root/adversary %d &>/root/adversary.log & "
             "echo $!>/root/adversary.pid\"",
            adv_ip, N, N-1, N);
    sleep(2);
    /* 6) Phase 2: both running */
    printf("[HOST] Phase 2 – victim + adversary\n");
    pqos_mon_reset();
    monitor_cores(CSTART, N, DUR, "victim_during.txt");
    /* 7) tear down adversary */
// ── Robust adversary teardown ──
printf("[HOST] Tearing down adversary (force kill)...\n");
{
    char cmd[512];

    // 1) Kill by PID file (SIGKILL), ignore any errors
    snprintf(cmd, sizeof(cmd),
        "ssh -i ~/.ssh/id_rsa_vm_rdt -oBatchMode=yes root@%s "
        "\"kill -9 $(cat /root/adversary.pid 2>/dev/null) 2>/dev/null;\"",
        adv_ip);
    system(cmd);  // we don't care if it fails

    // 2) Kill any leftover by name
    snprintf(cmd, sizeof(cmd),
        "ssh -i ~/.ssh/id_rsa_vm_rdt -oBatchMode=yes root@%s "
        "\"pkill -9 -f adversary 2>/dev/null;\"",
        adv_ip);
    system(cmd);

    // 3) Remove stale PID file
    snprintf(cmd, sizeof(cmd),
        "ssh -i ~/.ssh/id_rsa_vm_rdt -oBatchMode=yes root@%s "
        "\"rm -f /root/adversary.pid 2>/dev/null;\"",
        adv_ip);
    system(cmd);

    // 4) Sanity-check
    snprintf(cmd, sizeof(cmd),
        "ssh -i ~/.ssh/id_rsa_vm_rdt -oBatchMode=yes root@%s "
        "\"pgrep -f adversary > /dev/null && "
         "echo '[WARN] Adversary STILL RUNNING!' || "
         "echo '[OK] Adversary fully terminated.'\"",
        adv_ip);
    system(cmd);
}



    /* 8) Phase 3: victim alone again */
    printf("[HOST] Phase 3 – victim alone again\n");
    pqos_mon_reset();
    monitor_cores(CSTART, N, DUR, "victim_after.txt");
    /* 9) kill victim */
    run_cmd("ssh -i ~/.ssh/id_rsa_vm_rdt -oBatchMode=yes root@%s "
            "'kill $(cat /root/victim.pid) 2>/dev/null||true'",
            vm_ip);
    printf("\n[HOST] Done!\n"
           "  • Logs: victim_before.txt, victim_during.txt, victim_after.txt\n");
    pqos_fini();
    return 0;
}
