// Shared pieces of the QoL features: settings, the two patches into swat.exe (HUD hook and speech
// mute), key names, and text / pictures drawn over the game's picture.
#include "qol.h"
#include "hooks.h"
#include <stdio.h>
#include <math.h>

QolConfig g_qol;

// ---- keys ---------------------------------------------------------------------------------
static const struct { const char* name; int code; } kKeys[] = {
    {"LMOUSE", 1}, {"RMOUSE", 2}, {"MMOUSE", 4}, {"XMOUSE", 5}, {"YMOUSE", 6},
    {"TAB", VK_TAB}, {"SPACE", VK_SPACE}, {"ENTER", VK_RETURN}, {"RETURN", VK_RETURN}, {"BACKSPACE", VK_BACK},
    {"SHIFT", VK_SHIFT}, {"CTRL", VK_CONTROL}, {"CONTROL", VK_CONTROL}, {"ALT", VK_MENU}, {"CAPSLOCK", VK_CAPITAL},
    {"INSERT", VK_INSERT}, {"DELETE", VK_DELETE}, {"HOME", VK_HOME}, {"END", VK_END}, {"PAGEUP", VK_PRIOR}, {"PAGEDOWN", VK_NEXT},
    {"UP", VK_UP}, {"DOWN", VK_DOWN}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT},
    {"NUMPAD0", VK_NUMPAD0}, {"NUMPAD1", VK_NUMPAD1}, {"NUMPAD2", VK_NUMPAD2}, {"NUMPAD3", VK_NUMPAD3}, {"NUMPAD4", VK_NUMPAD4},
    {"NUMPAD5", VK_NUMPAD5}, {"NUMPAD6", VK_NUMPAD6}, {"NUMPAD7", VK_NUMPAD7}, {"NUMPAD8", VK_NUMPAD8}, {"NUMPAD9", VK_NUMPAD9},
    {"`", VK_OEM_3}, {"TILDE", VK_OEM_3}, {"-", VK_OEM_MINUS}, {"=", VK_OEM_PLUS}, {"[", VK_OEM_4}, {"]", VK_OEM_6},
    {";", VK_OEM_1}, {"'", VK_OEM_7}, {",", VK_OEM_COMMA}, {".", VK_OEM_PERIOD}, {"/", VK_OEM_2}, {"\\", VK_OEM_5},
};

