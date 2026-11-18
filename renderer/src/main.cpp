// ddraw.dll entry points. The game loads this instead of Windows' DirectDraw.
#include "common.h"
#include "frontend.h"
#include "gamefix.h"
#include "backend.h"
#include <ddraw.h>
#include <stdio.h>

// ---- crash report into the log ------------------------------------------------------------------
static const char* ModuleOf(void* addr, uintptr_t* off) {
    static char name[MAX_PATH];
    HMODULE m = nullptr;
    *off = (uintptr_t)addr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &m) && m) {
        GetModuleFileNameA(m, name, MAX_PATH);
        *off = (uintptr_t)addr - (uintptr_t)m;
        const char* s = strrchr(name, '\\');
        return s ? s + 1 : name;
    }
    return "?";
}
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* e) {
    static int count = 0;
    EXCEPTION_RECORD* r = e->ExceptionRecord; CONTEXT* c = e->ContextRecord;
    DWORD code = r->ExceptionCode;
    // only real faults (access violation, illegal instruction, divide by zero, stack overflow...);
    // the game may still handle some of them itself
    if (code < 0xC0000000 || code == 0xE06D7363 || count >= 3) return EXCEPTION_CONTINUE_SEARCH;
    {   // faults inside Windows / sound / driver code are usually expected and handled there
        uintptr_t o; const char* m = ModuleOf(r->ExceptionAddress, &o);
        char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH);
        const char* en = strrchr(exe, '\\'); en = en ? en + 1 : exe;
        if (_stricmp(m, "ddraw.dll") && _stricmp(m, en)) { LogOnce(_strdup(m), "note: exception %08lx inside %s (normally handled there)", code, m); return EXCEPTION_CONTINUE_SEARCH; }
    }
    count++;
    uintptr_t off; const char* mod = ModuleOf(r->ExceptionAddress, &off);
    Log("=== CRASH: exception %08lx at %p (%s+%lx)", r->ExceptionCode, r->ExceptionAddress, mod, (unsigned long)off);
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
        Log("    %s address %08lx", r->ExceptionInformation[0] ? "writing" : "reading", (unsigned long)r->ExceptionInformation[1]);
    Log("    eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx ebp=%08lx esp=%08lx", c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    uint32_t* sp = (uint32_t*)(uintptr_t)c->Esp;
    for (int i = 0; i < 48; i++) {
        if (!Readable(sp + i, 4)) break;
        uintptr_t o; const char* m = ModuleOf((void*)(uintptr_t)sp[i], &o);
        if (m[0] != '?') Log("    stack+%03x: %08lx %s+%lx", i * 4, (unsigned long)sp[i], m, (unsigned long)o);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        LogInit();
        ConfigLoad();
        AddVectoredExceptionHandler(0, CrashFilter);
        GameFixEarly();
    } else if (reason == DLL_PROCESS_DETACH) {
        Log("game closed");
    }
    return TRUE;
}

extern "C" {
__declspec(dllexport) HRESULT WINAPI DirectDrawCreate(GUID* guid, LPDIRECTDRAW* out, IUnknown*) {
    GameFixInstall();
    return FrontCreateDirectDraw(guid, (void**)out);
}
__declspec(dllexport) HRESULT WINAPI DirectDrawCreateEx(GUID*, void** out, REFIID, IUnknown*) {
    Log("DirectDrawCreateEx called (DirectX 7 interface) - not supported");
    if (out) *out = nullptr;
    return DDERR_UNSUPPORTED;
}
__declspec(dllexport) HRESULT WINAPI DirectDrawEnumerateA(LPDDENUMCALLBACKA cb, void* ctx) {
    if (cb) cb(nullptr, (char*)"Primary Display Driver", (char*)"display", ctx);
    return DD_OK;
}
__declspec(dllexport) HRESULT WINAPI DirectDrawEnumerateExA(LPDDENUMCALLBACKEXA cb, void* ctx, DWORD) {
    if (cb) cb(nullptr, (char*)"Primary Display Driver", (char*)"display", ctx, nullptr);
    return DD_OK;
}
__declspec(dllexport) HRESULT WINAPI DirectDrawCreateClipper(DWORD, LPDIRECTDRAWCLIPPER* out, IUnknown*) {
    Log("DirectDrawCreateClipper called - not supported");
    if (out) *out = nullptr;
    return DDERR_UNSUPPORTED;
}
}
