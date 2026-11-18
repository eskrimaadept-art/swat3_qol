// Direct3D 11 backend. One vertex layout (DirectX 6 screen-space vertices), one vertex shader and
// one "uber" pixel shader that emulates the two texture stages, alpha test, specular and fog the
// game uses, driven by a constant buffer. Pipeline objects (blend, depth, raster, sampler states)
// are cached. The picture is rendered at output resolution into its own render target (optionally
// multisampled), then copied to the swap chain with black bars where the aspect ratio differs.
#define INITGUID
#include "backend.h"
#define D3DCOLORVALUE_DEFINED          // already defined by the Direct3D 6 headers
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdio.h>
#include "shader_src.h"
#include "shader_blob.h"

namespace be {

struct Tex { ID3D11Texture2D* tex; ID3D11ShaderResourceView* srv; int w, h; bool mips; };

// ---- lock: the game thread and the window thread may both call in ------------------------------
static CritSec& Lk() { static CritSec* c = new CritSec(); return *c; }
#define LOCKED Guard _g(Lk())

// ---- device objects ---------------------------------------------------------------------------
static HWND g_hwnd;
static bool g_ready = false, g_initFailed = false;
static ID3D11Device* g_dev;
static ID3D11DeviceContext* g_ctx;
static IDXGISwapChain* g_swap;
static bool g_flipModel = false;
static ID3D11RenderTargetView* g_bbRTV;          // swap chain buffer
static int g_outW, g_outH;                       // swap chain size
static int g_msaa = 1;

static ID3D11Texture2D* g_rt;                    // the game's picture, at output scale
static ID3D11RenderTargetView* g_rtv;
static ID3D11Texture2D* g_rtResolve;             // non-MSAA copy (== g_rt without MSAA)
static ID3D11ShaderResourceView* g_rtSRV;
static ID3D11Texture2D* g_ds;
static ID3D11DepthStencilView* g_dsv;
static ID3D11Texture2D* g_staging;               // CPU read-back
static int g_rtW, g_rtH;
static bool g_rtDirty = true;                    // MSAA target changed since the last resolve

static int g_vw = 640, g_vh = 480;               // the game's resolution
static double g_scale = 1, g_offX = 0, g_offY = 0;

static ID3D11VertexShader* g_vs;
static ID3D11PixelShader* g_ps;
static ID3D11InputLayout* g_layout;
static ID3D11Buffer* g_cb;
static ID3D11Buffer* g_vb; static UINT g_vbSize, g_vbPos;
static ID3D11Buffer* g_ib; static UINT g_ibSize, g_ibPos;
static void (*g_presentCb)();

struct Vtx { float x, y, z, w; uint32_t dif, spec; float u0, v0, u1, v1; };

// ---- Direct3D 6 state as the game set it -------------------------------------------------------
struct Stage {
    uint32_t colorop = D3DTOP_DISABLE, carg1 = D3DTA_TEXTURE, carg2 = D3DTA_CURRENT;
    uint32_t alphaop = D3DTOP_DISABLE, aarg1 = D3DTA_TEXTURE, aarg2 = D3DTA_CURRENT;
    uint32_t texcoord = 0, addrU = D3DTADDRESS_WRAP, addrV = D3DTADDRESS_WRAP;
    uint32_t mag = D3DTFG_POINT, min = D3DTFN_POINT, mip = D3DTFP_NONE;
    Tex* tex = nullptr; bool keyed = false;
};
struct GameState {
    uint32_t zenable = 1, zwrite = 1, zfunc = D3DCMP_LESSEQUAL;
    uint32_t blend = 0, src = D3DBLEND_ONE, dst = D3DBLEND_ZERO;
    uint32_t atest = 0, afunc = D3DCMP_ALWAYS, aref = 0;
    uint32_t colorkey = 0, cull = D3DCULL_CCW, fill = D3DFILL_SOLID, zbias = 0;
    uint32_t fog = 0, fogmode = 0, fogcolor = 0; float fogstart = 0, fogend = 1, fogdensity = 1;
    uint32_t specular = 0, tfactor = 0xffffffff;
    Stage st[2];
    int vpX = 0, vpY = 0, vpW = 640, vpH = 480;
};
static GameState g_gs;

// ---- pipeline description and caches ------------------------------------------------------------
struct CB {                                       // must match cbuffer Frame in shader_src.h
    float vp[4], fogColor[4], fogParams[4], tfactor[4];
    int s0[4], s0b[4], s1[4], s1b[4], misc[4];
};
struct Smp { uint8_t addrU, addrV, filter, aniso; };   // filter: 0 point, 1 linear, 2 linear+mips, 3 aniso
struct Pipe {
    uint8_t blend, src, dst, zenable, zwrite, zfunc, cull, fill; int bias;
    Smp smp[2];
    CB cb;
    ID3D11ShaderResourceView* srv[2];
};

template <class K, class V> struct Cache {
    struct E { K k; V* v; }; Vec<E> e;
    V* find(const K& k) { for (auto& x : e) if (!memcmp(&x.k, &k, sizeof(K))) return x.v; return nullptr; }
    void add(const K& k, V* v) { e.push({k, v}); }
    void clear() { for (auto& x : e) x.v->Release(); e.clear(); }
};
struct BlendKey { uint8_t on, src, dst, premul; };
struct DepthKey { uint8_t en, write, func, pad; };
struct RastKey { uint8_t cull, fill, pad[2]; int bias; };
static Cache<BlendKey, ID3D11BlendState> g_blendCache;
static Cache<DepthKey, ID3D11DepthStencilState> g_depthCache;
static Cache<RastKey, ID3D11RasterizerState> g_rastCache;
static Cache<Smp, ID3D11SamplerState> g_smpCache;
static CB g_lastCB; static bool g_cbValid = false;

static D3D11_BLEND MapBlend(uint32_t b, bool alpha) {
    switch (b) {
    case D3DBLEND_ZERO: return D3D11_BLEND_ZERO;
    case D3DBLEND_ONE: return D3D11_BLEND_ONE;
    case D3DBLEND_SRCCOLOR: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case D3DBLEND_INVSRCCOLOR: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case D3DBLEND_SRCALPHA: return D3D11_BLEND_SRC_ALPHA;
    case D3DBLEND_INVSRCALPHA: return D3D11_BLEND_INV_SRC_ALPHA;
    case D3DBLEND_DESTALPHA: return D3D11_BLEND_DEST_ALPHA;
    case D3DBLEND_INVDESTALPHA: return D3D11_BLEND_INV_DEST_ALPHA;
    case D3DBLEND_DESTCOLOR: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case D3DBLEND_INVDESTCOLOR: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case D3DBLEND_SRCALPHASAT: return alpha ? D3D11_BLEND_ONE : D3D11_BLEND_SRC_ALPHA_SAT;
    default: return D3D11_BLEND_ONE;
    }
}

static ID3D11BlendState* GetBlend(BlendKey k) {
    if (ID3D11BlendState* s = g_blendCache.find(k)) return s;
    D3D11_BLEND_DESC d = {};
    D3D11_RENDER_TARGET_BLEND_DESC& r = d.RenderTarget[0];
    r.BlendEnable = k.on;
    uint32_t src = k.src, dst = k.dst;
    if (src == D3DBLEND_BOTHSRCALPHA) { src = D3DBLEND_SRCALPHA; dst = D3DBLEND_INVSRCALPHA; }
    if (src == D3DBLEND_BOTHINVSRCALPHA) { src = D3DBLEND_INVSRCALPHA; dst = D3DBLEND_SRCALPHA; }
    if (k.premul) { r.SrcBlend = D3D11_BLEND_ONE; r.DestBlend = D3D11_BLEND_INV_SRC_ALPHA; }
    else { r.SrcBlend = MapBlend(src, false); r.DestBlend = MapBlend(dst, false); }
    r.BlendOp = D3D11_BLEND_OP_ADD;
    r.SrcBlendAlpha = D3D11_BLEND_ONE; r.DestBlendAlpha = D3D11_BLEND_ZERO; r.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    // destination alpha stays 1 (the game never reads it back; 1999 cards had no alpha in 16-bit)
    r.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    ID3D11BlendState* s = nullptr;
    if (FAILED(g_dev->CreateBlendState(&d, &s))) { LogOnce("blendfail", "CreateBlendState failed (src %u dst %u)", k.src, k.dst); return nullptr; }
    g_blendCache.add(k, s);
    return s;
}
static ID3D11DepthStencilState* GetDepth(DepthKey k) {
    if (ID3D11DepthStencilState* s = g_depthCache.find(k)) return s;
    D3D11_DEPTH_STENCIL_DESC d = {};
    d.DepthEnable = k.en || k.write;
    d.DepthWriteMask = k.write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    d.DepthFunc = k.en ? (D3D11_COMPARISON_FUNC)(k.func >= 1 && k.func <= 8 ? k.func : D3D11_COMPARISON_LESS_EQUAL) : D3D11_COMPARISON_ALWAYS;
    ID3D11DepthStencilState* s = nullptr;
    if (FAILED(g_dev->CreateDepthStencilState(&d, &s))) { LogOnce("depthfail", "CreateDepthStencilState failed"); return nullptr; }
    g_depthCache.add(k, s);
    return s;
}
static ID3D11RasterizerState* GetRast(RastKey k) {
    if (ID3D11RasterizerState* s = g_rastCache.find(k)) return s;
    D3D11_RASTERIZER_DESC d = {};
    d.FillMode = k.fill == D3DFILL_WIREFRAME || k.fill == D3DFILL_POINT ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
    d.CullMode = k.cull == D3DCULL_CW ? D3D11_CULL_FRONT : k.cull == D3DCULL_CCW ? D3D11_CULL_BACK : D3D11_CULL_NONE;
    d.FrontCounterClockwise = FALSE;
    d.DepthBias = k.bias;
    d.DepthClipEnable = FALSE;            // see the vertex shader: the game draws some mirror geometry beyond z = 1
    d.MultisampleEnable = g_msaa > 1;
    ID3D11RasterizerState* s = nullptr;
    if (FAILED(g_dev->CreateRasterizerState(&d, &s))) { LogOnce("rastfail", "CreateRasterizerState failed"); return nullptr; }
    g_rastCache.add(k, s);
    return s;
}
static ID3D11SamplerState* GetSampler(Smp k) {
    if (ID3D11SamplerState* s = g_smpCache.find(k)) return s;
    D3D11_SAMPLER_DESC d = {};
    d.Filter = k.filter == 0 ? D3D11_FILTER_MIN_MAG_MIP_POINT : k.filter == 1 ? D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT
             : k.filter == 2 ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_ANISOTROPIC;
    auto addr = [](uint8_t a) { return a >= 1 && a <= 4 ? (D3D11_TEXTURE_ADDRESS_MODE)a : D3D11_TEXTURE_ADDRESS_WRAP; };
    d.AddressU = addr(k.addrU); d.AddressV = addr(k.addrV); d.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    d.MaxAnisotropy = k.filter == 3 ? k.aniso : 1;
    d.ComparisonFunc = D3D11_COMPARISON_NEVER;
    d.MinLOD = 0; d.MaxLOD = k.filter >= 2 ? D3D11_FLOAT32_MAX : 0;
    ID3D11SamplerState* s = nullptr;
    if (FAILED(g_dev->CreateSamplerState(&d, &s))) { LogOnce("smpfail", "CreateSamplerState failed"); return nullptr; }
    g_smpCache.add(k, s);
    return s;
}

static void ApplyViewport(double x, double y, double w, double h) {
    D3D11_VIEWPORT v;
    v.TopLeftX = (float)(x * g_scale); v.TopLeftY = (float)(y * g_scale);
    v.Width = (float)(w * g_scale); v.Height = (float)(h * g_scale);
    v.MinDepth = 0; v.MaxDepth = 1;
    g_ctx->RSSetViewports(1, &v);
}

static void Apply(const Pipe& p) {
    BlendKey bk = {p.blend, p.src, p.dst, 0};
    g_ctx->OMSetBlendState(GetBlend(bk), nullptr, 0xffffffff);
    DepthKey dk = {p.zenable, p.zwrite, p.zfunc, 0};
    g_ctx->OMSetDepthStencilState(GetDepth(dk), 0);
    RastKey rk = {p.cull, p.fill, {0, 0}, p.bias};
    g_ctx->RSSetState(GetRast(rk));
    ID3D11SamplerState* ss[2] = {GetSampler(p.smp[0]), GetSampler(p.smp[1])};
    g_ctx->PSSetSamplers(0, 2, ss);
    g_ctx->PSSetShaderResources(0, 2, p.srv);
    if (!g_cbValid || memcmp(&g_lastCB, &p.cb, sizeof(CB))) {
        g_ctx->UpdateSubresource(g_cb, 0, nullptr, &p.cb, 0, 0);
        g_lastCB = p.cb; g_cbValid = true;
    }
}

static ID3D11ShaderResourceView* g_white;          // 1x1 opaque white for stages without a texture

static Smp StageSampler(const Stage& s) {
    Smp k = {};
    k.addrU = (uint8_t)s.addrU; k.addrV = (uint8_t)s.addrV;
    bool linear = s.mag != D3DTFG_POINT || s.min != D3DTFN_POINT;
    bool mips = s.tex && s.tex->mips && (s.mip != D3DTFP_NONE || g_cfg.mipmaps);
    k.filter = !linear ? 0 : mips ? 2 : 1;
    if (linear && mips && g_cfg.anisotropy > 1) { k.filter = 3; k.aniso = (uint8_t)g_cfg.anisotropy; }
    return k;
}

// The game's 3D state as a pipeline
static void GamePipe(Pipe& p) {
    const GameState& g = g_gs;
    memset(&p, 0, sizeof(p));
    p.blend = (uint8_t)g.blend; p.src = (uint8_t)g.src; p.dst = (uint8_t)g.dst;
    p.zenable = (uint8_t)(g.zenable != 0); p.zwrite = (uint8_t)(g.zwrite != 0); p.zfunc = (uint8_t)g.zfunc;
    p.cull = (uint8_t)g.cull; p.fill = (uint8_t)g.fill;
    p.bias = -(int)(g.zbias * 336);
    CB& c = p.cb;
    c.vp[0] = (float)g.vpX; c.vp[1] = (float)g.vpY; c.vp[2] = (float)g.vpW; c.vp[3] = (float)g.vpH;
    c.fogColor[0] = ((g.fogcolor >> 16) & 255) / 255.f; c.fogColor[1] = ((g.fogcolor >> 8) & 255) / 255.f;
    c.fogColor[2] = (g.fogcolor & 255) / 255.f; c.fogColor[3] = 1;
    c.fogParams[0] = g.fogstart; c.fogParams[1] = g.fogend; c.fogParams[2] = g.fogdensity;
    c.tfactor[0] = ((g.tfactor >> 16) & 255) / 255.f; c.tfactor[1] = ((g.tfactor >> 8) & 255) / 255.f;
    c.tfactor[2] = (g.tfactor & 255) / 255.f; c.tfactor[3] = (g.tfactor >> 24) / 255.f;
    int* sa[2] = {c.s0, c.s1}; int* sb[2] = {c.s0b, c.s1b};
    bool keyedTex = false;
    for (int i = 0; i < 2; i++) {
        const Stage& s = g.st[i];
        uint32_t a[4] = {s.colorop, s.carg1, s.carg2, s.alphaop};
        uint32_t b[2] = {s.aarg1, s.aarg2};
        if (!s.tex) {
            // A stage without a texture reads opaque white, except that an alpha op that only
            // SELECTS the texture takes the diffuse alpha instead (how the 1999 drivers behaved).
            if (a[3] == D3DTOP_SELECTARG1 && (b[0] & 15) == D3DTA_TEXTURE) b[0] = (b[0] & ~15u) | D3DTA_DIFFUSE;
            if (a[3] == D3DTOP_SELECTARG2 && (b[1] & 15) == D3DTA_TEXTURE) b[1] = (b[1] & ~15u) | D3DTA_DIFFUSE;
        }
        sa[i][0] = (int)a[0]; sa[i][1] = (int)a[1]; sa[i][2] = (int)a[2]; sa[i][3] = (int)a[3];
        sb[i][0] = (int)b[0]; sb[i][1] = (int)b[1]; sb[i][2] = (int)(s.texcoord & 0xffff); sb[i][3] = 0;
        p.srv[i] = s.tex ? s.tex->srv : g_white;
        p.smp[i] = StageSampler(s);
        if (s.tex && s.keyed && (i == 0 || s.colorop != D3DTOP_DISABLE)) keyedTex = true;
    }
    if (g.st[0].colorop == D3DTOP_DISABLE) c.s1[0] = D3DTOP_DISABLE;
    // alpha test; colour keying (texels with the key are not drawn) works like an alpha test
    int af = g.atest ? (int)g.afunc : 0; float ref = (g.aref & 255) / 255.f;
    if (g.colorkey && keyedTex && (af == 0 || af == D3DCMP_ALWAYS)) { af = D3DCMP_GREATER; ref = 0.5f; }   // half: filtered edges stay clean
    if (af == D3DCMP_ALWAYS) af = 0;
    c.fogParams[3] = ref;
    c.misc[0] = af;
    c.misc[1] = !g.fog ? 0 : g.fogmode == D3DFOG_NONE ? 1 : g.fogmode == D3DFOG_EXP ? 2 : g.fogmode == D3DFOG_EXP2 ? 3 : 4;
    c.misc[2] = g.specular ? 1 : 0;
}

// Plain 2D drawing (blits, fills): texture or diffuse colour, no depth
static void Pipe2D(Pipe& p, Tex* t, bool blend, bool keyed, bool smooth) {
    memset(&p, 0, sizeof(p));
    p.blend = blend; p.src = D3DBLEND_SRCALPHA; p.dst = D3DBLEND_INVSRCALPHA;
    p.zfunc = D3DCMP_ALWAYS; p.cull = D3DCULL_NONE; p.fill = D3DFILL_SOLID;
    CB& c = p.cb;
    c.vp[0] = 0; c.vp[1] = 0; c.vp[2] = (float)g_vw; c.vp[3] = (float)g_vh;
    c.tfactor[0] = c.tfactor[1] = c.tfactor[2] = c.tfactor[3] = 1;
    int arg = t ? D3DTA_TEXTURE : D3DTA_DIFFUSE;
    c.s0[0] = t ? D3DTOP_MODULATE : D3DTOP_SELECTARG1; c.s0[1] = arg; c.s0[2] = D3DTA_DIFFUSE;
    c.s0[3] = t ? D3DTOP_MODULATE : D3DTOP_SELECTARG1; c.s0b[0] = arg; c.s0b[1] = D3DTA_DIFFUSE;
    c.s1[0] = D3DTOP_DISABLE; c.s1[3] = D3DTOP_DISABLE; c.s1b[2] = 1;
    c.misc[0] = keyed ? D3DCMP_GREATER : 0; c.fogParams[3] = 0;
    p.srv[0] = t ? t->srv : g_white; p.srv[1] = g_white;
    p.smp[0].addrU = p.smp[0].addrV = D3DTADDRESS_CLAMP; p.smp[0].filter = smooth ? 1 : 0;
    p.smp[1] = p.smp[0];
}

// ---- geometry upload ---------------------------------------------------------------------------
static bool EnsureVB(UINT bytes) {
    if (g_vb && g_vbSize >= bytes) return true;
    if (g_vb) g_vb->Release();
    g_vbSize = 1 << 20; while (g_vbSize < bytes) g_vbSize *= 2;
    D3D11_BUFFER_DESC d = {g_vbSize, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    g_vbPos = g_vbSize;
    if (FAILED(g_dev->CreateBuffer(&d, nullptr, &g_vb))) { g_vb = nullptr; Log("geometry buffers failed"); return false; }
    return true;
}
static bool EnsureIB(UINT bytes) {
    if (g_ib && g_ibSize >= bytes) return true;
    if (g_ib) g_ib->Release();
    g_ibSize = 1 << 18; while (g_ibSize < bytes) g_ibSize *= 2;
    D3D11_BUFFER_DESC d = {g_ibSize, D3D11_USAGE_DYNAMIC, D3D11_BIND_INDEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    g_ibPos = g_ibSize;
    if (FAILED(g_dev->CreateBuffer(&d, nullptr, &g_ib))) { g_ib = nullptr; Log("geometry buffers failed"); return false; }
    return true;
}
// Copies vertices into the ring buffer; returns the first vertex index or -1
static int PushVerts(const Vtx* v, UINT n) {
    UINT bytes = n * sizeof(Vtx);
    if (!EnsureVB(bytes)) return -1;
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (g_vbPos + bytes > g_vbSize) { g_vbPos = 0; mode = D3D11_MAP_WRITE_DISCARD; }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g_ctx->Map(g_vb, 0, mode, 0, &m))) return -1;
    memcpy((uint8_t*)m.pData + g_vbPos, v, bytes);
    g_ctx->Unmap(g_vb, 0);
    int first = (int)(g_vbPos / sizeof(Vtx));
    g_vbPos += bytes;
    g_vbPos = (g_vbPos + sizeof(Vtx) - 1) / sizeof(Vtx) * sizeof(Vtx);
    return first;
}
static int PushIndices(const uint16_t* idx, UINT n) {
    UINT bytes = n * 2;
    if (!EnsureIB(bytes + 4)) return -1;
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (g_ibPos + bytes > g_ibSize) { g_ibPos = 0; mode = D3D11_MAP_WRITE_DISCARD; }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g_ctx->Map(g_ib, 0, mode, 0, &m))) return -1;
    memcpy((uint8_t*)m.pData + g_ibPos, idx, bytes);
    g_ctx->Unmap(g_ib, 0);
    int first = (int)(g_ibPos / 2);
    g_ibPos += (bytes + 3) & ~3u;
    return first;
}

