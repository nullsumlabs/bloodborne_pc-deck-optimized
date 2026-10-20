#!/usr/bin/env bash
# Packages the port out of an AppImage this project already built: the package's own files
# (bb-probe, the GPU library, the online module, run.sh, the launcher, scripts, patches, tools) are
# replaced with what out/ and the working tree hold now, and everything else is kept — the
# distribution's libraries, its Python 3.12 with PyGObject, the GTK typelibs, the packaged ffmpeg.
#
#   bash build_steam_deck.sh && bash packaging/appimage-repack.sh \
#       --from dist/Bloodborne_PC-nocrash-bbport-steamdeck-0.51.1.AppImage \
#       --out  dist/Bloodborne_PC-flightrec--bbport-steamdeck-x86_64.AppImage
#
# Why this exists next to packaging/appimage-ubuntu.sh: that recipe copies ~150 libraries off the
# build host and insists on all of them (ffmpeg 6.1 and its codec set, GTK4, PyGObject, the icon
# themes, appimagetool). A host that cannot be made to carry all that can still produce a package
# from one that already ran: a library set that has been played for hours is a known-good one.
#
# What it needs: unsquashfs/mksquashfs (squashfs-tools) and python3. No appimagetool (the base
# carries the ELF runtime that is put back in front, byte for byte) and no FUSE (unsquashfs reads
# the filesystem directly, so nothing is mounted).
#
# The libraries are the base's, so a package repacked from a Deck build stays a Deck package: keep
# the -znver2 runtime by repacking a -steamdeck AppImage with binaries from build_steam_deck.sh.
#
# BB_REPACK_DIR overrides the scratch directory, BB_FORCE=1/--force allows writing over the output.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
root=$PWD

out=$root/dist/Bloodborne_PC-flightrec--bbport-steamdeck-x86_64.AppImage
from=
force=${BB_FORCE:-0}
work=${BB_REPACK_DIR:-$root/out/appimage-repack}

usage() {
    sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'
}

while (( $# )); do
    case ${1:-} in
        --from) from=${2:-}; shift 2 ;;
        --out) out=${2:-}; shift 2 ;;
        --force) force=1; shift ;;
        --help|-h) usage; exit 0 ;;
        *) echo "unknown option: $1 (--from, --out, --force)" >&2; exit 1 ;;
    esac
done

say() { printf '%s\n' "$*" >&2; }
die() { printf 'repack: %s\n' "$*" >&2; exit 1; }

