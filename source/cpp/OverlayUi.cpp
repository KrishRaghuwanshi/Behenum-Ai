// language: C++, file: OverlayUi.cpp, target: Windows 11, MSVC
// State manager for the native Chimera overlay.
// *All hotkeys via GetAsyncKeyState (no RegisterHotKey atoms, no detectable hooks)
// *Hide/show toggle: Ctrl+Alt+H (same stealth approach)
// *Answer buffer with SecureZeroMemory on hide/uninstall
// *Wires UiInstall/Uninstall/SetVisible/AppendAnswer called from ChimeraMain

#include <windows.h>
#include <string>
#include <vector>

// ─── forward decls from OverlayWindow.cpp ────────────────────────────────────
void OverlayWindowInstall();
void OverlayWindowUninstall();
void OverlayWindowSetVisible(bool visible);
void OverlayWindowRefresh();
void OverlayWindowResetScroll(); // resets answer scroll to top + invalidates
void OverlayWindowResetPosition(); // resets size/pos to default + shows window
void OverlayWindowMinimizeToDot(); // shrinks to tiny dot in corner

// ─── forward decls from ChimeraMain.cpp (actions) ────────────────────────────
extern void UiActionSend();
extern void UiActionSendImagesOnly();
extern void UiActionCaptureScreen();
extern void UiActionCaptureRegion();
extern void UiActionChatFromClipboard();
extern void UiActionClear();
extern void UiActionSetApiKeyFromClipboard();

// ─── forward decls for answer-buffer helpers defined later in this file ───────
void UiClearAnswer();
void UiAppendAnswer(const char* delta, size_t len);
void UiToggleVisible(); // defined after UiSetVisible below

// ─── answer buffer (streamed from NvidiaStreamAnswer callback) ─────────────────
static char   g_answer[16384] = { "Q sends screenshots to AI.\nA sends images only.\nW/E capture screen/region.\nH hides overlay." };
static size_t g_answerLen     = 82;

// ─── visibility + hotkey state ────────────────────────────────────────────────
static bool g_uiVisible   = true;
static bool g_hotPrev[6]  = {};  // Q A W E R H  -- previous GetAsyncKeyState state
static bool g_hidePrev    = false;

