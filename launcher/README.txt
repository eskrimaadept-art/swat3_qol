SWAT 3 Launcher 1.0.1
===================
One window for the settings of the Direct3D 11 renderer / QoL features, the ragdoll add-on and
the game's mods, with a Play button.

Install
  Copy SWAT3Launcher.exe into the SWAT 3 folder (next to swat.exe and ddraw.dll) and start the
  game with it. Make a desktop shortcut to it if you like.

Tabs
  Display         fullscreen, resolution in missions, anti-aliasing, field of view, intro skip...
  Gameplay (QoL)  command menu, team switching, nametags, quick restrain, wall blood
  Mods            tick the mods the game should load (see below)
  Ragdoll         on/off and feel of the ragdoll add-on (only if it is installed)

  "Play" saves and starts the game. "Save" only saves. Pick which game program to start at the
  bottom left if there is more than one (for example swat.exe and swat_original.exe).

Mods
  The game has its own mod system: every folder, .res or .zip file in its "Mods" folder is a
  mod, and swat.ini lists the active ones. The launcher shows them all with a tick box; the
  order (Move up / Move down) is the order the game loads them in.
  Mods that were installed by replacing the game's own files (Last Resort, for example) are not
  mods in this sense and cannot be switched off here.

What it changes
  swat3d11.ini, swat3ragdoll.ini, the "Mods=" line of swat.ini, and its own swat3launcher.ini.
  Only the lines for settings you changed are rewritten; everything else is left as it is.
