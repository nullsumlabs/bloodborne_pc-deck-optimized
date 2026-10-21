// tools/cpu_bench.c: the CPU-side loop shapes that fill a frame in this port, as a standalone
// benchmark — the part of an A/B that needs no game files, no Vulkan device and no window.
//
// Build it twice with tools/build_cpu_bench.sh (the portable baseline, and -march=znver2), run both
// on the machine being compared, and tools/cpubench.py prints one table: the same work per case,
// ns per iteration and MB/s, and the difference between the two binaries. Each case names the
// port code whose loop shape it takes:
//
//   copy_*        buffer-cache and texture uploads (gpu/shadps4/video_core/buffer_cache)
//   fill_*        resource clears (textures/texture_cache)
//   detile_*      block-linear to linear 32-bit texture copy (textures/shaders/detile.comp)
//   clamp_*       per-byte min/max over a surface (render tone/clamp passes)
//   scale_*       2x horizontal 4-tap float resample (upscale/)
//   page_scan     one byte per 4 KiB page over 64 MiB (the free check's page walk)
//   cp_decode     command-packet dispatch over 32-bit headers (cp/)
//   draw_math     4x4 matrix * vec4 over a frame's draws (render/vk_draw_prep)
//   hash_*        XXH3-64 over 16 KiB shader/pipeline blobs (layer/, when <xxhash.h> is available)
//
// Single-threaded on purpose: the port's CPU threads are what -march changes codegen for, and one
// thread keeps the comparison about codegen rather than about thread placement on the Deck's 4c/8t.
// Every case touches its buffers before timing, so page faults are not part of the numbers.
//
// Usage: bb-cpu-bench [--out FILE] [--budget MS] [--list]
// By default the run writes bb-cpu-bench-<tag>-<time>.log next to the executable (or in $PWD when
// that directory is read-only) and prints the table to stdout.
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef HAVE_XXHASH
#define XXH_INLINE_ALL
#include <xxhash.h>
#endif

#ifndef BUILD_TAG
#define BUILD_TAG "unknown"
#endif
#ifndef BUILD_FLAGS
#define BUILD_FLAGS "unknown"
#endif

#define MIB (1024u * 1024u)
#define SURFACE_W 1920u
#define SURFACE_H 1080u
#define SURFACE_BYTES (SURFACE_W * SURFACE_H * 4u)

static uint8_t src[64 * MIB];
static uint8_t dst[64 * MIB];
static uint8_t pages[64 * MIB];
static uint32_t packets[65536];
static float mat_in[16];
static float vec_in[4];
static volatile double sink;

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

// ---- the cases: one call is one "frame's worth" of that loop ----

static void copy_64b(void) {
    for (unsigned i = 0; i < 1024; ++i)
        memcpy(dst + i * 64, src + i * 64, 64);
}

static void copy_1k(void) {
    for (unsigned i = 0; i < 256; ++i)
        memcpy(dst + i * 1024, src + i * 1024, 1024);
}

static void copy_64k(void) {
    for (unsigned i = 0; i < 8; ++i)
        memcpy(dst + i * 65536, src + i * 65536, 65536);
}

static void copy_1m(void) {
    memcpy(dst, src, MIB);
}

static void fill_256k(void) {
    memset(dst, 0x5a, 256 * 1024);
}

// 8x8 micro tiles inside 64x64 macro tiles: what the detile shader walks for a 32-bit surface.
static void detile_1080p(void) {
    const uint32_t *tiled = (const uint32_t *)src;
    uint32_t *linear = (uint32_t *)dst;
    const unsigned macros_x = SURFACE_W / 64;
    for (unsigned y = 0; y < SURFACE_H; ++y) {
        const unsigned macro_row = (y >> 6) * macros_x;
        const unsigned micro_y = (y >> 3) & 7;
        for (unsigned mx = 0; mx < macros_x; ++mx) {
            const uint32_t *t = tiled + (macro_row + mx) * 4096 + micro_y * 512 + ((y & 7) * 8);
            uint32_t *line = linear + y * SURFACE_W + mx * 64;
            for (unsigned micro = 0; micro < 8; ++micro)
                memcpy(line + micro * 8, t + micro * 64, 32);
        }
    }
}

