#!/usr/bin/env bash
# Builds the AppDir AppImage out of an Ubuntu 24.04 host's own libraries — the package that ran for
# hours on the Steam Deck: 148 distro libraries, a distro Python 3.12, the port's own files, with
# glibc, the Vulkan loader and driver, X11/Wayland and MangoHud left to the system. The inventory is
# docs/APPIMAGE_DEPENDENCIES.md.
#
# packaging/appimage.sh is the other package: nix-appimage, a /nix/store inside, Mesa's Vulkan
# drivers and MangoHud bundled, debug info stripped, ~399 MB. The two are not interchangeable — this
# one needs the host's glibc 2.38 or newer, that one brings its own.
#
#   bash build_steam_deck.sh && bash packaging/appimage-ubuntu.sh    # package what out/ holds
#   bash packaging/appimage-ubuntu.sh --check                        # only list what this host lacks
#
# Needs: gcc, appimagetool (PATH or $BB_APPIMAGETOOL), and an Ubuntu 24.04 host carrying the
# libraries in packaging/appimage-libs.txt, python3.12, GTK4/libadwaita, ffmpeg 6.1 and the icon
# themes (--check names the missing packages' files; apt-get install libgtk-4-1 libadwaita-1-0
# ffmpeg adwaita-icon-theme fonts-dejavu-core python3-gi covers most).
#
# BB_HOST_USR overrides /usr, BB_APPDIR the scratch directory, BB_APPIMAGE_OUT the output path and
# BB_FORCE=1 allows writing over an existing file.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
root=$PWD

host_usr=${BB_HOST_USR:-/usr}
host_lib=$host_usr/lib/x86_64-linux-gnu
libs=$root/packaging/appimage-libs.txt
wrapper=$root/packaging/bbport-wrapper.sh
desktop=$root/packaging/bbport.desktop
icon=$root/packaging/bbport.png
appdir=${BB_APPDIR:-$root/out/appimage-ubuntu}/bbport.AppDir
out=${BB_APPIMAGE_OUT:-$root/dist/Bloodborne_PC-nocrash-bbport-steamdeck-x86_64.AppImage}
share=$appdir/usr/share/bbport
libdir=$appdir/usr/lib/x86_64-linux-gnu

# The typelibs the launcher's Python asks for, as the reference package carries them.
typelibs="Adw-1 GLib-2.0 GObject-2.0 Gdk-4.0 GdkPixbuf-2.0 Gio-2.0 Graphene-1.0 Gtk-4.0
          HarfBuzz-0.0 Pango-1.0 PangoCairo-1.0 cairo-1.0 freetype2-2.0 xlib-2.0"

check=
[[ ${1:-} == --check ]] && check=1

missing=0
want() {
    if [[ ! -e $1 ]]; then
        printf '  missing: %s\n' "$1" >&2
        missing=$((missing + 1))
    fi
}

# The host's library, wherever this distribution keeps it (multiarch, /lib, ldconfig's cache).
host_library() {
    if [[ -e $host_lib/$1 ]]; then
        echo "$host_lib/$1"
        return
    fi
    ldconfig -p 2>/dev/null | awk -v n="$1" '$1 == n { sub(/.*=> /, ""); print; exit }'
}

