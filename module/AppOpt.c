#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <time.h>
#include <unistd.h>

#define VERSION            "aki-0.1.0"
#define BASE_CPUSET        "/dev/cpuset/AkiAppOpt"
#define MAX_PKG_LEN        128
#define MAX_THREAD_LEN     32
#define DELAY_UNIT_MS      100ULL
#define DELAY_POLL_US      100000

typedef struct {
    char pkg[MAX_PKG_LEN];
    char thread[MAX_THREAD_LEN];
    char cpuset_dir[256];
    cpu_set_t cpus;
    uint64_t delay_ms;
} AffinityRule;

typedef struct {
    pid_t tid;
    char name[MAX_THREAD_LEN];
    char cpuset_dir[256];
    cpu_set_t cpus;
    uint64_t bind_after_ms;
} ThreadInfo;

typedef struct {
    pid_t pid;
    char pkg[MAX_PKG_LEN];
    char base_cpuset[128];
    cpu_set_t base_cpus;
    uint64_t detected_at_ms;
    uint64_t base_delay_ms;
    ThreadInfo* threads;
    size_t num_threads;
    size_t threads_cap;
    AffinityRule** thread_rules;
    size_t num_thread_rules;
    size_t thread_rules_cap;
} ProcessInfo;

typedef struct {
    pid_t pid;
    char pkg[MAX_PKG_LEN];
    uint64_t detected_at_ms;
} DetectedProcess;

typedef struct {
    cpu_set_t present_cpus;
    char present_str[128];
    char mems_str[32];
    bool cpuset_enabled;
    int base_cpuset_fd;
} CpuTopology;

typedef struct {
    atomic_int ref_count;
    AffinityRule* rules;
    size_t num_rules;
    time_t mtime;
    CpuTopology topo;
    char** pkgs;
    size_t num_pkgs;
    char config_file[4096];
} AppConfig;

typedef struct {
    ProcessInfo* procs;
    size_t num_procs;
    size_t procs_cap;
    int last_proc_count;
    bool scan_all_proc;
    pid_t* tracked_pids;
    size_t num_tracked_pids;
    size_t tracked_pids_cap;
    int last_proc_total;
} ProcCache;

static atomic_int config_updated = ATOMIC_VAR_INIT(0);
static int inotify_fd = -1;
static int inotify_wd = -1;
static int inotify_supported = 0;
static _Atomic(AppConfig*) current_config = NULL;

static char* strtrim(char* s) {
    char* end;
    while (isspace(*s)) s++;
    if (*s == 0) return s;
    end = s + strlen(s) - 1;
    while (end > s && isspace(*end)) end--;
    *(end + 1) = 0;
    return s;
}

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
    }
    return (uint64_t)time(NULL) * 1000ULL;
}

static uint64_t delay_deadline(uint64_t detected_at_ms, uint64_t delay_ms) {
    if (delay_ms > UINT64_MAX - detected_at_ms) return UINT64_MAX;
    return detected_at_ms + delay_ms;
}

static bool read_file(int dir_fd, const char* filename, char* buf, size_t buf_size) {
    int fd = openat(dir_fd, filename, O_RDONLY | O_CLOEXEC);
    if (fd == -1) return false;
    ssize_t n = read(fd, buf, buf_size - 1);
    close(fd);
    if (n <= 0) return false;
    buf[n] = '\0';
    return true;
}

static bool write_file(int dir_fd, const char* filename, const char* content, int flags) {
    int fd = openat(dir_fd, filename, flags | O_CLOEXEC, 0644);
    if (fd == -1) return false;
    ssize_t n = write(fd, content, strlen(content));
    close(fd);
    return (n == (ssize_t)strlen(content));
}

static int build_str(char *dest, size_t dest_size, ...) {
    va_list args;
    const char *segment;
    char *p = dest;
    size_t remaining = dest_size - 1;
    va_start(args, dest_size);
    while ((segment = va_arg(args, const char *)) != NULL) {
        size_t len = strlen(segment);
        if (len > remaining) {
            *p = '\0';
            va_end(args);
            return 0;
        }
        memcpy(p, segment, len);
        p += len;
        remaining -= len;
    }
    *p = '\0';
    va_end(args);
    return 1;
}

