# SWAT 3 internals used by the mods

Addresses are for the GOG 1.x `swat.exe`. The Last Resort exe (`swat.exe` in Marvin's folder at one
point) and the original exe (`swat_original.exe`, what Marvin plays) are the same build: 86 bytes differ,
none at any hook site below. Image base 0x400000, MSVC 6, no packer. See also `renderer-re-notes.md`.

## Last Resort exe vs original exe (the 86 bytes)
| Address | Original | Last Resort |
|---|---|---|
| 0x403491 (menu / fallback resolution) | 800x600 | 1920x1080 |
| 0x522ece (debrief SetMode) | 800x600 | 1920x1080 |
| 0x4064f5 (far plane push) | 1400.0 | 2800.0 |
| 0x565f80, 0x565f88, 0x566276, 0x56627e (texture limit) | 0x100 | 0x400 |
| 0x5844b1, 0x5844c2 (texture limit, imm16) | 0x100 | 0x400 |
| strings | `menu.dat`, master servers sierra.com | `m3nu.dat`, nuwon.net |
The texture-size error message's "256" is a separate push at 0x565f96 and 0x56628b (LR left it at 256).

## Engine / display
- engine = `*(0x6c0ddc)`; globals = `0x6cad10` (`*(0x6b0448)`); renderer = `*(globals+0xc)`.
- SetMode = `FUN_00407c30`, thiscall(engine, w, h, is3D, flag); its vtable slot is at `0x65d4c8`.
- Fallback / menu resolution globals `0x6c0d68` / `0x6c0d6c`. Mission resolution = engine[0x14d], [0x14e]
  (swat.cfg `res=`).
- Mode list: engine+0x3d0, count engine+0x3d4; entries 0x34 bytes ([0] w, [1] h, [2] bpp, [3] 3D flag,
  [4] render type). Enum callback `FUN_0055cfa0` keeps only 640x480, 800x600, 1024x768, 1280x1024,
  1600x1200 (16-bit, <= 2048). Patch at 0x55cff1: `81 f9 80 02 00 00 75 07` -> `eb 51` lets every mode in.
