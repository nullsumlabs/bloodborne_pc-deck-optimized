# What still depends on shadPS4

Inventory of every part of the project that relies on shadPS4-derived emulation — vendored code that
is compiled, the emulator's configuration surface the renderer still calls through, and the places
outside `gpu/` that have to match shadPS4 conventions. Companion to
[VULKAN_SHADERS.md](VULKAN_SHADERS.md), which measures the native-vs-emulated split by volume of
code; this file names the individual dependencies.

Vendoring is declared in [../gpu/VENDOR.txt](../gpu/VENDOR.txt): shadPS4 commit
`b4e7ae776071ceb1dd35004577a8719481e1d258`, directories `video_core`, `shader_recompiler`,
`common`, `core/libraries/{gnmdriver,videoout}`, the headers of `core/libraries/kernel`,
`core/libraries/videodec/video_utils`, `core/libraries/avplayer` (without the handle streamer),
plus the externals `gcn`, `sirit`, `half`. Local changes are marked `bbport:` (407 markers in 375
files).

## 1. HLE code compiled into libbbgpu

From the `add_library(bbgpu ...)` source list in `gpu/CMakeLists.txt`:

| Compiled source | Lines | What it emulates |
|---|---|---|
| `core/libraries/gnmdriver/gnmdriver.cpp` | 3,477 | GNM graphics-API entry points (the HLE side of the 99 `sce::Gnm` methods) |
| `core/libraries/videoout/driver.cpp` | 956 | the console's display model: synthetic vblank, flip-request queue, event delivery |
| `core/libraries/videoout/video_out.cpp` | 540 | VideoOut buffers, format conversion for the flip |
| `core/libraries/kernel/equeue.cpp` | 713 | PS4 kernel event queues — a kernel **object** emulation, not a rendering one |
| `core/libraries/avplayer/*.cpp` (6 files, globbed) | 2,131 | the movie/AVPlayer library HLE, ffmpeg-backed (3,013 lines with its headers) |
| `core/libraries/videodec/video_utils.cpp` | 49 | video-decode helpers for the above |
| `common/*.cpp` (12 files: `decoder`, `error`, `io_file`, `native_clock`, `number_utils`, `path_util`, `rdtsc`, `spin_lock`, `recursive_lock`, `string_util`, `thread`, `signal_context`) | 1,496 | shadPS4's runtime support layer: the PS4-tick clock, host signal capture, spin/recursive locks, RDTSC helpers |

`common/elf_info.h` (header) is also in use: the renderer and the online module read console ELF
metadata (title id, version) through `Common::ElfInfo::Instance()`. Its fields are private to
shadPS4's `Core::Emulator`, and **the port fills them at startup** —
`gpu/shim/bbgpu.cpp:80` (`ElfInfo's fields are private to Core::Emulator; the port fills them
here`), `BbGpu::FillElfInfo`, called from `Init` at line 274.

## 2. The emulator's configuration and platform surface

`gpu/shim/{core,common,imgui,tracy}` holds 17 stub headers that stand in for shadPS4's core
(`core/emulator_settings.h`, `core/memory.h`, `core/signals.h`, `core/address_space.h`,
`core/debug_state.h`, `core/libraries/libs.h`, `core/libraries/kernel/{kernel,threads}.h`,
`core/devtools/widget/gpu_memory.h`, `imgui/renderer/*`, `tracy/*`, `common/assert.cpp`,
`common/logging/*`). Three of these carry real behaviour:

**`core/emulator_settings.h`** replaces shadPS4's `EmulatorSettings` singleton with a class that
reads `BB_*` environment variables. The vendored tree reads **37 distinct members** through it:

- **24 from the renderer** (`video_core`, `shader_recompiler`), in three groups:
  - 9 diagnostics/validation: the seven `IsVk*` flags (`IsVkValidationEnabled`,
    `IsVkValidationCoreEnabled`, `IsVkValidationGpuEnabled`, `IsVkValidationSyncEnabled`,
    `IsVkHostMarkersEnabled`, `IsVkGuestMarkersEnabled`, `IsVkCrashDiagnosticEnabled`) plus
    `IsRenderdocEnabled` and `IsShaderCollect`
  - 6 feature flags: `IsPipelineCacheEnabled`, `IsPipelineCacheArchived`, `GetReadbacksMode`,
    `IsReadbackLinearImagesEnabled`, `IsRcasEnabled`, `GetRcasAttenuation`
  - 9 environment plumbing: `GetGpuId`, `GetPresentMode`, `IsDumpShaders`, `IsNullGPU`,
    `IsHdrAllowed`, `IsFsrEnabled`, `IsCopyGpuBuffers`, `IsDirectMemoryAccessEnabled`,
    `IsPatchShaders`
