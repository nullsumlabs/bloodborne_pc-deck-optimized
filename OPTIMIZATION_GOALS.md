# Performance goals — Steam Deck first

Optimisations worth doing that are **not** quality reductions (no preset, resolution or upscaler
downgrades). Compiled 2026-10-15, state as of 2026-10-19, from this project's own measurements and
design notes:
[parallel_gpu.md](docs/parallel_gpu.md), [CHANGES_2026-10-03.md](docs/CHANGES_2026-10-03.md),
[EMULATION_REMOVAL_PLAN.ru.md](docs/EMULATION_REMOVAL_PLAN.ru.md),
[TRANSLATION_GPU_COPY_LABELS.ru.md](docs/TRANSLATION_GPU_COPY_LABELS.ru.md),
[VULKAN_SHADERS.md](docs/VULKAN_SHADERS.md), [SHADPS4_DEPENDENCIES.md](docs/SHADPS4_DEPENDENCIES.md),
[APPIMAGE_DEPENDENCIES.md](docs/APPIMAGE_DEPENDENCIES.md) and the source itself.

**Standing caveat on every number below.** They were measured on an RX 7800 XT / RADV, several of
them with `taskset -c 0-3,8-11` as a 4-core / 8-thread stand-in for the Deck — not on Deck hardware.
[ROADMAP.md](docs/ROADMAP.md) still lists Steam Deck validation of the AppImage as open. Treat the
ordering as sound and the magnitudes as needing a Deck run.

**How the estimates below are built, and how much to trust each.** Three kinds of number appear, and
each is labelled where it is used. (a) *Measured* — the project's own A/Bs on the 7800 XT
(`7.72 → 7.35 µs/draw`, `141 → 155 FPS`), the tightest kind. (b) *Share of a stage* — a bound taken
from where the time sits inside one stage or thread (`buffer binding is ~12% of stage B`, `~9% + ~4%
of the GPU thread`); that becomes an end-to-end percentage only if the same stage is what limits the
frame. (c) *Extrapolation* — converting a desktop figure to the Deck, the weakest kind, always
spelled out. Every figure is stated against a frame: 8.8 ms on the desktop card used for these
measurements, 25–33 ms on the Deck at 720p.

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
| 1 | Image-source copies for the guest `3d5ebf4e` memcpy | GPU / bandwidth | 0.2–0.5 ms/frame on the Deck (measured 2026-10-16) — ≈1–3%, not the ≈10–20% extrapolated | Medium | **Built, small** — only the declining remainder, and off by default; see §1 |
| 2 | Parallel Vulkan recording (several command buffers) | CPU | Unmeasured ceiling — CPU was at 68% (545/800) at 4c/8t; ≈10–20% if half the idle capacity is recovered | Large | **Built since 0.3** (two threads on the Deck), never A/B'd; only matters where the CPU binds — see §2 |
| 3 | Move the buffer side of a draw from stage B to stage A | CPU | ≤12% of stage B; ≈2–6% end-to-end | Medium | Planned |
| 4 | ~~Deck-targeted build: `-march`/`-mtune` + PGO profile~~ | CPU | ≈4–5% (measured for LTO+PGO); `-march` should add a little on the Deck | Small | **Done** — `build_steam_deck.sh`, in 0.51.1 and flightrec; never played, so no Deck A/B |
| 5 | Guest copies off the GPU thread's critical path | CPU | ≤13% of the GPU thread (9% + 4%); ≈3–6% end-to-end | Medium | Idea in docs |
| 6 | Cheaper object motion vectors | CPU | ≤10% measured with motion off; ≈3–6% recoverable | Medium | Open |
| 7 | Scope `-fno-omit-frame-pointer` to the loader | CPU | ≈0–2%, usually under 1% | Small | Open |
| 8 | Occlusion / predication done by the GPU | CPU + GPU | Unquantified: ~60 events/frame, plus whatever the GPU draws that it should be culling | Medium | Verify state first |
| 9 | AOT shaders/pipelines + shipped pipeline cache | Both | ≈0 on average FPS; removes first-run and traversal hitches | Small/Medium | Roadmap |
| 10 | Deck-tune the streaming guards under a ~1 GB budget | Memory | ≈0 steady-state; a few % during area loads | Small | Open |
| 11 | Skip draws that have nothing to render into (the `pass 16384x16384` labels) | GPU / recording | 0.4–1.2 ms/frame on the Deck (from the profile labels, 2026-10-16) — ≈3–6% | Small | **Built, bit 33 on by default** (shipped in 0.51.1 and flightrec) — visual A/B clean (2026-10-17), frame times pending |

