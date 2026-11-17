# Linux support

Linux support has two tracks:

1. **Wine MVP:** build and run the existing Windows target in an isolated Wine prefix.
2. **Native port:** build an ELF executable against the system's SDL2, Vulkan, libpng and FFmpeg
   (`installer/dcme_install_linux.py`). It compiles, links and starts up to the window in CI-like containers; it has
   not yet been played end to end on real Linux hardware with a disc image.

The MVP preserves the project's legal/build model: the player supplies a supported disc image and the installer builds
the game locally. No game code or assets are distributed.

## Wine MVP

Requirements:

- 64-bit Wine with `winepath` (Wine 8+ recommended);
- working 64-bit Vulkan drivers for the GPU, including the distribution's 32/64-bit Wine Vulkan packages as applicable;
- the **unpacked Windows release package**, because a source checkout intentionally does not contain Python,
  llvm-mingw, SDL2, or the Vulkan SDK bundle;
- a supported Track 1 `.bin` or `.cue` image.

Check the host:

```sh
./linux/dcme-wine --package /path/to/DinoCrisisME_unpacked doctor
```

Build/install from the player's disc:

```sh
./linux/dcme-wine --package /path/to/DinoCrisisME_unpacked install \
  "/path/to/Dino Crisis (USA) (v1.1) (Track 1).bin" \
  "$HOME/Games/DinoCrisisME"
```

Run it:

```sh
./linux/dcme-wine run "$HOME/Games/DinoCrisisME"
```

The default prefix is `${XDG_DATA_HOME:-$HOME/.local/share}/dcme/wineprefix`. Set `WINEPREFIX` or pass `--prefix` to
change it. `DCME_WINE` and `DCME_WINEPATH` can select alternate Wine binaries.

### Proton / Steam Deck

The generated Windows game is a good Proton candidate: input and audio use SDL2, rendering uses Vulkan, and the
installer already detects Wine and avoids APIs known to fail in older Wine. For the first MVP, run the installer via
Wine as above, then add `DinoCrisisME.exe` as a non-Steam game and force a recent Proton version. The disc image must
remain at the path embedded during installation; if it moves, the game asks for it again.

Proton is not used directly by this wrapper because its command-line runtime and prefix layout are Steam-managed and
not a stable public interface. A later iteration can generate a Steam shortcut once the Wine MVP is validated on a
real Linux/Steam Deck host.

## Native build

Requirements (Debian/Ubuntu names; the installer prints the command for apt, dnf, pacman or zypper):

```sh
sudo apt install build-essential clang cmake ninja-build pkg-config libsdl2-dev libvulkan-dev libpng-dev \
  libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev
```

