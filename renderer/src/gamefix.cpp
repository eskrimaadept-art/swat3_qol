// Changes to swat.exe itself, made in memory when the game starts (the exe file is not touched).
// They give the ORIGINAL swat.exe what the Last Resort exe had built in, plus a few options:
// texture size limit, draw distance, every resolution in the video options, a forced mission
// resolution, intro skip and field of view. Every patch first checks that the bytes are what this
// game version has; with any other exe it does nothing and says so in the log.
#include "gamefix.h"
#include <stdio.h>
#include <math.h>

// Writes bytes into the game's code / data if the current bytes are `expect` (nullptr = any)
static bool Patch(uintptr_t addr, const void* expect, const void* bytes, size_t n) {
    uint8_t* p = (uint8_t*)addr;
    if (!Readable(p, n)) return false;
    if (expect && memcmp(p, expect, n)) return false;
    DWORD old;
    if (!VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(p, bytes, n);
    VirtualProtect(p, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, n);
    return true;
}
static bool Bytes(uintptr_t addr, const void* expect, size_t n) { return Readable((void*)addr, n) && !memcmp((void*)addr, expect, n); }

// ---- intro (runs at DLL load, before the game reads its command line) ---------------------------
void GameFixEarly() {
    if (!g_cfg.gameFixes || !g_cfg.skipIntro) return;
    // the game's "-nointro" switch clears this flag; the string next to it identifies the exe
    if (!Bytes(0x695d70, "-nointro", 9) || !Readable((void*)0x69525c, 4) || *(uint32_t*)0x69525c > 1) {
        Log("skip intro: this swat.exe build is not recognised - intro left on");
        return;
    }
    uint32_t zero = 0;
    if (Patch(0x69525c, nullptr, &zero, 4)) Log("skip intro: on");
    else Log("skip intro: could not change the setting");
}

// ---- texture size limit -------------------------------------------------------------------------
static void PatchTextureLimit() {
    int lim = g_cfg.maxTexture;
    if (lim != 256 && lim != 512 && lim != 1024 && lim != 2048 && lim != 4096) {
        Log("MaxTextureSize=%d ignored (use 256, 512, 1024, 2048 or 4096)", lim);
        return;
    }
    // cmp reg, imm32 (4 places), cmp cx/ax, imm16 (2 places), and the two "push 256" of the error message
    static const uintptr_t imm32[4] = {0x565f82, 0x565f8a, 0x566278, 0x566280};
    static const uintptr_t imm16[2] = {0x5844b4, 0x5844c4};
    static const uintptr_t msg[2] = {0x565f97, 0x56628c};
    if (!Bytes(0x565f80, "\x81\xff", 2) || !Bytes(0x565f88, "\x81\xfb", 2) || !Bytes(0x566276, "\x81\xfe", 2) ||
        !Bytes(0x56627e, "\x81\xfb", 2) || !Bytes(0x5844b1, "\x66\x81\xf9", 3) || !Bytes(0x5844c2, "\x66\x3d", 2) ||
        !Bytes(0x565f96, "\x68", 1) || !Bytes(0x56628b, "\x68", 1)) {
        Log("texture size limit: this swat.exe build is not recognised - left unchanged");
        return;
    }
    uint32_t cur = *(uint32_t*)imm32[0];
    if (cur != 256 && cur != 512 && cur != 1024 && cur != 2048 && cur != 4096) { Log("texture size limit: unexpected value %u - left unchanged", cur); return; }
    if ((int)cur == lim && *(uint32_t*)msg[0] == (uint32_t)lim) { Log("texture size limit: already %d", lim); return; }
    uint32_t v32 = (uint32_t)lim; uint16_t v16 = (uint16_t)lim;
    for (uintptr_t a : imm32) Patch(a, nullptr, &v32, 4);
    for (uintptr_t a : imm16) Patch(a, nullptr, &v16, 2);
    for (uintptr_t a : msg) Patch(a, nullptr, &v32, 4);
    Log("texture size limit: %u -> %d", cur, lim);
}

// ---- draw distance (far clipping plane) --------------------------------------------------------
static void PatchDrawDistance() {
    int d = g_cfg.drawDistance;
    if (d < 700 || d > 6000) { Log("DrawDistance=%d ignored (use 700 to 6000; 1400 = original game, 2800 = Last Resort)", d); return; }
    // push <float far plane> / lea ecx, ...
    if (!Bytes(0x4064f5, "\x68", 1) || !Bytes(0x4064fa, "\x8d", 1)) { Log("draw distance: this swat.exe build is not recognised - left unchanged"); return; }
    float cur; memcpy(&cur, (void*)0x4064f6, 4);
    if (!(cur >= 500 && cur <= 10000)) { Log("draw distance: unexpected value %g - left unchanged", cur); return; }
    float f = (float)d;
    if (cur == f) { Log("draw distance: already %d", d); return; }
    Patch(0x4064f6, nullptr, &f, 4);
    Log("draw distance: %g -> %d", cur, d);
}

// ---- every resolution in the video options ------------------------------------------------------
static void PatchModeFilter() {
    if (!g_cfg.allResolutions) return;
    // the mode list callback keeps only five classic sizes; jump straight to "keep this mode"
    static const uint8_t orig[8] = {0x81, 0xf9, 0x80, 0x02, 0x00, 0x00, 0x75, 0x07};
    static const uint8_t jmp[2] = {0xeb, 0x51};
    if (Bytes(0x55cff1, jmp, 2)) { Log("in-game resolutions: already unlocked"); return; }
    if (!Bytes(0x55cff1, orig, 8)) { Log("in-game resolutions: this swat.exe build is not recognised - unchanged"); return; }
    Patch(0x55cff1, orig, jmp, 2);
    Log("in-game resolutions: all sizes up to 2048 pixels unlocked in the game's video options");
}

// ---- forced mission resolution -----------------------------------------------------------------
typedef int(__thiscall* SetModeFn)(void* engine, int w, int h, int is3D, int flag);
static SetModeFn g_setMode;
static int g_forceW, g_forceH;

static int __fastcall SetModeHook(void* engine, void*, int w, int h, int is3D, int flag) {
    if (is3D == 1 && g_forceW > 0) {
        int tw = g_forceW, th = g_forceH;
        if (tw != w || th != h) Log("mission resolution: %dx%d instead of the game's %dx%d", tw, th, w, h);
        // the game falls back to these if the mode is refused
        int* fw = (int*)0x6c0d68; int* fh = (int*)0x6c0d6c;
        int ow = *fw, oh = *fh;
        *fw = tw; *fh = th;
        int r = g_setMode(engine, tw, th, is3D, flag);
        *fw = ow; *fh = oh;
        return r;
    }
    return g_setMode(engine, w, h, is3D, flag);
}

static void HookSetMode() {
    const char* m = g_cfg.missionRes;
    if (!_stricmp(m, "game") || !m[0]) { Log("mission resolution: left to the game"); return; }
    int w = 0, h = 0;
    if (!_stricmp(m, "desktop")) {
        w = GetSystemMetrics(SM_CXSCREEN); h = GetSystemMetrics(SM_CYSCREEN);
        if (w > 2048 || h > 2048) { double s = 2048.0 / (w > h ? w : h); w = (int)(w * s); h = (int)(h * s); }
    } else if (sscanf(m, "%dx%d", &w, &h) != 2 || w < 640 || h < 480 || w > 2048 || h > 2048) {
        Log("MissionResolution=%s not understood (use desktop, game, or e.g. 1920x1080; at most 2048 each way)", m);
        return;
    }
    uintptr_t slot = 0x65d4c8;
    if (!Readable((void*)slot, 4) || *(uintptr_t*)slot != 0x407c30) { Log("mission resolution: this swat.exe build is not recognised - left to the game"); return; }
    g_setMode = (SetModeFn)0x407c30;
    g_forceW = w; g_forceH = h;
    void* hook = (void*)SetModeHook;
    Patch(slot, nullptr, &hook, 4);
    if (!_stricmp(m, "desktop")) Log("mission resolution: desktop size (%dx%d)", w, h); else Log("mission resolution: %dx%d", w, h);
}

// ---- field of view ----------------------------------------------------------------------------
// The camera setup calls FUN_00579030(camera, a, b, verticalFov). The game's own vertical field
// of view is 60 degrees; zoomed views (scopes, Optiwand) pass less. We scale all of them by the same
// factor, so the chosen horizontal field of view applies to the normal view and zoom keeps working.
static int __fastcall CamSetThunk(void* cam, void*, float a, float b, float vfov) {
    int fov = g_cfg.fov;
    if (fov > 0) {
        uint8_t* engine = Readable((void*)0x6cad10, 4) ? *(uint8_t**)0x6cad10 : nullptr;
        int sw = 4, sh = 3;
        if (Readable(engine, 0x228)) { sw = *(int*)(engine + 0x220); sh = *(int*)(engine + 0x224); }
        if (sw > 0 && sh > 0) {
            double h = fov < 40 ? 40 : fov > 150 ? 150 : fov;
            double halfV = atan(tan(h * 3.14159265358979 / 360.0) * sh / sw);
            double k = tan(halfV) / 0.5773502691896251;           // relative to tan(30 degrees)
            vfov = (float)(2.0 * atan(k * tan(vfov * 0.5)));
        }
    }
    typedef int(__thiscall * Fn)(void*, float, float, float);
    return ((Fn)0x579030)(cam, a, b, vfov);
}
static void InstallFovPatch() {
    if (g_cfg.fov <= 0) return;
    static const uint8_t call[5] = {0xe8, 0x84, 0xe3, 0x01, 0x00};
    if (!Bytes(0x55aca7, call, 5)) { Log("FieldOfView: this swat.exe build is not recognised - FOV unchanged"); return; }
    int32_t rel = (int32_t)((uintptr_t)CamSetThunk - (0x55aca7 + 5));
    Patch(0x55aca8, nullptr, &rel, 4);
    Log("FieldOfView set to %d degrees (horizontal)", g_cfg.fov);
}

void GameFixInstall() {
    static bool done = false;
    if (done) return;
    done = true;
    InstallFovPatch();
    if (!g_cfg.gameFixes) { Log("game adjustments off"); return; }
    PatchTextureLimit();
    PatchDrawDistance();
    PatchModeFilter();
    HookSetMode();
}
