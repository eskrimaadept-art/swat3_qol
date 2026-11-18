// HLSL for the D3D11 backend: one vertex shader for DirectX 6 screen-space vertices and one "uber"
// pixel shader emulating two texture stages, alpha test, specular and fog.
#pragma once
static const char kShaderSrc[] = R"HLSL(
cbuffer Frame : register(b0) {
    float4 g_vp;         // viewport x, y, width, height (render-target pixels)
    float4 g_fogColor;
    float4 g_fogParams;  // start, end, density, alpha reference (0..1)
    float4 g_tfactor;
    int4   g_s0;         // stage 0: colour op, colour arg1, colour arg2, alpha op
    int4   g_s0b;        //          alpha arg1, alpha arg2, texcoord index, unused
    int4   g_s1;         // stage 1, same layout
    int4   g_s1b;
    int4   g_misc;       // alpha test func (0 = off, else D3DCMP 1..8), fog mode (0 off, 1 vertex, 2 exp, 3 exp2, 4 linear), specular add,
                         // sharp scaling of stage 0 (output pixels per texel x 1000, 0 = off)
};
Texture2D    g_t0 : register(t0);
Texture2D    g_t1 : register(t1);
SamplerState g_smp0 : register(s0);
SamplerState g_smp1 : register(s1);

struct VSIn  { float4 pos : POSITION; float4 dif : COLOR0; float4 spec : COLOR1; float2 uv0 : TEXCOORD0; float2 uv1 : TEXCOORD1; };
struct VSOut { float4 pos : SV_Position; float4 dif : COLOR0; float4 spec : COLOR1; float2 uv0 : TEXCOORD0; float2 uv1 : TEXCOORD1; };

// Vertices arrive in DirectX 6 screen space: x, y in pixels (pixel centres on integers), z the
// depth value, w the reciprocal of the homogeneous w.
// 1999 hardware rasterised these exactly where they are: no clipping against the near/far planes
// (depth saturates) and no rejection of a negative w. SWAT 3 relies on that: mirror reflections
// are drawn with z slightly above 1 and negative reciprocal w. So only the magnitude of w is used
// (it weights the perspective-correct interpolation), and depth clipping is off (depth clamps).
VSOut VSMain(VSIn i) {
    VSOut o;
    float w = 1.0;
    float rhw = abs(i.pos.w);
    if (rhw > 0.0000001) w = 1.0 / rhw;
    float x = (i.pos.x + 0.5 - g_vp.x) / g_vp.z * 2.0 - 1.0;
    float y = 1.0 - (i.pos.y + 0.5 - g_vp.y) / g_vp.w * 2.0;
    o.pos = float4(x * w, y * w, i.pos.z * w, w);
    o.dif = i.dif;
    o.spec = i.spec;
    o.uv0 = i.uv0;
    o.uv1 = i.uv1;
    return o;
}

float4 PickArg(int a, float4 cur, float4 tex, float4 dif, float4 spec) {
    int sel = a & 15;
    float4 v = dif;
    if (sel == 1) v = cur;
    else if (sel == 2) v = tex;
    else if (sel == 3) v = g_tfactor;
    else if (sel == 4) v = spec;
    if ((a & 16) != 0) v = float4(1.0, 1.0, 1.0, 1.0) - v;
    if ((a & 32) != 0) v = v.aaaa;
    return v;
}

float4 Combine(int op, float4 a1, float4 a2, float4 cur, float4 tex, float4 dif) {
    float4 r = a1;
    if (op == 3) r = a2;
    else if (op == 4) r = a1 * a2;
    else if (op == 5) r = a1 * a2 * 2.0;
    else if (op == 6) r = a1 * a2 * 4.0;
    else if (op == 7) r = a1 + a2;
    else if (op == 8) r = a1 + a2 - 0.5;
    else if (op == 9) r = (a1 + a2 - 0.5) * 2.0;
    else if (op == 10) r = a1 - a2;
    else if (op == 11) r = a1 + a2 - a1 * a2;
    else if (op == 12) r = lerp(a2, a1, dif.a);
    else if (op == 13) r = lerp(a2, a1, tex.a);
    else if (op == 14) r = lerp(a2, a1, g_tfactor.a);
    else if (op == 15) r = a1 + a2 * (1.0 - tex.a);
    else if (op == 16) r = lerp(a2, a1, cur.a);
    return saturate(r);
}

