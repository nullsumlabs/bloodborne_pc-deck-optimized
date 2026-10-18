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
| `BB_CRASH_RING` | `32768` | ring entries (32 B each: 1 MiB at the default) |

## Cost budget (acceptance)

| Item | Cost |
| --- | --- |
| Ring push | ~10–25 ns, no syscall, no allocation, no format |
| Ring memory | 1 MiB, `mmap`ed and pre-touched once at init |
| Heap sample | 2 guest calls + a few loads every 30 s, from the pad sampler thread (already runs) |
| Disk | **zero** during play; one `open` + writes at crash time |
| Off | one cached-boolean branch per note call |

Measured on the Deck once implemented: `frames.csv` p50/p99 unchanged within noise, and
`/proc/<pid>/io` `write_bytes` not growing over a 10-minute soak.

## Steps

- [ ] **1. `src/runtime_crash.c` + `.h` — the core.** Mode flag, ring (`ns, kind, tid, a, b`),
      call-site table (top 64 callers of map/flexible, bytes + count + failures), precomputed
      header (version, `BB_*` flags of interest, `/proc/version`), pre-touched 64 KiB scratch.
      Public: `runtime_crash_init`, `runtime_crash_set_image`, `runtime_crash_note_*`,
      `runtime_crash_heap_note`, `runtime_crash_report`, `runtime_crash_fd`, `runtime_crash_on`.
      *Done when:* compiles under `-Wall -Wextra -Werror`, `BB_CRASH_REPORT=0` allocates nothing.
- [ ] **2. Signal-safe writer.** `write(2)` only: no `malloc`, no `stdio`, no `dladdr`. Sections
      are emitted as they are produced, so a truncated file is still readable, and end with `END`.
      Sections: header, fault (signal, address, all 16 registers), guest windows, thread stacks,
      `/proc/self/maps`, heap trend, call-site table, ring, END.
- [ ] **3. Guest windows at fault time.** For each register pointing into the image, and for each
      image pointer found in the 2 KiB of stack around `rsp`: dump 64 bytes, one hop through a
      pointer if it leads into the image as well. Reads use `process_vm_readv`, so a bad pointer
      cannot fault again. *This is the part that answers `queue+0x78`.*
- [ ] **4. All-thread stacks into the report.** Parameterise `probe.c`'s `dump_frames` with an fd;
      the watchdog path writes to the same file.
- [ ] **5. `probe.c` wiring.** Init before the guest runs, image base after mapping, call
      `runtime_crash_report()` from `fault()`, keep `bbgpu_handle_fault`, the recovery `siglongjmp`
      and `bbgpu_dump_guest_writes` in front of it.
- [ ] **6. `runtime_memory.c` hooks.** Note every `map_flexible` / `posix_mmap` (size, result or
      error, caller triple) and every failure. Wire `runtime_heap_growths` into the header.
- [ ] **7. Heap trend.** `runtime_pad.c`: sample `malloc_stats` every `BB_CRASH_HEAP_SECONDS` when
      the crash report is on *or* `BB_FRAME_STATS`, and hand `in_use/system/max` to the recorder.
      Decoupled from the expensive frame statistics.
- [ ] **8. `run.sh`.** Create `logs/`, export `BB_CRASH_LOG` when `BB_CRASH_REPORT` is not `0`;
      print the path at start so the file is findable after a crash.
- [ ] **9. Test.** `tests/test_crash.c`: child arms the reporter, `SIGSEGV`s through a null store,
      parent asserts the file has header, registers, ring, `END`. Runs in `build.sh --test`.
- [ ] **10. Validation.** Deck soak: no writes during play, no p99 change, and a forced crash
      (`kill -SEGV`) produces a complete report.

## What it answers for `0x2199310`

| Fact | Source in the report |
| --- | --- |
| where and how it died | fault section, registers (`r12=0`) |
| was the arena exhausted, and its size | guest window at the queue pointer / one hop from it |
| did the backing mapping fail | ring: `flexfail`/`mapfail` with size, code and caller |
| was the heap at the ceiling | heap trend, high-water versus from-system |
| who asked for the memory (hours ago) | call-site table |

## Risks

- **A handler that dies mid-dump.** Mitigated by incremental writes + `END` marker + no stdio.
- **Sampler in the wrong thread.** `malloc_stats` runs where the pad sampler runs (game thread),
  as today; never in the fault handler.
- **Masking a race.** Only low-frequency events; the write ring stays opt-in.
- **SteamOS paths.** The report goes to the data directory beside the port, not `/var`; `run.sh`
  creates it.
