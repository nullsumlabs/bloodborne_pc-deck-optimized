# Native Vulkan translation vs emulated shadPS4 infrastructure

Static analysis of this repository, 2026-10-15. Scope: how much of the code that carries the
game's rendering path is **native Vulkan translation** (PS4 GNM/PM4 and GCN shaders turned into
Vulkan 1.3 pipelines and SPIR-V) and how much is still **emulated shadPS4/console
infrastructure** (HLE wrappers, the console memory model, CPU-side command-processor work).

## One correction to the usual framing

In this repository, shadPS4's `video_core` and `shader_recompiler` are **not** the emulated side —
they *are* the native Vulkan translator. shadPS4's renderer parses PS4/GNM PM4 packets, builds
`VkPipeline`/`VkCommandBuffer`, and emits SPIR-V from GCN shaders. What is genuinely "emulated
shadPS4 architecture" is the other half:

- the HLE system libraries (`gpu/shadps4/core/libraries`),
- the CPU-side console memory model (VRAM mirrors, page protection, userfaultfd, readback),
- constant-engine dumps and the SRT walker (descriptors read on the CPU during parsing),
- the synthetic vblank,
- the hand-written PS4 OS runtime (`src/runtime_*.c`).

The port's job — see [EMULATION_REMOVAL_PLAN.ru.md](EMULATION_REMOVAL_PLAN.ru.md) and
[TRANSLATOR_ARCHITECTURE.ru.md](TRANSLATOR_ARCHITECTURE.ru.md) — is progressively deleting that
emulation, which is why the ratio keeps moving toward the native side.

## Method

Lines of C/C++ plus host shader sources (`.c .cc .cpp .h .hpp .inc .frag .comp .glsl`),
excluding `gpu/third_party` (71.5k lines), CMake files, docs, scripts and JSON. Counts were then
cross-checked against the project's own design documents and against functional markers
(`vk::` usage, Vulkan extensions, `bbport:` change markers, `BB_*` switches). `rg` is unusable on
the machine this was measured on (missing `libpcre2`), so all counting used `grep`/`find`.

## Ratio by volume of code

~187.8k lines in the repository:

| Bucket | Lines | Share |
|---|---|---|
| **Native Vulkan / pipeline translation** | **98,612** | **52.5%** |
| **Emulated console infrastructure** | **25,761** | **13.7%** |
| Shared support (logging, clocks, tools, tests, packaging) | 15,592 | 8.3% |
| Network / virtual services (`gpu/bbnet`, shadPS4 NP + network HLE) | 48,150 | 25.6% |

**Rendering path only** — `libbbgpu` = 127,074 lines (`gpu/shadps4`, `shim`, `layer`, `cp`,
`games`, `dlss_bridge`):

> **≈ 79% native Vulkan translation vs ≈ 21% emulated-console residue** (98,612 vs 25,761 lines).

Whole repository ("everything the game runs through"): **≈ 74% translation vs ≈ 26% emulation**,
and **≈ 50/50** if the 48k-line `bbnet` online module is counted — it is virtual-console
infrastructure by nature, but has nothing to do with rendering.

An estimate of 90/10 is too optimistic; 50/50 is only true with the network HLE included. **The
defensible figure for the renderer is ~80% native / ~20% emulated**, and the trend is toward the
native side: each ported fix moves a piece of behaviour from CPU emulation to a real GPU
operation.

## 1. Native Vulkan pipeline translation — where it lives

