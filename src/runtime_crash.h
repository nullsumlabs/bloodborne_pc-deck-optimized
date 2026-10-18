/* bbport: always-on crash recorder (docs/CRASH_RECORDER_PLAN.md).
 *
 * A ring of low-frequency events kept in memory, and one file written when the process dies.
 * BB_CRASH_REPORT=0 turns all of it off: no ring, no sampler, and every note call is one branch
 * on a cached flag.
 */
#pragma once

#include <stdint.h>

#ifndef _WIN32
#include <ucontext.h>
#endif

/* Event kinds (runtime_crash_note). Kept small: the dump names them. */
enum {
    RuntimeCrashMap = 0, /* a posix mmap the game's heap grew with: a = size, b = address or errno */
    RuntimeCrashMapFail,
    RuntimeCrashFlex, /* sceKernelMapFlexibleMemory: a = size, b = address */
    RuntimeCrashFlexFail,
    RuntimeCrashHeap, /* a = in use, b = from the system */
    RuntimeCrashGrowth, /* a = heap growths so far, b = flexible bytes in use */
    RuntimeCrashMark, /* anything else: a, b */
    RuntimeCrashKindCount
};

int runtime_crash_on(void);
void runtime_crash_init(void);
void runtime_crash_set_image(uintptr_t base, uint64_t size);
void runtime_crash_note(unsigned kind, uint64_t a, uint64_t b);
/* Map/flexible events also feed the per-caller table (the guest call sites that asked). */
void runtime_crash_note_map(unsigned kind, uint64_t size, int64_t result, uint64_t address,
                            const uint64_t callers[3]);
void runtime_crash_heap_note(uint64_t in_use, uint64_t from_system, uint64_t max_in_use,
                             uint64_t max_from_system);

/* Writes the report up to the thread stacks and returns the descriptor (2 when no file could be
 * opened). Pass its result to runtime_crash_end() after the caller has added its own sections. */
int runtime_crash_begin(int sig, uintptr_t address, void *context, const char *fault_line);
void runtime_crash_end(int fd);