verify() {
    local name found
    echo "Checking what this recipe needs:" >&2
    # The repository's own pieces: the port's files and the package's wrapper, entry and menu entry.
    want "$libs"; want "$wrapper"; want "$desktop"; want "$icon"
    want "$root/run.sh"; want "$root/launcher/bbport_launcher.py"; want "$root/launcher/bbport_vulkan.py"
    want "$root/scripts/game_check.py"; want "$root/patches/Bloodborne.xml"; want "$root/tools/fsr4cap"
    want "$root/packaging/bbport-apprun.c"
    # What the build produced (same two files packaging/appimage.sh insists on).
    want "$root/out/bb-probe"; want "$root/out/gpu/libbbgpu.so"; want "$root/out/bb-gpu-capabilities"
    # The interpreter, its standard library and PyGObject.
    want "$host_usr/bin/python3.12"
    want "$host_usr/lib/python3.12/encodings/__init__.py"
    want "$host_usr/lib/python3/dist-packages/gi/_gi.cpython-312-x86_64-linux-gnu.so"
    # The launcher's toolkit.
    for name in $typelibs; do want "$host_lib/girepository-1.0/$name.typelib"; done
    want "$host_lib/gdk-pixbuf-2.0/2.10.0/loaders"
    if [[ -z $(query_loaders) ]]; then
        printf '  missing: gdk-pixbuf-query-loaders (in libgdk-pixbuf-2.0-0, or in PATH)\n' >&2
        missing=$((missing + 1))
    fi
    want "$host_lib/gtk-4.0/4.0.0"
    want "$host_usr/share/glib-2.0/schemas/gschemas.compiled"
    want "$host_usr/share/fonts/truetype/dejavu"
    want "$host_usr/share/icons/hicolor"
    want "$host_usr/share/icons/Adwaita"
    # The library set itself.
    while read -r name; do
        [[ -z $name ]] && continue
        found=$(host_library "$name")
        if [[ -z $found ]]; then
            printf '  missing: %s\n' "$host_lib/$name" >&2
            missing=$((missing + 1))
        fi
    done < "$libs"
    want "$(command -v gcc || echo /usr/bin/gcc)"
    if [[ -z $(appimagetool_path) ]]; then
        printf '  missing: appimagetool (in PATH, or named by BB_APPIMAGETOOL)\n' >&2
        missing=$((missing + 1))
    fi
    if (( missing )); then
        echo >&2
        echo "$missing piece(s) are not on this host; nothing was written." >&2
        exit 1
    fi
    echo "  all present." >&2
}

query_loaders() {
    local candidate
    for candidate in "$host_lib/gdk-pixbuf-2.0/gdk-pixbuf-query-loaders" \
                     "$host_usr/bin/gdk-pixbuf-query-loaders"; do
        [[ -x $candidate ]] && { echo "$candidate"; return; }
    done
    command -v gdk-pixbuf-query-loaders 2>/dev/null || true
}

appimagetool_path() {
    if [[ -n ${BB_APPIMAGETOOL:-} ]]; then
        echo "$BB_APPIMAGETOOL"
        return
    fi
    command -v appimagetool 2>/dev/null || true
}

verify

if [[ -n $check ]]; then
    echo "Nothing was written (--check)." >&2
    exit 0
fi

if [[ -e $out && ${BB_FORCE:-0} != 1 ]]; then
    echo "$out exists — not overwriting it (BB_FORCE=1 replaces it)." >&2
    exit 1
fi

rm -rf "$appdir"
mkdir -p "$share/bin/gpu" "$appdir/usr/bin" "$libdir/girepository-1.0" \
         "$appdir/usr/share/fonts/truetype" "$appdir/usr/share/icons" \
         "$appdir/usr/lib/python3/dist-packages" "$appdir/usr/share/glib-2.0/schemas"

# The port itself, installed the way packaging/default.nix installs it for the nix package.
install -m755 "$wrapper" "$share/bbport"
install -m644 "$root/run.sh" "$share/run.sh"
cp -r "$root/scripts" "$root/patches" "$root/launcher" "$share/"
mkdir -p "$share/tools"
cp -r "$root/tools/fsr4cap" "$share/tools/"
rm -rf "$share/tools/fsr4cap/__pycache__"
install -m755 "$root/out/bb-probe" "$share/bin/bb-probe"
install -m755 "$root/out/bb-gpu-capabilities" "$share/bin/bb-gpu-capabilities"
install -m755 "$root/out/gpu/libbbgpu.so" "$share/bin/gpu/libbbgpu.so"
# The online module and the DLSS bridge only when this build has them; the reference package has
# neither. No stripping either: it carries the debug info (libbbgpu.so is 152 MB of the 189 MB of
# libraries) and crash reports are read with those symbols.
if [[ -f $root/out/gpu/libbbnet.so ]]; then
    install -m755 "$root/out/gpu/libbbnet.so" "$share/bin/gpu/libbbnet.so"
