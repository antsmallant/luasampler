// This file is modified version from https://github.com/JieTrancender/game-server/tree/main/3rd/luaprofile
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "profile.h"
#include "imap.h"
#include "smap.h"
#include "icallpath.h"
#include "lobject.h"
#include "lfunc.h"
#include "lstate.h"
#include "lua.h"
#include "lauxlib.h"
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include "lprof.h"
#include <signal.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/ucontext.h>
#include <dlfcn.h>

/*
callpath node 构成一棵树，每个frame可以在这个树中找到一个 node。framestack 从 root frame 到 cur frame, 对应这棵树的某条路径。  

prototype 相当于一个 lua closure/c closure/c function 在 lua vm 中的唯一指针。它是 callpath node 的 key，一个新的 frame 要找到对应的 callpath node，就用 prototype 从父frame 的 node 里面查找 child node，找不到则创建。  

相同 prototype 的闭包或函数，可能会在 callpath tree 对应多个 node，这取决于它的父亲是谁。   

node1
  node21
    node31
对应着 frame1 -> frame2 -> frame3 的调用关系。 
*/

#define MAX_CALL_SIZE               1024
#define MAX_CO_SIZE                 1024
#define NANOSEC                     1000000000
#define MICROSEC                    1000000
#define MAX_SAMPLE_DEPTH            256

// 模式定义（统一用于 CPU/Memory）
#define MODE_OFF                    0
#define MODE_PROFILE                1
#define MODE_SAMPLE                 2

#define DEFAULT_CPU_SAMPLE_HZ       250

static char profile_context_key = 'x';


// forward decl for trap callback implemented later (needs structs defined)
static void _on_prof_trap_n(lua_State* L, unsigned int n);

#ifdef LUA_PROF_TRAP
// -------- CPU sampling (TLS + per-thread timer + signal handler) --------
static int start_thread_timer_hz(int hz);
static void stop_thread_timer(void);
static __thread lua_State* g_prof_current_L = NULL;
static __thread timer_t g_prof_timerid;
static int g_prof_signo = 0; // assigned on install
static __thread uintptr_t g_stack_lo = 0;
static __thread uintptr_t g_stack_hi = 0;
static char g_self_module[128];

#define C_RB_CAP 8192
#define C_MAX_FRAMES 64
typedef struct {
    uint16_t depth;
    uintptr_t pcs[C_MAX_FRAMES];
} c_sample_t;
static __thread c_sample_t g_c_rb[C_RB_CAP];
static __thread unsigned g_c_rb_head = 0;

/* write helpers (async-signal-safe) */
static inline void _hex_nibble(char n, char* out) {
    *out = (n < 10) ? ('0' + n) : ('a' + (n - 10));
}
static size_t _hex_ptr(uintptr_t v, char* buf) {
    char tmp[2 + sizeof(uintptr_t) * 2];
    size_t p = 0;
    tmp[p++] = '0'; tmp[p++] = 'x';
    int started = 0;
    for (int i = (int)(sizeof(uintptr_t) * 2 - 1); i >= 0; --i) {
        char nib = (char)((v >> (i * 4)) & 0xf);
        if (!started && nib == 0 && i != 0) continue;
        started = 1;
        _hex_nibble(nib, tmp + p);
        p++;
    }
    for (size_t i = 0; i < p; ++i) buf[i] = tmp[i];
    return p;
}

static void prof_sig_handler(int sig, siginfo_t* si, void* uctx) {
    (void)sig; (void)si; (void)uctx;
    lua_State* L = g_prof_current_L;
    if (L) {
        if (L->prof_ticks < 0x7fffffffU) {
            L->prof_ticks++;
        }
    }

    /* Grab C stack (best-effort, x86_64) and print one folded line to stderr */
#if defined(__x86_64__)
    ucontext_t* ctx = (ucontext_t*)uctx;
    uintptr_t ip = 0, bp = 0;
# ifdef REG_RIP
    ip = (uintptr_t)ctx->uc_mcontext.gregs[REG_RIP];
# endif
# ifdef REG_RBP
    bp = (uintptr_t)ctx->uc_mcontext.gregs[REG_RBP];
# endif
    /* collect frames into TLS ring buffer */
    unsigned idx = g_c_rb_head++ % C_RB_CAP;
    c_sample_t* cs = &g_c_rb[idx];
    cs->depth = 0;
    if (ip && cs->depth < C_MAX_FRAMES) {
        cs->pcs[cs->depth++] = ip;
    }
    uintptr_t lo = g_stack_lo, hi = g_stack_hi;
    int depth = 0;
    while (bp && bp >= lo && (bp + 2 * sizeof(uintptr_t)) < hi && depth < 64) {
        /* ret address at [bp + sizeof(void*)], next bp at [bp] */
        uintptr_t next_bp = 0;
        uintptr_t ret = 0;
        /* guard against invalid memory by simple bounds checks */
        next_bp = *((uintptr_t*)bp);
        ret = *((uintptr_t*)(bp + sizeof(uintptr_t)));
        if (next_bp <= bp || next_bp >= hi) break;
        if (cs->depth < C_MAX_FRAMES) {
            cs->pcs[cs->depth++] = ret;
        }
        bp = next_bp;
        depth++;
    }
#else
    (void)uctx;
#endif
}

static int install_prof_signal_once(void) {
    if (g_prof_signo != 0) return 0;
    int signo = SIGRTMIN + 1;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = prof_sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    if (sigaction(signo, &sa, NULL) != 0) return -1;
    g_prof_signo = signo;
    /* identify our own module basename for later frame filtering */
    Dl_info di;
    if (dladdr((void*)&prof_sig_handler, &di) && di.dli_fname) {
        const char* base = strrchr(di.dli_fname, '/');
        base = base ? base + 1 : di.dli_fname;
        size_t n = strlen(base);
        if (n >= sizeof(g_self_module)) n = sizeof(g_self_module) - 1;
        memcpy(g_self_module, base, n);
        g_self_module[n] = '\0';
    } else {
        g_self_module[0] = '\0';
    }
    return 0;
}

static int start_thread_timer_hz(int hz) {
    if (hz <= 0) hz = 250;
    if (install_prof_signal_once() != 0) return -1;
    /* cache stack bounds for safe FP walk */
    {
        pthread_attr_t attr;
        if (pthread_getattr_np(pthread_self(), &attr) == 0) {
            void* stackaddr = NULL;
            size_t stacksize = 0;
            if (pthread_attr_getstack(&attr, &stackaddr, &stacksize) == 0) {
                g_stack_lo = (uintptr_t)stackaddr;
                g_stack_hi = (uintptr_t)stackaddr + stacksize;
            }
            pthread_attr_destroy(&attr);
        }
    }
    struct sigevent sev;
    memset(&sev, 0, sizeof(sev));
#ifdef SIGEV_THREAD_ID
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev._sigev_un._tid = (int)syscall(SYS_gettid);
#else
    // Fallback: process-directed; acceptable in single-threaded lua
    sev.sigev_notify = SIGEV_SIGNAL;
#endif
    sev.sigev_signo = g_prof_signo;
    if (timer_create(CLOCK_THREAD_CPUTIME_ID, &sev, &g_prof_timerid) != 0) return -1;
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_value.tv_nsec = 1000000000LL / hz;
    its.it_interval.tv_nsec = its.it_value.tv_nsec;
    if (timer_settime(g_prof_timerid, 0, &its, NULL) != 0) return -1;
    return 0;
}

