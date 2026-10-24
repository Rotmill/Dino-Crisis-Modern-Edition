DINO CRISIS - Modern Edition v1.04

Dino Crisis (PS1) for Windows: over-the-shoulder camera, widescreen,
modern graphics.


DISC

Your own Dino Crisis disc image (not included), one of:

  Dino Crisis (USA) v1.1               SLUS-00922
  Dino Crisis (USA) v1.0               SLUS-00922
  Dino Crisis (Europe) PAL, English    SLES-02207   + .sbi
  Dino Crisis (France) PAL             SLES-02208   + .sbi
  Dino Crisis (Germany) PAL            SLES-02209   + .sbi
  Dino Crisis (Italy) PAL              SLES-02210   + .sbi
  Dino Crisis (Spain) PAL              SLES-02211   + .sbi
  Dino Crisis (Japan)                  SLPS-02180
  Dino Crisis, Russian (VIT / Akella)  SLUS-00922   the USA disc translated

.sbi: the LibCrypt file of your PAL disc dump, next to the image (the same
file DuckStation uses); the game says if it is the file of another disc.
Setup checks the image and says if it is not supported.


INSTALL

  1. Download DinoCrisisME_Setup.exe and run it.
  2. Choose your disc image (Track 1 .bin or .cue).
  3. Choose the folder where to install and press Install.

  Without the setup window: download DinoCrisisME_unpacked.zip from
  Releases (the same files Setup carries), unpack it and run from that
  folder:

  powershell -ExecutionPolicy Bypass -File installer\dcme-install.ps1 -DiscPath "D:\Dino Crisis (USA) (v1.1) (Track 1).bin" -InstallPath "C:\Games\DinoCrisisME"

  Windows 10 or 11, 64-bit, a graphics card with Vulkan.


KNOWN ISSUES

  Integrated graphics. At the first start the game turns its graphics
  down once (Anti-Aliasing, Materials, Shadows, Lamp Haze off). You can
  turn them back on in Modern Options > Graphics.

  Windows 11 Smart App Control. If setup stops at "Recompiling the game
  to C", this Windows feature is probably on. It only lets through apps
  it already knows, and the game is built from your disc right on your
  PC, so to Windows it is a brand-new app. To install and play, set
  Smart App Control to Off (Windows Security > App & browser control >
  Smart App Control settings) and run setup again.


PROBLEMS

  Send the log to the developer (Discord link in the main menu, or
  github.com/Rotmill/Dino-Crisis-Modern-Edition/issues):
  DinoCrisisME_log.txt next to DinoCrisisME.exe, or install-log.txt in
  the install folder if setup stopped.


Quicksave: F10 save, Del load, PageUp / PageDown slot (50 slots, data\quicksaves).
Remove the game: Uninstall.exe in its folder (the saves and mods there go too).
Mods: the mods folder next to DinoCrisisME.exe, made with Dino Crisis ME
Studio (separate download). A downloaded mod: its folder into mods.

Dino Crisis belongs to Capcom. This is a fan mod, not made or endorsed by
Capcom. Mod: MIT (LICENSE.txt). Recompiler framework: PolyForm
Noncommercial, noncommercial use only. Other parts: THIRD-PARTY.txt.
The installed game holds code recompiled from your disc: for your own use,
do not share or sell it.