// DirectX 6 flexible vertex format -> our vertex
static Vec<Vtx> g_conv;
static const Vtx* ConvertVerts(uint32_t fvf, const void* src, uint32_t n) {
    g_conv.resize((int)n);
    const uint8_t* p = (const uint8_t*)src;
    uint32_t pos = fvf & D3DFVF_POSITION_MASK;
    int posFloats = pos == D3DFVF_XYZRHW ? 4 : pos == D3DFVF_XYZ ? 3 : pos == D3DFVF_XYZB1 ? 4 : pos == D3DFVF_XYZB2 ? 5
                  : pos == D3DFVF_XYZB3 ? 6 : pos == D3DFVF_XYZB4 ? 7 : pos == D3DFVF_XYZB5 ? 8 : 3;
    if (pos != D3DFVF_XYZRHW) LogOnce("fvf_nonTL", "note: non-pretransformed vertex format %08x drawn as screen coordinates", fvf);
    int ntex = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    int stride = posFloats * 4 + ((fvf & D3DFVF_NORMAL) ? 12 : 0) + ((fvf & D3DFVF_RESERVED1) ? 4 : 0)
               + ((fvf & D3DFVF_DIFFUSE) ? 4 : 0) + ((fvf & D3DFVF_SPECULAR) ? 4 : 0) + ntex * 8;
    for (uint32_t i = 0; i < n; i++, p += stride) {
        Vtx& v = g_conv[(int)i];
        const float* f = (const float*)p;
        v.x = f[0]; v.y = f[1]; v.z = f[2]; v.w = pos == D3DFVF_XYZRHW ? f[3] : 1.f;
        const uint8_t* q = p + posFloats * 4;
        if (fvf & D3DFVF_NORMAL) q += 12;
        if (fvf & D3DFVF_RESERVED1) q += 4;
        v.dif = 0xffffffff; v.spec = 0xff000000;
        if (fvf & D3DFVF_DIFFUSE) { v.dif = *(const uint32_t*)q; q += 4; }
        if (fvf & D3DFVF_SPECULAR) { v.spec = *(const uint32_t*)q; q += 4; }
        v.u0 = v.v0 = v.u1 = v.v1 = 0;
        if (ntex >= 1) { v.u0 = ((const float*)q)[0]; v.v0 = ((const float*)q)[1]; }
        if (ntex >= 2) { v.u1 = ((const float*)q)[2]; v.v1 = ((const float*)q)[3]; }
        else { v.u1 = v.u0; v.v1 = v.v0; }
    }
    return g_conv.p;
}