## Where we are (2026-10-19)

The newest frame times in this file are still the 2026-10-16 Deck profiles behind items 1 and 11;
**no optimisation A/B has run since**, so *State* below means built, not confirmed on Deck:

- **Built and shipping:** item 4 — 0.51.1 (2026-10-17) and the 2026-10-18
  `dist/Bloodborne_PC-flightrec--…AppImage` both carry `-march=znver2` binaries, but neither has
  been played, so the ≈4–5% is still the desktop LTO+PGO figure (the PGO profile is still
  desktop-trained). Item 11 ships on by default; its 2026-10-17 visual A/B on the Deck was clean,
  and its `frames.csv` was never read.
- **Built and off:** item 1, behind profile + bit 32 — its ≈1–3% comes from the traces and logs,
  not a frame time; the residual `704x1x1` copy class is still unidentified.
- **Built and never measured:** item 2 — the Deck already records on two threads; the
  `BB_VK_RECORD_THREADS` / `BB_VK_SEGMENT_KB` sweep is still the way to settle it.
- **Untouched plans:** items 3, 5, 6, 7, 10; item 8's active occlusion path is still not
  established; item 9 is on the roadmap.
- **One new per-frame cost to account for:** the crash recorder, on by default in flightrec —
  ~24 ns a note, no syscall, no allocation, a 1 MiB ring, zero disk writes while playing, but the
  frame-time half of its budget (`frames.csv` p50/p99 with it on) is unmeasured, so it belongs in
  the same Deck run as item 11's frames.

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

**Estimated impact (superseded by the Deck measurement below).** ≈6% of the desktop frame at the very
most — the profile charges `3d5ebf4e` 0.5 ms of 8.8 ms and `parallel_gpu.md` says recognising
image-source copies "would remove most of that". The Deck figure is an *extrapolation* in both
directions: the same dispatches are
memory-bound, so at roughly a seventh of the bandwidth they should be a materially larger share of a
25–33 ms frame, best guess ≈10–20% — but the desktop measurement was taken where the GPU was already
idle 2.1 ms of the frame ("GPU idle between submissions", 24%), i.e. CPU-bound, so a GPU-side saving
partly hides there and should convert closer to one-for-one on the Deck, where 8 CUs and 720p make
the GPU the limiter. Cheap to falsify: the label to watch is already in `BB_GPU_PROFILE`, and the
question to answer first is whether the GPU busy time on Deck sits at the frame time.

**Measured on the Deck (2026-10-16, bit 32).** The trace had already answered most of the question
before any code was written: the image-copy detector was turning **95 of 107** sampled dispatches of
`3d5ebf4e` into image copies, and the 12 that declined all named the same predicate
(`dist/deck-copytrace/LOGS/20261016_011952.log`). The build that registers that missing destination
image from the source's layout then ran 31.7 s in the heavy area
(`dist/deck-copydest/LOGS/20261016_024050.log`): both declining
classes (128×128 / 65 536 B and 856×480 / 1 835 008 B) become image copies and stay in image space,
and the class the baseline had running — `dispatch cs 3d5ebf4e (256x1x1 groups)`, 2.5–5.0 dispatches
per frame, 0.20–0.46 ms/frame — stops running altogether. What is left of the emulated copies is one
class the trace never sampled (`704x1x1`, 1.2–2.0/frame, 0.21–0.35 ms/frame).

So the item is worth **≈0.2–0.5 ms/frame, ≈1–3% of the Deck's 17–23 ms frame**, in the frames that
run over the 60 FPS cap. The `≈10–20%` above counted the whole 8-dispatches-a-frame family; the
detector was already handling nearly all of it, so only the declining remainder was ever in play.
The bit stays **off by default** (nothing promotes it), and the residual class is unidentified.

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

**What was built (0.3, `4d4d208` 2026-10-06; async submit in 0.4).** Several command buffers: the
scheduler cuts the stream into segments of ~64 KiB of recorded closures (`BB_VK_SEGMENT_KB`; 128 KiB
while a render pass is open — the cut closes it and the next command buffer reopens it without
clearing), hands them round-robin to N recording threads, and those threads submit their own segments
(`BB_ASYNC_SUBMIT`). N is `BB_VK_RECORD_THREADS`, else 3 on ≥12 hardware threads and 2 on ≥8, so **the
Deck already records on two threads** (its log prints `GPU: 2 Vulkan recording threads, segments of
64 KiB`). Anchors: `vk_scheduler.cpp:100` (`RecordingThreadCount`), `:128` (`BB_VK_SEGMENT_KB`),
`:489` (`MaybeSplit`), `:523` (`HandOver`), `:633` (`SyncRecording`); `vk_scheduler.h:590`
(`MaxSegments` 64); `bbport_toggles.h:75` `ParallelRecording = 1 << 58` — bit *set* in the first
number of `BB_TOGGLE_FILE` means no cuts and one recording thread, which is the in-run A/B of exactly
this item.

