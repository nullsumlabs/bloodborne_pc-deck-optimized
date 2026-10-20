# Notes for agents

## Scope

bbport runs Bloodborne's own PS4 executable (CUSA03173, 1.09) on x86-64 Linux: a hand-written
runtime replaces the system libraries, and the graphics are translated to Vulkan by a heavily
extended fork of shadPS4's video core. No game files are in the tree — a session needs the user's
own dump.

| Path | What lives there |
|---|---|
| `src/` | Loader (`probe.c`) and the HLE runtime — one `runtime_*.c` per PS4 library; plain C11, `-Werror` |
| `gpu/` | The renderer library. `gpu/shadps4/` is vendored (see `gpu/VENDOR.txt`); the port's own code is `gpu/shim/`, `gpu/layer/`, `gpu/games/`, `gpu/dlss_bridge/`, and the online module `gpu/bbnet/` |
| `launcher/` | GTK4/PyGObject launcher, the `BB_*` switches and the i18n table |
| `packaging/` | AppImage recipes (host-library, repack), AppRun, wrapper, Nix |
| `scripts/` | Offline image preparation, module linking, patch compiler |
| `tools/`, `tests/` | Scripted runs, A/B and benchmark helpers; loader/runtime/renderer tests |

Areas that carry most of the open work: the memory and translation model (old NVIDIA path vs the
AMD sparse-arena path), the label/fence race behind the pool corruption, and Deck performance.

## Where the state lives

- `OPTIMIZATION_GOALS.md`, `OPTIMIZATION_STATUS.md` — current goals and what is measured.
- `CRASH_FIXES_STATUS.md` — each ported fix, and what was verified for it.
- `docs/CRASH_RECORDER_PLAN.md` — the crash recorder: what the report holds, cost budget, what is
  left to prove.
- `docs/history/SESSION_*.md` — the newest note is the live state of the branch; read it first and
  add to it rather than a new file.
- `docs/CHANGES_*.md` — per-release notes. README.md is the user-facing description.

## Checking a change

- `bash build.sh --test` — builds `out/bb-probe` and runs every C test (including `out/crash-test`).
- `python3 -m unittest discover -s tests` — the Python tests over those binaries.
- `ninja -C out/gpu motion-history-test ui-composition-test scene-resolution-test motion-shader-test
  settings-test` — renderer tests.
- `bash build_steam_deck.sh [--test]` then `packaging/appimage-ubuntu.sh`, or
  `packaging/appimage-repack.sh --from <base.AppImage> --out <new.AppImage>` for a fast package.
- The `znver2` builds are Deck-only (Van Gogh and Sephiroth are Zen 2); keep `build.sh`'s AppImage
  portable.

## Commits

- Commit concisely: a lowercase subject naming the area, then a short body for the why (what was
  broken, or what the change buys). One concern per commit.
- **No attribution.** No `Generated with ...`, no `Co-Authored-By`, no AI/tool trailers of any
  kind — end the message at its own last line.