- swat.cfg `display_mode=W H type flag`.
- `-nointro` clears the flag at 0x69525c (string "-nointro" at 0x695d70); writing 0 there skips the intro.
- Mirrors are drawn only at detail level 2 (engine+0x394).
- swat.ini: section name is at globals+0x74 (a runtime string); key `Mods` (string at 0x69975c) lists
  active mods, comma separated; mods live in the `Mods\` folder (folders, .res, .zip; `modinfo.dat`).
  `Paths` key in the same section.

## How the game draws (Direct3D 6)
- DirectX 6: IDirectDraw4, IDirectDrawSurface4, IDirect3D3, IDirect3DDevice3, IDirect3DViewport3,
  IDirect3DTexture2. ddraw.dll imports: DirectDrawCreate, DirectDrawEnumerateA.
- Usual state: stage0 colorop MODULATE (TEXTURE, DIFFUSE), alphaop MODULATE (TEXTURE, DIFFUSE); stage1
  alphaop MODULATE (TEXTURE, CURRENT); alpha test on, func GREATEREQUAL, ref 4; fog on, mode 0; each frame
  clears only Z (colour is not cleared).
- Some mirror geometry has z > 1 and negative rhw: the renderer must use w = 1/abs(rhw) and turn depth
  clipping off.
- Mirrors: reflected room drawn as far geometry clipped to the mirror outline, then an untextured glass
  quad (colour 0x08ff0000) with z-write, then the wall (rejected by depth), then the frame (4444 texture,
  transparent centre).
- Untextured stage rule: a missing texture reads as opaque white. Only when alphaop is SELECTARG1/2
  selecting TEXTURE is it replaced by DIFFUSE. (Replacing every TEXTURE arg squares the glass alpha
  8 -> below the alpha-test ref 4 -> no depth write -> wall drawn over the reflection: the framed-mirror bug.)

## Characters and entities
- Entity table: single player / host: session = engine+0x4d4, count +0x809c, wrappers +0x409c[i];
  multiplayer client: session = engine+0x4d8, count +0x8040, wrappers +0x4040[i].
- Wrapper: +0x12 char[32] name (officers spawn with their roster name), +0x34 game object.
- Class test `FUN_00434780(obj, "Char")` (thiscall).
- Character: +0x1c/+0x20/+0x24 position (y up, units ~inches), +0x278 active, +0x2dc body model object,
  +0x9d4 eye height, +0xb08 role (1-2, 6-7 red; 3-4, 8-9 blue), +0xb30 kind (1 SWAT officer, 3 suspect),
  +0xdf0 condition: 0 standing/crouching, 1 complying, 2 secured, 3 down, 4 dead, 5 hiding.
- Projection (as the HUD target markers, FUN_00493080): view matrix = *(renderer+0x7c) + 0x2c (3x4,
  FUN_0046a8b0 layout: cam = p.x*m[0..2] + p.y*m[3..5] + p.z*m[6..8] + m[9..11]);
  sx = x*r[0x4c]/z + r[0x44], sy = r[0x48] - y*r[0x50]/z.
- Secure (restrain) `0x5e5580`; notify `0x42f7e0`.
- World trace `FUN_0046f8b0` cdecl(result, from, to, ignore, mask, 1): result +0 hit point, +0x10
  distance (1e8 = nothing), +0x1c normal, +0x28 entity hit. Masks used: 0x10088 (line of sight),
  0x21009a (ragdoll vs level).
- Decal spawn `FUN_00517120("decal", pos, into, "sprite=splatter_sm, depth=24")`.
- Bullet hit handler `FUN_0061cbb0`: thiscall(bullet record in ecx: +0 from xyz, +0xc to xyz), stack:
  character, flag, wound offset x, y, z (relative to the character position). Entry bytes `6a ff 68 ..`
  (push -1 / push imm). Blood hook was at the call 0x61c74f.

## HUD / input hooks (QoL)
- HUD element draw hook 0x4999da; FOV 0x55aca7; speech mute 0x603320.
- Command menu list builder `FUN_0048a740`: slots in table order of first appearance per button, last
  valid row wins, trailing unavailable buttons trimmed. Dynamic "Stack up": swap contents (bytes 6..0x3f)
  of the interior Stack row and the last team-menu row (Optiwand, button 9) in dynamic mode, set the
  Stack row mode 2 / loc 3, the displaced Optiwand row mode 1 and its +0x30 flag 1; restore in stealth.
  (Moving Stack rows to button 9 instead made the game trim Restrain - bug fixed in 0.11.2.)
- The command menu object is freed at mission end: anything drawn from a render callback must check that
  the game drew its menu this frame (the `g_fresh` flag), or the debrief crashes (fixed in 0.12.4).
- Quick restrain: player = nearest officer within 150 units of the camera (kept between frames);
  candidate = complying non-officer within Reach whose body line (knees to head) passes within 14 units
  of the crosshair axis, plus line of sight via the trace (mask 0x10088). Direct restrain needs no cuffs.

## Skeleton / animation (ragdoll)
- 3ds Max Biped skeleton, 37 bones: 0 Pelvis, 1 Spine, 2 Spine1, 3 Spine2, 4 Neck, 5 Head,
  6-8 Ponytail1/11/12, 9 L Clavicle, 10 L UpperArm, 11 L Forearm, 12 L Hand, 13-18 L fingers,
  19 R Clavicle, 20 R UpperArm, 21 R Forearm, 22 R Hand, 23-28 R fingers, 29 L Thigh, 30 L Calf,
  31 L Foot, 32 L Toe0, 33 R Thigh, 34 R Calf, 35 R Foot, 36 R Toe0. Parent indices in the bone record.
  Local offsets (from the log): pelvis (0.7, 38.4, -1.1); spine segments 2.8 / 5.0 / 6.2; neck 8.1;
  head 4.2; clavicle +-1.5 z; upper arm 5.7; forearm 12.5; hand 10.8; thigh +-3.6 z; calf 19.1;
  foot 15.2; toe (4.5, 3.4).
- Animations `.GSA` (version 0x401/0x402, 0x2c-byte key records), models `.GSM`.
- Body model wrapper (vtable 0x65f91c, ctor FUN_00474a00): +0x20 skeleton handle, +0x2c model id (-1 none).
- Skeleton = `*( *(engine+0x1b0) + handle*4 )` (handle & 0xffff): +0x50 bones, +0x54 bone count,
  +0x58 root matrix, +0x88 "changed" flag.
- Bone record 0x94 bytes: +0x00 name, +0x14 parent (ushort, 0xffff none), +0x16 body region,
  +0x18 world matrix, +0x48 local matrix. Matrices are 12 floats: 3x3 rotation with the axes as ROWS,
  then translation; p' = p * M + t. Bone x axis points along the bone.
- `FUN_00573bb0` thiscall(skeleton): world = local * parent world; pelvis: local * B * root (B mirrors x).
- Skin instance: +0x78 skeleton, +0xb4 "settled" flag (mesh not updated when set).
- `FUN_005744c0` thiscall(skin instance, force): rebuilds world matrices if changed, moves the mesh.
  Entry bytes `83 ec 34 56 8b 74 24 3c`. Hooked at entry to override a dead body's pose.
- Knees and elbows bend towards -y of the upper bone (seen in the game's poses).

## Notes from testing in the real game
- Ragdoll: wounded suspects lie still and may die later; physics must not start on a body that is
  already lying still (unless shot). Bodies froze in propped poses with sticky friction; joint limits
  (hips, shoulders, neck, waist) were added in 0.4.
