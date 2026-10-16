# Performance goals — Steam Deck first

Optimisations worth doing that are **not** quality reductions (no preset, resolution or upscaler
downgrades). Compiled 2026-10-15 from this project's own measurements and design notes:
[parallel_gpu.md](docs/parallel_gpu.md), [CHANGES_2026-10-03.md](docs/CHANGES_2026-10-03.md),
[EMULATION_REMOVAL_PLAN.ru.md](docs/EMULATION_REMOVAL_PLAN.ru.md),
[TRANSLATION_GPU_COPY_LABELS.ru.md](docs/TRANSLATION_GPU_COPY_LABELS.ru.md),
[VULKAN_SHADERS.md](docs/VULKAN_SHADERS.md), [SHADPS4_DEPENDENCIES.md](docs/SHADPS4_DEPENDENCIES.md)
and the source itself.

**Standing caveat on every number below.** They were measured on an RX 7800 XT / RADV, several of
them with `taskset -c 0-3,8-11` as a 4-core / 8-thread stand-in for the Deck — not on Deck hardware.
[ROADMAP.md](docs/ROADMAP.md) still lists Steam Deck validation of the AppImage as open. Treat the
ordering as sound and the magnitudes as needing a Deck run.

## What the Deck changes

- 4 cores / 8 threads, AVX2, no AVX-512 → the serial CPU chain is the hard constraint, not the
  number of threads. The two-stage draw pipeline is on by default there (`BB_DRAW_PIPE` is on at
  8+ hardware threads).
- RDNA2 with 8 CUs and unified 16 GB LPDDR5 shared with the CPU → bandwidth-bound work costs far
  more than on a desktop card, and "VRAM" pressure is real system pressure.
- 1280×800 screen, usually driven at 720p output. At any output other than 1080p the port uses the
  **startup resolution patch** (README, "Resolution and preset changes"), which is the fastest path;
  live resolution is off by default and stays off for the Deck (see "Not worth it").

## Goals at a glance

| # | Goal | Side | Expected impact | Effort | State |
|---|---|---|---|---|---|
| 1 | Image-source copies for the guest `3d5ebf4e` memcpy | GPU / bandwidth | Largest Deck GPU item | Medium | Open |
| 2 | Parallel Vulkan recording (several command buffers) | CPU | Removes the serial cap at 4 cores | Large | Not started |
| 3 | Move the buffer side of a draw from stage B to stage A | CPU | ~12% of stage B | Medium | Planned |
| 4 | Deck-targeted build: `-march`/`-mtune` + PGO profile | CPU | ~5% (measured for LTO+PGO) | Small | Open |
| 5 | Guest copies off the GPU thread's critical path | CPU | ~9% + ~4% of the GPU thread | Medium | Idea in docs |
| 6 | Cheaper object motion vectors | CPU | ~10% | Medium | Open |
| 7 | Scope `-fno-omit-frame-pointer` to the loader | CPU | Small | Small | Open |
| 8 | Occlusion / predication done by the GPU | CPU | ~60 events/frame | Medium | Verify state first |
| 9 | AOT shaders/pipelines + shipped pipeline cache | Both | Stutter, not average FPS | Small/Medium | Roadmap |
| 10 | Deck-tune the streaming guards under a ~1 GB budget | Memory | Fewer area-load stalls | Small | Open |

## 1. Image-source copies for the guest `3d5ebf4e` memcpy

**Why.** `docs/parallel_gpu.md` names this the next GPU item and marks it "matters most on the
Steam Deck": the guest compute shader `3d5ebf4e` is a dword memcpy of render-target memory (the
1080p depth buffer ~12 MB, a G-buffer target, two 960×540 targets), 8 dispatches per frame, and each
one needs the image downloaded into a buffer, the scene proxy resolved, and the destination uploaded
again. On the 7800 XT it is 0.5 ms of an 8.8 ms frame; the work is memory-bound, so on the Deck's
much smaller bandwidth and 8 CUs it costs several ms of a 25–33 ms frame. Two nearby figures from
the same profile show the shape of it: "GPU idle between submissions" 2.1 ms and the guest copy
shader (now translated to SPIR-V by default, `BB_COPY_SHADER_NATIVE=0` keeps the HLE) 1.6–2.1 ms.