**What is left is measuring it, and on which machine it pays.** No A/B of the segmented path is
recorded anywhere, and the default (two threads, 64 KiB) comes from a desktop figure: "one thread
records ~75% of a core at 200 FPS (4 cores / 8 threads)". The Deck's own logs disagree on whether it
can matter at all: in the plaza fight the GPU was busy the whole frame ("GPU idle between
submissions" 0.77 ms/frame) — nothing on the CPU side helps there — while the static look-around run
in the heavy area had the GPU idle 10.3 ms of a 21 ms frame with `stage B busy 80–85%`, and that is
where two recording threads are worth checking. Sweep `BB_VK_RECORD_THREADS=1..4` and
`BB_VK_SEGMENT_KB=16/32/64/128`, same spot, plus the `1 << 58` A/B inside one run; read the
`Draw pipe` and `Recording: N submissions/frame, M command buffers each` lines.

**Estimated impact:** the least bounded number in this file, and the one the whole CPU-side list
hangs on. What *is* measured is the headroom: 545% of 800% used at 4c/8t, i.e. roughly a third of the
machine idle while the frame is limited. If half of that idle capacity came back the gain would be on the
order of 10–20%; if the chain only partly serialises, much less. Watch for the floor case —
measurements here were taken at 209 FPS in patch mode, and at a 30 FPS target on the Deck the GPU
may bind before the CPU does, in which case items 1 and 8 outrank this.

**Verify.** The `Draw pipe` line in `BB_FRAME_STATS` (stage A waiting, stage B busy); the same
`taskset -c 0-3,8-11` protocol the earlier A/Bs used; screenshots per phase.

## 3. Move the buffer side of a draw from stage B to stage A

**Why.** `docs/parallel_gpu.md` ends with exactly this as the next step: buffer binding is ~12% of
stage B (`ObtainBuffer` ~11%) while stage A "waits most of the time". Stage B is the limit, so
handing a seventh of its work to the under-used stage is the best ratio available short of item 2.

**How.** Stage A owns the buffer cache and hands its commands to stage B, as the constant ring
already does for small read-only buffers (`vk_constant_ring.h`).

**Estimated impact:** ≤12% of stage B, *share of a stage*, with `ObtainBuffer` ~11% of it the bulk.
End-to-end that is roughly 2–6%: stage B is the limiting stage so its busy time is close to the
frame in the CPU-bound case, but the seam between the stages needs a barrier and stage A is not
free — the doc's "waits most of the time" means it has room, not that the transfer is costless.

**Verify.** Stage A/stage B busy split in `BB_FRAME_STATS`; `BB_PIPE_VERIFY=N` for register-file
equality across the stages; frame rate A/B in one run.

## 4. ~~Deck-targeted build: `-march`/`-mtune` + PGO profile~~

**Done** — `build_steam_deck.sh` (commit `622e59d`): build.sh plus `-march=znver2 -mtune=znver2` on
every compiler invocation and on CMake's `CMAKE_C(XX)_FLAGS` (so the subprojects get it too). Still
open: the ≈4–5% below is a desktop measurement and the profile in `pgo/` is still desktop-trained
(`BB_PGO=generate` on the Deck, a few minutes of play, rebuild). The two AppImages that carry this
script's binaries — 0.51.1 and the 2026-10-18 flightrec package — have not been played, and the
package that was played for hours predates the script. What a package carries and what the host
must supply is recorded in [APPIMAGE_DEPENDENCIES.md](docs/APPIMAGE_DEPENDENCIES.md).

~~**Why.** There is no `-march`, `-mtune` or ISA level anywhere in `build.sh`, `gpu/CMakeLists.txt`,
`shell.nix` or `packaging/` — deliberately, "the same build runs on the Steam Deck"
(`docs/parallel_gpu.md`, "LTO and PGO"). PGO only engages when `pgo/*.gcda` exists
(`build.sh:27-31`), so the shipped AppImage carries no profile. Measured in the same section:~~

| build | FPS | GPU thread µs/draw |
|---|---|---|
| no LTO, no PGO | 73.2 | 7.72 |
| LTO | 74.4 | 7.58 (−1.8%) |
| LTO + PGO | 76.3 | 7.35 (−4.8%) |

~~**How.** A Deck package built with `-march=znver2 -mtune=znver2` (or `-march=x86-64-v3`, which the
Deck satisfies) plus a profile trained on the Deck (`BB_PGO=generate bash run.sh`, a few minutes of
ordinary play, then the plain build). Either ship it as the Deck AppImage or keep a baseline package
alongside it; a `x86-64-v3` baseline is safe for any CPU of that generation.~~

~~**Estimated impact:** ≈4–5%, *measured* — the table above is 73.2 → 76.3 FPS and −4.8% µs/draw for
LTO+PGO, with LTO alone worth −1.8%. That measurement is a desktop build with no `-march` at all, so
it is the floor rather than the total: the Deck adds AVX2 to the copy, hash and page-tracking loops
that currently get baseline codegen, which is where the extra should show up. This is also the only
item in the file whose gain arrives without touching rendering behaviour, which is why it is first
in the suggested order.~~

~~**Verify.** The `Frame stats` field "GPU thread us/draw" tolerates scene differences better than FPS
(that is how the table above was made); alternate builds across restarts because run-to-run spread is
±1.5 FPS.~~

## 5. Guest copies off the GPU thread's critical path

**Why.** The GPU thread is blocked ~9% in `Scheduler::WaitHostCopies` — ~130 times per frame at
EOP/EOS events, `WriteData` and submissions — plus ~4% in `BbCopy::WaitAsync`
(`docs/parallel_gpu.md`, "Second pass"). The doc's own suggestion for "What would scale": let the
idle draw-preparation workers drain a copy queue. On the Deck those are four `SCHED_IDLE` workers
with spare capacity.

**Note from an attempt that failed.** Running small copies on the idle *recording* thread moved the
copying onto the GPU thread instead (86.5 vs 87.2 FPS) and was reverted — the queue must be drained
somewhere that does not compete with the GPU thread.

**Estimated impact:** ≤13% of the GPU thread's time, *share of a stage* (9% `WaitHostCopies` + 4%
`WaitAsync`) — but a ceiling, not a forecast. The GPU thread must not be the last stage for the full
13% to reach the frame, and the failed attempt is the warning that matters: moving the same work to
the idle *recording* thread made things slightly worse (86.5 vs 87.2 FPS). Realistically 3–6% if the
queue is drained by the four `SCHED_IDLE` workers that genuinely have spare capacity.