| Path | Lines | Why it is native |
|---|---|---|
| `gpu/shadps4/shader_recompiler/` | 38,119 | GCN ISA → IR → SPIR-V. `frontend/decode.cpp`, `opcodes.h`, `format.cpp`, `translate/*` (vector/scalar ALU, memory, data share), 24 IR passes, `backend/spirv/emit_spirv*.cpp` emitting real SPIR-V through the vendored `sirit` (16.5k) |
| `gpu/shadps4/video_core/renderer_vulkan/` | 29,368 | `vk_pipeline_cache.cpp` (1,247) with `BB_ASYNC_PIPELINES` worker-thread compilation, `vk_pipeline_serialization.cpp` (578, disk cache), `vk_graphics_pipeline.cpp`, `vk_instance.cpp` (1,030), `vk_rasterizer.cpp` (4,738), `vk_scheduler.*` (2,407), `vk_presenter`, `fsr411/`, `host_passes/` |
| `gpu/shadps4/video_core/amdgpu/` | 8,708 | PM4 decode and register state → Vulkan state (`liverpool.cpp`, `pm4_cmds.h`, `tiling.cpp`, `resource.h`); `pm4_selftest.cpp` is a port addition |
| `gpu/shadps4/video_core/buffer_cache/`, `texture_cache/` | 12,529 | guest memory → `vk::Buffer`/`vk::Image`, detiling shaders, descriptors |
| `gpu/shadps4/video_core/host_shaders/` | 5,328 | GLSL → SPIR-V at build with `glslangValidator --target-env vulkan1.3`: 25 explicit `add_host_shader()` call sites plus a `foreach(BPP …)` loop (6 bit depths × macro/micro tiled) = **37 SPIR-V host modules** (motion vectors, TAA, FSR, occlusion resolve, tiling) |
| `gpu/cp/` | 617 | the port's **own PM4 decoder** replacing `liverpool.cpp` (`cp_decoder`, `cp_pm4.h`), shadow-verified in-game (`BB_CP_SHADOW=1`: ~800k buffers, ~200M packets, 0 mismatches) |
| `gpu/shim/bbgpu.cpp`, `bbport_gnm_hooks.cpp`, `bbport_gnm_table.inc`, `bbport_copy.cpp`, `bbport_guest_hooks.cpp` | 2,298 | the GNM entry-point table and the translation entry points |
| `gpu/games/`, `gpu/dlss_bridge/` | 495 | per-game profile and the DLSS/NVIDIA path |

Functional evidence:

- **4,246 `vk::` uses across 93 files** (vulkan-hpp), Vulkan 1.3 headers + VMA.
- **31 distinct vendor extensions** in use (6 AMD, 16 EXT, 7 KHR, 1 NV, 1 VALVE), including
  `VK_EXT_primitive_topology_list_restart`, `VK_NV_framebuffer_mixed_samples`,
  `VK_AMD_buffer_marker`, `VK_AMD_gcn_shader`, `VK_EXT_extended_dynamic_state3`,
  `VK_EXT_external_memory_host`, `VK_KHR_cooperative_matrix`, `VK_EXT_shader_float8`,
  `VK_VALVE_shader_mixed_float_dot_product`.
- **No x86 interpreter or recompiler anywhere**: `grep -i 'interpreter\|recompiler' src` returns
  nothing. The game's x86-64 code runs natively; "recompilation" exists only for the GPU shader
  ISA, which is the native translation path.

## 2. Emulated shadPS4 / console infrastructure

| Path | Lines | What it emulates |
|---|---|---|
| `gpu/shadps4/core/libraries/` | 11,759 | HLE of PS4 services: `gnmdriver.cpp` (3,477), `videoout/driver.cpp` (956, synthetic vblank), `kernel/equeue.cpp` (713), `avplayer` (3,013), `system` (1,099) |
| `gpu/shim/` (non-graphics part) | 6,186 | `bbport_free_check.cpp` (1,090), `bbport_overlay.cpp` (925), `bbport_guest_memory.cpp` (738, guest memory paging over dma-buf/host import), `bbport_game_menu.cpp`, `heap_sites.cpp`, plus the `gpu/shim/core/*` stubs of shadPS4 core (`emulator_settings.h`, `memory.h`, `signals.h`, `address_space.h`) |
| `src/runtime_*.c`, `probe.c` | 7,021 | hand-written HLE of the PS4 OS: memory, threads, sync, files, audio (ATRAC9), pad, saves, AppContent |
| `gpu/layer/` | 452 | the memory module (`BbLayer::GpuMemory`) and the shared write-trap reasons |
| `gpu/dlss_bridge/` | 343 | Wine helper / Vulkan memory bridge |
| `gpu/bbnet/` | 48,150 | the whole NP/network virtual console (auth, trophies, matching, signaling, HTTP/SSL) |

Inside the native tree, the port's own edits are visible as **407 `bbport:` markers across 375
files** and **267 distinct `BB_*` switches** across the tree — a good proxy for how much
behaviour is still toggle-reachable legacy rather than compiled in.

## 3. Remaining bottleneck files (heavy emulation / wrappers)

In rough order of cost, per the port's own plan:

