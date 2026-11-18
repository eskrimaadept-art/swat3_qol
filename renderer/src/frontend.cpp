// Front end: DirectDraw and Direct3D 6 objects as the game sees them. Surfaces keep their
// pixels in memory (16-bit as in 1999); textures and sprites are converted and uploaded to the
// backend when used. The game's screen (primary surface / back buffer) is the backend's render
// target: blits onto it become textured quads, presents go to the backend.
#define INITGUID
#include "frontend.h"
#include "stubs.h"
#include "backend.h"
#include "hooks.h"
#include "gamefix.h"
#include <stdio.h>
#include <stddef.h>

extern "C" const GUID IID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};

// ====================================================================================================
// Pixel formats
// ====================================================================================================
struct Fmt {
    int bpp; uint32_t r, g, b, a;
    bool operator==(const Fmt& o) const { return bpp == o.bpp && r == o.r && g == o.g && b == o.b && a == o.a; }
};
static const Fmt kFmt565 = {16, 0xf800, 0x07e0, 0x001f, 0};

static Fmt FmtFromPF(const DDPIXELFORMAT& pf) {
    Fmt f = kFmt565;
    if (!(pf.dwFlags & DDPF_RGB)) return f;
    f.bpp = (int)pf.dwRGBBitCount;
    f.r = pf.dwRBitMask; f.g = pf.dwGBitMask; f.b = pf.dwBBitMask;
    f.a = (pf.dwFlags & DDPF_ALPHAPIXELS) ? pf.dwRGBAlphaBitMask : 0;
    if (f.bpp != 16 && f.bpp != 32 && f.bpp != 24) { LogOnce("8bitfmt", "note: %d-bit surface requested (unsupported, using 16-bit)", f.bpp); f = kFmt565; }
    return f;
}
static void PFFromFmt(const Fmt& f, DDPIXELFORMAT& pf) {
    memset(&pf, 0, sizeof(pf));
    pf.dwSize = sizeof(pf);
    pf.dwFlags = DDPF_RGB | (f.a ? DDPF_ALPHAPIXELS : 0);
    pf.dwRGBBitCount = f.bpp;
    pf.dwRBitMask = f.r; pf.dwGBitMask = f.g; pf.dwBBitMask = f.b; pf.dwRGBAlphaBitMask = f.a;
}

// Channel conversion between a format and 0xAARRGGBB
struct Chan { int shift, bits; };
static Chan ChanOf(uint32_t m) {
    Chan c = {0, 0};
    if (!m) return c;
    while (!(m & 1)) { m >>= 1; c.shift++; }
    while (m & 1) { m >>= 1; c.bits++; }
    return c;
}
static inline uint32_t Expand(uint32_t v, int bits) {
    if (bits >= 8) return v >> (bits - 8);
    if (bits == 0) return 255;
    uint32_t r = v << (8 - bits);
    for (int b = bits; b < 8; b += bits) r |= r >> bits;   // replicate high bits
    return r & 255;
}
struct Conv {
    Fmt f; Chan r, g, b, a;
    uint32_t* lut = nullptr;                      // 16-bit formats: pixel -> ARGB
    void Init(const Fmt& fmt) {
        f = fmt; r = ChanOf(f.r); g = ChanOf(f.g); b = ChanOf(f.b); a = ChanOf(f.a);
        if (f.bpp == 16) {
            lut = (uint32_t*)malloc(65536 * 4);
            for (uint32_t p = 0; p < 65536; p++) lut[p] = Slow(p);
        }
    }
    uint32_t Slow(uint32_t p) const {
        uint32_t R = Expand((p & f.r) >> r.shift, r.bits), G = Expand((p & f.g) >> g.shift, g.bits), B = Expand((p & f.b) >> b.shift, b.bits);
        uint32_t A = f.a ? Expand((p & f.a) >> a.shift, a.bits) : 255;
        return (A << 24) | (R << 16) | (G << 8) | B;
    }
    uint32_t ToARGB(uint32_t p) const { return lut ? lut[p & 0xffff] : Slow(p); }
    uint32_t FromARGB(uint32_t c) const {
        auto put = [](uint32_t v8, const Chan& ch) -> uint32_t { if (!ch.bits) return 0; return (ch.bits >= 8 ? v8 << (ch.bits - 8) : v8 >> (8 - ch.bits)) << ch.shift; };
        return put((c >> 16) & 255, r) | put((c >> 8) & 255, g) | put(c & 255, b) | put(c >> 24, a);
    }
};
static Vec<Conv*> g_convs;
static const Conv& ConvFor(const Fmt& f) {
    for (Conv* c : g_convs) if (c->f == f) return *c;
    Conv* c = new Conv(); c->Init(f); g_convs.push(c);
    return *c;
}
static inline uint32_t ReadPx(const uint8_t* p, int bpp) {
    return bpp == 16 ? *(const uint16_t*)p : bpp == 32 ? *(const uint32_t*)p : (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16));
}
static inline void WritePx(uint8_t* p, int bpp, uint32_t v) {
    if (bpp == 16) *(uint16_t*)p = (uint16_t)v;
    else if (bpp == 32) *(uint32_t*)p = v;
    else { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); }
}

// ====================================================================================================
// Objects
// ====================================================================================================
struct DDraw; struct Surface; struct Device; struct Viewport; struct D3D; struct Clipper;
enum Kind { K_PRIMARY, K_BACK, K_ZBUF, K_TEX, K_PLAIN };
static const char* KindName(Kind k) { static const char* n[] = {"primary", "back buffer", "z-buffer", "texture", "plain"}; return n[k]; }

static DDraw* g_dd;
static struct { int frames, draws, uploads, getdc, locks, reads; } g_stats;
static const uint32_t kDcBlank = 0x010203;
static int g_lockLogged = 0;    // "nothing drawn here" colour for GDI text on the screen
static Surface* g_back;        // the game's 3D render surface (the backend's render target)
static Surface* g_primary;
static Device* g_device;

#define FWD_REF(core) \
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** o) override { return core->QueryInterface(r, o); } \
    ULONG STDMETHODCALLTYPE AddRef() override { return core->AddRef(); } \
    ULONG STDMETHODCALLTYPE Release() override { return core->Release(); }

static void Desc1To2(const DDSURFACEDESC* a, DDSURFACEDESC2* b) {
    memset(b, 0, sizeof(*b));
    memcpy(b, a, offsetof(DDSURFACEDESC, ddsCaps));
    b->dwSize = sizeof(*b);
    b->ddsCaps.dwCaps = a->ddsCaps.dwCaps;
}
static void Desc2To1(const DDSURFACEDESC2* b, DDSURFACEDESC* a) {
    DWORD size = a->dwSize ? a->dwSize : sizeof(*a);
    memcpy(a, b, offsetof(DDSURFACEDESC, ddsCaps));
    a->dwSize = size;
    a->ddsCaps.dwCaps = b->ddsCaps.dwCaps;
}

// Transparent (key) texels keep the key colour (bright pink) in their RGB. When the picture is
// scaled smoothly, edge pixels mix in a little of it: a pink outline. Give each transparent
// texel the colour of its visible neighbours instead, so the mix has nothing to show.
static void BleedKey(uint32_t* px, int w, int h) {
    static Vec<uint32_t> src;
    src.resize(w * h);
    memcpy(src.p, px, (size_t)w * h * 4);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        uint32_t& p = px[y * w + x];
        if (p >> 24) continue;
        uint32_t r = 0, g = 0, b = 0, n = 0;
        for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
            int xx = x + dx, yy = y + dy;
            if ((dx | dy) == 0 || xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
            uint32_t q = src.p[yy * w + xx];
            if (!(q >> 24)) continue;
            r += (q >> 16) & 255; g += (q >> 8) & 255; b += q & 255; n++;
        }
        p = n ? ((r / n) << 16) | ((g / n) << 8) | (b / n) : 0;     // alpha stays 0
    }
}

// ---- surface faces for other interface versions -------------------------------------------------
struct SurfFace3 : IDirectDrawSurface3Stub {
    Surface* s;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** o) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE AddAttachedSurface(IDirectDrawSurface3* a) override;
    HRESULT STDMETHODCALLTYPE Blt(RECT* d, IDirectDrawSurface3* src, RECT* sr, DWORD f, DDBLTFX* fx) override;
    HRESULT STDMETHODCALLTYPE BltFast(DWORD x, DWORD y, IDirectDrawSurface3* src, RECT* sr, DWORD f) override;
    HRESULT STDMETHODCALLTYPE Flip(IDirectDrawSurface3* t, DWORD f) override;
    HRESULT STDMETHODCALLTYPE GetAttachedSurface(DDSCAPS* caps, IDirectDrawSurface3** out) override;
    HRESULT STDMETHODCALLTYPE GetBltStatus(DWORD) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetFlipStatus(DWORD) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetCaps(DDSCAPS* caps) override;
    HRESULT STDMETHODCALLTYPE GetColorKey(DWORD f, DDCOLORKEY* k) override;
    HRESULT STDMETHODCALLTYPE SetColorKey(DWORD f, DDCOLORKEY* k) override;
    HRESULT STDMETHODCALLTYPE GetDC(HDC* dc) override;
    HRESULT STDMETHODCALLTYPE ReleaseDC(HDC dc) override;
    HRESULT STDMETHODCALLTYPE GetPixelFormat(DDPIXELFORMAT* pf) override;
    HRESULT STDMETHODCALLTYPE GetSurfaceDesc(DDSURFACEDESC* d) override;
    HRESULT STDMETHODCALLTYPE IsLost() override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE Restore() override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE Lock(RECT* r, DDSURFACEDESC* d, DWORD f, HANDLE h) override;
    HRESULT STDMETHODCALLTYPE Unlock(void* p) override;
    HRESULT STDMETHODCALLTYPE SetClipper(IDirectDrawClipper* c) override;
    HRESULT STDMETHODCALLTYPE GetClipper(IDirectDrawClipper** c) override;
    HRESULT STDMETHODCALLTYPE SetPalette(IDirectDrawPalette*) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetDDInterface(void** dd) override;
    HRESULT STDMETHODCALLTYPE PageLock(DWORD) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE PageUnlock(DWORD) override { return DD_OK; }
};
struct SurfFace7 : IDirectDrawSurface7Stub {
    Surface* s;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** o) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE AddAttachedSurface(IDirectDrawSurface7* a) override;
    HRESULT STDMETHODCALLTYPE Blt(RECT* d, IDirectDrawSurface7* src, RECT* sr, DWORD f, DDBLTFX* fx) override;
    HRESULT STDMETHODCALLTYPE BltFast(DWORD x, DWORD y, IDirectDrawSurface7* src, RECT* sr, DWORD f) override;
    HRESULT STDMETHODCALLTYPE Flip(IDirectDrawSurface7* t, DWORD f) override;
    HRESULT STDMETHODCALLTYPE GetAttachedSurface(DDSCAPS2* caps, IDirectDrawSurface7** out) override;
    HRESULT STDMETHODCALLTYPE GetBltStatus(DWORD) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetFlipStatus(DWORD) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetCaps(DDSCAPS2* caps) override;
    HRESULT STDMETHODCALLTYPE GetColorKey(DWORD f, DDCOLORKEY* k) override;
    HRESULT STDMETHODCALLTYPE SetColorKey(DWORD f, DDCOLORKEY* k) override;
    HRESULT STDMETHODCALLTYPE GetDC(HDC* dc) override;
    HRESULT STDMETHODCALLTYPE ReleaseDC(HDC dc) override;
    HRESULT STDMETHODCALLTYPE GetPixelFormat(DDPIXELFORMAT* pf) override;
    HRESULT STDMETHODCALLTYPE GetSurfaceDesc(DDSURFACEDESC2* d) override;
    HRESULT STDMETHODCALLTYPE IsLost() override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE Restore() override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE Lock(RECT* r, DDSURFACEDESC2* d, DWORD f, HANDLE h) override;
    HRESULT STDMETHODCALLTYPE Unlock(RECT* r) override;
    HRESULT STDMETHODCALLTYPE SetClipper(IDirectDrawClipper* c) override;
    HRESULT STDMETHODCALLTYPE GetClipper(IDirectDrawClipper** c) override;
    HRESULT STDMETHODCALLTYPE SetPalette(IDirectDrawPalette*) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetDDInterface(void** dd) override;
    HRESULT STDMETHODCALLTYPE PageLock(DWORD) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE PageUnlock(DWORD) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetUniquenessValue(DWORD* v) override;
    HRESULT STDMETHODCALLTYPE ChangeUniquenessValue() override;
    HRESULT STDMETHODCALLTYPE SetPriority(DWORD) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetPriority(DWORD* p) override { if (p) *p = 0; return DD_OK; }
    HRESULT STDMETHODCALLTYPE SetLOD(DWORD) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetLOD(DWORD* p) override { if (p) *p = 0; return DD_OK; }
};
struct SurfTex : IDirect3DTexture2Stub {
    Surface* s;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** o) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE GetHandle(IDirect3DDevice2*, D3DTEXTUREHANDLE* h) override { if (h) *h = (D3DTEXTUREHANDLE)(uintptr_t)s; return D3D_OK; }
    HRESULT STDMETHODCALLTYPE PaletteChanged(DWORD, DWORD) override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE Load(IDirect3DTexture2* src) override;
};