int ParseKey(const char* s) {
    while (*s == ' ') s++;
    if (!*s || !_stricmp(s, "NONE") || !strcmp(s, "0")) return 0;
    for (auto& k : kKeys) if (!_stricmp(s, k.name)) return k.code;
    if (!s[1] && ((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z'))) return toupper((unsigned char)s[0]);
    if ((s[0] == 'F' || s[0] == 'f') && s[1] >= '1' && s[1] <= '9') { int n = atoi(s + 1); if (n >= 1 && n <= 12) return VK_F1 + n - 1; }
    if (s[0] >= '0' && s[0] <= '9') {
        if (!s[1]) return s[0];                                  // a digit key
        char* e; long v = strtol(s, &e, 10);
        if (!*e && v > 0 && v < 256) return (int)v;              // a key code like in swat.cfg
    }
    LogOnce(_strdup(s), "key name '%s' not recognised - ignored", s);
    return 0;
}

const char* KeyName(int code, char* buf, int n) {
    buf[0] = 0;
    switch (code) {
    case 0: lstrcpynA(buf, "NONE", n); return buf;
    case 1: lstrcpynA(buf, "LMB", n); return buf;
    case 2: lstrcpynA(buf, "RMB", n); return buf;
    case 4: lstrcpynA(buf, "MMB", n); return buf;
    case 5: lstrcpynA(buf, "Mouse 4", n); return buf;
    case 6: lstrcpynA(buf, "Mouse 5", n); return buf;
    }
    if (code >= VK_F1 && code <= VK_F12) { _snprintf(buf, n, "F%d", code - VK_F1 + 1); buf[n - 1] = 0; return buf; }
    UINT sc = MapVirtualKeyA(code, 0);
    if (sc && GetKeyNameTextA(sc << 16, buf, n)) return buf;
    buf[0] = (code >= '0' && code < '0' + 0x2b) ? (char)code : '?'; buf[1] = 0;
    return buf;
}

int KeyOfMessage(UINT m, WPARAM wp, LPARAM lp, bool* repeat) {
    *repeat = false;
    switch (m) {
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: return 1;
    case WM_LBUTTONUP: return -1;
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: return 2;
    case WM_RBUTTONUP: return -2;
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: return 4;
    case WM_MBUTTONUP: return -4;
    case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK: return HIWORD(wp) == 1 ? 5 : 6;
    case WM_XBUTTONUP: return HIWORD(wp) == 1 ? -5 : -6;
    case WM_KEYDOWN: case WM_SYSKEYDOWN: *repeat = (lp & 0x40000000) != 0; return (int)wp;
    case WM_KEYUP: case WM_SYSKEYUP: return -(int)wp;
    }
    return 0;
}

// ---- game objects -------------------------------------------------------------------------
uint8_t* QolEngine() {
    static uint8_t* test = TestAddr("SWAT3D9_TEST_ENGINE");
    if (test) return test;
    return Readable((void*)0x6c0ddc, 4) ? *(uint8_t**)0x6c0ddc : nullptr;
}
uint8_t* QolRenderer() {
    static uint8_t* g = TestAddr("SWAT3D9_TEST_GLOBALS");
    uint8_t* base = g ? g : (uint8_t*)0x6cad10;
    return Readable(base + 0xc, 4) ? *(uint8_t**)(base + 0xc) : nullptr;
}

// ---- patches into swat.exe ----------------------------------------------------------------
static bool WriteCode(uintptr_t at, const void* bytes, size_t n) {
    DWORD old;
    if (!VirtualProtect((void*)at, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy((void*)at, bytes, n);
    VirtualProtect((void*)at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, n);
    return true;
}

static bool g_hudFrame;
static DWORD g_hudTick;
bool HudSeenRecently() { return g_hudTick && GetTickCount() - g_hudTick < 500; }

// Replaces "mov edx,[ecx]; call [edx+0x60]" in the game's HUD loop (ecx = HUD element)
static void __fastcall HudDrawElement(void* elem) {
    ctx::OnHudElement(elem);
    typedef void(__thiscall * DrawFn)(void*);
    ((DrawFn)(*(void***)elem)[0x60 / 4])(elem);
    if (ctx::IsMenuElement(elem)) { g_hudFrame = true; g_hudTick = GetTickCount(); }
}

static void InstallHudPatch() {
    uint8_t* t = TestAddr("SWAT3D9_TEST_HUDSITE");
    uintptr_t at = t ? (uintptr_t)t : 0x4999da;
    if (!Readable((void*)at, 5) || memcmp((void*)at, "\x8b\x11\xff\x52\x60", 5)) {
        Log("HUD hook: this swat.exe build is not recognised - command menu, nametags and quick restrain unavailable");
        return;
    }
    uint8_t call[5] = {0xe8};
    int32_t rel = (int32_t)((uintptr_t)&HudDrawElement - (at + 5));
    memcpy(call + 1, &rel, 4);
    if (WriteCode(at, call, 5)) Log("HUD hook installed");
}

// Speech: while the command menu hops between menu levels the officers would announce every
// level ("Element", "Report"...). The game's speech routine is skipped for a moment.
static volatile DWORD g_muteUntil;
void SpeechMute(int ms) { g_muteUntil = ms > 0 ? GetTickCount() + ms : 0; }
extern "C" int __cdecl QolSpeechMuted() { DWORD u = g_muteUntil; return u && (int)(u - GetTickCount()) > 0; }
extern "C" void QolSpeechStub();
asm(".text\n"
    ".globl _QolSpeechStub\n"
    "_QolSpeechStub:\n"
    "  push %ecx\n"
    "  call _QolSpeechMuted\n"
    "  pop %ecx\n"
    "  test %eax, %eax\n"
    "  jnz 1f\n"
    "  sub $0xa8, %esp\n"
    "  push $0x603326\n"
    "  ret\n"
    "1:\n"
    "  fldz\n"
    "  ret $0x24\n");

static void InstallSpeechMute() {
    const uintptr_t at = 0x603320;
    if (TestAddr("SWAT3D9_TEST_SPEECH") || !Readable((void*)at, 6) || memcmp((void*)at, "\x81\xec\xa8\x00\x00\x00", 6)) {
        Log("speech mute: this swat.exe build is not recognised - menu navigation stays audible");
        return;
    }
    uint8_t jmp[6] = {0xe9, 0, 0, 0, 0, 0x90};
    int32_t rel = (int32_t)((uintptr_t)&QolSpeechStub - (at + 5));
    memcpy(jmp + 1, &rel, 4);
    if (WriteCode(at, jmp, 6)) Log("speech mute installed");
}

// ---- overlay text and pictures ------------------------------------------------------------
void VirtToOut(double vx, double vy, int* ox, int* oy) {
    be::Mapping m = be::GetMapping();
    *ox = (int)floor(m.offX + vx * m.scale + 0.5);
    *oy = (int)floor(m.offY + vy * m.scale + 0.5);
}

struct Sprite { char text[64]; COLORREF color; int h; be::Tex* tex; int w, th; DWORD used; };
static Sprite g_sprites[64];

static HFONT FontFor(int h) {
    static struct { int h; HFONT f; } cache[8];
    static int next;
    for (auto& c : cache) if (c.f && c.h == h) return c.f;
    auto& c = cache[next++ & 7];
    if (c.f) DeleteObject(c.f);
    c.h = h;
    c.f = CreateFontA(-h, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Tahoma");
    return c.f;
}

// White text on black into a DIB: returns the coverage (0..255) per pixel
static bool RenderMask(HDC dc, uint32_t* bits, int w, int h, const char* s, int o, bool outline) {
    memset(bits, 0, (size_t)w * h * 4);
    GdiFlush();
    SetTextColor(dc, 0xffffff);
    int n = (int)strlen(s);
    if (outline) {
        for (int dy = -o; dy <= o; dy++) for (int dx = -o; dx <= o; dx++) if (dx || dy) TextOutA(dc, o + dx, o + dy, s, n);
    } else TextOutA(dc, o, o, s, n);
    GdiFlush();
    return true;
}

static Sprite* MakeSprite(const char* text, COLORREF color, int h) {
    DWORD now = GetTickCount();
    Sprite* slot = nullptr;
    for (auto& s : g_sprites)
        if (s.tex && s.color == color && s.h == h && !strcmp(s.text, text)) { s.used = now; return &s; }
    for (auto& s : g_sprites) if (!s.tex) { slot = &s; break; }
    if (!slot) {                                   // reuse the least recently used one
        slot = &g_sprites[0];
        for (auto& s : g_sprites) if (now - s.used > now - slot->used) slot = &s;
        be::TexDestroy(slot->tex); slot->tex = nullptr;
    }
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ oldF = SelectObject(dc, FontFor(h));
    SetBkMode(dc, TRANSPARENT);
    SIZE sz; GetTextExtentPoint32A(dc, text, (int)strlen(text), &sz);
    int o = h >= 24 ? 2 : 1;
    int w = sz.cx + 2 * o + 2, th = sz.cy + 2 * o;
    BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -th;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    uint32_t* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&bits, nullptr, 0);
    Sprite* result = nullptr;
    if (bmp && bits) {
        HGDIOBJ oldB = SelectObject(dc, bmp);
        Vec<uint8_t> outline; outline.resize(w * th);
        RenderMask(dc, bits, w, th, text, o, true);
        for (int i = 0; i < w * th; i++) outline[i] = (uint8_t)(bits[i] & 255);
        RenderMask(dc, bits, w, th, text, o, false);
        Vec<uint32_t> px; px.resize(w * th);
        uint32_t cr = GetRValue(color), cg = GetGValue(color), cb = GetBValue(color);
        for (int i = 0; i < w * th; i++) {
            uint32_t a = bits[i] & 255;                      // text coverage
            uint32_t ao = outline[i] * 220 / 255;            // dark outline, slightly see-through
            uint32_t alpha = a + ao * (255 - a) / 255;
            px[i] = (alpha << 24) | ((cr * a / 255) << 16) | ((cg * a / 255) << 8) | (cb * a / 255);
        }
        SelectObject(dc, oldB);
        be::Tex* t = be::TexCreate(w, th, false);
        if (t) {
            be::TexUpload(t, px.p, w * 4);
            lstrcpynA(slot->text, text, sizeof(slot->text));
            slot->color = color; slot->h = h; slot->tex = t; slot->w = w; slot->th = th; slot->used = now;
            result = slot;
        }
        DeleteObject(bmp);
    }
    SelectObject(dc, oldF);
    DeleteDC(dc);
    return result;
}

void OverlayText(const char* text, COLORREF color, int h, int x, int y, int align) {
    if (!text || !*text) return;
    Sprite* s = MakeSprite(text, color, h);
    if (!s) return;
    int x0 = x - s->w / 2, y0 = align == 0 ? y - s->th : y - s->th / 2;
    be::OverlayAdd(s->tex, x0, y0, s->w, s->th);
}

void OverlayImage(const uint32_t* px, int w, int h, int x, int y) {
    static be::Tex* tex; static int tw, th;
    if (!tex || tw != w || th != h) {
        if (tex) be::TexDestroy(tex);
        tex = be::TexCreate(w, h, false); tw = w; th = h;
        if (!px) return;                              // nothing to fill it with yet
    }
    if (px && tex) be::TexUpload(tex, px, w * 4);
    if (tex) be::OverlayAdd(tex, x, y, w, h);
}

// ---- per frame ----------------------------------------------------------------------------
static void OnPresent() {
    bool hud = g_hudFrame;
    g_hudFrame = false;
    be::OverlayClear();
    tags::NewFrame();
    if (hud) tags::Draw();
    ctx::FrameEnd(hud);
    if (hud) qc::Update();
}

static bool FreezeCursor() { return ctx::Active(); }
static void MouseDelta(int dx, int dy) { ctx::OnMouseDelta(dx, dy); }
static bool OnMessage(HWND h, UINT m, WPARAM wp, LPARAM lp, LRESULT* r) {
    *r = 0;
    if (ctx::OnMessage(h, m, wp, lp)) return true;
    if (qc::OnMessage(h, m, wp, lp)) return true;
    return false;
}

static int KeySetting(const char* key, const char* def) {
    char buf[64]; IniStr("Controls", key, def, buf, sizeof(buf));
    return ParseKey(buf);
}

void QolInstall() {
    static bool done;
    if (done) return;
    done = true;
    QolConfig& q = g_qol;
    q.menuKey = KeySetting("ContextMenuKey", "RMOUSE");
    q.cycleKey = KeySetting("TeamCycleKey", "XMOUSE");
    q.elementKey = KeySetting("TeamElementKey", "NONE");
    q.redKey = KeySetting("TeamRedKey", "NONE");
    q.blueKey = KeySetting("TeamBlueKey", "NONE");
    q.openAtTeam = IniInt("Controls", "OpenAtSelectedTeam", 1);
    q.contextReports = IniInt("Controls", "ContextReports", 1);
    q.dynamicStack = IniInt("Controls", "DynamicStack", 1);
    q.menuSize = IniInt("Controls", "MenuSize", 70);
    if (q.menuSize < 40) q.menuSize = 40; if (q.menuSize > 200) q.menuSize = 200;
    q.tagsOn = IniInt("Nametags", "Show", 1);
    q.tagsMaxDist = IniInt("Nametags", "MaxDistance", 1500);
    q.tagsTextSize = IniInt("Nametags", "TextSize", 0);
    q.qcOn = IniInt("QuickRestrain", "Enabled", 1);
    q.qcReach = IniInt("QuickRestrain", "Reach", 80);
    InstallHudPatch();
    InstallSpeechMute();
    ctx::Init();
    qc::Init();
    Log("nametags: %s (max distance %d, text size %d)", q.tagsOn ? "on" : "off", q.tagsMaxDist, q.tagsTextSize);
    HookCallbacks cb;
    cb.freezeCursor = FreezeCursor;
    cb.mouseDelta = MouseDelta;
    cb.onMessage = OnMessage;
    HooksSetCallbacks(cb);
    be::SetPresentCallback(OnPresent);
}