// ─── action thread dispatch: prevents UI thread from blocking/crashing ─────────
// Called by OverlayWindow.cpp when a button is clicked; it just calls into
// this module so the actual dispatch lives here.
static DWORD WINAPI HotkeyActionThread(LPVOID p) {
    int act = (int)(INT_PTR)p;
    __try {
        switch (act) {
        case 0: UiActionSend();          break;
        case 1: UiActionSendImagesOnly();break;
        case 2: UiActionCaptureScreen(); break;
        case 3: UiActionCaptureRegion(); break;
        case 4: {
            // R = toggle hide/show -- preserves answer buffer
            UiToggleVisible();
            break;
        }
        case 5: {
            // H = toggle hide/show -- preserves answer buffer (same as R)
            UiToggleVisible();
            break;
        }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Swallow any SEH (DXGI / D3D / WinHTTP access fault) so the overlay
        // process stays alive -- previously an unprotected crash here would
        // propagate up and appear to "close" the overlay.
        UiClearAnswer();
        const char msg[] = "[Error] Hotkey action failed (DXGI or API unavailable "
                           "in this process context). Try a different host process "
                           "or run ChimeraBootstrap as Administrator.";
        UiAppendAnswer(msg, sizeof(msg) - 1);
        OverlayWindowRefresh();
    }
    return 0;
}

static void DispatchHotkeyAction(int idx) {
    HANDLE h = CreateThread(nullptr, 0, HotkeyActionThread, (LPVOID)(INT_PTR)idx, 0, nullptr);
    if (h) CloseHandle(h);
}

// ─── hotkey poll: called from OverlayWindow WM_TIMER (every 50 ms) ────────────
// Uses GetAsyncKeyState to detect Ctrl+Alt+Q/A/W/E/R/H without RegisterHotKey.
// No atom is created, no detectable hook is installed.
void UiPollHotkeys() {
    // ── Double middle-click toggle (works in HackerRank — no keyboard) ────────
    {
        static bool prevM = false;
        static DWORD lastM = 0;
        bool mBtn = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
        if (mBtn && !prevM) {
            DWORD now = GetTickCount();
            if (now - lastM < 500) {
                UiToggleVisible();
                lastM = 0;
            } else {
                lastM = now;
            }
        }
        prevM = mBtn;
    }

    // ── Ctrl+Alt keyboard combos ──────────────────────────────────────────────
    bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    bool alt  = (GetAsyncKeyState(VK_MENU)    & 0x8000) != 0;

    if (!ctrl || !alt) {
        memset(g_hotPrev, 0, sizeof(g_hotPrev));
        return;
    }

    // Q=Send, A=ImagesOnly, W=Whole, E=Region, R=Hide/Show, H=Hide/Show
    const int vks[6] = { 'Q', 'A', 'W', 'E', 'R', 'H' };
    for (int i = 0; i < 6; i++) {
        bool down = (GetAsyncKeyState(vks[i]) & 0x8000) != 0;
        if (down && !g_hotPrev[i]) {
            DispatchHotkeyAction(i);
        }
        g_hotPrev[i] = down;
    }
}

// ─── lifecycle ────────────────────────────────────────────────────────────────
void UiInstall() {
    OverlayWindowInstall();
}

void UiUninstall() {
    SecureZeroMemory(g_answer, sizeof(g_answer));
    g_answerLen = 0;
    OverlayWindowUninstall();
}

void UiSetVisible(bool visible) {
    g_uiVisible = visible;
    OverlayWindowSetVisible(visible);
    if (!visible) {
        SecureZeroMemory(g_answer, sizeof(g_answer));
        g_answerLen = 0;
    }
}

// UiToggleVisible: hides/shows without wiping the answer buffer.
// 200ms debounce prevents double-trigger from multiple detection paths.
void UiToggleVisible() {
    static DWORD lastToggle = 0;
    DWORD now = GetTickCount();
    if (now - lastToggle < 200) return;
    lastToggle = now;
    g_uiVisible = !g_uiVisible;
    if (g_uiVisible) {
        OverlayWindowResetPosition();
    } else {
        // Shrink to tiny dot instead of hiding — clicking dot expands back
        OverlayWindowMinimizeToDot();
    }
    // Deliberately does NOT SecureZeroMemory(g_answer) -- preserve AI response.
}

// ─── answer feed ─────────────────────────────────────────────────────────────
void UiAppendAnswer(const char* delta, size_t len) {
    if (!delta || !len) return;
    size_t room = sizeof(g_answer) - 1 - g_answerLen;
    size_t copy = (len < room) ? len : room;
    memcpy(g_answer + g_answerLen, delta, copy);
    g_answerLen += copy;
    g_answer[g_answerLen] = '\0';
    OverlayWindowRefresh();
}

void UiClearAnswer() {
    SecureZeroMemory(g_answer, sizeof(g_answer));
    g_answerLen = 0;
    OverlayWindowResetScroll();  // reset answer panel scroll to top on every clear
}

// ─── accessors for OverlayWindow.cpp renderer ─────────────────────────────────
const char* UiGetAnswer()    { return g_answer;    }
size_t      UiGetAnswerLen() { return g_answerLen;  }
bool        UiIsVisible()    { return g_uiVisible;  }

// ─── UiDrawState (legacy accessor, kept for link compatibility) ───────────────
struct UiButton { const char* id; const char* label; LONG x, y, w, h; };
struct UiDrawState {
    bool visible;
    const char* answer;
    size_t answerLen;
    const UiButton* buttons;
    int buttonCount;
};

UiDrawState UiGetDrawState() {
    UiDrawState s = {};
    s.visible   = g_uiVisible;
    s.answer    = g_answer;
    s.answerLen = g_answerLen;
    s.buttons   = nullptr;
    s.buttonCount = 0;
    return s;
}