plus the Vulkan driver of the GPU (Mesa RADV/ANV, or NVIDIA's). Then, from a source checkout or the unpacked package:

```sh
python3 installer/dcme_install_linux.py -DiscPath "/path/Dino Crisis (USA) (v1.1) (Track 1).bin" -InstallPath ~/Games/DinoCrisisME
~/Games/DinoCrisisME/DinoCrisisME.sh
```

It runs the same steps as the Windows setup (recognise the disc, pinned psxrecomp, mod patch, recompile the player's
executable, hooks, rooms, data pack) with the system toolchain: Clang `-O2` when present (the compiler of the Windows
release), else GCC. The game folder holds `DinoCrisisME`, `dcme_data.pak` (PE resources do not exist on Linux),
`data/`, `mods/`, and the launcher `DinoCrisisME.sh`, which raises the stack limit to the 64 MB the Windows build links
and writes `DinoCrisisME_log.txt`. A menu entry goes to `~/.local/share/applications/dinocrisis-me.desktop`.

### How the port is put together

- `mod/src/include/dc_win32_posix.h` + `mod/src/runtime/dc_win32_posix.c`: the Win32 subset the mod runtime uses
  (FindFirstFile, wide paths, attributes, events, waitable threads, `_findfirst`/`_stat64`, ...) implemented over POSIX.
  `\` is a path separator and a missing path is looked up again case-insensitively, as NTFS would find it, so mod
  folders made on Windows work unchanged. The Windows build never includes it.
- Movies from mods (`dc_movie.cpp`): FFmpeg instead of Media Foundation, same interface (BGRA frames, int16 sound).
- PNG from mods and splash screens: libpng (`dc_png.cpp`) instead of WIC.
- Live reload of `mods/` (`dc_mods.c`): a once-a-second fingerprint of the first two folder levels instead of
  ReadDirectoryChangesW; mod packages (hard links / copies), WAV sounds and MP4 movies now work on Linux too.
- Packed data (`dcme_data.pak`): the stand-ins for `_stat64`/`_findfirst`/`GetFileAttributesEx` are built on every
  platform, so shaders, poses and faces load from the pack instead of being looked for as loose files.
- Presentation: the D3D11 shared-texture path stays Windows-only (`dcvk_shared_attach` reports it unavailable), the
  DXGI refresh grid is off, and the frame goes through SDL/Vulkan.
- Framework files (main.cpp, gpu.c, gpu_sw_renderer.c, gte.cpp, debug_server.c, interrupts.c, psx_fiber.c): small
  guarded edits, shipped through `mod/patches/psxrecomp-dcme.dcpatch` like every other framework change.
- `dc_boot.c`: enters `data/` next to the executable (as on Windows) and keeps stderr instead of redirecting it to `NUL`.

### Keeping the patches valid

Every disc version other than USA v1.1 ships `mod/versions/<v>/runtime.dcpatch`, which checks the sha256 of the mod
files it edits. Any edit of such a file breaks those versions until their patches are rebased. After editing runtime
or framework files run:

```sh
python3 linux/native/repatch.py <pristine psxrecomp> --framework-tree <prepared tree with your framework edits>
PSXRECOMP_PRISTINE=<pristine psxrecomp> bash linux/tests/test_patches.sh
```

`repatch.py` merges each version's edits three-way onto the new files (`git merge-file`), regenerates the patches,
proves each one round-trips, and stops on a conflict. The framework patch is regenerated only from a prepared tree;
the repository never stores framework text (`posdiff.py audit` stays at 0).

### Build checks without a disc

```sh
docker build -t dcme-linux-build linux/native
# a prepared framework tree (pinned psxrecomp + mod patch + mod/src copied in) mounted at /fw:
docker run --rm -v <tree>:/fw -v "$PWD/linux":/l dcme-linux-build /l/native/compile_check.sh /fw
```

`compile_check.sh` compiles every runtime translation unit for Linux. Linking it with stubs in place of the
recompiled game leaves only the game's own symbols unresolved, and the full CMake build of `mod/project` produces a
runnable ELF that starts up to renderer creation in a container without a GPU.

### Still open

- A full install and play-through on real hardware (Steam Deck, AMD/Intel/NVIDIA) and a performance baseline.
- Folder change notifications for the faces folder (it falls back to its rescan every 2 s).
- An x86 MXCSR mode in the software renderer has no ARM equivalent (only relevant to ARM Linux).
- A Linux release package (the Windows package carries Python, llvm-mingw, SDL2 and the Vulkan SDK; Linux uses the
  system's).

## Tests

```sh
bash linux/tests/test_native_foundation.sh      # gate + data pack + platform + Win32-over-POSIX tests
PSXRECOMP_PRISTINE=<dir> bash linux/tests/test_patches.sh
```

The MVP test uses fake Wine binaries, so it can run on any POSIX host without a disc image:

```sh
bash linux/tests/test_dcme_wine.sh
```

It verifies path conversion, installer arguments, isolated-prefix propagation, game launch, and rejection of an
incomplete source-only package. Real GPU/gameplay validation still requires Linux hardware, Wine, and the player's
supported disc image.