static void BindGeometry() {
    UINT stride = sizeof(Vtx), off = 0;
    g_ctx->IASetVertexBuffers(0, 1, &g_vb, &stride, &off);
    g_ctx->IASetIndexBuffer(g_ib, DXGI_FORMAT_R16_UINT, 0);
    g_ctx->IASetInputLayout(g_layout);
    g_ctx->VSSetShader(g_vs, nullptr, 0);
    g_ctx->PSSetShader(g_ps, nullptr, 0);
    g_ctx->VSSetConstantBuffers(0, 1, &g_cb);
    g_ctx->PSSetConstantBuffers(0, 1, &g_cb);
}

static void BindGameTarget() {
    g_ctx->OMSetRenderTargets(1, &g_rtv, g_dsv);
}

// Draws a primitive list; fans become indexed triangle lists
static Vec<uint16_t> g_idxTmp;
static void Draw(uint32_t prim, const Vtx* v, UINT nv, const uint16_t* idx, UINT ni) {
    if (!nv) return;
    D3D11_PRIMITIVE_TOPOLOGY topo;
    switch (prim) {
    case D3DPT_POINTLIST: topo = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST; break;
    case D3DPT_LINELIST: topo = D3D11_PRIMITIVE_TOPOLOGY_LINELIST; break;
    case D3DPT_LINESTRIP: topo = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP; break;
    case D3DPT_TRIANGLELIST: topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;
    case D3DPT_TRIANGLESTRIP: topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; break;
    case D3DPT_TRIANGLEFAN: {
        UINT n = idx ? ni : nv;
        if (n < 3) return;
        g_idxTmp.resize((int)((n - 2) * 3));
        for (UINT i = 0; i + 2 < n; i++) {
            g_idxTmp[(int)(i * 3)] = idx ? idx[0] : 0;
            g_idxTmp[(int)(i * 3 + 1)] = idx ? idx[i + 1] : (uint16_t)(i + 1);
            g_idxTmp[(int)(i * 3 + 2)] = idx ? idx[i + 2] : (uint16_t)(i + 2);
        }
        idx = g_idxTmp.p; ni = (UINT)g_idxTmp.n;
        topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        break;
    }
    default: return;
    }
    int first = PushVerts(v, nv);
    if (first < 0) return;
    BindGeometry();
    g_ctx->IASetPrimitiveTopology(topo);
    if (idx) {
        int fi = PushIndices(idx, ni);
        if (fi < 0) return;
        BindGeometry();
        g_ctx->DrawIndexed(ni, (UINT)fi, first);
    } else g_ctx->Draw(nv, (UINT)first);
    g_rtDirty = true;
}

