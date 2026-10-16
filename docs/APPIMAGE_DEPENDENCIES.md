# The packaged AppImage: what it carries, and what the host must provide

Reference artifact: `dist/Bloodborne_PC-nocrash-bbport-steamdeck-x86_64.AppImage` — 139 MiB, AppDir
+ SquashFS/ZSTD, `.desktop` `X-AppImage-Version=0.51`, built 2026-10-15 04:06. This is the build
that was played for hours, so its contents are the known-good dependency set.

**It is not the AppImage that `packaging/appimage.sh` builds.** That path is `nix-appimage`: the
AppDir holds a `/nix/store`, an AppRun from `nix-appimage`, Mesa's Vulkan drivers (RADV without LLVM,
ANV) with their ICDs, MangoHud, and debug info stripped by `packaging/default.nix`. The reference
AppImage holds none of those: it is a plain AppDir whose libraries come from an Ubuntu 24.04-era
host, with Vulkan and MangoHud left to the system. The two have different contents, sizes and host
requirements; everything below is about the Ubuntu AppDir one.

## Layout

| Path | What it is |
|---|---|
| `AppRun` | 820 KB static x86-64 stub (Ubuntu 24.04, GCC 13.3.0): `packaging/bbport-apprun.c`, the port's entry program with its target resolved at run time — `$APPDIR`, else the directory of `/proc/self/exe` — then `<appdir>/usr/share/bbport/bbport` |
| `bbport.desktop`, `bbport.png`, `.DirIcon` | AppStream entry (`Exec=bbport`, `X-AppImage-Version=0.51`) |
| `usr/share/bbport/` | the port: `bbport` wrapper (2.25 KB), `run.sh`, `bin/`, `launcher/`, `scripts/`, `patches/`, `tools/` |
| `usr/bin/` | `python3` → `python3.12` (8.0 MB) |
| `usr/lib/python3.12/` | full CPython 3.12 stdlib (53 MB) |
| `usr/lib/python3/dist-packages/` | `gi` (PyGObject) |
| `usr/lib/x86_64-linux-gnu/` | 148 shared libraries (189 MB) + `girepository-1.0/`, `gtk-4.0/`, `gdk-pixbuf-2.0/` |
| `usr/share/` | `bbport/`, `fonts/` (9 files, 4 MB), `icons/` (830 files, 14 MB), `glib-2.0/schemas/` |

## What this repository holds for this package

| Path | What it is |
|---|---|
| `packaging/bbport-wrapper.sh` | the package's `usr/share/bbport/bbport`, byte for byte |
| `packaging/appimage-libs.txt` | the 148 library names of `usr/lib/x86_64-linux-gnu`, as extracted |
| `packaging/bbport.desktop` | the AppStream entry (`Exec=bbport`, `X-AppImage-Version=0.51`) |
| `packaging/bbport.png` | the icon (and `.DirIcon`), committed with `git add -f`: `*.png` is ignored here for screenshots, and this one is a build input |
| `packaging/appimage-ubuntu.sh` | the recipe: assembles this AppDir out of the host's `/usr` and packages it with `appimagetool` |
| `packaging/bbport-apprun.c` | the `AppRun` it compiles — `bbport-entry.c` is the nix package's equivalent |

**The recipe is `packaging/appimage-ubuntu.sh`.**
`bash build_steam_deck.sh && bash packaging/appimage-ubuntu.sh` builds and packages; `--check` lists
what a host is missing first, and `BB_HOST_USR`, `BB_APPDIR`, `BB_APPIMAGE_OUT` and `BB_FORCE`
override the host root, the scratch directory, the output path and the refusal to overwrite an
existing file. It takes the port's files from `out/`, the wrapper, desktop entry and icon from
`packaging/`, a distro Python 3.12 plus the libraries of `appimage-libs.txt` from the host's `/usr`,
and compiles the entry point. `packaging/appimage.sh` remains the *nix* package — the table at the
end of this document is the difference between the two.

## The port's own files (these come from `build.sh` / `build_steam_deck.sh`)

