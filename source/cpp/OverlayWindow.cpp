// language: C++, file: OverlayWindow.cpp, target: Windows 11, MSVC
// Baihan-AI style native overlay -- dark/draggable/resizable, zero security reduction.
// *WDA_EXCLUDEFROMCAPTURE: never seen by OBS/proctoring/screen-share APIs
// *WS_EX_TOOLWINDOW: no taskbar button, no Alt-Tab entry
// *WS_EX_TOPMOST + SW_SHOWNOACTIVATE: always on top, no focus steal on launch
// *All action buttons dispatch to worker threads -- UI thread never blocks or crashes
// *Hotkeys polled via GetAsyncKeyState (no RegisterHotKey atoms, no detectable hooks)
// *Draggable (HTCAPTION on toolbar), resizable (HT* on border pixels)
// *Chat EDIT control: types message, Enter = send, Escape = clear

#include <windows.h>
#include <string>
#include <stdio.h>
#include <vector>
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

// ─── debug log helper — writes to overlay_debug.log in current directory ────────
#include <stdio.h>
static void DbgLog(const char* fmt, ...) {
    FILE* f = nullptr;
    fopen_s(&f, "overlay_debug.log", "a");
    if (!f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

// ─── colour palette (Baihan black) ─────────────────────────────────────────────
#define C_BG        RGB(10,10,12)
#define C_TOOLBAR   RGB(18,18,22)
#define C_PANEL_L   RGB(14,14,18)
#define C_PANEL_R   RGB(12,12,16)
#define C_DIVIDER   RGB(40,40,50)
#define C_BORDER    RGB(50,50,62)
#define C_SEPDIV    RGB(38,38,48)
#define C_BTN       RGB(35,35,45)
#define C_BTN_HOV   RGB(52,52,65)
#define C_BTN_SEND  RGB(35,35,45)
#define C_BTN_SENDH RGB(52,52,65)
#define C_BTN_API   RGB(35,35,45)
#define C_BTN_APIH  RGB(52,52,65)
#define C_BTN_HIDE  RGB(35,35,45)
#define C_BTN_HIDEH RGB(52,52,65)
#define C_TEXT      RGB(200,202,215)
#define C_DIM       RGB(95,97,112)
#define C_LABEL     RGB(130,132,148)
#define C_GOOD      RGB(50,185,90)
#define C_BAD       RGB(200,65,65)
#define C_INDBLINK  RGB(50,200,95)
#define C_CHAT_BG   RGB(8,8,10)
#define C_CHAT_BDR  RGB(45,47,60)
#define C_STATUS    RGB(12,12,16)
#define C_HL        RGB(25,25,35)     // row highlight in queue

// ─── layout ────────────────────────────────────────────────────────────────────
static constexpr int kToolH  = 44;          // toolbar
static constexpr int kChatH  = 0;           // chat bar REMOVED (typing triggers focus-loss detection)
static constexpr int kStatH  = 22;          // status bar at very bottom
static constexpr int kBorder = 5;           // resize-grip border thickness
static constexpr int kPad    = 8;
static constexpr int kBtnH   = 28;
static constexpr int kMinW   = 540;
static constexpr int kMinH   = 360;
static constexpr int kInitW  = 860;
static constexpr int kInitH  = 560;
static constexpr int kQueueItemH = 28;      // height of each queue row

// ─── window state ──────────────────────────────────────────────────────────────
static HWND   g_hwnd     = nullptr;
static HWND   g_hEdit    = nullptr;
static HANDLE g_uiThread = nullptr;
static DWORD  g_uiTid    = 0;
static bool   g_uiRunning = false;
static int    g_W = kInitW, g_H = kInitH;
static float  g_divRatio = 0.35f;
static int    g_hov = -1;
static bool   g_streaming = false;

// Unique class name per injection
static wchar_t g_wndClassName[48] = L"SysOverlayHelperClass";

// Whether our window is on SEB's non-Default desktop (for keyboard bypass + WDA skip).
static bool g_sebChildMode = false;

// Set to true ONLY by OverlayWindowUninstall() before posting WM_DESTROY.
// Blocks external WM_DESTROY sent by SEB/proctoring tools via SendMessage/PostMessage.
static bool g_intentionalDestroy = false;

// g_previewBmp and g_previewActive are reset by OverlayWindowMigrateToInputDesktop()
// which is called from ChimeraMain's DesktopMonitorThread on SwitchDesktop events.

// ─── button ids ────────────────────────────────────────────────────────────────
enum Btn { B_SEND=0, B_WHOLE, B_REGION, B_CHAT, B_CLEAR, B_HIDE, B_KEY, B_CLOSE, B_COUNT };
static RECT g_brc[B_COUNT];

// ─── forward decls wired from ChimeraMain / OverlayUi ──────────────────────────
void  UiActionSend();
void  UiActionSendImagesOnly();
void  UiActionCaptureScreen();
void  UiActionCaptureRegion();
void  UiActionClear();
void  UiActionSetApiKeyFromClipboard();
void  UiActionSendWithPrompt(const char* typedPrompt);
void  UiActionDeleteTrayItem(int index);
bool  UiGetTrayItemDimensions(int index, UINT* outW, UINT* outH);
HBITMAP UiGetTrayPreviewBitmap(int index, UINT* outW, UINT* outH);
void  UiPollHotkeys();
bool  UiKeyIsSet();
int   UiTrayCount();
bool  UiIsStreaming();
int   UiGetQueueTimestamps(DWORD* outTicks, int maxCount);
const char* UiGetAnswer();
size_t      UiGetAnswerLen();
bool        UiIsVisible();
void        UiSetVisible(bool visible);
void        UiToggleVisible();
void        UiClearAnswer();
void        UiAppendAnswer(const char* delta, size_t len);
void        UiClearStreaming();
void        UiActionCancel();

// ─── fonts ─────────────────────────────────────────────────────────────────────
static HFONT g_fNorm  = nullptr;
static HFONT g_fBold  = nullptr;
static HFONT g_fSmall = nullptr;
static HFONT g_fMono  = nullptr;  // Consolas for code blocks

// Copy targets: code blocks the user can click to copy
struct CopyTarget { RECT rc; std::string text; };
static std::vector<CopyTarget> g_copyTargets;

// Queue delete targets: × buttons next to each queued screenshot
struct QueueDeleteTarget { RECT rc; int index; };
static std::vector<QueueDeleteTarget> g_queueDeleteTargets;

// Image preview state: when user clicks a queue row, the image is decoded and shown fullscreen
static HBITMAP g_previewBmp  = nullptr;
static UINT    g_previewW    = 0;
static UINT    g_previewH    = 0;
static bool    g_previewActive = false;
static RECT    g_previewCloseRc = {};  // × close button rect

// Chat-focus state: true while EDIT has keyboard ownership
static bool g_chatFocused = false;

// Dot mode: true when overlay is minimized to a tiny dot
static bool g_isDot = false;

// Static brush for the chat EDIT background -- created once, avoids GDI leak
// from the old pattern of CreateSolidBrush on every WM_CTLCOLOREDIT call.
static HBRUSH g_hEditBrush = nullptr;

// Answer panel scroll offset in pixels (mousewheel-driven, reset by OverlayWindowResetScroll)
static int  g_answerScrollPx = 0;

// Foreground window before chat-bar activation -- restored when the user presses Enter/Escape.
static HWND g_prevForeground = nullptr;

static void EnsureFonts() {
    if (!g_fNorm)  g_fNorm  = CreateFontA(14, 0,0,0, FW_NORMAL,   0,0,0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
    if (!g_fBold)  g_fBold  = CreateFontA(14, 0,0,0, FW_SEMIBOLD, 0,0,0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
    if (!g_fSmall) g_fSmall = CreateFontA(12, 0,0,0, FW_NORMAL,   0,0,0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
}
static void EnsureMonoFont() {
    if (!g_fMono) g_fMono = CreateFontA(
        13,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,FIXED_PITCH,"Consolas");
}
static void DeleteFonts() {
    if (g_fNorm)     { DeleteObject(g_fNorm);     g_fNorm     = nullptr; }
    if (g_fBold)     { DeleteObject(g_fBold);     g_fBold     = nullptr; }
    if (g_fSmall)    { DeleteObject(g_fSmall);    g_fSmall    = nullptr; }
    if (g_fMono)     { DeleteObject(g_fMono);     g_fMono     = nullptr; }
    if (g_hEditBrush){ DeleteObject(g_hEditBrush);g_hEditBrush= nullptr; }
}
static void CopyTextToClipboard(const std::string& s) {
    if (!OpenClipboard(g_hwnd)) return;
    EmptyClipboard();
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, s.size()+1);
    if (hg) { memcpy(GlobalLock(hg), s.c_str(), s.size()+1); GlobalUnlock(hg); SetClipboardData(CF_TEXT, hg); }
    CloseClipboard();
}

// ─── helpers ──────────────────────────────────────────────────────────────────
static void FillRc(HDC dc, RECT r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c); FillRect(dc, &r, b); DeleteObject(b);
}
static void DrawRcBorder(HDC dc, RECT r, COLORREF c) {
    HPEN p = CreatePen(PS_SOLID, 1, c); HPEN op = (HPEN)SelectObject(dc, p);
    HBRUSH ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(p);
}
static void TxtLeft(HDC dc, const char* s, RECT r, COLORREF c, HFONT f = nullptr) {
    if (f) SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, c);
    DrawTextA(dc, s, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
}
static void TxtCenter(HDC dc, const char* s, RECT r, COLORREF c, HFONT f = nullptr) {
    if (f) SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, c);
    DrawTextA(dc, s, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
}
static void TxtWrap(HDC dc, const char* s, RECT r, COLORREF c, HFONT f = nullptr) {
    if (f) SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, c);
    DrawTextA(dc, s, -1, &r, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOCLIP);
}

// ─── layout ────────────────────────────────────────────────────────────────────
static int DivX()     { return kPad + (int)((g_W - kPad*2) * g_divRatio); }
static int ContentY() { return kToolH; }
static int ContentH() { return g_H - kToolH - kChatH - kStatH; }

static void LayoutBtns() {
    int y0 = (kToolH - kBtnH) / 2;
    // Left side: Send, Whole, Region, (Chat hidden), Clear
    // B_CHAT gets a zero-size rect so HitBtn never matches it
    struct { int w; } L[] = { {100},{82},{82},{0},{62} };
    int x = kPad;
    for (int i = 0; i <= B_CLEAR; i++) {
        if (L[i].w == 0) { g_brc[i] = {0,0,0,0}; continue; } // B_CHAT hidden
        g_brc[i] = { x, y0, x + L[i].w, y0 + kBtnH }; x += L[i].w + 5;
    }
    // Right side (right-to-left: HIDE then KEY)
    x = g_W - kPad;
    x -= 28; g_brc[B_HIDE]  = { x, y0, x + 28, y0 + kBtnH };
    x -= 5;
    x -= 78; g_brc[B_KEY]   = { x, y0, x + 78, y0 + kBtnH };
    // B_CLOSE at far-left of toolbar, separated from other buttons
    g_brc[B_CLOSE] = { g_W - kPad - 26, g_H - kStatH + 2, g_W - kPad, g_H - 2 };
}

static int HitBtn(int mx, int my) {
    // Check all buttons at their actual positions (toolbar + status bar)
    for (int i = 0; i < B_COUNT; i++)
        if (mx >= g_brc[i].left && mx < g_brc[i].right && my >= g_brc[i].top && my < g_brc[i].bottom) return i;
    return -1;
}

// ─── action thread dispatch ────────────────────────────────────────────────────
// __try/__except swallows DXGI/access-denied crashes so svchost does not die.
static char g_chatBuf[2048] = {};

static DWORD WINAPI ActionThread(LPVOID p) {
    // Switch this thread to the current input desktop before doing anything.
    // Required so region-capture's fullscreen selection window and any other
    // windows created here appear on SEB's desktop, not the Default desktop.
    {
        HDESK hDesk = OpenInputDesktop(0, FALSE,
            DESKTOP_CREATEWINDOW | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS);
        if (hDesk) { SetThreadDesktop(hDesk); CloseDesktop(hDesk); }
    }
    int act = (int)(INT_PTR)p;
    __try {
        switch (act) {
        case B_SEND:
            if (UiIsStreaming()) {
                UiActionCancel();
            } else {
                // Read typed text (populated by Enter key or by WM_LBUTTONDOWN before dispatch)
                char local[2048];
                memcpy(local, g_chatBuf, sizeof(local));
                memset(g_chatBuf, 0, sizeof(g_chatBuf));
                if (g_hEdit) SetWindowTextA(g_hEdit, "");
                UiActionSendWithPrompt(local[0] ? local : nullptr);
            }
            break;
        case B_WHOLE:  UiActionCaptureScreen();   break;
        case B_REGION: UiActionCaptureRegion();   break;
        case B_CHAT:   break; // dead — merged into B_SEND
        case B_CLEAR: UiActionClear();   break;
        case B_HIDE:  UiToggleVisible(); break; // shrinks to dot / expands back
        case B_KEY:   UiActionSetApiKeyFromClipboard(); break;
        case B_CLOSE:
            // Post WM_QUIT to the UI thread -- ends message loop, window destroyed,
            // DLL stays in memory but overlay disappears permanently until next Bootstrap run.
            if (g_uiTid) PostThreadMessageW(g_uiTid, WM_QUIT, 0, 0);
            break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        UiClearStreaming();  // prevent g_streaming stuck=true if WinHTTP threw
        UiClearAnswer();
        const char msg[] = "[Error] Action failed -- screenshot capture may be denied in this process context.\nTry running as Administrator or use a different host process.";
        UiAppendAnswer(msg, sizeof(msg)-1);
    }
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
    return 0;
}

static void DispatchAction(int btn) {
    HANDLE h = CreateThread(nullptr, 0, ActionThread, (LPVOID)(INT_PTR)btn, 0, nullptr);
    if (h) CloseHandle(h);
}

// ─── PAINT ─────────────────────────────────────────────────────────────────────
static void DrawButton(HDC dc, int i, const char* label, COLORREF base, COLORREF hover) {
    RECT r = g_brc[i];
    COLORREF c = (i == g_hov) ? hover : base;
    FillRc(dc, r, c);
    // Highlight top edge (subtle) on dark theme
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(
        min(255, GetRValue(c)+18), min(255, GetGValue(c)+18), min(255, GetBValue(c)+18)));
    HPEN op = (HPEN)SelectObject(dc, pen);
    MoveToEx(dc, r.left, r.top, nullptr); LineTo(dc, r.right, r.top);
    SelectObject(dc, op); DeleteObject(pen);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, C_TEXT);
    SelectObject(dc, g_fNorm);
    DrawTextA(dc, label, -1, &r, DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOCLIP);
}

// ─── Markdown answer renderer ─────────────────────────────────────────────────
// Parses triple-backtick code fences and renders ChatGPT-style:
//   regular text  = Segoe UI word-wrap
//   code block    = dark bg + Consolas + "Copy" button
// g_copyTargets rebuilt each frame; WM_LBUTTONDOWN checks it.
static void DrawAnswerMarkdown(HDC dc, RECT bounds, const char* text, size_t len, int scrollY) {
    g_copyTargets.clear();
    if (!text || !len) return;
    EnsureMonoFont();

    int x0 = bounds.left, x1 = bounds.right;
    int curY = bounds.top - scrollY;  // shift up by scroll offset: earlier content starts above top
    const int pad = 6;

    // Measure text height helper
    auto MeasH = [&](const char* s, int slen, int w, HFONT f) -> int {
        HFONT old = (HFONT)SelectObject(dc, f);
        RECT mr = {0, 0, w, 5000};
        DrawTextA(dc, s, slen, &mr, DT_LEFT|DT_TOP|DT_WORDBREAK|DT_CALCRECT);
        SelectObject(dc, old);
        return mr.bottom > mr.top ? (mr.bottom - mr.top) : 14;
    };

    const char* p = text, *end = text + len;
    while (p < end && curY < bounds.bottom) {
        // Find next code fence
        const char* fence = p;
        while (fence < end) {
            if (fence+2 < end && fence[0]=='`' && fence[1]=='`' && fence[2]=='`') break;
            fence++;
        }
        // Render plain text before fence
        if (fence > p) {
            std::string seg(p, fence);
            while (!seg.empty() && seg.back()=='\n') seg.pop_back();
            if (!seg.empty()) {
                int w = x1-x0-pad*2;
                int h = MeasH(seg.c_str(), -1, w, g_fNorm);
                RECT tr = {x0+pad, curY, x1-pad, curY+h};
                SelectObject(dc, g_fNorm);
                SetBkMode(dc, TRANSPARENT); SetTextColor(dc, C_TEXT);
                DrawTextA(dc, seg.c_str(), -1, &tr, DT_LEFT|DT_TOP|DT_WORDBREAK|DT_NOCLIP);
                curY += h + 4;
            }
        }
        if (fence >= end) break;

        // Skip opening ``` and capture optional language tag (e.g. "cpp", "python", "js")
        fence += 3;
        const char* langStart = fence;
        while (fence < end && *fence != '\n' && *fence != '\r') fence++;
        std::string langTag(langStart, fence);
        // trim trailing whitespace / CR from the tag
        while (!langTag.empty() && (langTag.back() == ' ' || langTag.back() == '\r' || langTag.back() == '\t'))
            langTag.pop_back();
        if (fence < end && *fence == '\r') fence++;
        if (fence < end && *fence == '\n') fence++;
        p = fence;

        // Find closing ```
        const char* close = p;
        while (close < end) {
            if (close+2 < end && close[0]=='`' && close[1]=='`' && close[2]=='`') break;
            close++;
        }
        std::string code(p, close < end ? close : end);
        if (!code.empty() && code.back()=='\n') code.pop_back();

        if (!code.empty()) {
            int w = x1-x0-pad*2;
            int h = MeasH(code.c_str(), -1, w, g_fMono);
            if (h < 16) h = 16;
            int blockH = h + pad*2 + 22;

            RECT bgRc = {x0+pad, curY, x1-pad, curY+blockH};
            FillRc(dc, bgRc, RGB(22,22,28));
            DrawRcBorder(dc, bgRc, RGB(55,58,75));

            // Header bar
            RECT hdrRc = {x0+pad, curY, x1-pad, curY+20};
            FillRc(dc, hdrRc, RGB(30,30,40));
            SelectObject(dc, g_fSmall); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, C_DIM);
            RECT lblRc = {x0+pad+6, curY, x0+pad+90, curY+20};
            DrawTextA(dc, langTag.empty() ? "code" : langTag.c_str(), -1, &lblRc, DT_LEFT|DT_VCENTER|DT_SINGLELINE);

            // Copy button
            RECT cpRc = {x1-pad-48, curY+2, x1-pad-2, curY+18};
            FillRc(dc, cpRc, RGB(55,55,70));
            SetTextColor(dc, C_TEXT);
            DrawTextA(dc, "Copy", -1, &cpRc, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            g_copyTargets.push_back({cpRc, code});

            // Code body
            RECT codeRc = {x0+pad+5, curY+22, x1-pad-5, curY+blockH-pad};
            SelectObject(dc, g_fMono); SetTextColor(dc, RGB(190,205,225));
            DrawTextA(dc, code.c_str(), -1, &codeRc, DT_LEFT|DT_TOP|DT_WORDBREAK|DT_NOCLIP);
            curY += blockH + 6;
        }
        // Skip closing fence
        if (close < end) {
            close += 3;
            while (close < end && *close != '\n') close++;
            if (close < end) close++;
        }
        p = (close < end) ? close : end;
    }
}

static void PaintOverlay(HDC hdc) {
    // If in dot mode, just draw a small colored circle
    if (g_isDot) {
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, g_W, g_H);
        HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
        // Dark dot with green accent
        RECT full = {0, 0, g_W, g_H};
        HBRUSH bg = CreateSolidBrush(RGB(30, 30, 36));
        FillRect(mem, &full, bg);
        DeleteObject(bg);
        // Green inner circle
        HBRUSH gr = CreateSolidBrush(RGB(50, 185, 90));
        HBRUSH oldBr = (HBRUSH)SelectObject(mem, gr);
        Ellipse(mem, 4, 4, g_W - 4, g_H - 4);
        SelectObject(mem, oldBr);
        DeleteObject(gr);
        BitBlt(hdc, 0, 0, g_W, g_H, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        return;
    }

    HDC   mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, g_W, g_H);
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
    EnsureFonts();

    // ── full background ──
    RECT full = {0,0,g_W,g_H};
    FillRc(mem, full, C_BG);

    // ── toolbar ──
    RECT tb = {0,0,g_W,kToolH};
    FillRc(mem, tb, C_TOOLBAR);
    // Thin bottom border on toolbar
    HPEN sep = CreatePen(PS_SOLID,1,C_BORDER);
    HPEN opsep = (HPEN)SelectObject(mem, sep);
    MoveToEx(mem,0,kToolH-1,nullptr); LineTo(mem,g_W,kToolH-1);
    SelectObject(mem,opsep); DeleteObject(sep);

    // Buttons
    SelectObject(mem, g_fNorm);
    bool nowStreaming = UiIsStreaming();
    DrawButton(mem, B_SEND,
        nowStreaming ? "Stop" : "Send",
        C_BTN, C_BTN_HOV);
    DrawButton(mem, B_WHOLE, "Whole  W",       C_BTN,       C_BTN_HOV);
    DrawButton(mem, B_REGION,"Region  E",      C_BTN,       C_BTN_HOV);
    // B_CHAT removed from UI — merged into Send
    DrawButton(mem, B_CLEAR, "Clear",          C_BTN,       C_BTN_HOV);
    DrawButton(mem, B_HIDE,  "\x96",           C_BTN,       C_BTN_HOV);
    DrawButton(mem, B_KEY,   "Set Key",        C_BTN,       C_BTN_HOV);

    // Streaming indicator (● blinking green dot next to Send)
    if (UiIsStreaming()) {
        DWORD t = GetTickCount();
        if ((t / 500) % 2 == 0) {
            HBRUSH br = CreateSolidBrush(C_GOOD);
            RECT ir = {kPad - 1, kToolH/2 - 4, kPad + 7, kToolH/2 + 5};
            // Shift to left of send button
            int lx = g_brc[B_SEND].left - 11;
            ir = {lx, kToolH/2-4, lx+8, kToolH/2+4};
            Ellipse(mem, ir.left, ir.top, ir.right, ir.bottom);
            DeleteObject(br);
        }
    }

    // ── content area ──
    int cy  = ContentY();
    int ch  = ContentH();
    int dvx = DivX();

    // Left panel: IMAGE QUEUE
    RECT lp = {0, cy, dvx, cy + ch};
    FillRc(mem, lp, C_PANEL_L);

    // Queue header
    RECT qhdr = {kPad, cy+6, dvx-kPad, cy+22};
    SelectObject(mem, g_fBold);
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, C_LABEL);
    DrawTextA(mem, "IMAGE QUEUE", -1, &qhdr, DT_LEFT|DT_VCENTER|DT_SINGLELINE);

    // Tray count
    int tray = UiTrayCount();
    char cntBuf[32]; sprintf_s(cntBuf, "%d / 16", tray);
    RECT cntr = {dvx - 48, cy+6, dvx - kPad, cy+22};
    SelectObject(mem, g_fSmall);
    SetTextColor(mem, tray > 0 ? C_GOOD : C_DIM);
    DrawTextA(mem, cntBuf, -1, &cntr, DT_RIGHT|DT_VCENTER|DT_SINGLELINE);

    // Divider under header
    {HPEN sp = CreatePen(PS_SOLID,1,C_DIVIDER); HPEN op2=(HPEN)SelectObject(mem,sp);
     MoveToEx(mem,0,cy+26,nullptr); LineTo(mem,dvx,cy+26);
     SelectObject(mem,op2); DeleteObject(sp);}

    // Queue items
    DWORD ticks[16] = {};
    int qCount = UiGetQueueTimestamps(ticks, 16);
    DWORD now = GetTickCount();
    int qy = cy + 30;
    g_queueDeleteTargets.clear();
    SelectObject(mem, g_fSmall);
    for (int i = 0; i < qCount && qy + kQueueItemH <= cy + ch; i++) {
        // Alternating row highlight
        RECT row = {1, qy, dvx-1, qy+kQueueItemH};
        if (i % 2 == 0) FillRc(mem, row, C_HL);

        // ● dot
        HBRUSH gb = CreateSolidBrush(C_GOOD);
        Ellipse(mem, kPad, qy+10, kPad+7, qy+17);
        DeleteObject(gb);

        // Label: "Shot N  WxH  (age)"
        char shotLabel[80];
        UINT iw = 0, ih = 0;
        UiGetTrayItemDimensions(i, &iw, &ih);
        DWORD ageSec = (now - ticks[i]) / 1000;
        if (ageSec < 60) sprintf_s(shotLabel, "Shot %d  %ux%u  (%lus)", i+1, iw, ih, (unsigned long)ageSec);
        else             sprintf_s(shotLabel, "Shot %d  %ux%u  (%lum)", i+1, iw, ih, (unsigned long)(ageSec/60));
        RECT lr = {kPad+10, qy, dvx-kPad-20, qy+kQueueItemH};
        SetTextColor(mem, C_TEXT);
        DrawTextA(mem, shotLabel, -1, &lr, DT_LEFT|DT_VCENTER|DT_SINGLELINE);

        // Delete × button (right side of row)
        RECT delRc = {dvx-22, qy+4, dvx-6, qy+kQueueItemH-4};
        FillRc(mem, delRc, RGB(210,70,70));
        SetTextColor(mem, C_TEXT);
        DrawTextA(mem, "x", -1, &delRc, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        g_queueDeleteTargets.push_back({delRc, i});

        // Separator
        {HPEN sp = CreatePen(PS_SOLID,1,C_SEPDIV); HPEN op2=(HPEN)SelectObject(mem,sp);
         MoveToEx(mem,1,qy+kQueueItemH-1,nullptr); LineTo(mem,dvx-1,qy+kQueueItemH-1);
         SelectObject(mem,op2); DeleteObject(sp);}
        qy += kQueueItemH;
    }

    if (qCount == 0) {
        RECT empty = {kPad, cy+35, dvx-kPad, cy+ch-10};
        SelectObject(mem, g_fSmall);
        SetTextColor(mem, C_DIM);
        DrawTextA(mem, "No screenshots queued.\n\nPress W to capture\nwhole screen, or E\nfor a region.", -1, &empty, DT_LEFT|DT_TOP|DT_WORDBREAK);
    }

    // Vertical divider
    {HPEN dv = CreatePen(PS_SOLID,1,C_DIVIDER); HPEN op2=(HPEN)SelectObject(mem,dv);
     MoveToEx(mem,dvx,cy,nullptr); LineTo(mem,dvx,cy+ch);
     SelectObject(mem,op2); DeleteObject(dv);}

    // Right panel: ANSWER
    RECT rp = {dvx+1, cy, g_W, cy+ch};
    FillRc(mem, rp, C_PANEL_R);

    // Answer header
    RECT ahdr = {dvx+kPad, cy+6, g_W-kPad, cy+22};
    SelectObject(mem, g_fBold);
    SetTextColor(mem, C_LABEL);
    DrawTextA(mem, "ANSWER", -1, &ahdr, DT_LEFT|DT_VCENTER|DT_SINGLELINE);

    // Streaming badge
    if (UiIsStreaming()) {
        RECT badge = {dvx+kPad+60, cy+8, dvx+kPad+60+45, cy+20};
        FillRc(mem, badge, C_BTN_SEND);
        SelectObject(mem, g_fSmall);
        SetTextColor(mem, C_TEXT);
        DrawTextA(mem, "  live", -1, &badge, DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    } else {
        RECT badge = {dvx+kPad+60, cy+8, dvx+kPad+60+38, cy+20};
        FillRc(mem, badge, C_BTN);
        SelectObject(mem, g_fSmall);
        SetTextColor(mem, C_DIM);
        DrawTextA(mem, " idle", -1, &badge, DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    }

    // Divider under header
    {HPEN sp = CreatePen(PS_SOLID,1,C_DIVIDER); HPEN op2=(HPEN)SelectObject(mem,sp);
     MoveToEx(mem,dvx+1,cy+26,nullptr); LineTo(mem,g_W,cy+26);
     SelectObject(mem,op2); DeleteObject(sp);}

    // Answer panel -- markdown renderer (code blocks + copy targets)
    RECT atxt = {dvx+kPad, cy+30, g_W-kPad, cy+ch-4};
    const char* ans = UiGetAnswer();
    size_t ansLen   = UiGetAnswerLen();
    if (ansLen > 0) {
        DrawAnswerMarkdown(mem, atxt, ans, ansLen, g_answerScrollPx);
    } else {
        SelectObject(mem, g_fNorm); SetBkMode(mem, TRANSPARENT); SetTextColor(mem, C_DIM);
        DrawTextA(mem, "Waiting for a response...\n\nType in the chat bar and press Enter,\nor use shortcut keys to send screenshots.", -1, &atxt, DT_LEFT|DT_TOP|DT_WORDBREAK);
    }

    // ── chat bar REMOVED (kChatH=0) ──

    // ── status bar ──
    int statY = cy + ch;
    RECT statBg = {0, statY, g_W, statY+kStatH};
    FillRc(mem, statBg, C_STATUS);
    {HPEN sp = CreatePen(PS_SOLID,1,C_DIVIDER); HPEN op2=(HPEN)SelectObject(mem,sp);
     MoveToEx(mem,0,statY,nullptr); LineTo(mem,g_W,statY);
     SelectObject(mem,op2); DeleteObject(sp);}

    // Status text
    bool keySet = UiKeyIsSet();
    RECT st = {kPad, statY, g_W-kPad, statY+kStatH};
    SelectObject(mem, g_fSmall);
    // Key indicator ●
    SetTextColor(mem, keySet ? C_GOOD : C_BAD);
    DrawTextA(mem, keySet ? "\x95 Key: SET" : "\x95 Key: NOT SET", -1, &st, DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    // Shortcut hints (right side)
    RECT hint = {g_W/3, statY, g_W-kPad, statY+kStatH};
    SetTextColor(mem, C_DIM);
    DrawTextA(mem, "Q=Send  W=Whole  E=Region  H=Hide  |  No auto-trigger", -1, &hint, DT_RIGHT|DT_VCENTER|DT_SINGLELINE);

    // Close button (×) in bottom-right of status bar
    DrawButton(mem, B_CLOSE, "\xD7", C_BTN, C_BTN_HOV);

    // ── image preview overlay (drawn on top of everything) ──
    if (g_previewActive && g_previewBmp) {
        // Dark background over content area
        RECT pvBg = {0, kToolH, g_W, g_H - kChatH - kStatH + kToolH};
        FillRc(mem, pvBg, RGB(18,18,22));

        // Scale image to fit content area with padding
        int areaW = g_W - kPad*2, areaH = ContentH() - 30;
        float scaleX = (float)areaW / g_previewW;
        float scaleY = (float)areaH / g_previewH;
        float scale = (scaleX < scaleY) ? scaleX : scaleY;
        if (scale > 1.0f) scale = 1.0f; // don't upscale
        int dw = (int)(g_previewW * scale), dh = (int)(g_previewH * scale);
        int dx = kPad + (areaW - dw) / 2, dy = kToolH + 26 + (areaH - dh) / 2;

        HDC bmpDC = CreateCompatibleDC(mem);
        HBITMAP obmp = (HBITMAP)SelectObject(bmpDC, g_previewBmp);
        SetStretchBltMode(mem, HALFTONE);
        StretchBlt(mem, dx, dy, dw, dh, bmpDC, 0, 0, g_previewW, g_previewH, SRCCOPY);
        SelectObject(bmpDC, obmp);
        DeleteDC(bmpDC);

        // Close × button (top-right of content area)
        g_previewCloseRc = {g_W - 36, kToolH + 4, g_W - 8, kToolH + 24};
        FillRc(mem, g_previewCloseRc, RGB(200,60,60));
        SelectObject(mem, g_fNorm); SetBkMode(mem, TRANSPARENT); SetTextColor(mem, C_TEXT);
        DrawTextA(mem, "X", -1, &g_previewCloseRc, DT_CENTER|DT_VCENTER|DT_SINGLELINE);

        // Dimensions label
        char dimLabel[48]; sprintf_s(dimLabel, "%ux%u", g_previewW, g_previewH);
        RECT dimRc = {kPad, kToolH+4, g_W-40, kToolH+24};
        SelectObject(mem, g_fSmall); SetTextColor(mem, C_DIM);
        DrawTextA(mem, dimLabel, -1, &dimRc, DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    }

    // Blit to screen
    BitBlt(hdc, 0, 0, g_W, g_H, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

// ─── EDIT subclass: Enter sends, Escape clears, restores NOACTIVATE ────────────
static WNDPROC g_origEditProc = nullptr;

static void ReleaseChatFocus() {
    if (!g_chatFocused) return;
    g_chatFocused    = false;
    g_prevForeground = nullptr;
    // Restore WS_EX_NOACTIVATE -- overlay must not passively hold keyboard focus.
    LONG_PTR ex = GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE);
    SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, ex | WS_EX_NOACTIVATE);
    // NOTE: NO SetForegroundWindow / SetFocus here.
    // Calling SetForegroundWindow from inside EditSubclassProc's WM_KEYDOWN handler
    // (which runs on the UI thread already processing a message) causes synchronous
    // WM_ACTIVATE + WM_KILLFOCUS delivery BACK to the overlay, creating re-entrant
    // message processing that destabilises the pump and makes the overlay appear to
    // close when the user presses Enter to send. The game/exam regains focus naturally
    // when the user clicks on it.
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
}

static LRESULT CALLBACK EditSubclassProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN) {
        if (wp == VK_RETURN) {
            int len = GetWindowTextLengthA(hw);
            if (len > 0 && len < (int)sizeof(g_chatBuf)-1)
                GetWindowTextA(hw, g_chatBuf, sizeof(g_chatBuf));
            else g_chatBuf[0] = '\0';
            SetWindowTextA(hw, "");
            DispatchAction(B_SEND);  // unified Send — reads g_chatBuf
            ReleaseChatFocus();
            return 0;
        }
        if (wp == VK_ESCAPE) {
            SetWindowTextA(hw, ""); g_chatBuf[0] = '\0';
            ReleaseChatFocus();
            return 0;
        }
    }
    if (msg == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
    return CallWindowProcA(g_origEditProc, hw, msg, wp, lp);
}

// ─── WM_NCHITTEST: drag + resize (no title bar) ────────────────────────────────
static LRESULT NCHitTest(HWND hwnd, int mx, int my) {
    RECT rc; GetWindowRect(hwnd, &rc);
    int x = mx - rc.left, y = my - rc.top;
    bool l = x < kBorder, r = x >= g_W-kBorder;
    bool t = y < kBorder, b = y >= g_H-kBorder;
    if (t && l) return HTTOPLEFT;
    if (t && r) return HTTOPRIGHT;
    if (b && l) return HTBOTTOMLEFT;
    if (b && r) return HTBOTTOMRIGHT;
    if (t)      return HTTOP;
    if (b)      return HTBOTTOM;
    if (l)      return HTLEFT;
    if (r)      return HTRIGHT;
    // Toolbar area = drag handle (but not on a button)
    if (y < kToolH && HitBtn(x, y) < 0) return HTCAPTION;
    return HTCLIENT;
}

// ─── window procedure ──────────────────────────────────────────────────────────
static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {

    case WM_NCCALCSIZE:
        // Remove non-client area entirely (title bar, resize border visual).
        // Resize still works via WM_NCHITTEST which we handle ourselves.
        // This eliminates the white top border drawn by WS_THICKFRAME.
        if (wp) return 0;
        return DefWindowProcW(hwnd, msg, wp, lp);

    case WM_NCHITTEST:
        return NCHitTest(hwnd, (short)LOWORD(lp), (short)HIWORD(lp));

    case WM_MOUSEACTIVATE:
        // Never activate the overlay -- keeps exam/game focus intact.
        return MA_NOACTIVATE;

    case WM_SETFOCUS:
        return 0;

    case WM_LBUTTONDOWN: {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);

        // If in dot mode, clicking the dot expands the overlay back
        if (g_isDot) {
            UiToggleVisible();
            return 0;
        }

        // 0. Preview close button?
        if (g_previewActive) {
            if (x >= g_previewCloseRc.left && x < g_previewCloseRc.right &&
                y >= g_previewCloseRc.top  && y < g_previewCloseRc.bottom) {
                // Close preview
                g_previewActive = false;
                if (g_previewBmp) { DeleteObject(g_previewBmp); g_previewBmp = nullptr; }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            // Any click outside close button also closes preview
            g_previewActive = false;
            if (g_previewBmp) { DeleteObject(g_previewBmp); g_previewBmp = nullptr; }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        // 1. Queue delete × button?
        for (auto& dt : g_queueDeleteTargets) {
            if (x >= dt.rc.left && x < dt.rc.right &&
                y >= dt.rc.top  && y < dt.rc.bottom) {
                UiActionDeleteTrayItem(dt.index);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
        }

        // 1b. Click on queue ROW (not ×) → open image preview
        {
            int dvx = DivX(), cy = ContentY();
            int qy = cy + 30;
            int qCount = UiTrayCount();
            if (x >= 0 && x < dvx && y >= qy && y < qy + qCount * kQueueItemH) {
                int idx = (y - qy) / kQueueItemH;
                if (idx >= 0 && idx < qCount) {
                    UINT pw = 0, ph = 0;
                    HBITMAP bmp = UiGetTrayPreviewBitmap(idx, &pw, &ph);
                    if (bmp) {
                        if (g_previewBmp) DeleteObject(g_previewBmp);
                        g_previewBmp = bmp; g_previewW = pw; g_previewH = ph;
                        g_previewActive = true;
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                    return 0;
                }
            }
        }

        // 2. Code-block Copy button? Only hit-test inside the visible answer panel.
        // g_copyTargets rects are in paint coords (include scroll offset already).
        // Click y is in window coords. Adjust by adding scroll offset back.
        {
            int dvx = DivX();
            int cy  = ContentY(), ch = ContentH();
            if (x > dvx && y > cy + 26 && y < cy + ch) {
                // Adjust y by scroll offset to match stored rect coordinates
                int adjustedY = y - g_answerScrollPx;
                for (auto& ct : g_copyTargets) {
                    if (x >= ct.rc.left && x < ct.rc.right &&
                        adjustedY >= ct.rc.top  && adjustedY < ct.rc.bottom) {
                        CopyTextToClipboard(ct.text);
                        return 0;
                    }
                }
            }
        }

        // 3. Toolbar button?
        int btn = HitBtn(x, y);
        if (btn >= 0) {
            // For Send button: grab EDIT text into g_chatBuf before dispatch
            if (btn == B_SEND && !UiIsStreaming() && g_hEdit) {
                int len = GetWindowTextLengthA(g_hEdit);
                if (len > 0 && len < (int)sizeof(g_chatBuf)-1)
                    GetWindowTextA(g_hEdit, g_chatBuf, sizeof(g_chatBuf));
                else g_chatBuf[0] = '\0';
            }
            DispatchAction(btn);
            return 0;
        }

        return 0;
    }

    case WM_MOUSEMOVE: {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        int newHov = HitBtn(x, y);
        if (newHov != g_hov) { g_hov = newHov; InvalidateRect(hwnd, nullptr, FALSE); }
        // Track mouse leave so hover clears
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_hov = -1; InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_SIZE:
        g_W = LOWORD(lp); g_H = HIWORD(lp);
        // Clamp: ensure ContentH() is never 0 or negative
        { int minH = kToolH + kChatH + kStatH + 40; if (g_H < minH) g_H = minH; }
        LayoutBtns();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm = (MINMAXINFO*)lp;
        mm->ptMinTrackSize = { kMinW, kMinH };
        return 0;
    }

    case WM_ERASEBKGND:
        return 1; // handled in WM_PAINT (double-buffer)

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        // __try/__except: any rendering fault (bad pointer from data race, GDI OOM, etc.)
        // is swallowed here so the message loop -- and the overlay -- stay alive.
        __try { PaintOverlay(hdc); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_TIMER:
        if (wp == 1) {
            // NOTE: Hotkeys are polled by HotkeyPollingThread in ChimeraMain.cpp (15ms).
            // UiPollHotkeys() removed from here to prevent double-toggle race:
            // both systems detecting the same keypress and cancelling each other out.
            // UPDATE: Re-added because GetAsyncKeyState from background threads
            // doesn't work in HackerRank. The UI thread (this timer) DOES work
            // because button clicks work. Debounce in UiToggleVisible prevents
            // double-toggle with the background polling thread.
            UiPollHotkeys();

            // ── Z-order: ALWAYS enforce topmost regardless of hide state ─────────
            // Even when user hid overlay (UiIsVisible()=false), Z-order must be
            // maintained so the window stays above SEB's browser after migration.
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);

            // Remaining enforcement only when user wants overlay visible.
            if (UiIsVisible()) {

            // WDA_EXCLUDEFROMCAPTURE — re-enforce on ALL desktops.
            // Proctoring tools or DWM resets can strip this flag.
            SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);

            // ── Visibility enforcement (re-show if SEB externally hid us) ────────
            // ONLY re-show if UiIsVisible() is true (user hasn't pressed Ctrl+Alt+H).
            // This prevents fighting with the user's intentional hide action.
            {
                if (!IsWindowVisible(hwnd)) {
                    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
                }
            }

            InvalidateRect(hwnd, nullptr, FALSE);

            } // end UiIsVisible() guard
        }
        return 0;

    case WM_CTLCOLOREDIT:
        // Use a pre-created static brush -- NOT CreateSolidBrush every call.
        // The old pattern leaked one GDI handle per EDIT repaint, exhausting
        // svchost's GDI table in ~100 calls and causing the overlay to go all white.
        if (!g_hEditBrush) g_hEditBrush = CreateSolidBrush(C_CHAT_BG);
        SetBkColor((HDC)wp, C_CHAT_BG);
        SetTextColor((HDC)wp, C_TEXT);
        return g_hEditBrush ? (LRESULT)g_hEditBrush : DefWindowProcW(hwnd, msg, wp, lp);

    case WM_MOVE: {
        // Prevent the toolbar from being dragged completely off-screen.
        // At least 44px (kToolH) of the top edge must stay visible.
        RECT wr; GetWindowRect(hwnd, &wr);
        int sx = GetSystemMetrics(SM_XVIRTUALSCREEN);
        int sy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int sw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        int sh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        int nx = wr.left, ny = wr.top;
        bool fix = false;
        if (wr.right  < sx + 40)       { nx = sx + 40  - (wr.right - wr.left);  fix = true; }
        if (wr.bottom < sy + kToolH)   { ny = sy + kToolH - (wr.bottom - wr.top); fix = true; }
        if (wr.left   > sx + sw - 40)  { nx = sx + sw - 40; fix = true; }
        if (wr.top    > sy + sh - kToolH) { ny = sy + sh - kToolH; fix = true; }
        if (fix) SetWindowPos(hwnd, nullptr, nx, ny, 0, 0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_WINDOWPOSCHANGING: {
        // Block any external attempt to change our Z-order away from topmost.
        // SEB calls SetWindowPos(hwnd, HWND_NOTOPMOST/HWND_BOTTOM) to push
        // overlay windows behind its fullscreen view.
        WINDOWPOS* pos = (WINDOWPOS*)lp;
        if (!(pos->flags & SWP_NOZORDER)) {
            // Something is requesting a Z-order change -- force topmost.
            pos->hwndInsertAfter = HWND_TOPMOST;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    case WM_STYLECHANGING: {
        // Prevent external code (SEB/proctoring) from stripping our critical
        // extended styles -- specifically WS_EX_TOPMOST (Z-order) and
        // WS_EX_TOOLWINDOW (no taskbar entry). Allow WS_EX_NOACTIVATE to
        // change freely so our chat-focus mechanism still works.
        if (wp == GWL_EXSTYLE) {
            STYLESTRUCT* ss = (STYLESTRUCT*)lp;
            ss->styleNew |= WS_EX_TOPMOST;     // must never be removed
            ss->styleNew |= WS_EX_TOOLWINDOW;  // no taskbar / no Alt-Tab
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    case WM_CLOSE:
        // Block ALL close requests. Only OverlayWindowUninstall() may destroy this window.
        // Without this, DefWindowProcW destroys the window -- DXGI desktop-duplication driver
        // messages and stray PostMessage(WM_CLOSE) from the host process can trigger this path.
        return 0;

    case WM_SYSCOMMAND:
        // Block Alt+F4 (SC_CLOSE) and any system minimize action (SC_MINIMIZE).
        // The overlay must stay visible and topmost at all times.
        if ((wp & 0xFFF0) == SC_CLOSE || (wp & 0xFFF0) == SC_MINIMIZE)
            return 0;
        break; // pass everything else (SC_MOVE, SC_SIZE) to DefWindowProcW

    // ── Raw Input keyboard handler ────────────────────────────────────────────
    // RIDEV_INPUTSINK delivers ALL keyboard events even without focus.
    // Completely independent of RegisterHotKey — works inside HackerRank.
    case WM_INPUT: {
        static bool riCtrl = false, riAlt = false, riShift = false;
        UINT size = 0;
        GetRawInputData((HRAWINPUT)lp, RID_INPUT, NULL, &size, sizeof(RAWINPUTHEADER));
        if (size <= 256) {
            BYTE buf[256];
            if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) > 0) {
                RAWINPUT* raw = (RAWINPUT*)buf;
                if (raw->header.dwType == RIM_TYPEKEYBOARD) {
                    USHORT vk = raw->data.keyboard.VKey;
                    bool up = (raw->data.keyboard.Flags & 1) != 0; // RI_KEY_BREAK

                    // Track modifiers
                    if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL)
                        riCtrl = !up;
                    if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU)
                        riAlt = !up;
                    if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT)
                        riShift = !up;

                    // Detect combos on key DOWN
                    if (!up) {
                        // Ctrl+Alt combos
                        if (riCtrl && riAlt) {
                            if (vk == 'H' || vk == 'R') UiToggleVisible();
                            if (vk == 'W') { HANDLE h = CreateThread(nullptr,0,[](LPVOID)->DWORD{ UiActionCaptureScreen(); return 0; },nullptr,0,nullptr); if(h) CloseHandle(h); }
                            if (vk == 'Q') { HANDLE h = CreateThread(nullptr,0,[](LPVOID)->DWORD{ UiActionSend(); return 0; },nullptr,0,nullptr); if(h) CloseHandle(h); }
                            if (vk == 'E') { HANDLE h = CreateThread(nullptr,0,[](LPVOID)->DWORD{ UiActionCaptureRegion(); return 0; },nullptr,0,nullptr); if(h) CloseHandle(h); }
                            if (vk == 'A') { HANDLE h = CreateThread(nullptr,0,[](LPVOID)->DWORD{ UiActionSendImagesOnly(); return 0; },nullptr,0,nullptr); if(h) CloseHandle(h); }
                        }
                        // Ctrl+Shift combos (backup)
                        if (riCtrl && riShift && !riAlt) {
                            if (vk == 'H') UiToggleVisible();
                            if (vk == 'W') { HANDLE h = CreateThread(nullptr,0,[](LPVOID)->DWORD{ UiActionCaptureScreen(); return 0; },nullptr,0,nullptr); if(h) CloseHandle(h); }
                            if (vk == 'Q') { HANDLE h = CreateThread(nullptr,0,[](LPVOID)->DWORD{ UiActionSend(); return 0; },nullptr,0,nullptr); if(h) CloseHandle(h); }
                        }
                    }
                }
            }
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    case WM_MOUSEWHEEL: {
        // Windows 10/11 delivers WM_MOUSEWHEEL to the window under the cursor
        // even if it has WS_EX_NOACTIVATE, so this reliably scrolls the answer panel.
        int delta = (short)HIWORD(wp);      // positive = scroll up, negative = scroll down
        g_answerScrollPx -= (delta / 5);    // 120 units per notch -> 24 px
        if (g_answerScrollPx < 0) g_answerScrollPx = 0;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_DESTROY:
        // Block WM_DESTROY unless it comes from OverlayWindowUninstall().
        // unknown windows. Without this check our message loop would obey and
        // the overlay would disappear on every SEB session start.
        if (!g_intentionalDestroy) return 0;
        KillTimer(hwnd, 1);
        DeleteFonts();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ─── find SEB's browser window (for child-window stealth mode) ─────────────────
// When SEB uses CreateNewDesktop kiosk mode, EnumWindows only returns top-level
// windows. A WS_CHILD window is NEVER returned by EnumWindows, so SEB's
// ApplicationMonitor never sees it, logs it, or hides it.
// This function finds SEB's largest visible window (the browser) to use as parent.
static HWND FindSebBrowserWindow() {    struct FindData { HWND result; DWORD bestArea; };
    FindData fd = { nullptr, 0 };
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        if (!IsWindowVisible(hwnd)) return TRUE;
        auto* fd = (FindData*)lp;
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!hp) return TRUE;
        wchar_t path[MAX_PATH] = {}; DWORD len = MAX_PATH;
        QueryFullProcessImageNameW(hp, 0, path, &len);
        CloseHandle(hp);
        if (!wcsstr(path, L"SafeExamBrowser")) return TRUE;
        RECT rc; GetWindowRect(hwnd, &rc);
        DWORD area = (DWORD)((rc.right - rc.left) * (rc.bottom - rc.top));
        if (area > fd->bestArea) { fd->bestArea = area; fd->result = hwnd; }
        return TRUE;
    }, (LPARAM)&fd);
    return fd.result;
}

// ─── UI message pump thread ────────────────────────────────────────────────────
static DWORD WINAPI UiMessagePump(LPVOID) {
    // ── DESKTOP FOLLOWING ────────────────────────────────────────────────────
    // SEB uses CreateNewDesktop kiosk mode: it calls CreateDesktop() to create
    // a separate Windows desktop object, then SwitchDesktop() to make it the
    // active display surface.  Windows on the original Default desktop become
    // invisible (not destroyed -- they are still alive on an off-screen desktop).
    //
    // Fix: before creating ANY window, switch THIS THREAD to the currently-active
    // (input) desktop.  Our window is then placed on the same desktop SEB is
    // displaying, so it appears on screen regardless of which desktop is active.
    {
        HDESK hInputDesk = OpenInputDesktop(0, FALSE,
            DESKTOP_CREATEWINDOW | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS);
        DbgLog("UiMessagePump: OpenInputDesktop returned %p", hInputDesk);
        if (hInputDesk) {
            BOOL ok = SetThreadDesktop(hInputDesk);
            DbgLog("UiMessagePump: SetThreadDesktop=%d err=%lu", ok, GetLastError());
            CloseDesktop(hInputDesk);
        }
    }

    // Read desktop name for debug
    {
        wchar_t dbgDesk[64] = {};
        HDESK hd = GetThreadDesktop(GetCurrentThreadId());
        if (hd) GetUserObjectInformationW(hd, UOI_NAME, dbgDesk, sizeof(dbgDesk), nullptr);
        DbgLog("UiMessagePump: thread desktop='%ls'", dbgDesk);
    }

    // Generate a unique class name so re-injection into the same svchost
    // doesn't collide with the old DLL's still-registered class.
    swprintf_s(g_wndClassName, L"SysH%08X", GetTickCount());

    WNDCLASSW wc = {};
    wc.lpfnWndProc   = OverlayWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = g_wndClassName;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(C_BG);
    RegisterClassW(&wc);

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int wx = screenW - kInitW - 24;
    int wy = 28;

    // ── Detect SEB desktop BEFORE window creation ───────────────────────────
    // On SEB's non-Default desktop: skip WS_EX_LAYERED to avoid DWM composition
    // issues (flickering, black outlines, rendering lag). Use regular GDI rendering.
    bool onSebDesktop = false;
    {
        wchar_t deskName[64] = {};
        HDESK hd = GetThreadDesktop(GetCurrentThreadId());
        if (hd) GetUserObjectInformationW(hd, UOI_NAME, deskName, sizeof(deskName), nullptr);
        onSebDesktop = (deskName[0] && _wcsicmp(deskName, L"Default") != 0);
    }

    DWORD exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    if (!onSebDesktop) {
        exStyle |= WS_EX_LAYERED;  // Use layered window ONLY on Default desktop
    }

    // Randomize initial size slightly (+/- 0-30px) to avoid fingerprinting
    int randW = kInitW + (GetTickCount() % 31) - 15;
    int randH = kInitH + ((GetTickCount() >> 4) % 21) - 10;

    g_hwnd = CreateWindowExW(
        exStyle,
        g_wndClassName, L"",
        WS_POPUP | WS_THICKFRAME,
        wx, wy, randW, randH,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_hwnd) return 1;

    DbgLog("UiMessagePump: CreateWindowExW -> hwnd=%p, onSebDesktop=%d", g_hwnd, onSebDesktop);

    // STEALTH: excluded from all OS capture/share/recording APIs.
    // Applied on ALL desktops. The old issue was WDA_EXCLUDEFROMCAPTURE + WS_EX_LAYERED
    // together on SEB's desktop causing invisible rendering. Now that WS_EX_LAYERED is
    // removed on SEB desktop, WDA_EXCLUDEFROMCAPTURE works correctly on its own.
    SetWindowDisplayAffinity(g_hwnd, WDA_EXCLUDEFROMCAPTURE);
    if (!onSebDesktop) {
        SetLayeredWindowAttributes(g_hwnd, 0, 235, LWA_ALPHA);  // Semi-transparent on Default only
    }
    DbgLog("UiMessagePump: WDA_EXCLUDEFROMCAPTURE applied, onSebDesktop=%d", onSebDesktop);

    g_W = kInitW; g_H = kInitH;
    LayoutBtns();

    // ── g_sebChildMode: keyboard bypass mode for SEB's non-Default desktop ────
    // On SEB's desktop (non-Default), use GetAsyncKeyState keyboard injection
    // to bypass SEB's WH_KEYBOARD_LL hook. Already detected above during window
    // creation - just copy the value here.
    g_sebChildMode = onSebDesktop;

    if (!g_hwnd) return 1;
    EnsureFonts();
    // Chat EDIT control removed -- typing in overlay triggers HackerRank focus-loss detection.
    // Users interact via hotkeys only (Ctrl+Alt+W/E/Q).

    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(g_hwnd);

    // ── Register Raw Input for keyboard (RIDEV_INPUTSINK) ─────────────────────
    // Receives ALL keyboard events even without focus, via WM_INPUT.
    // This is a completely separate input path from GetAsyncKeyState and
    // RegisterHotKey. Even when HackerRank registers global shortcuts,
    // Raw Input still delivers the keystrokes to us.
    {
        RAWINPUTDEVICE rid = {};
        rid.usUsagePage = 0x01;  // HID_USAGE_PAGE_GENERIC
        rid.usUsage     = 0x06;  // HID_USAGE_GENERIC_KEYBOARD
        rid.dwFlags     = 0x00000100; // RIDEV_INPUTSINK
        rid.hwndTarget  = g_hwnd;
        RegisterRawInputDevices(&rid, 1, sizeof(rid));
    }

    // Remove DWM border -- on dark overlays Windows draws a bright border
    // around WS_THICKFRAME windows. DWMWA_BORDER_COLOR = 34, DWMWA_COLOR_NONE = 0xFFFFFFFE
    {
        typedef HRESULT(WINAPI* pfnDwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);
        HMODULE hDwm = GetModuleHandleA("dwmapi.dll");
        if (!hDwm) hDwm = LoadLibraryA("dwmapi.dll");
        if (hDwm) {
            pfnDwmSetWindowAttribute fn = (pfnDwmSetWindowAttribute)
                GetProcAddress(hDwm, "DwmSetWindowAttribute");
            if (fn) {
                DWORD noBorder = 0xFFFFFFFE; // DWMWA_COLOR_NONE
                fn(g_hwnd, 34 /*DWMWA_BORDER_COLOR*/, &noBorder, sizeof(noBorder));
            }
        }
    }
    // 50ms timer: hotkey polling + streaming indicator refresh
    SetTimer(g_hwnd, 1, 50, nullptr);

    MSG m;
    while (g_uiRunning && GetMessageW(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    // ── Explicit cleanup after loop exits for ANY reason ──────────────────
    // If g_uiRunning was cleared by OverlayWindowMigrateToInputDesktop BEFORE
    // WM_DESTROY was dispatched, the loop exits without processing WM_DESTROY.
    // The window stays visible as a zombie (draws but accepts no messages).
    // Explicitly destroy it here to prevent that.
    KillTimer(g_hwnd, 1);
    DeleteFonts();
    if (g_hwnd) {
        g_intentionalDestroy = true;
        DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
    }
    return 0;
}

// ─── public lifecycle API (called from OverlayUi / ChimeraMain) ────────────────
void OverlayWindowInstall() {
    if (g_uiRunning) return;
    g_uiRunning = true;
    g_uiThread = CreateThread(nullptr, 0, UiMessagePump, nullptr, 0, &g_uiTid);
}

void OverlayWindowUninstall() {
    g_uiRunning = false;
    if (g_hwnd) {
        g_intentionalDestroy = true; // permit WM_DESTROY to proceed (only legitimate shutdown)
        PostMessage(g_hwnd, WM_DESTROY, 0, 0); g_hwnd = nullptr;
    }
    if (g_uiThread) {
        PostThreadMessageW(g_uiTid, WM_QUIT, 0, 0);
        WaitForSingleObject(g_uiThread, 3000);
        CloseHandle(g_uiThread); g_uiThread = nullptr;
    }
    UnregisterClassW(g_wndClassName, GetModuleHandleW(nullptr));
    if (g_hEdit) { DestroyWindow(g_hEdit); g_hEdit = nullptr; }
}

void OverlayWindowSetVisible(bool visible) {
    if (!g_hwnd) return;
    if (!visible && g_chatFocused) {
        g_chatFocused = false;
        g_prevForeground = nullptr;
        LONG_PTR ex = GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE);
        SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, ex | WS_EX_NOACTIVATE);
    }
    ShowWindow(g_hwnd, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (visible) InvalidateRect(g_hwnd, nullptr, FALSE);
}

void OverlayWindowRefresh() {
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
}

// OverlayWindowResetPosition: called by Ctrl+Alt+H (show path) to fix any
// resize collapse or off-screen drag that made the overlay unusable.
void OverlayWindowResetPosition() {
    if (!g_hwnd) return;
    g_isDot = false;
    g_W = kInitW; g_H = kInitH;
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int wx = screenW - kInitW - 24;
    if (wx < 0) wx = 0;
    SetWindowPos(g_hwnd, HWND_TOPMOST, wx, 28, kInitW, kInitH,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    LayoutBtns();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// Minimize to a tiny 20x20 dot in bottom-right corner.
// The dot is clickable — clicking it calls UiToggleVisible to expand back.

void OverlayWindowMinimizeToDot() {
    if (!g_hwnd) return;
    g_isDot = true;
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    // Place 20x20 dot at bottom-right, 30px from edge
    SetWindowPos(g_hwnd, HWND_TOPMOST, screenW - 50, screenH - 50, 20, 20,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    g_W = 20; g_H = 20;
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// OverlayWindowResetScroll: resets scroll to top, called by UiClearAnswer.
void OverlayWindowResetScroll() {
    g_answerScrollPx = 0;
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
}

// OverlayWindowMigrateToInputDesktop:
// Called by DesktopMonitorThread in ChimeraMain.cpp when SEB (or similar) calls
// CreateNewDesktop + SwitchDesktop. Our window on the old desktop becomes invisible
// (not destroyed -- the old desktop is simply no longer rendered to the screen).
// This function tears down the current window, waits for the UI thread to exit,
// then starts a fresh UI thread which calls SetThreadDesktop(OpenInputDesktop())
// before creating any windows, placing the new overlay on the active desktop.
void OverlayWindowMigrateToInputDesktop() {
    DbgLog("MIGRATE: starting migration");
    // 1. End the message loop cleanly then let the post-loop cleanup destroy the window.
    //    Old approach: set g_uiRunning=false + post WM_DESTROY.
    //    Bug: loop exits on g_uiRunning=false WITHOUT dispatching WM_DESTROY — zombie window.
    //    Fix: post WM_QUIT first (GetMessageW returns FALSE) → loop exits → post-loop
    //    cleanup in UiMessagePump runs DestroyWindow explicitly. No zombie.
    g_uiRunning = false;
    g_intentionalDestroy = true;
    if (g_uiTid) PostThreadMessageW(g_uiTid, WM_QUIT, 0, 0);
    // Also post WM_DESTROY as belt-and-suspenders in case thread is blocked on GetMessage
    if (g_hwnd) { PostMessage(g_hwnd, WM_DESTROY, 0, 0); g_hwnd = nullptr; }
    if (g_uiThread) {
        WaitForSingleObject(g_uiThread, 2000);
        CloseHandle(g_uiThread);
        g_uiThread = nullptr; g_uiTid = 0;
    }
    g_intentionalDestroy = false;

    // 2. Unregister the old class so the new UiMessagePump can register fresh
    if (g_wndClassName[0])
        UnregisterClassW(g_wndClassName, GetModuleHandleW(nullptr));

    // 3. Reset per-window state (dangling handles / mode flags)
    g_hEdit        = nullptr;
    g_chatFocused  = false;
    g_prevForeground = nullptr;
    g_hov          = -1;
    g_sebChildMode = false;  // reset; new UiMessagePump will re-detect
    if (g_previewBmp) { DeleteObject(g_previewBmp); g_previewBmp = nullptr; }
    g_previewActive = false;
    g_queueDeleteTargets.clear();
    g_copyTargets.clear();

    // 4. Brief pause: let the new desktop fully activate before creating windows
    Sleep(300);

    // 5. New UI pump -- first thing it does is SetThreadDesktop(OpenInputDesktop())
    g_uiRunning = true;
    g_uiThread  = CreateThread(nullptr, 0, UiMessagePump, nullptr, 0, &g_uiTid);
    DbgLog("MIGRATE: new UiMessagePump thread started, tid=%lu", g_uiTid);
}
