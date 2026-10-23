# Performance goals — Steam Deck first

Optimisations worth doing that are **not** quality reductions (no preset, resolution or upscaler
downgrades). First compiled 2026-10-15; this version rewritten **2026-10-21** for
`deck-0.6-crashfixes` (0.6's own renderer: `gpu/render`, `gpu/textures`, `gpu/gcn`, `gpu/cp`), and
checked line by line against that tree, with §1 ported and §4 implemented on **2026-10-22**, and
both of those measured on a real Deck on **2026-10-23**. Every item below is a measurement that
still stands, a missing piece of code, or a knob that has not been swept.

Sources: [parallel_gpu.md](docs/parallel_gpu.md) (the CPU profile's design notes),
[CHANGES_0.6.md](docs/CHANGES_0.6.md), [CHANGES_after_0.5.md](docs/CHANGES_after_0.5.md),
[SESSION_2026-09-30.md](docs/history/SESSION_2026-09-30.md) (the Deck copy-shader work),
[CRASH_FIXES_STATUS.md](CRASH_FIXES_STATUS.md) (which branch carries what), the `dist/deck-*` logs
and the source itself.

## Measurement ledger

Three kinds of number appear here, and each is labelled where used.

| Kind | What it is | Trust |
|---|---|---|
| **Deck** | A real Steam Deck run: the 2026-10-16 traces and sessions in `dist/deck-copytrace/LOGS` and `dist/deck-copydest/LOGS` | Highest available. Frame times there are 17–23 ms in the recorded windows |
| **Desktop** | The project's A/Bs on an RX 7800 XT, some with `taskset -c 0-3,8-11` as a 4c/8t stand-in | Sound for direction, not for magnitude on Deck |
| **Extrapolation** | A desktop figure converted to Deck, or a share of one stage turned into a share of a frame | Weakest; always spelled out, never a promise |

The newest Deck numbers are **2026-10-23**: two runs of `dist/Bloodborne-bbport-0.6-znver2-occlusion.AppImage`
(the znver2 build with §1 and §4 in it) on the same Deck — 60 s of load-and-look, then 8.7 minutes
of play (`~/bbwork/logs/bb-occlusion-tests-run2`). **No crash, 0 guest faults**, 28,281 frames:
**mean 18.41 ms (54.3 FPS), p50 17.95, p95 24.89, p99 28.30 ms**; 535 draws and 56 submissions a
frame, stage A 7.8 ms a frame. In-area windows sit at 48–58 FPS; the 60 FPS stretch at the end is
a boss arena with water to the horizon and no geometry (59.7 FPS, GPU thread idle 65–72% at ~350
draws a frame) and says nothing about speed. Those are levels, not deltas: nothing here is A/B'd
against a build without §1/§4 yet. §1's and §4's own numbers are in their sections.

New on 2026-10-21, and the only measurement in here taken on neither a Deck nor the project's
7800 XT: a standalone CPU benchmark of the port's hot loop shapes, built twice
(`-O2` and `-O2 -march=znver2 -mtune=znver2`), run on an **Intel i7-4770** (Haswell). It exists to
settle the build-flag question (§11) and it says nothing about the GPU:

| case | `-O2` ns/iter | `znver2` ns/iter | Δ |
|---|---|---|---|
| `hash_16k` (XXH3 over 16 KiB) | 17 158 | 6 651 | **−61%** |
| `page_scan` (1 cache line / 4 KiB page) | 329 385 | 258 473 | **−22%** |
| `clamp_1080p` (per-byte min/max) | 430 125 | 366 547 | **−15%** |
| `cp_decode` (packet dispatch) | 138 353 | 128 670 | −7% |
| `detile_1080p` | 1 517 431 | 1 422 958 | −2…−6%, not settled |
| `copy_1k` | 13 099 | 15 669 | **+20%** (slower) |
| everything else (64 B / 64 KiB / 1 MiB copies, fill, scale, draw math) | | | ±2%, i.e. noise |

Geomean 89%. Within-binary spread across two runs is under 2% on all but `detile`/`page_scan`
(~6%). The `copy_1k` regression is the interesting one: `-mtune=znver2` picks `rep movsb` for the
small copy, which loses to SIMD copies on Haswell. Whether it also loses on Zen 2 (where `rep movsb`
is good above ~256 B) is exactly what a Deck run answers — and §11 is written around it.

## What the Deck changes

- 4 cores / 8 threads, AVX2, no AVX-512 → the serial CPU chain is the hard constraint. The
  two-stage draw pipeline turns itself on at 8+ hardware threads (`DrawPipeWanted`,
  `gpu/render/raster_pipe.cpp:56`), so it is on there.
- RDNA2, 8 CUs, unified 16 GB LPDDR5 shared with the CPU → bandwidth-bound work costs far more than
  on a desktop card, and "VRAM" pressure is real system pressure. The background pre-upload is
  inert in the default configuration: `PreuploadMode` returns 0 while the port reads the game's
  memory in place (`gpu/buffers/buffer_cache.cpp:462`, `BB_GUEST_IN_PLACE` — on by default through
  `BB_PC_MODEL=1`, `run.sh:159`), and `run.sh:137` only sets `BB_PREUPLOAD=1` for the old model.
- 1280×800 screen, usually driven at 720p. At any output other than 1080p the port uses the
  **startup resolution patch**, which is the fastest path; the live scene-resolution path
  (`BB_SCENE_RESOLUTION`) engages only on 1920×1080 scene targets (`SceneTargets::EligibleScene`,
  `gpu/upscale/vk_scene_resolution.cpp:95`) and stays off on Deck.

## Goals at a glance

| # | Goal | Side | Expected impact | Effort | State in this tree |
|---|---|---|---|---|---|
| 1 | Skip draws with nothing to render into | GPU | **0.6–1.2 ms/frame (3–6%)** — Deck, 2026-10-16 | Small | **Ported 2026-10-22, engaged on Deck 2026-10-23** (16–17 no-target draws a frame skipped); A/B with `0 0` still to run |
| 2 | The remaining emulated `3d5ebf4e` copy class (`704x1x1`) | GPU | 0.21–0.35 ms/frame (~1–2%) | Small | HLE detector is in and on; this class declines it |
| 3 | Move buffer binding from stage B to stage A | CPU | ≤12% of stage B ⇒ ~2–6% end-to-end, **and stage B is 30–66% busy on Deck** so it is a p99 item | Medium | Planned, not started (§3 has the order of work); the stage-B gate is answered, the µs split is not |
| 4 | Occlusion queries + predication: settle the pair on Deck | GPU + CPU | 0.1 ms/frame at most from the label half; the queries' culling is real, its price unmeasured | Small to measure | **Both in and on; the narrowed label ordering (`=2`) is measured at 19% of the pass breaks it was built for (~0.1 ms/frame) and is now off by default (2026-10-23). What is left is the queries' own A/B** |
| 5 | Guest copies off the GPU thread's critical path | CPU | ≤13% of the GPU thread; 3–6% realistic | Medium | Partly built (deferred uploads, parallel copies); waits still unmeasured |
| 6 | Cheaper object motion vectors | CPU | 10% desktop ceiling; **≈3% of a Deck frame** left after the gate | Medium | **Pose-reuse gate already in** (`Motion::History::Moving`, `IndexRangeCache`); see §6 |
| 7 | Sweep the recording knobs that already exist | CPU | Unmeasured; +18% was the desktop stand-in | Small | Built: `BB_VK_RECORD_THREADS`, `BB_VK_SEGMENT_KB`, `BB_ASYNC_SUBMIT` |
| 8 | Scope `-fno-omit-frame-pointer` to the loader | CPU | ~0–1% | Small | Open (`gpu/CMakeLists.txt:12`) |
| 9 | AOT shaders/pipelines; ship the port's pipeline cache | Both | ≈0 average FPS; removes traversal hitches | Medium | Preload on start exists; install-time compile does not |
| 10 | Deck-tune the texture collector under the driver's budget | Memory | ≈0 steady state; fewer area-load spikes | Small | Budget-derived marks already follow the integrated budget |
| 11 | Tune the spirit of `-march` with `-mtune`, not only the target | CPU | Keeps AVX2, drops the `rep movsb` regression | Small | Build knob exists (`BB_DECK_MARCH`); no `-mtune` sweep |

## Where the branch stands

- **Already on by default, no action needed:** HLE compute image copies
  (`IsComputeImageCopy`, `gpu/render/raster_compute_hle.cpp:59`; `BB_HLE_OFF=1` disables,
  `BB_HLE_STATS=1` counts), two Vulkan recording threads on Deck (`RecordingThreadCount`,
  `gpu/render/scheduler.cpp:96`), async submit (`BB_ASYNC_SUBMIT`, default on), async pipeline
  compilation (`BB_ASYNC_PIPELINES`, default on, `gpu/render/pipeline_cache.cpp:765`), occlusion
  queries (`BB_OCCLUSION_QUERIES`, default on) with predication implemented in the command
  processor (`gpu/cp/cp_graphics.cpp:378`), the two-stage draw pipeline, occlusion results ordered
  before every label as on the PS4 (the narrow `LabelPolls` mode is `BB_OCCLUSION_LABEL_ORDER=2`,
  §4, measured at ≤0.1 ms/frame on Deck and therefore not the default), and the new memory model
  on every GPU (0.6: Yharnam 4.30 vs 4.60 ms a frame, Oedon Chapel 234 vs 213 FPS on a 7800 XT).
- **Built, never A/B'd on Deck:** the segmented recorder (§7) and the HLE copy detector (§2's
  measured part).
- **Not in this tree at all:** the crash recorder (`src/runtime_crash.c`, same branch) and the
  fork's GC retention rules.
- **One diagnostic to keep in mind:** `BB_FRAME_STATS` now writes a per-frame CSV
  (`BB_FRAME_LOG`) whose columns are the ones to compare between builds —
  `t_s,frame_ms,present_wait_ms,tick_wait_ms,stage_a_cpu_ms,images,image_mb,buffer_mb,protect_ms,`
  `resident_ms,create_ms,refresh_ms,write_fault_ms,read_faults,guest_copy_mb,draws,dispatches,`
  `submissions,compiles,preupload_mb,kernel_ms,alloc_mb,free_mb`
  (`gpu/platform/videoout/frame_stats.cpp:240`).

## 1. Skip draws with nothing to render into

**Ported 2026-10-22: on by default, needs the Deck A/B below.** `Rasterizer::DrawUnobservable`
(`gpu/render/rasterizer.cpp`) runs in `Draw`/`DrawIndirect` right after the pipeline selection, so a
dropped draw never becomes a packet: stage B does not see it and its register writes stay dirty for
the next draw's packet. The test is the one below — `key.mrt_mask == 0`, no depth/stencil target
(`depth_enable`/`stencil_enable` with a valid `depth_buffer`, the same expression
`PrepareRenderState` uses), and no stage writing memory (`Shader::Info::buffers`/`images`, the
port's own flatbuf/pagetable/fault buffer/clip planes excluded), with the pipeline named once by
the kept path. Experiment bit 33 (`1ull<<33`, `EXPERIMENTS_ON_DEFAULT` in `src/runtime_memory.c`);
`BB_TOGGLE_FILE` with `0 0` is the reference. One deliberate difference from the flightrec build:
the old guard also kept every draw while occlusion queries were on, which here would disable the
skip every frame (queries are on by default in 0.6). It is dropped instead: a ZPASS counter belongs
to a depth block, and a pass with no depth target writes none on the PS4 either, so the segment
this pass would open only counts samples the PS4 never counted.

**It is already here, and it is the same one as on `nocrash-optimizations`.** That branch has it as
`d1038c7`, in the 0.51 renderer (`gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp` — the
`old-bbhost-nocrash` "nothing to render into" this section started from), and 0.6's own renderer got
the port as `708a6c9`: same bit 33, same criteria, the occlusion difference above, the counters in
the same place. Nothing from the older branch is missing — `708a6c9` is an ancestor of this branch
and the run below shows it firing 16–17 times a frame. The one thing it lacks for the launcher is an
env var: today it is `BB_TOGGLE_FILE`'s second number (`0 0` is the reference), which a launcher
switch cannot set per run.

**Seen working on Deck 2026-10-23.** `no-target draws skipped` sits at **16–17/frame** in 85 of the long
run's 104 windows, 13–18 through the rest of the game, 11 through a load and 1–2 on menus and the
title — above the ~10 this text guessed at. That is the skip happening, not its price: no
`0 0` reference side was run, so 0.6–1.2 ms/frame is still the 2026-10-16 Deck number, and
`BB_GPU_PROFILE` was off in both runs, so the `pass 16384x16384 []` labels were not checked.

**What it was.** On `crash-logger-build`, from the 0.51 renderer; the description below is that
build's.

**What they are.** Both Deck logs carry two labels in most windows:

```
0.171 ms/frame   6.0/frame  pass 16384x16384 [] depth - vs 0000000089cda9e4 ps 0000000000000000
0.405 ms/frame   1.2/frame  pass 16384x16384 [] depth - vs 0000000089cda9e4 ps 00000000e54449c7
```

`[]` is an empty attachment list and `depth -` no depth target, so the draw has nothing to render
into: 8–12 of the first shape a frame and 1–2 of the second. `16384x16384` is not a real area — with
nothing attached the render state keeps the device's maximum extent and is only narrowed by each
attachment, so the size in the label is the port's own doing.

**What it costs.** From the labels: the first shape ~28 µs per draw, the second ~350 µs. Both run
with the maximum render area, so the driver rasterises a 16384² area with nothing attached. The old
host skipped exactly these two shapes (`old-bbhost-nocrash/src/host/render.cpp`, "nothing to render
into", counted in `draws_empty`), so there is precedent on this game.

**What was measured.** 0.6–1.2 ms/frame in the fight windows of `deck-copytrace`, up to 1.2 ms in
the heaviest — **3–6% of a 17–23 ms Deck frame**, more than everything §2 had left. The 2026-10-17
visual A/B was by eye only, which is what left it on by default; the `frames.csv` from that run was
never read.

## 2. The remaining emulated `3d5ebf4e` copy class

**What is already done.** The HLE detector recognises a compute shader that is a dword copy between
two registered images of the same layout and records one image copy instead
(`IsComputeImageCopy`, `gpu/render/raster_compute_hle.cpp:59`; called unconditionally from
`BindResources`, `gpu/render/raster_bind.cpp:259`). On the 2026-10-16 Deck trace it was turning
**95 of 107** sampled dispatches into image copies. Both declining classes the trace found
(128×128 / 65 536 B and 856×480 / 1 835 008 B) are handled by the build that registers the missing
destination image from the source's layout (`dist/deck-copydest`, 31.7 s in the heavy area), and the
class the baseline had running — `dispatch cs 3d5ebf4e (256x1x1 groups)`, 2.5–5.0 dispatches per
frame, 0.20–0.46 ms/frame — stops running altogether.

**What is left.** One class the trace never sampled: `704x1x1`, 1.2–2.0/frame, **0.21–0.35 ms/frame
(~1–2% of a Deck frame)**. Whether the detector can take it is open — it is the one size the trace
never saw. Record a window with `BB_IMAGE_COPY_LOG=1` and `BB_HLE_STATS=1` on Deck, read what declines it
(`FindImageFromRange` for source or destination, or the `MaxTail` / pitch / bit-depth checks), and
either extend the detector or accept the loss.

**How to measure the whole item.** `BB_HLE_OFF=1` in the same spot is the A/B: the compute copies
run as the game's shaders. That is the cleanest way to price what 0.6 now does by default.

**Verify.** `BB_HLE_STATS=1` prints the five-second split (image copy / meta clear / image clear /
other binds); `BB_GPU_PROFILE=1` should show the `3d5ebf4e` dispatch label shrinking to the residual;
`frames.csv` and screenshots.

## 3. Move the buffer side of a draw from stage B to stage A

**Why.** `docs/parallel_gpu.md` ends with this as the next step: buffer binding is ~12% of stage B
(`ObtainBuffer` ~11%) while stage A "waits most of the time". [Desktop, 0.51 CPU profile] Stage B is
the limiting stage, so handing a seventh of its work to the under-used stage is the best ratio
available short of more recording threads.

**State in 0.6 (checked 2026-10-22).** The 12%/11% are 0.51 numbers on a 7800 XT and have not been
reproduced here; three things changed under them:

- the constant ring already hands small read-only buffers to stage A (`CollectRingBindings`,
  `gpu/render/raster_pipe.cpp:296`: ≤16 KiB, read-only, not GPU-modified — unless
  `BB_CONSTANTS_IN_PLACE=1`);
- `ObtainBuffer` grew memo paths that return without doing work in the default in-place model
  (`gpu/buffers/buffer_cache.cpp:1129`: the layer `BindMemo` hit, the in-place read memo);
- vertex streams and the index buffer are resolved inside `BindResources` beside the texture helper
  (`draw_inputs`/`EarlyDrawInputs`, `gpu/render/raster_bind.cpp:383`), so part of the old
  `ObtainBuffer` time is already overlapped.

What still runs on stage B per draw: `ObtainBuffer` for the vertex ranges
(`gpu/render/raster_bind.cpp:566`), the index buffer (656) and every guest buffer the ring did not
take (893), plus the barrier bookkeeping (`bound_buffers`, `runtime.AccessBuffer`) and the
descriptor writes.

**Not the same change as the ring.** The ring moves *data*, for bindings whose result is a copy it
can make itself. The rest mutate the buffer cache — residency, `LayerBind`, uploads, shadow copies,
GPU-write notes — and record commands on stage B's scheduler, so "binding on stage A" means stage A
owning the buffer cache (what `docs/parallel_gpu.md` says), with the cache-coherency risk that
produced the 2026-10-01 heap corruption.

**Why it may not pay on Deck.** The Deck frame is GPU-bound, and the presenter keeps one frame in
flight: stage-B CPU time below the frame time hides behind the GPU, so cutting it moves p99/hitches
rather than average FPS. This is worth doing only if the measurement below puts buffer binding
above the GPU's frame time.

**Measure first, no code.** `BB_SECTIONS=1 BB_FRAME_STATS=1` prints `stage B us per packet:` with
`bind`, `buffers`, `ObtainBuffer`, `vertex`, `index`, `textures` and the `A: *` stage-A parts
(`gpu/render/raster_pipe.cpp:1029`), next to `stage B busy %` and `stage A waited %`. In the heavy
area on Deck: a few percent for `ObtainBuffer`+`vertex`+`index` with stage B below the frame time
ends this item (§2 and §4 are then the ones worth spending on); stage B above the frame time makes
extending the ring pattern to the vertex/index ranges the cheapest half of it.

**That gate was answered on 2026-10-23, and it says no for FPS.** In the 8.7-minute run `stage B
busy` was a median **48.8%** of the GPU thread's active time (30–66% across the windows), `stage A
waited` 2.6–8.1%, and stage A 7.8 ms of an 18.4 ms frame. Stage B sits under half of a frame the GPU
owns, so moving its `ObtainBuffer`/`vertex`/`index` work to stage A cannot raise FPS on this build;
it is a p99 item at best, and nothing in the run points at it as a p99 problem. `BB_SECTIONS` was
off, so the split *inside* stage B is still unmeasured — that is step 1 below.

**If it is picked up, in this order (planned, not started):**

1. **One Deck run with `BB_SECTIONS=1 BB_FRAME_STATS=1`** in the heavy area for the `bind` /
   `ObtainBuffer` / `vertex` / `index` µs per packet. Without it the rest is guesswork, and this
   run's numbers say it is a p99/hitch item, not an FPS one.
2. **Cheap half, small:** give the constant ring the vertex and index ranges. `CollectRingBindings`
   (`gpu/render/raster_pipe.cpp:296`) already copies ≤16 KiB read-only, non-GPU-written buffers on
   stage A; the game's index buffers and most of its vertex streams are small and read-only, so the
   criterion exists — this is a per-draw lookup on stage A plus a stage-B skip when the ring holds
   the range, not the buffer cache.
3. **The real item, large:** stage A owning the buffer cache — residency, `LayerBind`, uploads,
   shadow copies, GPU-write notes, and recording on stage A's scheduler. That is the change that
   produced the 2026-10-01 heap corruption; it needs a per-stage cache split or an explicit handoff,
   not a moved call. It is why this section waits for a measurement rather than a hunch.
4. **Whichever part lands, behind experiment bit 34**, so the launcher has a switch and the run has
   an in-run A/B from day one. `BB_PIPE_VERIFY=N` stays the correctness net (register-file equality
   across stages, `gpu/render/raster_pipe.cpp:49`).

**Verify.** `stage_a_cpu_ms` in the per-frame CSV and the `stage A waits over 0.5%: function:line
N%` line printed with `BB_FRAME_STATS` (`gpu/render/raster_pipe.cpp:1114`); `BB_PIPE_VERIFY=N` for
register-file equality across the stages (`gpu/render/raster_pipe.cpp:49`); `frames.csv` A/B.

## 4. Occlusion queries and predication: settle the pair

**Implemented 2026-10-22: a label ends the pass only where the game reads labels.** Before this,
`WriteBeforeLabel` ran for *every* end-of-pipe label written by the GPU while queries were pending:
`scheduler.EndRendering()`, an `endQuery`, `copyQueryPoolResults` with `eWait`, the resolve dispatch
between two full `eAllCommands` barriers, `resetQueryPool`, and a new segment. A pass break stores
and reloads every attachment (at 720p: 3.7 MB colour and as much depth), and with ~9 events and
~21 labels a frame (measured 2026-10-23, below; this text used to say ~60 events) the flush was only
ever needed for the labels the game polls — the ordering the PS4 gives, where a label ahead of the
counters would let the game read them stale.
GPU-side waits never needed it: the resolve and the label are in one stream.

So the port now learns which labels those are. A page a label was written to is watched for its
first CPU access (any read or write, any thread, one trap per page — `LabelPolls`,
`gpu/render/vk_occlusion_labels.h`). Until that access its labels leave the results to the next
boundary; from then on they are ordered as before, one pass break and one resolve each, exactly as
many as the game reads. Past 256 watched pages a page cannot be watched any more and is ordered
unasked, so a game whose labels are spread over more pages keeps the old behaviour rather than
losing the order.

**Flags (default changed 2026-10-23).** Default: before every label, as on the PS4 and as it was
before 2026-10-22 — the mode below is now `BB_OCCLUSION_LABEL_ORDER=2`, an experimental option
(measured, and not worth its traps; see the verdict). `=0`: never (statistics only, as it already
meant). The startup line says which, next to the existing "occlusion queries are Vulkan occlusion
queries" one.

**First Deck run of it (2026-10-22): a guest crash at the load, and why.** The build
(`dist/Bloodborne-bbport-0.6-znver2-occlusion.AppImage`) died on the first load from the title
screen: one RIP, one address inside direct memory, reported by GXWorker:1, GXWorker:3 and the main
thread at once (`docs/history/SESSION_2026-10-23.md`). `WriteBeforeLabel` arms the `QueryReads`
reason, which is a **no-access** trap (`TRAP_NO_ACCESS`, `src/runtime_memory.c:899`), on the page
holding the label — and label memory is the game's own small-object pool, written by every GXWorker
every frame. So arming it faults every thread touching that page, the first handler lifts the trap
for all of them, and `OnAccess` called the rest "not my trap" and returned false, which `probe.c`
turns into a fatal guest fault. The run died within a second of the queries going live (the
constructor's line and the fault are ~30 log lines apart), before the first five-second
`Occlusion queries:` line — so that run priced nothing in this item either way.

**Fixed 2026-10-23:** `OnAccess` retries the instruction when the page's trap is gone but its
mapping still allows the access (`StaleLabelTrapFault`), and counts it (`faults on an already
lifted label trap: N` in the five-second line). A page that is unmapped or still forbids the
access stays a guest fault. The fatal guest-fault line now prints the kernel's `err` code and the
page's trap reasons. `dist/Bloodborne-bbport-0.6-znver2-occlusion.AppImage` is rebuilt from it (the
crashing package is `dist/old/…-crashed.AppImage`). `BB_OCCLUSION_LABEL_ORDER=1` is the same build
with the trapping half removed (it orders before every label, arms no traps) — the way to test the
rest if this half ever needs to be taken out of a build again.

**Measured on Deck 2026-10-23 (the 8.7-minute run, 28,281 frames).** 253,161 events — 486/s, ~9 a
frame, up to ~2,090/s (~40 a frame) in the heaviest window — 8,809,175 segments, **0 not counted**,
and **0 counters reused before their results were written out of 252,755**. `faults on an already
lifted label trap`: **10 in the first 60 s** (the load the 2026-10-22 build died in) and **3 in the
whole long run**, with **0 guest faults in either run** — so that race is real, frequent at a load,
and now retried instead of fatal, which is what the diagnosis predicted.

**What the label half bought: 19% of what it was built for.** Labels written with an event pending:
16,837, of them **14,295 in a render pass**; **2,761 of those were left to the next boundary** and
**11,534 were still ordered**, i.e. 0.51 pass breaks a frame before the change and 0.41 after —
**0.10 a frame**. At 720p a pass break stores and reloads ~7.4 MB, so the whole saving is **well
under 0.1 ms a frame**: the premise in this section ("~60 events and a few hundred labels a frame")
was an order of magnitude above what the game does.

**Why so little: the game touches a label page immediately.** `pages read by the game` reached
~2,800 distinct pages over the run, and never more than **24 pages were watched at once** (cap 256,
so the fallback to ordering every label never engaged). A touched page is ordered from then on, so
"learn each page once" converges on nearly every page the game uses within a frame of a label
landing there. Buying the rest would take a per-label or per-query-set decision, not a per-page one
— not worth it for 0.1 ms/frame.

**The other half: do the queries pay?** **101,602 of 126,378 counter reads came back empty**
("none passed") against 24,776 with samples, so the counters really do hide geometry and the
game's predicated draws are skipped — the culling is there. What is still missing is its price:
`BB_OCCLUSION_QUERIES=0` on the same spot, `frames.csv` p50/p99 and `BB_GPU_PROFILE` GPU ms. That
is the one measurement left in this item, and it is worth more than everything the label half did.
`=2` and `=0` now bracket the label half if somebody wants it priced exactly; neither should need
another run otherwise.

**Switched off by default the same day.** `LabelPolls::ModeFromEnv` returns `Every` unless
`BB_OCCLUSION_LABEL_ORDER=2`, the traps and the retry stay in the tree for that mode, and the
failure the retry guards against is a path no default build takes. The reason is the two numbers
above: ~0.1 ms/frame bought by arming ~5 traps a second in the game's own label pool, on threads
whose timing the rest of this file is trying to protect. The narrow mode's numbers stand, and the
launcher can offer it as an experimental option (below) when somebody wants to spend the traps.

**The old version of this file was wrong.** It repeated 0.51's README ("synthetic pixel counters,
`IT_SET_PREDICATION` unimplemented"). In this tree the game's ZPASS_DONE events are real Vulkan
occlusion queries, on by default (`OcclusionTranslated`, `gpu/render/raster_signals.cpp:185`;
`BB_OCCLUSION_QUERIES=0` restores the fixed sequence), and predication is implemented in the command
processor (`Op::SetPredication` and `PredicationSkips`, `gpu/cp/cp_graphics.cpp:378,406`). Samples
are scaled to the game's resolution when a pass is drawn smaller
(`OcclusionQueries::SetPassScale`, `gpu/render/vk_occlusion.cpp:134`), and events are summed by one
resolve dispatch outside render passes (`gpu/render/vk_occlusion.cpp:170`).

**Why it still matters.** ~9 events a frame in play (40 in the heaviest window), each ending a query
segment, with a resolve dispatch per boundary, and the game's own draws predicated on the results —
so this is both cost and GPU culling, and the culling side is the one that is neither free nor
priced. `BB_PM4_SELFTEST=1` checks the event intervals; experiment bit 4 of the second
`BB_TOGGLE_FILE` number is the in-run A/B.

**Verify the change itself.** `occlusion-labels-test` (`ninja -C out/gpu occlusion-labels-test` then
`out/gpu/occlusion-labels-test`) covers the policy: the three modes, one watch per page, a touch
teaching a page, and the cap. In a run, the startup line names the mode and the five-second line the counts
above; `BB_OCCLUSION_READ_TRACE=1` still reports when the game first reads its counters and whether
they were there.

## 5. Guest copies off the GPU thread's critical path

**Why.** The GPU thread blocks in `WaitHostCopies` — at EOP/EOS events, `WriteData` and submissions
(`gpu/cp/cp_tasks.cpp:171,201,244`, `gpu/render/scheduler.h:348`). On 0.51 this was ~9% of the GPU
thread plus ~4% in `BbCopy::WaitAsync` [Desktop]. 0.6 already defers uploads and runs parallel
copies (`BbToggle::ParallelCopies`, `gpu/shim/bbport_copy.cpp:245`), so that 13% is stale as
written — the current number is `host_copies_wait_ns` / `host_copy_waits` in `BB_FRAME_STATS`.

**Measure before changing it.** If the waits are a few percent now, this item drops in the order. If
they are still double digits, the doc's own suggestion stands: let the four `SCHED_IDLE` draw-prep
workers drain a copy queue. One attempt to move small copies onto the idle *recording* thread made
things slightly worse (86.5 vs 87.2 FPS, 0.51) — the queue must be drained somewhere that does not
compete with the GPU thread.

**Estimated impact:** bounded by the measured wait share; 3–6% if it is still ~13% is the realistic
end of it.

**Verify.** `host copies` / blocked-share fields in `BB_FRAME_STATS`, copy counters, `frames.csv`.

## 6. Cheaper object motion vectors

**Why.** Measured at ~10% FPS on the desktop card (155 vs 141 FPS with `object_motion` off) [Desktop,
0.6 lineage]: stage B spends ~0.28 µs more per packet on bone-palette hashes, the index range cache
and motion pipeline variants. Motion vectors are required for temporal upscaling, so this is not a
quality knob to drop.

**State in 0.6 (checked 2026-10-22): the plan this item proposed is already in.** The reuse of the
previous frame's motion for an unchanged pose landed with the motion history:

- `Motion::History::Moving` (`gpu/upscale/motion_history.h`, the `GateKey`/`palette` pair) is the
  pose hash: a small-skeleton/prop draw whose bone palette did not change skips the index scan and
  `PrepareDraw` altogether (`gpu/render/rasterizer.cpp:620`), and its first changed frame stores
  positions, the next loads them;
- `Motion::IndexRangeCache` (`Revalidate = 32`) re-scans a static mesh's indices every 32nd frame at
  most, which is where an earlier ~10% of the GPU thread went.

So the 10% quoted above is what was *left* after that work: ~0.28 µs per packet across
`DrawRecord` (palette hashes, the gate and cache lookups, the motion pipeline variants). At the
~1500–1800 draws this port records in a frame that is ~0.5 ms of stage B — **≈3% of a 17 ms Deck
frame**, not 10%.

**What is genuinely left, and why it is small.** Characters (`BufferRole::Skeleton`) are deliberately
not gated (`gated = false`, `rasterizer.cpp:608`): their pose changes whenever they move, so the gate
would only pay in menus and idle frames, not in a fight. What remains is the per-draw gate lookup,
the `PrepareDraw` key, and the motion shader variants — all stage-B CPU time, which on a GPU-bound
Deck hides behind the GPU (see §3). Dropping or trimming motion vectors is not an option: temporal
upscaling needs them.

**Measure before writing code.** `BB_SECTIONS=1 BB_FRAME_STATS=1` prints `motion` µs per packet
beside `bind`/`textures`/`begin render` (`gpu/render/raster_pipe.cpp:1029`); `BB_OBJECT_MOTION=0` is
the reference side and also shows how much of the frame is motion vectors at all. Expect
single-digit on the Deck, and only in frames where stage B is the limiter.

**Verify.** `BB_DEBUG_MOTION`; the motion test targets are `EXCLUDE_FROM_ALL`, so `bash build.sh
--test` does not build them — `cmake --build out/gpu --target motion-history-test motion-shader-test`
then run the two binaries in `out/gpu`; `frames.csv` for the A/B.

## 7. Sweep the recording knobs that already exist

**What is built.** The scheduler cuts the stream into segments of ~64 KiB of recorded closures
(`BB_VK_SEGMENT_KB`), hands them to N recording threads and lets those threads submit
(`gpu/render/scheduler.cpp:96,126,146`). N is 2 from 8 hardware threads, 3 from 12, so **the Deck
already records on two threads**; `BB_ASYNC_SUBMIT=0` turns the submit side off.

**What is missing is the sweep.** No A/B of this path is recorded anywhere on Deck. The desktop
measurement behind the default (+18% with a 4c/8t `taskset` stand-in) is old. Run
`BB_VK_RECORD_THREADS=1/2/3` and `BB_VK_SEGMENT_KB=16/32/64/128` at the same spot, read the
`Recording: N submissions/frame` and CPU lines, and compare `frames.csv`. `BB_VK_RECORD_THREAD=0`
records everything on the calling thread, which is the cheap baseline.

**Expected impact:** unmeasured; what is known is that one thread records ~75% of a core at 200 FPS
[Desktop]. On Deck the GPU binds more often, so expect less than the desktop number, not more.

## 8. Scope `-fno-omit-frame-pointer` to the loader

**Why.** `gpu/CMakeLists.txt:12` sets it globally for the whole GPU library; its purpose is the
loader's thread dumps and the watchdog. On a CPU-bound workload it costs a little on every call.

**How.** Keep frame pointers in the loader/`shim` translation units, drop them for the hot renderer
and buffer paths — or make it a CMake option so both builds can be compared. **Verify:** the
loader's thread-dump tests still walk stacks; `stage_a_cpu_ms` in `frames.csv` and the GPU thread
µs/draw line in `Frame stats`.

**Estimated impact:** ~0–1%. The smallest item in the file; fold it into another change rather than
measuring it alone.

## 9. AOT shaders/pipelines, and ship the pipeline cache

**State.** The port already saves and preloads its own pipeline cache (`pipeline_store.cpp`,
"Preloaded N pipelines") and `run.sh:31` raises `MESA_SHADER_CACHE_MAX_SIZE` to 4G. `BB_ASYNC_PIPELINES`
(default on) compiles a new pipeline for a per-frame pass off the GPU thread. What does not exist is
compiling at install time and shipping the result in the package.

**Why it is worth it anyway.** This attacks hitches and first-run time, not average FPS. On a 4-core
Deck there is no headroom to absorb a compile mid-stream, so the stutter is what a player notices
most. Note that a shipped *Mesa* cache is driver-version-keyed and fragile; the port's own cache is
the portable half.

**Verify:** seconds to title, `compiles` column in `frames.csv`, the stall lines in `BB_FRAME_STATS`.

## 10. Texture collector under the Deck's budget

**State.** In this tree the collector takes its marks from the driver's current budget on integrated
GPUs (70 / 85 / 95%) and evicts by idle seconds: 20 s normally, 5 s under pressure, 1 s over the
critical mark, then by submissions (`gpu/textures/texture_cache_memory.cpp:159,192-199`). The fork's
`BB_GC_RECENT_FRAMES` / `BB_IMAGE_FREE_DELAY_FRAMES` rules are deliberately absent
([CRASH_FIXES_STATUS.md](CRASH_FIXES_STATUS.md), "Not taken"). `BB_GC_BUDGET_MB` reproduces a
Deck-sized budget on a desktop GPU.

**What to do.** Not a rewrite: sweep `BB_GC_IDLE_SECONDS` and `BB_GC_PRESSURE_IDLE_SECONDS` on Deck
and read the `texture collector: usage … starts at …, N images freed, M GPU-written kept` line in
`Frame stats` plus `gc_freed_images`/`gc_kept_images` in `frames.csv`. If area loads still show
re-upload thrash, the fork's "used in the last N submissions" rule is the known opt-in next step.

**Estimated impact:** ≈0 steady state; a few percent at most during area loads, which are also
§3/§5's territory.

## 11. Tune `-march` with `-mtune`, not only the target

**Why this is new.** `BB_DECK_MARCH` / `build_steam_deck.sh` sets both `-march` and `-mtune` to the
same value (`znver2`). The 2026-10-21 microbench says the tuned codegen is a large win on AVX2-shaped
loops (hash −61%, page walk −22%, clamp −15%) and a **20% loss on 1 KiB copies** — the `rep movsb`
choice — on Haswell. A Deck build that keeps the ISA but not the string-op tuning may be the better
package, and the reverse (`x86-64-v3` march with `znver2` tuning) is the other candidate.

**How.** Three builds, one source: `-march=znver2 -mtune=znver2` (today), `-march=znver2
-mtune=generic`, `-march=x86-64-v3 -mtune=znver2`. Screen them with the standalone CPU benchmark on
Deck (`bb-cpu-bench-*`, then `cpubench.py A.log B.log`; the tool and the comparison script are on
branch `bench/march-vs-standard`, copied into `dist/` when built), then A/B the two best as packages
with `frames.csv`.

**Estimated impact:** unknown on Zen 2; bounded above by the microbench's geomean (11% on the
Haswell run) and by the fact that the frame is mostly GPU-bound on Deck. Cheap to settle, and it
costs no rendering behaviour.

**Verify.** The microbench's `copy_1k`, `hash_16k`, `page_scan` lines first; only then a frame-time
A/B, because a microbench win that does not survive the frame is not a win.

## Experimental options for the launcher (planned, not built)

The launcher has nowhere to put a switch that trades a measurable risk for a measurable win, and
this file now has three candidates for it (plus one that cannot be a switch). When a *Mode →
Experimental* page exists, the list is:

| Option | Env | What it gives, and what it costs |
|---|---|---|
| Occlusion label ordering: skip the pass breaks nothing reads | `BB_OCCLUSION_LABEL_ORDER=2` | ~0.1 ms/frame, paid with ~5 protection traps a second in the game's own label pool (§4). Off by default, the only one measured on Deck |
| Empty-target draw skip (§1) | bit 33 today; needs an env var (say `BB_EMPTY_DRAWS=0`) before it can be a switch | **On** by default: 16–17 draws a frame, 0.6–1.2 ms/frame as measured 2026-10-16. A switch here is for an A/B against the reference, not for shipping it off |
| Buffer binding on stage A (§3) | none yet — build it behind experiment bit 34 | Would hand ~12% of stage B (0.51 desktop figure) to an idle stage A, for **p99/hitches only**: stage B is 30–66% busy on Deck. Not implemented |
| Deck code generation | `BB_DECK_MARCH` / `BB_DECK_MTUNE` at build time (`build_steam_deck.sh`) | Not a runtime switch — it needs a second binary in the package (`-march=znver2 -mtune=znver2`, the current build). CPU-side only; the 2026-10-21 microbench (§11) puts it large on AVX2-shaped loops and negative on 1 KiB copies |

Everything else that is on by default stays out of that page: the HLE image copies, two recording
threads with async submit/pipelines and the queries with predication *are* the shipped behaviour,
and a switch for them would only be a way to turn it off. The empty-target skip is the exception
listed above — also on by default, but it is the first thing to flip when the unaccounted lighting
popping on single items comes back (`docs/history/SESSION_2026-10-23.md`, "Still open"), and it is
the only way to A/B the skip without hand-editing `BB_TOGGLE_FILE`'s second number.

## Cheap A/Bs to run on the Deck before writing code

All of these are env vars in this tree; one session each, same spot, `frames.csv` on both sides.

| A/B | Env | What it prices |
|---|---|---|
| Occlusion queries | `BB_OCCLUSION_QUERIES=0` | §4: cost of queries and the culling they enable |
| Occlusion label order | `BB_OCCLUSION_LABEL_ORDER=2` | §4: priced 2026-10-23 at ≤0.1 ms/frame and off by default since; `=2` (the narrow mode) and `=0` (order nothing) only to bracket it exactly |
| HLE compute copies | `BB_HLE_OFF=1` | §2: what the image-copy detector is worth now |
| Draw pipeline | `BB_DRAW_PIPE=0` | The two-stage split itself, on Deck |
| Recording | `BB_VK_RECORD_THREADS=1..3`, `BB_VK_SEGMENT_KB=16..128` | §7 |
| Frames ahead | `BB_FRAMES_AHEAD=0/1/2` (`gpu/render/presenter.cpp:69`, default 1) | CPU/GPU overlap; `present_wait_ms` and p99 in `frames.csv` |
| Pre-upload | `BB_PREUPLOAD=1/2` with `BB_GUEST_IN_PLACE=0` (inert under the default model; 1 = rewrites only, 2 = everything) | Whether pre-upload helps the old model's VRAM copies |
| Texture GC | `BB_GC_BUDGET_MB`, `BB_GC_IDLE_SECONDS` | §10 |

## Not worth doing on the Deck

- **`BB_GUEST_VRAM` / dma-buf BAR import** — needs a discrete GPU with Resizable BAR; a unified
  memory system has no equivalent.
- **Sparse arena instead of the memory module** (`BB_PC_MODEL=0`, the old model) — the module is
  faster on AMD (4.30 vs 4.60 ms a frame) and the only path that works on NVIDIA; on a 1 GB budget
  it is the better side.
- **Live scene resolution at 720p** — `SceneTargets::EligibleScene` requires 1920×1080
  (`gpu/upscale/vk_scene_resolution.cpp:95`), so the reduced-target path cannot engage once the
  startup patch sets 720p; the patched path is far cheaper (0.51: post chain 8.4 ms live vs 1.4 ms
  patched).
- **Shipping a Mesa shader cache** — keyed by driver version; the port's own pipeline cache is the
  part that survives a driver update.

## Measure before optimising

```bash
BB_FRAME_STATS=1 BB_FRAME_LOG=/tmp/run.frames.csv BB_GPU_PROFILE=1 \
BB_GAME_DIR=/path/to/CUSA03173 bash run.sh
```

- `BB_FRAME_STATS=1` — the `Frame stats` window (GPU thread µs/draw, `stage A waits over 0.5%:`,
  `Stall:` lines, the memory/texture-collector report, the occlusion five-second line).
- `BB_FRAME_LOG` — the per-frame CSV listed in "Where the branch stands": that is what
  `tools/frames.py` reads (branch `bench/march-vs-standard`: median/p95/p99, hitch counts, and which
  columns are high on the long frames).
- `BB_GPU_PROFILE=1` — GPU ms per frame by pass label; "between submissions" is the GPU waiting for
  the CPU.
- `BB_DCB_STATS=1` (`gpu/cp/cp_diagnostics.cpp`), `BB_BUFFER_STATS=1`
  (`gpu/render/rasterizer.cpp`) — command buffers/draws per frame, buffer bindings per region.
- `BB_HLE_STATS=1`, `BB_IMAGE_COPY_LOG=1` — the HLE split and the copies recognised.

Decision rule: if GPU busy time is at or near the frame time, start at §1 (port it), then §4. If
`stage_a_cpu_ms`/`tick_wait_ms` dominate while cores idle, start at §3 and §5.

## How to land a change here

One step per change, switchable at run time, A/B across restarts, screenshots compared, counters
checked for the expected shift. In this tree the switch is either a `BbToggle` bit (a `BB_TOGGLE_FILE`
with two space-separated numbers; the second one is the experiment mask read by
`BbToggle::Experiment`) or an env var checked once at startup — both exist in the current code
(`BB_HLE_OFF`, `BB_EXP_BARRIER_ALL`). Run-to-run spread is ±1.5 FPS on the desktop card, so alternate
builds rather than trusting a single run.

Regressions to watch: the GPU-hang guard, the oversized-record-block path (`abc77f2`), and anything
that changes the shader `Info` layout or the pipeline cache key (both force a rebuild of every
pipeline).

## Which side can still move the frame rate

Checked against the code on 2026-10-22. The Deck frame is GPU-bound and the presenter keeps one
frame in flight, so stage-B CPU time below the frame time **hides behind the GPU**: shortening it
lowers p99 and hitches, not average FPS. That splits the list cleanly.

| Can raise FPS | Can only lower p99/hitches |
|---|---|
| §1 (done, ported 2026-10-22), §2, §4 | §3, §5, §6, §7, §8, §11 (CPU), §9 and §10 (hitches/memory) |

Read it off one run, not off this table: `present_wait_ms` and `BB_GPU_PROFILE`'s GPU busy time
against `frame_ms` in `frames.csv`, plus `stage B busy %` / `stage A waited %` from
`BB_FRAME_STATS`. If GPU busy is at or near the frame time, only the left column can pay.

## Suggested order (2026-10-23)

1. **The cheap A/Bs table** — one Deck session, and it decides between §1/§2/§4 (GPU) and
   §3/§5/§6 (CPU) on measurement rather than on this file's assumptions. The 2026-10-23 run already
   leans on it: stage A is 7.8 ms of an 18.4 ms frame with the GPU thread ~10% idle in the heavy
   windows, so the frame is GPU-bound and only the left column of the table below can pay.
2. **§4 is settled to the extent this pass can settle it.** The label half is fixed (0 guest faults
   over 8.7 min, 13 retries), worth **≤0.1 ms/frame** and off by default since 2026-10-23 (`=2`), so
   it needs no more code. The one thing left is the queries' own price: `BB_OCCLUSION_QUERIES=0` on
   Deck, same spot, `frames.csv` p50/p99 and `BB_GPU_PROFILE` GPU ms. 80% of counter reads come back
   empty, so there is real culling behind them — the A/B says whether it costs more than it saves.
3. **§2** — 1–2% of GPU time and a small detector extension, once a Deck trace names what declines
   the `704x1x1` class.
4. **§11** — a build-flag sweep screened by the CPU benchmark; no rendering change, and the Haswell
   run already suggests the current `-mtune` is not free.
5. **§6** only if the measurement puts stage B above the frame time — and then the gate and index
   cache are already in, so expect ~3%, not the 10% the desktop number suggests.
6. **§3 and §5** — the same rule: CPU time below the frame time is a p99 item, not an FPS item. §3
   additionally needs stage A to own the buffer cache.
7. **§7 and §8** — sweeps and a flag scope, not backlog.
8. Everything else in the table order.