- `bin/bb-probe` — 1.3 MB, `with debug_info, not stripped`
- `bin/bb-gpu-capabilities` — 26 KB
- `bin/gpu/libbbgpu.so` — **152 MB**, `with debug_info, not stripped`
- `run.sh` — byte-identical to this repository's `run.sh` (verified by `diff`)
- `launcher/` (`bbport_launcher.py`, `bbport_vulkan.py`, `bbport_i18n.py`, `bbport_assets.py`,
  `bb-launcher.sh`, `install-desktop.sh`, `io.github.bbport.Launcher.desktop.in`, `shell.nix`),
  `scripts/`, `patches/Bloodborne.xml`, `tools/fsr4cap/`
- **absent:** `libbbnet.so` (no online module), the DLSS bridge, FSR 4.1.1 model assets, game files.

## How it is wired

`AppRun` does what `packaging/bbport-entry.c` does — drops the host's library injection (`LD_PRELOAD`,
`LD_LIBRARY_PATH`, `LD_AUDIT`, `GTK_PATH`, `GTK_MODULES`, `GTK_EXE_PREFIX`, `GIO_EXTRA_MODULES`,
`GDK_PIXBUF_MODULEDIR`, `PYTHONHOME`, `PYTHONPATH`, each saved as `BB_HOST_*`), disables Steam's
overlay and fossilize layers — then runs the wrapper. Steam starts every non-Steam game with the
overlay preloaded and inside this package's glibc it cannot find its own `libGL.so.1`, hence the
workaround.

**Its counterpart in this repository is `packaging/bbport-apprun.c`, not `bbport-entry.c`.** The nix
package compiles the latter with an absolute `TARGET` known at build time; an AppImage is mounted
wherever its runtime chooses, so `bbport-apprun.c` resolves the package directory at run time
(`$APPDIR`, else the directory of `/proc/self/exe`) and forms `"%s" TARGET_SUFFIX`. Every literal of
the artifact's `AppRun` is in it — the same ten variable names, `BB_HOST_%s`, both
`DISABLE_VK_LAYER_VALVE_*` names, `APPDIR`, `bbport: /proc/self/exe` and
`%s/usr/share/bbport/bbport` — so the two behave alike, though the artifact's own binary is not
byte-identical to anything built here: it came from a copy of that program that was never committed.

The wrapper is now versioned: `packaging/bbport-wrapper.sh` is the package's
`usr/share/bbport/bbport` byte for byte (`cmp`; the extracted tree's permissions are not
informative — 7z drops them). It is a self-locating bash script
(`appdir=${APPDIR:-$(cd -- "$(dirname -- "$0")/../../.." && pwd)}`) that sets:

| Variable | Value |
|---|---|
| `BB_PREBUILT` | `1` |
| `PYTHONHOME` / `PYTHONPATH` | `$appdir/usr` / `$appdir/usr/lib/python3/dist-packages` |
| `PYTHONDONTWRITEBYTECODE` | `1` |
| `PATH` | `$appdir/usr/bin:/usr/bin:/bin` |
| `LD_LIBRARY_PATH` | `$appdir/usr/lib/x86_64-linux-gnu:$appdir/usr/lib` |
| `GI_TYPELIB_PATH` | `$appdir/usr/lib/x86_64-linux-gnu/girepository-1.0:$appdir/usr/lib/girepository-1.0` |
| `XDG_DATA_DIRS` | `$appdir/usr/share:/usr/share:/usr/local/share` |
| `GSETTINGS_SCHEMA_DIR` | `$appdir/usr/share/glib-2.0/schemas` |
| `LC_ALL` | `C.UTF-8` |
| `BB_DATA_DIR` | `${XDG_DATA_HOME:-$HOME/.local/share}/bbport` |

