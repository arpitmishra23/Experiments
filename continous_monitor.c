/*
 * cos.c
 *
 *
 *  1) Read the full L3 CBM mask from /sys/fs/resctrl/info/L3/cbm_mask (e.g. "7fff").
 *  2) Split its set‐bits into two halves (lower half vs. upper half).
 *  3) Write two schemata files:
 *       /sys/fs/resctrl/COS1/schemata = 
 *         MB:0=100;1=100
 *         L2:0=ffff;...;83=ffff
 *         L3:0=<full_mask>;1=<lower_half>
 *
 *       /sys/fs/resctrl/COS2/schemata = 
 *         MB:0=100;1=100
 *         L2:0=ffff;...;83=ffff
 *         L3:0=<full_mask>;1=<upper_half>
 *
 *  4) Create the COS1 and COS2 directories if needed.
 *  5) Assign VM1’s QEMU PID → COS1/tasks, VM2’s PID → COS2/tasks.
 *  6) Monitor both PIDs for <duration> seconds (using the pqos API).
 *
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

/*-----------------------------------------
 * Print an error message and exit
 *-----------------------------------------*/
static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(EXIT_FAILURE);
}

/*-----------------------------------------
 * Read the first line of a file into a malloc’d, NUL‐terminated string.
 * Caller must free() the returned buffer. Returns NULL on failure.
 *-----------------------------------------*/
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

/*-----------------------------------------
 * Parse a hex string (no “0x” prefix) into uint64_t.
 * Returns 0 on success, -1 on error.
 *-----------------------------------------*/
static int parse_hex_no_prefix(const char *hexstr, uint64_t *out) {
    char *endp = NULL;
    errno = 0;
    uint64_t val = strtoull(hexstr, &endp, 16);
    if (errno != 0 || (endp && *endp != '\0')) return -1;
    *out = val;
    return 0;
}

/*-----------------------------------------
 * Count how many “1” bits are in a 64‐bit integer.
 *-----------------------------------------*/
static int count_bits(uint64_t x) {
    int cnt = 0;
    while (x) {
        x &= (x - 1);
        cnt++;
    }
    return cnt;
}

/*-----------------------------------------
 * Detect how many sockets (physical_package_id) exist.
 * We glob “/sys/devices/system/cpu/cpu*topology/physical_package_id”,
 * read each file, collect unique IDs, sort them, return count.
 * Returns 0 on success, -1 on failure.
 * On success, *sockets_out is malloc’d array (sorted), *count_out is number of sockets.
 * Caller must free(*sockets_out).
 *-----------------------------------------*/