// A screen-aligned quad in game pixels
static void DrawQuad(float x0, float y0, float x1, float y1, float z, uint32_t col, float u0, float v0, float u1, float v1) {
    Vtx q[4] = {{x0 - 0.5f, y0 - 0.5f, z, 1, col, 0xff000000, u0, v0, u0, v0}, {x1 - 0.5f, y0 - 0.5f, z, 1, col, 0xff000000, u1, v0, u1, v0},
                {x1 - 0.5f, y1 - 0.5f, z, 1, col, 0xff000000, u1, v1, u1, v1}, {x0 - 0.5f, y1 - 0.5f, z, 1, col, 0xff000000, u0, v1, u0, v1}};
    uint16_t idx[6] = {0, 1, 2, 0, 2, 3};
    Draw(D3DPT_TRIANGLELIST, q, 4, idx, 6);
}

// ---- render targets ----------------------------------------------------------------------------
static void RingRelease();
static void ReleaseTargets() {
    if (g_ctx) g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
    if (g_rtSRV) { g_rtSRV->Release(); g_rtSRV = nullptr; }
    if (g_rtResolve && g_rtResolve != g_rt) g_rtResolve->Release();
    g_rtResolve = nullptr;
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    if (g_rt) { g_rt->Release(); g_rt = nullptr; }
    if (g_dsv) { g_dsv->Release(); g_dsv = nullptr; }
    if (g_ds) { g_ds->Release(); g_ds = nullptr; }
    if (g_staging) { g_staging->Release(); g_staging = nullptr; }
    RingRelease();
}

static void ComputeMapping() {
    double sx = (double)g_outW / g_vw, sy = (double)g_outH / g_vh;
    g_scale = sx < sy ? sx : sy;
    if (g_scale <= 0) g_scale = 1;
    g_rtW = (int)floor(g_vw * g_scale + 0.5); g_rtH = (int)floor(g_vh * g_scale + 0.5);
    if (g_rtW < 1) g_rtW = 1; if (g_rtH < 1) g_rtH = 1;
    g_offX = floor((g_outW - g_rtW) / 2.0); g_offY = floor((g_outH - g_rtH) / 2.0);
}

static bool CreateTargets() {
    ReleaseTargets();
    ComputeMapping();
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = g_rtW; d.Height = g_rtH; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = g_msaa; d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | (g_msaa > 1 ? 0 : D3D11_BIND_SHADER_RESOURCE);
    HRESULT hr = g_dev->CreateTexture2D(&d, nullptr, &g_rt);
    if (FAILED(hr) && g_msaa > 1) {
        Log("MSAA render target failed (%08lx), disabling MSAA", hr);
        g_msaa = 1; d.SampleDesc.Count = 1; d.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
        g_rastCache.clear();
        hr = g_dev->CreateTexture2D(&d, nullptr, &g_rt);
    }
    if (FAILED(hr)) { Log("render target %dx%d failed %08lx", g_rtW, g_rtH, hr); return false; }
    if (FAILED(g_dev->CreateRenderTargetView(g_rt, nullptr, &g_rtv))) { Log("render target view failed"); return false; }
    if (g_msaa > 1) {
        D3D11_TEXTURE2D_DESC r = d; r.SampleDesc.Count = 1; r.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(g_dev->CreateTexture2D(&r, nullptr, &g_rtResolve))) { Log("resolve texture failed"); return false; }
    } else g_rtResolve = g_rt;
    if (FAILED(g_dev->CreateShaderResourceView(g_rtResolve, nullptr, &g_rtSRV))) { Log("render target shader view failed"); return false; }
    D3D11_TEXTURE2D_DESC z = d; z.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; z.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    hr = g_dev->CreateTexture2D(&z, nullptr, &g_ds);
    if (FAILED(hr)) { Log("depth buffer failed %08lx", hr); return false; }
    if (FAILED(g_dev->CreateDepthStencilView(g_ds, nullptr, &g_dsv))) { Log("depth view failed"); return false; }
    float black[4] = {0, 0, 0, 1};
    g_ctx->ClearRenderTargetView(g_rtv, black);
    g_ctx->ClearDepthStencilView(g_dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1, 0);
    BindGameTarget();
    g_rtDirty = true;
    Log("virtual %dx%d -> render target %dx%d (scale %.3f) on output %dx%d", g_vw, g_vh, g_rtW, g_rtH, g_scale, g_outW, g_outH);
    return true;
}