Two absolute paths cannot be baked in at package time because the mount point is chosen by the
runtime, so the wrapper writes them on each launch: `$BB_DATA_DIR/fonts.conf` (pointing at
`$appdir/usr/share/fonts` plus the host's font directories) and `$BB_DATA_DIR/gdk-pixbuf-loaders.cache`
(`usr/lib/x86_64-linux-gnu/gdk-pixbuf-2.0/2.10.0/loaders.cache` with `@APPDIR@` replaced). Entry is
`python3 bbport_vulkan.py bbport_launcher.py` (or `run.sh` for `--play`).

## Bundled runtime libraries (189 MB, 148 files)

| Group | What is in it |
|---|---|
| GTK4 / GLib (launcher) | `libgtk-4.so.1`, `libadwaita-1.so.0`, `libgio-2.0`, `libglib-2.0`, `libgobject-2.0`, `libgmodule-2.0`, `libgirepository-1.0.so.1`, `librsvg-2.so.2`, `libgdk_pixbuf-2.0`, `libcairo*` (3), `libpango*` (3), `libgraphene-1.0`, `libepoxy.so.0`, `libharfbuzz`, `libfontconfig`, `libfreetype`, `libfribidi`, `libgraphite2`, `libthai`, `libdatrie`, `libpixman-1`, `libpng16`, `libjpeg.so.8`, `libtiff.so.6`, `libwebp*` (3), `libjxl*` (2), `liblcms2`, `libopenjp2`, `libexpat`, `libxml2`, `libXdamage`, `libXinerama` |
| ffmpeg 6.1 (AVPlayer / movies) | `libavcodec.so.60`, `libavformat.so.60`, `libavutil.so.58`, `libswscale.so.7`, `libswresample.so.4` + codecs: `libaom`, `libdav1d`, `librav1e`, `libSvtAv1Enc`, `libx264`, `libx265`, `libvpx`, `libvpl`, `libtheora*` (2), `libvorbis*` (3), `libopus`, `libmp3lame`, `libmpg123`, `libopenmpt`, `libsoxr`, `libspeex`, `libtwolame`, `libshine`, `libgsm`, `libcodec2`, `libchromaprint`, `libzvbi`, `libogg`, `libbluray`, `libudfread`, `libsrt-gnutls`, `librist`, `librtmp`, `libssh*` (2), `libzmq`, `librabbitmq`, `libnorm`, `libpgm-5.3`, `libsnappy`, `libgme` |
| Python + PyGObject | `python3.12`, stdlib, `dist-packages/gi`, `_gi.cpython-312-*.so`, 14 typelibs (`Gtk-4.0`, `Adw-1`, `Gdk-4.0`, `GdkPixbuf-2.0`, `Gio-2.0`, `GLib-2.0`, `GObject-2.0`, `Graphene-1.0`, `HarfBuzz-0.0`, `Pango-1.0`, `PangoCairo-1.0`, `cairo-1.0`, `freetype2-2.0`, `xlib-2.0`) |
| The port's own dependencies | `libSDL3.so.0`, `libZydis.so.4.0`, `libxxhash.so.0`, `libfmt.so.9`, `libstdc++.so.6`, `libgcc_s.so.1`, `libz`, `libzstd`, `liblz4`, `libbz2`, `liblzma`, `libgomp`, `libnuma` |
| TLS / HTTP / accounts | `libssl.so.3`, `libcrypto.so.3`, `libgnutls.so.30`, `libcurl-gnutls.so.4`, `libnghttp2`, `libpsl`, `libidn2`, `libunistring`, `libhogweed`, `libnettle`, `libgmp`, `libtasn1`, `libp11-kit`, `libgpg-error`, `libgcrypt`, `libldap.so.2`, `liblber.so.2`, `libsasl2`, `libkrb5*` (4), `libgssapi_krb5`, `libk5crypto`, `libkeyutils`, `libcom_err`, `libsodium`, `libmbedcrypto` |
| Misc | `libicu*` 74 (2), `libbrotli*` (3), `libsystemd.so.0`, `libselinux`, `libcap`, `libmount`, `libblkid`, `libbsd`, `libmd`, `libpcre2-8`, `libffi`, `libyaml-0`, `libcjson`, `libappstream.so.5`, `libxmlb`, `libstemmer`, `libLerc`, `libdeflate`, `libsharpyuv`, `libjbig`, `liblzo2` |

Full list, as extracted, in [the appendix](#appendix-the-full-library-list).

## What the host must provide

- **glibc and the dynamic loader** — none is bundled. Measured from the ELF symbol versions:
  `bb-probe`, `libbbgpu.so` and `python3.12` require up to **`GLIBC_2.38`**;
  `bb-gpu-capabilities` stops at `GLIBC_2.34`. So the host needs glibc ≥ 2.38 (Ubuntu 24.04 is 2.39;
  SteamOS 3.6 is 2.38, 3.5 is 2.37 and would refuse to start it).
- **A Vulkan loader and driver** — no `libvulkan.so.1`, no ICD JSON, no RADV/ANV, no lavapipe is
  bundled. `launcher/bbport_vulkan.py` still discovers a host NVIDIA ICD and passes its vendor
  libraries, as before.
- **X11/Wayland client libraries and the window system** — `libX11`, `libxcb`, `libwayland*` are not
  bundled; SDL3 links them from the host.
- **MangoHud, if the player wants the overlay** — not bundled here (the nix package bundles it).
- Steam's overlay is switched off by `AppRun` rather than worked around.

## How this differs from the nix package (`packaging/default.nix` + `packaging/appimage.sh`)

| | this AppDir (reference) | nix-appimage |
|---|---|---|
| Vulkan | host loader + driver | bundles Mesa RADV (no LLVM, ACO) and ANV + their ICDs |
| MangoHud | host | bundled, 64-bit, without xdg-utils |
| Debug info | kept: `libbbgpu.so` 152 MB, `bb-probe` 1.3 MB | `strip --strip-debug` on both |
| Runtime | 148 distro libraries + a distro Python 3.12 | the package's own Nix closure |
| glibc | host, ≥ 2.38 needed | package's own |
| Size | 139 MiB compressed | 399 MB (per `docs/CHANGES_0.51.md`) |
| Built by | `bash packaging/appimage-ubuntu.sh` (AppDir + `appimagetool`) | `bash packaging/appimage.sh` (`nix bundle`, `nix-appimage`) |

## Appendix: the full library list

`usr/lib/x86_64-linux-gnu` (148 files, 3 directories: `gdk-pixbuf-2.0/`, `girepository-1.0/`,
`gtk-4.0/`):

```
_gi.cpython-312-x86_64-linux-gnu.so
libLerc.so.4
libSDL3.so.0
libSvtAv1Enc.so.1
libXdamage.so.1
libXinerama.so.1
libZydis.so.4.0
libadwaita-1.so.0
libaom.so.3
libappstream.so.5
libavcodec.so.60
libavformat.so.60
libavutil.so.58
libblkid.so.1
libbluray.so.2
libbrotlicommon.so.1
libbrotlidec.so.1
libbrotlienc.so.1
libbsd.so.0
libbz2.so.1.0
libcairo-gobject.so.2
libcairo-script-interpreter.so.2
libcairo.so.2
libcap.so.2
libchromaprint.so.1
libcjson.so.1
libcodec2.so.1.2
libcom_err.so.2
libcrypto.so.3
libcurl-gnutls.so.4
libdatrie.so.1
libdav1d.so.7
libdeflate.so.0
libepoxy.so.0
libexpat.so.1
libffi.so.8
libfmt.so.9
libfontconfig.so.1
libfreetype.so.6
libfribidi.so.0
libgcc_s.so.1
libgcrypt.so.20
libgdk_pixbuf-2.0.so.0
libgio-2.0.so.0
libgirepository-1.0.so.1
libglib-2.0.so.0
libgme.so.0
libgmodule-2.0.so.0
libgmp.so.10
libgnutls.so.30
libgobject-2.0.so.0
libgomp.so.1
libgpg-error.so.0
libgraphene-1.0.so.0
libgraphite2.so.3
libgsm.so.1
libgssapi_krb5.so.2
libgtk-4.so.1
libharfbuzz.so.0
libhogweed.so.6
libhwy.so.1
libicudata.so.74
libicuuc.so.74
libidn2.so.0
libjbig.so.0
libjpeg.so.8
libjxl.so.0.7
libjxl_threads.so.0.7
libk5crypto.so.3
libkeyutils.so.1
libkrb5.so.3
libkrb5support.so.0
liblber.so.2
liblcms2.so.2
libldap.so.2
liblz4.so.1
liblzma.so.5
liblzo2.so.2
libmbedcrypto.so.7
libmd.so.0
libmount.so.1
libmp3lame.so.0
libmpg123.so.0
libnettle.so.8
libnghttp2.so.14
libnorm.so.1
libnuma.so.1
libogg.so.0
libopenjp2.so.7
libopenmpt.so.0
libopus.so.0
libp11-kit.so.0
libpango-1.0.so.0
libpangocairo-1.0.so.0
libpangoft2-1.0.so.0
libpcre2-8.so.0
libpgm-5.3.so.0
libpixman-1.so.0
libpng16.so.16
libpsl.so.5
librabbitmq.so.4
librav1e.so.0
librist.so.4
librsvg-2.so.2
librtmp.so.1
libsasl2.so.2
libselinux.so.1
libsharpyuv.so.0
libshine.so.3
libsnappy.so.1
libsodium.so.23
libsoxr.so.0
libspeex.so.1
libsrt-gnutls.so.1.5
libssh-gcrypt.so.4
libssh.so.4
libssl.so.3
libstdc++.so.6
libstemmer.so.0d
libswresample.so.4
libswscale.so.7
libsystemd.so.0
libtasn1.so.6
libthai.so.0
libtheoradec.so.1
libtheoraenc.so.1
libtiff.so.6
libtwolame.so.0
libudfread.so.0
libunistring.so.5
libvorbis.so.0
libvorbisenc.so.2
libvorbisfile.so.3
libvpl.so.2
libvpx.so.9
libwebp.so.7
libwebpmux.so.3
libx264.so.164
libx265.so.199
libxml2.so.2
libxmlb.so.2
libxvidcore.so.4
libxxhash.so.0
libyaml-0.so.2
libz.so.1
libzmq.so.5
libzstd.so.1
libzvbi.so.0
```

GdkPixbuf loaders (12) in `usr/lib/x86_64-linux-gnu/gdk-pixbuf-2.0/2.10.0/loaders/`, their cache
shipped with `@APPDIR@` placeholders.

## How this was read, and what is not verified

Read from the artifact on a macOS host: `7z l -slt` for the manifest, `7z x` to unpack the SquashFS,
then `ls`, `file`, `diff` (against this repository's `run.sh`), `cmp` and `grep -a` for the GLIBC
symbol versions and the `AppRun` literals. Nothing was built or run: the claim that it works is the
hours of play reported by the person who built it, not a test performed here.

Open:

- **The artifact's `AppRun` is its own binary**, built from a copy of the entry program that no ref
  carries. `packaging/bbport-apprun.c` is written to behave like it — its literals match one for one
  — but the bytes are not the artifact's.
- **`packaging/appimage-ubuntu.sh` has not been run end to end.** It was exercised against a stubbed
  host and toolchain: `--check` writes nothing, a missing piece is named and the build stops, the
  output file is never overwritten without `BB_FORCE=1`, and the AppDir it produced carried the
  reference's 2739 paths, name for name. No AppImage has been built or started with it, and the
  `gdk-pixbuf-query-loaders` step has only ever run against a stub.
- **The `AppRun` does not tell us how the 148 libraries were chosen** — the list here is what the
  package contains, and the recipe reproduces it by name.
- **These binaries are the portable build**, not a Zen 2 one: the artifact predates
  `build_steam_deck.sh`, so no `-march` item has been measured yet. Rebuilding it through
  `bash build_steam_deck.sh` and the same AppDir step is what would produce the Deck-targeted
  equivalent — and it needs a Linux x86-64 host (`packaging/appimage.sh` needs `readelf`, `nix` and
  `nix-appimage`; an Ubuntu 24.04 box with `appimagetool` is what produced this one).
