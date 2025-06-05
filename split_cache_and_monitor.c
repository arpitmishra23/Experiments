/*
 * vm_cache_partition_monitor.c
 *
 * For two given VM UUIDs, this program:
 *  1) Reads the full‐LLC CBM mask from /sys/fs/resctrl/info/L3/cbm_mask.
 *  2) Splits that mask into two equal‐share masks.
 *  3) (Modified here) Builds a schemata that gives socket 0 → 0x0 (no ways),
 *       and socket 1 → half‐mask or its complement.
 *  4) Creates (or reuses) /sys/fs/resctrl/COS1 and COS2, writing these schemata.
 *  5) Finds each VM’s QEMU PID by grepping “ps -ef … -uuid <VM> … guest=<VM>” (fallback if needed).
 *  6) Appends each PID into COS1/tasks (VM1) and COS2/tasks (VM2).
 *  7) Phase 1: monitors each PID separately (two groups of one) for ‘duration’ seconds,
 *       saving to:
 *         • VM1_half_baseline.txt
 *         • VM2_half_baseline.txt
 *  8) Cleans up COS1/COS2, returns PIDs to default (full LLC), re‐initializes PQoS.
 *  9) Phase 2: monitors each PID separately for ‘duration’ seconds,
 *       saving to:
 *         • VM1_normal.txt
 *         • VM2_normal.txt
 *
 * Usage:
 *   sudo ./vm_cache_partition_monitor <VM1_UUID> <VM2_UUID> <duration_seconds>
 *
 * Example:
 *   sudo ./vm_cache_partition_monitor f87b9c5a-bf69-4c13-8bc8-6bbf618c59a4 \
 *                                      1d8887d6-1c96-4722-b08d-603acd26f953 30
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <glob.h>
#include <sys/stat.h>
#include <time.h>
#include <pqos.h>

#define CBM_MASK_PATH       "/sys/fs/resctrl/info/L3/cbm_mask"
#define COS1_DIR            "/sys/fs/resctrl/COS1"
#define COS2_DIR            "/sys/fs/resctrl/COS2"
#define COS1_SCHEMATA       "/sys/fs/resctrl/COS1/schemata"
#define COS2_SCHEMATA       "/sys/fs/resctrl/COS2/schemata"
#define COS1_TASKS          "/sys/fs/resctrl/COS1/tasks"
#define COS2_TASKS          "/sys/fs/resctrl/COS2/tasks"
#define DEFAULT_TASKS       "/sys/fs/resctrl/tasks"

static void die(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(EXIT_FAILURE);
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char *buf = NULL;
    size_t len = 0;
    ssize_t rd = getline(&buf, &len, f);
    fclose(f);
    if (rd < 0) {
        free(buf);
        return NULL;
    }
    if (buf[rd - 1] == '\n') buf[rd - 1] = '\0';
    return buf;
}

static int parse_hex_u64(const char *hexstr, uint64_t *out) {
    char *endp = NULL;
    errno = 0;
    uint64_t val = strtoull(hexstr, &endp, 0);
    if (errno != 0 || (endp && *endp != '\0')) return -1;
    *out = val;
    return 0;
}

/* We only care about socket IDs (0 or 1 in most dual‐socket boxes). */
static int detect_sockets(int **sockets_out, int *count_out) {
    glob_t gl;
    if (glob("/sys/devices/system/cpu/cpu*/topology/physical_package_id", 0, NULL, &gl) != 0)
        return -1;
    int *ids = calloc(gl.gl_pathc, sizeof(int));
    if (!ids) { globfree(&gl); return -1; }
    int n = 0;
    for (size_t i = 0; i < gl.gl_pathc; i++) {
        char *buf = read_file(gl.gl_pathv[i]);
        if (!buf) continue;
        int pid = atoi(buf);
        free(buf);
        int found = 0;
        for (int j = 0; j < n; j++) {
            if (ids[j] == pid) { found = 1; break; }
        }
        if (!found) ids[n++] = pid;
    }
    globfree(&gl);
    /* Sort the socket IDs (so 0 comes before 1). */
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (ids[j] < ids[i]) {
                int t = ids[i]; ids[i] = ids[j]; ids[j] = t;
            }
        }
    }
    *sockets_out = ids;
    *count_out = n;
    return 0;
}