// ---- shaders ---------------------------------------------------------------------------------
static bool LoadShaders() {
    const void* vsCode = kVSBlob; SIZE_T vsLen = sizeof(kVSBlob);
    const void* psCode = kPSBlob; SIZE_T psLen = sizeof(kPSBlob);
    ID3DBlob* vsb = nullptr; ID3DBlob* psb = nullptr;
    const char* how = "built-in bytecode";
    if (!g_cfg.builtinShaders) {
        HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
        typedef HRESULT(WINAPI * CompileFn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
        CompileFn compile = dc ? (CompileFn)GetProcAddress(dc, "D3DCompile") : nullptr;
        if (compile) {
            ID3DBlob* err = nullptr;
            HRESULT hr = compile(kShaderSrc, sizeof(kShaderSrc) - 1, "swat3d11", nullptr, nullptr, "VSMain", "vs_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsb, &err);
            if (FAILED(hr)) { Log("vertex shader compile failed %08lx: %s", hr, err ? (const char*)err->GetBufferPointer() : ""); vsb = nullptr; }
            if (err) err->Release(); err = nullptr;
            hr = compile(kShaderSrc, sizeof(kShaderSrc) - 1, "swat3d11", nullptr, nullptr, "PSMain", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psb, &err);
            if (FAILED(hr)) { Log("pixel shader compile failed %08lx: %s", hr, err ? (const char*)err->GetBufferPointer() : ""); psb = nullptr; }
            if (err) err->Release();
            if (vsb && psb) {
                vsCode = vsb->GetBufferPointer(); vsLen = vsb->GetBufferSize();
                psCode = psb->GetBufferPointer(); psLen = psb->GetBufferSize();
                how = "compiled with d3dcompiler_47";
            }
        }
    }
    D3D11_INPUT_ELEMENT_DESC il[5] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 1, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    HRESULT h1 = g_dev->CreateVertexShader(vsCode, vsLen, nullptr, &g_vs);
    HRESULT h2 = g_dev->CreatePixelShader(psCode, psLen, nullptr, &g_ps);
    HRESULT h3 = g_dev->CreateInputLayout(il, 5, vsCode, vsLen, &g_layout);
    if (vsb) vsb->Release(); if (psb) psb->Release();
    if (FAILED(h1) || FAILED(h2) || FAILED(h3)) { Log("shader creation failed (%s): %08lx %08lx %08lx", how, h1, h2, h3); return false; }
    Log("shaders ready (%s)", how);
    return true;
}

// ---- window placement --------------------------------------------------------------------------
static void PlaceWindow() {
    HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoA(mon, &mi);
    RECT m = mi.rcMonitor;
    LONG style = GetWindowLongA(g_hwnd, GWL_STYLE), ex = GetWindowLongA(g_hwnd, GWL_EXSTYLE);
    if (g_cfg.fullscreen) {
        SetWindowLongA(g_hwnd, GWL_STYLE, (style & ~(WS_OVERLAPPEDWINDOW | WS_DLGFRAME | WS_BORDER)) | WS_POPUP | WS_VISIBLE);
        SetWindowLongA(g_hwnd, GWL_EXSTYLE, ex & ~(WS_EX_TOPMOST | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME));
        BOOL ok = SetWindowPos(g_hwnd, HWND_NOTOPMOST, m.left, m.top, m.right - m.left, m.bottom - m.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);
        RECT c; GetClientRect(g_hwnd, &c);
        Log("window -> monitor %ldx%ld at %ld,%ld (SetWindowPos %d, client now %ldx%ld)", m.right - m.left, m.bottom - m.top, m.left, m.top, ok, c.right, c.bottom);
    } else {
        LONG ns = (style & ~WS_POPUP) | WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE;
        SetWindowLongA(g_hwnd, GWL_STYLE, ns);
        SetWindowLongA(g_hwnd, GWL_EXSTYLE, ex & ~WS_EX_TOPMOST);
        RECT r = {0, 0, g_cfg.windowW, g_cfg.windowH};
        AdjustWindowRectEx(&r, ns, FALSE, ex & ~WS_EX_TOPMOST);
        int w = r.right - r.left, h = r.bottom - r.top;
        int x = m.left + ((m.right - m.left) - w) / 2, y = m.top + ((m.bottom - m.top) - h) / 2;
        if (y < m.top) y = m.top;
        SetWindowPos(g_hwnd, HWND_NOTOPMOST, x, y, w, h, SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);
        Log("window %dx%d (client %dx%d)", w, h, g_cfg.windowW, g_cfg.windowH);
    }
}

// ---- init --------------------------------------------------------------------------------------
static bool InitDevice() {
    HMODULE d3d = LoadLibraryA("d3d11.dll");
    if (!d3d) { Log("cannot load d3d11.dll - DirectX 11 is not available on this system"); return false; }
    auto create = (PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN)GetProcAddress(d3d, "D3D11CreateDeviceAndSwapChain");
    if (!create) { Log("D3D11CreateDeviceAndSwapChain not found"); return false; }

    PlaceWindow();
    RECT c; GetClientRect(g_hwnd, &c);
    int cw = c.right > 0 ? c.right : 640, ch = c.bottom > 0 ? c.bottom : 480;
    g_outW = g_cfg.fullscreen && g_cfg.outputW > 0 ? g_cfg.outputW : cw;
    g_outH = g_cfg.fullscreen && g_cfg.outputH > 0 ? g_cfg.outputH : ch;

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferDesc.Width = g_outW; sd.BufferDesc.Height = g_outH;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_hwnd; sd.Windowed = TRUE;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_10_0;
    static const struct { DXGI_SWAP_EFFECT e; UINT buffers; const char* name; } effects[] = {
        {(DXGI_SWAP_EFFECT)4 /* FLIP_DISCARD */, 2, "flip-discard"}, {(DXGI_SWAP_EFFECT)3 /* FLIP_SEQUENTIAL */, 2, "flip-sequential"},
        {DXGI_SWAP_EFFECT_DISCARD, 1, "bitblt"}};
    HRESULT hr = E_FAIL; const char* how = "";
    for (auto& e : effects) {
        sd.SwapEffect = e.e; sd.BufferCount = e.buffers;
        hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 3, D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &got, &g_ctx);
        if (SUCCEEDED(hr)) { how = e.name; g_flipModel = e.e != DXGI_SWAP_EFFECT_DISCARD; break; }
    }
    if (FAILED(hr)) { Log("Direct3D 11 device creation failed %08lx (a DirectX 10 class graphics card is required)", hr); return false; }

    IDXGIDevice* xd = nullptr; IDXGIAdapter* ad = nullptr; IDXGIFactory* f = nullptr;
    if (SUCCEEDED(g_dev->QueryInterface(IID_IDXGIDevice, (void**)&xd)) && SUCCEEDED(xd->GetAdapter(&ad))) {
        DXGI_ADAPTER_DESC desc; char name[128] = "";
        if (SUCCEEDED(ad->GetDesc(&desc))) WideCharToMultiByte(CP_ACP, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
        Log("Adapter: %s", name);
        if (SUCCEEDED(ad->GetParent(IID_IDXGIFactory, (void**)&f))) { f->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES); f->Release(); }
        ad->Release();
    }
    if (xd) xd->Release();

    g_msaa = 1;
    if (g_cfg.msaa > 1) {
        UINT q = 0;
        if (SUCCEEDED(g_dev->CheckMultisampleQualityLevels(DXGI_FORMAT_B8G8R8A8_UNORM, g_cfg.msaa, &q)) && q > 0) g_msaa = g_cfg.msaa;
        else Log("MSAA x%d not supported", g_cfg.msaa);
    }
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(g_swap->GetBuffer(0, IID_ID3D11Texture2D, (void**)&bb))) { Log("swap chain buffer unavailable"); return false; }
    hr = g_dev->CreateRenderTargetView(bb, nullptr, &g_bbRTV);
    bb->Release();
    if (FAILED(hr)) { Log("swap chain view failed %08lx", hr); return false; }

    D3D11_BUFFER_DESC cbd = {(UINT)((sizeof(CB) + 15) & ~15), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    if (FAILED(g_dev->CreateBuffer(&cbd, nullptr, &g_cb))) { Log("constant buffer failed"); return false; }
    if (!EnsureVB(1) || !EnsureIB(4)) return false;
    if (!LoadShaders()) return false;
    {   // 1x1 white
        uint32_t w = 0xffffffff;
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = td.Height = td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sdat = {&w, 4, 4};
        ID3D11Texture2D* t = nullptr;
        if (FAILED(g_dev->CreateTexture2D(&td, &sdat, &t)) || FAILED(g_dev->CreateShaderResourceView(t, nullptr, &g_white))) { Log("texture 1x1 failed"); return false; }
        t->Release();
    }
    Log("Direct3D 11 device created (feature level %x.%x, %s presentation), output %dx%d, vsync %d, msaa %d",
        (got >> 12) & 15, (got >> 8) & 15, how, g_outW, g_outH, g_cfg.vsync, g_msaa);
    return CreateTargets();
}

bool Init(HWND hwnd) {
    LOCKED;
    if (g_ready) return true;
    if (g_initFailed || !hwnd) return false;
    g_hwnd = hwnd;
    if (!InitDevice()) {
        g_initFailed = true;
        Log("ERROR: Direct3D 11 initialisation failed");
        MessageBoxA(hwnd, "The SWAT 3 Direct3D 11 renderer could not start.\n\nPlease send swat3d11.log (next to swat.exe).\n"
                          "To play in the meantime, put your previous ddraw.dll back.", "SWAT 3 renderer", MB_ICONERROR);
        return false;
    }
    g_ready = true;
    return true;
}
bool Ready() { return g_ready; }
HWND Window() { return g_hwnd; }
void Shutdown() {
    LOCKED;
    if (!g_ready) return;
    g_ready = false;
    ReleaseTargets();
    g_blendCache.clear(); g_depthCache.clear(); g_rastCache.clear(); g_smpCache.clear();
    if (g_ctx) g_ctx->ClearState();
}

void SetVirtualMode(int w, int h) {
    LOCKED;
    if (w <= 0 || h <= 0) return;
    if (w == g_vw && h == g_vh && g_rt) return;
    g_vw = w; g_vh = h;
    g_gs.vpX = 0; g_gs.vpY = 0; g_gs.vpW = w; g_gs.vpH = h;
    if (g_ready) CreateTargets();
}
void GetVirtualMode(int* w, int* h) { *w = g_vw; *h = g_vh; }
void GetOutputSize(int* w, int* h) { *w = g_outW; *h = g_outH; }
Mapping GetMapping() {
    LOCKED;
    Mapping m = {g_scale, g_offX, g_offY};
    if (g_ready && !g_cfg.fullscreen) {
        // windowed: the swap chain is stretched to the window's client area
        RECT c; GetClientRect(g_hwnd, &c);
        if (c.right > 0 && c.bottom > 0) {
            double fx = (double)c.right / g_outW, fy = (double)c.bottom / g_outH;
            m.scale *= fx; m.offX *= fx; m.offY *= fy;
        }
    } else if (g_ready && g_cfg.outputW > 0) {
        RECT c; GetClientRect(g_hwnd, &c);
        if (c.right > 0 && c.bottom > 0 && g_outW > 0) {
            double fx = (double)c.right / g_outW, fy = (double)c.bottom / g_outH;
            m.scale *= fx; m.offX *= fx; m.offY *= fy;
        }
    }
    return m;
}

// ---- textures ----------------------------------------------------------------------------------
Tex* TexCreate(int w, int h, bool mipmaps) {
    LOCKED;
    if (!g_ready || w <= 0 || h <= 0) return nullptr;
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = mipmaps ? 0 : 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (mipmaps ? D3D11_BIND_RENDER_TARGET : 0);
    d.MiscFlags = mipmaps ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
    Tex* t = new Tex();
    t->w = w; t->h = h; t->mips = mipmaps; t->tex = nullptr; t->srv = nullptr;
    HRESULT hr = g_dev->CreateTexture2D(&d, nullptr, &t->tex);
    if (FAILED(hr) && mipmaps) { d.MipLevels = 1; d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.MiscFlags = 0; t->mips = false; hr = g_dev->CreateTexture2D(&d, nullptr, &t->tex); }
    if (FAILED(hr) || FAILED(g_dev->CreateShaderResourceView(t->tex, nullptr, &t->srv))) {
        LogOnce("texfail", "texture %dx%d failed %08lx", w, h, hr);
        if (t->tex) t->tex->Release();
        delete t; return nullptr;
    }
    return t;
}
void TexUpload(Tex* t, const uint32_t* argb, int pitch) {
    LOCKED;
    if (!t || !g_ready) return;
    g_ctx->UpdateSubresource(t->tex, 0, nullptr, argb, pitch, 0);
    if (t->mips) g_ctx->GenerateMips(t->srv);
}
void TexDestroy(Tex* t) {
    LOCKED;
    if (!t) return;
    for (int i = 0; i < 2; i++) if (g_gs.st[i].tex == t) g_gs.st[i].tex = nullptr;
    if (g_ctx) { ID3D11ShaderResourceView* none[2] = {}; g_ctx->PSSetShaderResources(0, 2, none); }
    if (t->srv) t->srv->Release();
    if (t->tex) t->tex->Release();
    delete t;
}

// ---- Direct3D 6 state --------------------------------------------------------------------------
static float AsFloat(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }
void SetRenderState6(uint32_t s, uint32_t v) {
    LOCKED;
    GameState& g = g_gs;
    switch (s) {
    case D3DRENDERSTATE_ZENABLE: g.zenable = v; break;
    case D3DRENDERSTATE_ZWRITEENABLE: g.zwrite = v; break;
    case D3DRENDERSTATE_ZFUNC: g.zfunc = v; break;
    case D3DRENDERSTATE_ALPHABLENDENABLE: g.blend = v; break;
    case D3DRENDERSTATE_SRCBLEND: g.src = v; break;
    case D3DRENDERSTATE_DESTBLEND: g.dst = v; break;
    case D3DRENDERSTATE_ALPHATESTENABLE: g.atest = v; break;
    case D3DRENDERSTATE_ALPHAFUNC: g.afunc = v; break;
    case D3DRENDERSTATE_ALPHAREF: g.aref = v; break;
    case D3DRENDERSTATE_COLORKEYENABLE: g.colorkey = v; break;
    case D3DRENDERSTATE_CULLMODE: g.cull = v; break;
    case D3DRENDERSTATE_FILLMODE: g.fill = v; break;
    case D3DRENDERSTATE_ZBIAS: g.zbias = v; break;
    case D3DRENDERSTATE_FOGENABLE: g.fog = v; break;
    case D3DRENDERSTATE_FOGTABLEMODE: g.fogmode = v; break;
    case D3DRENDERSTATE_FOGCOLOR: g.fogcolor = v; break;
    case D3DRENDERSTATE_FOGTABLESTART: g.fogstart = AsFloat(v); break;
    case D3DRENDERSTATE_FOGTABLEEND: g.fogend = AsFloat(v); break;
    case D3DRENDERSTATE_FOGTABLEDENSITY: g.fogdensity = AsFloat(v); break;
    case D3DRENDERSTATE_SPECULARENABLE: g.specular = v; break;
    case D3DRENDERSTATE_TEXTUREFACTOR: g.tfactor = v; break;
    default: break;
    }
}
void SetTextureStageState6(uint32_t stage, uint32_t type, uint32_t v) {
    LOCKED;
    if (stage >= 2) { if (type == D3DTSS_COLOROP && v != D3DTOP_DISABLE) LogOnce("stage2", "note: texture stage %u used - only two stages are drawn", stage); return; }
    Stage& s = g_gs.st[stage];
    switch (type) {
    case D3DTSS_COLOROP: s.colorop = v; break;
    case D3DTSS_COLORARG1: s.carg1 = v; break;
    case D3DTSS_COLORARG2: s.carg2 = v; break;
    case D3DTSS_ALPHAOP: s.alphaop = v; break;
    case D3DTSS_ALPHAARG1: s.aarg1 = v; break;
    case D3DTSS_ALPHAARG2: s.aarg2 = v; break;
    case D3DTSS_TEXCOORDINDEX: s.texcoord = v; break;
    case D3DTSS_ADDRESS: s.addrU = s.addrV = v; break;
    case D3DTSS_ADDRESSU: s.addrU = v; break;
    case D3DTSS_ADDRESSV: s.addrV = v; break;
    case D3DTSS_MAGFILTER: s.mag = v; break;
    case D3DTSS_MINFILTER: s.min = v; break;
    case D3DTSS_MIPFILTER: s.mip = v; break;
    default: break;
    }
}
void SetTexture(uint32_t stage, Tex* t, bool keyed) {
    LOCKED;
    if (stage < 2) { g_gs.st[stage].tex = t; g_gs.st[stage].keyed = keyed; }
}
void SetViewport(int x, int y, int w, int h, float, float) {
    LOCKED;
    g_gs.vpX = x; g_gs.vpY = y; g_gs.vpW = w > 0 ? w : 1; g_gs.vpH = h > 0 ? h : 1;
}

void BeginScene() { LOCKED; if (g_ready) BindGameTarget(); }
void EndScene() {}

void DrawPrimitiveTL(uint32_t prim, uint32_t fvf, const void* verts, uint32_t count) {
    LOCKED;
    if (!g_ready || !verts || !count) return;
    const Vtx* v = ConvertVerts(fvf, verts, count);
    Pipe p; GamePipe(p); Apply(p);
    BindGameTarget();
    ApplyViewport(g_gs.vpX, g_gs.vpY, g_gs.vpW, g_gs.vpH);
    Draw(prim, v, count, nullptr, 0);
}
void DrawIndexedPrimitiveTL(uint32_t prim, uint32_t fvf, const void* verts, uint32_t nverts, const uint16_t* idx, uint32_t nidx) {
    LOCKED;
    if (!g_ready || !verts || !nverts || !idx || !nidx) return;
    const Vtx* v = ConvertVerts(fvf, verts, nverts);
    Pipe p; GamePipe(p); Apply(p);
    BindGameTarget();
    ApplyViewport(g_gs.vpX, g_gs.vpY, g_gs.vpW, g_gs.vpH);
    Draw(prim, v, nverts, idx, nidx);
}

// Part of the screen cleared: a quad that writes colour and/or depth
static void ClearRect(const D3DRECT& r, uint32_t flags, uint32_t color, float z) {
    Pipe p; Pipe2D(p, nullptr, false, false, false);
    bool depth = (flags & D3DCLEAR_ZBUFFER) != 0;
    p.zenable = 0; p.zwrite = depth; p.zfunc = D3DCMP_ALWAYS;
    Apply(p);
    if (!(flags & D3DCLEAR_TARGET)) {   // depth only: no colour writes
        D3D11_BLEND_DESC d = {}; d.RenderTarget[0].RenderTargetWriteMask = 0;
        static ID3D11BlendState* noColor = nullptr;
        if (!noColor) g_dev->CreateBlendState(&d, &noColor);
        g_ctx->OMSetBlendState(noColor, nullptr, 0xffffffff);
    }
    DepthKey dk = {1, (uint8_t)depth, D3DCMP_ALWAYS, 0};
    g_ctx->OMSetDepthStencilState(GetDepth(dk), 0);
    BindGameTarget();
    ApplyViewport(0, 0, g_vw, g_vh);
    DrawQuad((float)r.x1, (float)r.y1, (float)r.x2, (float)r.y2, z, color | 0xff000000, 0, 0, 0, 0);
}

void Clear(uint32_t count, const D3DRECT* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil) {
    LOCKED;
    if (!g_ready) return;
    D3DRECT full = {0, 0, g_vw, g_vh};
    if (!count || !rects) { count = 1; rects = &full; }
    for (uint32_t i = 0; i < count; i++) {
        const D3DRECT& r = rects[i];
        bool whole = r.x1 <= 0 && r.y1 <= 0 && r.x2 >= g_vw && r.y2 >= g_vh;
        if (whole) {
            if (flags & D3DCLEAR_TARGET) {
                float c[4] = {((color >> 16) & 255) / 255.f, ((color >> 8) & 255) / 255.f, (color & 255) / 255.f, 1};
                g_ctx->ClearRenderTargetView(g_rtv, c);
            }
            UINT df = ((flags & D3DCLEAR_ZBUFFER) ? D3D11_CLEAR_DEPTH : 0) | ((flags & D3DCLEAR_STENCIL) ? D3D11_CLEAR_STENCIL : 0);
            if (df) g_ctx->ClearDepthStencilView(g_dsv, df, z < 0 ? 0 : z > 1 ? 1 : z, (UINT8)stencil);
            g_rtDirty = true;
        } else if (r.x2 > r.x1 && r.y2 > r.y1) ClearRect(r, flags, color, z);
    }
}

// ---- 2D ----------------------------------------------------------------------------------------
void Blit(Tex* t, int texW, int texH, const RECT& src, const RECT& dst, bool blend) {
    LOCKED;
    if (!g_ready || !t) return;
    bool stretch = (src.right - src.left) != (dst.right - dst.left) || (src.bottom - src.top) != (dst.bottom - dst.top);
    bool smooth = g_cfg.smoothUI && (stretch || g_scale != floor(g_scale));
    // Transparent (colour key) texels have alpha 0. Scaled smoothly, edges get in-between alpha:
    // blend them, so outlines and letters keep their shape; unscaled, a plain cut-off is exact.
    Pipe p; Pipe2D(p, t, blend || smooth, true, smooth);
    p.cb.misc[0] = D3DCMP_GREATER; p.cb.fogParams[3] = smooth || blend ? 0.f : 0.5f;
    if (smooth) {                                       // output pixels per texel
        double sw = src.right - src.left, dw = dst.right - dst.left;
        double k = sw > 0 ? g_scale * dw / sw : g_scale;
        if (k > 1.01) p.cb.misc[3] = (int)(k * 1000);
    }
    Apply(p);
    BindGameTarget();
    ApplyViewport(0, 0, g_vw, g_vh);
    float u0 = (float)src.left / texW, v0 = (float)src.top / texH, u1 = (float)src.right / texW, v1 = (float)src.bottom / texH;
    DrawQuad((float)dst.left, (float)dst.top, (float)dst.right, (float)dst.bottom, 0, 0xffffffff, u0, v0, u1, v1);
}

void Fill(const RECT* r, uint32_t argb) {
    LOCKED;
    if (!g_ready) return;
    if (!r || (r->left <= 0 && r->top <= 0 && r->right >= g_vw && r->bottom >= g_vh)) {
        float c[4] = {((argb >> 16) & 255) / 255.f, ((argb >> 8) & 255) / 255.f, (argb & 255) / 255.f, 1};
        g_ctx->ClearRenderTargetView(g_rtv, c);
        g_rtDirty = true;
        return;
    }
    Pipe p; Pipe2D(p, nullptr, false, false, false);
    Apply(p);
    BindGameTarget();
    ApplyViewport(0, 0, g_vw, g_vh);
    DrawQuad((float)r->left, (float)r->top, (float)r->right, (float)r->bottom, 0, argb | 0xff000000, 0, 0, 0, 0);
}

static void Resolve() {
    if (g_msaa > 1 && g_rtDirty) g_ctx->ResolveSubresource(g_rtResolve, 0, g_rt, 0, DXGI_FORMAT_B8G8R8A8_UNORM);
    g_rtDirty = false;
}

// Copies game pixels out of a mapped picture at output scale
static void CopyOut(const D3D11_MAPPED_SUBRESOURCE& m, const RECT& r, uint32_t* out, int pitch) {
    static Vec<int> xs;
    int w = r.right - r.left;
    xs.resize(w);
    for (int x = 0; x < w; x++) { int sx = (int)((r.left + x + 0.5) * g_scale); xs[x] = sx < 0 ? 0 : sx >= g_rtW ? g_rtW - 1 : sx; }
    bool direct = g_scale == 1.0;
    for (int y = r.top; y < r.bottom; y++) {
        int sy = direct ? y : (int)((y + 0.5) * g_scale); if (sy >= g_rtH) sy = g_rtH - 1; if (sy < 0) sy = 0;
        const uint32_t* row = (const uint32_t*)((const uint8_t*)m.pData + (size_t)sy * m.RowPitch);
        uint32_t* o = out + (size_t)(y - r.top) * pitch;
        if (direct) memcpy(o, row + r.left, (size_t)w * 4);
        else for (int x = 0; x < w; x++) o[x] = row[xs[x]];
    }
}

// Read-back without waiting: while the game keeps reading the screen, every present also copies
// the finished picture into one of two CPU-readable textures. A read then uses the newest copy
// (one frame old) instead of stopping until the graphics card has finished the current frame.
static ID3D11Texture2D* g_ring[2]; static bool g_ringValid[2]; static int g_ringNext = 0, g_ringNewest = -1;
static DWORD g_lastRead = 0;
static ID3D11Texture2D* MakeStaging() {
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = g_rtW; d.Height = g_rtH; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* t = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &t))) return nullptr;
    return t;
}
static void RingRelease() {
    for (int i = 0; i < 2; i++) { if (g_ring[i]) g_ring[i]->Release(); g_ring[i] = nullptr; g_ringValid[i] = false; }
    g_ringNewest = -1;
}
static void RingCopy() {                    // called at present
    if (!g_lastRead || GetTickCount() - g_lastRead > 3000) { if (g_ring[0]) RingRelease(); return; }
    int i = g_ringNext;
    if (!g_ring[i]) g_ring[i] = MakeStaging();
    if (!g_ring[i]) return;
    Resolve();
    g_ctx->CopyResource(g_ring[i], g_rtResolve);
    g_ringValid[i] = true; g_ringNewest = i; g_ringNext ^= 1;
}