# The base: the one named, or the newest package in dist/ that is not the output itself.
if [[ -z $from ]]; then
    from=$(ls -t "$root"/dist/*.AppImage 2>/dev/null | grep -vxF -- "$out" | head -n1 || true)
    [[ -n $from ]] || die "no AppImage in dist/ to repack from (--from <file>)"
    say "Repacking from $from (newest in dist/)."
fi
# Both to an absolute path: the check at the end extracts the package from a scratch directory,
# where a relative --out names nothing (it fails there as "the runtime did not find its
# filesystem", which is not what happened).
absolute() {
    case $1 in
        /*) echo "$1" ;;
        *) echo "$PWD/$1" ;;
    esac
}
from=$(absolute "$from")
out=$(absolute "$out")
[[ -f $from ]] || die "no such AppImage: $from"
[[ -e $out && $force != 1 ]] && die "$out exists (--force replaces it)"

for tool in unsquashfs mksquashfs; do
    command -v "$tool" >/dev/null || die "$tool is not installed (squashfs-tools)"
done

# What the package carries from this tree, as packaging/appimage-ubuntu.sh installs it.
for piece in out/bb-probe out/gpu/libbbgpu.so out/bb-gpu-capabilities \
             launcher/bbport_launcher.py launcher/bbport_vulkan.py scripts/game_check.py \
             patches/Bloodborne.xml packaging/bbport-wrapper.sh; do
    [[ -e $root/$piece ]] || die "$piece is missing: build first (build_steam_deck.sh)"
done
if [[ ${BB_NET:-ON} == ON && ! -f $root/out/gpu/libbbnet.so ]]; then
    die "out/gpu/libbbnet.so is missing: build with the online module, or pass BB_NET=OFF"
fi

# The squashfs starts where the AppImage runtime looks for it: it scans forward from the end of its
# own ELF, and the runtime's bytes contain the string "hsqs" of its own search code, so the first
# match is not the superblock. A candidate counts only when unsquashfs reads a version 4 superblock
# there.
squashfs_offset() {
    local file=$1 candidate
    while read -r candidate; do
        candidate=${candidate%%:*}
        if unsquashfs -s -o "$candidate" "$file" >/dev/null 2>&1; then
            echo "$candidate"
            return 0
        fi
    done < <(LC_ALL=C grep -abo 'hsqs' "$file" | head -n8)
    return 1
}

offset=$(squashfs_offset "$from") || die "$from: no squashfs superblock found in it"
say "Base: squashfs at byte $offset of $(stat -c%s "$from") ($(basename -- "$from"))."

# The base's format, so the package keeps the layout it was built with (compressor and block size
# are what the runtime cares about; appimagetool writes zstd and 128K here).
super=$(unsquashfs -s -o "$offset" "$from")
compressor=$(sed -n 's/^Compression \([a-z0-9]*\).*/\1/p' <<<"$super")
block=$(sed -n 's/^Block size \([0-9]*\).*/\1/p' <<<"$super")
compressor=${compressor:-gzip}
block=${block:-131072}
say "Format: compression $compressor, block size $block."

rm -rf "$work"
mkdir -p "$work"
appdir=$work/bbport.AppDir

# The runtime (everything before the old filesystem) and the filesystem itself, read without
# mounting. The runtime goes back in front unchanged, so the new package is found the same way.
head -c "$offset" "$from" > "$work/runtime"
tail -c "+$((offset + 1))" "$from" > "$work/base.squashfs"
say "Unpacking the base..."
unsquashfs -no-progress -no-xattrs -f -d "$appdir" "$work/base.squashfs" >/dev/null

# The port, installed as the recipe installs it (same paths, same modes).
share=$appdir/usr/share/bbport
bindir=$share/bin
[[ -d $share ]] || die "$from does not look like this port's package: no usr/share/bbport in it"
mkdir -p "$share/tools" "$bindir/gpu"
install -m755 "$root/packaging/bbport-wrapper.sh" "$share/bbport"
install -m644 "$root/run.sh" "$share/run.sh"
rm -rf "$share/scripts" "$share/patches" "$share/launcher"
cp -r "$root/scripts" "$root/patches" "$root/launcher" "$share/"
cp -r "$root/tools/fsr4cap" "$share/tools/"
find "$share" -name __pycache__ -type d -prune -exec rm -rf {} +
install -m755 "$root/out/bb-probe" "$bindir/bb-probe"
install -m755 "$root/out/bb-gpu-capabilities" "$bindir/bb-gpu-capabilities"
install -m755 "$root/out/gpu/libbbgpu.so" "$bindir/gpu/libbbgpu.so"
if [[ ${BB_NET:-ON} == ON ]]; then
    install -m755 "$root/out/gpu/libbbnet.so" "$bindir/gpu/libbbnet.so"
else
    rm -f "$bindir/gpu/libbbnet.so"
    say 'BB_NET=OFF: the online module is not in this package.'
fi
[[ -f $root/out/libbbport_dlss.so ]] && install -m755 "$root/out/libbbport_dlss.so" "$bindir/libbbport_dlss.so"

# The package says which build it is, so a report or a save folder can be traced back to it.
version=${BB_REPACK_VERSION:-0.51.1-flightrec}
desktop=$appdir/bbport.desktop
if [[ -f $desktop ]]; then
    if grep -q '^X-AppImage-Version=' "$desktop"; then
        sed -i "s/^X-AppImage-Version=.*/X-AppImage-Version=$version/" "$desktop"
    else
        printf 'X-AppImage-Version=%s\n' "$version" >> "$desktop"
    fi
fi

say "Packing..."
mksquashfs "$appdir" "$work/new.squashfs" -noappend -no-progress -no-xattrs -root-owned \
    -comp "$compressor" -b "$block" >/dev/null

mkdir -p "$(dirname -- "$out")"
cat "$work/runtime" "$work/new.squashfs" > "$out"
chmod 755 "$out"

# Check the package the way its runtime does: extract it (no FUSE needed) and look at what came
# out. A wrong offset or a mangled filesystem fails here rather than on the Deck.
say "Checking the package..."
check=$work/check
rm -rf "$check"; mkdir -p "$check"
(cd "$check" && "$out" --appimage-extract >/dev/null 2>&1) ||
    die "$out does not extract: the runtime did not find its filesystem"
extracted=$check/squashfs-root
for piece in usr/share/bbport/bbport usr/share/bbport/run.sh usr/share/bbport/bin/bb-probe \
             usr/share/bbport/bin/gpu/libbbgpu.so AppRun; do
    [[ -e $extracted/$piece ]] || die "$out: $piece is not in the package"
done
cmp -s "$root/out/bb-probe" "$extracted/usr/share/bbport/bin/bb-probe" ||
    die "$out: the packaged bb-probe is not the one this tree built"
grep -q '^export BB_CRASH_REPORT=' "$extracted/usr/share/bbport/bbport" ||
    die "$out: the wrapper does not set BB_CRASH_REPORT"
grep -q 'Crash report:' "$extracted/usr/share/bbport/run.sh" ||
    die "$out: the packaged run.sh does not name the crash report"
# The port's own binaries are not stripped (-rdynamic, debug info kept for crash reports), so the
# recorder can be looked for by name. The symbols are written out first: piping nm into `grep -q`
# makes grep close the pipe once it has its match, and with pipefail set the SIGPIPE nm takes for
# that would fail the check even when the symbol was found.
nm -C "$extracted/usr/share/bbport/bin/bb-probe" > "$work/symbols.txt" 2>/dev/null || true
grep -q 'runtime_crash_begin' "$work/symbols.txt" ||
    die "$out: the packaged bb-probe has no crash recorder in it"

rm -rf "$check"
say "AppDir: $(du -sm "$appdir" | cut -f1) MB"
ls -lh "$out" >&2
say "md5: $(md5sum "$out" | cut -d' ' -f1)"
say "Built $out (base $(basename -- "$from"), $version)."