/* Split a full‐mask (e.g. 0x7fff) into two equal halves: mask1 gets lower bits, mask2 gets upper. */
static void split_mask(uint64_t full_mask, uint64_t *mask1, uint64_t *mask2) {
    int total_bits = 0;
    for (int b = 0; b < 64; b++) {
        if ((full_mask >> b) & 1ULL) total_bits++;
    }
    int half = total_bits / 2;
    uint64_t m1 = 0;
    int picked = 0;
    for (int b = 0; b < 64 && picked < half; b++) {
        if ((full_mask >> b) & 1ULL) {
            m1 |= (1ULL << b);
            picked++;
        }
    }
    *mask1 = m1;
    *mask2 = full_mask & ~m1;
}

static int mkdir_if_missing(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return 0;
        errno = ENOTDIR;
        return -1;
    }
    if (mkdir(path, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int write_str_to_file(const char *path, const char *str) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    size_t wr = fwrite(str, 1, strlen(str), f);
    fclose(f);
    return (wr == strlen(str)) ? 0 : -1;
}

static int append_str_to_file(const char *path, const char *str) {
    FILE *f = fopen(path, "a");
    if (!f) return -1;
    size_t wr = fwrite(str, 1, strlen(str), f);
    fwrite("\n", 1, 1, f);
    fclose(f);
    return (wr == strlen(str)) ? 0 : -1;
}

static pid_t find_vm_pid(const char *vmname) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "ps -ef | grep \"[q]emu-kvm.*-uuid.*%s.*-name.*guest=%s\" | awk '{print $2; exit}'",
             vmname, vmname);
    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    char line[64];
    if (!fgets(line, sizeof(line), p)) {
        pclose(p);
        snprintf(cmd, sizeof(cmd),
                 "ps -ef | grep \"[q]emu-kvm.*%s\" | awk '{print $2; exit}'",
                 vmname);
        p = popen(cmd, "r");
        if (!p) return -1;
        if (!fgets(line, sizeof(line), p)) {
            pclose(p);
            return -1;
        }
    }
    pclose(p);
    return (pid_t)atoi(line);
}

static int wait_for_pid(pid_t pid, int timeout_sec) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d", pid);
    for (int i = 0; i < timeout_sec; i++) {
        struct stat st;
        if (stat(path, &st) == 0) return 0;
        sleep(1);
    }
    return -1;
}

/*
 * Monitor two PIDs as two single‐PID groups for 'duration' seconds,
 * writing VM1 to outfile1 and VM2 to outfile2.
 */