bool ReadBack(const RECT& r, uint32_t* out, int pitch, bool recentOk) {
    LOCKED;
    if (!g_ready || r.right <= r.left || r.bottom <= r.top) return false;
    g_lastRead = GetTickCount();
    D3D11_MAPPED_SUBRESOURCE m;
    if (recentOk && g_ringNewest >= 0 && g_ringValid[g_ringNewest]) {
        ID3D11Texture2D* t = g_ring[g_ringNewest];
        if (SUCCEEDED(g_ctx->Map(t, 0, D3D11_MAP_READ, 0, &m))) { CopyOut(m, r, out, pitch); g_ctx->Unmap(t, 0); return true; }
    }
    if (!g_staging) g_staging = MakeStaging();
    if (!g_staging) return false;
    Resolve();
    g_ctx->CopyResource(g_staging, g_rtResolve);
    if (FAILED(g_ctx->Map(g_staging, 0, D3D11_MAP_READ, 0, &m))) return false;
    CopyOut(m, r, out, pitch);
    g_ctx->Unmap(g_staging, 0);
    return true;
}

// ---- overlay (QoL text and menus, drawn at output resolution) ----------------------------------
struct OvQuad { Tex* t; int x, y, w, h; };
static Vec<OvQuad> g_ov;
void OverlayClear() { LOCKED; g_ov.clear(); }
void OverlayAdd(Tex* t, int x, int y, int w, int h) { LOCKED; if (t) g_ov.push({t, x, y, w, h}); }
void SetPresentCallback(void (*fn)()) { g_presentCb = fn; }