static void stop_thread_timer(void) {
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    timer_settime(g_prof_timerid, 0, &its, NULL);
    timer_delete(g_prof_timerid);
    memset(&g_prof_timerid, 0, sizeof(g_prof_timerid));
}
#endif


// 获取单调递增的时间戳（纳秒），不会被 NTP 调整。
// 只用于计算时间差，不能当成绝对时间戳用于获取当前的年月日。
// 如果需要获取绝对时间戳，请使用 get_realtime_ns。    
static inline uint64_t
get_mono_ns() {
    struct timespec ti;
    clock_gettime(CLOCK_MONOTONIC, &ti);
    uint64_t sec = (uint64_t)ti.tv_sec;
    uint64_t nsec = (uint64_t)ti.tv_nsec;
    return sec * (uint64_t)NANOSEC + nsec;
}

// 获取绝对时间戳（纳秒），会受 NTP 调整。
// 可以用于获取当前的年月日。
static inline uint64_t
get_realtime_ns() {
    struct timespec ti;
    clock_gettime(CLOCK_REALTIME, &ti);
    uint64_t sec = (uint64_t)ti.tv_sec;
    uint64_t nsec = (uint64_t)ti.tv_nsec;
    return sec * (uint64_t)NANOSEC + nsec;
}

// 读取启动参数：{ cpu = "off|profile|sample", mem = "off|profile|sample", cpu_sample_hz = int }
static bool
read_arg(lua_State* L, int* out_cpu_mode, int* out_mem_mode, int* out_cpu_sample_hz) {
    if (!out_cpu_mode || !out_mem_mode || !out_cpu_sample_hz) return false;
    *out_cpu_mode = MODE_PROFILE;
    *out_mem_mode = MODE_PROFILE;
    *out_cpu_sample_hz = DEFAULT_CPU_SAMPLE_HZ;
    if (lua_gettop(L) < 1 || !lua_istable(L, 1)) return true;

    lua_getfield(L, 1, "cpu");
    if (lua_isstring(L, -1)) {
        const char* s = lua_tostring(L, -1);
        if (strcmp(s, "off") == 0) *out_cpu_mode = MODE_OFF;
        else if (strcmp(s, "profile") == 0) *out_cpu_mode = MODE_PROFILE;
        else if (strcmp(s, "sample") == 0) *out_cpu_mode = MODE_SAMPLE;
        else {printf("invalid cpu mode: %s\n", s); return false;}
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "mem");
    if (lua_isstring(L, -1)) {
        const char* s = lua_tostring(L, -1);
        if (strcmp(s, "off") == 0) *out_mem_mode = MODE_OFF;
        else if (strcmp(s, "profile") == 0) *out_mem_mode = MODE_PROFILE;
        else if (strcmp(s, "sample") == 0) *out_mem_mode = MODE_SAMPLE;
        else {printf("invalid mem mode: %s\n", s); return false;}
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "cpu_sample_hz");
    if (lua_isinteger(L, -1)) {
        int sp = (int)lua_tointeger(L, -1);
        if (sp > 0) *out_cpu_sample_hz = sp;
    }
    lua_pop(L, 1);
    return true;
}

struct call_frame {
    const void* prototype;
    struct icallpath_context*   path;
    bool  tail;
    uint64_t call_time;
    uint64_t co_cost;     // co yield cost 
};

struct call_state {
    lua_State*  co;
    uint64_t    leave_time; // co yield begin time
    int         top;
    struct call_frame call_list[0];
};

struct profile_context {
    uint64_t    start_time;
    bool        is_ready;
    bool        running_in_hook;
    lua_Alloc   last_alloc_f;
    void*       last_alloc_ud;
    struct imap_context*        cs_map;
    struct imap_context*        alloc_map;
    struct imap_context*        symbol_map;
    smap_t*                     sample_map;   // folded stacks for lua cpu sampling
    smap_t*                     c_sample_map; // folded stacks for c cpu sampling
    struct icallpath_context*   callpath;
    struct call_state*          cur_cs;
    int         cpu_mode;       // MODE_*
    int         mem_mode;       // MODE_*
    int         cpu_sample_hz;  // instruction count for LUA_MASKCOUNT
    uint64_t    rng_state;      // RNG state for sampling gaps
    uint64_t    profile_cost_ns;
};

struct callpath_node {
    struct callpath_node*   parent;
    const char* source;
    const char* name;
    int     line;
    int     depth;
    uint64_t last_ret_time;
    uint64_t call_count;
    uint64_t real_cost;
    uint64_t cpu_samples;    // sampling count (leaf samples), aggregated at dump
    uint64_t alloc_bytes;
    uint64_t free_bytes;
    uint64_t alloc_times;
    uint64_t free_times;
    uint64_t realloc_times;
};

struct alloc_node {
    size_t live_bytes;                // 当前存活字节
    struct callpath_node* path;       // 当前所有权路径
};

struct symbol_info {
    char* name;
    char* source;
    int line;
};

// 简单的字符串 HashMap（链式散列），用于 CPU 抽样折叠栈
// use external smap

static struct callpath_node*
callpath_node_create() {
    struct callpath_node* node = (struct callpath_node*)pmalloc(sizeof(*node));
    node->parent = NULL;
    node->source = NULL;
    node->name = NULL;
    node->line = 0;
    node->depth = 0;
    node->last_ret_time = 0;
    node->call_count = 0;
    node->real_cost = 0;
    node->cpu_samples = 0;
    node->alloc_bytes = 0;
    node->free_bytes = 0;
    node->alloc_times = 0;
    node->free_times = 0;
    node->realloc_times = 0;
    return node;
}

static struct alloc_node*
alloc_node_create() {
    struct alloc_node* node = (struct alloc_node*)pmalloc(sizeof(*node));
    node->live_bytes = 0;
    node->path = NULL;
    return node;
}

struct dump_call_path_arg {
    struct profile_context* pcontext;
    lua_State* L;
    uint64_t index;
    uint64_t alloc_bytes_sum;
    uint64_t free_bytes_sum;
    uint64_t alloc_times_sum;
    uint64_t free_times_sum;
    uint64_t realloc_times_sum;
};

static void _init_dump_call_path_arg(struct dump_call_path_arg* arg, struct profile_context* pcontext, lua_State* L) {
    arg->pcontext = pcontext;
    arg->L = L;
    arg->index = 0;
    arg->alloc_bytes_sum = 0;
    arg->free_bytes_sum = 0;
    arg->alloc_times_sum = 0;
    arg->free_times_sum = 0;
    arg->realloc_times_sum = 0;
}

struct sum_root_stat_arg {
    uint64_t real_cost_sum;
};

static void _init_sum_root_stat_arg(struct sum_root_stat_arg* arg) {
    arg->real_cost_sum = 0;
}

