// Quick restrain: look at someone who has complied (hands up), get close, and a "[F]  Restrain"
// prompt appears; the interact key then restrains them on the spot through the game's own
// "secure" routine (the cuffs never come out). Anywhere else the interact key works as normal.
#include "qol.h"
#include <stdio.h>
#include <math.h>

namespace qc {

static int g_interact;             // swat.cfg key code for "usegeneric"
static uint8_t* g_player;          // our own character (nearest officer to the camera)
static uint8_t* g_cand;            // who the prompt is for this frame
static bool g_swallowUp;           // the interact key went to us; swallow its release too
static char g_prompt[64];
static uintptr_t g_trace;          // the game's line-of-sight trace (0 = none)
static uintptr_t g_secure = 0x5e5580, g_notify = 0x42f7e0;
static bool g_on;

static int CfgKey(const char* name) {
    char path[MAX_PATH]; _snprintf(path, MAX_PATH, "%sswat.cfg", g_dir); path[MAX_PATH - 1] = 0;
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 0;
    DWORD size = GetFileSize(f, nullptr), got = 0;
    if (size == INVALID_FILE_SIZE || size > (1 << 20)) { CloseHandle(f); return 0; }
    char* buf = (char*)malloc(size + 1);
    ReadFile(f, buf, size, &got, nullptr); CloseHandle(f);
    buf[got] = 0;
    int code = 0;
    for (char* line = buf; line && *line;) {
        char* next = strchr(line, '\n');
        if (next) *next++ = 0;
        char* eq = strchr(line, '=');
        if (eq && line[0] >= '0' && line[0] <= '9') {
            char* v = eq + 1;
            v[strcspn(v, " \t\r\n")] = 0;
            if (!_stricmp(v, name)) { code = atoi(line); break; }
        }
        line = next;
    }
    free(buf);
    return code;
}

static float EyeOf(uint8_t* c) { float e = *(float*)(c + 0x9d4); return e > 10 && e < 120 ? e : 60; }

static bool LineOfSight(uint8_t* player, uint8_t* target) {
    if (!g_trace) return true;
    float from[3] = {*(float*)(player + 0x1c), *(float*)(player + 0x20) + EyeOf(player), *(float*)(player + 0x24)};
    float to[3] = {*(float*)(target + 0x1c), *(float*)(target + 0x20) + EyeOf(target) * 0.6f, *(float*)(target + 0x24)};
    float res[16] = {};
    res[4] = 1e8f;
    typedef void(__cdecl * TraceFn)(float*, float*, float*, void*, int, int);
    ((TraceFn)g_trace)(res, from, to, player, 0x10088, 0);
    return !(res[4] < 1e8f);
}

void Update() {
    g_cand = nullptr;
    if (!g_on || !ctx::InGame() || ctx::Active()) return;
    const Vec<tags::CharView>* cs = tags::Chars(nullptr);
    if (!cs || cs->empty()) return;
    // our own character: the officer nearest the camera
    uint8_t* me = nullptr; float best = 1e9f;
    for (auto& v : *cs) if (v.kind == 1 && v.dist < best) { best = v.dist; me = v.c; }
    if (me && best < 150) g_player = me;
    // someone complying, close, near the crosshair (anywhere from the knees to the head)
    const tags::CharView* pick = nullptr; float bestOff = 14;
    float reach = (float)g_qol.qcReach;
    for (auto& v : *cs) {
        if (v.kind == 1 || !v.active || v.state != 1) continue;
        if (!(v.chest[2] > 1) || v.dist > reach) continue;
        float off = 1e9f;
        for (int i = 0; i < 7; i++) {
            float t = -0.9f + i * 0.31666666f;
            float px = v.chest[0] + t * (v.head[0] - v.chest[0]);
            float py = v.chest[1] + t * (v.head[1] - v.chest[1]);
            float pz = v.chest[2] + t * (v.head[2] - v.chest[2]);
            if (pz <= 1) continue;
            float d = sqrtf(px * px + py * py);
            if (d < off) off = d;
        }
        if (off < bestOff) { bestOff = off; pick = &v; }
    }
    if (!pick || !g_player || !Readable(g_player, 0xdf4)) return;
    if (!LineOfSight(g_player, pick->c)) return;
    g_cand = pick->c;
    int vw, vh; be::GetVirtualMode(&vw, &vh);
    int ow, oh; be::GetOutputSize(&ow, &oh);
    int x, y; VirtToOut(vw / 2.0, vh / 2.0 + vh / 9.0, &x, &y);
    int fh = oh / 36; if (fh < 14) fh = 14;
    OverlayText(g_prompt, 0xffffff, fh, x, y, 1);
}

static void Restrain(uint8_t* t) {
    uint8_t* me = g_player;
    if (!me || !Readable(me, 0xdf4) || me[0xb30] != 1) { Log("quick restrain: own character not found - not restrained"); return; }
    if (!t || !Readable(t, 0xdf4) || !t[0x278] || t[0xdf0] != 1) { Log("quick restrain: person no longer complied"); return; }
    typedef int(__thiscall * SecureFn)(void*, int, void*);
    typedef int(__cdecl * NotifyFn)(void*);
    ((SecureFn)g_secure)(t, 0, me);
    ((NotifyFn)g_notify)(t);
    int st = t[0xdf0];
    if (st == 2) Log("quick restrain: restrained");
    else Log("quick restrain: the game did not restrain them (state %d)", st);
}

bool OnMessage(HWND, UINT m, WPARAM wp, LPARAM lp) {
    if (!g_on || !g_interact) return false;
    bool rep = false;
    int k = KeyOfMessage(m, wp, lp, &rep);
    if (k == -g_interact && g_swallowUp) { g_swallowUp = false; return true; }
    if (k != g_interact) return false;
    if (rep) return g_swallowUp;
    if (!g_cand || ctx::Active()) return false;
    uint8_t* t = g_cand;
    g_cand = nullptr;
    g_swallowUp = true;
    Restrain(t);
    return true;
}

void Init() {
    g_on = g_qol.qcOn != 0;
    g_interact = CfgKey("usegeneric");
    if (uint8_t* t = TestAddr("SWAT3D9_TEST_SECURE")) g_secure = (uintptr_t)t;
    if (uint8_t* t = TestAddr("SWAT3D9_TEST_NOTIFY")) g_notify = (uintptr_t)t;
    if (g_on && !g_interact) { Log("quick restrain: the interact key (usegeneric) is not bound in swat.cfg - off"); g_on = false; }
    char kn[32];
    _snprintf(g_prompt, sizeof(g_prompt), "[%s]  Restrain", KeyName(g_interact, kn, sizeof(kn)));
    g_prompt[sizeof(g_prompt) - 1] = 0;
    // the trace must be the routine this was made for (sub esp,0x88)
    g_trace = 0;
    if (uint8_t* t = TestAddr("SWAT3D9_TEST_TRACE")) g_trace = (uintptr_t)t;
    else if (Readable((void*)0x46f8b0, 6) && !memcmp((void*)0x46f8b0, "\x81\xec\x88\x00\x00\x00", 6)) g_trace = 0x46f8b0;
    if (g_on && !g_trace) Log("quick restrain: trace not recognised - no line-of-sight check");
    bool known = (g_secure != 0x5e5580 || (Readable((void*)g_secure, 4) && !memcmp((void*)g_secure, "\x53\x8b\x5c\x24", 4))) &&
                 (g_notify != 0x42f7e0 || (Readable((void*)g_notify, 6) && !memcmp((void*)g_notify, "\x8b\x0d\x88\x98\x6c\x00", 6)));
    if (g_on && !known) { Log("quick restrain: this swat.exe build is not recognised - off"); g_on = false; }
    Log("quick restrain: %s (interact key %s, reach %d, restrains on the spot)", g_on ? "on" : "off", kn, g_qol.qcReach);
}

}  // namespace qc
