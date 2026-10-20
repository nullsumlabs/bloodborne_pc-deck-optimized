/* bbport: the crash recorder (docs/CRASH_RECORDER_PLAN.md).
 *
 * Child 1 records events and exits normally: the report file must not exist afterwards (a session
 * that does not crash leaves nothing on disk). Child 2 records the same events and then stores
 * through a null pointer the way the game did: the report must hold the fault, the registers, the
 * guest memory around the pointers at the fault, the ring and END. Child 3 does the same with
 * BB_CRASH_REPORT=0 and must write no file at all.
 */
#define _GNU_SOURCE
#include "runtime_crash.h"

#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

static volatile uint64_t guest[8];
static const char *report_path;
static const char *stderr_path; /* the crashing child's stderr: what the console log gets */

static void record_events(void) {
    static const uint64_t callers[3] = {0x1122334, 0x2233445, 0x3344556};
    runtime_crash_set_image((uintptr_t)guest, sizeof guest);
    runtime_crash_note_map(RuntimeCrashFlex, 0x200000, 5, 0, callers); /* refused */
    runtime_crash_note_map(RuntimeCrashMap, 0x4000000, 0, 0x7f0000000000ull, callers);
    runtime_crash_note(RuntimeCrashDirect, 0x10000000, 0x42c00000);
    runtime_crash_note(RuntimeCrashGrowth, 41, 0);
    runtime_crash_heap_note(3900ull << 20, 4000ull << 20, 3950ull << 20, 4050ull << 20);
}

static void on_fault(int sig, siginfo_t *info, void *context) {
    const int fd = runtime_crash_begin(sig, (uintptr_t)info->si_addr, context, "test fault\n");
    runtime_crash_end(fd);
    _exit(128 + sig);
}

/* The game's crash of 2026-10-17: a store through a null pointer, with the object the code was
 * working on still in a register and spilled on the stack. The report has to find that pointer
 * on its own and read through it. */
static __attribute__((noinline)) void store_to(uintptr_t base, uintptr_t offset) {
    volatile uintptr_t keep = base; /* a spilled copy, as the game's frame had */
    (void)keep;
    *(volatile uint64_t *)(offset + 8) = 0xbeef; /* offset is zero: the fault is at 0x8 */
}

static void crash_on_null(void) {
    guest[2] = 0x1234567890abcdefull; /* only reachable through the window dump */
    guest[1] = (uint64_t)(uintptr_t)guest;
    store_to((uintptr_t)guest[1], (uintptr_t)guest[0]);
}

static int child(int which) {
    setenv("BB_CRASH_REPORT", which == 3 ? "0" : "1", 1);
    setenv("BB_CRASH_LOG", report_path, 1);
    if (stderr_path && which != 1) {
        const int fd = open(stderr_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            dup2(fd, 2);
            close(fd);
        }
    }
    runtime_crash_init();
    record_events();
    if (which == 1) return 0; /* nothing is written while the game runs */
    struct sigaction sa = {0};
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    crash_on_null();
    return 2; /* not reached */
}

static int run_child(int which) {
    const pid_t pid = fork();
    if (pid == 0) _exit(child(which));
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) return -1;
    return status;
}

/* The handler exits the way probe.c's does (128 + signal); a plain signal death is accepted too,
 * in case the test's own handler is the thing that breaks. */
static int died_of_fault(int status) {
    if (WIFSIGNALED(status)) return WTERMSIG(status) == SIGSEGV;
    return WIFEXITED(status) && WEXITSTATUS(status) == 128 + SIGSEGV;
}

static char *read_report(void) {
    FILE *f = fopen(report_path, "r");
    if (!f) return NULL;
    static char text[262144];
    const size_t n = fread(text, 1, sizeof text - 1, f);
    text[n] = 0;
    fclose(f);
    return text;
}

static int failed;

static void expect(const char *text, const char *what) {
    if (strstr(text, what)) return;
    fprintf(stderr, "crash test: the report has no \"%s\"\n", what);
    failed = 1;
}

int main(void) {
    char path[] = "/tmp/bbport-crash-test-XXXXXX";
    const int fd = mkstemp(path);
    if (fd < 0) {
        fprintf(stderr, "crash test: cannot create a temporary file\n");
        return 1;
    }
    close(fd);
    unlink(path);
    report_path = path;
    char noise[] = "/tmp/bbport-crash-test-XXXXXX";
    const int noise_fd = mkstemp(noise);
    if (noise_fd >= 0) close(noise_fd);
    stderr_path = noise;

    /* Playing without a crash writes nothing. */
    {
        const int status = run_child(1);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "crash test: the first child did not exit cleanly\n");
            return 1;
        }
    }
    struct stat st;
    if (stat(path, &st) == 0) {
        fprintf(stderr, "crash test: a report was written without a crash\n");
        return 1;
    }

    /* A crash writes all of it. */
    if (!died_of_fault(run_child(2))) {
        fprintf(stderr, "crash test: the crashing child did not die as expected\n");
        return 1;
    }
    char *text = read_report();
    if (!text) {
        fprintf(stderr, "crash test: no report after the crash\n");
        return 1;
    }
    expect(text, "bbport crash report");
    expect(text, "BB_CRASH_HEAP_SECONDS");
    expect(text, "kernel=");
    expect(text, "FAULT signal=11 (SIGSEGV) address=0x8");
    expect(text, "test fault");
    expect(text, "registers:");
    expect(text, "rip=");
    expect(text, "guest memory at the fault");
    expect(text, "1234567890abcdef"); /* read through a pointer found at the fault */
    if (!strstr(text, "-> guest +")) {
        fprintf(stderr, "crash test: no guest pointer was followed\n");
        failed = 1;
    }
    expect(text, "proc/self/maps:");
    expect(text, "heap: in use");
    expect(text, "heap trend");
    expect(text, "memory requests by caller");
    expect(text, "0x1122334");
    expect(text, "events:");
    expect(text, "flexfail");
    expect(text, "heap growths=41");
    expect(text, "\nEND\n");
    unlink(path);

    /* The console log (and the launcher's export of it) keeps the path and the fault line, but not
     * the report: the game's output is what a player sends when there is no file at hand. */
    {
        FILE *f = fopen(stderr_path, "r");
        static char line[2048];
        size_t n = f ? fread(line, 1, sizeof line - 1, f) : 0;
        if (f) fclose(f);
        line[n] = 0;
        if (!strstr(line, report_path)) {
            fprintf(stderr, "crash test: stderr does not name the report\n");
            failed = 1;
        }
        if (!strstr(line, "test fault")) {
            fprintf(stderr, "crash test: stderr does not give the fault line\n");
            failed = 1;
        }
        if (n >= sizeof line - 1 || strstr(line, "guest memory at the fault")) {
            fprintf(stderr, "crash test: stderr got the report itself, not just its name\n");
            failed = 1;
        }
    }

    /* BB_CRASH_REPORT=0 writes no file, even on a crash. */
    if (!died_of_fault(run_child(3))) {
        fprintf(stderr, "crash test: the third child did not die as expected\n");
        return 1;
    }
    if (stat(path, &st) == 0) {
        fprintf(stderr, "crash test: BB_CRASH_REPORT=0 wrote a report anyway\n");
        return 1;
    }
    unlink(stderr_path);
    if (failed) return 1;
    puts("crash test: ok");
    return 0;
}