static void clamp_1080p(void) {
    uint32_t *pixels = (uint32_t *)dst;
    for (unsigned i = 0; i < SURFACE_W * SURFACE_H; ++i) {
        const uint32_t v = pixels[i];
        const uint32_t clamped = (v & 0x00fcfcfcu) | 0x00030303u;
        pixels[i] = clamped;
    }
}

static void scale_1080p(void) {
    const float *in = (const float *)src;
    float *out = (float *)dst;
    for (unsigned y = 0; y < SURFACE_H; ++y) {
        const float *line = in + y * SURFACE_W;
        float *to = out + y * SURFACE_W * 2;
        for (unsigned x = 0; x < SURFACE_W; ++x) {
            const unsigned x0 = x > 0 ? x - 1 : 0;
            const unsigned x1 = x + 1 < SURFACE_W ? x + 1 : SURFACE_W - 1;
            const unsigned x2 = x + 2 < SURFACE_W ? x + 2 : SURFACE_W - 1;
            to[2 * x] = line[x0] * 0.25f + line[x] * 0.5f + line[x1] * 0.25f;
            to[2 * x + 1] = line[x] * 0.5f + line[x1] * 0.375f + line[x2] * 0.125f;
        }
    }
}

// One cache line per 4 KiB page: the page-tracking walks read the state, not the whole page.
static void page_scan(void) {
    uint64_t sum = 0;
    for (unsigned i = 0; i < 64 * MIB / 4096u; ++i) {
        const uint8_t *page = pages + i * 4096u;
        for (unsigned byte = 0; byte < 64; byte += 8) {
            uint64_t word;
            memcpy(&word, page + byte, sizeof word);
            sum += word;
        }
    }
    sink += (double)sum;
}

static void cp_decode(void) {
    uint32_t acc = 0;
    for (unsigned i = 0; i < 65536; ++i) {
        const uint32_t v = packets[i];
        switch ((v >> 28) & 0xf) {
        case 0: acc += v & 0xfffffff; break;
        case 1: acc ^= v << 3; break;
        case 2: acc += (v >> 4) & 0xffff; break;
        case 3: acc = (acc << 5) | (acc >> 27); acc ^= v; break;
        case 4: if (v & 0x800) acc -= v; break;
        case 5: acc += v & 0x3ff; break;
        case 6: acc ^= (v & 0xff00) >> 8; break;
        default: acc += 1; break;
        }
    }
    sink += acc;
}

static void draw_math(void) {
    float acc[4] = {0, 0, 0, 0};
    for (unsigned i = 0; i < 4096; ++i) {
        for (unsigned row = 0; row < 4; ++row) {
            acc[row] += mat_in[row * 4 + 0] * vec_in[0] + mat_in[row * 4 + 1] * vec_in[1] +
                        mat_in[row * 4 + 2] * vec_in[2] + mat_in[row * 4 + 3] * vec_in[3];
        }
    }
    sink += acc[0] + acc[1] + acc[2] + acc[3];
}

#ifdef HAVE_XXHASH
static void hash_16k(void) {
    uint64_t acc = 0;
    for (unsigned i = 0; i < 16; ++i)
        acc ^= XXH3_64bits(src + i * 16384, 16384);
    sink += (double)acc;
}
#endif

struct case_def {
    const char *name;
    void (*run)(void);
    double bytes; // per iteration, for MB/s
    const char *note;
};

