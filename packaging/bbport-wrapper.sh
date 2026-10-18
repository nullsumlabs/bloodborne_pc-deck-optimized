#!/usr/bin/env bash
# Entry of the packaged port: the launcher, or the game alone with --play.
set -euo pipefail
appdir=${APPDIR:-$(cd -- "$(dirname -- "$0")/../../.." && pwd)}
share=$appdir/usr/share/bbport
export APPDIR=$appdir
export BB_PREBUILT=1
export PYTHONHOME=$appdir/usr
export PYTHONPATH=$appdir/usr/lib/python3/dist-packages
export PYTHONDONTWRITEBYTECODE=1
export PATH=$appdir/usr/bin:/usr/bin:/bin
export LD_LIBRARY_PATH=$appdir/usr/lib/x86_64-linux-gnu:$appdir/usr/lib
export GI_TYPELIB_PATH=$appdir/usr/lib/x86_64-linux-gnu/girepository-1.0:$appdir/usr/lib/girepository-1.0
export XDG_DATA_DIRS=$appdir/usr/share:/usr/share:/usr/local/share
export GSETTINGS_SCHEMA_DIR=$appdir/usr/share/glib-2.0/schemas
export LC_ALL=${LC_ALL:-C.UTF-8}
# The crash recorder (docs/CRASH_RECORDER_PLAN.md) is on in the packaged port: it keeps its events
# in memory and writes one file, $BB_DATA_DIR/logs/crash-<time>.txt, only if the game dies.
# BB_CRASH_REPORT=0 turns it off; the launcher's log names the file at start.
export BB_CRASH_REPORT=${BB_CRASH_REPORT:-1}
# The player's own settings, saves and generated files, outside the (read-only) package.
export BB_DATA_DIR=${BB_DATA_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/bbport}
mkdir -p "$BB_DATA_DIR"
# Two files name absolute paths, and the package is mounted where the AppImage runtime decides:
# fontconfig's font directories and the GdkPixbuf loader cache are written here, with the paths
# this mount actually has. They are small and rewritten each launch.
python=$appdir/usr/bin/python3
cat > "$BB_DATA_DIR/fonts.conf" <<EOF
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">
<fontconfig>
  <dir>$appdir/usr/share/fonts</dir>
  <dir>/usr/share/fonts</dir>
  <dir>/usr/local/share/fonts</dir>
  <dir prefix="xdg">fonts</dir>
  <cachedir prefix="xdg">bbport/fontconfig</cachedir>
</fontconfig>
EOF
export FONTCONFIG_FILE=$BB_DATA_DIR/fonts.conf
bundled_cache=$appdir/usr/lib/x86_64-linux-gnu/gdk-pixbuf-2.0/2.10.0/loaders.cache
if [[ -f $bundled_cache ]]; then
    "$python" -c 'import sys; s, t, a = sys.argv[1:4]; open(t, "w").write(open(s).read().replace("@APPDIR@", a))' \
        "$bundled_cache" "$BB_DATA_DIR/gdk-pixbuf-loaders.cache" "$appdir" 2>/dev/null \
        && export GDK_PIXBUF_MODULE_FILE=$BB_DATA_DIR/gdk-pixbuf-loaders.cache
fi
if [[ ${1:-} == --play ]]; then
    shift
    exec "$python" "$share/launcher/bbport_vulkan.py" bash "$share/run.sh" "$@"
fi
exec "$python" "$share/launcher/bbport_vulkan.py" "$python" "$share/launcher/bbport_launcher.py" "$@"