// ---- present -----------------------------------------------------------------------------------
static void ResizeIfNeeded() {
    if (g_cfg.fullscreen && g_cfg.outputW > 0) return;    // fixed output size, scaled by DXGI
    RECT c; GetClientRect(g_hwnd, &c);
    if (c.right <= 0 || c.bottom <= 0 || (c.right == g_outW && c.bottom == g_outH)) return;
    if (!g_cfg.fullscreen) return;                        // windowed: stretched, size stays
    g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
    g_bbRTV->Release(); g_bbRTV = nullptr;
    if (FAILED(g_swap->ResizeBuffers(0, c.right, c.bottom, DXGI_FORMAT_UNKNOWN, 0))) { Log("swap chain resize failed"); }
    else { g_outW = c.right; g_outH = c.bottom; }
    ID3D11Texture2D* bb = nullptr;
    if (SUCCEEDED(g_swap->GetBuffer(0, IID_ID3D11Texture2D, (void**)&bb))) { g_dev->CreateRenderTargetView(bb, nullptr, &g_bbRTV); bb->Release(); }
    CreateTargets();
}

void Present() {
    void (*cb)() = g_presentCb;
    if (cb) cb();                              // outside the lock: callbacks call back into us
    LOCKED;
    if (!g_ready || !g_bbRTV) return;
    Resolve();
    // the game's picture onto the swap chain, centred, with black bars
    g_ctx->OMSetRenderTargets(1, &g_bbRTV, nullptr);
    float black[4] = {0, 0, 0, 1};
    g_ctx->ClearRenderTargetView(g_bbRTV, black);
    D3D11_VIEWPORT v = {0, 0, (float)g_outW, (float)g_outH, 0, 1};
    g_ctx->RSSetViewports(1, &v);
    Pipe p; Pipe2D(p, nullptr, false, false, false);
    p.srv[0] = g_rtSRV;
    p.cb.vp[2] = (float)g_outW; p.cb.vp[3] = (float)g_outH;
    p.cb.s0[0] = D3DTOP_SELECTARG1; p.cb.s0[1] = D3DTA_TEXTURE; p.cb.s0[3] = D3DTOP_SELECTARG1; p.cb.s0b[0] = D3DTA_TEXTURE;
    Apply(p);
    float x0 = (float)g_offX, y0 = (float)g_offY;
    DrawQuad(x0, y0, x0 + g_rtW, y0 + g_rtH, 0, 0xffffffff, 0, 0, 1, 1);
    // overlay quads (premultiplied alpha)
    if (g_ov.size()) {
        BlendKey bk = {1, 0, 0, 1};
        for (auto& q : g_ov) {
            Pipe o; Pipe2D(o, q.t, true, false, false);
            o.cb.vp[2] = (float)g_outW; o.cb.vp[3] = (float)g_outH;
            Apply(o);
            g_ctx->OMSetBlendState(GetBlend(bk), nullptr, 0xffffffff);
            DrawQuad((float)q.x, (float)q.y, (float)(q.x + q.w), (float)(q.y + q.h), 0, 0xffffffff, 0, 0, 1, 1);
        }
    }
    ID3D11ShaderResourceView* none[2] = {};
    g_ctx->PSSetShaderResources(0, 2, none);
    RingCopy();
    HRESULT hr = g_swap->Present(g_cfg.vsync ? 1 : 0, 0);
    if (FAILED(hr)) LogOnce("presentfail", "Present failed %08lx (reason %08lx)", hr, g_dev->GetDeviceRemovedReason());
    ResizeIfNeeded();
    BindGameTarget();
}