static inline char*
pstrdup(const char* s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char* d = (char*)pmalloc(n + 1);
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

// free counters stored as smap values (uint64_t*)
static void _free_counter_cb(const char* key, void* value, void* ud) {
    (void)key; (void)ud;
    if (value) pfree(value);
}

struct smap_dump_ctx {
    lua_State* L;
    size_t idx;
};

struct fg_dump_ctx {
    luaL_Buffer* buf;
    struct imap_context* symbol_map;
};

static void _fg_dump_cb(const char* key, void* value, void* ud) {
    struct fg_dump_ctx* ctx = (struct fg_dump_ctx*)ud;
    luaL_Buffer* b = ctx->buf;
    uint64_t samples = value ? *(uint64_t*)value : 0;
    if (samples == 0) return;
    const char* p = key;
    char token[64];
    size_t tlen = 0;
    int first = 1;
    while (1) {
        char c = *p++;
        if (c == ';' || c == '\0') {
            token[tlen] = '\0';
            void* fptr = NULL;
            sscanf(token, "%p", &fptr);
            uint64_t sym_key = (uint64_t)((uintptr_t)fptr);
            struct symbol_info* si = (struct symbol_info*)imap_query(ctx->symbol_map, sym_key);
            if (!first) luaL_addchar(b, ';');
            if (si) {
                char namebuf[512];
                const char* nm = (si->name && si->name[0]) ? si->name : "anonymous";
                const char* src = (si->source && si->source[0]) ? si->source : "(source)";
                int ln = si->line;
                int n = snprintf(namebuf, sizeof(namebuf)-1, "%s %s:%d", nm, src, ln);
                if (n > 0) luaL_addlstring(b, namebuf, (size_t)n);
            } else {
                luaL_addlstring(b, token, tlen);
            }
            tlen = 0;
            first = 0;
            if (c == '\0') break;
        } else {
            if (tlen + 1 < sizeof(token)) token[tlen++] = c;
        }
    }
    char tail[64];
    int m = snprintf(tail, sizeof(tail)-1, " %llu\n", (unsigned long long)samples);
    if (m > 0) luaL_addlstring(b, tail, (size_t)m);
}

/* write c_sample_map entries to file */
static void _cmap_write_cb(const char* key, void* value, void* ud) {
    FILE* fp = (FILE*)ud;
    if (!fp) return;
    uint64_t samples = value ? *(uint64_t*)value : 0;
    if (samples == 0) return;
    fprintf(fp, "%s %llu\n", key, (unsigned long long)samples);
}

/* ---- Dump helpers (refactor) ---- */
static void fold_c_tls_samples(struct profile_context* context) {
    if (!context || !context->c_sample_map) return;
    unsigned cap = C_RB_CAP;
    for (unsigned i = 0; i < cap; ++i) {
        c_sample_t* cs = &g_c_rb[i];
        if (cs->depth == 0) continue;
        char keybuf[4096];
        size_t kp = 0;
        /* choose start index (from leaf side): skip frames belonging to our module or vdso/sig trampolines */
        int start_idx_leaf = 0;
        for (int d = 0; d < cs->depth; ++d) { /* leaf -> root scan to find first non-internal leaf */
            Dl_info info;
            const char* so = NULL;
            const char* name = NULL;
            if (dladdr((void*)cs->pcs[d], &info)) {
                so = info.dli_fname;
                name = info.dli_sname;
            }
            const char* base = so ? strrchr(so, '/') : NULL;
            base = base ? base + 1 : so;
            int is_self = (base && g_self_module[0] && strcmp(base, g_self_module) == 0);
            int is_vdso = (base && strstr(base, "linux-vdso") != NULL);
            int is_sigret = (name && (strstr(name, "__restore_rt") || strstr(name, "rt_sigreturn")));
            if (!(is_self || is_vdso || is_sigret)) {
                start_idx_leaf = d;
                break;
            }
        }
        /* build root->leaf order for FlameGraph (reverse from depth-1 down to start_idx_leaf) */
        for (int d = cs->depth - 1; d >= start_idx_leaf; --d) {
            Dl_info info;
            const char* name = NULL;
            const char* so = NULL;
            if (dladdr((void*)cs->pcs[d], &info)) {
                if (info.dli_sname && info.dli_sname[0]) name = info.dli_sname;
                if (info.dli_fname && info.dli_fname[0]) so = info.dli_fname;
            }
            /* build display name */
            char namebuf[384];
            if (name && name[0]) {
                /* module!symbol+offset */
                uintptr_t baseaddr = info.dli_fbase ? (uintptr_t)info.dli_fbase : 0;
                uintptr_t off = baseaddr ? (uintptr_t)cs->pcs[d] - baseaddr : 0;
                const char* modbase = so ? (strrchr(so, '/') ? strrchr(so, '/') + 1 : so) : "unknown";
                snprintf(namebuf, sizeof(namebuf), "%s!%s+0x%lx", modbase, name, (unsigned long)off);
            } else if (so && so[0]) {
                /* basename(so)+offset */
                const char* base = strrchr(so, '/');
                base = base ? base + 1 : so;
                uintptr_t baseaddr = info.dli_fbase ? (uintptr_t)info.dli_fbase : 0;
                uintptr_t off = baseaddr ? (uintptr_t)cs->pcs[d] - baseaddr : (uintptr_t)cs->pcs[d];
                snprintf(namebuf, sizeof(namebuf), "%s+0x%lx", base, (unsigned long)off);
            } else {
                char hex[2 + sizeof(uintptr_t) * 2 + 1];
                size_t hn = _hex_ptr(cs->pcs[d], hex);
                hex[hn] = '\0';
                snprintf(namebuf, sizeof(namebuf), "%s", hex);
            }
            size_t nlen = strlen(namebuf);
            if (kp + nlen + 1 >= sizeof(keybuf)) break;
            if (kp > 0) keybuf[kp++] = ';';
            memcpy(keybuf + kp, namebuf, nlen);
            kp += nlen;
        }
        keybuf[kp] = '\0';
        if (kp > 0) {
            uint64_t* cnt = (uint64_t*)smap_get(context->c_sample_map, keybuf);
            if (!cnt) {
                char* keydup = pstrdup(keybuf);
                cnt = (uint64_t*)pmalloc(sizeof(uint64_t));
                *cnt = 0;
                smap_set(context->c_sample_map, keydup, cnt);
            }
            (*cnt)++;
        }
        /* do not clear here; allow other consumers (e.g. raw writer) to read; clear once after both done */
    }
}

static void write_c_samples_file(struct profile_context* context, const char* path) {
    if (!context || !context->c_sample_map || !path) return;
    FILE* fp = fopen(path, "w");
    if (!fp) return;
    smap_iterate(context->c_sample_map, _cmap_write_cb, fp);
    fclose(fp);
}

static void push_lua_folded_samples(lua_State* L, struct profile_context* context) {
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    struct fg_dump_ctx fctx = { .buf = &b, .symbol_map = context->symbol_map };
    smap_iterate(context->sample_map, _fg_dump_cb, &fctx);
    luaL_pushresult(&b);
}

static void write_c_samples_raw(const char* path) {
    if (!path) return;
    FILE* fp = fopen(path, "w");
    if (!fp) return;
    unsigned cap = C_RB_CAP;
    for (unsigned i = 0; i < cap; ++i) {
        c_sample_t* cs = &g_c_rb[i];
        if (cs->depth == 0) continue;
        size_t written = 0;
        /* raw output: module!0xoffset chain in root->leaf order */
        for (int d = cs->depth - 1; d >= 0; --d) {
            Dl_info info;
            const char* so = NULL;
            if (dladdr((void*)cs->pcs[d], &info)) {
                if (info.dli_fname && info.dli_fname[0]) so = info.dli_fname;
            }
            const char* modbase = "unknown";
            if (so && so[0]) {
                const char* base = strrchr(so, '/');
                modbase = base ? base + 1 : so;
            }
            uintptr_t baseaddr = info.dli_fbase ? (uintptr_t)info.dli_fbase : 0;
            uintptr_t off = baseaddr ? (uintptr_t)cs->pcs[d] - baseaddr : (uintptr_t)cs->pcs[d];
            fprintf(fp, "%s!0x%lx", modbase, (unsigned long)off);
            written++;
            if (d > 0) fputc(';', fp);
        }
        if (written > 0) fputc('\n', fp);
    }
    fclose(fp);
}

/* gperftools legacy cpuprofile format writer */
static void write_c_profile_pprof(struct profile_context* context, const char* path) {
    if (!path) return;
    FILE* fp = fopen(path, "wb");
    if (!fp) return;

    /* header: slots of pointer width, native endian */
    uintptr_t hdr[5];
    hdr[0] = (uintptr_t)0;         /* header count = 0 */
    hdr[1] = (uintptr_t)3;         /* header slots after this one = 3 */
    hdr[2] = (uintptr_t)0;         /* version = 0 */
    uint64_t period_us64 = 0;
    int hz = context ? context->cpu_sample_hz : DEFAULT_CPU_SAMPLE_HZ;
    if (hz <= 0) hz = DEFAULT_CPU_SAMPLE_HZ;
    period_us64 = (uint64_t)(1000000ULL / (uint64_t)hz);
    hdr[3] = (uintptr_t)period_us64; /* sampling period in microseconds */
    hdr[4] = (uintptr_t)0;         /* padding */
    fwrite(hdr, sizeof(uintptr_t), 5, fp);

    /* records: for each TLS sample write [count, depth, pcs...] with pcs leaf-first */
    unsigned cap = C_RB_CAP;
    for (unsigned i = 0; i < cap; ++i) {
        c_sample_t* cs = &g_c_rb[i];
        if (cs->depth == 0) continue;
        uintptr_t count = (uintptr_t)1;
        uintptr_t depth = (uintptr_t)cs->depth;
        fwrite(&count, sizeof(uintptr_t), 1, fp);
        fwrite(&depth, sizeof(uintptr_t), 1, fp);
        /* leaf-first as stored: pcs[0]=ip, pcs[1]=caller,... */
        for (uint16_t d = 0; d < cs->depth; ++d) {
            uintptr_t pc = (uintptr_t)cs->pcs[d];
            fwrite(&pc, sizeof(uintptr_t), 1, fp);
        }
    }

    /* trailer: 0,1,0 */
    uintptr_t tr[3];
    tr[0] = (uintptr_t)0;
    tr[1] = (uintptr_t)1;
    tr[2] = (uintptr_t)0;
    fwrite(tr, sizeof(uintptr_t), 3, fp);

    /* append /proc/self/maps as text */
    FILE* maps = fopen("/proc/self/maps", "r");
    if (maps) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), maps)) > 0) {
            fwrite(buf, 1, n, fp);
        }
        fclose(maps);
    }

    fclose(fp);
}