static bool parse_cpu_ranges(const char* spec, cpu_set_t* set, const cpu_set_t* present) {
    if (!spec) return false;
    const char* s = spec;
    bool parsed = false;

    while (true) {
        while (isspace((unsigned char)*s)) s++;
        if (!isdigit((unsigned char)*s)) return false;

        errno = 0;
        char* end;
        unsigned long a = strtoul(s, &end, 10);
        if (errno == ERANGE || end == s) return false;
        s = end;
        while (isspace((unsigned char)*s)) s++;

        unsigned long b = a;
        if (*s == '-') {
            s++;
            while (isspace((unsigned char)*s)) s++;
            if (!isdigit((unsigned char)*s)) return false;

            errno = 0;
            b = strtoul(s, &end, 10);
            if (errno == ERANGE || end == s) return false;
            s = end;
            while (isspace((unsigned char)*s)) s++;
        }

        if (a > b) { unsigned long t = a; a = b; b = t; }
        for (unsigned long i = a; i <= b && i < CPU_SETSIZE; i++) {
            if (present && !CPU_ISSET(i, present)) continue;
            CPU_SET(i, set);
        }
        parsed = true;

        if (!*s) break;
        if (*s != ',') return false;
        s++;
    }

    return parsed;
}

static char* cpu_set_to_str(const cpu_set_t *set) {
    size_t buf_size = 8 * CPU_SETSIZE;
    char *buf = malloc(buf_size);
    if (!buf) return NULL;
    int start = -1, end = -1;
    char *p = buf;
    size_t remain = buf_size - 1;
    bool first = true;

    for (int i = 0; i < CPU_SETSIZE; i++) {
        if (CPU_ISSET(i, set)) {
            if (start == -1) {
                start = end = i;
            } else if (i == end + 1) {
                end = i;
            } else {
                int needed;
                if (start == end) {
                    needed = snprintf(p, remain + 1, "%s%d", first ? "" : ",", start);
                } else {
                    needed = snprintf(p, remain + 1, "%s%d-%d", first ? "" : ",", start, end);
                }
                if (needed < 0 || (size_t)needed > remain) {
                    free(buf);
                    return NULL;
                }
                p += needed;
                remain -= needed;
                start = end = i;
                first = false;
            }
        }
    }
    if (start != -1) {
        int needed;
        if (start == end) {
            needed = snprintf(p, remain + 1, "%s%d", first ? "" : ",", start);
        } else {
            needed = snprintf(p, remain + 1, "%s%d-%d", first ? "" : ",", start, end);
        }
        if (needed < 0 || (size_t)needed > remain) {
            free(buf);
            return NULL;
        }
        p += needed;
    }
    *p = '\0';
    return buf;
}

static bool create_cpuset_dir(const char *path, const char *cpus, const char *mems) {
    if (mkdir(path, 0755) != 0 && errno != EEXIST) return false;
    if (chmod(path, 0755) != 0) return false;
    if (chown(path, 0, 0) != 0) return false;

    char cpus_path[256];
    build_str(cpus_path, sizeof(cpus_path), path, "/cpus", NULL);
    if (!write_file(AT_FDCWD, cpus_path, cpus, O_WRONLY | O_CREAT | O_TRUNC)) return false;

    char mems_path[256];
    build_str(mems_path, sizeof(mems_path), path, "/mems", NULL);
    return write_file(AT_FDCWD, mems_path, mems, O_WRONLY | O_CREAT | O_TRUNC);
}

static CpuTopology init_cpu_topo(void) {
    CpuTopology topo = { .cpuset_enabled = false, .base_cpuset_fd = -1 };
    CPU_ZERO(&topo.present_cpus);

    if (read_file(AT_FDCWD, "/sys/devices/system/cpu/present", topo.present_str, sizeof(topo.present_str))) {
        strtrim(topo.present_str);
    }
    parse_cpu_ranges(topo.present_str, &topo.present_cpus, NULL);

    if (access("/dev/cpuset", F_OK) != 0) return topo;

    if (create_cpuset_dir(BASE_CPUSET, topo.present_str, "0")) {
        topo.base_cpuset_fd = open(BASE_CPUSET, O_RDONLY | O_DIRECTORY);
        if (topo.base_cpuset_fd != -1) topo.cpuset_enabled = true;
    }

    char mems_path[256];
    build_str(mems_path, sizeof(mems_path), BASE_CPUSET, "/mems", NULL);
    if (!read_file(AT_FDCWD, mems_path, topo.mems_str, sizeof(topo.mems_str))) {
        build_str(topo.mems_str, sizeof(topo.mems_str), "0", NULL);
    } else {
        strtrim(topo.mems_str);
    }

    return topo;
}

typedef enum {
    RULE_ADD_ADDED,
    RULE_ADD_DUPLICATE,
    RULE_ADD_INVALID,
    RULE_ADD_SKIPPED,
    RULE_ADD_NO_MEMORY
} RuleAddResult;

