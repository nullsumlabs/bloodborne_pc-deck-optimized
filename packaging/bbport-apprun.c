// Entry point of the AppImage that packaging/appimage-ubuntu.sh builds: its AppRun.
//
// packaging/bbport-entry.c does the same job for the nix package, but its target is absolute at
// compile time — $out/libexec/bbport, known when default.nix builds it. An AppImage is mounted
// wherever its runtime chooses, so the package directory is resolved here at run time instead:
// $APPDIR when it is set (the type-2 runtime sets it), else the directory holding this executable;
// TARGET_SUFFIX is the wrapper's path inside the package.
//
// The environment work is entry.c's, for the same reason. Steam starts every non-Steam game with
// LD_PRELOAD=.../gameoverlayrenderer.so (any compatibility tool, Steam Deck included). Loaded into
// this package's own glibc, the overlay cannot find its libGL.so.1 and the first dynamic program,
// the wrapper's bash, exits at once: "error while loading shared libraries: libGL.so.1". Being
// static, this program is not affected. LD_LIBRARY_PATH, GTK_PATH and the like name host libraries
// just as incompatible with the package; Steam's Vulkan layers (overlay, fossilize) are host
// libraries too. Keep the two files' lists in step.
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef TARGET_SUFFIX
#error "TARGET_SUFFIX: the wrapper inside the package, e.g. \"/usr/share/bbport/bbport\""
#endif

static void drop(const char* name) {
    const char* value = getenv(name);
    if (!value || !*value) {
        unsetenv(name);
        return;
    }
    // Kept for diagnostics (the launcher's log shows the environment problems).
    char saved[128];
    snprintf(saved, sizeof saved, "BB_HOST_%s", name);
    setenv(saved, value, 1);
    unsetenv(name);
}

/// Copies a path the runtime or the kernel gave us, or refuses it: a truncated directory would
/// run something other than this package, so nothing here may cut it (and GCC 14's
/// -Wformat-truncation, an error in this build, is right to ask).
static int copy_path(char* out, size_t size, const char* text) {
    const size_t length = strlen(text);
    if (length >= size) {
        fprintf(stderr, "bbport: path too long for the package: %s\n", text);
        return -1;
    }
    memcpy(out, text, length + 1);
    return 0;
}

/// The package's own directory: $APPDIR when the runtime set it, else the one holding us.
static int package_directory(char* out, size_t size) {
    const char* appdir = getenv("APPDIR");
    if (appdir && *appdir) {
        return copy_path(out, size, appdir);
    }
    char exe[PATH_MAX];
    const ssize_t length = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (length <= 0) {
        perror("bbport: /proc/self/exe");
        return -1;
    }
    exe[length] = '\0';
    char* slash = strrchr(exe, '/');
    if (!slash || slash == exe) {
        fprintf(stderr, "bbport: %s: no directory to run the package from\n", exe);
        return -1;
    }
    *slash = '\0';
    return copy_path(out, size, exe);
}

int main(int argc, char** argv) {
    (void)argc;
    static const char* const host_only[] = {
        "LD_PRELOAD", "LD_LIBRARY_PATH", "LD_AUDIT", "GTK_PATH", "GTK_MODULES", "GTK_EXE_PREFIX",
        "GIO_EXTRA_MODULES", "GDK_PIXBUF_MODULEDIR", "PYTHONHOME", "PYTHONPATH",
    };
    for (size_t i = 0; i < sizeof host_only / sizeof host_only[0]; ++i) {
        drop(host_only[i]);
    }
    setenv("DISABLE_VK_LAYER_VALVE_steam_overlay_1", "1", 1);
    setenv("DISABLE_VK_LAYER_VALVE_steam_fossilize_1", "1", 1);
    // Room for TARGET_SUFFIX is what the directory buffer has to leave: the concatenation below
    // cannot truncate then, and GCC 14's -Wformat-truncation (an error here) can see that (GCC 13
    // did not ask). A path that would not fit is cut by APPDIR or /proc/self/exe, not here.
    char directory[PATH_MAX - sizeof TARGET_SUFFIX];
    if (package_directory(directory, sizeof directory) < 0) {
        return 127;
    }
    char target[PATH_MAX];
    snprintf(target, sizeof target, "%s" TARGET_SUFFIX, directory);
    argv[0] = target;
    execv(target, argv);
    perror("bbport: exec");
    return 127;
}
