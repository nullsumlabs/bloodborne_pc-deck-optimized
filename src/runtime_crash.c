/* bbport: see runtime_crash.h and docs/CRASH_RECORDER_PLAN.md.
 *
 * Everything here runs either rarely (a mapping event, a heap sample every 30 s) or once, in a
 * signal handler at the end of the process. The dump writer therefore uses write(2) only: no
 * stdio, no malloc, no dladdr, no locks. Sections are flushed as they are produced and the file
 * ends with END, so a handler that dies half way still leaves something readable.
 */
#define _GNU_SOURCE
#include "runtime_crash.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#ifndef _WIN32
#include <signal.h>
#include <sys/syscall.h> /* SYS_gettid: the note path must not pay for getpid() */
#include <ucontext.h>

#define CAP (1u << 15)
#define TABLE_SLOTS 64u
#define SCAN_BYTES 2048u
#define MAX_TARGETS 16u
#define MAPS_LIMIT (1u << 20)

typedef struct {
    uint64_t ns;
    uint32_t kind, tid;
    uint64_t a, b;
} Entry;
_Static_assert(sizeof(Entry) == 32, "ring entry");

typedef struct {
    uint64_t caller[3];
    uint64_t bytes;
    uint32_t count, fails;
} Caller;

/* 0 off, 1 on; -1 until runtime_crash_init has read the environment. */
static int enabled = -1;
static uintptr_t image_base;
static uint64_t image_size;

static Entry ring[CAP];
static _Atomic uint64_t ring_head;
static Caller table[TABLE_SLOTS];
static _Atomic uint32_t table_used;

static uint64_t start_ns;
static time_t start_wall;
static char path_buf[512];
static char header[2048];
static size_t header_len;

static uint64_t heap_max_in_use, heap_max_system;

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* Cached per thread: the call itself is a system call, and this is read on the note path. */
static uint32_t thread_id(void) {
#ifdef SYS_gettid
    static __thread uint32_t cached;
    if (!cached) cached = (uint32_t)syscall(SYS_gettid);
    return cached;
#else
    return (uint32_t)getpid();
#endif
}