static void clear_c_tls_samples(void) {
    unsigned cap = C_RB_CAP;
    for (unsigned i = 0; i < cap; ++i) {
        g_c_rb[i].depth = 0;
    }
}

static struct profile_context *
profile_create() {
    struct profile_context* context = (struct profile_context*)pmalloc(sizeof(*context));
    
    context->start_time = 0;
    context->is_ready = false;
    context->cs_map = imap_create();
    context->alloc_map = imap_create();
    context->symbol_map = imap_create();
    context->sample_map = smap_create(2048);
    context->c_sample_map = smap_create(2048);
    context->callpath = NULL;
    context->cur_cs = NULL;
    context->running_in_hook = false;
    context->last_alloc_f = NULL;
    context->last_alloc_ud = NULL;
    context->cpu_mode = MODE_PROFILE;       // default: profile
    context->mem_mode = MODE_PROFILE;       // default: profile
    context->cpu_sample_hz = DEFAULT_CPU_SAMPLE_HZ; // default: hz for sample
    context->rng_state = 0;
    context->profile_cost_ns = 0;
    return context;
}

static void
_ob_free_call_state(uint64_t key, void* value, void* ud) {
    pfree(value);
}

static void
_ob_free_symbol(uint64_t key, void* value, void* ud) {
    (void)key; (void)ud;
    struct symbol_info* si = (struct symbol_info*)value;
    if (si) {
        if (si->name) pfree(si->name);
        if (si->source) pfree(si->source);
        pfree(si);
    }
}

static void
_ob_free_alloc_node(uint64_t key, void* value, void* ud) {
    (void)key; (void)ud;
    struct alloc_node* an = (struct alloc_node*)value;
    if (an) {
        pfree(an);
    }
}

static void
profile_free(struct profile_context* context) {
    if (context->callpath) {
        icallpath_free(context->callpath);
        context->callpath = NULL;
    }

    imap_dump(context->cs_map, _ob_free_call_state, NULL);
    imap_free(context->cs_map);
    imap_dump(context->symbol_map, _ob_free_symbol, NULL);
    imap_free(context->symbol_map);
    if (context->sample_map) {
        // free smap entries' values (uint64_t*)
        smap_iterate(context->sample_map, _free_counter_cb, NULL);
        smap_free(context->sample_map);
    }
    if (context->c_sample_map) {
        smap_iterate(context->c_sample_map, _free_counter_cb, NULL);
        smap_free(context->c_sample_map);
    }
    imap_dump(context->alloc_map, _ob_free_alloc_node, NULL);
    imap_free(context->alloc_map);
    pfree(context);
}

static inline struct call_frame *
push_callframe(struct call_state* cs) {
    if(cs->top >= MAX_CALL_SIZE) {
        assert(false);
    }
    return &cs->call_list[cs->top++];
}

static inline struct call_frame *
pop_callframe(struct call_state* cs) {
    if(cs->top<=0) {
        assert(false);
    }
    return &cs->call_list[--cs->top];
}

static inline struct call_frame *
cur_callframe(struct call_state* cs) {
    if(cs->top<=0) {
        return NULL;
    }

    uint64_t idx = cs->top-1;
    return &cs->call_list[idx];
}

static inline struct profile_context *
get_profile_context(lua_State* L) {
    struct profile_context* ctx = NULL;
    lua_pushlightuserdata(L, &profile_context_key);
    lua_rawget(L, LUA_REGISTRYINDEX);
    ctx = (struct profile_context*)lua_touserdata(L, -1);
        lua_pop(L, 1);
    return ctx;
}

static void set_profile_context(lua_State* L, struct profile_context* ctx) {
    lua_pushlightuserdata(L, &profile_context_key);
    lua_pushlightuserdata(L, (void*)ctx);
    lua_rawset(L, LUA_REGISTRYINDEX);
}

static void unset_profile_context(lua_State* L) {
    lua_pushlightuserdata(L, &profile_context_key);
    lua_pushnil(L);
    lua_rawset(L, LUA_REGISTRYINDEX);
}

// --- Random gap generator (exponential/geometric) ---
static inline uint64_t xorshift64(uint64_t* s) {
    uint64_t x = (*s) ? *s : 88172645463393265ULL;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *s = x;
    return x;
}

static inline int next_exponential_gap(struct profile_context* ctx) {
    // mean = ctx->cpu_sample_hz (instructions)
    // u in (0,1], avoid 0
    uint64_t r = xorshift64(&ctx->rng_state);
    double u = ( (r >> 11) * (1.0 / 9007199254740992.0) ); // 53-bit to [0,1)
    if (u <= 0.0) u = 1e-12;
    int gap = (int)floor(-log(u) * (double)ctx->cpu_sample_hz);
    if (gap < 1) gap = 1;
    return gap;
}

