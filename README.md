# Dino Crisis - Modern Edition

A fan mod of Dino Crisis (PS1, 1999): the game is rebuilt from your own disc into a native Windows program, made to look and play like the Resident Evil remakes.

**Download:** `DinoCrisisME_Setup.exe` on the [Releases](../../releases) page.

## Install

1. Download `DinoCrisisME_Setup.exe` and run it.
2. Choose your disc image (Track 1 `.bin` or `.cue`).
3. Choose the folder where to install and press **Install**.

Remove the game: `Uninstall.exe` in its folder.

Without the setup window: download `DinoCrisisME_unpacked.zip` from Releases (the same files Setup carries), unpack it and run from that folder:

```
powershell -ExecutionPolicy Bypass -File installer\dcme-install.ps1 -DiscPath "D:\Dino Crisis (USA) (v1.1) (Track 1).bin" -InstallPath "C:\Games\DinoCrisisME"
```

## Features

- Over-the-shoulder camera
- Modern controls and movement behaviour
- Graphics improvements: shadows, ambient occlusion, anti-aliasing and more
- Facial animation and lip sync
- Widescreen 16:9, 21:9 and 32:9 without stretching, or the original 4:3
- Render resolution up to 4K, 30 / 60 / 120 FPS
- Many quality-of-life options
- Keyboard and mouse or a gamepad, every button can be remapped
- Mod support: a mods folder the game picks up while running, and [Dino Crisis ME Studio](../../../Dino-Crisis-Studio) to export and replace characters, rooms, textures, sounds and videos

## How it works

Nothing of the game is in this repository or in the installer. Setup reads your own disc image, recompiles the game's code to C and builds it on your PC together with the mod (a few minutes). The compiler and everything else it needs come with Setup.

That is also why there is no ready-made game to download: the built game contains code from your disc.

## Requirements

Your own Dino Crisis disc image (Track 1 `.bin` or `.cue`), one of:

| Disc | Code | |
|---|---|---|
| Dino Crisis (USA) v1.1 | SLUS-00922 | |
| Dino Crisis (USA) v1.0 | SLUS-00922 | |
| Dino Crisis (Europe) PAL, English | SLES-02207 | + `.sbi` |
| Dino Crisis (France) PAL | SLES-02208 | + `.sbi` |
| Dino Crisis (Germany) PAL | SLES-02209 | + `.sbi` |
| Dino Crisis (Italy) PAL | SLES-02210 | + `.sbi` |
| Dino Crisis (Spain) PAL | SLES-02211 | + `.sbi` |
| Dino Crisis (Japan) | SLPS-02180 | |
| Dino Crisis, Russian (VIT / Akella) | SLUS-00922 | the USA disc translated |

`.sbi` is the LibCrypt file of a PAL disc, next to the image (the same file DuckStation uses).

Windows 10 or 11, 64-bit, a graphics card with Vulkan.

## Known issues

- Windows 11 Smart App Control. If setup stops at "Recompiling the game to C", this Windows feature is probably on. It only lets through apps it already knows, and the game is built from your disc right on your PC, so to Windows it is a brand-new app. To install and play, set Smart App Control to Off (Windows Security > App & browser control > Smart App Control settings) and run setup again.

## What is in this repository

- `mod/` - everything Setup uses to build the game:
  - `src/` - the mod's runtime: renderer, camera, controls, menus, BIOS replacement, mods loader
  - `patches/` - the mod's changes to the recompiler framework
  - `generated/`, `versions/` - hooks into the recompiled game code, per disc version
  - `shaders/`, `assets/`, `config/`, `collision/` - shaders, the mod's own images and data, default settings, camera collision
  - `build/`, `project/`, `tools/` - build plans, the game project, the scripts Setup runs
- `installer/` - the setup window (`Setup.cs`), the install script (`dcme-install.ps1`), the uninstaller, the README that comes with Setup

Not here: the compiler (llvm-mingw), Python, SDL2 and the Vulkan headers. Setup and `DinoCrisisME_unpacked.zip` carry them. The recompiler framework [psxrecomp](https://github.com/mstan/psxrecomp) is downloaded by Setup at a fixed commit.

## Licence

The mod's own files: MIT (`LICENSE`). Recompiler framework and the mod's changes to it: PolyForm Noncommercial 1.0.0. Other parts: `mod/THIRD-PARTY.txt`.

Dino Crisis belongs to Capcom. This is a free fan mod, not made or endorsed by Capcom. The installed game holds code recompiled from your disc: for your own use, do not share or sell it.
