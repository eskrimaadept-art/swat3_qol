// Window and mouse translation. The game thinks it owns a screen of its chosen resolution
// (e.g. 800x600) at 0,0; the real window is the monitor (or a window) at another size. The
// game's user32 calls about the cursor and its window are redirected so it keeps seeing its own
// coordinates, and mouse messages are translated on the way in.
#include "hooks.h"
#include "backend.h"
#include "qol.h"
#include <math.h>

static HWND g_hwnd;
static WNDPROC g_oldProc;
static bool g_active = true;

typedef BOOL(WINAPI* GetCursorPosFn)(LPPOINT);
typedef BOOL(WINAPI* SetCursorPosFn)(int, int);
typedef BOOL(WINAPI* PtFn)(HWND, LPPOINT);
typedef BOOL(WINAPI* RectFn)(HWND, LPRECT);
typedef int(WINAPI* MetricsFn)(int);
typedef BOOL(WINAPI* SetWindowPosFn)(HWND, HWND, int, int, int, int, UINT);
typedef SHORT(WINAPI* KeyStateFn)(int);
static GetCursorPosFn r_GetCursorPos = GetCursorPos;
static SetCursorPosFn r_SetCursorPos = SetCursorPos;
static PtFn r_ScreenToClient = ScreenToClient, r_ClientToScreen = ClientToScreen;
static RectFn r_GetClientRect = GetClientRect, r_GetWindowRect = GetWindowRect;
static MetricsFn r_GetSystemMetrics = GetSystemMetrics;
static SetWindowPosFn r_SetWindowPos = SetWindowPos;
static KeyStateFn r_GetKeyState = GetKeyState;
static HookCallbacks g_cb;

static bool Mapped() { return g_hwnd && be::Ready(); }

// ---- cursor position ---------------------------------------------------------------------------
// The game reads the cursor and puts it back in the middle every frame (mouse look). One real
// pixel is less than one game pixel when the output is larger, so the remainder is carried over
// to keep slow mouse movements from being lost.
static bool g_haveAnchor = false;
static double g_anchorX, g_anchorY;      // game coordinates of the last SetCursorPos
static POINT g_anchorReal;               // where that put the real cursor (client pixels)
static double g_carryX, g_carryY;        // fraction carried into the current anchor
static double g_fracX, g_fracY;          // fraction of the last reading

static void RealToGame(int rx, int ry, double* gx, double* gy) {
    be::Mapping m = be::GetMapping();
    *gx = (rx - m.offX) / m.scale;
    *gy = (ry - m.offY) / m.scale;
}

static BOOL WINAPI h_GetCursorPos(LPPOINT p) {
    if (!p) return FALSE;
    if (!Mapped()) return r_GetCursorPos(p);
    POINT r;
    if (!r_GetCursorPos(&r)) return FALSE;
    r_ScreenToClient(g_hwnd, &r);
    if (g_cb.freezeCursor && g_cb.freezeCursor() && g_haveAnchor) {   // a popup menu owns the mouse
        int dx = r.x - g_anchorReal.x, dy = r.y - g_anchorReal.y;
        if (dx || dy) {                                // the movement scrolls the menu; the cursor goes back
            if (g_cb.mouseDelta) g_cb.mouseDelta(dx, dy);
            POINT s = g_anchorReal;
            r_ClientToScreen(g_hwnd, &s);
            r_SetCursorPos(s.x, s.y);
        }
        p->x = (LONG)floor(g_anchorX); p->y = (LONG)floor(g_anchorY);
        g_fracX = g_fracY = 0;
        return TRUE;
    }
    double gx, gy;
    if (g_haveAnchor) {
        be::Mapping m = be::GetMapping();
        gx = g_anchorX + (r.x - g_anchorReal.x) / m.scale + g_carryX;
        gy = g_anchorY + (r.y - g_anchorReal.y) / m.scale + g_carryY;
    } else RealToGame(r.x, r.y, &gx, &gy);
    double fx = floor(gx), fy = floor(gy);
    g_fracX = gx - fx; g_fracY = gy - fy;
    p->x = (LONG)fx; p->y = (LONG)fy;
    return TRUE;
}

