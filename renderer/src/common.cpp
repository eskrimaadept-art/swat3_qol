#include "common.h"
#include <stdio.h>
#include <stdarg.h>

Config g_cfg;
char g_dir[MAX_PATH];
char g_iniPath[MAX_PATH];
const char* g_baseName = "swat3d11";

static FILE* g_log = nullptr;
static CritSec g_logLock;

void LogInit() {
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    strcpy(g_dir, exe);
    char* sl = strrchr(g_dir, '\\'); if (sl) sl[1] = 0; else g_dir[0] = 0;
    _snprintf(g_iniPath, MAX_PATH, "%s%s.ini", g_dir, g_baseName);
    int level = GetPrivateProfileIntA("Debug", "LogLevel", 1, g_iniPath);
    if (level > 0) {
        char path[MAX_PATH];
        _snprintf(path, MAX_PATH, "%s%s.log", g_dir, g_baseName);
        g_log = fopen(path, "w");
    }
    SYSTEMTIME t; GetLocalTime(&t);
    Log("SWAT 3 Direct3D 11 renderer %s - %04d-%02d-%02d %02d:%02d - %s", SWAT3D11_VERSION, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, exe);
}

static void LogWrite(const char* fmt, va_list ap) {
    if (!g_log) return;
    Guard g(g_logLock);
    vfprintf(g_log, fmt, ap);
    fputc('\n', g_log);
    fflush(g_log);
}
void Log(const char* fmt, ...) { va_list ap; va_start(ap, fmt); LogWrite(fmt, ap); va_end(ap); }
void LogV(int level, const char* fmt, ...) {
    if (g_cfg.logLevel < level) return;
    va_list ap; va_start(ap, fmt); LogWrite(fmt, ap); va_end(ap);
}

static const char* g_onceKeys[256]; static int g_onceN = 0;
static bool FirstTime(const char* key) {
    Guard g(g_logLock);
    for (int i = 0; i < g_onceN; i++) if (g_onceKeys[i] == key || !strcmp(g_onceKeys[i], key)) return false;
    if (g_onceN < 256) g_onceKeys[g_onceN++] = key;
    return true;
}
void LogOnce(const char* key, const char* fmt, ...) {
    if (!FirstTime(key)) return;
    va_list ap; va_start(ap, fmt); LogWrite(fmt, ap); va_end(ap);
}
void Unimpl(const char* what) { if (FirstTime(what)) Log("UNIMPLEMENTED: %s (please send this log)", what); }

// ---- F11 frame trace ------------------------------------------------------------------------
static FILE* g_trace = nullptr;
static int g_traceFrames = 0;
bool Tracing() { return g_trace != nullptr; }
void TraceStart() {
    if (g_trace) return;
    char path[MAX_PATH];
    _snprintf(path, MAX_PATH, "%s%s_trace.txt", g_dir, g_baseName);
    g_trace = fopen(path, "w");
    g_traceFrames = 0;
    if (g_trace) { fprintf(g_trace, "%s %s frame trace\n", g_baseName, SWAT3D11_VERSION); Log("F11: tracing 2 frames"); }
}
void TraceLog(const char* fmt, ...) {
    if (!g_trace) return;
    va_list ap; va_start(ap, fmt); vfprintf(g_trace, fmt, ap); va_end(ap);
    fputc('\n', g_trace);
}
void TraceFrameEnd() {
    if (!g_trace) return;
    fprintf(g_trace, "==== end of frame %d ====\n", g_traceFrames);
    if (++g_traceFrames >= 2) { fclose(g_trace); g_trace = nullptr; Log("trace written to %s_trace.txt", g_baseName); }
}

// ---- settings -------------------------------------------------------------------------------
int IniInt(const char* sec, const char* key, int def) {
    char buf[64];
    IniStr(sec, key, "", buf, sizeof(buf));
    if (!buf[0]) return def;
    return atoi(buf);
}
void IniStr(const char* sec, const char* key, const char* def, char* out, int n) {
    char tmp[256];
    GetPrivateProfileStringA(sec, key, "", tmp, sizeof(tmp), g_iniPath);
    char* c = strchr(tmp, ';'); if (c) *c = 0;                       // trailing comment
    int len = (int)strlen(tmp);
    while (len && (tmp[len - 1] == ' ' || tmp[len - 1] == '\t')) tmp[--len] = 0;
    const char* s = tmp; while (*s == ' ' || *s == '\t') s++;
    if (!*s) s = def;
    strncpy(out, s, n - 1); out[n - 1] = 0;
}

void ConfigLoad() {
    Config& c = g_cfg;
    c.fullscreen = IniInt("Display", "Fullscreen", c.fullscreen);
    c.outputW = IniInt("Display", "OutputWidth", c.outputW);
    c.outputH = IniInt("Display", "OutputHeight", c.outputH);
    c.windowW = IniInt("Display", "WindowWidth", c.windowW);
    c.windowH = IniInt("Display", "WindowHeight", c.windowH);
    c.vsync = IniInt("Display", "VSync", c.vsync);
    c.msaa = IniInt("Display", "MSAA", c.msaa);
    c.anisotropy = IniInt("Display", "Anisotropy", c.anisotropy);
    c.mipmaps = IniInt("Display", "Mipmaps", c.mipmaps);
    c.fov = IniInt("Display", "FieldOfView", c.fov);
    c.smoothUI = IniInt("Display", "SmoothUI", c.smoothUI);
    c.gameFixes = IniInt("Game", "Enabled", c.gameFixes);
    c.allResolutions = IniInt("Game", "InGameResolutions", c.allResolutions);
    IniStr("Game", "MissionResolution", "game", c.missionRes, sizeof(c.missionRes));
    c.skipIntro = IniInt("Game", "SkipIntro", c.skipIntro);
    c.maxTexture = IniInt("Game", "MaxTextureSize", c.maxTexture);
    c.drawDistance = IniInt("Game", "DrawDistance", c.drawDistance);
    c.extraResolutions = IniInt("UI", "ExtraResolutions", c.extraResolutions);
    c.logLevel = IniInt("Debug", "LogLevel", c.logLevel);
    c.builtinShaders = IniInt("Debug", "BuiltInShaders", c.builtinShaders);
    if (c.msaa != 0 && c.msaa != 2 && c.msaa != 4 && c.msaa != 8) c.msaa = 0;
    if (c.anisotropy < 1) c.anisotropy = 1; if (c.anisotropy > 16) c.anisotropy = 16;
    if (c.windowW < 320) c.windowW = 320; if (c.windowH < 240) c.windowH = 240;
    Log("settings: fullscreen %d, output %dx%d, window %dx%d, vsync %d, msaa %d, anisotropy %d, mipmaps %d, fov %d, smooth UI %d",
        c.fullscreen, c.outputW, c.outputH, c.windowW, c.windowH, c.vsync, c.msaa, c.anisotropy, c.mipmaps, c.fov, c.smoothUI);
}

uint8_t* TestAddr(const char* name) {
    char env[32];
    return GetEnvironmentVariableA(name, env, sizeof(env)) ? (uint8_t*)(uintptr_t)strtoul(env, nullptr, 16) : nullptr;
}
