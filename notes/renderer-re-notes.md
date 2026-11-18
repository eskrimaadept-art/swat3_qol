# SWAT 3 (GOG, Elite Edition) — renderer reverse-engineering notes

Binary: `swat.exe`, SHA-1 `6399fd12223ceb90d13a671e027c1fd28faab686`, MSVC 6, no packer/DRM, image base 0x400000.
DirectX version: **DirectX 6** (IDirectDraw4, IDirectDrawSurface4, IDirect3D3, IDirect3DDevice3, IDirect3DViewport3, IDirect3DTexture2).
`ddraw.dll` imports: `DirectDrawCreate`, `DirectDrawEnumerateA` only.

## Global state

| Address / offset | Meaning |
|---|---|
| `0x6b0448` | `g_pGlobals` → `0x6cad10` |
| globals+0x00 | `Engine*` (main engine object) |
| globals+0x0c | `Renderer*` (abstract renderer, see below) |
| globals+0x14..0x48 | screen / texture pixel-format masks and shifts (16-bit) |
| globals+0x6c / +0x70 | screen width / height (default 640×480 at `0x55789b`) |
| engine+0x204..0x210 | window/screen rect |
| engine+0x244 | render type: 1 secondary adapter, 2 primary adapter, 3 software |
| engine+0x41c | IDirectDraw4 |
| engine+0x420 | IDirect3D3 |
| engine+0x424 | primary (front) surface; wrapped as sprite `_PrimarySprite_` (engine+0x1f8) |
| engine+0x428 | back buffer / render target; wrapped as sprite `_ScreenSprite_` (engine+0x1f4) |
| engine+0x42c | z-buffer surface |
| engine+0x430 | clipper |
| engine+0x434 | IDirect3DViewport3 |
| engine+0x438 | IDirect3DDevice3 |
| engine+0x464 | texture pixel format (DDPIXELFORMAT, 16-bit) |

## Key functions

| Address | Role |
|---|---|
| `0x55b6a0` | DirectDrawCreate + QI IDirectDraw4, picks HW/SW renderer |
| `0x55bc80` | SetCooperativeLevel / SetDisplayMode |
| `0x55bd80` | create primary, back buffer (flip chain or offscreen), clipper |
| `0x55c350` | QI IDirect3D3, z-buffer, CreateDevice, viewport, initial render states, creates Renderer |
| `0x55cb70` | initial texture stage states |
| `0x55d370` | z-buffer creation (EnumZBufferFormats) |
| `0x566b30` | clear back buffer (Blt COLORFILL) |
| `0x566c40` | **present**: windowed = primary.Blt(back); fullscreen = BltFast / Flip (`0x566cf4`) |
| `0x556550`, `0x55ba20` | shutdown / release |
| `0x5570d0` | lost-surface restore |
| `0x49a940` | HUD viewport-frame layout (chooses 640 vs 1024 art by screen width ≤ 800) |

## Renderer interface (globals+0x0c)

Hardware implementation vtable `0x665b40` (ctor `0x57a3e0`, object size 0x11fefc); software vtable `0x665b78` (ctor `0x57ab10`).

| Slot | Offset | HW impl | Meaning |
|---|---|---|---|
| 0 | +0x00 | `0x57ad30` | scalar deleting destructor |
| 1 | +0x04 | `0x57a470` | begin frame: BeginScene + default states |
| 2 | +0x08 | `0x57a610` | end frame: SetMode(1) + EndScene |
| 3 | +0x0c | `0x57aa30` → `0x57a650` | SetMode(n), table `0x6b1a50` (7 dwords/mode) |
| 4 | +0x10 | `0x57aa50` | SetTexture(image, stage) → device.SetTexture(stage, image+0x18) |
| 5 | +0x14 | `0x57aaa0` | DrawPrimitive passthrough |
| 6 | +0x18 | `0x57aad0` | DrawIndexedPrimitive passthrough |
| 7–9 | | HW only | destructor / internals |

Mode table fields: SRCBLEND, DESTBLEND, SPECULARENABLE, ZWRITEENABLE, ZFUNC, ALPHABLENDENABLE, stage-0 ADDRESS.
Mode 5 = SRCALPHA/INVSRCALPHA, no z-write, ZFUNC ALWAYS, alpha blend, clamp → **screen-space overlay mode**.
Vertices: FVF `0x1c4` (D3DTLVERTEX, pre-transformed screen coordinates).

## 2D / sprite system

Sprites are DirectDraw surfaces (image class vtable `0x665d88`, ctor `0x586100`): surface at +0x04, IDirect3DTexture2 at +0x18.

| Entry point | Implementation | Operation |
|---|---|---|
| `0x55f410` | `0x5684e0` → `0x56b1f0` | blit sprite at (x, y) — Blt, WAIT [+KEYSRC] |
| `0x55f440` | `0x568540` → `0x56b3c0` | stretch blit rect → rect |
| `0x55f6d0` | `0x5687f0` → `0x586d70` | tinted fill / fade |
| `0x55f5e0` | `0x56b470` | GDI text (GetDC + TextOutA, 10 px font) |
| `0x56b160` / `0x56b1c0` | | Lock / Unlock |

Colour key: magenta `0xf800f8` (RGB565).
Textures: created at `0x583fe0` / `0x585020` (DDSCAPS_TEXTURE, managed), converted on the CPU to 16-bit (`0x584200` 24-bit, `0x584320` 8-bit palettized, `0x584530` 32-bit).

Layout data: menus `M3nu.dat`/`MENU.DAT` (986 screens, absolute 800×600 coordinates); HUD `UIOfficer*.dat` / `UIDeathmatch.dat` (`Indicators` = anchor 0–9 + offset).

## DirectX methods the engine calls (static analysis)

IDirectDraw4: CreateSurface, CreateClipper, EnumDisplayModes, FlipToGDISurface, GetAvailableVidMem, GetCaps, GetDisplayMode, QueryInterface, RestoreAllSurfaces, RestoreDisplayMode, SetCooperativeLevel, SetDisplayMode, Release.
IDirectDrawSurface4: Blt (COLORFILL, KEYSRC, stretch), BltFast, Flip, Lock, Unlock, GetDC, ReleaseDC, GetSurfaceDesc, GetAttachedSurface, AddAttachedSurface, SetClipper, SetColorKey, GetColorKey, IsLost, Restore, QueryInterface (→ IDirect3DTexture2), Release.
IDirect3D3: CreateDevice, CreateViewport, EnumZBufferFormats, EvictManagedTextures, Release.
IDirect3DDevice3: BeginScene, EndScene, DrawPrimitive, DrawIndexedPrimitive, SetRenderState, SetTextureStageState, SetTexture, GetCaps, EnumTextureFormats, AddViewport, SetCurrentViewport, Release.
IDirect3DViewport3: SetViewport2, Clear2, Release.
IDirectDrawClipper: SetHWnd, Release.

Tools: `scripts/` (Ghidra Jython: StringXrefs, DumpRange, TraceCOM, ApplyDXTypes, FixFunctions; Python: classify, vtables, guids). Full decompile: `out/all.c`; traced virtual calls: `out/comcalls.txt`.
