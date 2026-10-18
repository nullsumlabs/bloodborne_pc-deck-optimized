# Crash recorder ("black box") — plan

A build that can be played for hours with **no disk writes and no measurable per-frame cost**, and
that writes **one file** when the game dies: everything needed to explain the death.

Branch feature. Everything is behind one flag, off means off (`BB_CRASH_REPORT=0`): no ring is
allocated, no sampler runs, the note path is one predictable branch on a cached `static const bool`.

## Why

The port already has the parts, but each is a separate `BB_*` measurement mode that prints to
stderr: the fault handler (`probe.c`), the write ring (`bbport_write_log`), the heap report
(`runtime_pad.c`), the call-site table (`bbport_heap_sites`), the all-thread dump (`probe.c`).
None of them together answers "why", and none of them survives a crash into a file unless
`BB_SAVE_LOG=1` is also on — which drags in the per-frame statistics.

The crash of 2026-10-17 is the target case: `guest offset 0x2199310, address 0x8`, a null store
with `r12` the return of `arena_alloc(&queue->0x78, 0x10, 0x10)`. Today we know *where* it died.
The missing facts are: was the arena full (and how big is it), did a mapping fail shortly before,
and was the guest heap climbing to its ceiling. All three are cheap to keep.

## Non-goals

- Not a debug build: the same `RelWithDebInfo`/PGO binary, one extra C file.
- Not the write ring: `BB_WRITE_LOG` stays a tripwire (it can mask the race, as seen with the
  fence bug). This recorder records *low-frequency* events only.
- No cores, no minidumps, no symbol table in the binary: raw PCs + `/proc/self/maps`, symbolized
  afterwards.
- No guest-code patching.

## Flags

| Flag | Default | Meaning |
| --- | --- | --- |
| `BB_CRASH_REPORT` | `1` | `1` arms the recorder; `0` disables all of it (compiled-in branch, no ring, no sampler, no fd) |
| `BB_CRASH_LOG` | `$BB_DATA_DIR/logs/crash-<time>.txt` (set by `run.sh`) | where the report goes; the file is opened **only** when the crash happens, so a clean session leaves no file |
| `BB_CRASH_HEAP_SECONDS` | `30` | heap trend sampling period in the game-thread sampler |

The ring is a fixed 32768 entries of 32 bytes (1 MiB), touched once at startup.

## Cost budget (acceptance)

| Item | Cost |
| --- | --- |
| Ring push | 24 ns measured here (15 ns of it is the timestamp), no syscall, no allocation, no format |
| Ring memory | 1 MiB, touched once at init (the note path takes no page fault) |
| Heap sample | 2 guest calls + a few loads every 30 s, from the pad sampler thread (already runs) |
| Disk | **zero** during play; one `open` + writes at crash time |
| Off | one cached-boolean branch per note call |

Measured on the Deck once implemented: `frames.csv` p50/p99 unchanged within noise, and
`/proc/<pid>/io` `write_bytes` not growing over a 10-minute soak.

## Steps

- [x] **1. `src/runtime_crash.c` + `.h` — the core.** Mode flag, ring (`ns, kind, tid, a, b`),
      call-site table (top 64 callers of map/flexible, bytes + count + failures), precomputed
      header (build, `BB_*` flags of interest, `/proc/version`), pre-touched ring and tables.
      Public: `runtime_crash_init`, `runtime_crash_set_image`, `runtime_crash_note`,
      `runtime_crash_note_map`, `runtime_crash_heap_note`, `runtime_crash_heap_sample`,
      `runtime_crash_begin`, `runtime_crash_end`, `runtime_crash_on`.
- [x] **2. Signal-safe writer.** `write(2)` only: no `malloc`, no `stdio`, no `dladdr`. Sections
      are emitted as they are produced, so a truncated file is still readable, and end with `END`.
      Sections: header, fault (signal, address, all 16 registers), guest windows, thread stacks
      (`probe.c` between `begin` and `end`), `/proc/self/maps`, heap trend, call-site table, ring,
      END. With the flag off the fault, registers and windows still go to stderr.
- [x] **3. Guest windows at fault time.** For each register pointing into the image, and for each
      image pointer found in the 2 KiB of stack around `rsp`: dump 64 bytes, one hop through a
      pointer if it leads into the image as well (pointers are labelled with their guest offset).
      Reads use `process_vm_readv`, so a bad pointer cannot fault again.
- [x] **4. All-thread stacks into the report.** `probe.c`'s `dump_frames` takes an fd; the
      `SIGUSR2` dumper and the watchdog write to the same file.