static void monitor_pids_pair(pid_t pid1, pid_t pid2,
                              int duration,
                              const char *outfile1,
                              const char *outfile2) {
    if (wait_for_pid(pid1, 5) < 0) die("PID %d not running", pid1);
    if (wait_for_pid(pid2, 5) < 0) die("PID %d not running", pid2);

    enum pqos_mon_event ev_mask =
          PQOS_MON_EVENT_L3_OCCUP
        | PQOS_PERF_EVENT_LLC_MISS
        | PQOS_PERF_EVENT_IPC
        | PQOS_MON_EVENT_LMEM_BW
        | PQOS_MON_EVENT_RMEM_BW;

    struct pqos_mon_data group0, group1;
    pid_t p1 = pid1, p2 = pid2;

    int ret = pqos_mon_start_pids(1, &p1, ev_mask, NULL, &group0);
    if (ret != PQOS_RETVAL_OK)
        die("pqos_mon_start_pids(PID %d) failed (ret=%d)", pid1, ret);

    ret = pqos_mon_start_pids(1, &p2, ev_mask, NULL, &group1);
    if (ret != PQOS_RETVAL_OK) {
        pqos_mon_stop(&group0);
        die("pqos_mon_start_pids(PID %d) failed (ret=%d)", pid2, ret);
    }

    struct pqos_mon_data *groups[2] = { &group0, &group1 };

    FILE *f1 = fopen(outfile1, "w");
    if (!f1) die("fopen %s: %s", outfile1, strerror(errno));
    FILE *f2 = fopen(outfile2, "w");
    if (!f2) die("fopen %s: %s", outfile2, strerror(errno));

    fprintf(f1,
      "TIME                PID     IPC      MISSES   LLC[KB]  MBL[MB/s]  MBR[MB/s]\n");
    fprintf(f2,
      "TIME                PID     IPC      MISSES   LLC[KB]  MBL[MB/s]  MBR[MB/s]\n");

    for (int s = 0; s < duration; s++) {
        sleep(1);
        char procpath[64];
        snprintf(procpath, sizeof(procpath), "/proc/%d", pid1);
        if (stat(procpath, &(struct stat){0}) != 0)
            die("PID %d disappeared", pid1);
        snprintf(procpath, sizeof(procpath), "/proc/%d", pid2);
        if (stat(procpath, &(struct stat){0}) != 0)
            die("PID %d disappeared", pid2);

        time_t t = time(NULL);
        char ts[32];
        strftime(ts, sizeof(ts), "%F %T", localtime(&t));

        if (pqos_mon_poll(groups, 2) != PQOS_RETVAL_OK)
            die("pqos_mon_poll failed");

        double   ipc1  = group0.values.ipc;
        uint64_t miss1 = group0.values.llc_misses_delta;
        uint64_t llc1  = group0.values.llc / 1024ULL;
        double   mbl1  = group0.values.mbm_local_delta  / 1e6;
        double   mbr1  = group0.values.mbm_remote_delta / 1e6;

        double   ipc2  = group1.values.ipc;
        uint64_t miss2 = group1.values.llc_misses_delta;
        uint64_t llc2  = group1.values.llc / 1024ULL;
        double   mbl2  = group1.values.mbm_local_delta  / 1e6;
        double   mbr2  = group1.values.mbm_remote_delta / 1e6;

        fprintf(f1,
          "TIME %s\n"
          "     %6d   %5.2f   %8" PRIu64 "k   %7" PRIu64 "    %6.2f     %6.2f\n",
          ts, pid1, ipc1, miss1/1024, llc1, mbl1, mbr1);

        fprintf(f2,
          "TIME %s\n"
          "     %6d   %5.2f   %8" PRIu64 "k   %7" PRIu64 "    %6.2f     %6.2f\n",
          ts, pid2, ipc2, miss2/1024, llc2, mbl2, mbr2);

        fflush(f1);
        fflush(f2);
    }

    pqos_mon_stop(&group0);
    pqos_mon_stop(&group1);

    fclose(f1);
    fclose(f2);
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <VM1_UUID> <VM2_UUID> <duration_seconds>\n", argv[0]);
        return EXIT_FAILURE;
    }
    const char *vm1 = argv[1];
    const char *vm2 = argv[2];
    int duration = atoi(argv[3]);
    if (duration <= 0) die("Invalid duration");

    /* 1) Read full‐LLC CBM mask */
    char *cbm_raw = read_file(CBM_MASK_PATH);
    if (!cbm_raw)
        die("Cannot read %s: %s", CBM_MASK_PATH, strerror(errno));

    uint64_t full_mask;
    if (strncasecmp(cbm_raw, "0x", 2) != 0) {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "0x%s", cbm_raw);
        if (parse_hex_u64(tmp, &full_mask) != 0)
            die("Invalid CBM \"%s\"", cbm_raw);
    } else {
        if (parse_hex_u64(cbm_raw, &full_mask) != 0)
            die("Invalid CBM \"%s\"", cbm_raw);
    }
    free(cbm_raw);
    printf("Full LLC CBM = 0x%016" PRIx64 "\n", full_mask);

    /* 2) Split mask into two halves */
    uint64_t mask1, mask2;
    split_mask(full_mask, &mask1, &mask2);
    char mask1_str[32], mask2_str[32];
    snprintf(mask1_str, sizeof(mask1_str), "0x%016" PRIx64, mask1);
    snprintf(mask2_str, sizeof(mask2_str), "0x%016" PRIx64, mask2);
    printf("Mask1 = %s, Mask2 = %s\n", mask1_str, mask2_str);

    /* 3) Detect sockets (0 and 1) */
    int *sockets = NULL, n_sockets = 0;
    if (detect_sockets(&sockets, &n_sockets) != 0 || n_sockets < 2)
        die("Failed to detect sockets or found fewer than 2 sockets");
    /* We assume socket IDs are “0” and “1” (two‐socket system). */
    printf("Detected sockets:");
    for (int i = 0; i < n_sockets; i++) {
        printf(" %d", sockets[i]);
    }
    printf("\n");

    /* 4) Create COS1/COS2 and write schemata so that:
     *    • socket  0 → 0x0  (no ways allowed)
     *    • socket  1 → mask1_str or mask2_str
     *
     *  i.e. VM1 only ever uses half of socket 1’s LLC, and none of socket 0’s.
     */
    if (mkdir_if_missing(COS1_DIR) != 0)
        die("mkdir %s: %s", COS1_DIR, strerror(errno));
    if (mkdir_if_missing(COS2_DIR) != 0)
        die("mkdir %s: %s", COS2_DIR, strerror(errno));

    /* We know sockets[] contains [0, 1] (after sorting). */
    int sock0 = sockets[0];  // must be 0
    int sock1 = sockets[1];  // must be 1

    char schem1[64], schem2[64];
    snprintf(schem1, sizeof(schem1),
             "L3:%d=0x0;L3:%d=%s",
             sock0,    /* socket 0 → zero mask */
             sock1,    /* socket 1 → half‐mask */
             mask1_str);
    snprintf(schem2, sizeof(schem2),
             "L3:%d=0x0;L3:%d=%s",
             sock0,    /* socket 0 → zero mask */
             sock1,    /* socket 1 → other half */
             mask2_str);

    if (write_str_to_file(COS1_SCHEMATA, schem1) != 0)
        die("Writing %s: %s", COS1_SCHEMATA, strerror(errno));
    if (write_str_to_file(COS2_SCHEMATA, schem2) != 0)
        die("Writing %s: %s", COS2_SCHEMATA, strerror(errno));

    printf("Wrote schemata:\n  COS1: %s\n  COS2: %s\n", schem1, schem2);
    free(sockets);

    /* 5) Find each VM’s QEMU PID */
    pid_t pid1 = find_vm_pid(vm1);
    if (pid1 <= 0) die("Cannot find QEMU PID for VM \"%s\"", vm1);
    pid_t pid2 = find_vm_pid(vm2);
    if (pid2 <= 0) die("Cannot find QEMU PID for VM \"%s\"", vm2);
    if (pid1 == pid2)
        die("VM1 and VM2 resolved to the same PID (%d). Provide two distinct VMs.", pid1);

    printf("VM1 \"%s\" → PID %d\n", vm1, pid1);
    printf("VM2 \"%s\" → PID %d\n", vm2, pid2);

    /* 6) Append each PID into COS1/tasks and COS2/tasks */
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", pid1);
        if (append_str_to_file(COS1_TASKS, buf) != 0)
            die("Appending %d to %s: %s", pid1, COS1_TASKS, strerror(errno));
        printf("Appended PID %d to %s\n", pid1, COS1_TASKS);

        snprintf(buf, sizeof(buf), "%d", pid2);
        if (append_str_to_file(COS2_TASKS, buf) != 0)
            die("Appending %d to %s: %s", pid2, COS2_TASKS, strerror(errno));
        printf("Appended PID %d to %s\n", pid2, COS2_TASKS);
    }

    /* 7) Initialize PQoS */
    struct pqos_config cfg = { .interface = PQOS_INTER_OS_RESCTRL_MON };
    if (pqos_init(&cfg) != PQOS_RETVAL_OK) die("pqos_init failed");
    if (pqos_mon_reset() != PQOS_RETVAL_OK) die("pqos_mon_reset failed");

    /* 8) Phase 1: monitor half‐cache (socket 1 only, half‐ways) for 'duration' seconds */
    printf("Phase 1: half‐cache (on socket 1) baseline (duration = %d s)\n", duration);
    pqos_mon_reset();
    monitor_pids_pair(pid1, pid2, duration,
                      "VM1_half_baseline.txt",
                      "VM2_half_baseline.txt");
    printf("Phase 1 complete: VM1_half_baseline.txt, VM2_half_baseline.txt\n");

    /* 9) Clean up COS1/COS2, return PIDs to default (full LLC), then re‐initialize PQoS */
    pqos_mon_reset();
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", pid1);
        if (append_str_to_file(DEFAULT_TASKS, buf) != 0)
            die("Appending %d to %s: %s", pid1, DEFAULT_TASKS, strerror(errno));
        printf("Returned PID %d to default group\n", pid1);

        snprintf(buf, sizeof(buf), "%d", pid2);
        if (append_str_to_file(DEFAULT_TASKS, buf) != 0)
            die("Appending %d to %s: %s", pid2, DEFAULT_TASKS, strerror(errno));
        printf("Returned PID %d to default group\n", pid2);
    }
    if (rmdir(COS1_DIR) != 0) die("rmdir %s: %s", COS1_DIR, strerror(errno));
    if (rmdir(COS2_DIR) != 0) die("rmdir %s: %s", COS2_DIR, strerror(errno));
    printf("Removed COS1 and COS2\n");

    pqos_fini();
    /* Re‐init PQoS for Phase 2 */
    if (pqos_init(&cfg) != PQOS_RETVAL_OK) die("pqos_init (phase 2) failed");
    if (pqos_mon_reset() != PQOS_RETVAL_OK) die("pqos_mon_reset (phase 2) failed");

    /* 10) Phase 2: monitor normal (full‐cache) contention for 'duration' seconds */
    printf("Phase 2: normal (duration = %d s)\n", duration);
    pqos_mon_reset();
    monitor_pids_pair(pid1, pid2, duration,
                      "VM1_normal.txt",
                      "VM2_normal.txt");
    printf("Phase 2 complete: VM1_normal.txt, VM2_normal.txt\n");

    pqos_fini();
    return EXIT_SUCCESS;
}