**Verified open.** No buffer-copy path tests whether its source is an image
(`grep` of `gpu/shadps4/video_core/buffer_cache/*.cpp` for image-source tests → nothing);
`copyImageToBuffer` exists only for the upscaler and the memory model
(`vk_temporal_upscaler.cpp:125`, `vk_runtime.cpp:323,475`); `3d5ebf4e` appears in no source file or
game profile (`gpu/games/bloodborne.cpp` knows only `buffer_copy_shader = 0xfefebf9f`).

**How.** Recognize a copy whose source range is a registered image and emit an image copy
(`vkCmdCopyImageToBuffer`, or a proxy-sized copy when the scene is reduced — the source is often
read back only to be sampled later as R32F), instead of image → download → buffer → upload. Extend
the game profile with the shader id if the shortcut keys off it.

**Verify.** `BB_GPU_PROFILE=1` per-label GPU ms (the profile already labels recognises, fills,
uploads and downloads); draw counts and `BB_FRAME_STATS` unchanged; screenshots. Bits in
`BB_TOGGLE_FILE` for an in-run A/B as with the other GPU steps.

## 2. Parallel Vulkan recording

**Why.** The measured limit on 4 cores / 8 threads (`docs/CHANGES_2026-10-03.md`, patch mode,
209 FPS, 4c/8t): ~545% of 800% CPU used, and "предел — последовательная цепочка «поток команд →
запись draw → запись Vulkan»". Per-thread: `bb:DrawRec` 72% + 14%, GPU command thread 56% + 20%,
`bb:VkRecorder` 72% + 1 (mostly time inside RADV), game main thread 46% + 10%, four draw-prep
threads ~20% each. Further scaling "требует распараллелить запись Vulkan (несколько командных
буферов) — крупная переделка, не начата".

**How.** Several recording command buffers with barriers only at the seams, each owning its own
stretch of packets. This is the item that lets the Deck use cores it currently cannot.

**Verify.** The `Draw pipe` line in `BB_FRAME_STATS` (stage A waiting, stage B busy); the same
`taskset -c 0-3,8-11` protocol the earlier A/Bs used; screenshots per phase.

## 3. Move the buffer side of a draw from stage B to stage A

**Why.** `docs/parallel_gpu.md` ends with exactly this as the next step: buffer binding is ~12% of
stage B (`ObtainBuffer` ~11%) while stage A "waits most of the time". Stage B is the limit, so
handing a seventh of its work to the under-used stage is the best ratio available short of item 2.

**How.** Stage A owns the buffer cache and hands its commands to stage B, as the constant ring
already does for small read-only buffers (`vk_constant_ring.h`).

**Verify.** Stage A/stage B busy split in `BB_FRAME_STATS`; `BB_PIPE_VERIFY=N` for register-file
equality across the stages; frame rate A/B in one run.

## 4. Deck-targeted build: `-march`/`-mtune` + PGO profile

**Why.** There is no `-march`, `-mtune` or ISA level anywhere in `build.sh`, `gpu/CMakeLists.txt`,
`shell.nix` or `packaging/` — deliberately, "the same build runs on the Steam Deck"
(`docs/parallel_gpu.md`, "LTO and PGO"). PGO only engages when `pgo/*.gcda` exists
(`build.sh:27-31`), so the shipped AppImage carries no profile. Measured in the same section:

| build | FPS | GPU thread µs/draw |
|---|---|---|
| no LTO, no PGO | 73.2 | 7.72 |
| LTO | 74.4 | 7.58 (−1.8%) |
| LTO + PGO | 76.3 | 7.35 (−4.8%) |

**How.** A Deck package built with `-march=znver2 -mtune=znver2` (or `-march=x86-64-v3`, which the
Deck satisfies) plus a profile trained on the Deck (`BB_PGO=generate bash run.sh`, a few minutes of
ordinary play, then the plain build). Either ship it as the Deck AppImage or keep a baseline package
alongside it; a `x86-64-v3` baseline is safe for any CPU of that generation.

