# swat3_qol

Mods for SWAT 3 (1999, GOG Elite Edition) by Marvin (not a coder: explain things plainly, no jargon).

## Parts
- `launcher/` - SWAT3Launcher.exe: settings for the renderer / QoL / ragdoll ini files, mod manager
  (game's own Mods folder + `Mods=` line in swat.ini), Play button. Win32, single file.
- `renderer/` - ddraw.dll (Direct3D 11 renderer, base name swat3d11, ini swat3d11.ini) with the QoL
  features built in (command menu, nametags, quick restrain, wall blood, game fixes). **Source lost in a
  workspace reset on 2026-11-18; needs rewriting.** Last released build: 0.13.1.
- `ragdoll/` - dsound.dll (swat3ragdoll.ini). **Source lost in the same reset; needs rewriting.**
  Last released build: 0.4.
- `notes/` - reverse-engineering notes about swat.exe.

## Rules
- Commit and push after every meaningful change, so a workspace reset never loses work again.
- Never commit game files (swat.exe, swat_original.exe, .gsa/.gsm/.res/.dat assets) or build outputs.
- Tests run under Wine are simulations, not the real game: always say so to Marvin.
- Marvin runs the ORIGINAL swat.exe (named swat_original.exe) with Last Resort mod data in the folder.

## Build
Cross-compiled with llvm-mingw (i686 for the game DLLs, which must be 32-bit; x86_64 for the launcher).
Launcher: windres launcher.rc -> res, then clang++ -O2 -mwindows launcher.cpp + res
-lcomctl32 -lshell32 -luser32 -lgdi32 (add -DLAUNCHER_TEST for the -tab/-flip/-save test switches).