void TraceState() {
    if (!Tracing()) return;
    const GameState& g = g_gs;
    TraceLog("STATE z %u zwrite %u zfunc %u | blend %u src %u dst %u | atest %u afunc %u aref %u | colorkey %u | cull %u fill %u zbias %u",
             g.zenable, g.zwrite, g.zfunc, g.blend, g.src, g.dst, g.atest, g.afunc, g.aref, g.colorkey, g.cull, g.fill, g.zbias);
    TraceLog("STATE fog %u mode %u color %08x start %g end %g density %g | specular %u | tfactor %08x",
             g.fog, g.fogmode, g.fogcolor, g.fogstart, g.fogend, g.fogdensity, g.specular, g.tfactor);
    for (int i = 0; i < 2; i++) {
        const Stage& s = g.st[i];
        TraceLog("STATE stage %d: colorop %u (arg1 %u arg2 %u) alphaop %u (arg1 %u arg2 %u) texcoord %u addr %u/%u mag %u min %u mip %u | texture %s key %d",
                 i, s.colorop, s.carg1, s.carg2, s.alphaop, s.aarg1, s.aarg2, s.texcoord, s.addrU, s.addrV, s.mag, s.min, s.mip, s.tex ? "yes" : "none", s.keyed);
    }
    TraceLog("STATE viewport %d,%d %dx%d", g.vpX, g.vpY, g.vpW, g.vpH);
}

}  // namespace be
