# The Deck AppImage, built the way deadinside28's 0.6 image is built

Their `0.6` AppImage (`Bloodborne-bbport-deadinside28-0.6.AppImage`) is not an FHS AppDir: it is a
`nix bundle --bundler github:ralismark/nix-appimage` of `packaging/default.nix`, i.e. a squashfs
holding a `/nix/store`. The package in this tree is the same thing — same derivation shape, same
bundler — with one deliberate difference: the port's own binaries are built for Zen 2.

```
bash build_steam_deck.sh      # out/: -march=znver2 -mtune=znver2 (re-enters shell.nix by itself)
bash packaging/appimage.sh    # dist/Bloodborne-bbport-x86_64.AppImage
```

`bash build.sh && bash packaging/appimage.sh` is the portable (baseline x86-64) build of the same
image. `packaging/appimage-ubuntu.sh` and `packaging/appimage-repack.sh` are the *other* recipes:
an FHS AppDir with libraries copied off the build host, not a nix store.

## What "near-exact" means, concretely

| piece | theirs (0.6) | ours |
|---|---|---|
| bundler | `nix bundle --bundler ralismark/nix-appimage` | same, same script (`appimage.sh`) |
| entry | static `bin/bbport` → `libexec/bbport` (shell wrapper) → store python + GTK4 launcher | same files, same wiring |
| Vulkan drivers | `bbport-vulkan-drivers`: RADV + ANV from Mesa, ICD `library_path` rewritten into the package, `BB_BUNDLED_VK_DRIVER_FILES` exported by the wrapper | same, Mesa 26.2.5 |
| FSR 4 assets | `share/bbport/fsr4_shaders` | same (fetched with `tools/fetch_fsr4_assets.sh`) |
| launcher stack | gtk4 (no GStreamer), libadwaita, MangoHud, python3+pygobject, spirv-tools | same components |
| port binaries | stock x86-64 build, GCC 15.3/16.2 | **znver2 build, GCC 16.2** |

Differences that are just "newer nixpkgs" rather than recipe changes (their image predates this
channel by a few weeks): Mesa 26.2.5 vs 26.2.4, glibc 2.44-57 vs 2.44-25, bash 5.3p20 vs 5.3p15,
280 vs 399 store paths and 371 MB vs 505 MB (their closure kept more of the desktop stack:
pipewire, alsa-lib, dbus; this tree's overrides trim GTK 4, MangoHud and python to what the port
loads).
Pin `(import <nixpkgs>)` to their revision if an exact bit-for-bit twin is ever needed.

## What was verified (2026-10-22 build)

Commands are from the tree root; the AppImage was extracted with `--appimage-extract`.

| claim | evidence |
|---|---|
| the Deck flags reached both compilers | `out/gpu/CMakeCache.txt`: `CMAKE_C_FLAGS:STRING=-march=znver2 -mtune=znver2` (and `_CXX_`); `DW_AT_producer` of `out/bb-probe`: `GNU C11 16.2.0 … -march=znver2 -mtune=znver2` |
| the code really is Zen 2 | `libbbgpu.so`: 8 462 `%ymm`, 384 `vfmadd*`, 48 `vpermq`, BMI2 `shlx`/`rorx` present; **0** `%zmm` |
| the image carries that build, not the old one | packaged `share/bbport/bin/gpu/libbbgpu.so` shows the identical counts (8 462 / 384 / 48), GCC 16.2.0, 23.5 MB after `strip --strip-debug` |
| it loads like theirs | `.interp` of `bb-probe` = `/nix/store/…-glibc-2.44-57/lib/ld-linux-x86-64.so.2` (store loader, as in their image), and every `NEEDED` of `bb-probe`, `libbbgpu.so`, `libbbnet.so` resolves inside the image through the RUNPATH |
| the bundled driver is selected | `libexec/bbport` sets `BB_BUNDLED_VK_DRIVER_FILES` to both ICDs; `radeon_icd.x86_64.json` names the bundled `libvulkan_radeon.so`; launching `out/nix-roots/bbport/bin/bbport` prints `Vulkan: bundled AMD/Intel drivers; no accessible NVIDIA ICD found` and then loads GTK4 (it stops at "no display" in a sandbox, which is the expected end of that smoke test) |

**Not verified:** no frame was rendered and no frame time was measured — this host is a Haswell
(`i7-4770`), where `-march=znver2` code (AMD-only `clzero`/`wbnoinvd`/SSE4A) may fault, and its
kernel refuses the user namespaces `AppRun` needs to mount the squashfs (`cannot write uid_map`).
Both the runtime check and any A/B against the baseline image have to happen on a Deck or another
Zen 2 machine:

```
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/radeon_icd.x86_64.json ./Bloodborne-bbport-x86_64.AppImage
```

forces SteamOS's own Mesa even with ours bundled (the same A/B the stock image allows).

## Two things a first run trips over

- `nix bundle` is an experimental command; `packaging/appimage.sh` now passes
  `--extra-experimental-features 'nix-command flakes'`, so a stock `nix.conf` needs no edit.
- `fsr4_shaders/` is gitignored (Q2RTX-built assets from AMD's MIT-licensed FSR 4), and
  `default.nix` installs it. On a fresh clone run `bash tools/fetch_fsr4_assets.sh` first (189
  files, 30 MB), otherwise the derivation fails with `cp: cannot stat 'fsr4_shaders'`.
