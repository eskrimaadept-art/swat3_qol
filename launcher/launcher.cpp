// SWAT 3 Launcher: settings for the Direct3D 11 renderer / QoL features (swat3d11.ini), the
// ragdoll add-on (swat3ragdoll.ini), the game's own mods (Mods folder + swat.ini) and a Play button.
// Lives in the game folder. Only the keys it shows are written; everything else in the ini files
// (comments, other settings) is left as it is.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <algorithm>

#define LAUNCHER_VERSION "1.0.1"

static char g_dir[MAX_PATH];           // game folder, with trailing backslash
static std::string Path(const char* file) { return std::string(g_dir) + file; }
static bool Exists(const std::string& p) { DWORD a = GetFileAttributesA(p.c_str()); return a != INVALID_FILE_ATTRIBUTES; }
static bool IsDir(const std::string& p) { DWORD a = GetFileAttributesA(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }

// ---- settings table -------------------------------------------------------------------------
enum { T_CHECK, T_COMBO, T_EDIT, T_MISSIONRES, T_WINSIZE, T_NOTE };
enum { F_RENDER, F_RAGDOLL };
enum { TAB_DISPLAY, TAB_QOL, TAB_MODS, TAB_RAGDOLL, TAB_COUNT };
struct Opt {
    int tab, type, file;
    const char* label; const char* section; const char* key; const char* def;
    const char* choices;                 // "value|label;value|label" (label optional)
    HWND ctl; HWND lbl;
    std::vector<std::string> values;     // combo: value of each item
};
static const char* kKeys = "RMOUSE|Right mouse button;MMOUSE|Middle mouse button;XMOUSE|Mouse button 4 (side);YMOUSE|Mouse button 5 (side);"
                           "TAB|Tab;Q|Q;E|E;F|F;G|G;V|V;X|X;Z|Z;NONE|Off";
static Opt g_opts[] = {
    {TAB_DISPLAY, T_CHECK, F_RENDER, "Fullscreen (borderless, fills the monitor)", "Display", "Fullscreen", "1"},
    {TAB_DISPLAY, T_MISSIONRES, F_RENDER, "Resolution in missions", "Game", "MissionResolution", "game"},
    {TAB_DISPLAY, T_WINSIZE, F_RENDER, "Window size (when not fullscreen)", "Display", "WindowWidth", "1280"},
    {TAB_DISPLAY, T_CHECK, F_RENDER, "VSync (no tearing)", "Display", "VSync", "1"},
    {TAB_DISPLAY, T_COMBO, F_RENDER, "Anti-aliasing", "Display", "MSAA", "0", "0|Off;2|2x;4|4x;8|8x"},
    {TAB_DISPLAY, T_COMBO, F_RENDER, "Texture sharpness at angles", "Display", "Anisotropy", "8", "1|Off;2|2x;4|4x;8|8x;16|16x"},
    {TAB_DISPLAY, T_CHECK, F_RENDER, "Smooth distant textures (less shimmer)", "Display", "Mipmaps", "1"},
    {TAB_DISPLAY, T_CHECK, F_RENDER, "Smooth scaling of menus and HUD", "Display", "SmoothUI", "1"},
    {TAB_DISPLAY, T_COMBO, F_RENDER, "Field of view", "Display", "FieldOfView", "0", "0|Game default (about 91);95|95;100|100;105|105;110|110;120|120"},
    {TAB_DISPLAY, T_CHECK, F_RENDER, "Skip the intro videos", "Game", "SkipIntro", "0"},
    {TAB_DISPLAY, T_COMBO, F_RENDER, "Largest texture the game may load", "Game", "MaxTextureSize", "2048", "256|256 (original game);512|512;1024|1024 (Last Resort);2048|2048;4096|4096"},
    {TAB_DISPLAY, T_COMBO, F_RENDER, "Draw distance", "Game", "DrawDistance", "2800", "1400|Original game;2800|Far (Last Resort)"},

    {TAB_QOL, T_COMBO, F_RENDER, "Command menu button", "Controls", "ContextMenuKey", "RMOUSE", kKeys},
    {TAB_QOL, T_COMBO, F_RENDER, "Switch team button (Element / Red / Blue)", "Controls", "TeamCycleKey", "XMOUSE", kKeys},
    {TAB_QOL, T_COMBO, F_RENDER, "Command menu size", "Controls", "MenuSize", "70", "50|50%;60|60%;70|70%;80|80%;90|90%;100|100% (original)"},
    {TAB_QOL, T_CHECK, F_RENDER, "Menu opens at the selected team's orders", "Controls", "OpenAtSelectedTeam", "1"},
    {TAB_QOL, T_CHECK, F_RENDER, "Fitting reports listed at the top of the menu", "Controls", "ContextReports", "1"},
    {TAB_QOL, T_CHECK, F_RENDER, "\"Stack up\" can be ordered in dynamic mode", "Controls", "DynamicStack", "1"},
    {TAB_QOL, T_CHECK, F_RENDER, "Officer nametags", "Nametags", "Show", "1"},
    {TAB_QOL, T_COMBO, F_RENDER, "Hide nametags beyond", "Nametags", "MaxDistance", "1500", "500|Close (500);1000|Medium (1000);1500|Far (1500);3000|Very far (3000);0|Never hide"},
    {TAB_QOL, T_CHECK, F_RENDER, "Quick restrain (interact key on someone who surrendered)", "QuickRestrain", "Enabled", "1"},
    {TAB_QOL, T_CHECK, F_RENDER, "Restrain instantly, without taking out the cuffs", "QuickRestrain", "Direct", "1"},
    {TAB_QOL, T_CHECK, F_RENDER, "Blood on walls behind people who are shot", "Blood", "WallSplatter", "1"},
    {TAB_QOL, T_CHECK, F_RENDER, "Officers leave blood too", "Blood", "IncludeOfficers", "1"},

    {TAB_RAGDOLL, T_CHECK, F_RAGDOLL, "Ragdoll physics for characters who die", "Ragdoll", "Enabled", "1"},
    {TAB_RAGDOLL, T_COMBO, F_RAGDOLL, "Death animation before physics takes over", "Ragdoll", "Delay", "0.3", "0|None (collapse at once);0.3|Short (0.3 s);0.6|Medium (0.6 s);1.0|Long (1 s)"},
    {TAB_RAGDOLL, T_COMBO, F_RAGDOLL, "Bullets push the body", "Ragdoll", "BulletPush", "1.0", "0|Off;0.5|Lightly;1.0|Normal;2.0|Hard (movie style)"},
    {TAB_RAGDOLL, T_COMBO, F_RAGDOLL, "Gravity", "Ragdoll", "Gravity", "1.0", "0.6|Low (floaty);1.0|Normal;1.4|Heavy"},
    {TAB_RAGDOLL, T_COMBO, F_RAGDOLL, "Friction", "Ragdoll", "Friction", "0.6", "0.3|Slippery;0.6|Normal;0.9|Sticky"},
    {TAB_RAGDOLL, T_CHECK, F_RAGDOLL, "Wounded who die on their own go limp too", "Ragdoll", "AlreadyLying", "0"},
};
static const int kNumOpts = sizeof(g_opts) / sizeof(g_opts[0]);

static HWND g_wnd, g_tab, g_modList, g_exeCombo, g_status;
static std::vector<HWND> g_tabCtl[TAB_COUNT];
static HFONT g_font, g_bold;
static int g_dpi = 96;
static int S(int v) { return MulDiv(v, g_dpi, 96); }
static std::string IniFile(int f) { return Path(f == F_RENDER ? "swat3d11.ini" : "swat3ragdoll.ini"); }

static std::string IniGet(const std::string& file, const char* sec, const char* key, const char* def) {
    char buf[512];
    GetPrivateProfileStringA(sec, key, def, buf, sizeof(buf), file.c_str());
    std::string s = buf;
    size_t c = s.find(';'); if (c != std::string::npos) s.erase(c);          // trailing comment
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    return s;
}

// Change one "Key=value" line of an ini file and leave every other line exactly as it is
// (value == nullptr removes the line). Adds the key / section when missing.
static bool IniSet(const std::string& file, const char* sec, const char* key, const char* value) {
    std::string text; { FILE* f = fopen(file.c_str(), "rb"); if (f) { char buf[4096]; size_t n; while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n); fclose(f); } }
    std::vector<std::string> lines; size_t pos = 0;
    while (pos < text.size()) { size_t e = text.find('\n', pos); if (e == std::string::npos) { lines.push_back(text.substr(pos)); break; } lines.push_back(text.substr(pos, e - pos)); pos = e + 1; }
    for (auto& l : lines) if (!l.empty() && l.back() == '\r') l.pop_back();
    auto trim = [](std::string t) { while (!t.empty() && strchr(" \t", t.back())) t.pop_back(); size_t i = 0; while (i < t.size() && strchr(" \t", t[i])) i++; return t.substr(i); };
    int secStart = -1, secEnd = (int)lines.size(), keyLine = -1; size_t klen = strlen(key);
    for (int i = 0; i < (int)lines.size(); i++) {
        std::string t = trim(lines[i]);
        if (t.size() > 2 && t[0] == '[' && t.back() == ']') {
            if (secStart >= 0) { secEnd = i; break; }
            if (!_stricmp(t.substr(1, t.size() - 2).c_str(), sec)) secStart = i;
            continue;
        }
        if (secStart >= 0 && keyLine < 0 && !_strnicmp(t.c_str(), key, klen)) { std::string r = trim(t.substr(klen)); if (!r.empty() && r[0] == '=') keyLine = i; }
    }
    std::string line = value ? std::string(key) + "=" + value : "";
    if (keyLine >= 0) { if (value) lines[keyLine] = line; else lines.erase(lines.begin() + keyLine); }
    else if (value) {
        if (secStart < 0) { if (!lines.empty() && !trim(lines.back()).empty()) lines.push_back(""); lines.push_back(std::string("[") + sec + "]"); lines.push_back(line); }
        else { int at = secEnd; while (at > secStart + 1 && trim(lines[at - 1]).empty()) at--; lines.insert(lines.begin() + at, line); }
    }
    FILE* f = fopen(file.c_str(), "wb"); if (!f) return false;
    bool ok = true;
    for (auto& l : lines) ok &= fprintf(f, "%s\r\n", l.c_str()) >= 0;
    ok &= fclose(f) == 0;
    return ok;
}

