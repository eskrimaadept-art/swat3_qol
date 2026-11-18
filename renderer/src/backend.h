// Backend: draws what the DirectDraw / Direct3D 6 front end receives, with Direct3D 11.
// All coordinates the front end passes are in the game's own screen pixels ("virtual" resolution,
// e.g. 800x600 menus or 1920x1080 missions). The backend renders at the output's resolution and
// scales; the game never notices.
#pragma once
#include "common.h"
#include <d3d.h>

namespace be {

struct Tex;                                     // a GPU texture (BGRA8)

bool Init(HWND hwnd);                           // create device / swap chain (once)
bool Ready();
HWND Window();
void Shutdown();

void SetVirtualMode(int w, int h);              // the game's resolution changed
void GetVirtualMode(int* w, int* h);
void GetOutputSize(int* w, int* h);
struct Mapping { double scale, offX, offY; };   // output pixel = off + virtual * scale
Mapping GetMapping();

// textures
Tex* TexCreate(int w, int h, bool mipmaps);
void TexUpload(Tex* t, const uint32_t* argb, int pitchBytes);   // whole texture, 0xAARRGGBB
void TexDestroy(Tex* t);

// Direct3D 6 state
void SetRenderState6(uint32_t state, uint32_t value);
void SetTextureStageState6(uint32_t stage, uint32_t type, uint32_t value);
void SetTexture(uint32_t stage, Tex* t, bool keyed);           // keyed: texture uses its colour key
void SetViewport(int x, int y, int w, int h, float minZ, float maxZ);
void Clear(uint32_t count, const D3DRECT* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil);
void BeginScene();
void EndScene();
void DrawPrimitiveTL(uint32_t prim, uint32_t fvf, const void* verts, uint32_t count);
void DrawIndexedPrimitiveTL(uint32_t prim, uint32_t fvf, const void* verts, uint32_t nverts, const uint16_t* idx, uint32_t nidx);

// 2D (DirectDraw blits onto the game's back buffer)
void Blit(Tex* t, int texW, int texH, const RECT& src, const RECT& dst, bool blend);   // blend: alpha from texture
void Fill(const RECT* r, uint32_t argb);                        // nullptr = whole screen
bool ReadBack(const RECT& r, uint32_t* out, int pitchPixels);   // game pixels, 0xAARRGGBB
void Present();

// overlay drawn over the finished picture at output resolution (QoL features)
void OverlayClear();
void OverlayAdd(Tex* t, int x, int y, int w, int h);
void SetPresentCallback(void (*fn)());
void TraceState();

}  // namespace be