fi
if [[ -f $root/out/libbbport_dlss.so ]]; then
    install -m755 "$root/out/libbbport_dlss.so" "$share/bin/libbbport_dlss.so"
    for ngx in "$root"/out/libnvidia-ngx-dlss.so.*; do
        [[ -f $ngx ]] && install -m755 "$ngx" "$share/bin/"
    done
fi

# A distribution Python 3.12 with PyGObject: the launcher is Python, and the package brings its own
# interpreter so the host's version cannot matter.
install -m755 "$host_usr/bin/python3.12" "$appdir/usr/bin/python3.12"
ln -sf python3.12 "$appdir/usr/bin/python3"
cp -a "$host_usr/lib/python3.12" "$appdir/usr/lib/"
cp -a "$host_usr/lib/python3/dist-packages/gi" "$appdir/usr/lib/python3/dist-packages/"

# The library set, by the names the played package carries. Dereferenced: the package holds plain
# files under their sonames, not the host's symlink chains.
while read -r name; do
    [[ -z $name ]] && continue
    cp -L "$(host_library "$name")" "$libdir/$name"
done < "$libs"

for name in $typelibs; do
    install -m644 "$host_lib/girepository-1.0/$name.typelib" "$libdir/girepository-1.0/"
done

# GdkPixbuf loaders and the GTK4 modules. The loader cache names the loaders by absolute path, and
# the mount point is the runtime's to choose, so the package's copy carries @APPDIR@ instead; the
# wrapper substitutes the mount path at launch (see docs/APPIMAGE_DEPENDENCIES.md).
cp -a "$host_lib/gdk-pixbuf-2.0" "$libdir/"
cp -a "$host_lib/gtk-4.0" "$libdir/"
loaders=$libdir/gdk-pixbuf-2.0/2.10.0/loaders
"$(query_loaders)" "$loaders"/*.so > "$loaders.cache.tmp"
sed "s|$appdir|@APPDIR@|g" "$loaders.cache.tmp" > "$loaders.cache"
rm -f "$loaders.cache.tmp"
grep -q '@APPDIR@' "$loaders.cache" || {
    echo "the loader cache names no @APPDIR@ path: gdk-pixbuf-query-loaders printed something else" >&2
    exit 1
}

install -m644 "$host_usr/share/glib-2.0/schemas/gschemas.compiled" \
    "$appdir/usr/share/glib-2.0/schemas/gschemas.compiled"
cp -a "$host_usr/share/fonts/truetype/dejavu" "$appdir/usr/share/fonts/truetype/"
cp -a "$host_usr/share/icons/hicolor" "$host_usr/share/icons/Adwaita" "$appdir/usr/share/icons/"

# The entry point: static, so Steam's LD_PRELOAD cannot stop it before the wrapper's first dynamic
# program starts, and resolving the package directory at run time (packaging/bbport-apprun.c).
gcc -O2 -Wall -Werror -static -DTARGET_SUFFIX='"/usr/share/bbport/bbport"' \
    "$root/packaging/bbport-apprun.c" -o "$appdir/AppRun"
install -m644 "$desktop" "$appdir/bbport.desktop"
install -m644 "$icon" "$appdir/bbport.png"
install -m644 "$icon" "$appdir/.DirIcon"

mkdir -p "$(dirname -- "$out")"
ARCH=x86_64 "$(appimagetool_path)" --no-appstream "$appdir" "$out"
echo "AppDir $appdir: $(du -sm "$appdir" | cut -f1) MB"
ls -lh "$out"