// ---- the surface ------------------------------------------------------------------------------
struct Surface : IDirectDrawSurface4Stub {
    LONG ref = 1;
    Kind kind;
    int w = 0, h = 0;
    Fmt fmt = kFmt565;
    DDSCAPS2 caps = {};
    uint8_t* bits = nullptr; int pitch = 0;
    bool hasKey = false; uint32_t keyLo = 0, keyHi = 0;
    // GPU copies: bit 0 = colour key applied, bit 1 = the format's own alpha used
    be::Tex* gpu[4] = {}; bool dirty[4] = {true, true, true, true};
    Surface* attached = nullptr;          // primary -> back buffer, back buffer -> z-buffer
    Surface* owner = nullptr;             // a back buffer belongs to its primary
    IDirectDrawClipper* clipper = nullptr;
    DWORD uniq = 1;
    // screen lock / DC
    uint8_t* screenSnap = nullptr; RECT lockRect = {}; bool lockReadOnly = false;
    HDC dc = nullptr; HBITMAP dib = nullptr; HGDIOBJ dcOld = nullptr; void* dibBits = nullptr; int dibW = 0, dibH = 0; bool dcOut = false;
    SurfFace3 f3; SurfFace7 f7; SurfTex tex;

    Surface(Kind k, int ww, int hh, const Fmt& f) : kind(k), w(ww), h(hh), fmt(f) {
        f3.s = this; f7.s = this; tex.s = this;
        pitch = ((w * fmt.bpp / 8) + 3) & ~3;
        if (k == K_TEX || k == K_PLAIN || k == K_ZBUF) bits = (uint8_t*)calloc((size_t)pitch * (h > 0 ? h : 1), 1);
    }
    ~Surface() {
        for (auto& t : gpu) if (t) be::TexDestroy(t);
        free(bits); free(screenSnap);
        if (dc) { SelectObject(dc, dcOld); DeleteObject(dib); DeleteDC(dc); }
        if (attached && attached->owner == this) { attached->owner = nullptr; attached->Release(); }
        if (g_back == this) g_back = nullptr;
        if (g_primary == this) g_primary = nullptr;
    }
    bool Screen() const { return kind == K_PRIMARY || kind == K_BACK; }
    void Changed() { for (auto& d : dirty) d = true; uniq++; }

    // ---- GPU copy for drawing ----
    be::Tex* Gpu(bool keyed, bool formatAlpha) {
        if (Screen() || !bits || w <= 0 || h <= 0) return nullptr;
        if (!fmt.a) formatAlpha = false;
        if (!hasKey) keyed = false;
        int i = (keyed ? 1 : 0) | (formatAlpha ? 2 : 0);
        if (gpu[i] && !dirty[i]) return gpu[i];
        if (!gpu[i]) gpu[i] = be::TexCreate(w, h, kind == K_TEX && g_cfg.mipmaps);
        if (!gpu[i]) return nullptr;
        const Conv& c = ConvFor(fmt);
        static Vec<uint32_t> tmp;
        tmp.resize(w * h);
        int bpp = fmt.bpp / 8;
        for (int y = 0; y < h; y++) {
            const uint8_t* src = bits + (size_t)y * pitch;
            uint32_t* d = tmp.p + (size_t)y * w;
            for (int x = 0; x < w; x++) {
                uint32_t p = ReadPx(src + x * bpp, fmt.bpp);
                uint32_t argb = c.ToARGB(p);
                if (!formatAlpha) argb |= 0xff000000;
                if (keyed && p >= keyLo && p <= keyHi) argb &= 0x00ffffff;
                d[x] = argb;
            }
        }
        if (keyed) BleedKey(tmp.p, w, h);
        be::TexUpload(gpu[i], tmp.p, w * 4);
        dirty[i] = false;
        g_stats.uploads++;
        return gpu[i];
    }

