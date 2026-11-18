SWAT 3 ragdoll experiment - version 0.4
===========================================
A moment after a character dies, the death animation is replaced by physics: the body collapses
under gravity and is stopped by floors, walls and furniture. Living characters are not touched.

This is a first version. Expect rough edges: odd poses, hands or feet poking into things.

Install
  Copy dsound.dll and swat3ragdoll.ini into the SWAT 3 folder (next to the game exe and ddraw.dll),
  replacing the files from the earlier tests.

Test
  Shoot suspects in different spots: open floor, next to a wall, on stairs, near furniture.
  Then send back swat3ragdoll.log from the game folder and say what looked wrong.

New in 0.4: real joint limits. Hips, shoulders, neck and waist only move as far as a person's
can, so no more splits, folded necks or twisted torsos.

Settings are in swat3ragdoll.ini (delay before physics starts, gravity, friction, bullet push).

Remove
  Delete dsound.dll and swat3ragdoll.ini from the game folder.
