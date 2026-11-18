// Nametags: each officer's name above their head. Characters are read from the game's entity
// list and put into camera space with the game's own view matrix and projection.
#include "qol.h"
#include <math.h>

namespace tags {

static Vec<CharView> g_chars;
static bool g_scanned, g_valid;
static Projection g_proj;
static bool g_loggedTags;

void NewFrame() { g_scanned = false; }

static uintptr_t IsClassFn() {
    static uintptr_t f = 0;
    if (!f) {
        uint8_t* t = TestAddr("SWAT3D9_TEST_ISCLASS");
        if (t) f = (uintptr_t)t;
        else if (Readable((void*)0x434780, 4) && !memcmp((void*)0x434780, "\x8b\x01\x56\x57", 4)) f = 0x434780;
        else { f = 1; Log("nametags: this swat.exe build is not recognised - off"); }
    }
    return f;
}

static bool Scan() {
    g_chars.clear();
    uint8_t* eng = QolEngine();
    if (!eng || !Readable(eng, 0x4e0)) return false;
    uint8_t* s = *(uint8_t**)(eng + 0x4d8);
    int wrapOff, cntOff;
    if (s) { wrapOff = 0x4040; cntOff = 0x8040; }
    else { s = *(uint8_t**)(eng + 0x4d4); wrapOff = 0x409c; cntOff = 0x809c; }
    if (!s || !Readable(s, cntOff + 4)) return false;
    int n = *(int*)(s + cntOff);
    if (n < 1 || n > 0x1000) return false;
    uint8_t* r = QolRenderer();
    if (!r || !Readable(r, 0x80)) return false;
    uint8_t* mm = *(uint8_t**)(r + 0x7c);
    if (!mm || !Readable(mm, 0x5c)) return false;
    g_proj.kx = *(float*)(r + 0x4c); g_proj.cx = *(float*)(r + 0x44);
    g_proj.ky = *(float*)(r + 0x50); g_proj.cy = *(float*)(r + 0x48);
    const float* M = (const float*)(mm + 0x2c);   // 3x3 rotation (by columns) + translation
    typedef int(__thiscall * IsClass)(void*, const char*);
    if (IsClassFn() == 1) return false;
    IsClass isClass = (IsClass)IsClassFn();
    uint8_t** wr = (uint8_t**)(s + wrapOff);
    for (int i = 0; i < n; i++) {
        uint8_t* w = wr[i];
        if (!w || !Readable(w, 0x38)) continue;
        uint8_t* c = *(uint8_t**)(w + 0x34);
        if (!c || !Readable(c, 0xdf4) || !isClass(c, "Char")) continue;
        CharView v = {};
        v.c = c;
        memcpy(v.name, w + 0x12, 32); v.name[32] = 0;
        v.kind = c[0xb30]; v.role = c[0xb08]; v.active = c[0x278] != 0; v.state = c[0xdf0];
        float eye = *(float*)(c + 0x9d4);
        if (!(eye > 10 && eye < 120)) eye = 60;
        v.eye = eye;
        float x = *(float*)(c + 0x1c), y = *(float*)(c + 0x20), z = *(float*)(c + 0x24);
        float ys[2] = {y + eye + 14, y + eye * 0.6f};
        float* out[2] = {v.head, v.chest};
        for (int k = 0; k < 2; k++)
            for (int j = 0; j < 3; j++) out[k][j] = M[j] * x + M[3 + j] * ys[k] + M[6 + j] * z + M[9 + j];
        v.dist = sqrtf(v.chest[0] * v.chest[0] + v.chest[1] * v.chest[1] + v.chest[2] * v.chest[2]);
        g_chars.push(v);
    }
    return true;
}

const Vec<CharView>* Chars(Projection* proj) {
    if (!g_scanned) { g_scanned = true; g_valid = Scan(); }
    if (!g_valid) return nullptr;
    if (proj) *proj = g_proj;
    return &g_chars;
}

struct Tag { int x, y; COLORREF color; char text[33]; float dist; };

void Draw() {
    if (!g_qol.tagsOn) return;
    Projection p;
    const Vec<CharView>* cs = Chars(&p);
    if (!cs) return;
    int vw, vh; be::GetVirtualMode(&vw, &vh);
    float maxD = g_qol.tagsMaxDist > 0 ? (float)g_qol.tagsMaxDist : 1e9f;
    Vec<Tag> list;
    for (auto& v : *cs) {
        if (v.kind != 1 || !v.name[0]) continue;
        float z = v.head[2];
        if (z < 8) continue;
        float d = sqrtf(v.head[0] * v.head[0] + v.head[1] * v.head[1] + z * z);
        if (d < 40 || d > maxD) continue;
        float sx = v.head[0] * p.kx / z + p.cx, sy = p.cy - v.head[1] * p.ky / z;
        if (sx < -200 || sx > vw + 200 || sy < -100 || sy > vh + 100) continue;
        Tag t = {};
        t.x = (int)sx; t.y = (int)sy; t.dist = d;
        int r = v.role;
        t.color = (r == 1 || r == 2 || r == 6 || r == 7) ? 0x3c3ceb : (r == 3 || r == 4 || r == 8 || r == 9) ? 0xff8250 : 0xebebeb;
        for (int i = 0; i < 32 && v.name[i]; i++) t.text[i] = (char)toupper((unsigned char)v.name[i]);
        list.push(t);
    }
    // far ones first so near names are drawn on top
    for (int i = 1; i < list.size(); i++)
        for (int j = i; j > 0 && list[j - 1].dist < list[j].dist; j--) { Tag t = list[j]; list[j] = list[j - 1]; list[j - 1] = t; }
    if (list.size() && !g_loggedTags) {
        g_loggedTags = true;
        for (auto& t : list) Log("nametag: %s at %d,%d (distance %.0f)", t.text, t.x, t.y, t.dist);
    }
    int ow, oh; be::GetOutputSize(&ow, &oh);
    int h = g_qol.tagsTextSize > 0 ? g_qol.tagsTextSize : oh / 68;
    if (h < 11) h = 11;
    for (auto& t : list) {
        int x, y; VirtToOut(t.x, t.y, &x, &y);
        OverlayText(t.text, t.color, h, x, y, 0);
    }
}

}  // namespace tags
