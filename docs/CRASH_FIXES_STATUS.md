# Crash fixes ported from BloodBorneButPc — status

This file is the handoff for the branch `awakechan-fixes`: what was ported, where the source of the
fixes is, what was verified and on what, and what is left. It exists because the conversation in
which the port was made cannot be exported — this can.

## The three repositories

| Repository | Role |
|---|---|
| `Awakenchan/BloodBorneButPc` | **The source of the fixes** (Windows fork of the engine). `engine/README.md` is the change table — one line per change with its switch name, crash fixes included. `engine/bbhost-changes.patch` is the whole engine diff, and it applies to `DarkIzuku/bloodborne_pc` at tag `v0.4.0-darkizuku-preview.1`. |
| `nullsumlabs/bbhost-nocrash` | The port that was made first, into a different codebase (`src/host`, `src/hle`, `src/engine`; no vendored shadPS4 tree). Its `FIXES-LOG.md`, and the diffs of branch `awakechan-fixes`, are the same fixes expressed there — useful as a second reading of each fix. |
| `nullsumlabs/bloodborne_pc-nocrash` | **The target**: bbport (this repository). |

The two forks share a base but have diverged, so a fix is never applied as a patch: it is read in the
fork and written again in this architecture, with the fork's switch names kept where the fork has one
(so the same `BB_*` variable means the same thing in both).

## Branch `awakechan-fixes`

Based on `master` (`3c57da4`, 0.51). Eight commits, **local only — not pushed**. Each ported fix is
one commit, so any one of them can be reverted on its own.

| Commit | What it prevents |
|---|---|
| `d79ba11` | **GPU hang.** The collector freed, and wrote back, textures the game was still using: stalls of 0.3–2 s while an area loads, then `VK_ERROR_DEVICE_LOST` in a full-screen blit. `BB_GC_RECENT_FRAMES` (48), `BB_IMAGE_FREE_DELAY_FRAMES` (32). |
| `441ca25` | **Game stops from a garbage descriptor.** `NameOf(DataFormat)`, `NameOf(NumberFormat)` and `MapNumberConversion` (SnormNz with a packed format) asserted from the logging path itself; now they answer `"Unknown"` / no conversion. |
| `9d4cd1a` | **`exit(21)` on an unknown sysctl name.** Now `ENOENT`, as on FreeBSD, with the mib in the log. |
| `ca3cab0` | **Game stops on an out-of-order flip IRQ.** The label write can still be on its way from the draw recording thread; the flip waits for it up to 20 ms, then flips anyway and warns. |
| `87f54ad` | **Null write in audio decoding.** `run_job()` (AJM) allocated the sum of the game's input sizes and copied into it without checking; now bounded to 64 MiB with `INVALID_PARAMETER` on failure. |
| `032d784` | **Null write in the save list.** `save_search()` grew its entry list with an unchecked `realloc`; the growth now stops at the failure. |
| `e5cf2de` | The four above, in `docs/CHANGES_after_0.5.md` (Russian, as the file is). |
| `f9b2fb2` | **Game stops on oversized command data.** `ReserveRecordData` asserted that one command's payload fits the 128 KiB record chunk. The payload now goes to a block of its own, owned and reused by the chunk; `Allocate` spills instead of returning null. |

## Already in bbport — checked, not re-ported

These two of the three fixes that were asked for are already here, so nothing was written for them:

- **VRAM cleanup over a long session.** `GarbageCollectImages` already sizes its trigger/pressure/
  critical marks from the driver's budget (on integrated GPUs and with `BB_GC_BUDGET_MB`), evicts on
  `BB_GC_IDLE_SECONDS` (20 s) and `BB_GC_PRESSURE_IDLE_SECONDS` (5 s), and `BufferCache` has
  `BB_VRAM_IDLE_SECONDS` (60 s) with `LayerYieldToImages` under pressure. The fork's
  `BB_VRAM_PRESSURE_PERCENT` is the same idea with different numbers on discrete cards.
- **Direct memory pool for a scene above 1080p.** `run.sh` already exports `BB_DMEM_MB=9152` when the
  output is not 1080p with the startup patch, and `src/runtime_memory.c` honours it (5056–16384 MiB).

