SWAT 3 Direct3D 11 renderer - test build 0.13.1
===============================================

What it is
----------
A replacement ddraw.dll made just for SWAT 3. It runs the game's graphics
on DirectX 11 instead of the old DirectX 6, without dgVoodoo.

 - The 3D renders at your monitor's full resolution.
 - Menus are scaled up to fit your monitor (they keep their 4:3 shape
   with black bars at the sides).
 - Textures in full 32-bit colour, with optional anisotropic filtering
   and anti-aliasing (see swat3d11.ini).
 - Includes the right-click command menu, quick restrain, nametags, wall
   blood and the FieldOfView setting.

This is the first DirectX 11 build. It has only been tested with test
programs, not with the real game - please report what you see.

New in 0.13.1
-------------
 - Fixed the framed mirror that showed the wall instead of a reflection.
   The mirror's glass is an almost invisible sheet (3% opaque) whose only
   job is to stop the wall behind it from being drawn over the
   reflection. The renderer made untextured see-through surfaces even
   more see-through than the game asked for, the game's own "too faint
   to draw" rule then threw the glass away, and the wall won.
 - Side effect of the same fix: other untextured see-through surfaces
   (tinted glass, some shadows and effects) are drawn at the strength
   the game intends. They may look a little stronger than before.

New in 0.13.0
-------------
 - The add-on features are now built into the renderer:
     * quick restrain: look at someone who has complied, up close, and
       press your interact key ("[F] Restrain" prompt)
     * officer nametags
     * wall blood splatter
     * the team name shown briefly when you switch teams
   Settings: [QuickRestrain], [Nametags], [Blood] in swat3d11.ini.
   (Raw mouse input is not included yet.)
 - The right-click command menu is 30% smaller (MenuSize in the ini).
 - F11 frame recording now also records the game's current settings.

New in 0.12.4
-------------
 - Fixed the crash when a mission ends and the debrief screen should
   appear (the command menu code kept using the game's menu after the
   game had removed it).

New in 0.12.3
-------------
 - Resolution is now chosen IN THE GAME: the video options list
   widescreen sizes (1920x1080, your monitor's size and others), not just
   the original five 4:3 sizes. Missions use what you pick there.
   (MissionResolution=desktop in swat3d11.ini forces your monitor's size
   instead, as 0.12.1/0.12.2 did.)
 - SkipIntro=1 in swat3d11.ini skips the intro videos.

New in 0.12.2
-------------
 - Texture size limit raised to 2048 by default (MaxTextureSize in
   swat3d11.ini; up to 4096).
 - The "Texture ... is an invalid size" message now shows the real limit
   (the game always said 256, even in the Last Resort exe).

New in 0.12.1: made for the ORIGINAL swat.exe
--------------------------------------------
The Last Resort swat.exe is the original exe with a few changes built in.
This build applies the same changes to the original exe while the game
runs (nothing on disk is modified), so you no longer need Last Resort's
exe or launcher:
 - widescreen mission resolutions
 - larger textures are accepted (original limit: 256)
 - draw distance doubled, as in Last Resort
Each can be changed or switched off in swat3d11.ini, section [Game].
Start the game with the original swat.exe.

New in 0.12.0
-------------
 - Drawing moved from Direct3D 9 to Direct3D 11.
 - Mirrors: the game draws mirror reflections in a way modern Direct3D 9
   throws away (slightly beyond the far depth limit). This build draws
   them the way 1999 graphics cards did. Please check a mirror.

Not in this build yet
---------------------
Raw mouse input. Everything else from the dgVoodoo add-on (its dsound.dll)
is built in now - do not use that add-on together with this renderer.

INSTALL
-------
1. Open your SWAT 3 folder (the one with swat.exe in it).

2. Make a new folder there called "dgVoodoo backup" and MOVE these files
   into it, if they are there:
       ddraw.dll   D3DImm.dll   dgVoodoo.conf   dgVoodooCpl.exe
       dsound.dll  swat3qol.ini        (the QoL add-on)
   (Keep them - this is how you go back.)

3. Copy ddraw.dll and swat3d11.ini from this package into the SWAT 3
   folder.

4. Start the game with the original swat.exe, open its video options
   and pick your resolution (for example 1920 x 1080).

Needs Windows 10 or 11 and a graphics card from roughly 2008 or later.

IF SOMETHING GOES WRONG
-----------------------
A file called swat3d11.log appears next to swat.exe every time you play.
Please send it back with a screenshot or a description.

Things worth checking:
 - does the game start, and do the menus look right
 - mirrors (do they reflect now?)
 - walls, lighting, glass, smoke, muzzle flashes, blood, the HUD
 - the right-click command menu
 - alt-tab out and back in
 - performance (smooth or stuttering)

If the picture is black or badly wrong, open swat3d11.ini, set
BuiltInShaders=1 and try again - and tell me whether that changed it.

UNINSTALL
---------
Delete ddraw.dll and swat3d11.ini from the SWAT 3 folder, then move the
files from "dgVoodoo backup" back into it.