static HWND Make(const char* cls, const char* text, DWORD style, int x, int y, int w, int h, int tab, int id = 0, DWORD ex = 0) {
    HWND c = CreateWindowExA(ex, cls, text, WS_CHILD | style, S(x), S(y), S(w), S(h), g_wnd, (HMENU)(INT_PTR)id, GetModuleHandleA(nullptr), nullptr);
    SendMessageA(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    if (tab >= 0) g_tabCtl[tab].push_back(c);
    return c;
}

static void ComboAdd(Opt& o, const std::string& value, const std::string& label) {
    SendMessageA(o.ctl, CB_ADDSTRING, 0, (LPARAM)label.c_str());
    o.values.push_back(value);
}
static void ComboSelect(Opt& o, const std::string& value) {
    for (size_t i = 0; i < o.values.size(); i++)
        if (!_stricmp(o.values[i].c_str(), value.c_str()) || (atof(value.c_str()) != 0 && atof(o.values[i].c_str()) == atof(value.c_str()) && o.type == T_COMBO && strchr("0123456789.", value[0]))) {
            SendMessageA(o.ctl, CB_SETCURSEL, i, 0); return;
        }
    ComboAdd(o, value, value + "  (custom)");                               // keep a value we do not list
    SendMessageA(o.ctl, CB_SETCURSEL, o.values.size() - 1, 0);
}

struct Mode { int w, h; };
static std::vector<Mode> Modes(int maxEach) {
    std::vector<Mode> m;
    DEVMODEA dm; memset(&dm, 0, sizeof(dm)); dm.dmSize = sizeof(dm);
    for (int i = 0; EnumDisplaySettingsA(nullptr, i, &dm); i++) {
        int w = dm.dmPelsWidth, h = dm.dmPelsHeight;
        if (w < 640 || h < 480 || w > maxEach || h > maxEach) continue;
        bool have = false; for (auto& k : m) if (k.w == w && k.h == h) have = true;
        if (!have) m.push_back({w, h});
    }
    std::sort(m.begin(), m.end(), [](const Mode& a, const Mode& b) { return a.w != b.w ? a.w < b.w : a.h < b.h; });
    return m;
}

// ---- mods -----------------------------------------------------------------------------------
// The game looks in its "Mods" folder (one sub-folder, .res or .zip file per mod) and activates
// the ones named in swat.ini: Mods=name1,name2 (in the section that also holds its other settings).
static std::string g_modSection;
static std::vector<std::string> g_modNames;

static std::string ReadFile(const std::string& p) {
    std::string s; FILE* f = fopen(p.c_str(), "rb"); if (!f) return s;
    char buf[4096]; size_t n; while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f); return s;
}
static std::string Trim(std::string s) {
    while (!s.empty() && strchr(" \t\r\n\"", s.back())) s.pop_back();
    size_t i = 0; while (i < s.size() && strchr(" \t\"", s[i])) i++;
    return s.substr(i);
}
// Which section of swat.ini holds the Mods line (or would)?
static std::string FindModSection() {
    std::string text = ReadFile(Path("swat.ini")), cur, withPaths, first;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos); if (e == std::string::npos) e = text.size();
        std::string line = Trim(text.substr(pos, e - pos)); pos = e + 1;
        if (line.size() > 2 && line[0] == '[' && line.back() == ']') { cur = line.substr(1, line.size() - 2); if (first.empty()) first = cur; continue; }
        if (!_strnicmp(line.c_str(), "Mods", 4) && Trim(line.substr(4))[0] == '=') return cur;
        if (!_strnicmp(line.c_str(), "Paths", 5) && withPaths.empty()) withPaths = cur;
    }
    return !withPaths.empty() ? withPaths : first;
}
static std::string ModAuthor(const std::string& folder) {
    std::string text = ReadFile(folder + "\\modinfo.dat");
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos); if (e == std::string::npos) e = text.size();
        std::string line = Trim(text.substr(pos, e - pos)); pos = e + 1;
        if (!_strnicmp(line.c_str(), "Author", 6)) { std::string v = Trim(line.substr(6)); if (!v.empty() && v[0] == '=') v = Trim(v.substr(1)); return v; }
    }
    return "";
}
static void LoadMods() {
    ListView_DeleteAllItems(g_modList);
    g_modNames.clear();
    g_modSection = FindModSection();
    std::vector<std::string> active;
    if (!g_modSection.empty()) {
        char buf[6000]; GetPrivateProfileStringA(g_modSection.c_str(), "Mods", "", buf, sizeof(buf), Path("swat.ini").c_str());
        std::string s = buf; size_t p = 0;
        while (p <= s.size()) { size_t e = s.find(',', p); if (e == std::string::npos) e = s.size(); std::string n = Trim(s.substr(p, e - p)); if (!n.empty()) active.push_back(n); p = e + 1; }
    }
    struct Found { std::string name, kind; };
    std::vector<Found> found;
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(Path("Mods\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::string n = fd.cFileName;
            if (n == "." || n == "..") continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { std::string a = ModAuthor(Path("Mods\\") + n); found.push_back({n, a.empty() ? "Folder" : "by " + a}); }
            else {
                size_t dot = n.rfind('.'); if (dot == std::string::npos) continue;
                std::string ext = n.substr(dot);
                if (_stricmp(ext.c_str(), ".res") && _stricmp(ext.c_str(), ".zip")) continue;
                found.push_back({n.substr(0, dot), ext.substr(1) + " file"});
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    // active ones first, in the order the game loads them; then the rest by name
    std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return _stricmp(a.name.c_str(), b.name.c_str()) < 0; });
    std::vector<Found> order;
    for (auto& a : active) {
        bool have = false;
        for (auto& f : found) if (!_stricmp(f.name.c_str(), a.c_str())) { order.push_back(f); have = true; break; }
        if (!have) order.push_back({a, "listed in swat.ini, not found in the Mods folder"});
    }
    for (auto& f : found) { bool in = false; for (auto& o : order) if (!_stricmp(o.name.c_str(), f.name.c_str())) in = true; if (!in) order.push_back(f); }
    for (size_t i = 0; i < order.size(); i++) {
        LVITEMA it = {}; it.mask = LVIF_TEXT; it.iItem = (int)i; it.pszText = (char*)order[i].name.c_str();
        ListView_InsertItem(g_modList, &it);
        ListView_SetItemText(g_modList, (int)i, 1, (char*)order[i].kind.c_str());
        ListView_SetCheckState(g_modList, (int)i, i < active.size());
        g_modNames.push_back(order[i].name);
    }
}
static bool SaveMods() {
    if (g_modNames.empty() && !Exists(Path("swat.ini"))) return true;
    std::string list;
    for (size_t i = 0; i < g_modNames.size(); i++) if (ListView_GetCheckState(g_modList, (int)i)) { if (!list.empty()) list += ","; list += g_modNames[i]; }
    if (g_modSection.empty()) return list.empty();
    char buf[6000]; GetPrivateProfileStringA(g_modSection.c_str(), "Mods", "", buf, sizeof(buf), Path("swat.ini").c_str());
    if (list == Trim(buf)) return true;                                     // nothing changed: leave swat.ini alone
    return IniSet(Path("swat.ini"), g_modSection.c_str(), "Mods", list.empty() ? nullptr : list.c_str());
}
static void MoveMod(int dir) {
    int i = ListView_GetNextItem(g_modList, -1, LVNI_SELECTED), j = i + dir;
    if (i < 0 || j < 0 || j >= (int)g_modNames.size()) return;
    char a0[260], a1[260], b0[260], b1[260];
    ListView_GetItemText(g_modList, i, 0, a0, 260); ListView_GetItemText(g_modList, i, 1, a1, 260);
    ListView_GetItemText(g_modList, j, 0, b0, 260); ListView_GetItemText(g_modList, j, 1, b1, 260);
    BOOL ca = ListView_GetCheckState(g_modList, i), cb = ListView_GetCheckState(g_modList, j);
    ListView_SetItemText(g_modList, i, 0, b0); ListView_SetItemText(g_modList, i, 1, b1);
    ListView_SetItemText(g_modList, j, 0, a0); ListView_SetItemText(g_modList, j, 1, a1);
    ListView_SetCheckState(g_modList, i, cb); ListView_SetCheckState(g_modList, j, ca);
    std::swap(g_modNames[i], g_modNames[j]);
    ListView_SetItemState(g_modList, j, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
}

// ---- load / save ----------------------------------------------------------------------------
static void LoadAll() {
    for (int i = 0; i < kNumOpts; i++) {
        Opt& o = g_opts[i];
        std::string v = IniGet(IniFile(o.file), o.section, o.key, o.def);
        if (o.type == T_CHECK) SendMessageA(o.ctl, BM_SETCHECK, atoi(v.c_str()) ? BST_CHECKED : BST_UNCHECKED, 0);
        else if (o.type == T_WINSIZE) ComboSelect(o, v + "x" + IniGet(IniFile(o.file), "Display", "WindowHeight", "720"));
        else ComboSelect(o, v);
    }
    LoadMods();
}
static std::string Value(Opt& o) {
    if (o.type == T_CHECK) return SendMessageA(o.ctl, BM_GETCHECK, 0, 0) == BST_CHECKED ? "1" : "0";
    int sel = (int)SendMessageA(o.ctl, CB_GETCURSEL, 0, 0);
    return sel >= 0 && sel < (int)o.values.size() ? o.values[sel] : o.def;
}
static bool SaveAll() {
    bool ok = true, ragdoll = Exists(Path("swat3ragdoll.ini")) || Exists(Path("dsound.dll"));
    for (int i = 0; i < kNumOpts; i++) {
        Opt& o = g_opts[i];
        if (o.file == F_RAGDOLL && !ragdoll) continue;
        std::string file = IniFile(o.file), v = Value(o);
        if (o.type == T_WINSIZE) {
            int w = atoi(v.c_str()), h = atoi(v.c_str() + v.find('x') + 1);
            if (w == atoi(IniGet(file, "Display", "WindowWidth", "1280").c_str()) && h == atoi(IniGet(file, "Display", "WindowHeight", "720").c_str())) continue;
            char b[16]; sprintf(b, "%d", w); ok &= IniSet(file, "Display", "WindowWidth", b);
            sprintf(b, "%d", h); ok &= IniSet(file, "Display", "WindowHeight", b);
            continue;
        }
        std::string cur = IniGet(file, o.section, o.key, "\x01");
        if (cur == v || (cur == "\x01" && v == o.def && false)) continue;   // unchanged: do not touch the file
        if (cur != "\x01" && o.type != T_MISSIONRES && atof(cur.c_str()) == atof(v.c_str()) && strchr("0123456789.", v[0]) && strchr("0123456789.", cur[0])) continue;
        ok &= IniSet(file, o.section, o.key, v.c_str());
    }
    ok &= SaveMods();
    int sel = (int)SendMessageA(g_exeCombo, CB_GETCURSEL, 0, 0);
    if (sel >= 0) { char exe[260]; SendMessageA(g_exeCombo, CB_GETLBTEXT, sel, (LPARAM)exe); if (IniGet(Path("swat3launcher.ini"), "Launcher", "Exe", "") != exe) IniSet(Path("swat3launcher.ini"), "Launcher", "Exe", exe); }
    SetWindowTextA(g_status, ok ? "Settings saved." : "Could not save - is the game folder write-protected? Try running the launcher as administrator.");
    return ok;
}
static void Play() {
    if (!SaveAll()) return;
    char exe[260] = "";
    int sel = (int)SendMessageA(g_exeCombo, CB_GETCURSEL, 0, 0);
    if (sel >= 0) SendMessageA(g_exeCombo, CB_GETLBTEXT, sel, (LPARAM)exe);
    if (!exe[0]) { MessageBoxA(g_wnd, "No SWAT 3 program (swat.exe) was found next to the launcher.\n\nPut SWAT3Launcher.exe in the SWAT 3 game folder.", "SWAT 3 Launcher", MB_ICONWARNING); return; }
    std::string cmd = "\"" + Path(exe) + "\"";
    STARTUPINFOA si = {}; si.cb = sizeof(si); PROCESS_INFORMATION pi = {};
    std::vector<char> buf(cmd.begin(), cmd.end()); buf.push_back(0);
    if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, g_dir, &si, &pi)) {
        // needs administrator rights (or similar): let Windows ask
        if ((INT_PTR)ShellExecuteA(g_wnd, nullptr, Path(exe).c_str(), nullptr, g_dir, SW_SHOWNORMAL) <= 32) { MessageBoxA(g_wnd, "The game could not be started.", "SWAT 3 Launcher", MB_ICONWARNING); return; }
    } else { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    DestroyWindow(g_wnd);
}

static void ShowTab(int t) {
    for (int i = 0; i < TAB_COUNT; i++) for (HWND c : g_tabCtl[i]) ShowWindow(c, i == t ? SW_SHOW : SW_HIDE);
}

enum { ID_PLAY = 100, ID_SAVE, ID_CLOSE, ID_MODUP, ID_MODDOWN, ID_MODFOLDER, ID_MODREFRESH };

static void Build() {
    const int W = 620, top = 44, rowH = 29, lblW = 290, ctlX = 322, ctlW = 262;
    g_tab = Make(WC_TABCONTROLA, "", WS_VISIBLE | WS_CLIPSIBLINGS, 8, 8, W - 16, 440, -1);
    static const char* tabs[TAB_COUNT] = {"Display", "Gameplay (QoL)", "Mods", "Ragdoll"};
    for (int i = 0; i < TAB_COUNT; i++) { TCITEMA ti = {}; ti.mask = TCIF_TEXT; ti.pszText = (char*)tabs[i]; SendMessageA(g_tab, TCM_INSERTITEMA, i, (LPARAM)&ti); }
    int row[TAB_COUNT] = {0};
    bool haveRender = Exists(Path("ddraw.dll")), haveRag = Exists(Path("dsound.dll")) && Exists(Path("swat3ragdoll.ini"));
    if (!haveRender) {
        Make("STATIC", "The renderer (ddraw.dll) is not in this folder. These settings only work once it is installed.", WS_VISIBLE, 22, top, W - 44, 18, TAB_DISPLAY);
        Make("STATIC", "The renderer (ddraw.dll) is not in this folder. These settings only work once it is installed.", 0, 22, top, W - 44, 18, TAB_QOL);
        row[TAB_DISPLAY] = row[TAB_QOL] = 1;
    }
    if (!haveRag) { Make("STATIC", "The ragdoll add-on (dsound.dll + swat3ragdoll.ini) is not installed in this folder.", 0, 22, top, W - 44, 18, TAB_RAGDOLL); row[TAB_RAGDOLL] = 1; }
    for (int i = 0; i < kNumOpts; i++) {
        Opt& o = g_opts[i];
        int y = top + row[o.tab]++ * rowH;
        if (o.type == T_CHECK) { o.ctl = Make("BUTTON", o.label, BS_AUTOCHECKBOX | WS_TABSTOP, 22, y, W - 50, 22, o.tab); continue; }
        o.lbl = Make("STATIC", o.label, 0, 22, y + 3, lblW, 20, o.tab);
        o.ctl = Make("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ctlX, y, ctlW, 300, o.tab);
        if (o.type == T_MISSIONRES) {
            ComboAdd(o, "game", "Chosen in the game's video options");
            DEVMODEA dm; memset(&dm, 0, sizeof(dm)); dm.dmSize = sizeof(dm);
            char b[64] = "Same as the desktop";
            if (EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm)) sprintf(b, "Same as the desktop (%lu x %lu)", dm.dmPelsWidth, dm.dmPelsHeight);
            ComboAdd(o, "desktop", b);
            for (auto& m : Modes(2048)) { char v[32], l[32]; sprintf(v, "%dx%d", m.w, m.h); sprintf(l, "%d x %d", m.w, m.h); ComboAdd(o, v, l); }
        } else if (o.type == T_WINSIZE) {
            std::vector<Mode> ms = Modes(8192);
            static const Mode common[] = {{1024, 768}, {1280, 720}, {1600, 900}, {1920, 1080}};
            for (auto& c : common) { bool have = false; for (auto& k : ms) if (k.w == c.w && k.h == c.h) have = true; if (!have) ms.push_back(c); }
            std::sort(ms.begin(), ms.end(), [](const Mode& a, const Mode& b) { return a.w != b.w ? a.w < b.w : a.h < b.h; });
            for (auto& m : ms) { char v[32], l[32]; sprintf(v, "%dx%d", m.w, m.h); sprintf(l, "%d x %d", m.w, m.h); ComboAdd(o, v, l); }
        } else {
            std::string c = o.choices; size_t p = 0;
            while (p < c.size()) {
                size_t e = c.find(';', p); if (e == std::string::npos) e = c.size();
                std::string item = c.substr(p, e - p); size_t bar = item.find('|');
                ComboAdd(o, item.substr(0, bar), bar == std::string::npos ? item : item.substr(bar + 1));
                p = e + 1;
            }
        }
    }
    // Mods tab
    Make("STATIC", "Tick the mods the game should load. Mods are the folders, .res and .zip files in the game's \"Mods\" folder.", 0, 22, top, W - 44, 18, TAB_MODS);
    g_modList = Make(WC_LISTVIEWA, "", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER | WS_TABSTOP, 22, top + 24, W - 150, 330, TAB_MODS, 0, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(g_modList, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
    LVCOLUMNA col = {}; col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.pszText = (char*)"Mod"; col.cx = S(210); ListView_InsertColumn(g_modList, 0, &col);
    col.pszText = (char*)"Details"; col.cx = S(230); ListView_InsertColumn(g_modList, 1, &col);
    Make("BUTTON", "Move up", BS_PUSHBUTTON | WS_TABSTOP, W - 120, top + 24, 98, 26, TAB_MODS, ID_MODUP);
    Make("BUTTON", "Move down", BS_PUSHBUTTON | WS_TABSTOP, W - 120, top + 54, 98, 26, TAB_MODS, ID_MODDOWN);
    Make("BUTTON", "Open folder", BS_PUSHBUTTON | WS_TABSTOP, W - 120, top + 100, 98, 26, TAB_MODS, ID_MODFOLDER);
    Make("BUTTON", "Refresh", BS_PUSHBUTTON | WS_TABSTOP, W - 120, top + 130, 98, 26, TAB_MODS, ID_MODREFRESH);
    Make("STATIC", "Mods higher in the list are loaded first. Mods that replace the game's own files (like Last Resort) are not listed here.", 0, 22, top + 360, W - 44, 34, TAB_MODS);

    // bottom bar
    Make("STATIC", "Game:", WS_VISIBLE, 12, 462, 40, 20, -1);
    g_exeCombo = Make("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VISIBLE | WS_TABSTOP, 54, 458, 170, 200, -1);
    HWND play = Make("BUTTON", "Play", BS_DEFPUSHBUTTON | WS_VISIBLE | WS_TABSTOP, W - 300, 456, 110, 30, -1, ID_PLAY);
    SendMessageA(play, WM_SETFONT, (WPARAM)g_bold, TRUE);
    Make("BUTTON", "Save", BS_PUSHBUTTON | WS_VISIBLE | WS_TABSTOP, W - 184, 456, 84, 30, -1, ID_SAVE);
    Make("BUTTON", "Close", BS_PUSHBUTTON | WS_VISIBLE | WS_TABSTOP, W - 94, 456, 84, 30, -1, ID_CLOSE);
    g_status = Make("STATIC", "", WS_VISIBLE, 12, 494, W - 24, 18, -1);

    // game programs in this folder
    char self[MAX_PATH]; GetModuleFileNameA(nullptr, self, MAX_PATH);
    const char* selfName = strrchr(self, '\\'); selfName = selfName ? selfName + 1 : self;
    std::string want = IniGet(Path("swat3launcher.ini"), "Launcher", "Exe", "");
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(Path("swat*.exe").c_str(), &fd);
    int pick = -1, n = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!_stricmp(fd.cFileName, selfName) || !_strnicmp(fd.cFileName, "swat3launcher", 13)) continue;
            SendMessageA(g_exeCombo, CB_ADDSTRING, 0, (LPARAM)fd.cFileName);
            if (!_stricmp(fd.cFileName, want.c_str())) pick = n;
            if (pick < 0 && want.empty() && !_stricmp(fd.cFileName, "swat.exe")) pick = n;
            n++;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    SendMessageA(g_exeCombo, CB_SETCURSEL, pick >= 0 ? pick : 0, 0);
    if (!n) SetWindowTextA(g_status, "No swat.exe found here - put SWAT3Launcher.exe in the SWAT 3 game folder.");
    ShowTab(0);
    LoadAll();
}

static LRESULT CALLBACK WndProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_NOTIFY:
        if (((NMHDR*)lp)->hwndFrom == g_tab && ((NMHDR*)lp)->code == TCN_SELCHANGE) ShowTab((int)SendMessageA(g_tab, TCM_GETCURSEL, 0, 0));
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_PLAY: Play(); return 0;
        case ID_SAVE: SaveAll(); return 0;
        case ID_CLOSE: case IDCANCEL: DestroyWindow(w); return 0;
        case ID_MODUP: MoveMod(-1); return 0;
        case ID_MODDOWN: MoveMod(1); return 0;
        case ID_MODREFRESH: LoadMods(); return 0;
        case ID_MODFOLDER:
            if (!IsDir(Path("Mods"))) CreateDirectoryA(Path("Mods").c_str(), nullptr);
            ShellExecuteA(w, "open", Path("Mods").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        break;
    case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN:
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(w, m, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR cmdLine, int) {
    GetModuleFileNameA(nullptr, g_dir, MAX_PATH);
    char* sl = strrchr(g_dir, '\\'); if (sl) sl[1] = 0;
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    HDC dc = GetDC(nullptr); g_dpi = GetDeviceCaps(dc, LOGPIXELSY); ReleaseDC(nullptr, dc);
    g_font = CreateFontA(-MulDiv(9, g_dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    g_bold = CreateFontA(-MulDiv(10, g_dpi, 72), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    WNDCLASSA wc = {}; wc.lpfnWndProc = WndProc; wc.hInstance = inst; wc.lpszClassName = "Swat3Launcher";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(1));
    RegisterClassA(&wc);
    RECT r = {0, 0, S(620), S(520)};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    int w = r.right - r.left, h = r.bottom - r.top;
    g_wnd = CreateWindowA("Swat3Launcher", "SWAT 3 Launcher " LAUNCHER_VERSION, style, (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h, nullptr, nullptr, inst, nullptr);
    Build();
#ifdef LAUNCHER_TEST
    // test switches: "-tab N" opens a tab, "-save" saves and exits (used by the automated test)
    if (cmdLine && strstr(cmdLine, "-tab ")) { int t = atoi(strstr(cmdLine, "-tab ") + 5); SendMessageA(g_tab, TCM_SETCURSEL, t, 0); ShowTab(t); }
    if (cmdLine && strstr(cmdLine, "-flip")) {            // test: change everything
        for (int i = 0; i < kNumOpts; i++) {
            Opt& o = g_opts[i];
            if (o.type == T_CHECK) SendMessageA(o.ctl, BM_SETCHECK, SendMessageA(o.ctl, BM_GETCHECK, 0, 0) == BST_CHECKED ? BST_UNCHECKED : BST_CHECKED, 0);
            else SendMessageA(o.ctl, CB_SETCURSEL, o.type == T_MISSIONRES ? 1 : 0, 0);
        }
        for (size_t i = 0; i < g_modNames.size(); i++) ListView_SetCheckState(g_modList, (int)i, i != 0);
        MoveMod(0); ListView_SetItemState(g_modList, 2, LVIS_SELECTED, LVIS_SELECTED); MoveMod(-1);
    }
    if (cmdLine && strstr(cmdLine, "-save")) { SaveAll(); return 0; }
#endif
    ShowWindow(g_wnd, SW_SHOW);
    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageA(g_wnd, &msg)) continue;
        TranslateMessage(&msg); DispatchMessageA(&msg);
    }
    return 0;
}
