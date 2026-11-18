// Shared basics: settings (swat3d11.ini), log file, small containers (no C++ runtime library).
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define SWAT3D11_VERSION "0.14.4"

// ---- settings ---------------------------------------------------------------------------------
struct Config {
    // [Display]
    int fullscreen = 1;
    int outputW = 0, outputH = 0;          // 0 = monitor size
    int windowW = 1280, windowH = 720;
    int vsync = 1;
    int msaa = 0;
    int anisotropy = 8;
    int mipmaps = 1;
    int fov = 0;                           // horizontal degrees, 0 = game default
    int smoothUI = 1;
    // [Game]
    int gameFixes = 1;
    int allResolutions = 1;
    char missionRes[32] = "game";
    int skipIntro = 0;
    int maxTexture = 2048;
    int drawDistance = 2800;
    // [UI]
    int extraResolutions = 1;
    // [Debug]
    int logLevel = 1;
    int builtinShaders = 0;
};
extern Config g_cfg;
extern char g_dir[MAX_PATH];               // game folder, with trailing backslash
extern char g_iniPath[MAX_PATH];
extern const char* g_baseName;             // "swat3d11"
void ConfigLoad();
int IniInt(const char* sec, const char* key, int def);
void IniStr(const char* sec, const char* key, const char* def, char* out, int n);

// ---- log ------------------------------------------------------------------------------------
void LogInit();
void Log(const char* fmt, ...);
void LogV(int level, const char* fmt, ...);            // only when LogLevel >= level
void LogOnce(const char* key, const char* fmt, ...);   // once per key
void Unimpl(const char* what);                         // once per name: "UNIMPLEMENTED: ..."
bool Tracing();                                        // F11 frame trace active
void TraceLog(const char* fmt, ...);
void TraceFrameEnd();
void TraceStart();

// ---- tiny growable array (POD-ish element types) --------------------------------------------
template <class T> struct Vec {
    T* p = nullptr; int n = 0, cap = 0;
    constexpr Vec() {}
    Vec(const Vec&) = delete; Vec& operator=(const Vec&) = delete;
    ~Vec() { free(p); }
    void reserve(int c) { if (c <= cap) return; int nc = cap ? cap : 8; while (nc < c) nc *= 2; p = (T*)realloc(p, sizeof(T) * nc); cap = nc; }
    void resize(int m) { reserve(m); if (m > n) memset((void*)(p + n), 0, sizeof(T) * (m - n)); n = m; }
    void push(const T& v) { reserve(n + 1); memcpy((void*)(p + n), (const void*)&v, sizeof(T)); n++; }
    void clear() { n = 0; }
    void erase(int i) { memmove((void*)(p + i), (void*)(p + i + 1), sizeof(T) * (n - i - 1)); n--; }
    T& operator[](int i) { return p[i]; }
    const T& operator[](int i) const { return p[i]; }
    T* begin() { return p; } T* end() { return p + n; }
    int size() const { return n; }
    bool empty() const { return n == 0; }
    T& back() { return p[n - 1]; }
};

// Simple lock for state shared with the window thread
struct CritSec {
    CRITICAL_SECTION cs; CritSec() { InitializeCriticalSection(&cs); }
    void lock() { EnterCriticalSection(&cs); } void unlock() { LeaveCriticalSection(&cs); }
};
struct Guard { CritSec& c; Guard(CritSec& x) : c(x) { c.lock(); } ~Guard() { c.unlock(); } };

static inline bool Readable(const void* p, size_t n) { return p && !IsBadReadPtr(p, n); }
uint8_t* TestAddr(const char* envName);    // test-harness address override (nullptr = none)