1. **`gpu/shadps4/video_core/buffer_cache/buffer_cache_layer.cpp` (1,590), `buffer_cache.{cpp,h}`
   (7,630 module)** — VRAM mirrors of guest pages. The model exists because the translator reads
   T#/V#/S# descriptors on the CPU during parsing, which DXVK and a PC driver never do.
2. **`video_core/page_manager.cpp` (776), `page_manager.h` (74), `fault_manager.cpp` (185)** —
   page-protection write tracking, userfaultfd, readback, preload, early labels. Memory model 0.3:
   **still the default**; removal is planned after NVIDIA validation. Alongside
   `multi_level_page_table.h`.
3. **`video_core/amdgpu/liverpool.cpp` (2,905, 34 `bbport:` markers)** — the CPU-side command
   processor: constant-engine dumps (~14k `DumpConstRam`/s measured), the SRT walker,
   `WaitRegMem`, `WRITE_DATA`/DMA ordering, `MEM_SEMAPHORE` at parse time
   (`BB_MEM_SEMAPHORE_ORDER`), occlusion handled by the CPU. Partially replaced by `gpu/cp/` plus
   `BB_CP_DECODER`; guarded by `BB_PM4_SELFTEST`.
4. **`renderer_vulkan/vk_rasterizer.cpp` (4,738), `vk_draw_pipe.h`, `vk_draw_prep.cpp`** — the
   two-stage A/B draw path lives here, and so do the CE/SRT and descriptor-walk emulation paths.
   The docs put GPU-write accounting (`PendingWriteOverlaps`, `IsRegionGpuModified`) at **~15% of
   GpuComm**.
5. **`texture_cache/` (4,899)** — textures identified **by address and hash** rather than as
   objects; intercepting `Gnm::Texture::init*` and the TPF loader is still a roadmap item. The
   address/hash model is called out as emulation in the docs, together with the unstable
   `BB_OCCLUSION_QUERIES` path.
6. **`core/libraries/videoout/driver.cpp` (956), `video_out.cpp` (540)** — synthetic vblank and
   flip timing.
7. **`core/libraries/gnmdriver/gnmdriver.cpp` (3,477)** — the GNM API HLE layer. Bloodborne's
   libGnm is statically linked, so this is console-call emulation the translator does not need.
8. **`gpu/shim/bbport_guest_memory.cpp` (738), `gpu/layer/bblayer_gpu_memory.*` (413)** — VM
   paging and dma-buf chunk management: the "one memory" model only the PS4 had.
9. **`src/runtime_*.c` (5,987)** — accurate but still hand-written console-OS emulation;
   `runtime_memory.c` (1,046) is where page traps and the guest address space live.
10. **`gpu/bbnet/` (48,150)** — by far the largest non-rendering block: a full virtual-console
    online stack.
11. **`gpu/shim/core/emulator_settings.h`, `memory.h`, `signals.h`, `address_space.h`,
    `bbport_toggles.h`** — thin shadPS4-core scaffolding the vendored tree still
    compiles against. The intended direction is to replace these with the narrow module
    interfaces of [TRANSLATOR_ARCHITECTURE.ru.md](TRANSLATOR_ARCHITECTURE.ru.md): memory, cp,
    state, resources, render, shaders, platform, games.

## Caveats

- Lines of code are a **proxy for structure, not for runtime cost**. The project's own
  measurements are the better guide to where emulation actually costs: ~15% of GpuComm in GPU-write
  accounting, ~14k `DumpConstRam`/s (~4–5 MB/s) of constant-engine dumps, ~3.3 µs per draw in
  stage B at 99% occupancy on 4 cores.
- `import_names.inc` (750 lines) sits in `src/` as generated guest-import data, not code; counting
  it moves the emulation bucket by ~0.4%.
- `gpu/shim` was measured over all files while `src` was measured over C/C++ only; the difference
  is a few hundred lines of vendored Tracy/ImGui stubs. Every bucket is affected by well under 1%.
- `gpu/third_party` (71.5k lines: sirit, GCN headers, FSR-Vulkan, Dear ImGui, fonts, half,
  httplib) is excluded from every figure above. Conceptually `sirit` (16.5k) belongs to the native
  side — it is the SPIR-V builder the recompiler emits through.
- Nothing under `gpu/` compiles on the machine this was measured on; the analysis is a static read
  of the tree plus its design documents, not a build or a profiling run.