static BOOL WINAPI h_SetCursorPos(int x, int y) {
    if (!Mapped()) return r_SetCursorPos(x, y);
    be::Mapping m = be::GetMapping();
    POINT r = {(LONG)floor(m.offX + (x + 0.5) * m.scale), (LONG)floor(m.offY + (y + 0.5) * m.scale)};
    g_carryX = g_fracX; g_carryY = g_fracY;
    g_anchorX = x; g_anchorY = y;
    g_anchorReal = r;
    g_haveAnchor = true;
    if (g_cb.freezeCursor && g_cb.freezeCursor()) return TRUE;      // leave the real cursor where the menu is
    POINT s = r;
    r_ClientToScreen(g_hwnd, &s);
    return r_SetCursorPos(s.x, s.y);
}

static BOOL WINAPI h_ScreenToClient(HWND h, LPPOINT p) { return Mapped() && h == g_hwnd ? TRUE : r_ScreenToClient(h, p); }
static BOOL WINAPI h_ClientToScreen(HWND h, LPPOINT p) { return Mapped() && h == g_hwnd ? TRUE : r_ClientToScreen(h, p); }
static BOOL WINAPI h_GetClientRect(HWND h, LPRECT r) {
    if (Mapped() && h == g_hwnd && r) { int w, hh; be::GetVirtualMode(&w, &hh); SetRect(r, 0, 0, w, hh); return TRUE; }
    return r_GetClientRect(h, r);
}
static BOOL WINAPI h_GetWindowRect(HWND h, LPRECT r) {
    if (Mapped() && h == g_hwnd && r) { int w, hh; be::GetVirtualMode(&w, &hh); SetRect(r, 0, 0, w, hh); return TRUE; }
    return r_GetWindowRect(h, r);
}
static int WINAPI h_GetSystemMetrics(int i) {
    if (Mapped() && (i == SM_CXSCREEN || i == SM_CYSCREEN || i == SM_CXFULLSCREEN || i == SM_CYFULLSCREEN)) {
        int w, h; be::GetVirtualMode(&w, &h);
        return i == SM_CXSCREEN || i == SM_CXFULLSCREEN ? w : h;
    }
    return r_GetSystemMetrics(i);
}
static BOOL WINAPI h_SetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, UINT flags) {
    // the game sizes its window to the display mode; the renderer decides the window's size
    if (h == g_hwnd && be::Ready()) {
        if (flags & SWP_SHOWWINDOW) ShowWindow(h, SW_SHOW);
        return TRUE;
    }
    return r_SetWindowPos(h, after, x, y, cx, cy, flags);
}
static SHORT WINAPI h_GetKeyState(int vk) {
    if (g_cb.fakeKeyDown && g_cb.fakeKeyDown(vk)) return (SHORT)0x8000;
    return r_GetKeyState(vk);
}

// ---- window messages ---------------------------------------------------------------------------
static void ClipToWindow(bool on) {
    if (on && g_cfg.fullscreen) { RECT r; r_GetWindowRect(g_hwnd, &r); ClipCursor(&r); }
    else ClipCursor(nullptr);
}

static LPARAM MapMouse(LPARAM lp) {
    double gx, gy;
    RealToGame((short)LOWORD(lp), (short)HIWORD(lp), &gx, &gy);
    int w, h; be::GetVirtualMode(&w, &h);
    int x = (int)floor(gx), y = (int)floor(gy);
    if (x < 0) x = 0; if (y < 0) y = 0; if (x >= w) x = w - 1; if (y >= h) y = h - 1;
    return MAKELPARAM(x, y);
}