- **5 from VideoOut** (`driver.cpp`: `GetVblankFrequency`, `IsUncappedVblank`, `GetFrameLimit`;
  `video_out.cpp`: `GetInternalScreenWidth/Height`)
- **8 from `common/logging/log.cpp`** (`IsLogEnable`, `IsLogSync`, `IsLogSkipDuplicate`,
  `GetLogType`, `GetLogFilter`, `GetLogSizeLimit`, `GetLogFlushLevel`, `GetLogMaxSkipDuration`) —
  this file is **not compiled**: the build uses the port's `shim/common/logging/log.cpp` instead,
  which reads `BB_GPU_LOG`. Those 8 reads are dead in the current build.

So 29 of the 37 are live. The shim also still declares switches that only make sense in an emulator
with an ELF loader: `IsUserfaultfdTracking()` (hard `false`) and `IsPatchShaders()` (`false`).

**`core/libraries/kernel/kernel.h` + `threads.h`** — the console kernel surface:
`Libraries::Kernel::sceKernelIsNeoMode()` is answered `return 0` (base PS4) in
`gpu/shim/bbgpu.cpp:226` and consulted by `video_core/amdgpu/liverpool.cpp:93` (counter-pair
count), `texture_cache/image_info.cpp` (Neo-mode alt tiling, lines 82 and 143) and
`vk_presenter.cpp`. `Libraries::Kernel::Thread` is a `std::jthread` wrapper that calls
`runtime_thread_attach_host("bb:hle")` so HLE threads can call guest code (AvPlayer allocator
callbacks); `kernel.h` declares `Libraries::Kernel::__Error()`.

**`common/assert.cpp`, `common/logging/log.cpp`** — the port's own implementations of shadPS4's
assert/logging interfaces, so the vendored `ASSERT`/`LOG_*` macros keep working.

## 3. The console memory model (inside the vendored tree)

Detailed in [VULKAN_SHADERS.md](VULKAN_SHADERS.md), section 3: `video_core/page_manager.*` (850
lines), `video_core/buffer_cache/fault_manager.*` (227), `multi_level_page_table.h`,
`buffer_cache/buffer_cache_layer.cpp` (1,590), `cache_storage.*`. This is the emulation the
translator's later modules are meant to delete; the shared page-trap bookkeeping on the port's side
is `gpu/layer/bblayer_write_traps.h` + `runtime_memory_trap` in `src/runtime_memory.c`.

## 4. What is *not* here

`gpu/shadps4/core/` contains only `libraries/` and `platform.h`. There is **no ELF loader, no
linker, no kernel/process emulation, no `emulator.cpp`, no `memory.cpp`** — shadPS4's emulator core
was never vendored. The guest image is mapped and entered by the port's own `src/probe.c`, and PS4
system calls are served by `src/runtime_*.c`. Nothing in the project runs the game through
shadPS4's emulation loop.

## 5. `gpu/bbnet` — a second, independent copy of shadPS4's console HLE

48,150 lines: shadPS4's `core/libraries/{network,np}` plus the `shadnet` client (a fork of
shadp2p), built as `libbbnet.so` and loaded by `src/runtime_net.c` only for online play
(`BB_NET=OFF` or missing protobuf/OpenSSL/zlib/miniupnpc leaves it out and the port plays offline).

- Its own parallel shims: `gpu/bbnet/include/core/*` — `emulator_settings.h`, `user_manager.h`,
  `user_settings.h`, `file_sys/fs.h`, `libraries/libs.h`,
  `libraries/{rtc,invitation_dialog}/*`, `libraries/kernel/{kernel,file_system}.h`,
  `libraries/kernel/threads/pthread.h`, `imgui/{invitation_prompt_layer,shadnet_notifications_layer}.h`.
- Built `-fvisibility=hidden -Wl,-Bsymbolic -Wl,--exclude-libs,ALL`, so its copies of shadPS4's
  kernel shims and symbol table never meet libbbgpu's; it links `libbbgpu` only for shadPS4's
  `common/` code (logging, thread names).
- Excluded by CMake filter: `np_trophy`, `trophy_ui`, `np_sns_facebook_dialog`, `np_profile_dialog/`,
  `np_commerce/` — "their shadPS4 versions need the emulator's UI layers"; trophies and system
  dialogs stay in the C runtime.

## 6. Behaviour outside `gpu/` that must match shadPS4

No code is reused here, but the port's own code reproduces shadPS4's conventions:

| Where | What it matches |
|---|---|
| `scripts/patches.py` (lines 1, 259, 318) | compiles **shadPS4/GoldHEN XML** patch files into `out/patches.bin` for the loader; also accepts third-party patch XML in shadPS4 format |
| `scripts/mods.py:179` | "shadPS4's loose overlay convention" for mods; pinned by `tests/test_mods.py:89` (`test_shadps4_sibling_folder_and_global_disable`) |
| `src/runtime_content.c:84` | add-on layout `<user>/addcont/<title id>/<entitlement label>` — "shadPS4's layout" |
| `src/runtime_savedata.c:72` | the Bloodborne "sound hack" ported from the Diegolix29 shadPS4 fork |
| `src/runtime_pad.c:25,106,184` | analog stick conversion and dead-zone mapping "as shadPS4 does" |
| `src/runtime_memory.c:457` | "as shadPS4's early GPU unmap" |
| `run.sh:217-229`, `scripts/online.py`, `launcher/bbport_launcher.py:149-153,347-350` | default online server is shadPS4's public shadNet (`srv.shadps4.net:31313`); exports `SHADPS4_HTTP_HOST_OVERRIDES_JSON`, a shadPS4-named variable consumed by the vendored network HLE |
| `gpu/dlss_bridge/*` | DLSS bridge adapted from `IFreemz/shadPS4-Bloodborne-DLSS-FSR`; copyright retained |
| `shell.nix:24`, `build.sh`, `gpu/CMakeLists.txt` | build wiring described as "shadPS4 video core dependencies" |

## 7. Tests

`tests/test_gpu_runtime_stubs.cpp` exists because the renderer links against the port's runtime: it
substitutes `runtime_memory_*`, `runtime_thread_attach_host`, `runtime_file_translate`,
`runtime_fault_recover` and friends, aborting on guest access, so renderer tests run without a
guest process. `tests/test_motion_history.cpp`, `tests/test_ui_composition.cpp` and
`tests/test_mods.py` also reference shadPS4 semantics.

## 8. Vendored files present but not compiled

Replaced by the port or irrelevant to this build (verified against the explicit source list in
`gpu/CMakeLists.txt`; the only globs are `video_core/*.cpp`, `shader_recompiler/*.cpp`,
`shim/*.cpp`, `layer/*.cpp`, `games/*.cpp`, `cp/*.cpp`, `avplayer/*.cpp`):

- `common/logging/log.cpp`, `common/assert.cpp`, `common/memory_patcher.cpp`, `common/key_manager.cpp`,
  `common/ntapi.cpp`, `common/discord_rpc_handler.cpp`, `common/stb.cpp`
- `video_core/renderdoc.cpp` (excluded explicitly: `list(FILTER VIDEO_CORE EXCLUDE REGEX "renderdoc\\.cpp$")`)

## 9. What each dependency waits on

From the module plan in [TRANSLATOR_ARCHITECTURE.ru.md](TRANSLATOR_ARCHITECTURE.ru.md):

| Dependency | Retired by |
|---|---|
| `EmulatorSettings` + kernel-HLE surface (§2) | the `platform` module, beside the runtime |
| page manager / fault manager / VRAM mirrors (§3) | memory module step 1 (`BB_LAYER_MEMORY`, `runtime_memory_trap`) |
| constant-engine dumps, SRT walker, `MEM_SEMAPHORE` on the CPU (§1 `liverpool.cpp` path) | `cp` step 2 (`gpu/cp/`, `BB_CP_DECODER`, `BB_MEM_SEMAPHORE_ORDER`) |
| `gnmdriver` / `videoout` / `avplayer` HLE (§1) | `platform` module; the GNM entry points already have the port's own hooks |
| memory model in `buffer_cache` / `texture_cache` (§3) | `state` + `resources` steps 3 |
| `bbnet` (§5) | deliberately kept: it is the online module, offline-optional |

## Caveats

- Counts are static: lines and call sites as written, verified with `grep`/`find` (the `rg` binary
  on the measuring machine is broken — missing `libpcre2`). Nothing was built or run; none of the
  `gpu/` tree compiles on that machine.
- "Not compiled" in section 8 was determined from the explicit source list and the globs in
  `gpu/CMakeLists.txt`; a future glob could change it.
- The `EmulatorSettings` split counts the members the vendored tree *reads*; the shim itself
  declares more (the full list is in `gpu/shim/core/emulator_settings.h`).
- `gpu/third_party` (sirit, GCN headers, half, FSR-Vulkan, Dear ImGui) is also vendored from
  shadPS4's externals where noted in `gpu/VENDOR.txt`, but it is translation infrastructure, not
  emulation, and is excluded from this inventory.