    void FillDesc(DDSURFACEDESC2* d) {
        DWORD size = d->dwSize;
        memset(d, 0, size >= sizeof(DDSURFACEDESC2) ? sizeof(DDSURFACEDESC2) : size);
        d->dwSize = size;
        d->dwFlags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT | (hasKey ? DDSD_CKSRCBLT : 0);
        d->dwWidth = w; d->dwHeight = h; d->lPitch = pitch;
        if (Screen()) { int vw, vh; be::GetVirtualMode(&vw, &vh); if (be::Ready()) { d->dwWidth = w; d->dwHeight = h; } d->lPitch = ((w * 2) + 3) & ~3; }
        d->ddckCKSrcBlt.dwColorSpaceLowValue = keyLo; d->ddckCKSrcBlt.dwColorSpaceHighValue = keyHi;
        if (kind == K_ZBUF) {
            memset(&d->ddpfPixelFormat, 0, sizeof(DDPIXELFORMAT));
            d->ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT); d->ddpfPixelFormat.dwFlags = DDPF_ZBUFFER;
            d->ddpfPixelFormat.dwZBufferBitDepth = fmt.bpp; d->ddpfPixelFormat.dwZBitMask = fmt.bpp == 16 ? 0xffff : 0xffffff;
        } else PFFromFmt(fmt, d->ddpfPixelFormat);
        d->ddsCaps = caps;
        if (kind == K_PRIMARY && attached) { d->dwFlags |= DDSD_BACKBUFFERCOUNT; d->dwBackBufferCount = 1; }
    }

    // ---- IUnknown ----
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** o) override {
        if (!o) return E_POINTER;
        if (r == IID_IUnknown || r == IID_IDirectDrawSurface4) { *o = static_cast<IDirectDrawSurface4*>(this); AddRef(); return S_OK; }
        if (r == IID_IDirectDrawSurface || r == IID_IDirectDrawSurface2 || r == IID_IDirectDrawSurface3) { *o = &f3; AddRef(); return S_OK; }
        if (r == IID_IDirectDrawSurface7) { *o = &f7; AddRef(); return S_OK; }
        if (r == IID_IDirect3DTexture2) { *o = &tex; AddRef(); return S_OK; }
        LogGuidOnce("Surface::QueryInterface unsupported", r);
        *o = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG n = InterlockedDecrement(&ref);
        if (n == 0) delete this;
        return n < 0 ? 0 : n;
    }
    static void LogGuidOnce(const char* what, REFIID r) {
        char key[96];
        _snprintf(key, sizeof(key), "%s %08lx", what, r.Data1);
        LogOnce(strdup(key), "%s: {%08lx-%04x-%04x}", what, r.Data1, r.Data2, r.Data3);
    }

    // ---- attach ----
    HRESULT STDMETHODCALLTYPE AddAttachedSurface(IDirectDrawSurface4* a) override;
    HRESULT STDMETHODCALLTYPE DeleteAttachedSurface(DWORD, IDirectDrawSurface4* a) override;
    HRESULT STDMETHODCALLTYPE GetAttachedSurface(DDSCAPS2* c, IDirectDrawSurface4** out) override {
        if (!out) return DDERR_INVALIDPARAMS;
        *out = nullptr;
        Surface* s = AttachedFor(c);
        if (!s) return DDERR_NOTFOUND;
        s->AddRef(); *out = s; return DD_OK;
    }
    Surface* AttachedFor(const DDSCAPS2* c) {
        if (!attached) return nullptr;
        DWORD want = c ? c->dwCaps : 0;
        if ((want & DDSCAPS_ZBUFFER) && attached->kind != K_ZBUF) {
            return attached->attached && attached->attached->kind == K_ZBUF ? attached->attached : nullptr;
        }
        if ((want & (DDSCAPS_BACKBUFFER | DDSCAPS_FLIP)) && attached->kind != K_BACK) return nullptr;
        return attached;
    }

    // ---- drawing ----
    HRESULT STDMETHODCALLTYPE Blt(RECT* d, IDirectDrawSurface4* src, RECT* sr, DWORD f, DDBLTFX* fx) override;
    HRESULT STDMETHODCALLTYPE BltFast(DWORD x, DWORD y, IDirectDrawSurface4* src, RECT* sr, DWORD f) override;
    HRESULT STDMETHODCALLTYPE Flip(IDirectDrawSurface4*, DWORD) override;
    HRESULT STDMETHODCALLTYPE GetBltStatus(DWORD) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetFlipStatus(DWORD) override { return DD_OK; }

    // ---- description ----
    HRESULT STDMETHODCALLTYPE GetCaps(DDSCAPS2* c) override { if (!c) return DDERR_INVALIDPARAMS; *c = caps; return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetPixelFormat(DDPIXELFORMAT* pf) override {
        if (!pf) return DDERR_INVALIDPARAMS;
        DDSURFACEDESC2 d; d.dwSize = sizeof(d); FillDesc(&d); *pf = d.ddpfPixelFormat; return DD_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSurfaceDesc(DDSURFACEDESC2* d) override {
        if (!d) return DDERR_INVALIDPARAMS;
        if (!d->dwSize) d->dwSize = sizeof(DDSURFACEDESC2);
        FillDesc(d); return DD_OK;
    }
    HRESULT STDMETHODCALLTYPE GetColorKey(DWORD f, DDCOLORKEY* k) override {
        if (!k) return DDERR_INVALIDPARAMS;
        if (!(f & DDCKEY_SRCBLT) || !hasKey) return DDERR_NOCOLORKEY;
        k->dwColorSpaceLowValue = keyLo; k->dwColorSpaceHighValue = keyHi; return DD_OK;
    }
    HRESULT STDMETHODCALLTYPE SetColorKey(DWORD f, DDCOLORKEY* k) override {
        if (!(f & DDCKEY_SRCBLT)) { LogOnce("destkey", "note: destination colour key blit not supported"); return DD_OK; }
        bool had = hasKey; uint32_t lo = keyLo, hi = keyHi;
        if (!k) hasKey = false;
        else { hasKey = true; keyLo = k->dwColorSpaceLowValue; keyHi = (f & DDCKEY_COLORSPACE) ? k->dwColorSpaceHighValue : keyLo; if (keyHi < keyLo) keyHi = keyLo; }
        if (had != hasKey || lo != keyLo || hi != keyHi) { dirty[1] = dirty[3] = true; }
        return DD_OK;
    }
    HRESULT STDMETHODCALLTYPE IsLost() override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE Restore() override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE SetClipper(IDirectDrawClipper* c) override { clipper = c; return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetClipper(IDirectDrawClipper** c) override { if (!c) return DDERR_INVALIDPARAMS; *c = clipper; if (clipper) clipper->AddRef(); return clipper ? DD_OK : DDERR_NOCLIPPERATTACHED; }
    HRESULT STDMETHODCALLTYPE SetPalette(IDirectDrawPalette*) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetDDInterface(void** dd) override;
    HRESULT STDMETHODCALLTYPE PageLock(DWORD) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE PageUnlock(DWORD) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetUniquenessValue(DWORD* v) override { if (v) *v = uniq; return DD_OK; }
    HRESULT STDMETHODCALLTYPE ChangeUniquenessValue() override { uniq++; return DD_OK; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, void*, DWORD, DWORD) override { return DD_OK; }

    // ---- CPU access ----
    HRESULT STDMETHODCALLTYPE Lock(RECT* r, DDSURFACEDESC2* d, DWORD f, HANDLE) override;
    HRESULT STDMETHODCALLTYPE Unlock(RECT*) override;
    HRESULT STDMETHODCALLTYPE GetDC(HDC* out) override;
    HRESULT STDMETHODCALLTYPE ReleaseDC(HDC) override;
    void FillDC(bool screen, int dibPitch, int dh);
};

static Surface* AsSurface(void* p) {
    if (!p) return nullptr;
    // any of our surface interfaces -> the surface
    static void* vt[3]; static bool init = false;
    if (!init) { Surface s(K_PLAIN, 0, 0, kFmt565); vt[0] = *(void**)&s; vt[1] = *(void**)&s.f3; vt[2] = *(void**)&s.f7; init = true; }
    void* v = *(void**)p;
    if (v == vt[0]) return (Surface*)p;
    if (v == vt[1]) return ((SurfFace3*)p)->s;
    if (v == vt[2]) return ((SurfFace7*)p)->s;
    LogOnce("foreignsurf", "note: a surface that is not ours was passed in (%p)", p);
    return nullptr;
}

// ---- attach -----------------------------------------------------------------------------------
HRESULT Surface::AddAttachedSurface(IDirectDrawSurface4* a) {
    Surface* s = AsSurface(a);
    if (!s) return DDERR_INVALIDPARAMS;
    if (s->kind != K_ZBUF) LogOnce("attachnonz", "note: AddAttachedSurface of non-z surface");
    Surface* target = this;
    while (target->attached && target->attached->kind != K_ZBUF && target->attached != s) target = target->attached;
    if (target->attached == s) return DD_OK;
    if (target->attached) target->attached->Release();
    s->AddRef();
    target->attached = s;
    return DD_OK;
}
HRESULT Surface::DeleteAttachedSurface(DWORD, IDirectDrawSurface4* a) {
    Surface* s = AsSurface(a);
    for (Surface* t = this; t; t = t->attached) {
        if (t->attached && (!s || t->attached == s) && t->attached->owner != t) { Surface* x = t->attached; t->attached = nullptr; x->Release(); return DD_OK; }
    }
    return DDERR_SURFACENOTATTACHED;
}

// ---- presenting -------------------------------------------------------------------------------
static int g_frames = 0;
static DWORD g_statStart = 0;
static void PresentFrame() {
    be::Present();
    TraceFrameEnd();
    g_frames++;
    g_stats.frames++;
    DWORD now = GetTickCount();
    if (!g_statStart) g_statStart = now;
    if (now - g_statStart >= 30000) {          // a performance line every 30 seconds
        double s = (now - g_statStart) / 1000.0;
        Log("perf: %.0f fps | per frame: %.0f draws, %.1f texture uploads, %.2f screen text, %.2f screen locks, %.2f screen reads",
            g_stats.frames / s, (double)g_stats.draws / g_stats.frames, (double)g_stats.uploads / g_stats.frames,
            (double)g_stats.getdc / g_stats.frames, (double)g_stats.locks / g_stats.frames, (double)g_stats.reads / g_stats.frames);
        memset(&g_stats, 0, sizeof(g_stats));
        g_statStart = now;
    }
}
HRESULT Surface::Flip(IDirectDrawSurface4*, DWORD) { PresentFrame(); return DD_OK; }

// ---- blits ------------------------------------------------------------------------------------
static void ClipRects(RECT& d, RECT& s, int dw, int dh, int sw, int sh, bool stretch) {
    // keep both rectangles inside their surfaces (the 1999 drivers clipped silently)
    if (!stretch) {
        int dx = 0;
        if (d.left < 0) { dx = -d.left; d.left += dx; s.left += dx; }
        if (d.top < 0) { dx = -d.top; d.top += dx; s.top += dx; }
        if (s.left < 0) { dx = -s.left; s.left += dx; d.left += dx; }
        if (s.top < 0) { dx = -s.top; s.top += dx; d.top += dx; }
        if (d.right > dw) { s.right -= d.right - dw; d.right = dw; }
        if (d.bottom > dh) { s.bottom -= d.bottom - dh; d.bottom = dh; }
        if (s.right > sw) { d.right -= s.right - sw; s.right = sw; }
        if (s.bottom > sh) { d.bottom -= s.bottom - sh; s.bottom = sh; }
    } else {
        if (s.left < 0 || s.top < 0 || s.right > sw || s.bottom > sh) LogOnce("srcclamp", "note: blit source rect out of bounds - clamped");
        if (s.left < 0) s.left = 0; if (s.top < 0) s.top = 0; if (s.right > sw) s.right = sw; if (s.bottom > sh) s.bottom = sh;
    }
}

// CPU blit between memory surfaces (nearest-neighbour stretch, optional colour key)
static void CpuBlit(Surface* dst, RECT d, Surface* src, RECT s, bool key) {
    int dw = d.right - d.left, dh = d.bottom - d.top, sw = s.right - s.left, sh = s.bottom - s.top;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    // clip destination
    int x0 = d.left < 0 ? -d.left : 0, y0 = d.top < 0 ? -d.top : 0;
    int x1 = d.right > dst->w ? dw - (d.right - dst->w) : dw, y1 = d.bottom > dst->h ? dh - (d.bottom - dst->h) : dh;
    bool same = dst->fmt == src->fmt;
    const Conv& sc = ConvFor(src->fmt); const Conv& dc = ConvFor(dst->fmt);
    int sb = src->fmt.bpp / 8, db = dst->fmt.bpp / 8;
    if (same && !key && dw == sw && dh == sh) {
        for (int y = y0; y < y1; y++) {
            int sy = s.top + y; if (sy < 0 || sy >= src->h) continue;
            int sx0 = s.left + x0, n = x1 - x0;
            if (sx0 < 0) { n += sx0; x0 -= sx0; sx0 = 0; }
            if (sx0 + n > src->w) n = src->w - sx0;
            if (n <= 0) continue;
            memmove(dst->bits + (size_t)(d.top + y) * dst->pitch + (size_t)(d.left + x0) * db, src->bits + (size_t)sy * src->pitch + (size_t)sx0 * sb, (size_t)n * db);
        }
        return;
    }
    for (int y = y0; y < y1; y++) {
        int sy = s.top + (int)((int64_t)y * sh / dh);
        if (sy < 0 || sy >= src->h) continue;
        uint8_t* drow = dst->bits + (size_t)(d.top + y) * dst->pitch;
        const uint8_t* srow = src->bits + (size_t)sy * src->pitch;
        for (int x = x0; x < x1; x++) {
            int sx = s.left + (int)((int64_t)x * sw / dw);
            if (sx < 0 || sx >= src->w) continue;
            uint32_t p = ReadPx(srow + (size_t)sx * sb, src->fmt.bpp);
            if (key && p >= src->keyLo && p <= src->keyHi) continue;
            WritePx(drow + (size_t)(d.left + x) * db, dst->fmt.bpp, same ? p : dc.FromARGB(sc.ToARGB(p)));
        }
    }
}

// Write changed pixels back onto the screen: argb pixels with alpha 0 are left alone
static void ScreenWrite(const RECT& r, const uint32_t* argb, int pitchPx) {
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;
    // the same text usually lands in the same box every frame: keep a few textures around
    static struct { be::Tex* t; int w, h; DWORD used; } cache[4];
    int pick = -1, oldest = 0;
    for (int i = 0; i < 4; i++) {
        if (cache[i].t && cache[i].w == w && cache[i].h == h) { pick = i; break; }
        if (cache[i].used < cache[oldest].used) oldest = i;
    }
    if (pick < 0) {
        pick = oldest;
        if (cache[pick].t) be::TexDestroy(cache[pick].t);
        cache[pick].t = be::TexCreate(w, h, false); cache[pick].w = w; cache[pick].h = h;
        if (!cache[pick].t) return;
    }
    cache[pick].used = GetTickCount();
    static Vec<uint32_t> tmp;                  // edges of the drawn text blend smoothly when scaled
    tmp.resize(w * h);
    for (int y = 0; y < h; y++) memcpy(tmp.p + (size_t)y * w, argb + (size_t)y * pitchPx, (size_t)w * 4);
    BleedKey(tmp.p, w, h);
    be::TexUpload(cache[pick].t, tmp.p, w * 4);
    RECT s = {0, 0, w, h};
    be::Blit(cache[pick].t, w, h, s, r, false);
}

static uint32_t FillToARGB(Surface* s, uint32_t raw) { return ConvFor(s->Screen() ? kFmt565 : s->fmt).ToARGB(raw) | 0xff000000; }

static HRESULT DoBlt(Surface* dst, RECT* dr, Surface* src, RECT* sr, DWORD f, DDBLTFX* fx) {
    RECT d = dr ? *dr : RECT{0, 0, dst->w, dst->h};
    if (Tracing()) TraceLog("BLT kind %d <- %d flags %lx dst %ld,%ld-%ld,%ld", dst->kind, src ? src->kind : -1, f, d.left, d.top, d.right, d.bottom);
    if (f & DDBLT_COLORFILL) {
        uint32_t raw = fx ? fx->dwFillColor : 0;
        if (dst->Screen()) { be::Fill(&d, FillToARGB(dst, raw)); if (dst->kind == K_PRIMARY) PresentFrame(); return DD_OK; }
        if (!dst->bits) return DD_OK;
        int b = dst->fmt.bpp / 8;
        for (int y = d.top < 0 ? 0 : d.top; y < d.bottom && y < dst->h; y++)
            for (int x = d.left < 0 ? 0 : d.left; x < d.right && x < dst->w; x++) WritePx(dst->bits + (size_t)y * dst->pitch + (size_t)x * b, dst->fmt.bpp, raw);
        dst->Changed();
        return DD_OK;
    }
    if (f & DDBLT_DEPTHFILL) {
        if (dst->kind == K_ZBUF) {
            float z = fx ? (float)((double)fx->dwFillDepth / (dst->fmt.bpp == 16 ? 65535.0 : 16777215.0)) : 1.f;
            be::Clear(0, nullptr, D3DCLEAR_ZBUFFER, 0, z > 1 ? 1 : z, 0);
        }
        return DD_OK;
    }
    if (!src) { LogOnce("bltnosrc", "note: Blt without source, flags %08lx", f); return DD_OK; }
    RECT s = sr ? *sr : RECT{0, 0, src->w, src->h};
    if (f & (DDBLT_DDFX | DDBLT_ROTATIONANGLE)) LogOnce("bltfx", "note: Blt DDFX effects (%08lx) ignored", f);
    if (f & (DDBLT_KEYDEST | DDBLT_KEYDESTOVERRIDE)) LogOnce("destkey", "note: destination colour key blit not supported");
    bool key = (f & (DDBLT_KEYSRC | DDBLT_KEYSRCOVERRIDE)) != 0;
    uint32_t saveLo = src->keyLo, saveHi = src->keyHi; bool saveHas = src->hasKey;
    if ((f & DDBLT_KEYSRCOVERRIDE) && fx) { src->hasKey = true; src->keyLo = fx->ddckSrcColorkey.dwColorSpaceLowValue; src->keyHi = fx->ddckSrcColorkey.dwColorSpaceHighValue; if (src->keyHi < src->keyLo) src->keyHi = src->keyLo; src->dirty[1] = src->dirty[3] = true; }
    bool stretch = (d.right - d.left) != (s.right - s.left) || (d.bottom - d.top) != (s.bottom - s.top);

    if (dst->Screen() && !src->Screen()) {
        ClipRects(d, s, dst->w, dst->h, src->w, src->h, stretch);
        if (d.right > d.left && d.bottom > d.top) {
            be::Tex* t = src->Gpu(key && src->hasKey, false);
            be::Blit(t, src->w, src->h, s, d, false);
        }
        if (dst->kind == K_PRIMARY) { LogOnce("plain2prim", "note: blit straight to the front buffer (drawn on back buffer + present)"); PresentFrame(); }
    } else if (dst->Screen() && src->Screen()) {
        if (dst->kind == K_PRIMARY && src->kind == K_BACK) PresentFrame();          // windowed present
        else if (dst == src || (dst->kind == K_BACK && src->kind == K_BACK)) LogOnce("back2back", "note: back buffer self-blit ignored");
        else if (dst->kind == K_BACK && src->kind == K_PRIMARY) LogOnce("prim2back", "note: front-to-back blit ignored");
    } else if (!dst->Screen() && src->Screen()) {
        // screen -> memory (e.g. a saved picture of the screen)
        int vw, vh; be::GetVirtualMode(&vw, &vh);
        if (s.left < 0) s.left = 0; if (s.top < 0) s.top = 0; if (s.right > vw) s.right = vw; if (s.bottom > vh) s.bottom = vh;
        int sw = s.right - s.left, sh = s.bottom - s.top;
        if (sw > 0 && sh > 0 && dst->bits) {
            Vec<uint32_t> px; px.resize(sw * sh);
            g_stats.reads++;
            if (be::ReadBack(s, px.p, sw)) {
                Surface tmp(K_PLAIN, sw, sh, Fmt{32, 0xff0000, 0xff00, 0xff, 0});
                for (int y = 0; y < sh; y++) memcpy(tmp.bits + (size_t)y * tmp.pitch, px.p + (size_t)y * sw, (size_t)sw * 4);
                RECT ts = {0, 0, sw, sh};
                CpuBlit(dst, d, &tmp, ts, false);
                dst->Changed();
            }
        }
    } else {
        if (dst->bits && src->bits) { CpuBlit(dst, d, src, s, key && src->hasKey); dst->Changed(); }
    }
    if ((f & DDBLT_KEYSRCOVERRIDE) && fx) { src->hasKey = saveHas; src->keyLo = saveLo; src->keyHi = saveHi; src->dirty[1] = src->dirty[3] = true; }
    return DD_OK;
}

HRESULT Surface::Blt(RECT* d, IDirectDrawSurface4* src, RECT* sr, DWORD f, DDBLTFX* fx) { return DoBlt(this, d, AsSurface(src), sr, f, fx); }
HRESULT Surface::BltFast(DWORD x, DWORD y, IDirectDrawSurface4* srcI, RECT* sr, DWORD f) {
    Surface* src = AsSurface(srcI);
    if (!src) return DDERR_INVALIDPARAMS;
    RECT s = sr ? *sr : RECT{0, 0, src->w, src->h};
    RECT d = {(LONG)x, (LONG)y, (LONG)(x + (s.right - s.left)), (LONG)(y + (s.bottom - s.top))};
    if (Tracing()) TraceLog("BLTFAST kind %d <- %d at %lu,%lu", kind, src->kind, x, y);
    return DoBlt(this, &d, src, &s, (f & DDBLTFAST_SRCCOLORKEY) ? DDBLT_KEYSRC : 0, nullptr);
}

// ---- CPU access -------------------------------------------------------------------------------
HRESULT Surface::Lock(RECT* r, DDSURFACEDESC2* d, DWORD f, HANDLE) {
    if (!d) return DDERR_INVALIDPARAMS;
    if (!d->dwSize) d->dwSize = sizeof(DDSURFACEDESC2);
    FillDesc(d);
    if (Tracing()) TraceLog("LOCK kind %d %dx%d flags %lx", kind, w, h, f);
    if (Screen()) {
        // the screen is on the graphics card: read it back (slow, rare)
        LogOnce("screenlock", "note: screen surface locked - slow path");
        g_stats.locks++;
        pitch = ((w * 2) + 3) & ~3;
        if (!bits) bits = (uint8_t*)calloc((size_t)pitch * h, 1);
        static Vec<uint32_t> px; px.resize(w * h);
        RECT all = {0, 0, w, h};
        if (g_lockLogged < 3) {
            g_lockLogged++;
            Log("screen lock %d: flags %08lx, rect %s, called from %p", g_lockLogged, f, r ? "part" : "whole", __builtin_return_address(0));
        }
        if (be::ReadBack(all, px.p, w, true)) {
            for (int y = 0; y < h; y++) {
                uint16_t* o = (uint16_t*)(bits + (size_t)y * pitch); const uint32_t* q = px.p + (size_t)y * w;
                for (int x = 0; x < w; x++) { uint32_t c = q[x]; o[x] = (uint16_t)(((c >> 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 3) & 0x001f)); }
            }
        }
        lockReadOnly = (f & DDLOCK_READONLY) != 0;
        if (!lockReadOnly) { if (!screenSnap) screenSnap = (uint8_t*)malloc((size_t)pitch * h); memcpy(screenSnap, bits, (size_t)pitch * h); }
        lockRect = r ? *r : RECT{0, 0, w, h};
        PFFromFmt(kFmt565, d->ddpfPixelFormat);
    }
    if (!bits) return DDERR_GENERIC;
    d->lPitch = pitch;
    int bpp = (Screen() ? 16 : fmt.bpp) / 8;
    d->lpSurface = bits + (r ? (size_t)r->top * pitch + (size_t)r->left * bpp : 0);
    d->dwFlags |= DDSD_LPSURFACE;
    return DD_OK;
}
HRESULT Surface::Unlock(RECT*) {
    if (Screen()) {
        if (bits && screenSnap && !lockReadOnly) {
            // pixels the game changed go back onto the screen
            const Conv& c = ConvFor(kFmt565);
            int x0 = w, y0 = h, x1 = -1, y1 = -1;
            for (int y = 0; y < h; y++) {
                const uint16_t* a = (const uint16_t*)(bits + (size_t)y * pitch); const uint16_t* b = (const uint16_t*)(screenSnap + (size_t)y * pitch);
                for (int x = 0; x < w; x++) if (a[x] != b[x]) { if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
            }
            if (x1 >= x0) {
                int ww = x1 - x0 + 1, hh = y1 - y0 + 1;
                if (g_lockLogged < 6) { g_lockLogged++; Log("screen lock: the game changed %dx%d pixels at %d,%d", ww, hh, x0, y0); }
                Vec<uint32_t> px; px.resize(ww * hh);
                for (int y = 0; y < hh; y++) {
                    const uint16_t* a = (const uint16_t*)(bits + (size_t)(y0 + y) * pitch) + x0; const uint16_t* b = (const uint16_t*)(screenSnap + (size_t)(y0 + y) * pitch) + x0;
                    for (int x = 0; x < ww; x++) px[y * ww + x] = a[x] != b[x] ? (c.ToARGB(a[x]) | 0xff000000) : 0;
                }
                RECT rr = {x0, y0, x1 + 1, y1 + 1};
                ScreenWrite(rr, px.p, ww);
            }
        }
        // the buffers are kept for the next lock (the game locks every frame)
        if (kind == K_PRIMARY && !lockReadOnly) PresentFrame();
        return DD_OK;
    }
    Changed();
    return DD_OK;
}

HRESULT Surface::GetDC(HDC* out) {
    if (!out) return DDERR_INVALIDPARAMS;
    if (dcOut) { *out = dc; return DD_OK; }
    int dw = w, dh = h;
    bool screen = Screen();
    Fmt f = screen ? Fmt{32, 0xff0000, 0xff00, 0xff, 0} : fmt;
    int dibPitch = ((dw * f.bpp / 8) + 3) & ~3;
    if (dc && dibW == dw && dibH == dh) { dcOut = true; FillDC(screen, dibPitch, dh); *out = dc; return DD_OK; }
    if (dc) { SelectObject(dc, dcOld); DeleteObject(dib); DeleteDC(dc); dc = nullptr; dib = nullptr; dibBits = nullptr; }
    struct { BITMAPINFOHEADER h; DWORD masks[3]; } bi = {};
    bi.h.biSize = sizeof(BITMAPINFOHEADER); bi.h.biWidth = dw; bi.h.biHeight = -dh; bi.h.biPlanes = 1;
    bi.h.biBitCount = (WORD)f.bpp;
    bi.h.biCompression = f.bpp == 24 ? BI_RGB : BI_BITFIELDS;
    bi.masks[0] = f.r; bi.masks[1] = f.g; bi.masks[2] = f.b;
    dc = CreateCompatibleDC(nullptr);
    dib = CreateDIBSection(dc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
    if (!dc || !dib || !dibBits) { if (dib) DeleteObject(dib); if (dc) DeleteDC(dc); dc = nullptr; dib = nullptr; return DDERR_GENERIC; }
    dcOld = SelectObject(dc, dib);
    dibW = dw; dibH = dh; dcOut = true;
    FillDC(screen, dibPitch, dh);
    *out = dc;
    return DD_OK;
}
void Surface::FillDC(bool screen, int dibPitch, int dh) {
    if (screen) {
        // No read-back from the graphics card (too slow every frame): GDI draws onto a blank page
        // and only what it drew is put on the screen afterwards.
        uint32_t* p = (uint32_t*)dibBits;
        for (size_t i = 0, n = (size_t)dibPitch / 4 * dh; i < n; i++) p[i] = kDcBlank;
        g_stats.getdc++;
    } else if (bits) {
        for (int y = 0; y < dh; y++) memcpy((uint8_t*)dibBits + (size_t)y * dibPitch, bits + (size_t)y * pitch, (size_t)(dibPitch < pitch ? dibPitch : pitch));
    }
}
HRESULT Surface::ReleaseDC(HDC) {
    if (!dc || !dcOut) return DD_OK;
    dcOut = false;
    GdiFlush();
    int dw = w, dh = h;
    bool screen = Screen();
    int bpp = screen ? 32 : fmt.bpp;
    int dibPitch = ((dw * bpp / 8) + 3) & ~3;
    if (screen) {
        const uint32_t* now = (const uint32_t*)dibBits;
        int x0 = dw, y0 = dh, x1 = -1, y1 = -1, pp = dibPitch / 4;
        for (int y = 0; y < dh; y++) {
            const uint32_t* row = now + (size_t)y * pp;
            for (int x = 0; x < dw; x++)
                if ((row[x] & 0xffffff) != kDcBlank) { if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
        }
        if (x1 >= x0) {
            int ww = x1 - x0 + 1, hh = y1 - y0 + 1;
            Vec<uint32_t> px; px.resize(ww * hh);
            for (int y = 0; y < hh; y++) for (int x = 0; x < ww; x++) {
                uint32_t a = now[(y0 + y) * pp + x0 + x] & 0xffffff;
                px[y * ww + x] = a != kDcBlank ? (a | 0xff000000) : 0;
            }
            RECT rr = {x0, y0, x1 + 1, y1 + 1};
            ScreenWrite(rr, px.p, ww);
        }
        if (kind == K_PRIMARY) PresentFrame();
    } else if (bits) {
        for (int y = 0; y < dh; y++) memcpy(bits + (size_t)y * pitch, (uint8_t*)dibBits + (size_t)y * dibPitch, (size_t)(dibPitch < pitch ? dibPitch : pitch));
        Changed();
    }
    return DD_OK;          // the DC and its bitmap are kept for the next GetDC
}

// ---- faces: forwarding with descriptor conversion ------------------------------------------------
HRESULT SurfFace3::QueryInterface(REFIID r, void** o) { return s->QueryInterface(r, o); }
ULONG SurfFace3::AddRef() { return s->AddRef(); }
ULONG SurfFace3::Release() { return s->Release(); }
HRESULT SurfFace3::AddAttachedSurface(IDirectDrawSurface3* a) { return s->AddAttachedSurface((IDirectDrawSurface4*)a); }
HRESULT SurfFace3::Blt(RECT* d, IDirectDrawSurface3* src, RECT* sr, DWORD f, DDBLTFX* fx) { return DoBlt(s, d, AsSurface(src), sr, f, fx); }
HRESULT SurfFace3::BltFast(DWORD x, DWORD y, IDirectDrawSurface3* src, RECT* sr, DWORD f) { return s->BltFast(x, y, (IDirectDrawSurface4*)src, sr, f); }
HRESULT SurfFace3::Flip(IDirectDrawSurface3*, DWORD f) { return s->Flip(nullptr, f); }
HRESULT SurfFace3::GetAttachedSurface(DDSCAPS* caps, IDirectDrawSurface3** out) {
    if (!out) return DDERR_INVALIDPARAMS;
    DDSCAPS2 c = {}; if (caps) c.dwCaps = caps->dwCaps;
    Surface* a = s->AttachedFor(&c);
    *out = nullptr;
    if (!a) return DDERR_NOTFOUND;
    a->AddRef(); *out = &a->f3; return DD_OK;
}
HRESULT SurfFace3::GetCaps(DDSCAPS* caps) { if (!caps) return DDERR_INVALIDPARAMS; caps->dwCaps = s->caps.dwCaps; return DD_OK; }
HRESULT SurfFace3::GetColorKey(DWORD f, DDCOLORKEY* k) { return s->GetColorKey(f, k); }
HRESULT SurfFace3::SetColorKey(DWORD f, DDCOLORKEY* k) { return s->SetColorKey(f, k); }
HRESULT SurfFace3::GetDC(HDC* dc) { return s->GetDC(dc); }
HRESULT SurfFace3::ReleaseDC(HDC dc) { return s->ReleaseDC(dc); }
HRESULT SurfFace3::GetPixelFormat(DDPIXELFORMAT* pf) { return s->GetPixelFormat(pf); }
HRESULT SurfFace3::GetSurfaceDesc(DDSURFACEDESC* d) {
    if (!d) return DDERR_INVALIDPARAMS;
    DDSURFACEDESC2 d2; d2.dwSize = sizeof(d2); s->FillDesc(&d2); Desc2To1(&d2, d); return DD_OK;
}
HRESULT SurfFace3::Lock(RECT* r, DDSURFACEDESC* d, DWORD f, HANDLE h) {
    if (!d) return DDERR_INVALIDPARAMS;
    DDSURFACEDESC2 d2 = {}; d2.dwSize = sizeof(d2);
    HRESULT hr = s->Lock(r, &d2, f, h);
    if (SUCCEEDED(hr)) Desc2To1(&d2, d);
    return hr;
}
HRESULT SurfFace3::Unlock(void*) { return s->Unlock(nullptr); }
HRESULT SurfFace3::SetClipper(IDirectDrawClipper* c) { return s->SetClipper(c); }
HRESULT SurfFace3::GetClipper(IDirectDrawClipper** c) { return s->GetClipper(c); }
HRESULT SurfFace3::GetDDInterface(void** dd) { return s->GetDDInterface(dd); }

HRESULT SurfFace7::QueryInterface(REFIID r, void** o) { return s->QueryInterface(r, o); }
ULONG SurfFace7::AddRef() { return s->AddRef(); }
ULONG SurfFace7::Release() { return s->Release(); }
HRESULT SurfFace7::AddAttachedSurface(IDirectDrawSurface7* a) { return s->AddAttachedSurface((IDirectDrawSurface4*)a); }
HRESULT SurfFace7::Blt(RECT* d, IDirectDrawSurface7* src, RECT* sr, DWORD f, DDBLTFX* fx) { return DoBlt(s, d, AsSurface(src), sr, f, fx); }
HRESULT SurfFace7::BltFast(DWORD x, DWORD y, IDirectDrawSurface7* src, RECT* sr, DWORD f) { return s->BltFast(x, y, (IDirectDrawSurface4*)src, sr, f); }
HRESULT SurfFace7::Flip(IDirectDrawSurface7*, DWORD f) { return s->Flip(nullptr, f); }
HRESULT SurfFace7::GetAttachedSurface(DDSCAPS2* caps, IDirectDrawSurface7** out) {
    if (!out) return DDERR_INVALIDPARAMS;
    Surface* a = s->AttachedFor(caps);
    *out = nullptr;
    if (!a) return DDERR_NOTFOUND;
    a->AddRef(); *out = &a->f7; return DD_OK;
}
HRESULT SurfFace7::GetCaps(DDSCAPS2* caps) { return s->GetCaps(caps); }
HRESULT SurfFace7::GetColorKey(DWORD f, DDCOLORKEY* k) { return s->GetColorKey(f, k); }
HRESULT SurfFace7::SetColorKey(DWORD f, DDCOLORKEY* k) { return s->SetColorKey(f, k); }
HRESULT SurfFace7::GetDC(HDC* dc) { return s->GetDC(dc); }
HRESULT SurfFace7::ReleaseDC(HDC dc) { return s->ReleaseDC(dc); }
HRESULT SurfFace7::GetPixelFormat(DDPIXELFORMAT* pf) { return s->GetPixelFormat(pf); }
HRESULT SurfFace7::GetSurfaceDesc(DDSURFACEDESC2* d) { return s->GetSurfaceDesc(d); }
HRESULT SurfFace7::Lock(RECT* r, DDSURFACEDESC2* d, DWORD f, HANDLE h) { return s->Lock(r, d, f, h); }
HRESULT SurfFace7::Unlock(RECT* r) { return s->Unlock(r); }
HRESULT SurfFace7::SetClipper(IDirectDrawClipper* c) { return s->SetClipper(c); }
HRESULT SurfFace7::GetClipper(IDirectDrawClipper** c) { return s->GetClipper(c); }
HRESULT SurfFace7::GetDDInterface(void** dd) { return s->GetDDInterface(dd); }
HRESULT SurfFace7::GetUniquenessValue(DWORD* v) { return s->GetUniquenessValue(v); }
HRESULT SurfFace7::ChangeUniquenessValue() { return s->ChangeUniquenessValue(); }

HRESULT SurfTex::QueryInterface(REFIID r, void** o) { return s->QueryInterface(r, o); }
ULONG SurfTex::AddRef() { return s->AddRef(); }
ULONG SurfTex::Release() { return s->Release(); }
HRESULT SurfTex::Load(IDirect3DTexture2* srcI) {
    if (!srcI) return DDERR_INVALIDPARAMS;
    Surface* src = ((SurfTex*)srcI)->s;
    if (!src->bits || !s->bits) return D3D_OK;
    if (src->w != s->w || src->h != s->h || !(src->fmt == s->fmt)) {
        LogOnce("=texload", "note: Texture2::Load between different sizes/formats");
        RECT a = {0, 0, s->w, s->h}, b = {0, 0, src->w, src->h};
        CpuBlit(s, a, src, b, false);
    } else memcpy(s->bits, src->bits, (size_t)s->pitch * s->h);
    s->hasKey = src->hasKey; s->keyLo = src->keyLo; s->keyHi = src->keyHi;
    s->Changed();
    return D3D_OK;
}

// ====================================================================================================
// Clipper
// ====================================================================================================
struct Clipper : IDirectDrawClipper {
    LONG ref = 1; HWND hwnd = nullptr;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** o) override {
        if (r == IID_IUnknown || r == IID_IDirectDrawClipper) { *o = this; AddRef(); return S_OK; }
        *o = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override { LONG n = InterlockedDecrement(&ref); if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE GetClipList(RECT*, RGNDATA*, DWORD*) override { Unimpl("IDirectDrawClipper::GetClipList"); return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetHWnd(HWND* h) override { if (h) *h = hwnd; return DD_OK; }
    HRESULT STDMETHODCALLTYPE Initialize(IDirectDraw*, DWORD) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE IsClipListChanged(WINBOOL* c) override { if (c) *c = FALSE; return DD_OK; }
    HRESULT STDMETHODCALLTYPE SetClipList(RGNDATA*, DWORD) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE SetHWnd(DWORD, HWND h) override { hwnd = h; return DD_OK; }
};

// ====================================================================================================
// Direct3D: device caps, viewport, device
// ====================================================================================================
static void FillDeviceDesc(D3DDEVICEDESC* d) {
    DWORD size = d->dwSize ? d->dwSize : sizeof(D3DDEVICEDESC);
    D3DDEVICEDESC x = {};
    x.dwSize = size;
    x.dwFlags = D3DDD_COLORMODEL | D3DDD_DEVCAPS | D3DDD_TRANSFORMCAPS | D3DDD_LIGHTINGCAPS | D3DDD_BCLIPPING | D3DDD_LINECAPS |
                D3DDD_TRICAPS | D3DDD_DEVICERENDERBITDEPTH | D3DDD_DEVICEZBUFFERBITDEPTH | D3DDD_MAXBUFFERSIZE | D3DDD_MAXVERTEXCOUNT;
    x.dcmColorModel = D3DCOLOR_RGB;
    x.dwDevCaps = D3DDEVCAPS_FLOATTLVERTEX | D3DDEVCAPS_EXECUTESYSTEMMEMORY | D3DDEVCAPS_TLVERTEXSYSTEMMEMORY | D3DDEVCAPS_TEXTUREVIDEOMEMORY |
                  D3DDEVCAPS_DRAWPRIMTLVERTEX | D3DDEVCAPS_CANRENDERAFTERFLIP | D3DDEVCAPS_TEXTURENONLOCALVIDMEM | D3DDEVCAPS_DRAWPRIMITIVES2 |
                  D3DDEVCAPS_HWRASTERIZATION | D3DDEVCAPS_TEXTURESYSTEMMEMORY;
    x.dtcTransformCaps.dwSize = sizeof(D3DTRANSFORMCAPS); x.dtcTransformCaps.dwCaps = D3DTRANSFORMCAPS_CLIP;
    x.bClipping = TRUE;
    x.dlcLightingCaps.dwSize = sizeof(D3DLIGHTINGCAPS);
    x.dlcLightingCaps.dwCaps = D3DLIGHTCAPS_POINT | D3DLIGHTCAPS_SPOT | D3DLIGHTCAPS_DIRECTIONAL;
    x.dlcLightingCaps.dwLightingModel = D3DLIGHTINGMODEL_RGB; x.dlcLightingCaps.dwNumLights = 8;
    D3DPRIMCAPS p = {};
    p.dwSize = sizeof(D3DPRIMCAPS);
    p.dwMiscCaps = D3DPMISCCAPS_MASKZ | D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW;
    p.dwRasterCaps = D3DPRASTERCAPS_DITHER | D3DPRASTERCAPS_ZTEST | D3DPRASTERCAPS_SUBPIXEL | D3DPRASTERCAPS_FOGVERTEX | D3DPRASTERCAPS_FOGTABLE |
                     D3DPRASTERCAPS_MIPMAPLODBIAS | D3DPRASTERCAPS_ZBIAS | D3DPRASTERCAPS_ANISOTROPY | D3DPRASTERCAPS_WFOG | D3DPRASTERCAPS_ZFOG;
    p.dwZCmpCaps = p.dwAlphaCmpCaps = 0xff;
    p.dwSrcBlendCaps = p.dwDestBlendCaps = 0x1fff;
    p.dwShadeCaps = D3DPSHADECAPS_COLORFLATRGB | D3DPSHADECAPS_COLORGOURAUDRGB | D3DPSHADECAPS_SPECULARFLATRGB | D3DPSHADECAPS_SPECULARGOURAUDRGB |
                    D3DPSHADECAPS_ALPHAFLATBLEND | D3DPSHADECAPS_ALPHAGOURAUDBLEND | D3DPSHADECAPS_FOGFLAT | D3DPSHADECAPS_FOGGOURAUD;
    p.dwTextureCaps = D3DPTEXTURECAPS_PERSPECTIVE | D3DPTEXTURECAPS_ALPHA | D3DPTEXTURECAPS_TRANSPARENCY | D3DPTEXTURECAPS_BORDER | D3DPTEXTURECAPS_POW2;
    p.dwTextureFilterCaps = D3DPTFILTERCAPS_NEAREST | D3DPTFILTERCAPS_LINEAR | D3DPTFILTERCAPS_MIPNEAREST | D3DPTFILTERCAPS_MIPLINEAR |
                            D3DPTFILTERCAPS_LINEARMIPNEAREST | D3DPTFILTERCAPS_LINEARMIPLINEAR | D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR |
                            D3DPTFILTERCAPS_MINFANISOTROPIC | D3DPTFILTERCAPS_MIPFPOINT | D3DPTFILTERCAPS_MIPFLINEAR | D3DPTFILTERCAPS_MAGFPOINT |
                            D3DPTFILTERCAPS_MAGFLINEAR | D3DPTFILTERCAPS_MAGFANISOTROPIC;
    p.dwTextureBlendCaps = D3DPTBLENDCAPS_DECAL | D3DPTBLENDCAPS_MODULATE | D3DPTBLENDCAPS_DECALALPHA | D3DPTBLENDCAPS_MODULATEALPHA |
                           D3DPTBLENDCAPS_DECALMASK | D3DPTBLENDCAPS_MODULATEMASK | D3DPTBLENDCAPS_COPY | D3DPTBLENDCAPS_ADD;
    p.dwTextureAddressCaps = D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_MIRROR | D3DPTADDRESSCAPS_CLAMP | D3DPTADDRESSCAPS_BORDER | D3DPTADDRESSCAPS_INDEPENDENTUV;
    x.dpcLineCaps = p; x.dpcTriCaps = p;
    x.dwDeviceRenderBitDepth = DDBD_16 | DDBD_32;
    x.dwDeviceZBufferBitDepth = DDBD_16 | DDBD_24 | DDBD_32;
    x.dwMaxBufferSize = 0; x.dwMaxVertexCount = 65535;
    x.dwMinTextureWidth = 1; x.dwMinTextureHeight = 1;
    x.dwMaxTextureWidth = 4096; x.dwMaxTextureHeight = 4096;
    x.dwMaxTextureRepeat = 8192; x.dwMaxTextureAspectRatio = 4096; x.dwMaxAnisotropy = 16;
    x.dvGuardBandLeft = -4096; x.dvGuardBandTop = -4096; x.dvGuardBandRight = 4096; x.dvGuardBandBottom = 4096;
    x.dwStencilCaps = 0xff;
    x.dwFVFCaps = 2;
    x.dwTextureOpCaps = 0x3ffff;
    x.wMaxTextureBlendStages = 8; x.wMaxSimultaneousTextures = 2;
    memcpy(d, &x, size < sizeof(x) ? size : sizeof(x));
    d->dwSize = size;
}

static void Fill16(DDPIXELFORMAT* pf, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    memset(pf, 0, sizeof(*pf));
    pf->dwSize = sizeof(*pf); pf->dwFlags = DDPF_RGB | (a ? DDPF_ALPHAPIXELS : 0); pf->dwRGBBitCount = 16;
    pf->dwRBitMask = r; pf->dwGBitMask = g; pf->dwBBitMask = b; pf->dwRGBAlphaBitMask = a;
}

struct Viewport : IDirect3DViewport3Stub {
    LONG ref = 1;
    D3DVIEWPORT2 vp = {};
    Device* dev = nullptr;
    Viewport() { vp.dwSize = sizeof(vp); vp.dwWidth = 640; vp.dwHeight = 480; vp.dvMaxZ = 1; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** o) override {
        if (r == IID_IUnknown || r == IID_IDirect3DViewport3 || r == IID_IDirect3DViewport2 || r == IID_IDirect3DViewport) { *o = this; AddRef(); return S_OK; }
        *o = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override { LONG n = InterlockedDecrement(&ref); if (!n) delete this; return n; }
    void Apply();
    HRESULT STDMETHODCALLTYPE Initialize(IDirect3D*) override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT* v) override {
        if (!v) return DDERR_INVALIDPARAMS;
        v->dwX = vp.dwX; v->dwY = vp.dwY; v->dwWidth = vp.dwWidth; v->dwHeight = vp.dwHeight;
        v->dvMinZ = vp.dvMinZ; v->dvMaxZ = vp.dvMaxZ; v->dvScaleX = vp.dwWidth / 2.f; v->dvScaleY = vp.dwHeight / 2.f;
        v->dvMaxX = 1; v->dvMaxY = 1; return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE SetViewport(D3DVIEWPORT* v) override {
        if (!v) return DDERR_INVALIDPARAMS;
        vp.dwX = v->dwX; vp.dwY = v->dwY; vp.dwWidth = v->dwWidth; vp.dwHeight = v->dwHeight; vp.dvMinZ = v->dvMinZ; vp.dvMaxZ = v->dvMaxZ;
        Apply(); return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetViewport2(D3DVIEWPORT2* v) override { if (!v) return DDERR_INVALIDPARAMS; DWORD s = v->dwSize; *v = vp; if (s) v->dwSize = s; return D3D_OK; }
    HRESULT STDMETHODCALLTYPE SetViewport2(D3DVIEWPORT2* v) override {
        if (!v) return DDERR_INVALIDPARAMS;
        vp = *v;
        if (Tracing()) TraceLog("VIEWPORT %p: %lu,%lu %lux%lu  clip %.3f,%.3f %.3fx%.3f  z %.3f..%.3f", this, vp.dwX, vp.dwY, vp.dwWidth, vp.dwHeight, vp.dvClipX, vp.dvClipY, vp.dvClipWidth, vp.dvClipHeight, vp.dvMinZ, vp.dvMaxZ);
        Apply(); return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE SetBackground(D3DMATERIALHANDLE) override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE Clear(DWORD count, D3DRECT* rects, DWORD flags) override {
        if (Tracing()) TraceLog("CLEAR vp %p count %lu flags %lx (v1)", this, count, flags);
        be::Clear(count, rects, flags & (D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER), 0, 1.f, 0);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE Clear2(DWORD count, D3DRECT* rects, DWORD flags, D3DCOLOR color, D3DVALUE z, DWORD stencil) override {
        if (Tracing()) {
            TraceLog("CLEAR vp %p count %lu flags %lx color %08lx z %.4f", this, count, flags, color, z);
            for (DWORD i = 0; i < count && rects; i++) TraceLog("   rect %ld,%ld - %ld,%ld", rects[i].x1, rects[i].y1, rects[i].x2, rects[i].y2);
        }
        be::Clear(count, rects, flags, color, z, stencil);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE AddLight(IDirect3DLight*) override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE DeleteLight(IDirect3DLight*) override { return D3D_OK; }
};

struct Device : IDirect3DDevice3Stub {
    LONG ref = 1;
    Surface* target;
    Viewport* current = nullptr;
    Vec<Viewport*> viewports;
    DWORD rs[256] = {};
    DWORD tss[8][32] = {};
    Surface* tex[8] = {};
    IDirect3D3* d3d;
    Device(Surface* t, IDirect3D3* d) : target(t), d3d(d) {
        rs[D3DRENDERSTATE_ZENABLE] = 1; rs[D3DRENDERSTATE_ZWRITEENABLE] = 1; rs[D3DRENDERSTATE_ZFUNC] = D3DCMP_LESSEQUAL;
        rs[D3DRENDERSTATE_SRCBLEND] = D3DBLEND_ONE; rs[D3DRENDERSTATE_DESTBLEND] = D3DBLEND_ZERO; rs[D3DRENDERSTATE_CULLMODE] = D3DCULL_CCW;
        rs[D3DRENDERSTATE_ALPHAFUNC] = D3DCMP_ALWAYS; rs[D3DRENDERSTATE_FILLMODE] = D3DFILL_SOLID; rs[D3DRENDERSTATE_SHADEMODE] = D3DSHADE_GOURAUD;
        rs[D3DRENDERSTATE_TEXTUREFACTOR] = 0xffffffff; rs[D3DRENDERSTATE_FOGTABLEDENSITY] = 0x3f800000; rs[D3DRENDERSTATE_FOGTABLEEND] = 0x3f800000;
        for (int i = 0; i < 8; i++) {
            tss[i][D3DTSS_COLOROP] = i == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE; tss[i][D3DTSS_COLORARG1] = D3DTA_TEXTURE; tss[i][D3DTSS_COLORARG2] = D3DTA_CURRENT;
            tss[i][D3DTSS_ALPHAOP] = i == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE; tss[i][D3DTSS_ALPHAARG1] = D3DTA_TEXTURE; tss[i][D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
            tss[i][D3DTSS_TEXCOORDINDEX] = i; tss[i][D3DTSS_ADDRESS] = tss[i][D3DTSS_ADDRESSU] = tss[i][D3DTSS_ADDRESSV] = D3DTADDRESS_WRAP;
            tss[i][D3DTSS_MAGFILTER] = D3DTFG_POINT; tss[i][D3DTSS_MINFILTER] = D3DTFN_POINT; tss[i][D3DTSS_MIPFILTER] = D3DTFP_NONE;
        }
        // the backend starts from the same defaults
        static const DWORD init[] = {D3DRENDERSTATE_ZENABLE, D3DRENDERSTATE_ZWRITEENABLE, D3DRENDERSTATE_ZFUNC, D3DRENDERSTATE_SRCBLEND, D3DRENDERSTATE_DESTBLEND,
                                     D3DRENDERSTATE_CULLMODE, D3DRENDERSTATE_ALPHAFUNC, D3DRENDERSTATE_FILLMODE, D3DRENDERSTATE_TEXTUREFACTOR,
                                     D3DRENDERSTATE_FOGTABLEDENSITY, D3DRENDERSTATE_FOGTABLEEND};
        for (DWORD s : init) be::SetRenderState6(s, rs[s]);
        for (DWORD i = 0; i < 2; i++) for (DWORD t = 1; t < 22; t++) be::SetTextureStageState6(i, t, tss[i][t]);
    }
    ~Device() { for (auto& t : tex) if (t) t->Release(); for (auto v : viewports) v->Release(); if (g_device == this) g_device = nullptr; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** o) override {
        if (r == IID_IUnknown || r == IID_IDirect3DDevice3) { *o = this; AddRef(); return S_OK; }
        Surface::LogGuidOnce("Device3::QueryInterface unsupported", r);
        *o = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override { LONG n = InterlockedDecrement(&ref); if (!n) delete this; return n; }

    HRESULT STDMETHODCALLTYPE GetCaps(D3DDEVICEDESC* hal, D3DDEVICEDESC* hel) override {
        if (hal) FillDeviceDesc(hal);
        if (hel) { DWORD s = hel->dwSize ? hel->dwSize : sizeof(D3DDEVICEDESC); FillDeviceDesc(hel); (void)s; }
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStats(D3DSTATS* s) override { if (s) { DWORD z = s->dwSize; memset(s, 0, z); s->dwSize = z; } return D3D_OK; }
    HRESULT STDMETHODCALLTYPE AddViewport(IDirect3DViewport3* v) override {
        Viewport* vp = (Viewport*)v; if (!vp) return DDERR_INVALIDPARAMS;
        for (auto x : viewports) if (x == vp) return D3D_OK;
        vp->AddRef(); vp->dev = this; viewports.push(vp); return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE DeleteViewport(IDirect3DViewport3* v) override {
        for (int i = 0; i < viewports.size(); i++) if (viewports[i] == (Viewport*)v) { if (current == viewports[i]) current = nullptr; viewports[i]->dev = nullptr; viewports[i]->Release(); viewports.erase(i); return D3D_OK; }
        return DDERR_INVALIDPARAMS;
    }
    HRESULT STDMETHODCALLTYPE NextViewport(IDirect3DViewport3* ref_, IDirect3DViewport3** out, DWORD flags) override {
        if (!out) return DDERR_INVALIDPARAMS;
        *out = nullptr;
        if (!viewports.size()) return D3D_OK;
        if (flags == D3DNEXT_HEAD) *out = viewports[0];
        else if (flags == D3DNEXT_TAIL) *out = viewports.back();
        else for (int i = 0; i + 1 < viewports.size(); i++) if (viewports[i] == (Viewport*)ref_) *out = viewports[i + 1];
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumTextureFormats(LPD3DENUMPIXELFORMATSCALLBACK cb, void* ctx) override {
        if (!cb) return DDERR_INVALIDPARAMS;
        DDPIXELFORMAT pf;
        Fill16(&pf, 0xf800, 0x07e0, 0x001f, 0); if (cb(&pf, ctx) == D3DENUMRET_CANCEL) return D3D_OK;
        Fill16(&pf, 0x7c00, 0x03e0, 0x001f, 0x8000); if (cb(&pf, ctx) == D3DENUMRET_CANCEL) return D3D_OK;
        Fill16(&pf, 0x0f00, 0x00f0, 0x000f, 0xf000); if (cb(&pf, ctx) == D3DENUMRET_CANCEL) return D3D_OK;
        Fill16(&pf, 0x7c00, 0x03e0, 0x001f, 0); cb(&pf, ctx);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE BeginScene() override {
        if (Tracing()) { TraceLog("BeginScene"); be::TraceState(); }
        be::BeginScene(); return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE EndScene() override { if (Tracing()) TraceLog("end of 3D scene"); be::EndScene(); return D3D_OK; }
    HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D3** d) override { if (!d) return DDERR_INVALIDPARAMS; *d = d3d; d3d->AddRef(); return D3D_OK; }
    HRESULT STDMETHODCALLTYPE SetCurrentViewport(IDirect3DViewport3* v) override {
        current = (Viewport*)v;
        if (current) current->Apply();
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentViewport(IDirect3DViewport3** v) override { if (!v) return DDERR_INVALIDPARAMS; *v = current; if (current) current->AddRef(); return current ? D3D_OK : D3DERR_NOCURRENTVIEWPORT; }
    HRESULT STDMETHODCALLTYPE SetRenderTarget(IDirectDrawSurface4* s, DWORD) override {
        Surface* t = AsSurface(s);
        if (t && t->Screen()) target = t; else LogOnce("rtnonscreen", "note: render target set to a non-screen surface (ignored)");
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRenderTarget(IDirectDrawSurface4** s) override { if (!s) return DDERR_INVALIDPARAMS; *s = target; target->AddRef(); return D3D_OK; }
    HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE t, DWORD* v) override { if (!v) return DDERR_INVALIDPARAMS; *v = (DWORD)t < 256 ? rs[t] : 0; return D3D_OK; }
    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE t, DWORD v) override { if ((DWORD)t < 256) rs[t] = v; be::SetRenderState6(t, v); return D3D_OK; }
    HRESULT STDMETHODCALLTYPE GetLightState(D3DLIGHTSTATETYPE, DWORD* v) override { if (v) *v = 0; return D3D_OK; }
    HRESULT STDMETHODCALLTYPE SetLightState(D3DLIGHTSTATETYPE, DWORD) override { LogOnce("lightstate", "note: SetLightState ignored"); return D3D_OK; }
    HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE, D3DMATRIX*) override { LogOnce("settransform", "note: SetTransform ignored (game uses pre-transformed vertices)"); return D3D_OK; }
    HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE, D3DMATRIX* m) override { if (m) { memset(m, 0, sizeof(*m)); m->_11 = m->_22 = m->_33 = m->_44 = 1; } return D3D_OK; }
    HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE, D3DMATRIX*) override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE SetClipStatus(D3DCLIPSTATUS*) override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS* c) override { if (c) memset(c, 0, sizeof(*c)); return D3D_OK; }
    HRESULT STDMETHODCALLTYPE GetTexture(DWORD stage, IDirect3DTexture2** t) override {
        if (!t || stage >= 8) return DDERR_INVALIDPARAMS;
        *t = tex[stage] ? &tex[stage]->tex : nullptr; if (tex[stage]) tex[stage]->AddRef(); return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE SetTexture(DWORD stage, IDirect3DTexture2* t) override {
        if (stage >= 8) return DDERR_INVALIDPARAMS;
        Surface* s = t ? ((SurfTex*)t)->s : nullptr;
        if (s == tex[stage]) return D3D_OK;
        if (s) s->AddRef();
        if (tex[stage]) tex[stage]->Release();
        tex[stage] = s;
        if (Tracing()) {
            if (s) TraceLog("TEX %lu = %p %dx%d bpp %d amask %lx key %d(%lx)", stage, s, s->w, s->h, s->fmt.bpp, s->fmt.a, s->hasKey, s->keyLo);
            else TraceLog("TEX %lu = none", stage);
        }
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE t, DWORD* v) override {
        if (!v || stage >= 8 || (DWORD)t >= 32) return DDERR_INVALIDPARAMS;
        *v = tss[stage][t]; return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE t, DWORD v) override {
        if (stage >= 8 || (DWORD)t >= 32) return DDERR_INVALIDPARAMS;
        tss[stage][t] = v;
        be::SetTextureStageState6(stage, t, v);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD* passes) override { if (passes) *passes = 1; return D3D_OK; }

    void BindTextures() {
        bool keyOn = rs[D3DRENDERSTATE_COLORKEYENABLE] != 0;
        for (int i = 0; i < 2; i++) {
            Surface* s = tex[i];
            be::Tex* t = s ? s->Gpu(keyOn, true) : nullptr;
            be::SetTexture(i, t, s && keyOn && s->hasKey);
        }
    }
    void TraceDraw(const char* what, D3DPRIMITIVETYPE pt, DWORD fvf, const void* v, DWORD n, const WORD* idx, DWORD ni, DWORD flags) {
        if (!Tracing()) return;
        if (!n) { TraceLog("%s prim %d fvf %lx n %lu (empty)", what, pt, fvf, n); return; }
        const float* f = (const float*)v;
        int stride = 0;
        { uint32_t pos = fvf & D3DFVF_POSITION_MASK; stride = (pos == D3DFVF_XYZRHW ? 16 : 12) + ((fvf & D3DFVF_DIFFUSE) ? 4 : 0) + ((fvf & D3DFVF_SPECULAR) ? 4 : 0) + ((fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT) * 8 + ((fvf & D3DFVF_NORMAL) ? 12 : 0); }
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f, z0 = 1e9f, z1 = -1e9f, w0 = 1e9f, w1 = -1e9f;
        for (DWORD i = 0; i < n; i++) {
            const float* p = (const float*)((const uint8_t*)f + (size_t)i * stride);
            if (p[0] < x0) x0 = p[0]; if (p[0] > x1) x1 = p[0]; if (p[1] < y0) y0 = p[1]; if (p[1] > y1) y1 = p[1];
            if (p[2] < z0) z0 = p[2]; if (p[2] > z1) z1 = p[2]; if (p[3] < w0) w0 = p[3]; if (p[3] > w1) w1 = p[3];
        }
        TraceLog("%s prim %d fvf %lx verts %lu idx %lu flags %lx  x %.1f..%.1f y %.1f..%.1f z %.5f..%.5f rhw %.4f..%.4f", what, pt, fvf, n, ni, flags, x0, x1, y0, y1, z0, z1, w0, w1);
        (void)idx;
    }
    HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE pt, DWORD fvf, void* v, DWORD n, DWORD flags) override {
        TraceDraw("DP", pt, fvf, v, n, nullptr, 0, flags);
        BindTextures();
        g_stats.draws++;
        be::DrawPrimitiveTL(pt, fvf, v, n);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE pt, DWORD fvf, void* v, DWORD n, WORD* idx, DWORD ni, DWORD flags) override {
        TraceDraw("DIP", pt, fvf, v, n, idx, ni, flags);
        BindTextures();
        g_stats.draws++;
        be::DrawIndexedPrimitiveTL(pt, fvf, v, n, idx, ni);
        return D3D_OK;
    }
};

void Viewport::Apply() {
    if (!dev || dev->current != this) return;
    be::SetViewport((int)vp.dwX, (int)vp.dwY, (int)vp.dwWidth, (int)vp.dwHeight, vp.dvMinZ, vp.dvMaxZ);
}

struct D3D : IDirect3D3Stub {
    DDraw* dd;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** o) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE EnumDevices(LPD3DENUMDEVICESCALLBACK cb, void* ctx) override {
        if (!cb) return DDERR_INVALIDPARAMS;
        D3DDEVICEDESC hal = {}, hel = {};
        hal.dwSize = hel.dwSize = sizeof(D3DDEVICEDESC);
        FillDeviceDesc(&hal);
        GUID g = IID_IDirect3DHALDevice;
        cb(&g, (char*)"Direct3D HAL (swat3d11)", (char*)"Direct3D HAL", &hal, &hel, ctx);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE CreateLight(IDirect3DLight**, IUnknown*) override { Unimpl("IDirect3D3::CreateLight"); return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateViewport(IDirect3DViewport3** out, IUnknown*) override {
        if (!out) return DDERR_INVALIDPARAMS;
        *out = new Viewport(); return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE CreateDevice(REFCLSID, IDirectDrawSurface4* s, IDirect3DDevice3** out, IUnknown*) override {
        if (!out) return DDERR_INVALIDPARAMS;
        Surface* t = AsSurface(s);
        if (!t) return DDERR_INVALIDPARAMS;
        Log("CreateDevice on %s surface %dx%d", t->Screen() ? "back buffer" : KindName(t->kind), t->w, t->h);
        Device* d = new Device(t, this);
        g_device = d;
        *out = d; return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumZBufferFormats(REFCLSID, LPD3DENUMPIXELFORMATSCALLBACK cb, void* ctx) override {
        if (!cb) return DDERR_INVALIDPARAMS;
        DDPIXELFORMAT pf = {};
        pf.dwSize = sizeof(pf); pf.dwFlags = DDPF_ZBUFFER;
        pf.dwZBufferBitDepth = 16; pf.dwZBitMask = 0xffff;
        if (cb(&pf, ctx) == D3DENUMRET_CANCEL) return D3D_OK;
        pf.dwZBufferBitDepth = 24; pf.dwZBitMask = 0xffffff;
        if (cb(&pf, ctx) == D3DENUMRET_CANCEL) return D3D_OK;
        pf.dwZBufferBitDepth = 32; pf.dwZBitMask = 0xffffff00;
        cb(&pf, ctx);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE EvictManagedTextures() override { return D3D_OK; }
};

// ====================================================================================================
// DirectDraw
// ====================================================================================================
struct DDFace1; struct DDFace2; struct DDFace7;
struct DDraw : IDirectDraw4Stub {
    LONG ref = 1;
    HWND hwnd = nullptr;
    int modeW = 0, modeH = 0;
    D3D d3d;
    DDFace1* f1; DDFace2* f2; DDFace7* f7;
    DDraw();

    void EnsureBackend() {
        if (be::Ready()) return;
        HWND h = hwnd ? hwnd : HooksWindow();
        static bool told = false;
        if (!h) { if (!told) { told = true; Log("note: drawing requested before the game gave its window"); } return; }
        if (be::Init(h) && modeW > 0) be::SetVirtualMode(modeW, modeH);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** o) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG n = InterlockedDecrement(&ref);
        if (n == 0) { Log("DirectDraw object released"); ref = 1; return 0; }   // kept: surfaces may still refer to it
        return n;
    }
    HRESULT STDMETHODCALLTYPE Compact() override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE CreateClipper(DWORD, IDirectDrawClipper** out, IUnknown*) override { if (!out) return DDERR_INVALIDPARAMS; *out = new Clipper(); return DD_OK; }
    HRESULT STDMETHODCALLTYPE CreateSurface(DDSURFACEDESC2* d, IDirectDrawSurface4** out, IUnknown*) override {
        if (!d || !out) return DDERR_INVALIDPARAMS;
        Surface* s = Create(d);
        *out = s;
        return s ? DD_OK : DDERR_GENERIC;
    }
    Surface* Create(DDSURFACEDESC2* d);
    HRESULT STDMETHODCALLTYPE EnumDisplayModes(DWORD f, DDSURFACEDESC2* filter, void* ctx, LPDDENUMMODESCALLBACK2 cb) override;
    HRESULT STDMETHODCALLTYPE FlipToGDISurface() override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetCaps(DDCAPS* hal, DDCAPS* hel) override {
        if (hal) FillCaps(hal);
        if (hel) FillCaps(hel);
        return DD_OK;
    }
    static void FillCaps(DDCAPS* c) {
        DWORD size = c->dwSize ? c->dwSize : sizeof(DDCAPS);
        DDCAPS x = {};
        x.dwSize = size;
        x.dwCaps = DDCAPS_3D | DDCAPS_BLT | DDCAPS_BLTCOLORFILL | DDCAPS_BLTDEPTHFILL | DDCAPS_BLTSTRETCH | DDCAPS_COLORKEY | DDCAPS_CANBLTSYSMEM | DDCAPS_GDI | DDCAPS_ZBLTS;
        x.dwCaps2 = DDCAPS2_WIDESURFACES | DDCAPS2_NOPAGELOCKREQUIRED | DDCAPS2_CANRENDERWINDOWED | DDCAPS2_FLIPNOVSYNC;
        x.dwCKeyCaps = DDCKEYCAPS_SRCBLT;
        x.dwFXCaps = DDFXCAPS_BLTSTRETCHX | DDFXCAPS_BLTSTRETCHY | DDFXCAPS_BLTSHRINKX | DDFXCAPS_BLTSHRINKY;
        x.dwVidMemTotal = 256u << 20; x.dwVidMemFree = 240u << 20;
        x.ddsCaps.dwCaps = DDSCAPS_3DDEVICE | DDSCAPS_BACKBUFFER | DDSCAPS_COMPLEX | DDSCAPS_FLIP | DDSCAPS_FRONTBUFFER | DDSCAPS_OFFSCREENPLAIN |
                           DDSCAPS_PRIMARYSURFACE | DDSCAPS_SYSTEMMEMORY | DDSCAPS_TEXTURE | DDSCAPS_VIDEOMEMORY | DDSCAPS_ZBUFFER | DDSCAPS_MIPMAP;
        memcpy(c, &x, size < sizeof(x) ? size : sizeof(x));
        c->dwSize = size;
    }
    HRESULT STDMETHODCALLTYPE GetDisplayMode(DDSURFACEDESC2* d) override {
        if (!d) return DDERR_INVALIDPARAMS;
        DWORD size = d->dwSize ? d->dwSize : sizeof(*d);
        memset(d, 0, size); d->dwSize = size;
        int w = modeW, h = modeH;
        if (w <= 0) { w = GetSystemMetrics(SM_CXSCREEN); h = GetSystemMetrics(SM_CYSCREEN); }
        d->dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
        d->dwWidth = w; d->dwHeight = h; d->lPitch = w * 2; d->dwRefreshRate = 60;
        PFFromFmt(kFmt565, d->ddpfPixelFormat);
        return DD_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFourCCCodes(DWORD* n, DWORD*) override { if (n) *n = 0; return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetGDISurface(IDirectDrawSurface4** s) override { if (!s) return DDERR_INVALIDPARAMS; *s = g_primary; if (g_primary) g_primary->AddRef(); return g_primary ? DD_OK : DDERR_NOTFOUND; }
    HRESULT STDMETHODCALLTYPE GetMonitorFrequency(DWORD* f) override { if (f) *f = 60; return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetScanLine(DWORD* l) override { if (l) *l = 0; return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetVerticalBlankStatus(WINBOOL* b) override { if (b) *b = TRUE; return DD_OK; }
    HRESULT STDMETHODCALLTYPE Initialize(GUID*) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE RestoreDisplayMode() override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND h, DWORD f) override {
        Log("SetCooperativeLevel(hwnd=%p, flags=%08lx)", h, f);
        if (h) { hwnd = h; HooksInstall(h); }
        return DD_OK;
    }
    HRESULT STDMETHODCALLTYPE SetDisplayMode(DWORD w, DWORD h, DWORD bpp, DWORD, DWORD) override {
        Log("SetDisplayMode(%lu x %lu x %lu)", w, h, bpp);
        modeW = (int)w; modeH = (int)h;
        EnsureBackend();
        be::SetVirtualMode(modeW, modeH);
        return DD_OK;
    }
    HRESULT STDMETHODCALLTYPE WaitForVerticalBlank(DWORD, HANDLE) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetAvailableVidMem(DDSCAPS2*, DWORD* total, DWORD* fr) override { if (total) *total = 256u << 20; if (fr) *fr = 240u << 20; return DD_OK; }
    HRESULT STDMETHODCALLTYPE RestoreAllSurfaces() override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetDeviceIdentifier(DDDEVICEIDENTIFIER* id, DWORD) override {
        if (!id) return DDERR_INVALIDPARAMS;
        memset(id, 0, sizeof(*id));
        strcpy(id->szDriver, "swat3d11"); strcpy(id->szDescription, "SWAT 3 Direct3D 11 renderer");
        return DD_OK;
    }
};

HRESULT Surface::GetDDInterface(void** dd) { if (!dd) return DDERR_INVALIDPARAMS; *dd = g_dd; g_dd->AddRef(); return DD_OK; }
HRESULT D3D::QueryInterface(REFIID r, void** o) { return dd->QueryInterface(r, o); }
ULONG D3D::AddRef() { return dd->AddRef(); }
ULONG D3D::Release() { return dd->Release(); }

// ---- display modes ------------------------------------------------------------------------------
struct WH { int w, h; };
static void ModeList(Vec<WH>& out) {
    static const WH base[] = {{640, 480}, {800, 600}, {1024, 768}, {1152, 864}, {1280, 720}, {1280, 800}, {1280, 960}, {1280, 1024}, {1360, 768},
                              {1366, 768}, {1440, 900}, {1600, 900}, {1600, 1200}, {1680, 1050}, {1920, 1080}, {1920, 1200}, {2560, 1080}, {2560, 1440},
                              {2560, 1600}, {3440, 1440}, {3840, 2160}};
    static const WH extra[] = {{1067, 600}, {960, 600}, {1400, 600}, {1138, 640}, {1024, 640}, {1200, 675}, {1152, 720}, {1229, 768}, {1365, 768}};
    out.clear();
    for (auto& m : base) out.push(m);
    if (g_cfg.extraResolutions) for (auto& m : extra) out.push(m);
    WH desk = {GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    out.push(desk);
    // sort, remove duplicates
    for (int i = 0; i < out.size(); i++) for (int j = i + 1; j < out.size(); j++)
        if (out[j].w < out[i].w || (out[j].w == out[i].w && out[j].h < out[i].h)) { WH t = out[i]; out[i] = out[j]; out[j] = t; }
    for (int i = 1; i < out.size();) if (out[i].w == out[i - 1].w && out[i].h == out[i - 1].h) out.erase(i); else i++;
}
HRESULT DDraw::EnumDisplayModes(DWORD, DDSURFACEDESC2* filter, void* ctx, LPDDENUMMODESCALLBACK2 cb) {
    if (!cb) return DDERR_INVALIDPARAMS;
    Vec<WH> modes; ModeList(modes);
    for (auto& m : modes) {
        if (filter && (filter->dwFlags & DDSD_WIDTH) && filter->dwWidth != (DWORD)m.w) continue;
        if (filter && (filter->dwFlags & DDSD_HEIGHT) && filter->dwHeight != (DWORD)m.h) continue;
        if (filter && (filter->dwFlags & DDSD_PIXELFORMAT) && filter->ddpfPixelFormat.dwRGBBitCount && filter->ddpfPixelFormat.dwRGBBitCount != 16) continue;
        DDSURFACEDESC2 d = {};
        d.dwSize = sizeof(d);
        d.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
        d.dwWidth = m.w; d.dwHeight = m.h; d.lPitch = m.w * 2; d.dwRefreshRate = 60;
        PFFromFmt(kFmt565, d.ddpfPixelFormat);
        if (cb(&d, ctx) == DDENUMRET_CANCEL) break;
    }
    return DD_OK;
}

// ---- surface creation -----------------------------------------------------------------------------
Surface* DDraw::Create(DDSURFACEDESC2* d) {
    DWORD caps = (d->dwFlags & DDSD_CAPS) ? d->ddsCaps.dwCaps : 0;
    int w = (d->dwFlags & DDSD_WIDTH) ? (int)d->dwWidth : 0, h = (d->dwFlags & DDSD_HEIGHT) ? (int)d->dwHeight : 0;
    Fmt fmt = (d->dwFlags & DDSD_PIXELFORMAT) ? FmtFromPF(d->ddpfPixelFormat) : kFmt565;
    Surface* s = nullptr;
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        EnsureBackend();
        int vw = modeW, vh = modeH;
        if (vw <= 0) be::GetVirtualMode(&vw, &vh);
        bool chain = (d->dwFlags & DDSD_BACKBUFFERCOUNT) && d->dwBackBufferCount > 0;
        Log("CreateSurface: primary %dx%d%s", vw, vh, chain ? " with back buffer" : "");
        s = new Surface(K_PRIMARY, vw, vh, kFmt565);
        s->caps.dwCaps = caps | DDSCAPS_FRONTBUFFER | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM | DDSCAPS_VISIBLE;
        if (g_primary) LogOnce("2prim", "note: a second primary surface was created");
        g_primary = s;
        if (chain) {
            Surface* b = new Surface(K_BACK, vw, vh, kFmt565);
            b->caps.dwCaps = DDSCAPS_BACKBUFFER | DDSCAPS_FLIP | DDSCAPS_COMPLEX | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM | (caps & DDSCAPS_3DDEVICE);
            b->owner = s;
            s->attached = b;
            g_back = b;
        }
        if (be::Ready() && vw > 0) be::SetVirtualMode(vw, vh);
    } else if (caps & DDSCAPS_ZBUFFER) {
        int bits = (d->dwFlags & DDSD_PIXELFORMAT) ? (int)d->ddpfPixelFormat.dwZBufferBitDepth : 16;
        if (bits != 16 && bits != 24 && bits != 32) bits = 16;
        Log("CreateSurface: z-buffer %dx%d %d-bit", w, h, bits);
        Fmt zf = {bits == 24 ? 32 : bits, 0, 0, 0, 0};
        s = new Surface(K_ZBUF, w, h, zf);
        s->caps.dwCaps = caps | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM;
    } else if ((caps & DDSCAPS_3DDEVICE) && !(caps & DDSCAPS_TEXTURE)) {
        int vw = modeW, vh = modeH;
        if (vw <= 0) be::GetVirtualMode(&vw, &vh);
        if (w != vw || h != vh) Log("note: 3D render surface %dx%d differs from mode %dx%d - using surface size", w, h, vw, vh);
        Log("CreateSurface: 3D back buffer %dx%d", w, h);
        EnsureBackend();
        s = new Surface(K_BACK, w > 0 ? w : vw, h > 0 ? h : vh, kFmt565);
        s->caps.dwCaps = caps | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM;
        g_back = s;
        if (be::Ready() && w > 0 && h > 0) be::SetVirtualMode(w, h);
    } else {
        Kind k = (caps & DDSCAPS_TEXTURE) ? K_TEX : K_PLAIN;
        if (w <= 0 || h <= 0) { Log("CreateSurface: %s without size (caps %08lx) refused", KindName(k), caps); return nullptr; }
        if (g_cfg.logLevel >= 2) Log("CreateSurface: %s %dx%d %d-bit", KindName(k), w, h, fmt.bpp);
        s = new Surface(k, w, h, fmt);
        s->caps.dwCaps = caps | ((caps & DDSCAPS_SYSTEMMEMORY) ? 0 : DDSCAPS_VIDEOMEMORY);
        if ((d->dwFlags & DDSD_MIPMAPCOUNT) && d->dwMipMapCount > 1) LogOnce("mipchain", "note: mipmapped texture chain requested (only the top level is kept)");
    }
    s->caps.dwCaps2 = d->ddsCaps.dwCaps2;
    if (d->dwFlags & DDSD_CKSRCBLT) {
        s->hasKey = true; s->keyLo = d->ddckCKSrcBlt.dwColorSpaceLowValue; s->keyHi = d->ddckCKSrcBlt.dwColorSpaceHighValue;
        if (s->keyHi < s->keyLo) s->keyHi = s->keyLo;
    }
    return s;
}

// ---- older DirectDraw interfaces ----------------------------------------------------------------------
struct DDFace1 : IDirectDrawStub {
    DDraw* d;
    FWD_REF(d)
    HRESULT STDMETHODCALLTYPE Compact() override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE CreateClipper(DWORD f, IDirectDrawClipper** c, IUnknown* u) override { return d->CreateClipper(f, c, u); }
    HRESULT STDMETHODCALLTYPE CreateSurface(DDSURFACEDESC* sd, IDirectDrawSurface** out, IUnknown*) override {
        if (!sd || !out) return DDERR_INVALIDPARAMS;
        DDSURFACEDESC2 d2; Desc1To2(sd, &d2);
        Log("CreateSurface (DirectDraw v1/v2 caller): %lux%lu caps %08lx", d2.dwWidth, d2.dwHeight, d2.ddsCaps.dwCaps);
        Surface* s = d->Create(&d2);
        *out = s ? (IDirectDrawSurface*)&s->f3 : nullptr;
        return s ? DD_OK : DDERR_GENERIC;
    }
    HRESULT STDMETHODCALLTYPE EnumDisplayModes(DWORD f, DDSURFACEDESC* filter, void* ctx, LPDDENUMMODESCALLBACK cb) override {
        struct C { void* ctx; LPDDENUMMODESCALLBACK cb; } c = {ctx, cb};
        DDSURFACEDESC2 f2; if (filter) Desc1To2(filter, &f2);
        return d->EnumDisplayModes(f, filter ? &f2 : nullptr, &c, Thunk);
    }
    static HRESULT WINAPI Thunk(DDSURFACEDESC2* m, void* p) {
        struct C { void* ctx; LPDDENUMMODESCALLBACK cb; };
        C* c = (C*)p; DDSURFACEDESC m1 = {}; m1.dwSize = sizeof(m1); Desc2To1(m, &m1); return c->cb(&m1, c->ctx);
    }
    HRESULT STDMETHODCALLTYPE FlipToGDISurface() override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetCaps(DDCAPS* a, DDCAPS* b) override { return d->GetCaps(a, b); }
    HRESULT STDMETHODCALLTYPE GetDisplayMode(DDSURFACEDESC* sd) override {
        if (!sd) return DDERR_INVALIDPARAMS;
        DDSURFACEDESC2 d2 = {}; d2.dwSize = sizeof(d2); d->GetDisplayMode(&d2); Desc2To1(&d2, sd); return DD_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMonitorFrequency(DWORD* f) override { return d->GetMonitorFrequency(f); }
    HRESULT STDMETHODCALLTYPE GetScanLine(DWORD* l) override { return d->GetScanLine(l); }
    HRESULT STDMETHODCALLTYPE GetVerticalBlankStatus(WINBOOL* b) override { return d->GetVerticalBlankStatus(b); }
    HRESULT STDMETHODCALLTYPE Initialize(GUID*) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE RestoreDisplayMode() override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND h, DWORD f) override {
        Log("SetCooperativeLevel from old-interface caller (hwnd=%p, flags=%08lx) ignored", h, f);
        if (h && !d->hwnd) { d->hwnd = h; HooksInstall(h); }
        return DD_OK;
    }
    HRESULT STDMETHODCALLTYPE SetDisplayMode(DWORD w, DWORD h, DWORD) override { Log("SetDisplayMode %lux%lu from old-interface caller ignored", w, h); return DD_OK; }
    HRESULT STDMETHODCALLTYPE WaitForVerticalBlank(DWORD, HANDLE) override { return DD_OK; }
};
struct DDFace2 : IDirectDraw2Stub {
    DDraw* d;
    FWD_REF(d)
    HRESULT STDMETHODCALLTYPE Compact() override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE CreateClipper(DWORD f, IDirectDrawClipper** c, IUnknown* u) override { return d->CreateClipper(f, c, u); }
    HRESULT STDMETHODCALLTYPE CreateSurface(DDSURFACEDESC* sd, IDirectDrawSurface** out, IUnknown* u) override { return d->f1->CreateSurface(sd, out, u); }
    HRESULT STDMETHODCALLTYPE EnumDisplayModes(DWORD f, DDSURFACEDESC* filter, void* ctx, LPDDENUMMODESCALLBACK cb) override { return d->f1->EnumDisplayModes(f, filter, ctx, cb); }
    HRESULT STDMETHODCALLTYPE FlipToGDISurface() override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetCaps(DDCAPS* a, DDCAPS* b) override { return d->GetCaps(a, b); }
    HRESULT STDMETHODCALLTYPE GetDisplayMode(DDSURFACEDESC* sd) override { return d->f1->GetDisplayMode(sd); }
    HRESULT STDMETHODCALLTYPE GetMonitorFrequency(DWORD* f) override { return d->GetMonitorFrequency(f); }
    HRESULT STDMETHODCALLTYPE GetScanLine(DWORD* l) override { return d->GetScanLine(l); }
    HRESULT STDMETHODCALLTYPE GetVerticalBlankStatus(WINBOOL* b) override { return d->GetVerticalBlankStatus(b); }
    HRESULT STDMETHODCALLTYPE Initialize(GUID*) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE RestoreDisplayMode() override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND h, DWORD f) override { return d->f1->SetCooperativeLevel(h, f); }
    HRESULT STDMETHODCALLTYPE SetDisplayMode(DWORD w, DWORD h, DWORD, DWORD, DWORD) override { Log("SetDisplayMode %lux%lu from old-interface caller ignored", w, h); return DD_OK; }
    HRESULT STDMETHODCALLTYPE WaitForVerticalBlank(DWORD, HANDLE) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetAvailableVidMem(DDSCAPS*, DWORD* t, DWORD* f) override { return d->GetAvailableVidMem(nullptr, t, f); }
};
struct DDFace7 : IDirectDraw7Stub {
    DDraw* d;
    FWD_REF(d)
    HRESULT STDMETHODCALLTYPE Compact() override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE CreateClipper(DWORD f, IDirectDrawClipper** c, IUnknown* u) override { return d->CreateClipper(f, c, u); }
    HRESULT STDMETHODCALLTYPE CreateSurface(DDSURFACEDESC2* sd, IDirectDrawSurface7** out, IUnknown*) override {
        if (!sd || !out) return DDERR_INVALIDPARAMS;
        Surface* s = d->Create(sd);
        *out = s ? (IDirectDrawSurface7*)&s->f7 : nullptr;
        return s ? DD_OK : DDERR_GENERIC;
    }
    HRESULT STDMETHODCALLTYPE EnumDisplayModes(DWORD f, DDSURFACEDESC2* filter, void* ctx, LPDDENUMMODESCALLBACK2 cb) override { return d->EnumDisplayModes(f, filter, ctx, cb); }
    HRESULT STDMETHODCALLTYPE FlipToGDISurface() override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetCaps(DDCAPS* a, DDCAPS* b) override { return d->GetCaps(a, b); }
    HRESULT STDMETHODCALLTYPE GetDisplayMode(DDSURFACEDESC2* sd) override { return d->GetDisplayMode(sd); }
    HRESULT STDMETHODCALLTYPE GetMonitorFrequency(DWORD* f) override { return d->GetMonitorFrequency(f); }
    HRESULT STDMETHODCALLTYPE GetScanLine(DWORD* l) override { return d->GetScanLine(l); }
    HRESULT STDMETHODCALLTYPE GetVerticalBlankStatus(WINBOOL* b) override { return d->GetVerticalBlankStatus(b); }
    HRESULT STDMETHODCALLTYPE Initialize(GUID*) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE RestoreDisplayMode() override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND h, DWORD f) override { Log("SetCooperativeLevel from DirectDraw7 caller (hwnd=%p, flags=%08lx) ignored", h, f); return DD_OK; }
    HRESULT STDMETHODCALLTYPE SetDisplayMode(DWORD w, DWORD h, DWORD, DWORD, DWORD) override { Log("SetDisplayMode %lux%lu from DirectDraw7 caller ignored", w, h); return DD_OK; }
    HRESULT STDMETHODCALLTYPE WaitForVerticalBlank(DWORD, HANDLE) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetAvailableVidMem(DDSCAPS2* c, DWORD* t, DWORD* f) override { return d->GetAvailableVidMem(c, t, f); }
    HRESULT STDMETHODCALLTYPE RestoreAllSurfaces() override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetDeviceIdentifier(DDDEVICEIDENTIFIER2* id, DWORD) override {
        if (!id) return DDERR_INVALIDPARAMS;
        memset(id, 0, sizeof(*id));
        strcpy(id->szDriver, "swat3d11"); strcpy(id->szDescription, "SWAT 3 Direct3D 11 renderer");
        return DD_OK;
    }
};

DDraw::DDraw() {
    d3d.dd = this;
    f1 = new DDFace1(); f1->d = this;
    f2 = new DDFace2(); f2->d = this;
    f7 = new DDFace7(); f7->d = this;
}
HRESULT DDraw::QueryInterface(REFIID r, void** o) {
    if (!o) return E_POINTER;
    if (r == IID_IUnknown || r == IID_IDirectDraw) { *o = f1; AddRef(); return S_OK; }
    if (r == IID_IDirectDraw2) { *o = f2; AddRef(); return S_OK; }
    if (r == IID_IDirectDraw4) { *o = static_cast<IDirectDraw4*>(this); AddRef(); return S_OK; }
    if (r == IID_IDirectDraw7) { *o = f7; AddRef(); return S_OK; }
    if (r == IID_IDirect3D3) { *o = &d3d; AddRef(); return S_OK; }
    Surface::LogGuidOnce("DirectDraw::QueryInterface unsupported", r);
    *o = nullptr; return E_NOINTERFACE;
}

// ====================================================================================================
// Entry points
// ====================================================================================================
HRESULT FrontCreateDirectDraw(GUID* guid, void** out) {
    if (!out) return DDERR_INVALIDPARAMS;
    if (guid && guid != (GUID*)DDCREATE_EMULATIONONLY && guid != (GUID*)DDCREATE_HARDWAREONLY) LogOnce("ddguid", "note: DirectDrawCreate with a specific device GUID (ignored)");
    if (!g_dd) g_dd = new DDraw(); else g_dd->AddRef();
    *out = g_dd->f1;
    Log("DirectDrawCreate -> %p", *out);
    return DD_OK;
}