static struct icallpath_context*
get_frame_path(struct profile_context* context, lua_State* co, lua_Debug* far, struct icallpath_context* pre_path, struct call_frame* frame) {
    if (!context->callpath) {
        struct callpath_node* node = callpath_node_create();
        node->name = "root";
        node->source = "root";
        node->call_count = 1;
        context->callpath = icallpath_create(0, node);
    }
    if (!pre_path) {
        pre_path = context->callpath;
    }

    struct call_frame* cur_cf = frame;
    uint64_t k = (uint64_t)((uintptr_t)cur_cf->prototype);
    struct icallpath_context* cur_path = icallpath_get_child(pre_path, k);
    if (!cur_path) {
        struct callpath_node* path_parent = (struct callpath_node*)icallpath_getvalue(pre_path);
        struct callpath_node* node = callpath_node_create();

        node->parent = path_parent;
        node->depth = path_parent->depth + 1;
        node->last_ret_time = 0;
        node->real_cost = 0;
        node->call_count = 0;
        cur_path = icallpath_add_child(pre_path, k, node);
    }

    struct callpath_node* cur_node = (struct callpath_node*)icallpath_getvalue(cur_path);
    if (cur_node->name == NULL) {
        uint64_t sym_key = (uint64_t)((uintptr_t)cur_cf->prototype);
        struct symbol_info* si = (struct symbol_info*)imap_query(context->symbol_map, sym_key);
        if (!si) {
            lua_getinfo(co, "nSl", far);
            const char* name = far->name;
            int line = far->linedefined;
            const char* source = far->source;
            char flag = far->what[0];
            if (flag == 'C') {
                lua_Debug ar2;
                int i=0;
                int ret = 0;
                do {
                    i++;
                    ret = lua_getstack(co, i, &ar2);
                    if(ret) {
                        lua_getinfo(co, "Sl", &ar2);
                        if(ar2.what[0] != 'C') {
                            line = ar2.currentline;
                            source = ar2.source;
                            break;
                        }
                    }
                } while(ret);
            }
            si = (struct symbol_info*)pmalloc(sizeof(struct symbol_info));
            si->name = pstrdup(name ? name : "null");
            si->source = pstrdup(source ? source : "null");
            si->line = line;
            imap_set(context->symbol_map, sym_key, si);
        }
        cur_node->name = si->name;
        cur_node->source = si->source;
        cur_node->line = si->line;
    }
    
    return cur_path;
}

// 按路径更新节点（仅更新当前节点的 self 计数，父链累计推迟到 dump 聚合）
static inline void _mem_update_on_path(struct callpath_node* node,
    size_t alloc_bytes, uint64_t alloc_times, size_t free_bytes, uint64_t free_times, uint64_t realloc_times) {
    if (!node) return;
    if (alloc_bytes) node->alloc_bytes += alloc_bytes;
    if (alloc_times) node->alloc_times += alloc_times;
    if (free_bytes) node->free_bytes += free_bytes;
    if (free_times) node->free_times += free_times;
    if (realloc_times) node->realloc_times += realloc_times;
}

// 取当前栈的叶子节点
static inline struct callpath_node* _current_leaf_node(struct profile_context* context) {
    struct call_state* cs = context->cur_cs;
    if (!cs) return NULL;
    struct call_frame* leaf = cur_callframe(cs);
    if (!leaf || !leaf->path) return NULL;
    return (struct callpath_node*)icallpath_getvalue(leaf->path);
}

/*
获取各种类型函数的 prototype，包括 LUA_VLCL、LUA_VCCL、LUA_VLCF。   
如果没有正确获取 prototype，那么像 tonumber 和 print 这类 LUA_VLCF 使用栈上的函数指针来充当 prototype,
会导致同一层的这类函数被合并为同一个节点，比如下面的代码，最终 print 会错误的合并到 tonumber 的节点中，显示为
2 次 tonumber 调用。 

验证代码:
local function test1()
    local profile = require "profile"
    local json = require "cjson"
    profile.start()
    tonumber("123")    
    print("111")
    local result = profile.stop()
    skynet.error("test1:", json.encode(result))
end
test1()

结果可用 https://jsongrid.com/ 查看
*/
static const void* _get_prototype(lua_State* L, lua_Debug* ar) {
    const void* proto = NULL;
    if (ar->i_ci && ar->i_ci->func.p) {
        const TValue* tv = s2v(ar->i_ci->func.p);
        if (ttislcf(tv)) {
            // LUA_VLCF：轻量 C 函数，直接取 c 函数指针
            proto = (const void*)fvalue(tv);
        } else if (ttisclosure(tv)) {
            const Closure* cl = clvalue(tv);
            if (cl->c.tt == LUA_VLCL) {
                proto = (const void*)cl->l.p;   // Lua 函数 → Proto*
            } else if (cl->c.tt == LUA_VCCL) {
                proto = (const void*)cl->c.f; // C 闭包 → lua_CFunction
            }
        }
    }
    if (!proto) {
        // 兜底：仍可能遇到少数拿不到 TValue 的情况
        lua_getinfo(L, "f", ar);
        proto = lua_topointer(L, -1);
        lua_pop(L, 1);
        printf("get prototype by getinfo: %p\n", proto);
    }
    return proto;
}

// hook alloc/free/realloc 事件
static void*
_hook_alloc(void *ud, void *ptr, size_t _osize, size_t _nsize) {   
    struct profile_context* context = (struct profile_context*)ud;
    void* alloc_ret = context->last_alloc_f(context->last_alloc_ud, ptr, _osize, _nsize);
    if (context->running_in_hook || !context->is_ready) {
        return alloc_ret;
    }

    size_t oldsize = (ptr == NULL) ? 0 : _osize;
    size_t newsize = _nsize;

    if (oldsize == 0 && newsize > 0) {
        // alloc

        // 更新节点
        struct callpath_node* leaf = _current_leaf_node(context);
        if (leaf) _mem_update_on_path(leaf, newsize, 1, 0, 0, 0);

        // 创建映射
        struct alloc_node* an = (struct alloc_node*)imap_query(context->alloc_map, (uint64_t)(uintptr_t)alloc_ret);
        if (an == NULL) an = alloc_node_create();
        an->live_bytes = newsize;
        an->path = _current_leaf_node(context);
        imap_set(context->alloc_map, (uint64_t)(uintptr_t)alloc_ret, an);

    } else if (oldsize > 0 && newsize == 0) {
        // free
        
        struct alloc_node* an = (struct alloc_node*)imap_remove(context->alloc_map, (uint64_t)(uintptr_t)ptr);
        if (an) {
            // 更新节点
            size_t sub_bytes = an->live_bytes; 
            uint64_t sub_times = (an->live_bytes ? 1 : 0);
            if (an->path && an->live_bytes > 0) {
                _mem_update_on_path(an->path, 0, 0, sub_bytes, sub_times, 0);
            }
            pfree(an);
            an = NULL;
        }

    } else if (oldsize > 0 && newsize > 0) {
        // realloc
        
        // 参照 gperftools 的逻辑，realloc 拆分为 free 和 alloc 两个事件，但此处为了反映 gc 的压力，不增加 alloc_times 和 free_times。
        // 1、旧 node，free_bytes 加上 oldsize；
        // 2、新 node，alloc_bytes 加上 newsize，realloc_times 加 1；

        // 旧路径
        struct alloc_node* old_an = (struct alloc_node*)imap_query(context->alloc_map, (uint64_t)(uintptr_t)ptr);
        if (old_an && old_an->path) {
            _mem_update_on_path(old_an->path, 0, 0, oldsize, 0, 0);
        }

        // 新路径
        struct callpath_node* leaf = _current_leaf_node(context);
        if (leaf) _mem_update_on_path(leaf, newsize, 0, 0, 0, 1);

        // 更新映射（搬移或原地）
        if (alloc_ret != ptr && alloc_ret != NULL) {
            struct alloc_node* an = (struct alloc_node*)imap_remove(context->alloc_map, (uint64_t)(uintptr_t)ptr);
            if (!an) an = alloc_node_create();
            an->live_bytes = newsize;
            an->path = _current_leaf_node(context);
            imap_set(context->alloc_map, (uint64_t)(uintptr_t)alloc_ret, an);
        } else {
            struct alloc_node* an = (struct alloc_node*)imap_query(context->alloc_map, (uint64_t)(uintptr_t)ptr);
            bool exists = (an != NULL);
            if (!exists) an = alloc_node_create();
            an->live_bytes = newsize;
            an->path = _current_leaf_node(context);
            if (!exists) imap_set(context->alloc_map, (uint64_t)(uintptr_t)ptr, an);
        }
    }

    return alloc_ret;
}

