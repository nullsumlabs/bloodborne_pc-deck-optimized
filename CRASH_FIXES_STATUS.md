# Crash fixes — status on `deck-0.6-crashfixes`

Copied from `master` (where it tracked the `awakechan-fixes` port into the 0.51 tree) and rewritten
for `deck-0.6`; the fixes it asked for landed on `deck-0.6-crashfixes`, which branches from it.
Every commit below was built (`bash build.sh --test`) and ran the Python suite (131 tests) on this
branch before it was committed. Not run in the game: no `eboot.bin` here.

## Where the work is

| Branch | Tree | Crash work on it |
|---|---|---|
| `deck-0.6` | 0.6 GPU tree (`gpu/render`, `gpu/textures`, `gpu/gcn`, `gpu/cp`) | none of the seven ports; no crash recorder |
| `deck-0.6-crashfixes` (here) | `deck-0.6` | four of the seven (`a7ced51`, `ab93072`, `5601d6c`, `abc77f2`), the free-check page walk (`2254306`) |
| `master` | 0.51 (`gpu/shadps4`) + the seven ports + `nocrash-optimizations` | all seven fixes |
| `crash-logger-build` | `master` | crash recorder (`src/runtime_crash.c`, `tests/test_crash.c`, `run.sh`), free-check page walking `9c0838f`, 0.5 perf fixes `0a5442d`/`d1038c7` |

The seven ports are one commit each, so any one can be read alone: `441ca25`, `9d4cd1a`, `87f54ad`,
`032d784`, `f9b2fb2`, `ca3cab0`, `d79ba11`.

## The seven, against 0.6

**Done upstream in 0.6 — do not port**

- `441ca25` garbage descriptor. `NameOf` answers `"FormatReserved"`/`"NumberReserved"`
  (`gpu/gcn/formats.cpp:96,105`), and `MapNumberConversion` returns `None` for SnormNz over a packed
  format (`gpu/gcn/formats.h:282-293`, the comment names #166). `docs/CHANGES_0.6.md:132` lists
  #144/#165/#166.

**Ported here** (0.6 had the pre-fix 0.51 code at each of these points)

- `9d4cd1a` unknown sysctl → `a7ced51`: an unknown mib returns `ENOENT` instead of `exit(21)`, with
  the mib logged the first eight times.
- `87f54ad` audio → `ab93072`: the join of a job's inputs is bounded at 64 MiB; on failure the job
  reports `INVALID_PARAMETER` with no input instead of writing through a null pointer.
- `032d784` save list → `5601d6c`: `save_search` stops at a failed `realloc` with what it read.
- `f9b2fb2` oversized record data → `abc77f2`: a payload over `RecordChunk::Capacity` lives in a
  block of its own, owned and reused by the chunk; nothing is dropped and the chunk's own space is
  unchanged. A failed allocation or a payload beyond 16 MiB is still a named stop.
  Checked in a harness under ASan/UBSan: in-chunk, oversized, alignment, reuse after `Execute`, two
  spills not overlapping, a full chunk spilling, the 16 MiB ceiling.

**Not taken**

- GC retention (the fork's "used in the last N submissions" rule and the free delay): left out, and
  only worth adding opt-in — no 0.51 hang has been reproduced on 0.6, and the existing budget-derived
  marks have not been shown to fail.
- Crash recorder (`crash-logger-build`): a diagnostic, not a fix, and it changes the startup path;
  keep it on its own branch until a report is wanted from players.
- `ca3cab0` out-of-order flip. The flip now runs in CP order, after the writes before it, through
  `RunInOrder` (`gpu/cp/cp_graphics.cpp:320-324`) — the fix's intent. The assert remains at
  `gpu/platform/videoout/videoout.cpp:394`, so a bounded wait still matters only if a CPU-written
  label (GCN 3/4 path) can be in flight when the IRQ is raised.
- `d79ba11` GC hang guard: not present, and not shown to be needed (next section).

## 0.6's collector

`GarbageCollectImages` (`gpu/textures/texture_cache_memory.cpp:153`) takes its marks from the driver
budget using `BB_GC_BUDGET_MB`, evicts by idle seconds (`BB_GC_IDLE_SECONDS` 20,
`BB_GC_PRESSURE_IDLE_SECONDS` 5), and never frees a tiled GPU-written image (`:231-234`). Memo hits
touch the LRU (`gpu/textures/texture_cache.cpp:110-111`); deletion waits for GPU completion
(`gpu/textures/texture_cache_track.cpp:168-192`).

Missing are the fork's "used in the last N submissions" rule (`BB_GC_RECENT_FRAMES`, 48) and its
`BB_IMAGE_FREE_DELAY_FRAMES` (32) free delay — left out on purpose (see "Not taken"). If the
load-time stall or the `DEVICE_LOST` blit turns up on the Deck, reproduce it first, then add them
opt-in and compare.

## Left over from the fork's list

- **Image write-back GC** (re-tile GPU-drawn images to free them under pressure): absent, and
  `:231-234` is the comment that says so. A feature, not a crash fix — the remaining long-session
  VRAM growth.
- **Red-zone-safe stores** (607 sites, `patches/redzone.json`): absent, and this repo has no such
  site list — deliberately. The fork's `protect_red_zones` docstring limits the hazard to Windows:
  "Where Windows saves extended state in the compact XSAVE layout (Intel 12th gen and later), an
  exception's frame holds the AVX registers up to 0x28 bytes below rsp, over the System V red zone
  where leaf functions keep live locals (PS4 and Linux leave those 128 bytes alone)." Linux's
  `get_sigframe` steps `sp` down past the 128-byte red zone before writing the frame, so a signal at
  a store the GPU tracker write-protected cannot overwrite the writer's red-zone locals. Do not add
  the JSON or the `link()` hook here; our own guest hooks take the 128 bytes explicitly
  (`GuestRedZoneBytes`, `gpu/shim/bbport_guest_code_hooks.cpp:32,145`).
- **`BB_GC_PRESSURE_PERCENT`**: superseded by the budget-derived marks above.
- **Empty recording chunk**: handled — `CurrentChunk()` creates one on demand
  (`gpu/render/scheduler.h:469`).
- **A fault at the end of a GPU mapping**: not re-compared with the fork.

## Our own work

- **Crash recorder** (`crash-logger-build`): `src/runtime_crash.c`, `tests/test_crash.c`, and `run.sh`
  naming the report before the game starts. 0.6's `src/probe.c` has the `SIGSEGV`/`SIGBUS` handler,
  backtrace and watchdog, but no recorder.
- **Free-check page walking** (`9c0838f`, on `crash-logger-build`): ported as `2254306` — only the
  pages being written to are walked first, the rest round-robin a slice at a time, and a page idle
  for 30 s is dropped. Its session log stayed on `crash-logger-build`.
- **`0a5442d`, `d1038c7`** (0.5 perf fixes, `crash-logger-build` only): they patch
  `gpu/shadps4/...`, which 0.6 replaced — re-express rather than port.

## Checks

```bash
bash build.sh --test                                    # C tests into out/
python3 -m unittest discover -s tests
bash build.sh                                           # out/gpu/libbbgpu.so
```
