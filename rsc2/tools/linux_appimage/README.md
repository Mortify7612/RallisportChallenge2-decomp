# Linux AppImage build

`build.sh` here builds a single-file AppImage of the native Linux port:
the portable glibc-2.35 binary from `../linux_portable/`, SDL2 and an
`AppRun` that finds the player's game data. Game data is never included.

```sh
rsc2/tools/linux_appimage/build.sh [jobs]
```

Output: `dist/rsc2-<version>-x86_64.AppImage`.

## What it needs

- `docker` or `podman` with a working daemon (`ENGINE=` overrides the choice).
  The build happens in `Containerfile`: Ubuntu 22.04, GCC 15 from the
  toolchain PPA -- the same recipe as the release tarball's
  `../linux_portable/Containerfile`.
- Network access the first time: glslang and the Vulkan headers at tag
  `vulkan-sdk-1.4.363.0`, SDL2 2.32.10 and appimagetool are downloaded into
  `.cache/downloads/` (gitignored). Later runs reuse them.
- The generated game code in `rsc2/src/recomp/gen` (`rsc2/regen.sh`) and the
  game's XBE, as for any build.

`appimagetool` is itself an AppImage, but the build does not need FUSE: it
is extracted once (`--appimage-extract`) into `.cache/appimagetool/` and its
`AppRun` is run from there.

## Layout

| Path | What |
|---|---|
| `build.sh` | host side: downloads, container build, AppDir, appimagetool |
| `container_build.sh` | inside the container: glslang, SDL2, the game, the bundled libraries |
| `Containerfile` | the Ubuntu 22.04 + GCC 15 image |
| `AppRun` | finds the game data and starts the binary |
| `rsc2.desktop`, `rsc2.png` | desktop entry and icon for the AppImage |
| `.cache/` | downloads and build trees (gitignored) |
| `dist/` | the built AppImage (gitignored) |

The game binary is built in `rsc2/build-appimage/`, never in `rsc2/build/`.

## Notes

- SDL2 is built from source with `SDL_*_SHARED=ON` for every backend, so the
  AppImage does not depend on the distribution's libpulse/libsystemd/
  libsndfile chain. `container_build.sh` audits the final ELF dependencies
  and fails if something outside the allow-list appears.
- `VERSION=...` overrides the version in the file name (default:
  `git describe --tags --always --dirty`), `OUT=...` the output path.
- `rsc2/RUNNING.md` has the player-facing description: where to put the game
  files, `RSC2_GAME_DIR`, FUSE caveats and the headless smoke test.