Also verified present in bbport: swapchain recreation on `eErrorOutOfDateKHR`/`eErrorSurfaceLostKHR`,
`VK_FORMAT_UNDEFINED` skipping unknown formats, the texture/sampler cleanup walk limit
(`visited > 256`), a null current chunk replaced instead of read (issue #100), and crash reports for
host faults on game threads (`src/probe.c`: `SIGSEGV`/`SIGBUS` handler, backtrace, watchdog).

## What was verified, and on what

The port was written on macOS, where **nothing in `gpu/` or the engine can be built** (no cmake, no
Vulkan, no SDL3; `third_party/LibAtrac9` is an uninitialised submodule). What was run there:

- `python3 -m unittest discover -s tests`: 108 tests, the same 4 failures and 1 error before and after
  the branch (all host-only: `/private/var` symlink resolution in `test_packaged_vulkan`, a missing
  module in `test_upscaler_assets`, `test_mods`). Compared with `git stash` runs of the same suite.
- `src/runtime_savedata.c` syntax-checked with clang, 0 errors; `src/runtime_ajm.c` and
  `src/runtime_kernel.c` have the same error count before and after (Linux-only APIs: `__rdtsc`,
  `CLOCK_REALTIME_COARSE`, `RUSAGE_THREAD`, `getrandom`, `pthread_getattr_np`, ucontext registers —
  none inside the edited regions).
- The two new environment helpers (`GcRecentFrames`, `ImageFreeDelayFrames`) compiled and run
  standalone with clang, `-Wall -Wextra`.
- The real `RecordChunk` class text, taken verbatim from `vk_scheduler.h`, compiled in a standalone
  harness under ASan/UBSan and exercised: a payload inside the chunk, a payload larger than the
  chunk, 64-byte alignment, reuse of the block after `Execute`, two oversized payloads not
  overlapping, a full chunk spilling instead of returning null, and an absurd size refused.

**Not verified: that any of this compiles or behaves as intended in the GPU tree.** That needs the
Linux box.

## On the Linux box

```bash
git clone --recursive https://github.com/nullsumlabs/bloodborne_pc-nocrash   # or: bash build.sh in the tree
bash build.sh                     # out/bb-probe, out/gpu/libbbgpu.so
bash build.sh --test
python3 -m unittest discover -s tests
ninja -C out/gpu motion-history-test ui-composition-test scene-resolution-test motion-shader-test settings-test
```

Then, in the game, with the fixes at their defaults, and the two switches for A/B:

```bash
BB_FRAME_STATS=1 BB_GAME_DIR=/path/to/CUSA03173 bash run.sh
# the pressure report now ends with "N in use kept since the last report"
BB_GC_RECENT_FRAMES=0 BB_IMAGE_FREE_DELAY_FRAMES=0   # the guard off, the old behaviour
```

What to watch for, in order of likelihood: a compile error in `vk_scheduler.h` (the new
`SpillBlock`/`AllocateOversized`), then the collector keeping too much VRAM on a small card under
pressure (lower `BB_GC_RECENT_FRAMES`, the emergency pass is exempt on purpose), then the flip-IRQ
wait showing up as a hitched frame (it is bounded at 20 ms and reports itself).

## Left over from the fork's list

Not ported, with why:

- **Image write-back GC** (`BB_IMAGE_WRITEBACK_GC`, `WriteBackTiledImage`): frees GPU-drawn tiled
  images under VRAM pressure after writing them back, re-tiled. This is the remaining long-session
  VRAM growth in bbport — its own comment in `GarbageCollectImages` says tiled GPU-written images
  cannot be freed and VRAM grows with every area visited up to the pressure mark. It needs a
  re-tile/readback path that bbport does not have, so it is a feature rather than a crash fix.
- **Intel 12th gen and later red-zone-safe stores** (`patches/redzone.json`, 607 sites): the fork's
  reason is Windows-specific (exception frames overwriting the game's stack values); on Linux the
  equivalent hazard is a signal frame. Porting needs the site list and patch-compiler support.
- **Texture memory sized to the GPU** (`BB_GC_PRESSURE_PERCENT`): see the VRAM item above.
- **Empty recording chunk** and **write faults at the end of a GPU mapping**: bbport handles the
  neighbouring cases in its own way (issue #100, `fault_manager.cpp`), but the fork's specific fixes
  were not compared line by line. Worth a look with a build in hand.