- [x] **5. `probe.c` wiring.** `runtime_crash_init()` at the top of `main`, image base right after
      the image is mapped, `runtime_crash_begin/end` from `fault()` and the watchdog, after
      `bbgpu_handle_fault`, the recovery `siglongjmp` and `bbgpu_dump_guest_writes`.
- [x] **6. `runtime_memory.c` hooks.** Every `map_flexible` and `posix_mmap` is noted (size, address
      or error, caller triple), every direct allocation and every refusal too — 14 direct
      allocations and 20 flexible mappings in a measured session, so recording all of them is
      free. The heap growth counter rides with the `posix_mmap`.
- [x] **7. Heap trend.** `runtime_crash_heap_sample()` reads `malloc_stats` from the pad sampler
      thread every `BB_CRASH_HEAP_SECONDS`, independent of `BB_FRAME_STATS` (which keeps its own
      5-second print). The libc stub is looked at with a safe read first: the module may not be
      loaded yet.
- [x] **8. `run.sh`.** Creates `logs/`, exports `BB_CRASH_LOG=$data/logs/crash-<time>.txt` when
      `BB_CRASH_REPORT` is not `0`, and prints the path at start.
- [x] **9. Test.** `tests/test_crash.c` (in `build.sh --test`, needs no GPU library and no guest):
      a child that plays and exits writes nothing; a child that stores through a null pointer
      leaves a report with the fault, the registers, the marker read through a spilled pointer,
      the ring and `END`; a child with `BB_CRASH_REPORT=0` writes nothing even when it crashes.
- [ ] **10. Validation on the Deck.** A soak with the game: no writes while playing, no p99 change,
      and a forced crash (`kill -SEGV`) produces a complete report.

A package with the recorder on by default exists to play that soak with:
`dist/Bloodborne_PC-flightrec--bbport-steamdeck-x86_64.AppImage`, built by
`packaging/appimage-repack.sh` out of the 0.51.1 Deck package's libraries, with its own README in
`dist/`. `packaging/bbport-wrapper.sh` exports `BB_CRASH_REPORT=1`, so the packaged port turns the
recorder on without a variable being set anywhere.

## What it answers for `0x2199310`

| Fact | Source in the report |
| --- | --- |
| where and how it died | fault section, registers (`r12=0`) |
| was the arena exhausted, and its size | guest window at the queue pointer / one hop from it |
| did the backing mapping fail | ring: `flexfail`/`mapfail` with size, code and caller |
| was the heap at the ceiling | heap trend, high-water versus from-system |
| who asked for the memory | call-site table |
| everything the session grew with | the ring (all growth events since start, not just the failures) |

## Verified here (this machine, no game)

`bash build.sh --test` builds the port and runs every test, including `out/crash-test`, which
crashes on purpose four times (`BB_CRASH_REPORT=1` twice, `=0` once) and reads the files back.
A hand-run with the report path set produces (excerpt):

```
bbport crash report
build=... kernel=...
FAULT signal=11 (SIGSEGV) address=0x8 tid=539900
  Guest fault (signal 11) at guest offset 0x2199310, address 0x8
registers:
  r12=000061e8520f2040 (guest +0x0)
  ...
  cr2=0x8
  at 0x61e8520f2040 (guest +0x0)
    +0x8: 000061e8520f2040  -> guest +0x0
    +0x10: 1234567890abcdef
heap: in use 4020.0 MB (most so far), from the system 4096.0 MB (most so far)
heap trend: +0.000 s: in use 3000.0 MB, from the system 4096.0 MB ...
0.0 MB, 2 requests, 1 refused, from +0x21992db < +0x219a104 < +0x21a0f80
events: 7 recorded since start, capacity 32768
  +0.000 s flexfail a=0x1000000 b=0x5 ...
  +0.000 s direct a=0x10000000 b=0x42c00000
END
```

`run.sh` prints `Crash report: <BB_DATA_DIR>/logs/crash-<time>.txt` and creates the directory;
with `BB_CRASH_REPORT=0` it prints nothing and creates nothing.

The note path, 10 million notes in a loop: **24.2 ns** with the recorder on, 1.4 ns with it off
(the cached flag). The first version of this measured 370 ns per note, because
`#include <sys/syscall.h>` sat inside `#ifdef SYS_gettid` — the macro that header defines, so
`thread_id()` fell through to `getpid()`, which is a 345 ns system call on this machine. The
thread id is read once per thread now.

## Risks

- **A handler that dies mid-dump.** Mitigated by incremental writes + `END` marker + no stdio.
- **Sampler in the wrong thread.** `malloc_stats` runs where the pad sampler runs (game thread),
  as today; never in the fault handler.
- **Masking a race.** Only low-frequency events; the write ring stays opt-in.
- **SteamOS paths.** The report goes to the data directory beside the port, not `/var`; `run.sh`
  creates it.