static const struct case_def cases[] = {
    {"copy_64b", copy_64b, 1024.0 * 64, "small buffer/texture copies (buffer_cache, textures)"},
    {"copy_1k", copy_1k, 256.0 * 1024, "buffer-cache chunk copies"},
    {"copy_64k", copy_64k, 8.0 * 65536, "texture rows and medium uploads"},
    {"copy_1m", copy_1m, (double)MIB, "large resource uploads"},
    {"fill_256k", fill_256k, 256.0 * 1024, "resource clears (texture_cache)"},
    {"detile_1080p", detile_1080p, (double)SURFACE_BYTES * 2,
     "block-linear to linear 32-bit surface (detile.comp's walk)"},
    {"clamp_1080p", clamp_1080p, (double)SURFACE_BYTES, "per-byte min/max over a surface"},
    {"scale_1080p", scale_1080p, (double)SURFACE_BYTES * 3, "2x horizontal 4-tap float resample"},
    {"page_scan", page_scan, (double)(64 * MIB / 4096u) * 64, "one cache line per 4 KiB page"},
    {"cp_decode", cp_decode, (double)65536 * sizeof(uint32_t), "command-packet dispatch (cp/)"},
    {"draw_math", draw_math, 4096.0 * 4 * 16 * sizeof(float), "4x4 matrix per draw (draw prep)"},
#ifdef HAVE_XXHASH
    {"hash_16k", hash_16k, 16.0 * 16384, "XXH3-64 of shader/pipeline blobs (layer/, xxhash)"},
#endif
};

static void prepare(void) {
    for (unsigned i = 0; i < sizeof(src); ++i)
        src[i] = (uint8_t)(i * 31u + 7u);
    memset(dst, 0, sizeof(dst));
    memset(pages, 0x11, sizeof(pages));
    for (unsigned i = 0; i < 65536; ++i)
        packets[i] = (i * 2654435761u) ^ (i << 16);
    for (unsigned i = 0; i < 16; ++i)
        mat_in[i] = (float)((i * 7) % 13) * 0.125f - 0.5f;
    for (unsigned i = 0; i < 4; ++i)
        vec_in[i] = (float)i * 0.25f + 0.125f;
}

// One timed round: at least `budget_ms` of this case, so timer overhead is noise.
static double measure(struct case_def entry, double budget_ms) {
    double iterations = 1, elapsed = 0;
    double best_ns = 0;
    for (;;) {
        const double start = now_ns();
        for (double i = 0; i < iterations; ++i)
            entry.run();
        elapsed = now_ns() - start;
        if (elapsed >= budget_ms * 1e6)
            break;
        iterations *= (elapsed > 0 && elapsed < 1e5) ? 16 : 2;
    }
    best_ns = elapsed / iterations;
    // Four more rounds at the same size: the fastest is the least disturbed one, and a sleeping
    // core's clock ramp shows up in the spread rather than in the reported figure.
    for (unsigned round = 0; round < 4; ++round) {
        const double start = now_ns();
        for (double i = 0; i < iterations; ++i)
            entry.run();
        const double ns = (now_ns() - start) / iterations;
        if (ns < best_ns)
            best_ns = ns;
    }
    return best_ns;
}

static int writable_directory(const char *path) {
    char probe[PATH_MAX];
    if (snprintf(probe, sizeof probe, "%s/.bb-cpu-bench-probe.%ld", path, (long)getpid()) >=
        (int)sizeof probe)
        return 0;
    FILE *file = fopen(probe, "w");
    if (!file)
        return 0;
    fclose(file);
    unlink(probe);
    return 1;
}

static void executable_directory(char *out, size_t size) {
    char path[PATH_MAX];
    const ssize_t length = readlink("/proc/self/exe", path, sizeof path - 1);
    if (length <= 0) {
        snprintf(out, size, ".");
        return;
    }
    path[length] = '\0';
    char *slash = strrchr(path, '/');
    if (!slash) {
        snprintf(out, size, ".");
        return;
    }
    *slash = '\0';
    snprintf(out, size, "%s", path);
}

/// The first core's scaling clock, when the kernel reports one: the Deck's 15 W budget moves it.
static double cpu_mhz(void) {
    FILE *file = fopen("/proc/cpuinfo", "r");
    if (!file)
        return 0;
    char line[256];
    double mhz = 0;
    while (fgets(line, sizeof line, file)) {
        if (sscanf(line, "cpu MHz : %lf", &mhz) == 1)
            break;
    }
    fclose(file);
    return mhz;
}

