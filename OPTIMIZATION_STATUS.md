# Optimisation status — where this branch stands (2026-10-17)

Branch `nocrash-optimizations`. The two changes below are committed (2026-10-17); goal 1's copy
destination has still not run on hardware and stays behind its profile flag and experiment bit 32.
Nothing has been pushed. Read this file before touching anything, and re-read
`OPTIMIZATION_GOALS.md` §1 and §11 for the measurements behind the two changes.

## The two changes in the tree

| # | What | State | Where |
|---|---|---|---|
| 11 | Drop draws with nothing to render into | in, **on by default in shipped builds** | `vk_rasterizer.cpp:1568` (`SkipNoTargetDraw = 1ull << 33`), `:1592` (`DrawUnobservable`) |
| 1 | The guest memcpy `3d5ebf4e` as an image copy | in, **behind profile + experiment bit 32** | `vk_rasterizer.cpp:2921` (`CopyDestinationImage = 1ull << 32`), `:2924` (`IsComputeImageCopy`), `:2968` |
| 1 | The destination image a copy has no binding for yet | new, same gate | `texture_cache.cpp:797` / `texture_cache.h:137` (`FindOrCreateCopyDestination`) |

A third, smaller thing rides along: `driver.cpp:598` adds `no-target draws skipped N/frame` to the
`Frame stats` line, and `bbport_toggles.h:178` adds the two counters behind it.

**Two different bit masks, do not confuse them** (this has bitten once already):

- `runtime_disabled_optimizations` — the *first* number in `BB_TOGGLE_FILE`, `BbToggle::Disabled`,
  the enum in `gpu/shim/bbport_toggles.h`. Bit 32 = `RecordPrefetch`, bit 33 = `TextureViewMemo`.
- `runtime_experiment_bits` — the *second* number, `BbToggle::Experiment`. The two new features use
  raw bits here: **32** = copy destination image, **33** = skip no-target draws.

## Goal 11 — the no-target skip

What it does: a draw whose pass has no colour attachment and no depth/stencil target, and no stage
that stores to memory (buffer store, image store, atomic; the port's own page-table/fault buffers do
not count), is counted and dropped. The old host skipped exactly these shapes; this one recorded
them as `pass 16384x16384 [] depth -` draws at 0.2–0.6 ms each, ~10 a frame.

Evidence so far (all from logs, none from hardware yet):

- `BB_GPU_PROFILE` on the Deck (2026-10-16 `deck-copytrace`) shows the two labels costing
  0.6–0.8 ms/frame together, up to 1.2 ms in the heaviest window — 3–6 % of a 17–23 ms frame.
- The library in `dist/deck-notarget/` (bit off unless you set it) was built and handed over; the
  **A/B run has not happened yet — the logs are the next thing to arrive.**

The production package `dist/Bloodborne_PC-nocrash-bbport-steamdeck-online.AppImage` now runs with
the bit **on** by default (`src/runtime_memory.c:702`, `EXPERIMENTS_ON_DEFAULT = 1ull<<33`,
`:704` initialises the mask with it; `:711-715` — a `BB_TOGGLE_FILE` with one number keeps the
default, with two numbers the file wins both ways). Verified in the packaged `bb-probe`: the symbol
holds `0x200000000`.

### What to ask of the logs

1. `Frame stats` in the heavy area: `no-target draws skipped` ≈10/frame with the file absent or
   `0 8589934592`, 0 with `0 0`. If it stays 0 where the `16384x16384` labels appear, the log names
   the reason: `No-target draw kept: vs <hash> ps <hash> (a stage writes memory)`.
2. `BB_GPU_PROFILE`: both `pass 16384x16384 [] …` labels gone.
3. `logs/<stamp>.frames.csv`: frame time with the bit on vs off, same spot and direction.
4. Anything that breaks with it on and not with it off — a fence the game waits for, a pass the
   following draws expected to be open. Clear the bit (`echo 0 0 > toggles.txt`) and keep the log.

Quiet A/B (no files written): launcher, log switch **off**, `BB_TOGGLE_FILE=/home/deck/toggles.txt`
in "Доп. переменные"; `echo 0 0` / `echo 0 8589934592` flips it live (re-read every 250 ms). Do not
use `run-notarget.sh` for it — it forces `BB_SAVE_LOG=1 BB_HLE_STATS=1`.

## Goal 1 — the copy shader as an image copy (the "GPU writes" testing)

What is in the tree: `IsComputeImageCopy` already turned the copy shader's dispatches into image
copies where **both** ranges are registered images. The remaining dispatches were declined with
"destination range is not a registered image" and fell back to the emulated memcpy — a download of
the source and an upload of the destination, per dispatch, per frame.

The 2026-10-16 Deck run with `BB_COPY_IMAGE_TRACE=3d5ebf4e` showed those declined copies have a
destination that *is* an image in the frames that match: the game simply binds it later in the same
frame. So the change registers one for the copy:

```cpp
image1_id = texture_cache.FindOrCreateCopyDestination(buf1.base_address, buf1.GetSize(),
                                                     texture_cache.GetImage(image0_id));
```

`FindOrCreateCopyDestination` (`texture_cache.cpp:797`) builds the destination's `ImageInfo` from
the **source's** layout (same size, pitch and bpp by construction — the detector compared them),
clears metadata and stencil, registers it through the normal `FindImage` path, refuses it when the
range resolves into another image, and then marks it

```cpp
image.flags = (image.flags & ImageFlagBits::Registered) | ImageFlagBits::GpuModified;
image.gpu_write_tick = scheduler.CurrentTick();
```