static RuleAddResult add_rule(AffinityRule** rules, size_t* rules_cnt,
                              const CpuTopology* topo, const char* pkg,
                              const char* thread, const char* cpus_spec,
                              uint64_t delay_ms) {
    if (strlen(pkg) >= MAX_PKG_LEN || strlen(thread) >= MAX_THREAD_LEN) {
        return RULE_ADD_INVALID;
    }

    for (size_t i = 0; i < *rules_cnt; i++) {
        if (strcmp((*rules)[i].pkg, pkg) == 0 &&
            strcmp((*rules)[i].thread, thread) == 0) {
            return RULE_ADD_DUPLICATE;
        }
    }

    cpu_set_t set;
    CPU_ZERO(&set);
    if (!parse_cpu_ranges(cpus_spec, &set, &topo->present_cpus) || CPU_COUNT(&set) == 0) {
        return RULE_ADD_INVALID;
    }

    char* dir_name = cpu_set_to_str(&set);
    if (!dir_name) return RULE_ADD_NO_MEMORY;

    AffinityRule rule = {0};
    char path[256];
    if (!build_str(path, sizeof(path), BASE_CPUSET, "/", dir_name, NULL) ||
        !build_str(rule.pkg, sizeof(rule.pkg), pkg, NULL) ||
        !build_str(rule.thread, sizeof(rule.thread), thread, NULL) ||
        !build_str(rule.cpuset_dir, sizeof(rule.cpuset_dir), dir_name, NULL)) {
        free(dir_name);
        return RULE_ADD_INVALID;
    }

    if (!create_cpuset_dir(path, dir_name, topo->mems_str)) {
        free(dir_name);
        return RULE_ADD_SKIPPED;
    }

    rule.cpus = set;
    rule.delay_ms = delay_ms;
    free(dir_name);

    AffinityRule* tmp = realloc(*rules, (*rules_cnt + 1) * sizeof(AffinityRule));
    if (!tmp) return RULE_ADD_NO_MEMORY;
    *rules = tmp;
    memcpy(&(*rules)[*rules_cnt], &rule, sizeof(AffinityRule));
    (*rules_cnt)++;
    return RULE_ADD_ADDED;
}

static bool build_pkg_list(const AffinityRule* rules, size_t rules_cnt,
                           char*** out_pkgs, size_t* out_cnt) {
    char** pkgs = NULL;
    size_t cnt = 0;

    for (size_t i = 0; i < rules_cnt; i++) {
        bool exists = false;
        for (size_t j = 0; j < cnt; j++) {
            if (strcmp(pkgs[j], rules[i].pkg) == 0) {
                exists = true;
                break;
            }
        }
        if (exists) continue;

        char** tmp = realloc(pkgs, (cnt + 1) * sizeof(char*));
        if (!tmp) goto error;
        pkgs = tmp;
        pkgs[cnt] = strdup(rules[i].pkg);
        if (!pkgs[cnt]) goto error;
        cnt++;
    }

    *out_pkgs = pkgs;
    *out_cnt = cnt;
    return true;

error:
    for (size_t i = 0; i < cnt; i++) free(pkgs[i]);
    free(pkgs);
    return false;
}

static AppConfig* load_config(const char* config_file, const CpuTopology* topo, time_t* last_mtime) {
    struct stat st;
    if (stat(config_file, &st)) return NULL;
    AppConfig* cfg = calloc(1, sizeof(AppConfig));
    if (!cfg) return NULL;
    cfg->ref_count = 1;
    cfg->topo = *topo;
    build_str(cfg->config_file, sizeof(cfg->config_file), config_file, NULL);

    if (last_mtime && *last_mtime == st.st_mtime && *last_mtime != -1) {
        free(cfg);
        return NULL;
    }

    FILE* fp = fopen(config_file, "r");
    if (!fp) {
        free(cfg);
        return NULL;
    }

    AffinityRule* new_rules = NULL;
    char** new_pkgs = NULL;
    size_t rules_cnt = 0, pkgs_cnt = 0, fail_cnt = 0;
    char line[256];

    while (fgets(line, sizeof(line), fp)) {
        char* p = strtrim(line);
        if (!*p || *p == '#' || (p[0] == '/' && p[1] == '/')) continue;

        char* eq = strchr(p, '=');
        if (!eq) {
            fail_cnt++;
            continue;
        }
        *eq++ = 0;

        char* br = strchr(p, '{');
        char* thread = "";
        uint64_t delay_ms = 0;
        if (br) {
            *br++ = 0;
            char* eb = strchr(br, '}');
            if (!eb) {
                fail_cnt++;
                continue;
            }
            *eb = 0;
            thread = strtrim(br);

            char* suffix = strtrim(eb + 1);
            if (*suffix) {
                if (*suffix != ':') {
                    fail_cnt++;
                    continue;
                }
                char* delay = strtrim(suffix + 1);
                if (!isdigit((unsigned char)*delay)) {
                    fail_cnt++;
                    continue;
                }

                errno = 0;
                char* delay_end;
                unsigned long long units = strtoull(delay, &delay_end, 10);
                delay_end = strtrim(delay_end);
                if (errno == ERANGE || *delay_end || units > UINT64_MAX / DELAY_UNIT_MS) {
                    fail_cnt++;
                    continue;
                }
                delay_ms = (uint64_t)units * DELAY_UNIT_MS;
            }
        }

        char* pkg = strtrim(p);
        char* cpus = strtrim(eq);

        char* hash = strchr(cpus, '#');
        if (hash) {
            *hash = '\0';
            cpus = strtrim(cpus);
        }

        RuleAddResult result = add_rule(&new_rules, &rules_cnt, &cfg->topo,
                                        pkg, thread, cpus, delay_ms);
        if (result == RULE_ADD_INVALID) {
            fail_cnt++;
        } else if (result == RULE_ADD_NO_MEMORY) {
            goto error;
        }

    }

    if (!build_pkg_list(new_rules, rules_cnt, &new_pkgs, &pkgs_cnt)) goto error;

    if (cfg->rules) free(cfg->rules);
    if (cfg->pkgs) {
        for (size_t i = 0; i < cfg->num_pkgs; i++) free(cfg->pkgs[i]);
        free(cfg->pkgs);
    }

    if (last_mtime) *last_mtime = st.st_mtime;
    cfg->rules = new_rules;
    cfg->num_rules = rules_cnt;
    cfg->pkgs = new_pkgs;
    cfg->num_pkgs = pkgs_cnt;
    cfg->mtime = st.st_mtime;

    fclose(fp);
    printf("配置文件解析完成，共加载 %zu 条规则\n", rules_cnt);
    if (fail_cnt > 0) {
        fprintf(stderr, "警告: %zu 条规则因格式、CPU 范围或长度无效被跳过\n", fail_cnt);
    }
    return cfg;

error:
    if (new_rules) free(new_rules);
    if (new_pkgs) {
        for (size_t i = 0; i < pkgs_cnt; i++) free(new_pkgs[i]);
        free(new_pkgs);
    }
    fclose(fp);
    free(cfg);
    return NULL;
}

