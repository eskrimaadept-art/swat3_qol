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
extern "C" int atexit(void (*)(void));