static void host_line(char *out, size_t size) {
    char model[128] = "unknown CPU";
    FILE *file = fopen("/proc/cpuinfo", "r");
    if (file) {
        char line[256];
        while (fgets(line, sizeof line, file)) {
            if (strncmp(line, "model name", 10) == 0) {
                char *colon = strchr(line, ':');
                if (colon) {
                    snprintf(model, sizeof model, "%s", colon + 1);
                    char *newline = strchr(model, '\n');
                    if (newline)
                        *newline = '\0';
                    char *start = model;
                    while (*start == ' ' || *start == '\t')
                        ++start;
                    memmove(model, start, strlen(start) + 1);
                }
                break;
            }
        }
        fclose(file);
    }
    snprintf(out, size, "%s, %ld threads", model, sysconf(_SC_NPROCESSORS_ONLN));
}

int main(int argc, char **argv) {
    const char *out_path = NULL;
    double budget_ms = 300;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else if (strncmp(argv[i], "--out=", 6) == 0) {
            out_path = argv[i] + 6;
        } else if (strcmp(argv[i], "--budget") == 0 && i + 1 < argc) {
            budget_ms = atof(argv[++i]);
        } else if (strncmp(argv[i], "--budget=", 9) == 0) {
            budget_ms = atof(argv[i] + 9);
        } else if (strcmp(argv[i], "--list") == 0) {
            for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i)
                printf("%-14s %s\n", cases[i].name, cases[i].note);
            return 0;
        } else {
            fprintf(stderr, "usage: bb-cpu-bench [--out FILE] [--budget MS] [--list]\n");
            return 2;
        }
    }
    if (budget_ms < 1)
        budget_ms = 1;

    prepare();

    char log_path[PATH_MAX + 128];
    FILE *log = NULL;
    if (out_path) {
        log = fopen(out_path, "w");
    } else {
        char directory[PATH_MAX];
        executable_directory(directory, sizeof directory);
        char stamp[32];
        time_t now = time(NULL);
        struct tm local;
        localtime_r(&now, &local);
        strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &local);
        if (!writable_directory(directory))
            snprintf(directory, sizeof directory, ".");
        // Two runs in the same second (a script running both binaries) each keep their log.
        for (unsigned attempt = 1; attempt <= 99 && !log; ++attempt) {
            if (attempt == 1)
                snprintf(log_path, sizeof log_path, "%s/bb-cpu-bench-%s-%s.log", directory,
                         BUILD_TAG, stamp);
            else
                snprintf(log_path, sizeof log_path, "%s/bb-cpu-bench-%s-%s-%u.log", directory,
                         BUILD_TAG, stamp, attempt);
            log = fopen(log_path, "wx");
            if (!log && errno != EEXIST)
                break;
        }
        out_path = log_path;
    }
    if (!log) {
        fprintf(stderr, "bb-cpu-bench: cannot write %s: %s\n", out_path, strerror(errno));
        return 1;
    }

    char host[256];
    host_line(host, sizeof host);
    const double start_mhz = cpu_mhz();
    printf("bb-cpu-bench %s: %s\n", BUILD_TAG, host);
    printf("build: %s\n", BUILD_FLAGS);
    printf("log: %s\n\n", out_path);
    fprintf(log, "# bb-cpu-bench %s\n", BUILD_TAG);
    fprintf(log, "# host: %s\n", host);
    fprintf(log, "# build: %s\n", BUILD_FLAGS);
    fprintf(log, "# budget_ms: %.0f, single-threaded, fastest of 5 rounds\n", budget_ms);
    fprintf(log, "# cpu_mhz_start: %.0f\n", start_mhz);
    fprintf(log, "# MB = 10^6 bytes\n");
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i)
        fprintf(log, "# case %s: %s\n", cases[i].name, cases[i].note);
    fprintf(log, "case,bytes_per_iter,ns_per_iter,mb_per_s\n");
    printf("%-14s %12s %12s\n", "case", "ns/iter", "MB/s");

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        const double ns = measure(cases[i], budget_ms);
        const double mbps = cases[i].bytes / ns * 1e3;
        printf("%-14s %12.1f %12.0f\n", cases[i].name, ns, mbps);
        fflush(stdout);
        fprintf(log, "%s,%.0f,%.2f,%.0f\n", cases[i].name, cases[i].bytes, ns, mbps);
    }
    fprintf(log, "# cpu_mhz_end: %.0f\n", cpu_mhz());
    fprintf(log, "# sink: %.0f\n", sink);
    fclose(log);
    printf("\nwrote %s\n", out_path);
    return 0;
}