**Verify.** The blocked-share fields in `BB_FRAME_STATS`; copy stats (`BB_CE_STATS`/copy counters);
frame rate and screenshots.

## 6. Cheaper object motion vectors

**Why.** Measured at ~10% FPS (155 vs 141 FPS with `object_motion=0`); stage B spends ~0.28 µs more
per packet on bone-palette hashes, the index range cache and motion pipeline variants. Needed for
temporal upscaling, so it is not a quality knob to drop — but on 4 cores 10% is material.

**How.** Reuse the previous frame's motion for draws whose pose buffer is unchanged (the pose hash
and `Motion::IndexRangeCache` already exist), or compute the vectors on the GPU.

**Estimated impact:** 10% is the *measured* ceiling (155 vs 141 FPS with `object_motion=0`) and it
is a ceiling, not a target — motion vectors are required for temporal upscaling, so this is strictly
about making them cheaper. The time being attacked is ~0.28 µs per packet of stage B on the
bone-palette hash, index-range cache and motion pipeline variants; recovering part of that for the
draws whose pose is unchanged lands in the 3–6% range, with the rest of the distance back to 10%
only reachable by moving the work to the GPU.

**Verify.** `BB_DEBUG_MOTION`, the motion vector test targets
(`motion-history-test`, `motion-shader-test`), and frame rate A/B.

## 7. Scope `-fno-omit-frame-pointer` to the loader

**Why.** `gpu/CMakeLists.txt:12` sets it globally for the GPU library; its purpose is the loader's
thread dumps and the watchdog. On a CPU-bound workload it costs a little on every call.

**How.** Build `bb-probe`/`shim` with frame pointers, the hot GPU library without — or make it an
option. **Verify:** the loader's thread dump tests still walk stacks; the µs/draw field.