static int pid_cmp(const void *a, const void *b) {
    return (*(pid_t*)a - *(pid_t*)b);
}

static void proc_collect(const AppConfig* cfg, ProcCache* cache, size_t* count) {
    DIR* proc_dir = opendir("/proc");
    if (!proc_dir) return;
    int proc_fd = dirfd(proc_dir);
    *count = 0;

    if (cache->procs == NULL) {
        cache->procs_cap = 2048;
        cache->procs = calloc(cache->procs_cap, sizeof(ProcessInfo));
        if (!cache->procs) {
            closedir(proc_dir);
            return;
        }
    }

    size_t detected_count = cache->num_procs;
    DetectedProcess* detected = NULL;
    if (detected_count > 0) {
        detected = malloc(detected_count * sizeof(DetectedProcess));
        if (detected) {
            for (size_t i = 0; i < detected_count; i++) {
                detected[i].pid = cache->procs[i].pid;
                build_str(detected[i].pkg, sizeof(detected[i].pkg), cache->procs[i].pkg, NULL);
                detected[i].detected_at_ms = cache->procs[i].detected_at_ms;
            }
        } else {
            detected_count = 0;
        }
    }

    struct dirent* ent;
    time_t current_time = time(NULL);
    uint64_t current_monotonic_ms = monotonic_ms();
    int current_proc_total = 0;
    while ((ent = readdir(proc_dir))) {
        char *end;
        long pid = strtol(ent->d_name, &end, 10);
        if (*end != '\0')  continue;
        current_proc_total++;

        if (!cache->scan_all_proc) {
            bool is_tracked = (cache->num_tracked_pids > 0 &&
                bsearch(&pid, cache->tracked_pids, cache->num_tracked_pids,
                        sizeof(pid_t), pid_cmp) != NULL);
            if (!is_tracked) {
                struct stat statbuf;
                if (fstatat(proc_fd, ent->d_name, &statbuf, AT_SYMLINK_NOFOLLOW) != 0) continue;
                if (current_time - statbuf.st_mtime > 60) continue;
            }
        }

        int pid_fd = openat(proc_fd, ent->d_name, O_RDONLY | O_DIRECTORY);
        if (pid_fd == -1) continue;

        char cmd[MAX_PKG_LEN] = {0};
        if (!read_file(pid_fd, "cmdline", cmd, sizeof(cmd))) {
            close(pid_fd);
            continue;
        }
        char* name = strrchr(cmd, '/');
        name = name ? name + 1 : cmd;

        bool found = false;
        for (size_t j = 0; j < cfg->num_pkgs; j++) {
            if (strcmp(name, cfg->pkgs[j]) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            close(pid_fd);
            continue;
        }

        if (*count >= cache->procs_cap) {
            size_t new_cap = cache->procs_cap * 2;
            ProcessInfo* new_procs = realloc(cache->procs, new_cap * sizeof(ProcessInfo));
            if (!new_procs) {
                close(pid_fd);
                continue;
            }
            memset(new_procs + cache->procs_cap, 0, (new_cap - cache->procs_cap) * sizeof(ProcessInfo));
            cache->procs = new_procs;
            cache->procs_cap = new_cap;
        }

        ProcessInfo* proc = &cache->procs[*count];

        proc->pid = pid;
        build_str(proc->pkg, sizeof(proc->pkg), name, NULL);
        proc->detected_at_ms = current_monotonic_ms;
        for (size_t i = 0; i < detected_count; i++) {
            if (detected[i].pid == pid && strcmp(detected[i].pkg, name) == 0) {
                proc->detected_at_ms = detected[i].detected_at_ms;
                break;
            }
        }
        CPU_ZERO(&proc->base_cpus);
        proc->base_cpuset[0] = '\0';
        proc->base_delay_ms = 0;
        proc->num_threads = 0;
        proc->num_thread_rules = 0;

        if (!proc->thread_rules || proc->thread_rules_cap < 8) {
            size_t new_cap = proc->thread_rules_cap ? proc->thread_rules_cap * 2 : 8;
            AffinityRule** tmp = realloc(proc->thread_rules, new_cap * sizeof(AffinityRule*));
            if (!tmp) {
                close(pid_fd);
                continue;
            }
            proc->thread_rules = tmp;
            proc->thread_rules_cap = new_cap;
        }

        for (size_t i = 0; i < cfg->num_rules; i++) {
            const AffinityRule* rule = &cfg->rules[i];
            if (strcmp(rule->pkg, proc->pkg) != 0) continue;

            if (rule->thread[0]) {
                if (proc->num_thread_rules >= proc->thread_rules_cap) {
                    size_t new_cap = proc->thread_rules_cap * 2;
                    AffinityRule** tmp = realloc(proc->thread_rules, new_cap * sizeof(AffinityRule*));
                    if (!tmp) break;
                    proc->thread_rules = tmp;
                    proc->thread_rules_cap = new_cap;
                }
                proc->thread_rules[proc->num_thread_rules++] = (AffinityRule*)rule;
            } else {
                CPU_OR(&proc->base_cpus, &proc->base_cpus, &rule->cpus);
                build_str(proc->base_cpuset, sizeof(proc->base_cpuset), rule->cpuset_dir, NULL);
                proc->base_delay_ms = rule->delay_ms;
            }
        }

        if (CPU_COUNT(&proc->base_cpus) == 0 && proc->num_thread_rules == 0) {
            close(pid_fd);
            continue;
        }

        int task_fd = openat(pid_fd, "task", O_RDONLY | O_DIRECTORY);
        close(pid_fd);
        if (task_fd == -1) {
            continue;
        }

        DIR* task_dir = fdopendir(task_fd);
        if (!task_dir) {
            close(task_fd);
            continue;
        }

        if (!proc->threads || proc->threads_cap < 512) {
            size_t new_cap = proc->threads_cap ? proc->threads_cap * 2 : 64;
            ThreadInfo* tmp = realloc(proc->threads, new_cap * sizeof(ThreadInfo));
            if (!tmp) {
                closedir(task_dir);
                continue;
            }
            proc->threads = tmp;
            proc->threads_cap = new_cap;
        }

        struct dirent* tent;
        while ((tent = readdir(task_dir))) {
            char *end2;
            long tid = strtol(tent->d_name, &end2, 10);
            if (*end2 != '\0')  continue;
            char tname[MAX_THREAD_LEN] = {0};
            char comm_path[64];
            snprintf(comm_path, sizeof(comm_path), "%s/comm", tent->d_name);
            if (!read_file(task_fd, comm_path, tname, sizeof(tname))) continue;

            strtrim(tname);

            if (proc->num_threads >= proc->threads_cap) {
                size_t new_cap = proc->threads_cap * 2;
                ThreadInfo* tmp = realloc(proc->threads, new_cap * sizeof(ThreadInfo));
                if (!tmp) continue;
                proc->threads = tmp;
                proc->threads_cap = new_cap;
            }

            ThreadInfo* ti = &proc->threads[proc->num_threads];
            ti->tid = tid;
            build_str(ti->name, sizeof(ti->name), tname, NULL);
            CPU_ZERO(&ti->cpus);
            const AffinityRule* matched = NULL;
            int best_literal = -1;

            for (size_t i = 0; i < proc->num_thread_rules; i++) {
                const AffinityRule* rule = proc->thread_rules[i];
                if (strcmp(rule->thread, ti->name) == 0) {
                    CPU_ZERO(&ti->cpus);
                    CPU_OR(&ti->cpus, &ti->cpus, &rule->cpus);
                    matched = rule;
                    break;
                }
                if (fnmatch(rule->thread, ti->name, FNM_NOESCAPE) == 0) {
                    int lit = 0;
                    for (const char *c = rule->thread; *c; c++)
                        if (*c != '*' && *c != '?' && *c != '[') lit++;
                    if (lit > best_literal) {
                        best_literal = lit;
                        CPU_ZERO(&ti->cpus);
                        CPU_OR(&ti->cpus, &ti->cpus, &rule->cpus);
                        matched = rule;
                    }
                }
            }

            if (matched) {
                build_str(ti->cpuset_dir, sizeof(ti->cpuset_dir), matched->cpuset_dir, NULL);
                ti->bind_after_ms = delay_deadline(proc->detected_at_ms, matched->delay_ms);
            } else {
                ti->cpus = proc->base_cpus;
                build_str(ti->cpuset_dir, sizeof(ti->cpuset_dir), proc->base_cpuset, NULL);
                ti->bind_after_ms = delay_deadline(proc->detected_at_ms, proc->base_delay_ms);
            }

            proc->num_threads++;
        }

        closedir(task_dir);

        if (proc->num_threads > 0 && proc->threads_cap > proc->num_threads * 2) {
            size_t new_cap = proc->num_threads + proc->num_threads / 2;
            if (new_cap < 64) new_cap = 64;
            ThreadInfo* tmp = realloc(proc->threads, new_cap * sizeof(ThreadInfo));
            if (tmp) {
                proc->threads = tmp;
                proc->threads_cap = new_cap;
            }
        }

        (*count)++;
    }
    closedir(proc_dir);
    free(detected);
    if (current_proc_total > cache->last_proc_total) {
        cache->scan_all_proc = true;
    } else {
        cache->scan_all_proc = false;
    }
    cache->last_proc_total = current_proc_total;
}

static void update_cache(ProcCache* cache, const AppConfig* cfg, int* affinity_counter) {
    bool need_reload = false;
    struct sysinfo info;
    if (sysinfo(&info) != 0) {
        need_reload = true;
    } else {
        int current_proc_count = info.procs;
        if (current_proc_count > cache->last_proc_count + 11) {
            need_reload = true;
        } else if (current_proc_count > cache->last_proc_count) {
            *affinity_counter = 0;
        }
        cache->last_proc_count = current_proc_count;
    }
    static int kill_check_skip = 0;
    if (!need_reload && cache->procs != NULL) {
        if (++kill_check_skip >= 5) {
            kill_check_skip = 0;
            for (size_t i = 0; i < cache->num_procs; i++) {
                if (kill(cache->procs[i].pid, 0) != 0) {
                    need_reload = true;
                    break;
                }
            }
        }
    }
    if (need_reload) {
        kill_check_skip = 0;
        size_t new_count = 0;
        proc_collect(cfg, cache, &new_count);

        if (new_count > cache->tracked_pids_cap) {
            size_t new_cap = cache->tracked_pids_cap ? cache->tracked_pids_cap * 2 : new_count;
            pid_t* new_pids = realloc(cache->tracked_pids, new_cap * sizeof(pid_t));
            if (new_pids) {
                cache->tracked_pids = new_pids;
                cache->tracked_pids_cap = new_cap;
            }
        }

        if (cache->tracked_pids) {
            cache->num_tracked_pids = 0;
            for (size_t i = 0; i < new_count; i++) {
                if (cache->num_tracked_pids < cache->tracked_pids_cap) {
                    cache->tracked_pids[cache->num_tracked_pids++] = cache->procs[i].pid;
                }
            }
            if (cache->num_tracked_pids > 0) {
                qsort(cache->tracked_pids, cache->num_tracked_pids, sizeof(pid_t), pid_cmp);
            }
        }
        
        cache->num_procs = new_count;
        *affinity_counter = 0;
    }
}

static uint64_t apply_affinity(ProcCache* cache, const CpuTopology* topo, uint64_t now_ms) {
    uint64_t next_delayed_bind_ms = UINT64_MAX;
    for (size_t i = 0; i < cache->num_procs; i++) {
        const ProcessInfo* proc = &cache->procs[i];
        for (size_t j = 0; j < proc->num_threads; j++) {
            const ThreadInfo* ti = &proc->threads[j];
            if (ti->bind_after_ms > now_ms) {
                if (ti->bind_after_ms < next_delayed_bind_ms) {
                    next_delayed_bind_ms = ti->bind_after_ms;
                }
                continue;
            }
            if (topo->cpuset_enabled && topo->base_cpuset_fd != -1) {
                char tid_str[32];
                snprintf(tid_str, sizeof(tid_str), "%d\n", ti->tid);
                if (CPU_COUNT(&ti->cpus) == 0) {
                    cpu_set_t curr;
                    if (sched_getaffinity(ti->tid, sizeof(curr), &curr) == -1) continue;
                    if (CPU_EQUAL(&topo->present_cpus, &curr)) continue;
                    write_file(topo->base_cpuset_fd, "tasks", tid_str, O_WRONLY | O_APPEND);
                } else {
                    cpu_set_t curr;
                    if (sched_getaffinity(ti->tid, sizeof(curr), &curr) == -1) continue;
                    if (CPU_EQUAL(&ti->cpus, &curr)) continue;
                    if (ti->cpuset_dir[0]) {
                        int fd = openat(topo->base_cpuset_fd, ti->cpuset_dir, O_RDONLY | O_DIRECTORY);
                        if (fd != -1) {
                            write_file(fd, "tasks", tid_str, O_WRONLY | O_APPEND);
                            close(fd);
                        }
                    }
                }
            }
            if (CPU_COUNT(&ti->cpus) == 0) continue;
            if (sched_setaffinity(ti->tid, sizeof(ti->cpus), &ti->cpus) == -1 && errno == ESRCH) {
                cache->last_proc_count = 0;
            }
        }
    }
    return next_delayed_bind_ms;
}

static void config_release(AppConfig* cfg) {
    if (!cfg) return;
    if (atomic_fetch_sub(&cfg->ref_count, 1) == 1) {
        if (cfg->rules) free(cfg->rules);
        if (cfg->pkgs) {
            for (size_t i = 0; i < cfg->num_pkgs; i++) free(cfg->pkgs[i]);
            free(cfg->pkgs);
        }
        free(cfg);
    }
}

static AppConfig* get_config() {
    AppConfig* cfg = atomic_load_explicit(&current_config, memory_order_acquire);
    if (!cfg) return NULL;
    int old_ref = atomic_fetch_add_explicit(&cfg->ref_count, 1, memory_order_acq_rel);
    if (old_ref <= 0) {
        atomic_fetch_sub_explicit(&cfg->ref_count, 1, memory_order_release);
        return NULL;
    }
    if (atomic_load_explicit(&current_config, memory_order_acquire) != cfg) {
        atomic_fetch_sub_explicit(&cfg->ref_count, 1, memory_order_release);
        return NULL;
    }
    return cfg;
}

static void* config_loader_thread(void* arg) {
    int interval = *(int*)arg;
    free(arg);
    pthread_setname_np(pthread_self(), "ConfigLoader");

    time_t last_mtime = -1;
    while (1) {
        if (inotify_supported) {
            fd_set rfds;
            struct timeval tv;
            FD_ZERO(&rfds);
            FD_SET(inotify_fd, &rfds);
            tv.tv_sec = interval;
            tv.tv_usec = 0;

            int ret = select(inotify_fd + 1, &rfds, NULL, NULL, &tv);
            if (ret < 0) {
                if (errno == EINTR) continue;
                inotify_supported = 0;
                close(inotify_fd);
                inotify_fd = -1;
                continue;
            } else if (ret == 0) {
                continue;
            }

            char buf[4096] __attribute__((aligned(8)));
            ssize_t len = read(inotify_fd, buf, sizeof(buf));
            if (len <= 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    inotify_supported = 0;
                    close(inotify_fd);
                    inotify_fd = -1;
                }
                continue;
            }

            bool reload_needed = false;
            for (char* p = buf; p < buf + len;) {
                struct inotify_event* event = (struct inotify_event*)p;
                if (event->mask & (IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF)) {
                    reload_needed = true;

                    if (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
                        sleep(interval);
                        AppConfig* cfg = get_config();
                        if (cfg) {
                            inotify_rm_watch(inotify_fd, inotify_wd);
                            inotify_wd = inotify_add_watch(inotify_fd, cfg->config_file, IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF);
                            last_mtime = -1;
                            config_release(cfg);
                        }
                        if (inotify_wd < 0) {
                            inotify_supported = 0;
                            close(inotify_fd);
                            inotify_fd = -1;
                            break;
                        }
                    }
                }
                p += sizeof(struct inotify_event) + event->len;
            }

            if (reload_needed) {
                AppConfig* cfg = get_config();
                if (cfg) {
                    AppConfig* new_config = load_config(cfg->config_file, &cfg->topo, &last_mtime);
                    if (new_config) {
                        AppConfig* old_config = atomic_exchange(&current_config, new_config);
                        atomic_store(&config_updated, 1);
                        if (old_config) config_release(old_config);
                    }
                    config_release(cfg);
                }
            }
        } else {
            AppConfig* cfg = get_config();
            if (cfg) {
                AppConfig* new_config = load_config(cfg->config_file, &cfg->topo, &last_mtime);
                if (new_config) {
                    AppConfig* old_config = atomic_exchange(&current_config, new_config);
                    atomic_store(&config_updated, 1);
                    if (old_config) config_release(old_config);
                }
                config_release(cfg);
            }
            sleep(interval);
        }
    }
    return NULL;
}

