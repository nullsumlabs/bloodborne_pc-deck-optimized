# Crash fixes — status on `deck-0.6`

Copied from `master` (where it tracked the `awakechan-fixes` port into the 0.51 tree) and rewritten
for this branch. Source-level only: nothing here was built or run on `deck-0.6`.

## Where the work is

| Branch | Tree | Crash work on it |
|---|---|---|
| `deck-0.6` (here) | 0.6 GPU tree (`gpu/render`, `gpu/textures`, `gpu/gcn`, `gpu/cp`) | none of the seven ports; no crash recorder |
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

**Needs porting** (0.6 has the pre-fix 0.51 code at each of these points; `master` has the fix)

- `9d4cd1a` unknown sysctl: `exit(21)` at `src/runtime_kernel.c:254-257`; should be `ENOENT`, with
  the mib logged.
- `87f54ad` audio: `src/runtime_ajm.c:317-319` mallocs the sum of the input sizes and copies into it
  unchecked. Bound it (64 MiB) and return `INVALID_PARAMETER`.
- `032d784` save list: `src/runtime_savedata.c:289` grows its entry list with an unchecked `realloc`.
- `f9b2fb2` oversized record data: `ReserveRecordData` asserts `bytes + 1024 <= 128 KiB`
  (`gpu/render/scheduler.h:282`), then `RecordData` memcpys into a null `Allocate`
  (`:299-300`; `gpu/render/record_chunk.h:59-67` returns null when full).

**Mostly obsolete — re-check before porting**

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
`BB_IMAGE_FREE_DELAY_FRAMES` (32) free delay. Nothing in the 0.6 code shows the 0.51 hang is still
there, so reproduce the load-time stall and the `DEVICE_LOST` blit on the Deck first.

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

## Our own work not on this branch

- **Crash recorder** (`crash-logger-build`): `src/runtime_crash.c`, `tests/test_crash.c`, and `run.sh`
  naming the report before the game starts. 0.6's `src/probe.c` has the `SIGSEGV`/`SIGBUS` handler,
  backtrace and watchdog, but no recorder.
- **Free-check page walking** (`9c0838f`, on `crash-logger-build` only): the fix walks only the
  pages being written to and drops them after 30 s idle (1164 lines). Here — and on `master` — it is
  the 1090-line pre-fix version, which sweeps every page ever seen.
- **`0a5442d`, `d1038c7`** (0.5 perf fixes, `crash-logger-build` only): they patch
  `gpu/shadps4/...`, which 0.6 replaced — re-express rather than port.

## Checks

```bash
bash build.sh --test                                    # C tests into out/
python3 -m unittest discover -s tests
bash build.sh                                           # out/gpu/libbbgpu.so
```