`clean` because the copy is about to fill it (a `RefreshImage` must not upload the game's stale
bytes over the copy), `GpuModified` + a fresh tick because the bytes are the GPU's from then on: a
guest or buffer reader has to download them (`SafeToDownload`), and the collector treats it like a
render target.

Gates: `Profile::copy_destination_image` (set for Bloodborne, `gpu/games/bloodborne.cpp:48`) **and**
experiment bit 32 (`BbToggle::Experiment`) — so the Deck can A/B it live, and `dry_run` in the trace
path creates nothing.

### The open questions — this is the part that must not get lost

1. **Is `GpuModified` + `clean` the right accounting for a range the guest also writes?** The
   destination of these copies is a 1080p-ish render-target-sized range; if the guest or a buffer
   consumer reads it back before the next copy, the download must return the copied bytes and not
   the previous frame's. Watch `BB_HLE_REPORT`/`Write notices`, the `guest copies …` and
   `layer memory ms` lines in `Frame stats`, and any tearing of the affected surfaces.
2. **Does the collector's eviction (`TextureCollector`, bit 31 of the *disabled* mask) free a
   destination image it should keep?** It is not `Registered`-only any more; if a freed destination
   comes back as a fresh image, the next copy re-registers it — cheap, but a cycle. Look for the
   image count (`images registered`) growing per frame.
3. **Does registering a destination the game binds later fight with the game's own binding?** The
   game binds the same range as an image in other frames; the registry may resolve the two into one
   image with a different size (the guard above declines that copy). Watch for
   `destination range is not a registered image` still appearing with the bit on.
4. **Cost vs the win**: three dispatches a frame in the plaza scene, each currently a download +
   upload. Measure before believing: `BB_GPU_PROFILE` label of the copy shader, the `guest copies`
   and `buffer uploads` lines, and `frames.csv`.

### How to read the next run

```
BB_COPY_IMAGE_TRACE=3d5ebf4e BB_GPU_PROFILE=1 BB_SAVE_LOG=1 BB_HLE_STATS=1 BB_TOGGLE_FILE=… \
BB_GAME_DIR=/path/to/CUSA03173 ./…prefix…-online.AppImage --play
```

- with the bit clear (`0 0`): the declined copies and their verdict in the trace.
- with bit 32 set (`0 4294967296`): the same copies should now report
  `match (a destination image is registered for it)`, and the emulated-memcpy label and the
  download/upload counters should drop.
- bit 33 set as well (`0 4294967296 8589934592`? no — the file is two numbers: use
  `0 4294967296` for the copy test alone, `0 8589934592` for the skip alone, `0 12884901888` for
  both, `1ull<<32 | 1ull<<33`).

## Environment notes (this host, so a rebuild does not need rediscovering)

- `../deps/prefix` is the dependency prefix (`../deps/env.sh`); `build.sh` / `build_steam_deck.sh`
  now source it themselves. It carries SDL3, miniz, protobuf 3.21.12 (**dev + `libprotoc.so.32` +
  `libprotobuf.so.32.0.12`**, extracted from debs in `/tmp/debs`), nlohmann_json, Zycore/Zydis,
  Xbyak, glslang and the ffmpeg libs this Ubuntu install lacks. No sudo was needed for any of it.
- `out/steamdeck/cxx` and `cc` are wrappers (`g++-14 -march=znver2 -mtune=znver2 -O2 "$@"`) and the
  `out/gpu` CMake cache uses them — every build in this tree is a Deck build whatever script runs.
- Packaging on this host: `BB_HOST_USR=/home/dncomp/bbwork/bbhost/usr` (a symlink farm plus the
  missing libraries) and `BB_APPIMAGETOOL=/home/dncomp/bbwork/tools/appimagetool/usr/bin/appimagetool`
  (extracted; the recipe now adds its directory to `PATH` for `desktop-file-validate`).
- ninja links need `LIBRARY_PATH=/home/dncomp/bbwork/deps/prefix/usr/lib/x86_64-linux-gnu` for the
  pre-existing `-lZycore` gap when run by hand.

## Do not forget

- `gpu/third_party/fsr-vulkan` is dirty: `src/ffx_vk_fsr4_v07.c` carries the per-pass timestamp
  profiler (`BB_FSR4_PROFILE=1`, 300-frame report) and pipeline statistics (`BB_FSR4_STATS=1`).
  Both are `getenv`-gated and create nothing without the variables, so a "production" build is fine
  as it stands — but the submodule content is not committed anywhere.
- The two toggles that shipped on: bit 33 (skip) is **default on** now; bit 32 (copy destination) is
  off until the profile sets it *and* the bit is set.
- `OPTIMIZATION_GOALS.md` §11 and `dist/deck-notarget/README.md` hold the full argument for the skip;
  `dist/Bloodborne_PC-nocrash-bbport-steamdeck-online.README.md` holds what was verified about the
  package that was just built.

## Next steps, in order

1. Read the Deck logs for the skip (goal 11) — the A/B and the frame times decide whether it stays
   default-on, and whether anything breaks.
2. Same run with bit 32 for the copy change (goal 1) and read the four questions above; then decide
   whether `copy_destination_image` stays true for Bloodborne and whether the destination should be
   marked `GpuModified` at all.
3. Only after both: commit the tree (goal 1 + goal 11 separately reads better than one commit), and
   move the default-on of bit 33 from `src/runtime_memory.c` into whatever the release process is.