static void print_help(const char* prog_name) {
    printf("Usage: %s [OPTIONS]\n", prog_name);
    printf("Options:\n");
    printf("  -c <config_file>   指定配置文件 (默认: ./applist.conf)\n");
    printf("  -s <interval>      设置检查间隔(秒) (必须>=1, 默认: 2)\n");
    printf("  -v                 显示程序版本\n");
    printf("  -h                 显示帮助信息\n");
    printf("\n示例:\n");
    printf("  %s -c /data/applist.conf -s 3\n", prog_name);
}

int main(int argc, char **argv) {
    CpuTopology topo = init_cpu_topo();
    char config_file[4096] = "./applist.conf";
    int sleep_interval = 2;
    int opt;
    while ((opt = getopt(argc, argv, "c:s:hv")) != -1) {
        switch (opt) {
            case 'c':
                build_str(config_file, sizeof(config_file), optarg, NULL);
                printf("配置文件: %s\n", config_file);
                break;
            case 's':
            {
                char *endptr;
                long val = strtol(optarg, &endptr, 10);
                if (endptr == optarg || *endptr != '\0' || val < 1) {
                    fprintf(stderr, "无效的时间间隔: %s\n", optarg);
                    fprintf(stderr, "间隔必须是 >=1 的整数\n");
                    exit(EXIT_FAILURE);
                }
                sleep_interval = (int)val;
                printf("检查间隔: %d 秒\n", sleep_interval);
                break;
            }
            case 'v':
                printf("AppOpt 版本 %s\n", VERSION);
                exit(EXIT_SUCCESS);
            case 'h':
                print_help(argv[0]);
                exit(EXIT_SUCCESS);
            default:
                print_help(argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    struct stat st;
    if (stat(config_file, &st) != 0) {
        const char* initial_content = "# 规则编写与使用说明请参考 http://AppOpt.suto.top\n\n";
        if (write_file(AT_FDCWD, config_file, initial_content, O_WRONLY | O_CREAT | O_TRUNC)) {
            printf("配置文件不存在，重建一个空的配置文件: %s\n", config_file);
        }
    }

    AppConfig* initial_config = load_config(config_file, &topo, NULL);
    if (!initial_config) {
        fprintf(stderr, "初始配置加载失败\n");
        exit(EXIT_FAILURE);
    }
    atomic_store(&current_config, initial_config);
    atomic_store(&config_updated, 1);

    inotify_fd = inotify_init1(IN_CLOEXEC);
    if (inotify_fd >= 0) {
        int flags = fcntl(inotify_fd, F_GETFL);
        if (flags >= 0) fcntl(inotify_fd, F_SETFL, flags | O_NONBLOCK);
        inotify_wd = inotify_add_watch(inotify_fd, config_file, IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF);
        if (inotify_wd >= 0) {
            inotify_supported = 1;
            printf("启用inotify监控配置文件变更\n");
        } else {
            close(inotify_fd);
            inotify_fd = -1;
            printf("inotify初始化失败，使用轮询模式\n");
        }
    }

    pthread_t loader_thread;
    int* interval_ptr = malloc(sizeof(int));
    if (!interval_ptr) {
        config_release(initial_config);
        if (inotify_supported) close(inotify_fd);
        exit(EXIT_FAILURE);
    }
    *interval_ptr = sleep_interval;

    if (pthread_create(&loader_thread, NULL, config_loader_thread, interval_ptr) != 0) {
        perror("配置加载器线程创建失败");
        free(interval_ptr);
        config_release(initial_config);
        if (inotify_supported) close(inotify_fd);
        exit(EXIT_FAILURE);
    }
    pthread_detach(loader_thread);

    ProcCache cache = {0};
    int affinity_counter = 0;
    uint64_t next_scan_ms = 0;
    uint64_t next_delayed_bind_ms = UINT64_MAX;
    printf("启动AppOpt服务 v%s\n", VERSION);

    for (;;) {
        uint64_t now_ms = monotonic_ms();
        if (atomic_exchange(&config_updated, 0)) {
            cache.scan_all_proc = true;
            cache.last_proc_count = 0;
            next_scan_ms = 0;
            next_delayed_bind_ms = UINT64_MAX;
        }

        if (now_ms >= next_scan_ms) {
            AppConfig* cfg = get_config();
            if (cfg) {
                update_cache(&cache, cfg, &affinity_counter);
                affinity_counter--;
                if (affinity_counter < 1) {
                    now_ms = monotonic_ms();
                    next_delayed_bind_ms = apply_affinity(&cache, &cfg->topo, now_ms);
                    affinity_counter = 5;
                }
                config_release(cfg);
            }
            next_scan_ms = now_ms + (uint64_t)sleep_interval * 1000ULL;
        }

        now_ms = monotonic_ms();
        if (next_delayed_bind_ms != UINT64_MAX && now_ms >= next_delayed_bind_ms) {
            next_delayed_bind_ms = apply_affinity(&cache, &topo, now_ms);
        }
        usleep(DELAY_POLL_US);
    }
}
