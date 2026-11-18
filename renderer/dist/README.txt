SWAT 3 Direct3D 11 renderer 0.15.0 - REBUILD TEST (stage 2)
============================================================
The rebuilt renderer (stage 1, tested in 0.14.9) plus the quality-of-life features, rewritten
from scratch after their source code was lost:
  - SWAT 4-style command menu (right mouse button) with team selection (mouse button 4 cycles
    Element / Red / Blue), "All commands", context reports in green, Stack Up in dynamic mode
  - team name shown briefly in the middle of the screen when you switch team
  - nametags above officers (red / blue team colours)
  - quick restrain: look at someone with their hands up, get close, "[key]  Restrain" appears,
    press your interact key. They are restrained on the spot.
Wall blood is gone for good. Quick restrain only does the on-the-spot version now (the old
"Direct=0" mode that took the cuffs out is gone).

All settings are in swat3d11.ini ([Controls], [Nametags], [QuickRestrain]).

Install
  1. Back up your current ddraw.dll and swat3d11.ini.
  2. Copy this ddraw.dll and swat3d11.ini into the SWAT 3 folder.
  3. To go back, restore your backed-up files.

What to test
  In a mission:
  - right-click: the menu opens at the selected team's orders. Move the mouse up/down or use the
    wheel to pick, left-click to give the order. "All commands" goes to the full menu.
  - mouse button 4: team switch (the team name appears). Right-click again: that team's orders.
  - aim at a suspect who shows a report (e.g. down, secured): it should be listed in green.
  - dynamic mode: Stack Up should be on 9 in the team menu.
  - nametags over your officers.
  - quick restrain on someone with their hands up.
  Then send swat3d11.log from the game folder - whatever happened. If something looks wrong,
  press F11 at that moment and also send swat3d11_trace.txt.