**Verify.** The `Frame stats` field "GPU thread us/draw" tolerates scene differences better than FPS
(that is how the table above was made); alternate builds across restarts because run-to-run spread is
±1.5 FPS.

## 5. Guest copies off the GPU thread's critical path

**Why.** The GPU thread is blocked ~9% in `Scheduler::WaitHostCopies` — ~130 times per frame at
EOP/EOS events, `WriteData` and submissions — plus ~4% in `BbCopy::WaitAsync`
(`docs/parallel_gpu.md`, "Second pass"). The doc's own suggestion for "What would scale": let the
idle draw-preparation workers drain a copy queue. On the Deck those are four `SCHED_IDLE` workers
with spare capacity.

**Note from an attempt that failed.** Running small copies on the idle *recording* thread moved the
copying onto the GPU thread instead (86.5 vs 87.2 FPS) and was reverted — the queue must be drained
somewhere that does not compete with the GPU thread.

**Verify.** The blocked-share fields in `BB_FRAME_STATS`; copy stats (`BB_CE_STATS`/copy counters);
frame rate and screenshots.

## 6. Cheaper object motion vectors

**Why.** Measured at ~10% FPS (155 vs 141 FPS with `object_motion=0`); stage B spends ~0.28 µs more
per packet on bone-palette hashes, the index range cache and motion pipeline variants. Needed for
temporal upscaling, so it is not a quality knob to drop — but on 4 cores 10% is material.

**How.** Reuse the previous frame's motion for draws whose pose buffer is unchanged (the pose hash
and `Motion::IndexRangeCache` already exist), or compute the vectors on the GPU.

**Verify.** `BB_DEBUG_MOTION`, the motion vector test targets
(`motion-history-test`, `motion-shader-test`), and frame rate A/B.

## 7. Scope `-fno-omit-frame-pointer` to the loader

**Why.** `gpu/CMakeLists.txt:12` sets it globally for the GPU library; its purpose is the loader's
thread dumps and the watchdog. On a CPU-bound workload it costs a little on every call.

**How.** Build `bb-probe`/`shim` with frame pointers, the hot GPU library without — or make it an
option. **Verify:** the loader's thread dump tests still walk stacks; the µs/draw field.

## 8. Occlusion and predication done by the GPU

**Why.** The game issues ~60 occlusion events a frame. README says the new model turns them into
Vulkan occlusion queries, while the same README's debug-menu section says they "still use synthetic
pixel counters (`PixelPipeStatDump`)" and that `IT_SET_PREDICATION` is unimplemented — the two
statements do not agree, and the earlier note in `EMULATION_REMOVAL_PLAN.ru.md` records the opt-in
Vulkan-query path flashing the lamp glow grey with the upscaler.

**How.** First establish which path is active where (`vk_occlusion.cpp`, `BB_OCCLUSION_QUERIES`,
`BB_OCCLUSION_TRACE=1`), then make the counter stable enough to default on, and implement
predication. **Verify:** `BB_PM4_SELFTEST=1` (occlusion interval checks), read traces, frame rate.

## 9. AOT shaders/pipelines, and ship the pipeline cache

**Why.** Roadmap item: "Shaders translated ahead of time, at install, not during play".
`vk_pipeline_serialization.cpp` and `BB_ASYNC_PIPELINES` already exist and `run.sh:31` raises
`MESA_SHADER_CACHE_MAX_SIZE=4G`; what is missing is compiling during install and shipping the result
in the AppImage. This attacks hitches and first-run stutter (frame time), not average FPS.

**Verify.** First-run time to the title screen and the stall lines in `BB_FRAME_STATS`.

## 10. Deck-tune the streaming guards under a ~1 GB budget

