SWAT 3 Direct3D 11 renderer 0.14.5 - REBUILD TEST (stage 1)
===========================================================
This is the renderer rebuilt from scratch after its source code was lost. It should look and
behave like 0.13.1, but it is new code, so it needs testing.

Stage 1 contains the renderer itself and the game adjustments:
  - Direct3D 11 picture, fullscreen / window, resolution scaling, anti-aliasing, anisotropic
    filtering, mipmaps, VSync
  - field of view
  - in-game resolution list unlocked, forced mission resolution, intro skip,
    texture size limit, draw distance
  - mouse translation, F11 frame trace, crash report in the log

NOT in this build yet (stage 2 brings them back): right-click command menu, team selection,
nametags, quick restrain, wall blood. Their settings in swat3d11.ini are kept but do nothing.
For normal play, keep using 0.13.1 until stage 2 is ready.

Install
  1. Back up your current ddraw.dll and swat3d11.ini (0.13.1).
  2. Copy this ddraw.dll and swat3d11.ini into the SWAT 3 folder.
  3. To go back, restore your backed-up files.

What to test
  Start the game, go through the menus, play part of a mission, finish it (debrief), quit.
  Check: menus and mouse, mission picture, mirrors, HUD, text, resolution changes.
  Then send swat3d11.log from the game folder - whatever happened. If something looks wrong,
  press F11 at that moment and also send swat3d11_trace.txt.
