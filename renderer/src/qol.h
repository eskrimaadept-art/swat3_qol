// Quality-of-life features built into the renderer: SWAT 4-style command menu with team
// selection, nametags above officers, quick restrain. Shared pieces live in qol.cpp.
#pragma once
#include "common.h"
#include "backend.h"

void QolInstall();                       // once, when the game hands us its window

// ---- settings ([Controls], [Nametags], [QuickRestrain]) -------------------------------------
struct QolConfig {
    int menuKey = 2, cycleKey = 5, elementKey = 0, redKey = 0, blueKey = 0;
    int openAtTeam = 1, contextReports = 1, dynamicStack = 1, menuSize = 70;
    int tagsOn = 1, tagsMaxDist = 1500, tagsTextSize = 0;
    int qcOn = 1, qcReach = 80;
};
extern QolConfig g_qol;

// ---- shared helpers ------------------------------------------------------------------------
int ParseKey(const char* s);                        // "RMOUSE", "F", "F5", "TAB", "70" ... ; 0 = none
const char* KeyName(int code, char* buf, int n);    // for prompts and the log
// Which key a window message is about: >0 key went down, <0 key came up (negated code), 0 = none.
// repeat is set for a held keyboard key's auto-repeat.
int KeyOfMessage(UINT m, WPARAM wp, LPARAM lp, bool* repeat);
void SpeechMute(int ms);                            // silence the game's speech for a moment (0 = stop)
bool HudSeenRecently();                             // in a mission, HUD drawn in the last half second
uint8_t* QolEngine();                               // the game's engine object (or nullptr)
uint8_t* QolRenderer();                             // the game's 3D view (camera, projection)

// Text drawn over the picture, at output resolution. Sprites are cached by text/colour/size.
// align: 0 = centre the text on x, bottom at y; 1 = centre on x and y
void OverlayText(const char* text, COLORREF color, int heightPx, int x, int y, int align);
void OverlayImage(const uint32_t* premulArgb, int w, int h, int x, int y);   // drawn this frame only
// Map game (virtual) pixels to output pixels
void VirtToOut(double vx, double vy, int* ox, int* oy);

// ---- command menu (ctxmenu.cpp) -----------------------------------------------------------
namespace ctx {
void Init();
void OnHudElement(void* elem);           // every HUD element the game draws
bool IsMenuElement(void* elem);
void FrameEnd(bool hudDrawn);            // at present
bool OnMessage(HWND h, UINT m, WPARAM wp, LPARAM lp);   // true = swallow
void OnMouseDelta(int dx, int dy);       // real mouse movement while the menu has the mouse
bool Active();                           // menu open (owns the mouse)
bool InGame();
}

// ---- nametags (nametags.cpp) --------------------------------------------------------------
namespace tags {
struct CharView {                        // one character in the mission
    uint8_t* c;                          // the game's character object
    char name[33];
    uint8_t kind, role, state; bool active;
    float eye;                           // eye height above the feet
    float head[3], chest[3];             // camera space (x right, y up, z forward)
    float dist;                          // camera to chest
};
struct Projection { float kx, cx, ky, cy; };
const Vec<CharView>* Chars(Projection* proj);   // this frame's characters (scanned once per frame)
void NewFrame();
void Draw();                             // names of officers in view
}

// ---- quick restrain (quickcuff.cpp) -------------------------------------------------------
namespace qc {
void Init();
void Update();                           // per frame: find who can be restrained, draw the prompt
bool OnMessage(HWND h, UINT m, WPARAM wp, LPARAM lp);
}
