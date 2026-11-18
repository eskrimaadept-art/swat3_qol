// SWAT 4-style command menu. The game's own command menu (a HUD element, vtable 0x65ff9c) is
// driven by "pressing" its number keys through its own key handler, while our popup shows the
// items of the menu level the game is at. Team selection (Element / Red / Blue) opens the menu
// straight into that team's orders; "context reports" collects the reports the game currently
// offers (the game marks them) and lists them at the top; "dynamic stack" makes Stack Up
// available on 9 in dynamic mode by swapping two rows of the game's menu table.
#include "qol.h"
#include <stdio.h>

namespace ctx {

static const uintptr_t kMenuVtbl = 0x65ff9c;
static uintptr_t g_vtbl = kMenuVtbl;
static const char* kTeamNames[3] = {"Element", "Red", "Blue"};
static const char* kToast[3] = {"ELEMENT", "RED TEAM", "BLUE TEAM"};
static const COLORREF kTeamColor[3] = {0x5ac8eb, 0x3c3ceb, 0xff8250};

struct Item {
    int slot;          // game menu slot 0..9 (-1 = ours)
    int target;        // >0: opens a submenu
    bool avail;
    int value;         // 0 = game item, 1 = "All commands", 2 = report
    int extra;         // the game's "report applies now" flag
    char label[64];
};

static uint8_t* g_menu;
static DWORD g_seen;
static bool g_fresh;
static bool g_open, g_teamMode, g_teamSeen, g_dirty, g_shown, g_loggedMenu;
static int g_teamMenuId = -1, g_team, g_sel;
static double g_accum;
static int g_step, g_frames, g_stepSlot, g_reportSlot = -1;
static DWORD g_toastTick;
static Vec<Item> g_items, g_reports;

// popup picture
static Vec<uint32_t> g_px;
static int g_pw, g_ph, g_px0, g_py0;
static bool g_pxNew;

// ---- the game's menu ----------------------------------------------------------------------
bool IsMenuElement(void* e) { return e && Readable(e, 4) && *(uintptr_t*)e == g_vtbl; }

static void Press(int code) {
    if (!g_menu || !Readable(g_menu, 0x6d0)) return;
    typedef void(__thiscall * KeyFn)(void*, int, int);
    KeyFn fn = (KeyFn)(*(void***)g_menu)[0x6c / 4];
    fn(g_menu, code, 1);
}
static void Hop(int slot) { SpeechMute(250); Press(0x37 + slot); }
static int CurMenu() { return g_menu && Readable(g_menu + 0x140, 2) ? *(uint16_t*)(g_menu + 0x140) : -1; }

static void Hide() { g_shown = false; }

static bool Same(const Vec<Item>& a, const Vec<Item>& b) {
    if (a.size() != b.size()) return false;
    for (int i = 0; i < a.size(); i++) {
        const Item &x = a[i], &y = b[i];
        if (x.slot != y.slot || x.target != y.target || x.avail != y.avail || x.value != y.value || strcmp(x.label, y.label)) return false;
    }
    return true;
}

static void ReadItems() {
    Vec<Item> list;
    uint8_t* m = g_menu;
    if (!m || !Readable(m, 0x6d0)) return;
    int count = *(uint16_t*)(m + 0x6c8);
    if (count >= 0x29) count = 0;
    uint8_t** tbl = *(uint8_t***)(m + 0x12c);
    for (int i = 0; i < count && i <= 9; i++) {
        int idx = *(int16_t*)(m + 0x40c + 2 * i);
        if (idx < 0 || !tbl || !Readable(tbl + idx, 4)) continue;
        uint8_t* rec = tbl[idx];
        if (!Readable(rec, 0x40)) continue;
        Item it = {};
        it.slot = i;
        it.target = *(int16_t*)(rec + 6);
        it.avail = m[0x664 + i] != 0;
        const char* l = *(const char**)(rec + 0x34);
        lstrcpynA(it.label, l && Readable(l, 1) ? l : "", sizeof(it.label));
        it.extra = *(int*)(rec + 0x20);
        list.push(it);
    }
    int cur = CurMenu();
    if (g_teamMode && g_step == 0) {
        if (cur == 1 && g_teamSeen) g_teamMode = false;
        else {
            if (g_teamMenuId < 0) g_teamMenuId = cur;
            g_teamSeen = true;
            Vec<Item> full;
            Item all = {}; all.slot = -1; all.avail = true; all.value = 1; lstrcpynA(all.label, "All commands", 64);
            full.push(all);
            if (cur == g_teamMenuId) for (auto& r : g_reports) full.push(r);
            for (auto& it : list) full.push(it);
            list.clear();
            for (auto& it : full) list.push(it);
        }
    }
    if (!Same(list, g_items)) {
        g_items.clear();
        for (auto& it : list) g_items.push(it);
        if (g_sel >= g_items.size()) g_sel = g_items.size() - 1;
        if (g_sel < 0) g_sel = 0;
        g_dirty = true;
        if (!g_loggedMenu && cur == 1 && !g_teamMode) {
            g_loggedMenu = true;
            Log("command menu (current menu %d):", cur);
            for (auto& it : g_items) Log("   %d %s%s%s", it.slot + 1, it.label, it.target > 0 ? " >" : "", it.avail ? "" : " (unavailable)");
        }
    }
}

static void Close(bool pressBack) {
    g_open = false; g_teamMode = false;
    if (g_step >= 3 && g_step <= 5) g_step = 0;
    if (pressBack) { SpeechMute(250); Press(0x35); }
    Hide();
}

static void Choose() {
    if (g_sel < 0 || g_sel >= g_items.size() || g_step) return;
    Item it = g_items[g_sel];
    if (!it.avail) return;
    if (it.value == 1) {                                  // All commands: back to the top level
        SpeechMute(250); Press(0x35); g_teamMode = false; g_sel = 0; g_accum = 0;
    } else if (it.value == 2) {                           // a report: top -> Report -> the report
        Hide(); g_open = false; g_teamMode = false;
        SpeechMute(250); Press(0x35);
        g_step = 4; g_stepSlot = it.slot; g_frames = 0;
        Log("report: %s", it.label);
    } else {
        SpeechMute(0);
        Press(0x37 + it.slot);
        if (it.target > 0) { g_sel = 0; g_accum = 0; }
        else Close(false);
    }
    g_dirty = true;
}

static bool IsTeamName(const char* s) { for (auto n : kTeamNames) if (!lstrcmpiA(s, n)) return true; return false; }

static void RunSteps() {
    if (!g_step) return;
    int cur = CurMenu();
    if (++g_frames >= 6) { Log("menu step %d timed out", g_step); g_step = 0; return; }
    switch (g_step) {
    case 1:
        if (cur != 1) break;
        g_reports.clear(); g_reportSlot = -1;
        for (auto& it : g_items)
            if (it.value == 0 && it.avail && it.target > 0 && !IsTeamName(it.label)) { g_reportSlot = it.slot; break; }
        if (g_reportSlot >= 0) { Hop(g_reportSlot); g_step = 2; g_frames = 0; ReadItems(); }
        else { g_step = 3; g_frames = 0; }
        break;
    case 2:
        if (cur == 1) break;
        for (auto& it : g_items)
            if (it.value == 0 && it.avail && it.extra && it.target <= 0) { Item r = it; r.value = 2; g_reports.push(r); }
        if (g_reports.size()) {
            Log("context reports available:");
            for (auto& r : g_reports) Log("   %s", r.label);
        }
        SpeechMute(250); Press(0x35);
        g_step = 3; g_frames = 0; ReadItems();
        break;
    case 3:
        if (cur != 1) break;
        g_step = 0; g_frames = 0;
        if (g_qol.openAtTeam) {
            for (auto& it : g_items)
                if (it.slot >= 0 && it.avail && it.target > 0 && !lstrcmpiA(it.label, kTeamNames[g_team])) {
                    Hop(it.slot);
                    g_teamMode = true; g_teamSeen = false; g_teamMenuId = -1; g_sel = 1;
                    break;
                }
        }
        ReadItems();
        break;
    case 4:
        if (cur != 1) break;
        Hop(g_reportSlot); g_step = 5; g_frames = 0;
        break;
    case 5:
        if (cur == 1) break;
        SpeechMute(0); Press(0x37 + g_stepSlot); g_step = 0; ReadItems();
        break;
    }
}

// ---- dynamic stack ------------------------------------------------------------------------
static uint8_t** g_dsTbl; static int g_dsCount;
static uint8_t *g_rowS, *g_rowO;
static bool g_swapped, g_dsLogged;
static int g_saveO1c, g_saveO24, g_saveS1c; static uint8_t g_saveS30;
static uint8_t *g_modeObj, **g_modeArr; static int g_modeIdx = -1;

static void SwapRows() {
    for (int i = 6; i < 0x40; i++) { uint8_t t = g_rowS[i]; g_rowS[i] = g_rowO[i]; g_rowO[i] = t; }
}
static bool RowsStillValid() {
    if (!Readable(g_rowS, 0x40) || !Readable(g_rowO, 0x40)) return false;
    if (*(int16_t*)(g_rowS + 2) != 2 || *(int16_t*)(g_rowS + 4) != 1) return false;
    if (*(int16_t*)(g_rowO + 2) != 2 || *(int16_t*)(g_rowO + 4) != 9) return false;
    if (g_swapped) return *(int*)(g_rowO + 0xc) == 0x1c && *(int*)(g_rowO + 0x1c) == 2;
    return *(int*)(g_rowS + 0xc) == 0x1c && *(int*)(g_rowS + 0x1c) == 1 && *(int*)(g_rowO + 0xc) != 0x1c;
}
static void FindRows(uint8_t** tbl, int n) {
    g_rowS = g_rowO = nullptr; g_swapped = false;
    if (!tbl || n < 1 || n > 2000 || !Readable(tbl, 4 * n)) return;
    int last = -1;
    for (int i = 0; i < n; i++) {
        uint8_t* r = tbl[i];
        if (!r || !Readable(r, 0x40) || *(int16_t*)(r + 2) != 2) continue;
        last = i;
        if (*(int16_t*)(r + 4) != 1 || *(int*)(r + 0xc) != 0x1c || *(int*)(r + 0x1c) != 1) continue;
        if (!g_rowS || (*(int*)(g_rowS + 0x24) != 1 && *(int*)(r + 0x24) == 1)) g_rowS = r;
    }
    if (last >= 0) { uint8_t* r = tbl[last]; if (*(int16_t*)(r + 4) == 9 && *(int*)(r + 0xc) != 0x1c) g_rowO = r; }
    if (!g_rowS || !g_rowO) g_rowS = g_rowO = nullptr;
    if (!g_dsLogged) { g_dsLogged = true; Log("dynamic stack: %s", g_rowS ? "Stack row and button-9 row found" : "menu layout not recognised - off"); }
}
static bool DynamicMode() {
    uint8_t* obj = *(uint8_t**)(g_menu + 0x3c);
    if (!obj || !Readable(obj, 0x3c)) return false;
    uint8_t** arr = *(uint8_t***)(obj + 0x34);
    int n = *(int*)(obj + 0x38);
    if (obj == g_modeObj && arr == g_modeArr && g_modeIdx >= 0 && g_modeIdx < n) {
        uint8_t* e = arr[g_modeIdx];
        if (e && Readable(e, 0x10c) && *(int*)(e + 0x40) == 7) return *(int*)(e + 0x108) >= 2;
    }
    g_modeObj = obj; g_modeArr = arr; g_modeIdx = -1;
    if (!arr || n < 1 || n > 256 || !Readable(arr, 4 * n)) return false;
    for (int i = 0; i < n; i++) {
        uint8_t* e = arr[i];
        if (e && Readable(e, 0x10c) && *(int*)(e + 0x40) == 7) { g_modeIdx = i; return *(int*)(e + 0x108) >= 2; }
    }
    return false;
}
static void DynamicStack() {
    if (!g_qol.dynamicStack || !g_menu || !Readable(g_menu, 0x6d0)) return;
    uint8_t** tbl = *(uint8_t***)(g_menu + 0x12c);
    int n = *(int*)(g_menu + 0x130);
    if (tbl != g_dsTbl || n != g_dsCount || !g_rowS || !RowsStillValid()) {
        g_dsTbl = tbl; g_dsCount = n;
        FindRows(tbl, n);
    }
    if (!g_rowS) return;
    bool dyn = DynamicMode();
    if (dyn && !g_swapped) {
        SwapRows();
        g_saveO1c = *(int*)(g_rowO + 0x1c); g_saveO24 = *(int*)(g_rowO + 0x24);
        *(int*)(g_rowO + 0x1c) = 2; *(int*)(g_rowO + 0x24) = 3;
        g_saveS1c = *(int*)(g_rowS + 0x1c); g_saveS30 = g_rowS[0x30];
        *(int*)(g_rowS + 0x1c) = 1; g_rowS[0x30] = 1;
        g_swapped = true;
        Log("dynamic stack: Stack available on 9 (dynamic mode)");
    } else if (!dyn && g_swapped) {
        *(int*)(g_rowS + 0x1c) = g_saveS1c; g_rowS[0x30] = g_saveS30;
        *(int*)(g_rowO + 0x1c) = g_saveO1c; *(int*)(g_rowO + 0x24) = g_saveO24;
        SwapRows();
        g_swapped = false;
        Log("dynamic stack: restored (stealth mode)");
    }
}

// ---- popup picture ------------------------------------------------------------------------
static void Label(const Item& it, char* out, int n) {
    if (it.value == 1) _snprintf(out, n, "\xab   %s", it.label);
    else if (it.value == 2) _snprintf(out, n, "\xbb   %s", it.label);
    else _snprintf(out, n, "%d   %s%s", (it.slot + 1) % 10, it.label, it.target > 0 ? "   \xbb" : "");
    out[n - 1] = 0;
}

static inline uint32_t Px(COLORREF c) { return 0xff000000u | (GetRValue(c) << 16) | (GetGValue(c) << 8) | GetBValue(c); }

static void DrawPopup() {
    int ow, oh; be::GetOutputSize(&ow, &oh);
    int fh = g_qol.menuSize * oh / 3800; if (fh < 11) fh = 11;
    int rowH = fh + fh / 2, pad = fh;
    bool header = g_teamMode;
    int rows = g_items.size() + (header ? 1 : 0);
    if (!rows) { Hide(); return; }
    HDC dc = CreateCompatibleDC(nullptr);
    HFONT font = CreateFontA(-fh, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Tahoma");
    HGDIOBJ oldF = SelectObject(dc, font);
    char buf[96];
    int textW = 0;
    for (auto& it : g_items) { Label(it, buf, sizeof(buf)); SIZE s; GetTextExtentPoint32A(dc, buf, (int)strlen(buf), &s); if (s.cx > textW) textW = s.cx; }
    if (header) { SIZE s; GetTextExtentPoint32A(dc, kToast[g_team], (int)strlen(kToast[g_team]), &s); if (s.cx > textW) textW = s.cx; }
    int w = textW + 2 * pad, h = rows * rowH + pad;
    BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    uint32_t* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&bits, nullptr, 0);
    if (!bmp || !bits) { SelectObject(dc, oldF); DeleteObject(font); DeleteDC(dc); return; }
    HGDIOBJ oldB = SelectObject(dc, bmp);
    auto fill = [&](int x0, int y0, int x1, int y1, uint32_t c) {
        for (int y = y0 < 0 ? 0 : y0; y < y1 && y < h; y++) for (int x = x0 < 0 ? 0 : x0; x < x1 && x < w; x++) bits[y * w + x] = c;
    };
    fill(0, 0, w, h, 0xff141c24);
    int y = pad / 2;
    if (header) {
        COLORREF tc = kTeamColor[g_team];
        uint32_t dark = 0xff000000u | ((GetRValue(tc) * 2 / 5) << 16) | ((GetGValue(tc) * 2 / 5) << 8) | (GetBValue(tc) * 2 / 5);
        fill(0, 0, w, y + rowH, dark);
        y += rowH;
    }
    for (int i = 0; i < g_items.size(); i++) if (i == g_sel) fill(2, y + i * rowH, w - 2, y + (i + 1) * rowH, 0xff2f5d7a);
    // border
    fill(0, 0, w, 1, 0xff5a7890); fill(0, h - 1, w, h, 0xff5a7890); fill(0, 0, 1, h, 0xff5a7890); fill(w - 1, 0, w, h, 0xff5a7890);
    GdiFlush();
    SetBkMode(dc, TRANSPARENT);
    int ty = pad / 2 + (rowH - fh) / 2;
    if (header) {
        SetTextColor(dc, kTeamColor[g_team]);
        TextOutA(dc, pad, ty, kToast[g_team], (int)strlen(kToast[g_team]));
        ty += rowH;
    }
    for (int i = 0; i < g_items.size(); i++) {
        const Item& it = g_items[i];
        Label(it, buf, sizeof(buf));
        COLORREF c = !it.avail ? 0x767069 : i == g_sel ? 0xffffff : it.value == 2 ? 0x96e696 : 0xd7cdbe;
        SetTextColor(dc, c);
        TextOutA(dc, pad, ty + i * rowH, buf, (int)strlen(buf));
    }
    GdiFlush();
    g_px.resize(w * h);
    const uint32_t a = 235;
    for (int i = 0; i < w * h; i++) {
        uint32_t p = bits[i];
        uint32_t r = ((p >> 16) & 255) * a / 255, g = ((p >> 8) & 255) * a / 255, b = (p & 255) * a / 255;
        g_px[i] = (a << 24) | (r << 16) | (g << 8) | b;
    }
    SelectObject(dc, oldB); DeleteObject(bmp);
    SelectObject(dc, oldF); DeleteObject(font); DeleteDC(dc);
    g_pw = w; g_ph = h;
    g_px0 = ow / 2 + 2 * fh;
    g_py0 = (oh - h) / 2;
    g_pxNew = true;
    g_shown = true;
}

// ---- per frame ----------------------------------------------------------------------------
void OnHudElement(void* e) {
    if (IsMenuElement(e)) { g_menu = (uint8_t*)e; g_seen = GetTickCount(); g_fresh = true; }
}
bool InGame() { return g_menu && GetTickCount() - g_seen < 500; }
bool Active() { return g_open && InGame(); }

void FrameEnd(bool) {
    if (!g_fresh) {                    // no HUD this frame (menus, loading, paused): nothing of ours shows
        if (g_menu) { g_menu = nullptr; g_open = false; g_teamMode = false; g_step = 0; Hide(); }
        return;
    }
    g_fresh = false;
    DynamicStack();
    if (!g_open && !g_step) Hide();
    else {
        ReadItems();
        RunSteps();
        if (g_open && !g_step && (g_dirty || !g_shown)) { g_dirty = false; DrawPopup(); }
        if (g_step) Hide();
    }
    if (g_shown && g_open && g_pw > 0) { OverlayImage(g_pxNew ? g_px.p : nullptr, g_pw, g_ph, g_px0, g_py0); g_pxNew = false; }
    // team toast
    if (!g_open && InGame() && GetTickCount() - g_toastTick < 1500) {
        int vw, vh; be::GetVirtualMode(&vw, &vh);
        int ow, oh; be::GetOutputSize(&ow, &oh);
        int x, y; VirtToOut(vw / 2.0, vh / 2.0 + vh / 14.0, &x, &y);
        int fh = oh / 36; if (fh < 14) fh = 14;
        OverlayText(kToast[g_team], kTeamColor[g_team], fh, x, y, 1);
    }
}

void OnMouseDelta(int, int dy) {
    if (!g_open || g_items.empty()) return;
    int ow, oh; be::GetOutputSize(&ow, &oh);
    double step = oh / 38.0 * 1.5;
    g_accum += dy;
    int last = g_items.size() - 1;
    while (g_accum > step && g_sel < last) { g_accum -= step; g_sel++; g_dirty = true; }
    while (g_accum < -step && g_sel > 0) { g_accum += step; g_sel--; g_dirty = true; }
    if ((g_sel == 0 && g_accum < 0) || (g_sel == last && g_accum > 0)) g_accum = 0;
}

static void Move(int d) {
    int n = g_items.size(); if (!n) return;
    g_sel += d; if (g_sel < 0) g_sel = 0; if (g_sel >= n) g_sel = n - 1;
    g_accum = 0; g_dirty = true;
}

bool OnMessage(HWND, UINT m, WPARAM wp, LPARAM lp) {
    if (!InGame()) {
        if (g_open) { g_open = false; g_teamMode = false; g_step = 0; Hide(); }
        return false;
    }
    bool rep = false;
    int k = KeyOfMessage(m, wp, lp, &rep);
    // team selection
    if (k > 0 && !rep) {
        int t = -1;
        if (k == g_qol.cycleKey) t = (g_team + 1) % 3;
        else if (k == g_qol.elementKey) t = 0;
        else if (k == g_qol.redKey) t = 1;
        else if (k == g_qol.blueKey) t = 2;
        if (t >= 0) {
            g_team = t;
            g_toastTick = GetTickCount();
            if (g_open) {
                SpeechMute(250); Press(0x35);
                g_sel = 0; g_accum = 0; g_teamMode = false; g_frames = 0;
                g_step = g_qol.openAtTeam ? 3 : 0;
                g_dirty = true;
            }
            return true;
        }
    }
    if (k < 0 && (-k == g_qol.cycleKey || -k == g_qol.elementKey || -k == g_qol.redKey || -k == g_qol.blueKey)) return true;
    // the menu key
    if (g_qol.menuKey && (k == g_qol.menuKey || k == -g_qol.menuKey)) {
        if (k > 0 && !rep) {
            if (g_open) Close(true);
            else {
                SpeechMute(250); Press(0x35);
                g_open = true; g_sel = 0; g_accum = 0; g_reports.clear(); g_teamMode = false; g_frames = 0;
                g_step = g_qol.contextReports ? 1 : (g_qol.openAtTeam ? 3 : 0);
                g_dirty = true;
                LogV(2, "context menu opened");
            }
        }
        return true;
    }
    if (!g_open) return false;
    switch (m) {
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: Choose(); return true;
    case WM_LBUTTONUP: return true;
    case WM_MOUSEWHEEL: Move((short)HIWORD(wp) > 0 ? -1 : 1); return true;
    case WM_KEYDOWN:
        if (wp == VK_RETURN) { Choose(); return true; }
        if (wp == VK_ESCAPE) { Close(true); return true; }
        if (wp == VK_UP) { Move(-1); return true; }
        if (wp == VK_DOWN) { Move(1); return true; }
        break;
    case WM_KEYUP:
        if (wp == VK_RETURN || wp == VK_ESCAPE || wp == VK_UP || wp == VK_DOWN) return true;
        break;
    }
    return false;
}

void Init() {
    uint8_t* t = TestAddr("SWAT3D9_TEST_MENUVTBL");
    if (t) g_vtbl = (uintptr_t)t;
    char a[32], b[32], c[32], d[32], e[32];
    Log("command menu key: %s (code %d)", g_qol.menuKey ? KeyName(g_qol.menuKey, a, 32) : "NONE", g_qol.menuKey);
    Log("context reports: %d, stack in dynamic mode: %d", g_qol.contextReports, g_qol.dynamicStack);
    Log("team keys: cycle %s, element %s, red %s, blue %s; menu opens %s", KeyName(g_qol.cycleKey, b, 32), KeyName(g_qol.elementKey, c, 32),
        KeyName(g_qol.redKey, d, 32), KeyName(g_qol.blueKey, e, 32), g_qol.openAtTeam ? "at the selected team" : "at the top level");
}

}  // namespace ctx