static void copy_str(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* ---- init: the environment, and the header the report starts with ------------------------------- */

static void hput(const char *s) {
    size_t n = strlen(s);
    if (header_len + n >= sizeof header) n = sizeof header - 1 - header_len;
    memcpy(header + header_len, s, n);
    header_len += n;
}
static void hput_n(const char *s, size_t n) {
    if (header_len + n >= sizeof header) n = sizeof header - 1 - header_len;
    memcpy(header + header_len, s, n);
    header_len += n;
}
static void hput_dec(uint64_t v) {
    char b[24];
    size_t i = sizeof b;
    do {
        b[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    hput_n(b + i, sizeof b - i);
}
static void hput_env(const char *name) {
    const char *value = getenv(name);
    hput(name);
    hput("=");
    hput(value ? value : "(unset)");
    hput("\n");
}

void runtime_crash_init(void) {
    if (enabled >= 0) return;
    const char *mode = getenv("BB_CRASH_REPORT");
    enabled = !(mode && (!strcmp(mode, "0") || !strcmp(mode, "off")));
    if (!enabled) return;
    start_ns = now_ns();
    start_wall = time(NULL);
    {
        const char *path = getenv("BB_CRASH_LOG");
        if (path && *path) copy_str(path_buf, sizeof path_buf, path);
        else copy_str(path_buf, sizeof path_buf, "crash-report.txt");
    }
    /* Touched once here so that neither the note path nor the handler takes a page fault on it. */
    memset(ring, 0, sizeof ring);
    memset(table, 0, sizeof table);

    hput("bbport crash report\n");
    hput("build=");
    hput(__DATE__);
    hput(" ");
    hput(__TIME__);
    hput("\npid=");
    hput_dec((uint64_t)getpid());
    hput("\nstarted=");
    hput_dec((uint64_t)start_wall);
    hput("\nreport=");
    hput(path_buf);
    hput("\n");
    hput_env("BB_CRASH_REPORT");
    hput_env("BB_CRASH_HEAP_SECONDS");
    hput_env("BB_FPS");
    hput_env("BB_RENDER_RES");
    hput_env("BB_OUTPUT_RES");
    hput_env("BB_DMEM_MB");
    hput_env("BB_PC_MODEL");
    hput_env("BB_GUEST_IN_PLACE");
    hput_env("BB_UFFD");
    hput_env("BB_WRITE_LOG");
    hput_env("BB_FRAME_STATS");
    hput_env("BB_ONLINE");
    /* The kernel, once: a driver or kernel change is part of the story of a crash weeks later. */
    {
        int fd = open("/proc/version", O_RDONLY | O_CLOEXEC);
        char buf[256];
        ssize_t n = fd >= 0 ? read(fd, buf, sizeof buf) : -1;
        if (fd >= 0) close(fd);
        if (n > 0) {
            hput("kernel=");
            hput_n(buf, (size_t)n);
            if (buf[n - 1] != '\n') hput("\n");
        }
    }
}

int runtime_crash_on(void) {
    if (enabled < 0) runtime_crash_init();
    return enabled == 1;
}

void runtime_crash_set_image(uintptr_t base, uint64_t size) {
    image_base = base;
    image_size = size;
}

static int in_image(uintptr_t v) {
    return image_base && v >= image_base && v - image_base < image_size;
}

/* ---- recording ---------------------------------------------------------------------------------- */

void runtime_crash_note(unsigned kind, uint64_t a, uint64_t b) {
    if (!runtime_crash_on()) return;
    const uint64_t slot = atomic_fetch_add_explicit(&ring_head, 1, memory_order_relaxed) % CAP;
    ring[slot].ns = now_ns();
    ring[slot].kind = kind;
    ring[slot].tid = thread_id();
    ring[slot].a = a;
    ring[slot].b = b;
}

static void table_note(const uint64_t callers[3], uint64_t bytes, int refused) {
    const uint32_t used = atomic_load_explicit(&table_used, memory_order_relaxed);
    for (uint32_t i = 0; i < used && i < TABLE_SLOTS; ++i) {
        if (!memcmp(table[i].caller, callers, sizeof table[i].caller)) {
            __atomic_add_fetch(&table[i].bytes, bytes, __ATOMIC_RELAXED);
            __atomic_add_fetch(&table[i].count, 1u, __ATOMIC_RELAXED);
            if (refused) __atomic_add_fetch(&table[i].fails, 1u, __ATOMIC_RELAXED);
            return;
        }
    }
    const uint32_t slot = atomic_fetch_add_explicit(&table_used, 1, memory_order_relaxed);
    if (slot >= TABLE_SLOTS) return;
    memcpy(table[slot].caller, callers, sizeof table[slot].caller);
    table[slot].bytes = bytes;
    table[slot].count = 1;
    table[slot].fails = refused ? 1 : 0;
}

void runtime_crash_note_map(unsigned kind, uint64_t size, int64_t result, uint64_t address,
                            const uint64_t callers[3]) {
    if (!runtime_crash_on()) return;
    const int refused = result != 0;
    unsigned event = kind;
    if (kind == RuntimeCrashMap) event = refused ? RuntimeCrashMapFail : RuntimeCrashMap;
    if (kind == RuntimeCrashFlex) event = refused ? RuntimeCrashFlexFail : RuntimeCrashFlex;
    runtime_crash_note(event, size, refused ? (uint64_t)result : address);
    table_note(callers, size, refused);
}

void runtime_crash_heap_note(uint64_t in_use, uint64_t from_system, uint64_t max_in_use,
                             uint64_t max_from_system) {
    if (!runtime_crash_on()) return;
    heap_max_in_use = max_in_use;
    heap_max_system = max_from_system;
    runtime_crash_note(RuntimeCrashHeap, in_use, from_system);
}

/* The game's libc heap, from the same thread that reads the pad (as the frame statistics do). The
 * stub at libc.prx+0x1f8c0 is looked at with a safe read: before that module is loaded, or with
 * another libc, nothing here runs. */
typedef struct {
    uint16_t size, version;
    uint32_t reserved;
    uint64_t max_system, system, max_in_use, in_use;
} MallocManagedSize;
typedef int (*MallocStats)(MallocManagedSize *) __attribute__((sysv_abi));

static int heap_period_seconds(void) {
    static int period = -1;
    if (period < 0) {
        const char *env = getenv("BB_CRASH_HEAP_SECONDS");
        period = env && *env ? atoi(env) : 30;
        if (period < 1) period = 1;
        else if (period > 3600) period = 3600;
    }
    return period;
}

void runtime_crash_heap_sample(void) {
    if (!runtime_crash_on() || !image_base) return;
    static uint64_t last;
    const uint64_t now = now_ns();
    if (last && now - last < (uint64_t)heap_period_seconds() * 1000000000ull) return;
    last = now;
    const uintptr_t stub = image_base + 0x56e0000ull + 0x1f8c0ull;
    unsigned char code[2];
    struct iovec local = {code, sizeof code};
    struct iovec remote = {(void *)stub, sizeof code};
    if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != (ssize_t)sizeof code) return;
    if (code[0] != 0xff || code[1] != 0x25) return; /* another libc */
    MallocStats stats = (MallocStats)stub;
    MallocManagedSize m = {sizeof m, 1, 0, 0, 0, 0, 0};
    if (stats(&m) != 0) return;
    runtime_crash_heap_note(m.in_use, m.system, m.max_in_use, m.max_system);
}

/* ---- the writer: write(2) only, flushed as it goes ---------------------------------------------- */

static char out_buf[65536];
static size_t out_len;
static int out_fd = 2;

static void out_flush(void) {
    size_t off = 0;
    while (off < out_len) {
        const ssize_t n = write(out_fd, out_buf + off, out_len - off);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    out_len = 0;
}
static void out_put(const char *s, size_t n) {
    while (n) {
        if (out_len == sizeof out_buf) out_flush();
        size_t room = sizeof out_buf - out_len;
        const size_t chunk = n < room ? n : room;
        memcpy(out_buf + out_len, s, chunk);
        out_len += chunk;
        s += chunk;
        n -= chunk;
    }
}
static void put(const char *s) { out_put(s, strlen(s)); }
static void put_dec(uint64_t v) {
    char b[24];
    size_t i = sizeof b;
    do {
        b[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    out_put(b + i, sizeof b - i);
}
static void put_hex(uint64_t v) {
    static const char digits[] = "0123456789abcdef";
    char b[19];
    size_t i = sizeof b;
    do {
        b[--i] = digits[v & 15];
        v >>= 4;
    } while (v);
    b[--i] = 'x';
    b[--i] = '0';
    out_put(b + i, sizeof b - i);
}
static void put_fixed16(uint64_t v) {
    static const char digits[] = "0123456789abcdef";
    char b[16];
    for (size_t i = 0; i < 16; ++i) b[i] = digits[(v >> (60 - i * 4)) & 15];
    out_put(b, sizeof b);
}
static void put_mb(uint64_t v) {
    const uint64_t whole = v >> 20;
    put_dec(whole);
    put(".");
    put_dec(((v >> 10) - (whole << 10)) * 10 / 1024);
}
static void put_time(uint64_t ns) {
    const uint64_t d = ns > start_ns ? ns - start_ns : 0;
    put_dec(d / 1000000000ull);
    put(".");
    const uint64_t ms = (d % 1000000000ull) / 1000000ull;
    if (ms < 100) put("0");
    if (ms < 10) put("0");
    put_dec(ms);
}

static const char *signal_name(int sig) {
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    case SIGABRT: return "SIGABRT";
    case SIGFPE: return "SIGFPE";
    case SIGALRM: return "SIGALRM";
    default: return "?";
    }
}

static const char *kind_name(unsigned kind) {
    switch (kind) {
    case RuntimeCrashMap: return "map";
    case RuntimeCrashMapFail: return "mapfail";
    case RuntimeCrashFlex: return "flex";
    case RuntimeCrashFlexFail: return "flexfail";
    case RuntimeCrashDirect: return "direct";
    case RuntimeCrashDirectFail: return "directfail";
    case RuntimeCrashHeap: return "heap";
    case RuntimeCrashGrowth: return "growth";
    case RuntimeCrashMark: return "mark";
    default: return "?";
    }
}

/* The memory a null store and its like trip over: the object the faulting code was working on.
 * Reads go through process_vm_readv, so an unmapped pointer here cannot fault a second time. */
static void put_window(const char *why, uintptr_t address) {
    uint64_t words[8];
    struct iovec local = {words, sizeof words};
    struct iovec remote = {(void *)address, sizeof words};
    const ssize_t n = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
    put("  ");
    put(why);
    put(" ");
    put_hex(address);
    if (in_image(address)) {
        put(" (guest +");
        put_hex(address - image_base);
        put(")");
    }
    put("\n");
    if (n <= 0) {
        put("    (unreadable)\n");
        return;
    }
    for (size_t i = 0; i < (size_t)n / 8; ++i) {
        put("    +");
        put_hex(i * 8);
        put(": ");
        put_fixed16(words[i]);
        if (in_image((uintptr_t)words[i])) {
            put("  -> guest +");
            put_hex((uintptr_t)words[i] - image_base);
        }
        put("\n");
    }
}

static uintptr_t targets[MAX_TARGETS];
static unsigned target_count;

static void add_target(uintptr_t v) {
    if (!in_image(v)) return;
    for (unsigned i = 0; i < target_count; ++i)
        if (targets[i] == v) return;
    if (target_count < MAX_TARGETS) targets[target_count++] = v;
}

/* Every register that points into the guest image, every such pointer within 2 KiB of the stack
 * pointer (a spilled queue object), and one hop through each of them. */
static void collect_targets(ucontext_t *uc) {
    target_count = 0;
    if (!uc) return;
    static const int regs[] = {REG_RAX, REG_RBX, REG_RCX, REG_RDX, REG_RSI, REG_RDI, REG_RBP,
                               REG_RSP, REG_R8,  REG_R9,  REG_R10, REG_R11, REG_R12, REG_R13,
                               REG_R14, REG_R15, REG_RIP};
    const greg_t *g = uc->uc_mcontext.gregs;
    for (size_t i = 0; i < sizeof regs / sizeof regs[0]; ++i) add_target((uintptr_t)g[regs[i]]);
    const uintptr_t sp = (uintptr_t)g[REG_RSP];
    for (int i = 0; i < 2; ++i) {
        uint64_t words[SCAN_BYTES / 8];
        const uintptr_t at = i ? sp : (sp > SCAN_BYTES ? sp - SCAN_BYTES : 0);
        struct iovec local = {words, sizeof words};
        struct iovec remote = {(void *)at, sizeof words};
        const ssize_t n = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
        for (size_t w = 0; w < (size_t)n / 8; ++w) add_target((uintptr_t)words[w]);
    }
    const unsigned direct = target_count;
    for (unsigned i = 0; i < direct && target_count < MAX_TARGETS; ++i) {
        uint64_t value;
        struct iovec local = {&value, sizeof value};
        struct iovec remote = {(void *)targets[i], sizeof value};
        if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == (ssize_t)sizeof value)
            add_target((uintptr_t)value);
    }
}

static void put_registers(void *context) {
    const ucontext_t *uc = context;
    static const char *const names[16] = {"r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15",
                                          "rdi", "rsi", "rbp", "rbx", "rdx", "rax", "rcx", "rsp"};
    if (!uc) {
        put("registers: (no context)\n");
        return;
    }
    const greg_t *g = uc->uc_mcontext.gregs;
    for (int i = 0; i < 16; ++i) {
        put("  ");
        put(names[i]);
        put("=");
        put_fixed16((uint64_t)g[i]);
        if (in_image((uintptr_t)g[i])) {
            put(" (guest +");
            put_hex((uintptr_t)g[i] - image_base);
            put(")");
        }
        put("\n");
    }
    put("  rip=");
    put_fixed16((uint64_t)g[REG_RIP]);
    put("  rflags=");
    put_fixed16((uint64_t)g[REG_EFL]);
    put("  trap=");
    put_dec((uint64_t)g[REG_TRAPNO]);
    put("  err=");
    put_hex((uint64_t)g[REG_ERR]);
    put("  cr2=");
    put_hex((uint64_t)g[REG_CR2]);
    put("\n");
}

int runtime_crash_begin(int sig, uintptr_t address, void *context, const char *fault_line) {
    runtime_crash_init();
    out_len = 0;
    out_fd = 2;
    if (enabled) {
        const int fd = open(path_buf, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd >= 0) out_fd = fd;
    }
    if (out_fd != 2) {
        /* One line on stderr as well: the console log, and the launcher's export of it, then name
         * the report and where the game died, instead of ending in a bare exit status. The whole
         * report stays in the file (stderr only sees this). */
        static const char lead[] = "\ncrash report: ";
        static const char indent[] = "  ";
        ssize_t written_ = write(2, lead, sizeof lead - 1);
        written_ = write(2, path_buf, strlen(path_buf));
        written_ = write(2, "\n", 1);
        if (fault_line && *fault_line) {
            written_ = write(2, indent, sizeof indent - 1);
            written_ = write(2, fault_line, strlen(fault_line));
        }
        (void)written_;
    }
    out_put(header, header_len);
    put("\nFAULT signal=");
    put_dec((uint64_t)sig);
    put(" (");
    put(signal_name(sig));
    put(") address=");
    put_hex(address);
    put(" tid=");
    put_dec((uint64_t)thread_id());
    put("\n");
    if (fault_line && *fault_line) {
        put("  ");
        put(fault_line);
        const size_t n = strlen(fault_line);
        if (fault_line[n - 1] != '\n') put("\n");
    }
    put("\nregisters:\n");
    put_registers(context);
    put("\nguest memory at the fault (registers, stack, one hop):\n");
    collect_targets(context);
    for (unsigned i = 0; i < target_count; ++i) put_window("at", targets[i]);
    if (!target_count) put("  (nothing points into the image)\n");
    out_flush();
    return out_fd;
}

/* ---- the rest of the report --------------------------------------------------------------------- */

static void dump_maps(void) {
    put("\nproc/self/maps:\n");
    const int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        put("  (unavailable)\n");
        return;
    }
    char buf[4096];
    uint64_t total = 0;
    for (;;) {
        const ssize_t n = read(fd, buf, sizeof buf);
        if (n <= 0) break;
        total += (uint64_t)n;
        if (total > MAPS_LIMIT) {
            put("  (truncated)\n");
            break;
        }
        out_put(buf, (size_t)n);
    }
    close(fd);
    if (out_len && out_buf[out_len - 1] != '\n') put("\n");
}

static void dump_heap(void) {
    put("\nheap: in use ");
    put_mb(heap_max_in_use);
    put(" MB (most so far), from the system ");
    put_mb(heap_max_system);
    put(" MB (most so far)\n");
    const uint64_t n = atomic_load_explicit(&ring_head, memory_order_relaxed);
    const uint64_t first = n > CAP ? n - CAP : 0;
    put("heap trend (in use, from the system; sampled since the process started):\n");
    int shown = 0;
    for (uint64_t i = first; i < n; ++i) {
        const Entry *e = &ring[i % CAP];
        if (e->kind != RuntimeCrashHeap) continue;
        put("  +");
        put_time(e->ns);
        put(" s: in use ");
        put_mb(e->a);
        put(" MB, from the system ");
        put_mb(e->b);
        put(" MB\n");
        ++shown;
    }
    if (!shown) put("  (no sample yet)\n");
}

static void dump_callers(void) {
    uint32_t used = atomic_load_explicit(&table_used, memory_order_relaxed);
    if (used > TABLE_SLOTS) used = TABLE_SLOTS;
    put("\nmemory requests by caller (guest offsets, most bytes first):\n");
    unsigned order[TABLE_SLOTS];
    for (uint32_t i = 0; i < used; ++i) order[i] = i;
    for (uint32_t i = 1; i < used; ++i) {
        const unsigned v = order[i];
        uint32_t j = i;
        while (j && table[order[j - 1]].bytes < table[v].bytes) {
            order[j] = order[j - 1];
            --j;
        }
        order[j] = v;
    }
    for (uint32_t i = 0; i < used; ++i) {
        const Caller *c = &table[order[i]];
        put("  ");
        put_mb(c->bytes);
        put(" MB, ");
        put_dec(c->count);
        put(" requests, ");
        put_dec(c->fails);
        put(" refused, from +");
        put_hex(c->caller[0]);
        put(" < +");
        put_hex(c->caller[1]);
        put(" < +");
        put_hex(c->caller[2]);
        put("\n");
    }
    if (!used) put("  (none: no mapping was made yet)\n");
}

static int is_important(unsigned kind) {
    return kind == RuntimeCrashMapFail || kind == RuntimeCrashFlexFail ||
           kind == RuntimeCrashDirectFail || kind == RuntimeCrashMark;
}

static void dump_ring(void) {
    const uint64_t n = atomic_load_explicit(&ring_head, memory_order_relaxed);
    const uint64_t first = n > CAP ? n - CAP : 0;
    const uint64_t recent = n > 4000 ? n - 4000 : first;
    put("\nevents: ");
    put_dec(n);
    put(" recorded since start, capacity ");
    put_dec(CAP);
    if (n > CAP) {
        put(" (the oldest ");
        put_dec(n - CAP);
        put(" were overwritten)");
    }
    put("\n  +");
    put_time(now_ns());
    put(" s is the fault\n");
    if (!n) {
        put("  (none)\n");
        return;
    }
    /* Only worth a section when the ring has actually wrapped: before that, everything below is
     * in the "last events" list anyway. */
    if (first < recent) {
        put("  refusals and marks among the overwritten events:\n");
        int earlier = 0;
        for (uint64_t i = first; i < recent; ++i) {
            const Entry *e = &ring[i % CAP];
            if (!is_important(e->kind)) continue;
            put("  +");
            put_time(e->ns);
            put(" s ");
            put(kind_name(e->kind));
            put(" size=");
            put_dec(e->a);
            put(" code=");
            put_hex(e->b);
            put(" tid=");
            put_dec(e->tid);
            put("\n");
            ++earlier;
        }
        if (!earlier) put("  (none)\n");
    }
    put("  the last events:\n");
    for (uint64_t i = recent; i < n; ++i) {
        const Entry *e = &ring[i % CAP];
        put("  +");
        put_time(e->ns);
        put(" s ");
        put(kind_name(e->kind));
        if (e->kind == RuntimeCrashHeap) {
            put(" in use ");
            put_mb(e->a);
            put(" MB, system ");
            put_mb(e->b);
            put(" MB");
        } else if (e->kind == RuntimeCrashGrowth) {
            put(" heap growths=");
            put_dec(e->a);
        } else {
            put(" a=");
            put_hex(e->a);
            put(" b=");
            put_hex(e->b);
        }
        put(" tid=");
        put_dec(e->tid);
        put("\n");
    }
}

void runtime_crash_end(int fd) {
    out_fd = fd >= 0 ? fd : 2;
    dump_maps();
    /* With BB_CRASH_REPORT=0 there is no ring and no trend: the fault, the registers and the guest
     * memory still go to stderr, and nothing else. */
    if (enabled) {
        dump_heap();
        dump_callers();
        dump_ring();
    }
    put("\nEND\n");
    out_flush();
    if (out_fd > 2) close(out_fd);
    out_fd = 2;
}

#else /* _WIN32: no signal path to report through */

int runtime_crash_on(void) { return 0; }
void runtime_crash_init(void) {}
void runtime_crash_set_image(uintptr_t base, uint64_t size) { (void)base; (void)size; }
void runtime_crash_note(unsigned kind, uint64_t a, uint64_t b) { (void)kind; (void)a; (void)b; }
void runtime_crash_note_map(unsigned kind, uint64_t size, int64_t result, uint64_t address,
                            const uint64_t callers[3]) {
    (void)kind; (void)size; (void)result; (void)address; (void)callers;
}
void runtime_crash_heap_note(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)a; (void)b; (void)c; (void)d;
}
int runtime_crash_begin(int sig, uintptr_t address, void *context, const char *fault_line) {
    (void)sig; (void)address; (void)context; (void)fault_line; return 2;
}
void runtime_crash_end(int fd) { (void)fd; }

#endif