// hook call/ret 事件
static void
_hook_call(lua_State* L, lua_Debug* far) {
    uint64_t begin_time = get_mono_ns();

    struct profile_context* context = get_profile_context(L);
    if (context == NULL) {
        printf("resolve hook fail, profile not started\n");
        return;
    }
    if(!context->is_ready) {
        return;
    }

    context->running_in_hook = true;

    int event = far->event;

    struct call_state* cs = context->cur_cs;
    if (!context->cur_cs || context->cur_cs->co != L) {
        uint64_t key = (uint64_t)((uintptr_t)L);
        cs = imap_query(context->cs_map, key);
        if (cs == NULL) {
            cs = (struct call_state*)pmalloc(sizeof(struct call_state) + sizeof(struct call_frame)*MAX_CALL_SIZE);
            cs->co = L; 
            cs->top = 0;
            cs->leave_time = 0;
            imap_set(context->cs_map, key, cs);
        }

        if (context->cur_cs) {
            context->cur_cs->leave_time = begin_time;
        }
        context->cur_cs = cs;
    }
    if (cs->leave_time > 0) {
        assert(begin_time >= cs->leave_time);
        uint64_t co_cost = begin_time - cs->leave_time;

        for (int i = 0; i < cs->top; i++) {
            cs->call_list[i].co_cost += co_cost;
        }
        cs->leave_time = 0;
    }
    assert(cs->co == L);

    if (event == LUA_HOOKCALL || event == LUA_HOOKTAILCALL) {
        struct icallpath_context* pre_callpath = NULL;
        struct call_frame* pre_frame = cur_callframe(cs);
        if (pre_frame) {
            pre_callpath = pre_frame->path;
        }
        struct call_frame* frame = push_callframe(cs);
        frame->tail = (event == LUA_HOOKTAILCALL);
        frame->co_cost = 0;
        frame->prototype = _get_prototype(L, far);    
        frame->path = get_frame_path(context, L, far, pre_callpath, frame);
        if (frame->path) {
            struct callpath_node* node = (struct callpath_node*)icallpath_getvalue(frame->path);
            ++node->call_count;
        }
        frame->call_time = get_mono_ns();

    } else if (event == LUA_HOOKRET) {
        if (cs->top <= 0) {
            context->running_in_hook = false;
            return;
        }
        bool tail_call = false;
        do {
            struct call_frame* cur_frame = pop_callframe(cs);
            struct callpath_node* cur_path = (struct callpath_node*)icallpath_getvalue(cur_frame->path);
            uint64_t total_cost = begin_time - cur_frame->call_time;
            uint64_t real_cost = total_cost - cur_frame->co_cost;
            assert(begin_time >= cur_frame->call_time && total_cost >= cur_frame->co_cost);
            cur_path->last_ret_time = begin_time;
            cur_path->real_cost += real_cost;

            struct call_frame* pre_frame = cur_callframe(cs);
            tail_call = pre_frame ? cur_frame->tail : false;
        } while(tail_call);
    }

    context->profile_cost_ns += (get_mono_ns() - begin_time);
    context->running_in_hook = false;
}

static void _dump_call_path(struct icallpath_context* path, struct dump_call_path_arg* arg);

static void _dump_call_path_child(uint64_t key, void* value, void* ud) {
    struct dump_call_path_arg* arg = (struct dump_call_path_arg*)ud;
    _dump_call_path((struct icallpath_context*)value, arg);
    lua_seti(arg->L, -2, ++arg->index);
}

static void _dump_call_path(struct icallpath_context* path, struct dump_call_path_arg* arg) {
    lua_checkstack(arg->L, 3);
    lua_newtable(arg->L);

    // 递归获取所有子节点的指标和
    struct dump_call_path_arg child_arg;
    _init_dump_call_path_arg(&child_arg, arg->pcontext, arg->L);
    if (icallpath_children_size(path) > 0) {
        lua_newtable(arg->L);
        icallpath_dump_children(path, _dump_call_path_child, &child_arg);
        lua_setfield(arg->L, -2, "children");
    }

    struct callpath_node* node = (struct callpath_node*)icallpath_getvalue(path);

    // 本节点的聚合指标=本节点指标+所有子节点的指标
    uint64_t alloc_bytes_incl = node->alloc_bytes + child_arg.alloc_bytes_sum;
    uint64_t free_bytes_incl = node->free_bytes + child_arg.free_bytes_sum;
    uint64_t alloc_times_incl = node->alloc_times + child_arg.alloc_times_sum;
    uint64_t free_times_incl = node->free_times + child_arg.free_times_sum;
    uint64_t realloc_times_incl = node->realloc_times + child_arg.realloc_times_sum;

    // 本节点的其他指标
    uint64_t real_cost = node->real_cost;
    uint64_t call_count = node->call_count;
    uint64_t inuse_bytes = (alloc_bytes_incl >= free_bytes_incl ? alloc_bytes_incl - free_bytes_incl : 9999999999);

    // 累加到父节点
    arg->alloc_bytes_sum += alloc_bytes_incl;
    arg->free_bytes_sum += free_bytes_incl;
    arg->alloc_times_sum += alloc_times_incl;
    arg->free_times_sum += free_times_incl;
    arg->realloc_times_sum += realloc_times_incl;

    // 导出本节点的聚合指标
    char name[512] = {0};
    snprintf(name, sizeof(name)-1, "%s %s:%d", node->name ? node->name : "", node->source ? node->source : "", node->line);
    lua_pushstring(arg->L, name);
    lua_setfield(arg->L, -2, "name");

    lua_pushinteger(arg->L, node->last_ret_time);
    lua_setfield(arg->L, -2, "last_ret_time");

    if (arg->pcontext->cpu_mode == MODE_PROFILE) {
        lua_pushinteger(arg->L, call_count);
        lua_setfield(arg->L, -2, "call_count");

        lua_pushinteger(arg->L, real_cost);
        lua_setfield(arg->L, -2, "cpu_cost_ns");

        uint64_t parent_real_cost = 0;
        if (node->parent) {
            parent_real_cost = node->parent->real_cost;
        }
        double percent = parent_real_cost > 0 ? ((double)real_cost / parent_real_cost * 100.0) : 100;
        char percent_str[32] = {0};
        snprintf(percent_str, sizeof(percent_str)-1, "%.2f", percent);
        lua_pushstring(arg->L, percent_str);
        lua_setfield(arg->L, -2, "cpu_cost_percent");

    }

    if (arg->pcontext->mem_mode == MODE_PROFILE) {
        lua_pushinteger(arg->L, (lua_Integer)alloc_bytes_incl);
        lua_setfield(arg->L, -2, "alloc_bytes");

        lua_pushinteger(arg->L, (lua_Integer)free_bytes_incl);
        lua_setfield(arg->L, -2, "free_bytes");

        lua_pushinteger(arg->L, (lua_Integer)alloc_times_incl);
        lua_setfield(arg->L, -2, "alloc_times");

        lua_pushinteger(arg->L, (lua_Integer)free_times_incl);
        lua_setfield(arg->L, -2, "free_times");

        lua_pushinteger(arg->L, (lua_Integer)realloc_times_incl);
        lua_setfield(arg->L, -2, "realloc_times");

        lua_pushinteger(arg->L, (lua_Integer)inuse_bytes);
        lua_setfield(arg->L, -2, "inuse_bytes");
    }

    if (path == arg->pcontext->callpath) {
        lua_pushinteger(arg->L, arg->pcontext->profile_cost_ns);
        lua_setfield(arg->L, -2, "profile_cost_ns");
    }
}