**Estimated impact:** ≈0–2%, in practice usually under 1% — on x86-64 with the register pressure of
these loops, keeping a frame pointer is close to free, and on a workload that is memory-bound part
of the time it can be free outright. This is the smallest item in the file; fold it into another
change rather than measuring it on its own.

## 8. Occlusion and predication done by the GPU

**Why.** The game issues ~60 occlusion events a frame. README says the new model turns them into
Vulkan occlusion queries, while the same README's debug-menu section says they "still use synthetic
pixel counters (`PixelPipeStatDump`)" and that `IT_SET_PREDICATION` is unimplemented — the two
statements do not agree, and the earlier note in `EMULATION_REMOVAL_PLAN.ru.md` records the opt-in
Vulkan-query path flashing the lamp glow grey with the upscaler.

**How.** First establish which path is active where (`vk_occlusion.cpp`, `BB_OCCLUSION_QUERIES`,
`BB_OCCLUSION_TRACE=1`), then make the counter stable enough to default on, and implement
predication. **Verify:** `BB_PM4_SELFTEST=1` (occlusion interval checks), read traces, frame rate.

**Estimated impact:** unquantified, and it genuinely cuts both ways — which is why the state has to
be established first rather than assumed. If the game is currently issuing draws that real hardware
would have culled, occluding them properly could remove a large slice of GPU work, and on 8 CUs that
would make it the biggest win in this file. If the counters are already cheap and nothing is
mis-culled, the gain is ~60 events per frame of CPU and effectively zero. Against that, the one
time the real-query path was tried it broke the lamp glow's colour under the upscaler
(`EMULATION_REMOVAL_PLAN.ru.md`), so the cost of getting it wrong is a visual regression, not just
a slider. Note that "Measure before optimising" already books it on the GPU side; that is a working
assumption from the culling argument above, not a finding, and the same section's decision rule is
what should settle it.

## 9. AOT shaders/pipelines, and ship the pipeline cache

**Why.** Roadmap item: "Shaders translated ahead of time, at install, not during play".
`vk_pipeline_serialization.cpp` and `BB_ASYNC_PIPELINES` already exist and `run.sh:31` raises
`MESA_SHADER_CACHE_MAX_SIZE=4G`; what is missing is compiling during install and shipping the result
in the AppImage. This attacks hitches and first-run stutter (frame time), not average FPS.

**Estimated impact:** ≈0 on average FPS — by design, since this removes *compilation* from the
play session and not work from the frame. What moves is frame time: the first-run cost to reach the
title screen, and the hitch spikes during traversal and area loads. On the Deck the perceived gain
is larger than the number suggests, because a 4-core CPU has no spare headroom to absorb a compile
in the middle of streaming, so the stutter is what a player notices most. Measure this one in
stall lines and seconds-to-title, not in FPS.

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

**Estimated impact:** ≈0% on steady-state FPS, and a few percent at most during traversal, where it
acts on the 40–50 ms area-load frames and not on the 25–33 ms ordinary ones. The guards trade
residency for fewer re-uploads (keeping images alive longer costs memory, which is the scarce
resource here), so the payoff is fewer or milder spikes rather than a higher average — and it is
bounded by the composition of those frames: ~30 ms of stage-B draw work and ~12 ms of copies, most
of which items 2, 3 and 5 are the actual fix for. Do this after them, and only if the pressure
report still shows re-upload thrash.

**Verify.** The pressure report in `BB_FRAME_STATS` (including the count of images kept as in use),
stall lines, and VRAM as reported by `VK_EXT_memory_budget`.

## 11. Draws with nothing to render into

**What they are.** Both Deck logs (`dist/deck-copytrace/LOGS`, `dist/deck-copydest/LOGS`) carry two
labels in most windows:

```
0.171 ms/frame   6.0/frame  pass 16384x16384 [] depth - vs 0000000089cda9e4 ps 0000000000000000
0.405 ms/frame   1.2/frame  pass 16384x16384 [] depth - vs 0000000089cda9e4 ps 00000000e54449c7
```

`[]` is an empty attachment list and `depth -` no depth target, so the draw has nothing to render
into: 8–12 of the first shape a frame (0.17–0.65 ms/frame) and 1–2 of the second (0.13–0.65
ms/frame). The `16384x16384` is not a real area — with nothing attached the render state keeps the
device's maximum extent (`vk_rasterizer.cpp`, `BeginRenderingFull`: the extent starts at
`GetMaxFramebufferWidth/Height` and is only narrowed by each attachment), so the size in the label is
the port's own doing.

