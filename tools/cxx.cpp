// Minimal C++ support without a C++ runtime library: new/delete on the C heap, and stubs the
// compiler may reference. (Built with -fno-exceptions -fno-rtti -fno-threadsafe-statics.)
#include <stdlib.h>
#include <stddef.h>
void* operator new(size_t n) { void* p = malloc(n ? n : 1); if (!p) abort(); return p; }
void* operator new[](size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }
extern "C" void __cxa_pure_virtual() { abort(); }


// DLL entry without a C runtime: run the global constructors (lld provides __CTOR_LIST__ for
// MinGW targets: -1, constructors..., 0), then the DLL's own DllMain.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef void (*Ctor)();
extern "C" Ctor __CTOR_LIST__[];
extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID);
extern "C" BOOL WINAPI DllMainCRTStartup(HINSTANCE h, DWORD reason, LPVOID p) {
    if (reason == DLL_PROCESS_ATTACH) {
        int n = 0;
        while (__CTOR_LIST__[n + 1]) n++;
        for (int i = n; i >= 1; i--) __CTOR_LIST__[i]();
    }
    return DllMain(h, reason, p);
}
extern "C" int atexit(void (*)(void)) { return 0; }   // nothing is torn down at exit