static void sum_root_stat(uint64_t key, void* value, void* ud) {
    struct sum_root_stat_arg* arg = (struct sum_root_stat_arg*)ud;
    struct icallpath_context* path = (struct icallpath_context*)value;
    struct callpath_node* node = (struct callpath_node*)icallpath_getvalue(path);
    arg->real_cost_sum += node->real_cost;
}

static void update_root_stat(struct profile_context* pcontext, lua_State* L) {
    struct icallpath_context* path = pcontext->callpath;
    if (!path) return;
    struct callpath_node* root = (struct callpath_node*)icallpath_getvalue(path);
    if (!root) return;

    root->last_ret_time = get_mono_ns();
    if (icallpath_children_size(path) > 0) {
        struct sum_root_stat_arg arg;
        _init_sum_root_stat_arg(&arg);
        icallpath_dump_children(path, sum_root_stat, &arg);
        root->real_cost = arg.real_cost_sum;
    }
}

static void dump_call_path(struct profile_context* pcontext, lua_State* L) {
    struct dump_call_path_arg arg;
    _init_dump_call_path_arg(&arg, pcontext, L);
    _dump_call_path(pcontext->callpath, &arg);
}

static int 
get_all_coroutines(lua_State* L, lua_State** result, int maxsize) {
    int i = 0;
    struct global_State* lG = L->l_G;
    result[i++] = lG->mainthread;

    struct GCObject* obj = lG->allgc;
    while (obj && i < maxsize) {
        if (obj->tt == LUA_TTHREAD) {
            result[i++] = gco2th(obj);
        }
        obj = obj->next;
    }
    return i;
}

static int _stop_gc_if_need(lua_State* L) {
    // stop gc before set hook
    int gc_was_running = lua_gc(L, LUA_GCISRUNNING, 0);
    if (gc_was_running) { lua_gc(L, LUA_GCSTOP, 0); }
    return gc_was_running;
}

static void _restart_gc_if_need(lua_State* L, int gc_was_running) {
    if (gc_was_running) { lua_gc(L, LUA_GCRESTART, 0); }
}

static void
_set_hook_all_co(lua_State* L) {
    struct profile_context* ctx = get_profile_context(L);
    if (!ctx) {
        printf("hook all co fail, profile not started\n");
        return;
    }
    if (ctx->cpu_mode == MODE_OFF) {
        return;
    }
    // stop gc before set hook
    int gc_was_running = _stop_gc_if_need(L);
    lua_State* states[MAX_CO_SIZE] = {0};
    int i = get_all_coroutines(L, states, MAX_CO_SIZE);
    for (i = i - 1; i >= 0; i--) {
        if (ctx && ctx->cpu_mode == MODE_PROFILE) {
            // profiling (full call/ret)
            lua_sethook(states[i], _hook_call, LUA_MASKCALL | LUA_MASKRET, 0);
        }
    }
    _restart_gc_if_need(L, gc_was_running);
}

static void 
_unset_hook_all_co(lua_State* L) {
    struct profile_context* ctx = get_profile_context(L);
    if (!ctx) {
        printf("unhook all co fail, profile not started\n");
        return;
    }
    if (ctx->cpu_mode == MODE_OFF) {
        return;
    }    
    // stop gc before unset hook
    int gc_was_running = _stop_gc_if_need(L); 
    lua_State* states[MAX_CO_SIZE] = {0};
    int sz = get_all_coroutines(L, states, MAX_CO_SIZE);
    int i;
    for (i = sz - 1; i >= 0; i--) {
        lua_sethook(states[i], NULL, 0, 0);
    }
    _restart_gc_if_need(L, gc_was_running);
}

static int
_lstart(lua_State* L) {
    struct profile_context* context = get_profile_context(L);
    if (context != NULL) {
        printf("start fail, profile already started\n");
        return 0;
    }

    // parse options: start([opts]), opts is a table
    int cpu_mode = MODE_PROFILE; // default profile
    int mem_mode = MODE_PROFILE; // default profile
    int cpu_sample_hz = DEFAULT_CPU_SAMPLE_HZ;
    bool read_ok = read_arg(L, &cpu_mode, &mem_mode, &cpu_sample_hz);
    if (!read_ok) {
        printf("start fail, invalid options\n");
        return 0;
    }

    lua_gc(L, LUA_GCCOLLECT, 0);  // full gc

    context = profile_create();
    context->running_in_hook = true;
    context->start_time = get_mono_ns();
    context->is_ready = true;
    context->cpu_mode = cpu_mode;
    context->mem_mode = mem_mode;
    context->cpu_sample_hz = cpu_sample_hz;
    // seed rng with time xor state pointer
    context->rng_state = get_mono_ns() ^ (uint64_t)(uintptr_t)context;
    
    context->last_alloc_f = lua_getallocf(L, &context->last_alloc_ud);
    if (mem_mode != MODE_OFF) {
        lua_setallocf(L, _hook_alloc, context);
    }
    set_profile_context(L, context);

    if (cpu_mode == MODE_SAMPLE) {
        g_prof_current_L = L;
        lua_prof_set_cb_n(_on_prof_trap_n);        
        if (start_thread_timer_hz(cpu_sample_hz) != 0) {
            printf("start thread timer fail\n");
        }
    } else if (cpu_mode == MODE_PROFILE || mem_mode != MODE_OFF) {
        _set_hook_all_co(L);
    }
    
    context->running_in_hook = false;
    printf("luaprofile started, cpu_mode = %d, mem_mode = %d, cpu_sample_hz = %d, last_alloc_ud = %p\n", context->cpu_mode, context->mem_mode, context->cpu_sample_hz, context->last_alloc_ud);    
    return 0;
}

static int
_lstop(lua_State* L) {
    struct profile_context* context = get_profile_context(L);
    if (context == NULL) {
        printf("stop fail, profile not started\n");
        return 0;
    }
    
    context->running_in_hook = true;
    context->is_ready = false;
    lua_setallocf(L, context->last_alloc_f, context->last_alloc_ud);
    _unset_hook_all_co(L);
    unset_profile_context(L);
    profile_free(context);
    context = NULL;
    // stop sampler
    if (g_prof_timerid) {
        stop_thread_timer();
    }
    g_prof_current_L = NULL;
    printf("luaprofile stopped\n");
    return 0;
}

static int
_lmark(lua_State* L) {
    struct profile_context* context = get_profile_context(L);
    if (context == NULL) {
        printf("mark fail, profile not started\n");
        return 0;
    }
    lua_State* co = lua_tothread(L, 1);
    if(co == NULL) {
        co = L;
    }
    if(context->is_ready) {
        lua_sethook(co, _hook_call, LUA_MASKCALL | LUA_MASKRET, 0);
    }
    g_prof_current_L = co;
    lua_pushboolean(L, context->is_ready);
    return 1;
}