**What it costs.** Per pass, from the labels above: the first shape is cheap (0.236 ms/frame over
8.3 draws = ~28 µs each), the second expensive (0.526 ms/frame over 1.5 = ~350 µs each), so of the
three quarters of a millisecond most of it is the 1–2 draws with a fragment stage. Both shapes run
with the port's maximum render area, so the driver rasterises a 16384² area with nothing attached
for each one.

**Why they should be skipped.** The old host skipped exactly these two shapes
(`old-bbhost-nocrash/src/host/render.cpp`, the `host_gpu_draw` path: "nothing to render into" and
"no colour, no depth write: nothing observable", counted in `draws_empty` and reported at exit), so
there is precedent on this game. The port draws them instead, and each one opens a render pass and
records a draw. The change is `Rasterizer::DrawUnobservable`: a draw with no color attachment and no
depth or stencil target whose stages write no memory (no buffer store of any kind, no image store,
no atomic) is counted and dropped — no pass, no pipeline, no descriptor set. Nothing about it can be
observed. Gate: experiment bit **33** of `BB_TOGGLE_FILE`'s second number, so the reference
behaviour is the bit clear and the whole thing A/Bs inside one run.

**The guard has to know the port's own buffers.** The attachment test is easy (`key.mrt_mask` is the
fragment shader's exports and drives `state.num_color_attachments`, `db_desc` is set only with
`depth_enable` or `stencil_enable`), but "writes no memory" is not: a shader reached through the
page table, the fault buffer, the flattened constant buffer or the clip planes carries `is_written`
for those, and a first version of the guard counted them — which would have kept every draw in
`BB_GUEST_IN_PLACE` runs and made the whole experiment a no-op that looked like "nothing
happened". Only guest buffers (constant, vertex, stream, GDS), image stores and atomics count. A
dropped draw also skips `PostDraw`: it reads no vertex buffer, writes no range and needs no note.
The one way a draw like this can still be seen is the game's occlusion queries
(`BB_OCCLUSION_QUERIES=1`): with nothing attached every fragment passes the depth test, so a draw
they measure is recorded as before (the counter is not part of the reference runs, where the
counted sequence is written when decoded).

**Measured prize:** 0.6–0.8 ms/frame in the fight windows of `deck-copytrace`, up to 1.2 ms in the
heaviest — ≈3–6% of a 17–23 ms Deck frame, more than everything goal 1 had left (≈1–3%). The
2026-10-17 Deck A/B was by eye only — frames matched with the bit on and off, which is what left it
on by default; the `frames.csv` from that run was not read. Not yet verified: whether the prize
converts to frame time where the GPU is the limiter, and whether the guest reads anything back that
those draws produced. The guest-side read is what `no_target_vertices`
answers: those draws carry the game's own vertex counts, and a `VS` that writes a buffer would have
kept the draw.

**Verify.** `no-target draws skipped N/frame` in `Frame stats` (≈10 in the heavy area with the bit
set, 0 without it); both `pass 16384x16384 []` labels gone from `BB_GPU_PROFILE`; `frames.csv` on
and off in the same spot; screenshots identical. If the counter stays at 0 with the bit set, the
reason is printed instead: `No-target draw kept: vs ... ps ... (a stage writes memory)` — one line
per pipeline, the first eight, so a run that changes nothing names its own cause. Package:
`dist/deck-notarget` (rebuilt after the guard fix: `dist/deck-notarget/gpu/libbbgpu.so`,
`dist/deck-notarget.tar.gz` and the AppImage's
`usr/share/bbport/bin/gpu/libbbgpu.so` are all md5 `ebb5e278…`).

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

Decision rule: if GPU busy time is at or near the frame time, start at item 11 and then item 1. If
the serial CPU chain sits at 90%+ while cores idle, start at items 3 and 5 (item 2 is already
built, and item 11 is the one measurement away from being settled).

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

1. ~~**Item 4** — a few build lines, no rendering behaviour touched, measurable with the existing
   µs/draw counter.~~ **Done** (`build_steam_deck.sh`); the next measurement on a Deck confirms it.
2. **Item 11** — the measured GPU-side win that is left (3–6%), one Deck A/B away; then **item 1**
   (≈1–3%, built and measured).
3. **Items 3 and 5** — both reduce what stage B and the GPU thread must do, without the large
   refactor of item 2.
4. **Item 2** — already built (two recording threads on the Deck); sweep `BB_VK_RECORD_THREADS` and
   `BB_VK_SEGMENT_KB` where the CPU does bind, rather than reworking it.