static LRESULT CALLBACK HookProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (g_cb.onMessage) {
        LRESULT r;
        if (g_cb.onMessage(h, m, wp, lp, &r)) return r;
    }
    switch (m) {
    case WM_KEYDOWN:
        if (wp == VK_F11) TraceStart();
        break;
    case WM_ACTIVATEAPP:
        g_active = wp != 0;
        ClipToWindow(g_active);
        break;
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK: case WM_MBUTTONDOWN: case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK: case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
        if (be::Ready()) lp = MapMouse(lp);
        break;
    case WM_MOUSEWHEEL:                                // screen coordinates
        if (be::Ready()) { POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)}; r_ScreenToClient(h, &pt); lp = MapMouse(MAKELPARAM(pt.x, pt.y)); }
        break;
    case WM_SIZE:                                      // the game sees its own resolution
        if (be::Window() && wp != SIZE_MINIMIZED) { int w, hh; be::GetVirtualMode(&w, &hh); lp = MAKELPARAM(w, hh); }
        break;
    }
    return CallWindowProcA(g_oldProc, h, m, wp, lp);
}

// ---- import table patching ---------------------------------------------------------------------
static void* PatchIAT(HMODULE mod, const char* dll, const char* fn, void* hook) {
    uint8_t* base = (uint8_t*)mod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); d->Name; d++) {
        if (_stricmp((const char*)(base + d->Name), dll)) continue;
        IMAGE_THUNK_DATA* names = (IMAGE_THUNK_DATA*)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        IMAGE_THUNK_DATA* addrs = (IMAGE_THUNK_DATA*)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, addrs++) {
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (strcmp((const char*)ibn->Name, fn)) continue;
            void* old = (void*)addrs->u1.Function;
            DWORD prot;
            VirtualProtect(&addrs->u1.Function, sizeof(void*), PAGE_READWRITE, &prot);
            addrs->u1.Function = (uintptr_t)hook;
            VirtualProtect(&addrs->u1.Function, sizeof(void*), prot, &prot);
            Log("hooked %s!%s", dll, fn);
            return old;
        }
    }
    return nullptr;
}

void HooksInstall(HWND hwnd) {
    if (g_hwnd || !hwnd) return;
    g_hwnd = hwnd;
    HMODULE exe = GetModuleHandleA(nullptr);
    struct { const char* name; void* hook; void** real; } list[] = {
        {"GetCursorPos", (void*)h_GetCursorPos, (void**)&r_GetCursorPos}, {"SetCursorPos", (void*)h_SetCursorPos, (void**)&r_SetCursorPos},
        {"ScreenToClient", (void*)h_ScreenToClient, (void**)&r_ScreenToClient}, {"ClientToScreen", (void*)h_ClientToScreen, (void**)&r_ClientToScreen},
        {"GetClientRect", (void*)h_GetClientRect, (void**)&r_GetClientRect}, {"GetWindowRect", (void*)h_GetWindowRect, (void**)&r_GetWindowRect},
        {"GetSystemMetrics", (void*)h_GetSystemMetrics, (void**)&r_GetSystemMetrics}, {"SetWindowPos", (void*)h_SetWindowPos, (void**)&r_SetWindowPos},
        {"GetKeyState", (void*)h_GetKeyState, (void**)&r_GetKeyState}};
    for (auto& e : list) {
        void* old = PatchIAT(exe, "user32.dll", e.name, e.hook);
        if (old) *e.real = old;
    }
    g_oldProc = (WNDPROC)SetWindowLongA(hwnd, GWL_WNDPROC, (LONG)(uintptr_t)HookProc);
    Log("input translation installed on window %p", hwnd);
    QolInstall();
    if (GetEnvironmentVariableA("SWAT3D9_TRACE_NOW", nullptr, 0)) TraceStart();
}

void HooksSetCallbacks(const HookCallbacks& cb) { g_cb = cb; }
HWND HooksWindow() { return g_hwnd; }
LRESULT HooksCallOriginal(HWND h, UINT m, WPARAM wp, LPARAM lp) { return CallWindowProcA(g_oldProc, h, m, wp, lp); }