static int
_lunmark(lua_State* L) {
    struct profile_context* context = get_profile_context(L);
    if (context == NULL) {
        printf("unmark fail, profile not started\n");
        return 0;
    }
    lua_State* co = lua_tothread(L, 1);
    if(co == NULL) {
        co = L;
    }
    lua_sethook(co, NULL, 0, 0);
    return 0;
}

static int
_ldump(lua_State* L) {
    struct profile_context* context = get_profile_context(L);
    if (context) {
        /* fold C stack TLS buffer into c_sample_map using dladdr */
        fold_c_tls_samples(context);
        // full gc
        lua_gc(L, LUA_GCCOLLECT, 0);

        // stop gc before dump
        int gc_was_running = _stop_gc_if_need(L); 
        context->running_in_hook = true;
        uint64_t cur_time = get_mono_ns();
        uint64_t profile_time = cur_time - context->start_time;
        lua_pushinteger(L, profile_time);

        if (context->cpu_mode == MODE_SAMPLE) {
            /* dump Lua folded stacks */
            push_lua_folded_samples(L, context);
            /* also write C stacks (names already resolved in keys) to a file */
            write_c_samples_file(context, "cpu-c-samples.txt");
            /* and emit raw addresses for offline symbolization */
            write_c_samples_raw("cpu-c-samples.raw");
            /* and emit legacy pprof file for pprof toolchain */
            write_c_profile_pprof(context, "cpu-c-profile.pprof");
            /* now it's safe to clear TLS buffer once */
            clear_c_tls_samples();
        } else {
            // tracing dump
            if (context->callpath) {
                update_root_stat(context, L);
                dump_call_path(context, L);
            } else {
                lua_newtable(L);
            }
        }
        context->running_in_hook = false;
        _restart_gc_if_need(L, gc_was_running);
        return 2;
    }
    return 0;
}

static int _lget_mono_ns(lua_State* L) {
    lua_pushinteger(L, get_mono_ns());
    return 1;
}

// -------- CPU sampling (timer + trap callback) --------


// Construct folded key and ensure symbol info
/* Safe stack sampler: does NOT call Lua debug API; walks CallInfo chain */
static void record_lua_sample_weight(lua_State* L, unsigned int weight) {
    struct profile_context* context = get_profile_context(L);
    if (!context || context->cpu_mode != MODE_SAMPLE || context->running_in_hook) return;
    context->running_in_hook = true;
    char keybuf[4096];
    size_t kp = 0;
    const void* protos[MAX_SAMPLE_DEPTH];
    int nframes = 0;

    /* 1) 遍历 CallInfo 链，采集自叶到根的 Proto/函数指针，并为每一帧写入符号信息（source/line，占位名） */
    {
        CallInfo* ci = L->ci;
        while (ci && nframes < MAX_SAMPLE_DEPTH) {
            const TValue* tv = s2v(ci->func.p);
            const void* proto = NULL;
            const Proto* lua_p = NULL;

            if (ttislcf(tv)) {
                proto = (const void*)fvalue(tv); /* 轻量 C 函数 */
            } else if (ttisclosure(tv)) {
                const Closure* cl = clvalue(tv);
                if (cl->c.tt == LUA_VLCL) {
                    lua_p = cl->l.p;            /* Lua 函数 */
                    proto = (const void*)lua_p;
                } else if (cl->c.tt == LUA_VCCL) {
                    proto = (const void*)cl->c.f; /* C 闭包 */
                }
            }

            if (proto) {
                uint64_t sym_key = (uint64_t)((uintptr_t)proto);
                struct symbol_info* si = (struct symbol_info*)imap_query(context->symbol_map, sym_key);
                if (!si) {
                    si = (struct symbol_info*)pmalloc(sizeof(struct symbol_info));
                    if (lua_p) {
                        const char* src = lua_p->source ? getstr(lua_p->source) : "null";
                        si->name = pstrdup("(lua)");
                        si->source = pstrdup(src ? src : "null");
                        si->line = lua_p->linedefined;
                    } else {
                        si->name = pstrdup("(C)");
                        si->source = pstrdup("(C)");
                        si->line = -1;
                    }
                    imap_set(context->symbol_map, sym_key, si);
                }
                protos[nframes++] = proto;
            }
            ci = ci->previous;
        }
    }

    /* 2) 为每一帧按需补齐函数名：仅当缓存里是占位名时，才调用 debug API 获取 name */
    {
        lua_Debug ar;
        for (int lvl = 0; lvl < nframes; ++lvl) {
            uint64_t sk = (uint64_t)((uintptr_t)protos[lvl]);
            struct symbol_info* si = (struct symbol_info*)imap_query(context->symbol_map, sk);
            if (si && si->name && si->name[0] != '(') {
                continue; /* 缓存已有人类可读的函数名，跳过 */
            }
            if (!lua_getstack(L, lvl, &ar)) break;
            int ok = lua_getinfo(L, "n", &ar);
            if (ok && ar.name && ar.name[0]) {
                if (!si) {
                    si = (struct symbol_info*)pmalloc(sizeof(struct symbol_info));
                    si->source = pstrdup("unknown");
                    si->line = -1;
                    imap_set(context->symbol_map, sk, si);
                } else if (si->name) {
                    pfree(si->name);
                }
                si->name = pstrdup(ar.name);
            }
        }
    }

    // Build folded key as root->...->leaf (FlameGraph expected order)

    for (int idx = nframes - 1; idx >= 0; --idx) {
        char token[32];
        int n = snprintf(token, sizeof(token), "%p", protos[idx]);
        if (n > 0) {
            if (kp + (size_t)n + 1 < sizeof(keybuf)) {
                if (kp > 0) keybuf[kp++] = ';';
                memcpy(keybuf + kp, token, (size_t)n);
                kp += (size_t)n;
            } else break;
        }
    }
    keybuf[kp] = '\0';
    if (kp > 0) {
        uint64_t* cnt = (uint64_t*)smap_get(context->sample_map, keybuf);
        if (!cnt) {
            cnt = (uint64_t*)pmalloc(sizeof(uint64_t));
            *cnt = 0;
            smap_set(context->sample_map, keybuf, cnt);
        }
        (*cnt) += (uint64_t)(weight ? weight : 1);
    }
    context->running_in_hook = false;
}

static void _on_prof_trap_n(lua_State* L, unsigned int n) {
    record_lua_sample_weight(L, n);
}

// sleep(seconds): 使用 POSIX nanosleep，支持小数秒，自动处理被信号打断
static int _lsleep(lua_State* L) {
    lua_Number sec = luaL_checknumber(L, 1);
    if (sec < 0) sec = 0;

    lua_Number integral = 0;
    lua_Number frac = modf(sec, &integral);

    struct timespec req;
    req.tv_sec = (time_t)integral;
    long nsec = (long)(frac * 1000000000.0);
    if (nsec < 0) nsec = 0;
    if (nsec >= 1000000000L) {
        req.tv_sec += 1;
        nsec -= 1000000000L;
    }
    req.tv_nsec = nsec;

    while (nanosleep(&req, &req) == -1 && errno == EINTR) {
        // 被信号中断则继续睡剩余时间
    }
    return 0;
}

int
luaopen_luaprofilec(lua_State* L) {
    luaL_checkversion(L);
     luaL_Reg l[] = {
        {"start", _lstart},
        {"stop", _lstop},
        {"mark", _lmark},
        {"unmark", _lunmark},
        {"dump", _ldump},
        {"getnanosec", _lget_mono_ns},
        {"sleep", _lsleep},
        {NULL, NULL},
    };
    luaL_newlib(L, l);
    return 1;
}