**Why.** Area loads are 40–50 ms frames (down from 60–170 ms) made of ~30 ms of stage-B draw work
plus ~12 ms of copies — so items 2, 3 and 5 are the real fix. What is left is tuning the guards that
stop texture re-upload thrash: `BB_GC_RECENT_FRAMES=48` and `BB_IMAGE_FREE_DELAY_FRAMES=32` (both in
`texture_cache.cpp:31,46`), `BB_STAGING_KEEP_MB` (512) and `BB_FAULT_WINDOW` (256 KiB). The texture
collector's thresholds already follow `VK_EXT_memory_budget` (70 / 85 / 95%), which was added for
integrated GPUs — the Deck is exactly that case.

**How.** A/B those defaults on a small driver budget (`BB_GC_BUDGET_MB` reproduces a Deck-sized
budget on a desktop GPU). Guards that keep images alive longer trade memory for fewer re-uploads;
the right point is different at ~1 GB than at 16 GB.

**Verify.** The pressure report in `BB_FRAME_STATS` (including the count of images kept as in use),
stall lines, and VRAM as reported by `VK_EXT_memory_budget`.

## Not worth doing on the Deck

- **`BB_GUEST_VRAM` / dma-buf BAR import** — needs a discrete GPU with Resizable BAR; a unified
  memory system has no equivalent.
- **Sparse arena as the memory path** (`BB_LAYER_MEMORY=0`) — the AMD default, but the 2026-10-10
  note measured the memory module holding ~1 GB out of VRAM in Hunter's Nightmare (100 vs 154 FPS).
  On a small budget the module is probably the better side; worth one A/B, not a rewrite.
- **Live resolution at 720p** — `SceneTargets::EligibleScene` requires exactly 1920×1080
  (`vk_scene_resolution.cpp:95`), so the reduced-target path cannot engage at 720p, and the patch
  path the Deck already uses is far cheaper (the half-resolution post chain was 8.4 ms live vs
  1.4 ms patched).

## Measure before optimising

Which side binds at the target preset decides the order: items 1 and 8 are GPU, items 2–7 are CPU.

```bash
BB_FRAME_STATS=1 BB_GPU_PROFILE=1 BB_DCB_STATS=1 BB_BUFFER_STATS=1 BB_GAME_DIR=/path/to/CUSA03173 bash run.sh
```

- `BB_FRAME_STATS=1` — "GPU thread us/draw", the `Draw pipe` stage A/stage B split, `Stall:` lines,
  the texture-pressure report.
- `BB_GPU_PROFILE=1` — GPU ms per frame by pass label (passes, dispatches, upscaler runs,
  submissions; "between submissions" is the GPU waiting for the CPU).
- `BB_DCB_STATS=1` — command buffers, draws and state per frame.
- `BB_BUFFER_STATS=1` — buffer bindings per guest region, reuse distance, GPU lag.

Decision rule: if GPU busy time is at or near the frame time, start at item 1. If the serial CPU
chain sits at 90%+ while cores idle, start at items 2, 3 and 5.

## How to land a change here

Every performance step in this project has been landed the same way, and it is worth keeping:
one step per change, behind a `BB_TOGGLE_FILE` bit so it can be switched off in a running game and
compared in a single run, A/B across restarts because run-to-run spread is ±1.5 FPS, screenshots
compared for identical output, and the `Frame stats` counters checked for the expected shift
(`docs/parallel_gpu.md`, "Every step keeps a `BB_TOGGLE_FILE` bit so it can be switched off at run
time and compared by screenshot and frame rate"). Regressions to watch: the GPU-hang guard
(`BB_GC_RECENT_FRAMES`), the oversized-record-block path, and anything that changes the shader
`Info` layout or the pipeline key version (both force cache rebuilds).

## Suggested order

1. **Item 4** — a few build lines, no rendering behaviour touched, measurable with the existing
   µs/draw counter.
2. **Item 1** — the largest Deck-side GPU win, and independent of the CPU work.
3. **Items 3 and 5** — both reduce what stage B and the GPU thread must do, without the large
   refactor of item 2.
4. **Item 2** — the structural fix for 4 cores; decide after measuring what 1, 3 and 5 already
   bought, since it is a large rework.