static int detect_sockets(int **sockets_out, int *count_out) {
    glob_t gl;
    if (glob("/sys/devices/system/cpu/cpu*/topology/physical_package_id",
             0, NULL, &gl) != 0)
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
    /* Sort ascending */
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

/*-----------------------------------------
 * Split ‘full_mask’ into two halves:
 *   - mask1 = lowest floor(total_bits/2) set bits
 *   - mask2 = remaining set bits
 *-----------------------------------------*/
static void split_mask(uint64_t full_mask, uint64_t *mask1, uint64_t *mask2) {
    int total_bits = count_bits(full_mask);
    int half = (total_bits )/ 2;  /* floor */
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

/*-----------------------------------------
 * mkdir -p: create a directory if missing (0755).
 *-----------------------------------------*/
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

/*-----------------------------------------
 * Write a NUL‐terminated string into a file (overwrite). Returns 0 on success.
 *-----------------------------------------*/
static int write_str_to_file(const char *path, const char *str) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    size_t wr = fwrite(str, 1, strlen(str), f);
    fclose(f);
    return (wr == strlen(str)) ? 0 : -1;
}

/*-----------------------------------------
 * Append a NUL‐terminated string + "\n" to a file. Returns 0 on success.
 *-----------------------------------------*/
static int append_str_to_file(const char *path, const char *str) {
    FILE *f = fopen(path, "a");
    if (!f) return -1;
    size_t wr = fwrite(str, 1, strlen(str), f);
    fwrite("\n", 1, 1, f);
    fclose(f);
    return (wr == strlen(str)) ? 0 : -1;
}

/*-----------------------------------------
 * Find the QEMU‐KVM PID for a given VM UUID/name.
 * Returns >0 on success, -1 on failure.
 *-----------------------------------------*/
static pid_t find_vm_pid(const char *vmname) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "ps -ef | grep \"[q]emu-kvm.*-uuid.*%s.*-name.*guest=%s\" | "
             "awk '{print $2; exit}'",
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

/*-----------------------------------------
 * Wait up to timeout_sec for /proc/<pid> to exist.
 * Returns 0 if pid appears, -1 otherwise.
 *-----------------------------------------*/
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

/*-----------------------------------------
 * Monitor two PIDs for ‘duration’ seconds, dump stats to two files.
 *-----------------------------------------*/
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

/*-----------------------------------------
 * main()
 *-----------------------------------------*/
int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <VM1_UUID> <VM2_UUID> <duration_seconds>\n", argv[0]);
        return EXIT_FAILURE;
    }
    const char *vm1 = argv[1];
    const char *vm2 = argv[2];
    int duration = atoi(argv[3]);
    if (duration <= 0) die("Invalid duration");

    /* 1) Read the full CBM (e.g. "7fff") from sysfs. */
    char *cbm_raw = read_file(CBM_MASK_PATH);
    if (!cbm_raw)
        die("Cannot read %s: %s", CBM_MASK_PATH, strerror(errno));

    /* If it has a "0x" prefix, skip it. */
    char *hex_no_prefix = cbm_raw;
    if ((cbm_raw[0] == '0' && (cbm_raw[1] == 'x' || cbm_raw[1] == 'X'))) {
        hex_no_prefix = cbm_raw + 2;
    }
    size_t width = strlen(hex_no_prefix);
    if (width == 0 || width > 16) die("Unexpected CBM \"%s\"", hex_no_prefix);

    /* Parse that pure‐hex string into a uint64_t. */
    uint64_t full_mask;
    if (parse_hex_no_prefix(hex_no_prefix, &full_mask) != 0)
        die("Invalid CBM \"%s\"", hex_no_prefix);

    printf("Full LLC CBM = 0x%0*" PRIx64 "\n", (int)width, full_mask);

    int total_ways = count_bits(full_mask);
    if (total_ways == 0) die("Full mask has zero bits set—unexpected");

    /* 2) Split those set bits into two halves (lower/upper). */
    uint64_t mask_lower, mask_upper;
    split_mask(full_mask, &mask_lower, &mask_upper);

    /* 3) Convert each half back into zero‐padded hex of the same width. */
    char plain_lower[32], plain_upper[32];
    snprintf(plain_lower,  sizeof(plain_lower),  "%0*" PRIx64, (int)width, mask_lower);
    snprintf(plain_upper,  sizeof(plain_upper),  "%0*" PRIx64, (int)width, mask_upper);

    printf(" total ways   = %d\n", total_ways);
    printf(" socket1 lower = 0x%s\n", plain_lower);
    printf(" socket1 upper = 0x%s\n", plain_upper);

    /* 4) Detect sockets (physical_package_id). */
    int *sockets = NULL, n_sockets = 0;
    if (detect_sockets(&sockets, &n_sockets) != 0 || n_sockets < 2)
        die("Failed to detect sockets or found fewer than 2 sockets");
    printf("Detected sockets:");
    for (int i = 0; i < n_sockets; i++) {
        printf(" %d", sockets[i]);
    }
    printf("\n");

    int sock0 = sockets[0];   /* usually 0 */
    int sock1 = sockets[1];   /* usually 1 */
    free(sockets);

    /* 5) Create /sys/fs/resctrl/COS1 and COS2 if they don’t exist. */
    if (mkdir_if_missing(COS1_DIR) != 0)
        die("mkdir %s: %s", COS1_DIR, strerror(errno));
    if (mkdir_if_missing(COS2_DIR) != 0)
        die("mkdir %s: %s", COS2_DIR, strerror(errno));
     
    /* 6) Initialize PQoS via OS‐Resctrl (control + monitor). */
    struct pqos_config cfg = { .interface = PQOS_INTER_OS };
    if (pqos_init(&cfg) != PQOS_RETVAL_OK)
        die("pqos_init failed");
    if (pqos_mon_reset() != PQOS_RETVAL_OK)
        die("pqos_mon_reset failed");
    /* Do NOT call pqos_mon_reset() again after writing schemata. */

    

    /* 7) Build exactly one “L3:” line with semicolon-separated pairs. */
    char cos1_schema[1024];
    snprintf(cos1_schema, sizeof(cos1_schema),
             "MB:0=80;1=80\n"
             "L2:0=ffff;1=ffff;2=ffff;3=ffff;4=ffff;5=ffff;6=ffff;7=ffff;"
             "8=ffff;9=ffff;10=ffff;11=ffff;12=ffff;13=ffff;14=ffff;15=ffff;"
             "16=ffff;17=ffff;18=ffff;19=ffff;64=ffff;65=ffff;66=ffff;67=ffff;"
             "68=ffff;69=ffff;70=ffff;71=ffff;72=ffff;73=ffff;74=ffff;75=ffff;"
             "76=ffff;77=ffff;78=ffff;79=ffff;80=ffff;81=ffff;82=ffff;83=ffff\n"
             "L3:%d=%s;%d=%s\n",
             sock0, plain_lower,
             sock1, plain_lower);

    char cos2_schema[1024];
    snprintf(cos2_schema, sizeof(cos2_schema),
             "MB:0=20;1=20\n"
             "L2:0=ffff;1=ffff;2=ffff;3=ffff;4=ffff;5=ffff;6=ffff;7=ffff;"
             "8=ffff;9=ffff;10=ffff;11=ffff;12=ffff;13=ffff;14=ffff;15=ffff;"
             "16=ffff;17=ffff;18=ffff;19=ffff;64=ffff;65=ffff;66=ffff;67=ffff;"
             "68=ffff;69=ffff;70=ffff;71=ffff;72=ffff;73=ffff;74=ffff;75=ffff;"
             "76=ffff;77=ffff;78=ffff;79=ffff;80=ffff;81=ffff;82=ffff;83=ffff\n"
             "L3:%d=%s;%d=%s\n",
             sock0, plain_upper,
             sock1, plain_upper);

    /* 8) Write those exact strings into COS1_SCHEMATA and COS2_SCHEMATA. */
    if (write_str_to_file(COS1_SCHEMATA, cos1_schema) != 0)
        die("Writing %s: %s", COS1_SCHEMATA, strerror(errno));
    if (write_str_to_file(COS2_SCHEMATA, cos2_schema) != 0)
        die("Writing %s: %s", COS2_SCHEMATA, strerror(errno));

    printf("Wrote COS1 schemata:\n%s\n", cos1_schema);
    printf("Wrote COS2 schemata:\n%s\n", cos2_schema);

    /* 9) Find each VM’s QEMU PID and append to COS1/tasks & COS2/tasks. */
    pid_t pid1 = find_vm_pid(vm1);
    if (pid1 <= 0) die("Cannot find QEMU PID for VM \"%s\"", vm1);
    pid_t pid2 = find_vm_pid(vm2);
    if (pid2 <= 0) die("Cannot find QEMU PID for VM \"%s\"", vm2);
    if (pid1 == pid2)
        die("VM1 and VM2 resolved to the same PID (%d). Provide two distinct VMs.", pid1);

    printf("VM1 \"%s\" → PID %d\n", vm1, pid1);
    printf("VM2 \"%s\" → PID %d\n", vm2, pid2);

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

    /* 10) Monitor both for ‘duration’ seconds (optional). */
    printf("Phase 1: half‐cache monitoring (duration = %d s)\n", duration);
    /* We do NOT call pqos_mon_reset() again or it would revert to default masks. */
    monitor_pids_pair(pid1, pid2, duration,
                      "VM1_half_baseline.txt",
                      "VM2_half_baseline.txt");
    printf("Phase 1 complete: VM1_half_baseline.txt, VM2_half_baseline.txt\n");

    /*
     * If you ever want to restore the default CAT masks, you can run:
     *
     *   pqos_mon_reset();
     *   echo "" > /sys/fs/resctrl/COS1/tasks
     *   echo "" > /sys/fs/resctrl/COS2/tasks
     *   rmdir /sys/fs/resctrl/COS1
     *   rmdir /sys/fs/resctrl/COS2
     *   pqos_fini();
     *
     * Until you do that, “cat /sys/fs/resctrl/COS1/schemata” and
     * “cat /sys/fs/resctrl/COS2/schemata” will continue showing your custom masks.
     */

        /* -------------------------------------------------------------------------------------------- */
    /*                            CLEANUP & PHASE 2                         */
    /*  We do not return tasks to default or remove COS1/COS2 here, so our masks remain.  */       
    pqos_mon_reset();
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", pid1);
        if (append_str_to_file(COS1_TASKS, buf) != 0)
            die("Appending %d to %s: %s", pid1, COS1_TASKS, strerror(errno));
        printf("Returned PID %d to default group\n", pid1);

        snprintf(buf, sizeof(buf), "%d", pid2);
        if (append_str_to_file(COS2_TASKS, buf) != 0)
            die("Appending %d to %s: %s", pid2, COS2_TASKS, strerror(errno));
        printf("Returned PID %d to default group\n", pid2);
    }
    if (rmdir(COS1_DIR) != 0) die("rmdir %s: %s", COS1_DIR, strerror(errno));
    if (rmdir(COS2_DIR) != 0) die("rmdir %s: %s", COS2_DIR, strerror(errno));
    printf("Removed COS1 and COS2\n");

    pqos_fini();
    if (pqos_init(&cfg) != PQOS_RETVAL_OK) die("pqos_init (phase 2) failed");
    if (pqos_mon_reset() != PQOS_RETVAL_OK) die("pqos_mon_reset (phase 2) failed");

    printf("Phase 2: normal (duration = %d s)\n", duration);
    monitor_pids_pair(pid1, pid2, duration,
                      "VM1_normal.txt",
                      "VM2_normal.txt");
    printf("Phase 2 complete: VM1_normal.txt, VM2_normal.txt\n");
    pqos_fini();
    
    /* -------------------------------------------------------------------------------------------- */

    /* Exit now. COS1/COS2 + their schemata remain exactly as we wrote them. */


    return EXIT_SUCCESS;
}