// "Sharp bilinear": each texel is drawn as a crisp block; only the one output pixel where two
// texels meet is blended. Scaled 2D art and text keep even, sharp shapes at any scale.
float2 SharpUV(Texture2D t, float2 uv, float k) {
    float w, h;
    t.GetDimensions(w, h);
    float2 size = float2(w, h);
    float2 texel = uv * size;
    float2 fl = floor(texel);
    float2 cd = (texel - fl) - 0.5;
    float range = 0.5 - 0.5 / k;
    float2 f = (cd - clamp(cd, -range, range)) * k + 0.5;
    return (fl + f) / size;
}

float4 PSMain(VSOut i) : SV_Target {
    float2 c0 = i.uv0;
    if (g_s0b.z == 1) c0 = i.uv1;
    float2 c1 = i.uv1;
    if (g_s1b.z == 0) c1 = i.uv0;
    if (g_misc.w > 1000) c0 = SharpUV(g_t0, c0, g_misc.w / 1000.0);
    float4 t0 = g_t0.Sample(g_smp0, c0);
    float4 t1 = g_t1.Sample(g_smp1, c1);
    float4 dif = i.dif;
    float4 spec = i.spec;
    float4 cur = dif;

    if (g_s0.x != 1) {
        float4 c = Combine(g_s0.x, PickArg(g_s0.y, cur, t0, dif, spec), PickArg(g_s0.z, cur, t0, dif, spec), cur, t0, dif);
        float4 a = cur;
        if (g_s0.w != 1) a = Combine(g_s0.w, PickArg(g_s0b.x, cur, t0, dif, spec), PickArg(g_s0b.y, cur, t0, dif, spec), cur, t0, dif);
        cur = float4(c.rgb, a.a);
        if (g_s1.x != 1) {
            float4 c2 = Combine(g_s1.x, PickArg(g_s1.y, cur, t1, dif, spec), PickArg(g_s1.z, cur, t1, dif, spec), cur, t1, dif);
            float4 a2 = cur;
            if (g_s1.w != 1) a2 = Combine(g_s1.w, PickArg(g_s1b.x, cur, t1, dif, spec), PickArg(g_s1b.y, cur, t1, dif, spec), cur, t1, dif);
            cur = float4(c2.rgb, a2.a);
        }
    }

    int af = g_misc.x;
    if (af != 0) {
        float a8 = floor(cur.a * 255.0 + 0.5);
        float r8 = floor(g_fogParams.w * 255.0 + 0.5);
        bool keep = true;
        if (af == 1) keep = false;
        else if (af == 2) keep = a8 < r8;
        else if (af == 3) keep = a8 == r8;
        else if (af == 4) keep = a8 <= r8;
        else if (af == 5) keep = a8 > r8;
        else if (af == 6) keep = a8 != r8;
        else if (af == 7) keep = a8 >= r8;
        if (!keep) discard;
    }

    if (g_misc.z != 0) cur.rgb = saturate(cur.rgb + spec.rgb);

    int fm = g_misc.y;
    if (fm != 0) {
        float f = spec.a;
        float d = i.pos.z;
        if (fm == 2) f = exp(-g_fogParams.z * d);
        else if (fm == 3) f = exp(-(g_fogParams.z * d) * (g_fogParams.z * d));
        else if (fm == 4) f = (g_fogParams.y - d) / max(g_fogParams.y - g_fogParams.x, 0.000001);
        cur.rgb = lerp(g_fogColor.rgb, cur.rgb, saturate(f));
    }
    return cur;
}
)HLSL";
