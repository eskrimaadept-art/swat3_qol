#pragma once
#include "common.h"

// Hooks for the QoL features (all optional)
struct HookCallbacks {
    bool (*freezeCursor)() = nullptr;                                  // a popup menu owns the mouse
    bool (*fakeKeyDown)(int vk) = nullptr;                            // pretend a key is held
    bool (*onMessage)(HWND, UINT, WPARAM, LPARAM, LRESULT*) = nullptr; // true = handled
};

void HooksInstall(HWND hwnd);              // when the game gives us its window
void HooksSetCallbacks(const HookCallbacks& cb);
HWND HooksWindow();
LRESULT HooksCallOriginal(HWND h, UINT m, WPARAM wp, LPARAM lp);
