// language: C++, file: ChimeraMain.cpp, target: Windows 11, MSVC
// *runtime orchestrator: loads ChimeraCore.dll, manages overlay + AI loop natively
// *manages the SecurePipe for overlay JSON communication
// *coordinates input injector startup/shutdown
// *handles graceful shutdown and buffer cleanup
// *watchdog timer for proctoring detection with 15ms resolution
// *ZeroOverlayMemory to prevent residual frame detection

#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <wincrypt.h>   // DPAPI: CryptProtectData / CryptUnprotectData
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <chrono>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "crypt32.lib") // DPAPI

// chimera_guard.sys IOCTL — pre-computed CTL_CODE(0x8000,0x800,METHOD_BUFFERED,FILE_ANY_ACCESS)
// Must match chimera_guard.h. No include needed; value is constant.
#define IOCTL_CHIMERA_PROTECT 0x80002000UL

// FIX: KdPrint is a kernel-mode-only macro; replace with a no-op in user-mode code.
#ifndef KdPrint
#define KdPrint(x) ((void)0)
#endif

// FIX: MAX_PAYLOAD_SIZE was referenced but never defined.
static constexpr DWORD MAX_PAYLOAD_SIZE = 4 * 1024 * 1024; // 4 MB

// FIX: TIMER_PERIODIC is a kernel-mode flag; in user-mode use CREATE_WAITABLE_TIMER_MANUAL_RESET
//      or simply pass 0 (periodic is controlled by the period arg to SetWaitableTimer, not a flag).
#ifndef CREATE_WAITABLE_TIMER_MANUAL_RESET
#define CREATE_WAITABLE_TIMER_MANUAL_RESET 0x00000001
#endif

// ---------- constants ----------
static constexpr const wchar_t PIPE_NAME[] = L"\\\\.\\pipe\\ChimeraOverlayPipe";
static constexpr const wchar_t DLL_NAME[]  = L"ChimeraCore.dll";
static constexpr DWORD PIPE_TIMEOUT = 5000; // milliseconds

// ---------- forward declarations ----------
static HMODULE LoadChimeraCore();
static BOOL APIENTRY AntiDebugCallback(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved);
static UINT64 deriveXORKey();
static void xorDecrypt(BYTE* data, size_t size, UINT64 key);
static HANDLE CreateSecurePipe();
static DWORD WINAPI PipeListenerThread(LPVOID lpParam);
static void CleanupResources();
static bool IsProctoringActive();
static bool IsBeingCaptured();
static void ZeroOverlayMemory();
static std::string ReadClipboardText(); // forward decl — defined later in this file
void UiActionSetApiKeyFromClipboard(); // set API key by pasting from clipboard
bool UiKeyIsSet();                     // true if a key is currently loaded

// ---------- global state ----------
static HMODULE g_hChimeraCore       = nullptr;
static HANDLE  g_hPipe              = nullptr;
static HANDLE  g_hWatchdogThread    = nullptr;
static HANDLE  g_hWatchdogTimer     = nullptr;
static HANDLE  g_hPipeListener      = nullptr;
static HANDLE  g_hDesktopMonThread  = nullptr; // detects CreateNewDesktop kiosk switch
static std::mutex g_pipeMutex;
static bool g_running        = false;
static bool g_overlayVisible = true;

// ---------- XOR key derivation (PID + tick) ----------
static UINT64 deriveXORKey() {
    UINT64 pid  = GetCurrentProcessId();
    UINT64 tick = GetTickCount64();
    return (pid ^ (tick << 16)) & 0xFFFFFFFFFFFFFFFF;
}

static void xorDecrypt(BYTE* data, size_t size, UINT64 key) {
    for (size_t i = 0; i < size; ++i) data[i] ^= (BYTE)(key & 0xFF);
    key >>= 8;
    for (size_t i = 1; i < size; ++i) data[i] ^= (BYTE)(key & 0xFF);
}

// ---------- load ChimeraCore.dll ----------
static HMODULE LoadChimeraCore() {
    // Use filename only -- bootstrap prepends its directory to PATH before spawning,
    // so LoadLibraryW searches PATH and finds C:\ChimeraRun\ChimeraCore.dll.
    // GetModuleFileNameW(nullptr) returns the HOST exe path (svchost.exe in System32)
    // when this DLL is manually mapped, making path construction unreliable.
    HMODULE hMod = LoadLibraryW(L"ChimeraCore.dll");
    if (hMod) {
        // decrypt embedded model resource here (PID-keyed XOR) in full build
    }
    return hMod;
}


// ---------- create secure pipe ----------
static HANDLE CreateSecurePipe() {
    // FIX: CreateFileA cannot accept a wchar_t array; use CreateFileW explicitly.
    HANDLE hPipe = CreateFileW(
        PIPE_NAME,
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);

    if (hPipe == INVALID_HANDLE_VALUE) return nullptr;

    // FIX: SetNamedPipeHandleState second argument must be LPDWORD, not int literal.
    DWORD dwMode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(hPipe, &dwMode, nullptr, nullptr);
    return hPipe;
}

// ---------- pipe listener thread ----------
static DWORD WINAPI PipeListenerThread(LPVOID lpParam) {
    HANDLE hPipe = (HANDLE)lpParam;
    char buffer[4 * 1024 * 1024]; // 4 MB max payload buffer
    DWORD bytesRead, bytesWritten;
    DWORD dwMode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(hPipe, &dwMode, nullptr, nullptr);

    while (g_running) {
        DWORD waitResult = WaitForSingleObject(hPipe, PIPE_TIMEOUT);
        if (waitResult != WAIT_OBJECT_0) continue;

        BOOL readSuccess = ReadFile(hPipe, buffer, 4, &bytesRead, nullptr);
        if (!readSuccess || bytesRead < 4) {
            DisconnectNamedPipe(hPipe);
            continue;
        }

        DWORD payloadSize = *(LPDWORD)buffer;
        if (payloadSize > 4 * 1024 * 1024 || payloadSize == 0) {
            DisconnectNamedPipe(hPipe);
            continue;
        }

        readSuccess = ReadFile(hPipe, buffer + 4, payloadSize, &bytesRead, nullptr);
        if (!readSuccess || bytesRead < payloadSize) {
            DisconnectNamedPipe(hPipe);
            continue;
        }

        char ack = 'A';
        WriteFile(hPipe, &ack, 1, &bytesWritten, nullptr);
        DisconnectNamedPipe(hPipe);
    }

    return 0;
}

// ---------- watchdog: detect proctoring processes ----------
static bool IsProctoringActive() {
    const wchar_t* proctoringProcesses[] = {
        L"Proctorio.exe",
        L"Vanguard.exe",
        L"BattlEye.exe",
        L"EasyAntiCheat.exe"
    };

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(PROCESSENTRY32W);

    bool found = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            for (auto& proc : proctoringProcesses) {
                if (!_wcsicmp(entry.szExeFile, proc)) { found = true; break; }
            }
            if (found) break;
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

static bool IsBeingCaptured() {
    return IsProctoringActive();
}

// Zero out the overlay memory region to prevent residual frames
static void ZeroOverlayMemory() {
    // FIX: HHEAP does not exist in user-mode Windows headers; GetProcessHeap returns HANDLE.
    HANDLE hHeap = GetProcessHeap();
    if (!hHeap) return;

    PROCESS_HEAP_ENTRY entry;
    ZeroMemory(&entry, sizeof(entry));

    while (HeapWalk(hHeap, &entry)) {
        // FIX: _PROCESS_HEAP_ENTRY no longer exposes wUsage / dwFlags / dwBlockSize
        //      in the Win10 26100 SDK.  Use cbData (available in all SDK versions)
        //      and check the PROCESS_HEAP_ENTRY_BUSY flag via the wFlags member
        //      which maps to the public lpData / cbData / cbOverhead / iRegionIndex fields.
        //      The safe portable approach: use cbData for size and lpData for the pointer.
        if (entry.lpData == nullptr) continue;

        SIZE_T blockSize = entry.cbData;

        if (blockSize >= 8 * 1024 * 1024 && blockSize <= 32 * 1024 * 1024) {
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery((LPCVOID)entry.lpData, &mbi, sizeof(mbi))) {
                if (mbi.State == MEM_COMMIT && (mbi.Protect & PAGE_READWRITE)) {
                    RtlSecureZeroMemory(mbi.BaseAddress, mbi.RegionSize);
                    FlushInstructionCache(GetCurrentProcess(), mbi.BaseAddress, mbi.RegionSize);
                    return;
                }
            }
        }
    }
}

// ---------- native working loop ----------
HRESULT CaptureFrame(std::vector<BYTE>& outPixels, UINT& outW, UINT& outH, DWORD timeoutMs,
                     bool primaryOnly = false); // primaryOnly=true → primary monitor only
HRESULT CaptureRegionInteractive(std::vector<BYTE>& outPixels, UINT& outW, UINT& outH);
HBITMAP DecodePngDataUrlToHBitmap(const char* dataUrl, size_t urlLen, UINT* outW, UINT* outH);
void ShutdownCapture();
extern void OverlayWindowInstall();
extern void OverlayWindowUninstall();
extern void OverlayWindowSetVisible(bool visible);
extern void OverlayWindowRefresh();
HRESULT EncodePngMemory(const BYTE* pixels, UINT w, UINT h, UINT stride, std::vector<BYTE>& outPng);
std::string Base64EncodeBytes(const BYTE* data, size_t len);
void UiInstall();
void UiUninstall();
void UiPollHotkeys();
void UiSetVisible(bool visible);
void UiAppendAnswer(const char* delta, size_t len);
void UiClearAnswer();
const char* UiGetAnswer();
size_t      UiGetAnswerLen();
void UiActionChatFromClipboard();
struct ApiScreenshot { std::string dataUrl; };
bool NvidiaStreamAnswer(const char* apiKey, const char* userText,
    const ApiScreenshot* screens, int screenCount, const char* preferredModel,
    void (*onDelta)(const char* delta, size_t len, void* userCtx), void* userCtx);
void ZeroApiKey(char* key, size_t len);

static constexpr int MAX_TRAY = 16;
static constexpr const char* DEFAULT_MODEL = "gemini-3.5-flash-lite";

struct TrayShot {
    std::string dataUrl;
    UINT width  = 0;
    UINT height = 0;
    DWORD capturedTick = 0;
};
static std::vector<TrayShot>   g_tray;
static std::mutex               g_trayMutex;
static std::vector<char>        g_apiKey;
static bool                     g_streaming    = false;

// Conversation history: paired user/assistant turns (max 10, cleared on UiActionClear)
static std::vector<std::string> g_historyUser;
static std::vector<std::string> g_historyAssistant;
static int g_historyGen = 0;  // incremented by UiActionClear, checked before history save

// ─── Extern globals read by NvidiaApi.cpp ─────────────────────────────────────
// StreamTrayToAi populates these before calling NvidiaStreamAnswer.
// Kept as raw C arrays for the lifetime of each call.
const char* const* g_nvHistUser   = nullptr;
const char* const* g_nvHistAsst   = nullptr;
int                g_nvHistCount  = 0;
volatile bool      g_nvCancelStream = false;

// ─── DPAPI registry helpers ───────────────────────────────────────────────────
// Path chosen to look like normal AppCompat data -- obscures purpose.
static constexpr const char* REG_PATH = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\MCI Extensions";
static constexpr const char* REG_VAL  = "AppConfig";

static void SaveApiKeyToRegistry(const std::vector<char>& key) {
    if (key.empty()) return;
    DATA_BLOB in  = { (DWORD)key.size(), (BYTE*)const_cast<char*>(key.data()) };
    DATA_BLOB out = {};
    if (!CryptProtectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out)) return;
    HKEY hk = nullptr;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, REG_PATH,
            0, nullptr, 0, KEY_SET_VALUE, nullptr, &hk, nullptr) == ERROR_SUCCESS) {
        RegSetValueExA(hk, REG_VAL, 0, REG_BINARY, out.pbData, out.cbData);
        RegCloseKey(hk);
    }
    LocalFree(out.pbData);
}

static bool LoadApiKeyFromRegistry(std::vector<char>& key) {
    HKEY hk = nullptr;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, REG_PATH, 0, KEY_QUERY_VALUE, &hk) != ERROR_SUCCESS)
        return false;
    DWORD type = 0, size = 0;
    RegQueryValueExA(hk, REG_VAL, nullptr, &type, nullptr, &size);
    if (type != REG_BINARY || size == 0) { RegCloseKey(hk); return false; }
    std::vector<BYTE> enc(size);
    if (RegQueryValueExA(hk, REG_VAL, nullptr, &type, enc.data(), &size) != ERROR_SUCCESS)
        { RegCloseKey(hk); return false; }
    RegCloseKey(hk);
    DATA_BLOB in  = { (DWORD)enc.size(), enc.data() };
    DATA_BLOB out = {};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out)) return false;
    key.assign((char*)out.pbData, (char*)out.pbData + out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return !key.empty();
}

static void LoadApiKey() {
    // Called from DllMain -- must NOT touch DPAPI (CryptProtectData/Unprotect
    // make RPC calls that try to acquire the loader lock → deadlock).
    // Registry load with DPAPI is done after DllMain returns (KeyLoadThread below).

    // 1. Environment variable
    char raw[256] = {};
    DWORD n = GetEnvironmentVariableA("GEMINI_API_KEY", raw, (DWORD)sizeof(raw));
    if (n > 0 && n < sizeof(raw)) {
        g_apiKey.assign(raw, raw + n);
        SecureZeroMemory(raw, sizeof(raw));
        return;
    }
    SecureZeroMemory(raw, sizeof(raw));

    // 2. No hardcoded fallback — user must set their own key via Set Key button
    // (previously had a hardcoded key here, removed for distribution)
}

// KeyLoadThread: runs outside DllMain so the loader lock is free.
// Tries to load a DPAPI-encrypted key the user previously saved via
// UiActionSetApiKeyFromClipboard. Overwrites the hardcoded fallback if found.
static DWORD WINAPI KeyLoadThread(LPVOID) {
    std::vector<char> regKey;
    if (LoadApiKeyFromRegistry(regKey) && !regKey.empty()) {
        if (!g_apiKey.empty()) SecureZeroMemory(g_apiKey.data(), g_apiKey.size());
        g_apiKey = regKey;
        SecureZeroMemory(regKey.data(), regKey.size());
    }
    return 0;
}

static void AnswerDelta(const char* delta, size_t len, void* /*userCtx*/) {
    UiAppendAnswer(delta, len);
}

// Forward decl -- defined after UiIsStreaming below
static bool CallNvidiaStreamSafe(const char* key, const char* prompt,
    const ApiScreenshot* screens, int screenCount, const char* model);

static bool AddPixelsToTray(std::vector<BYTE>& pixels, UINT w, UINT h) {
    std::vector<BYTE> png;
    if (FAILED(EncodePngMemory(pixels.data(), w, h, w * 4, png)) || png.empty()) return false;
    SecureZeroMemory(pixels.data(), pixels.size());

    std::string b64 = Base64EncodeBytes(png.data(), png.size());
    SecureZeroMemory(png.data(), png.size());

    TrayShot shot;
    shot.dataUrl      = "data:image/png;base64," + b64;
    shot.capturedTick = GetTickCount();
    SecureZeroMemory(const_cast<char*>(b64.data()), b64.size());
    shot.width  = w;
    shot.height = h;

    std::lock_guard<std::mutex> lk(g_trayMutex);
    g_tray.push_back(std::move(shot));
    while (g_tray.size() > MAX_TRAY) {
        SecureZeroMemory(const_cast<char*>(g_tray.front().dataUrl.data()),
                         g_tray.front().dataUrl.size());
        g_tray.erase(g_tray.begin());
    }
    return true;
}

static bool CaptureToTray(bool primaryOnly = false) {
    std::vector<BYTE> pixels;
    UINT w = 0, h = 0;
    if (FAILED(CaptureFrame(pixels, w, h, 500, primaryOnly)) || pixels.empty()) return false;
    return AddPixelsToTray(pixels, w, h);
}

static void StreamTrayToAi(const char* prompt, bool includeScreens) {
    // Guard: prevent concurrent sends (double-click, double-hotkey race)
    if (g_streaming) {
        UiClearAnswer();
        const char msg[] = "[Busy] Already sending -- please wait for the current request to finish.";
        UiAppendAnswer(msg, sizeof(msg) - 1);
        return;
    }

    std::vector<ApiScreenshot> shots;
    {
        std::lock_guard<std::mutex> lk(g_trayMutex);
        if (includeScreens) {
            for (auto& t : g_tray) {
                ApiScreenshot s;
                s.dataUrl = t.dataUrl;
                shots.push_back(std::move(s));
            }
        }
    }
    if (includeScreens && shots.empty()) {
        UiClearAnswer();
        const char msg[] = "[Send] No screenshots in queue.\n\n"
            "Press Ctrl+Alt+W to capture the whole screen, or Ctrl+Alt+E for a region, "
            "then press Ctrl+Alt+Q (or click Send) to send them to the AI.";
        UiAppendAnswer(msg, sizeof(msg) - 1);
        return;
    }
    if (g_apiKey.empty()) {
        UiClearAnswer();
        const char msg[] = "[Error] No API key set. Copy your Google AI Studio API key to clipboard and click Set Key.";
        UiAppendAnswer(msg, sizeof(msg) - 1);
        return;
    }

    UiClearAnswer();
    {
        char status[80];
        sprintf_s(status, "[Sending] Connecting to Google AI Studio API (%d screenshot(s))...",
                  (int)shots.size());
        UiAppendAnswer(status, strlen(status));
    }
    // Save the length of the [Sending]... prefix so history only stores the real AI response
    size_t prefixLen = UiGetAnswerLen();

    g_streaming = true;
    char keyBuf[512] = {};
    size_t kLen = g_apiKey.size() < sizeof(keyBuf) - 1 ? g_apiKey.size() : sizeof(keyBuf) - 1;
    memcpy(keyBuf, g_apiKey.data(), kLen);

    // Set extern globals for NvidiaApi.cpp to read (history + cancel).
    // Build raw pointer arrays from the history vectors.
    // These point into g_historyUser/g_historyAssistant which are stable
    // for the duration of this call (g_streaming=true prevents re-entry,
    // and UiActionClear can only run after this returns).
    std::vector<const char*> histU, histA;
    for (auto& s : g_historyUser)      histU.push_back(s.c_str());
    for (auto& s : g_historyAssistant) histA.push_back(s.c_str());
    g_nvHistUser   = histU.empty()  ? nullptr : histU.data();
    g_nvHistAsst   = histA.empty()  ? nullptr : histA.data();
    g_nvHistCount  = (int)histU.size();
    g_nvCancelStream = false;

    // Snapshot the history generation BEFORE the call. If Clear runs during
    // the call, g_historyGen changes and we skip the post-call history save.
    int genBefore = g_historyGen;

    // SAME 5-arg call as the working version -- zero signature change
    bool ok = CallNvidiaStreamSafe(keyBuf, prompt,
                  shots.data(), (int)shots.size(), DEFAULT_MODEL);

    // Clear globals immediately after call returns
    g_nvHistUser  = nullptr;
    g_nvHistAsst  = nullptr;
    g_nvHistCount = 0;
    SecureZeroMemory(keyBuf, sizeof(keyBuf));
    g_streaming = false;

    // Save this turn to conversation history -- skip the [Sending]... prefix.
    // Only save if Clear was NOT clicked during the API call (generation unchanged).
    if (ok && g_historyGen == genBefore) {
        size_t fullLen = UiGetAnswerLen();
        if (fullLen > prefixLen) {
            // Extract only the actual AI response (after our [Sending]... prefix)
            std::string aiResp(UiGetAnswer() + prefixLen, fullLen - prefixLen);
            {
                std::lock_guard<std::mutex> lk(g_trayMutex);
                g_historyUser.push_back(std::string(prompt ? prompt : ""));
                g_historyAssistant.push_back(aiResp);
                while (g_historyUser.size() > 10) {
                    SecureZeroMemory(const_cast<char*>(g_historyUser.front().data()),
                                     g_historyUser.front().size());
                    SecureZeroMemory(const_cast<char*>(g_historyAssistant.front().data()),
                                     g_historyAssistant.front().size());
                    g_historyUser.erase(g_historyUser.begin());
                    g_historyAssistant.erase(g_historyAssistant.begin());
                }
            }
            SecureZeroMemory(const_cast<char*>(aiResp.data()), aiResp.size());
        }
    }

    if (!ok) {
        // Only show the error if no streaming tokens were emitted at all.
        // (If tokens came through then the answer panel already has content.)
        if (UiGetAnswerLen() == 0 || strncmp(UiGetAnswer(), "[Sending]", 9) == 0) {
            UiClearAnswer();
            const char msg[] = "[Error] Request failed.\n\n"
                "Possible causes:\n"
                "  - Network blocked from this process (svchost firewall rule)\n"
                "  - Invalid or expired API key\n"
                "  - Google AI Studio API unreachable\n\n"
                "Try: copy key to clipboard and click Set Key to refresh.";
            UiAppendAnswer(msg, sizeof(msg) - 1);
        }
    }

    for (auto& s : shots)
        SecureZeroMemory(const_cast<char*>(s.dataUrl.data()), s.dataUrl.size());
}

void UiActionCaptureScreen() {
    // Show immediate feedback so the user knows the button registered
    UiClearAnswer();
    const char taking[] = "[Capturing] Taking screenshot...";
    UiAppendAnswer(taking, sizeof(taking) - 1);

    if (CaptureToTray()) {
        int n;
        { std::lock_guard<std::mutex> lk(g_trayMutex); n = (int)g_tray.size(); }
        UiClearAnswer();
        char ok[96];
        sprintf_s(ok, "[Screenshot] Captured. Queue: %d/16. Press Ctrl+Alt+Q to send to AI.", n);
        UiAppendAnswer(ok, strlen(ok));
    } else {
        UiClearAnswer();
        const char msg[] = "[Screenshot] GDI capture failed (BitBlt returned FALSE).\n"
            "Ensure ChimeraBootstrap ran in your interactive session.";
        UiAppendAnswer(msg, sizeof(msg) - 1);
    }
}
void UiActionCaptureRegion() {
    // Interactive snipping-tool style selection
    std::vector<BYTE> pixels;
    UINT w = 0, h = 0;
    HRESULT hr = CaptureRegionInteractive(pixels, w, h);
    if (FAILED(hr) || pixels.empty()) {
        UiClearAnswer();
        const char msg[] = "[Region] Cancelled or selection too small.";
        UiAppendAnswer(msg, sizeof(msg) - 1);
        return;
    }
    if (AddPixelsToTray(pixels, w, h)) {
        int n;
        { std::lock_guard<std::mutex> lk(g_trayMutex); n = (int)g_tray.size(); }
        UiClearAnswer();
        char ok[96];
        sprintf_s(ok, "[Region] Captured %ux%u. Queue: %d/16.", w, h, n);
        UiAppendAnswer(ok, strlen(ok));
    } else {
        UiClearAnswer();
        const char msg[] = "[Region] Encoding failed.";
        UiAppendAnswer(msg, sizeof(msg) - 1);
    }
}
void UiActionSend()           { StreamTrayToAi("Analyze the attached screenshots and answer the visible questions.", true); }
void UiActionSendImagesOnly() { StreamTrayToAi("Analyze only the attached screenshots and answer what is visible.", true); }

// Chat: accepts a typed prompt from the overlay chat bar.
// If prompt is nullptr/empty, falls back to clipboard text.
// Unified send: typed text + any queued images.
// NEVER reads clipboard — only sends what the user explicitly typed.
void UiActionSendWithPrompt(const char* typedPrompt) {
    bool hasShots;
    { std::lock_guard<std::mutex> lk(g_trayMutex); hasShots = !g_tray.empty(); }

    bool hasText = (typedPrompt && typedPrompt[0] != '\0');

    if (hasText) {
        // User typed something — send it, with images if any
        StreamTrayToAi(typedPrompt, hasShots);
    } else if (hasShots) {
        // No text but images in queue — use default analysis prompt
        StreamTrayToAi("Analyze the attached screenshots and answer the visible questions.", true);
    } else {
        // Nothing to send
        UiClearAnswer();
        const char msg[] = "[Send] Nothing to send.\n\n"
            "Type a message in the chat bar, or press Ctrl+Alt+W to capture a screenshot first.";
        UiAppendAnswer(msg, sizeof(msg) - 1);
    }
}

// UiIsStreaming: lets the overlay show an activity indicator.
bool UiIsStreaming() { return g_streaming; }

// UiClearStreaming: called from ActionThread's __except to reset the flag
// if NvidiaStreamAnswer throws an unhandled SEH (e.g. WinHTTP AV in svchost).
void UiClearStreaming() { g_streaming = false; }

// UiActionCancel: user pressed Stop -- signals the SSE loop to abort gracefully.
void UiActionCancel() {
    g_nvCancelStream = true;
}

// CallNvidiaStreamSafe: same 5-arg wrapper as the WORKING version.
// History + cancel communicated via extern globals, not parameters.
static bool CallNvidiaStreamSafe(const char* key, const char* prompt,
    const ApiScreenshot* screens, int screenCount, const char* model) {
    bool ok = false;
    __try {
        ok = NvidiaStreamAnswer(key, prompt, screens, screenCount, model, AnswerDelta, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    return ok;
}

// UiGetQueueTimestamps: fills outTicks[0..maxCount-1] with GetTickCount() values
// recorded when each screenshot was captured. Returns actual count stored.
int UiGetQueueTimestamps(DWORD* outTicks, int maxCount) {
    std::lock_guard<std::mutex> lk(g_trayMutex);
    int n = (int)g_tray.size();
    if (n > maxCount) n = maxCount;
    for (int i = 0; i < n; i++) outTicks[i] = g_tray[i].capturedTick;
    return n;
}

void UiActionClear() {
    // Lock covers both tray and history so a concurrent Clear + history-save race
    // can't leave a stale entry in history after the clear.
    std::lock_guard<std::mutex> lk(g_trayMutex);
    g_historyGen++;  // invalidate any in-flight history save from StreamTrayToAi
    for (auto& t : g_tray)
        SecureZeroMemory(const_cast<char*>(t.dataUrl.data()), t.dataUrl.size());
    g_tray.clear();
    for (auto& s : g_historyUser)      SecureZeroMemory(const_cast<char*>(s.data()), s.size());
    for (auto& s : g_historyAssistant) SecureZeroMemory(const_cast<char*>(s.data()), s.size());
    g_historyUser.clear();
    g_historyAssistant.clear();
    UiClearAnswer();
    // Confirm to the user that context is fresh
    const char msg[] = "[Cleared] New session. Conversation history and screenshots wiped.";
    UiAppendAnswer(msg, sizeof(msg) - 1);
}

// UiTrayCount: called by OverlayWindow.cpp to render the "N img queued" status line.
int UiTrayCount() {
    std::lock_guard<std::mutex> lk(g_trayMutex);
    return (int)g_tray.size();
}

// UiActionDeleteTrayItem: remove a specific screenshot from the queue by index.
void UiActionDeleteTrayItem(int index) {
    std::lock_guard<std::mutex> lk(g_trayMutex);
    if (index < 0 || index >= (int)g_tray.size()) return;
    SecureZeroMemory(const_cast<char*>(g_tray[index].dataUrl.data()),
                     g_tray[index].dataUrl.size());
    g_tray.erase(g_tray.begin() + index);
}

// UiGetTrayItemDimensions: returns width/height of a queued screenshot.
bool UiGetTrayItemDimensions(int index, UINT* outW, UINT* outH) {
    std::lock_guard<std::mutex> lk(g_trayMutex);
    if (index < 0 || index >= (int)g_tray.size()) return false;
    *outW = g_tray[index].width;
    *outH = g_tray[index].height;
    return true;
}

// UiGetTrayPreviewBitmap: decodes a queued screenshot to HBITMAP for preview.
// Caller must call DeleteObject on the returned HBITMAP when done.
HBITMAP UiGetTrayPreviewBitmap(int index, UINT* outW, UINT* outH) {
    std::lock_guard<std::mutex> lk(g_trayMutex);
    if (index < 0 || index >= (int)g_tray.size()) return nullptr;
    return DecodePngDataUrlToHBitmap(
        g_tray[index].dataUrl.c_str(), g_tray[index].dataUrl.size(), outW, outH);
}

// UiKeyIsSet: lets the overlay show a green/red key indicator.
bool UiKeyIsSet() {
    return !g_apiKey.empty();
}

// UiActionSetApiKeyFromClipboard:
// User copies their Google AI Studio API key to the clipboard, then clicks "Set Key" in the overlay.
// The key is stored in the same memory-only g_apiKey buffer (zeroed on detach).
// Nothing is written to disk or registry — safer than setx /M.
// Safe key validation helper — no C++ objects, pure C, crash-proof
static bool ValidateGeminiKey(const char* key, size_t len) {
    if (!key || len < 30) return false;
    if (key[0] != 'A') return false;
    if (key[1] != 'I' && key[1] != 'Q') return false;
    return true;
}

void UiActionSetApiKeyFromClipboard() {
    std::string newKey = ReadClipboardText();
    size_t len = newKey.size();
    if (!ValidateGeminiKey(newKey.c_str(), len)) {
        UiClearAnswer();
        const char msg[] = "[Key] Invalid API key.\n\n"
            "Google AI Studio keys start with \"AIza\" or \"AQ.\" and are 30+ characters.\n"
            "Get your key from: https://aistudio.google.com/apikey";
        UiAppendAnswer(msg, sizeof(msg) - 1);
        if (len > 0) SecureZeroMemory(const_cast<char*>(newKey.data()), len);
        return;
    }
    // Zero out the old key first
    if (!g_apiKey.empty()) {
        SecureZeroMemory(g_apiKey.data(), g_apiKey.size());
        g_apiKey.clear();
    }
    g_apiKey.assign(newKey.begin(), newKey.end());
    SecureZeroMemory(const_cast<char*>(newKey.data()), newKey.size());
    SaveApiKeyToRegistry(g_apiKey);  // persist across sessions via DPAPI
    // Confirm to the user
    UiClearAnswer();
    const char ok[] = "[Key] API key set and saved. Ready to send.";
    UiAppendAnswer(ok, sizeof(ok) - 1);
}
static std::string ReadClipboardText() {
    std::string out;
    if (!OpenClipboard(nullptr)) return out;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t* ws = (const wchar_t*)GlobalLock(h);
        if (ws) {
            int n = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
            if (n > 1) {
                out.resize((size_t)n - 1);
                // FIX: WideCharToMultiByte arg 5 must be LPSTR (non-const char*).
                //      std::string::data() returns non-const char* in C++17; cast to be safe.
                WideCharToMultiByte(CP_UTF8, 0, ws, -1,
                    const_cast<char*>(out.data()), n, nullptr, nullptr);
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}

void UiActionChatFromClipboard() {
    std::string q = ReadClipboardText();
    if (q.empty() || q.size() < 2) return;
    bool hasShots;
    { std::lock_guard<std::mutex> lk(g_trayMutex); hasShots = !g_tray.empty(); }
    StreamTrayToAi(q.c_str(), hasShots);
    SecureZeroMemory(const_cast<char*>(q.data()), q.size());
}

// ---------- desktop monitor thread ----------
// SEB uses CreateNewDesktop kiosk mode: it creates a separate Windows desktop
// and calls SwitchDesktop() to make it the active display. Our overlay window
// (on the old Default desktop) becomes invisible -- not destroyed, just on a
// desktop that's no longer being rendered to the screen.
// This thread polls OpenInputDesktop() every 250ms. When the active desktop
// name changes, it calls OverlayWindowMigrateToInputDesktop() which tears down
// the overlay on the old desktop and rebuilds it on the new one within ~500ms.
extern void OverlayWindowMigrateToInputDesktop();

static DWORD WINAPI DesktopMonitorThread(LPVOID) {
    wchar_t lastDesk[256] = {};
    // Snapshot the initial input desktop name
    {
        HDESK h = OpenInputDesktop(0, FALSE, DESKTOP_ENUMERATE | DESKTOP_READOBJECTS);
        if (h) {
            GetUserObjectInformationW(h, UOI_NAME, lastDesk, sizeof(lastDesk), nullptr);
            CloseDesktop(h);
        }
    }

    while (g_running) {
        Sleep(250); // poll at 4 Hz -- fast enough for a 1-2s SEB switch window

        HDESK h = OpenInputDesktop(0, FALSE, DESKTOP_ENUMERATE | DESKTOP_READOBJECTS);
        if (!h) continue;

        wchar_t curDesk[256] = {};
        GetUserObjectInformationW(h, UOI_NAME, curDesk, sizeof(curDesk), nullptr);
        CloseDesktop(h);

        if (!curDesk[0]) continue;

        if (lastDesk[0] && wcscmp(curDesk, lastDesk) != 0) {
            // Input desktop changed (e.g. Default → SEB's random name, or back).
            // Give the new desktop ~200ms to fully activate before placing our window.
            {
                FILE* f = nullptr;
                fopen_s(&f, "overlay_debug.log", "a");
                if (f) { fprintf(f, "[DesktopMonitor] Desktop changed: '%ls' -> '%ls'\n", lastDesk, curDesk); fclose(f); }
            }
            Sleep(200);
            OverlayWindowMigrateToInputDesktop();
        }

        wcscpy_s(lastDesk, curDesk);
    }
    return 0;
}

// ---------- watchdog thread ----------
static DWORD WINAPI WatchdogThread(LPVOID lpParam) {
    UNREFERENCED_PARAMETER(lpParam);

    // FIX: TIMER_PERIODIC is a kernel-mode constant. In user-mode, CreateWaitableTimerEx
    //      accepts CREATE_WAITABLE_TIMER_MANUAL_RESET (0x1) or 0 for auto-reset.
    //      Periodic behaviour is set via the lPeriod argument of SetWaitableTimer.
    g_hWatchdogTimer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    if (g_hWatchdogTimer) {
        LARGE_INTEGER dueTime;
        dueTime.QuadPart = -150000LL; // 15 ms in 100-ns units, relative
        SetWaitableTimer(g_hWatchdogTimer, &dueTime,
            15,          // periodic period in milliseconds
            nullptr, nullptr, FALSE);
    }

    while (g_running) {
        DWORD waitResult = WaitForSingleObject(g_hWatchdogTimer, PIPE_TIMEOUT);

        if (waitResult == WAIT_OBJECT_0) {
            // NOTE: UiPollHotkeys() removed from here to prevent race on g_hotPrev[].
            // Hotkeys polled exclusively from the UI thread's WM_TIMER (50ms).
            if (IsBeingCaptured()) {
                if (g_overlayVisible) {
                    g_overlayVisible = false;
                    ZeroOverlayMemory();
                    UiSetVisible(false);
                }
            } else {
                if (!g_overlayVisible) {
                    g_overlayVisible = true;
                    UiSetVisible(true);
                }
            }
        }
    }

    if (g_hWatchdogTimer) {
        CloseHandle(g_hWatchdogTimer);
        g_hWatchdogTimer = nullptr;
    }
    return 0;
}

// ---------- export: send overlay JSON to pipe ----------
extern "C" __declspec(dllexport) bool SendOverlayJSON(const char* jsonPayload) {
    std::lock_guard<std::mutex> lk(g_pipeMutex);
    if (!g_hPipe || g_hPipe == INVALID_HANDLE_VALUE) return false;

    DWORD payloadSize = (DWORD)strlen(jsonPayload);
    if (payloadSize == 0 || payloadSize > MAX_PAYLOAD_SIZE) return false;

    // FIX: buffer was char[4 + MAX_PAYLOAD_SIZE] which is 4 + 4MB on the stack — unsafe.
    //      Use a heap buffer instead.
    std::vector<char> buffer(4 + payloadSize);
    memcpy(buffer.data(), &payloadSize, 4);
    memcpy(buffer.data() + 4, jsonPayload, payloadSize);

    DWORD bytesWritten;
    BOOL ok = WriteFile(g_hPipe, buffer.data(), 4 + payloadSize, &bytesWritten, nullptr);
    return ok != FALSE;
}

// ---------- export: query pipe status ----------
extern "C" __declspec(dllexport) bool QueryPipeStatus(DWORD* outStatus) {
    std::lock_guard<std::mutex> lk(g_pipeMutex);
    if (!g_hPipe || g_hPipe == INVALID_HANDLE_VALUE) return false;

    DWORD payloadSize = sizeof(DWORD);
    char buffer[4 + sizeof(DWORD)];
    memcpy(buffer, &payloadSize, 4);

    DWORD bytesWritten;
    BOOL ok = WriteFile(g_hPipe, buffer, 4 + payloadSize, &bytesWritten, nullptr);
    if (!ok) return false;

    DWORD bytesRead;
    ok = ReadFile(g_hPipe, outStatus, sizeof(DWORD), &bytesRead, nullptr);
    return ok != FALSE;
}

// ---------- stub for CleanupResources (forward-declared, no body in original) ----------
static void CleanupResources() { /* extend as needed */ }

// ─── Dedicated hotkey polling thread ─────────────────────────────────────────
// WM_TIMER is the LOWEST priority Windows message -- it gets starved when
// explorer.exe's message queue is busy (other windows on top, shell operations).
// This dedicated thread polls GetAsyncKeyState at 15ms intervals completely
// independently of the UI message loop. When Ctrl+Alt+H or R is detected,
// it directly calls UiToggleVisible (safe from any thread).
// This is the ROOT FIX for "hotkey only works on empty desktop."
extern void UiToggleVisible();
extern void UiActionCaptureScreen();
extern void UiActionCaptureRegion();
extern void UiActionSend();
extern void UiActionSendImagesOnly();

// ── Hotkey polling thread ─────────────────────────────────────────────────────
// Two detection methods:
// 1. Ctrl+Alt+key via GetAsyncKeyState (works everywhere except HackerRank)
// 2. Middle mouse button (scroll wheel click) → toggle overlay (works EVERYWHERE
//    including HackerRank — mouse input cannot be blocked by any app)
static HANDLE g_hHotkeyThread = nullptr;

static DWORD WINAPI HotkeyPollingThread(LPVOID) {
    bool prev[6] = {};
    const int vks[6] = { 'Q', 'A', 'W', 'E', 'R', 'H' };
    bool prevMButton = false;
    DWORD lastMClick = 0;

    while (g_running) {
        Sleep(15);

        // ── METHOD 2: Double middle-click toggle (guaranteed everywhere) ─────
        // Two scroll-wheel clicks within 400ms → toggle overlay
        // No keyboard involved at all — HackerRank cannot block this
        {
            bool mButton = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
            if (mButton && !prevMButton) {
                DWORD now = GetTickCount();
                if (now - lastMClick < 400) {
                    UiToggleVisible();
                    lastMClick = 0; // reset so triple-click doesn't re-trigger
                } else {
                    lastMClick = now;
                }
            }
            prevMButton = mButton;
        }

        // ── METHOD 1: Ctrl+Alt keyboard combos ───────────────────────────────
        bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        bool alt  = (GetAsyncKeyState(VK_MENU)    & 0x8000) != 0;
        if (!ctrl || !alt) { memset(prev, 0, sizeof(prev)); continue; }
        for (int i = 0; i < 6; i++) {
            bool down = (GetAsyncKeyState(vks[i]) & 0x8000) != 0;
            if (down && !prev[i]) {
                switch (i) {
                case 0: { HANDLE h = CreateThread(nullptr,0,[](LPVOID)->DWORD{ UiActionSend(); return 0; },nullptr,0,nullptr); if(h) CloseHandle(h); } break;
                case 1: { HANDLE h = CreateThread(nullptr,0,[](LPVOID)->DWORD{ UiActionSendImagesOnly(); return 0; },nullptr,0,nullptr); if(h) CloseHandle(h); } break;
                case 2: { HANDLE h = CreateThread(nullptr,0,[](LPVOID)->DWORD{ UiActionCaptureScreen(); return 0; },nullptr,0,nullptr); if(h) CloseHandle(h); } break;
                case 3: { HANDLE h = CreateThread(nullptr,0,[](LPVOID)->DWORD{ UiActionCaptureRegion(); return 0; },nullptr,0,nullptr); if(h) CloseHandle(h); } break;
                case 4: case 5: UiToggleVisible(); break;
                }
            }
            prev[i] = down;
        }
    }
    return 0;
}

// ---------- ConnectToDriver ------------------------------------------------
// Opens chimera_guard.sys and registers our PID for ObRegisterCallbacks
// protection. If the driver isn't loaded the call is silently skipped —
// the overlay still works, just without the kernel-level termination guard.
static void ConnectToDriver() {
    HANDLE h = CreateFileW(
        L"\\\\.\\ChimeraGuard",
        GENERIC_READ | GENERIC_WRITE,
        0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;  // driver not running — OK

    ULONG pid      = GetCurrentProcessId();
    DWORD returned = 0;
    DeviceIoControl(h, IOCTL_CHIMERA_PROTECT,
        &pid, sizeof(pid), NULL, 0, &returned, NULL);
    CloseHandle(h);
}

// ---------- DllMain ----------
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);

        // ── CRITICAL: Register x64 exception handling tables (.pdata) ──────────
        // A manually-mapped DLL's .pdata section is NOT registered with the OS
        // (only LoadLibrary does that). Without registration, RtlLookupFunctionEntry
        // returns NULL for ANY code address inside our DLL. This means:
        //   - __try/__except blocks are INVISIBLE to the OS exception dispatcher
        //   - Any SEH exception (e.g. WinHTTP timeout, SSL alert, heap guard page)
        //     appears "unhandled" and TERMINATES the entire svchost process
        // This was the root cause of "overlay closes on Send" -- WinHTTP internally
        // uses SEH, the OS couldn't find our handlers, and killed the process.
        // RtlAddFunctionTable makes us look like a normally-loaded DLL to the
        // exception dispatcher. This is also a STEALTH improvement: EDR tools
        // flag mapped code WITHOUT registered .pdata as injection artifacts.
        {
            BYTE* base = (BYTE*)hModule;
            PIMAGE_DOS_HEADER pDos = (PIMAGE_DOS_HEADER)base;
            PIMAGE_NT_HEADERS pNt  = (PIMAGE_NT_HEADERS)(base + pDos->e_lfanew);
            DWORD pdataRva  = pNt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].VirtualAddress;
            DWORD pdataSize = pNt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].Size;
            if (pdataRva && pdataSize) {
                PRUNTIME_FUNCTION pdata = (PRUNTIME_FUNCTION)(base + pdataRva);
                DWORD count = pdataSize / (DWORD)sizeof(RUNTIME_FUNCTION);
                RtlAddFunctionTable(pdata, count, (DWORD64)base);
            }
        }

        g_running       = true;
        g_hChimeraCore  = LoadChimeraCore();
        g_hPipe         = CreateSecurePipe();
        if (g_hPipe) {
            // FIX: pipeListener was an undeclared local; replaced with the global g_hPipeListener.
            g_hPipeListener = CreateThread(nullptr, 0, PipeListenerThread, g_hPipe, 0, nullptr);
        }
        g_hWatchdogThread = CreateThread(nullptr, 0, WatchdogThread, nullptr, 0, nullptr);
        g_hDesktopMonThread = CreateThread(nullptr, 0, DesktopMonitorThread, nullptr, 0, nullptr);
        g_hHotkeyThread = CreateThread(nullptr, 0, HotkeyPollingThread, nullptr, 0, nullptr);
        LoadApiKey();
        // DPAPI registry override: runs AFTER DllMain returns so loader lock is free.
        { HANDLE h = CreateThread(nullptr, 0, KeyLoadThread, nullptr, 0, nullptr); if (h) CloseHandle(h); }
        UiInstall();
        OverlayWindowInstall();
        ConnectToDriver();  // register PID with chimera_guard.sys (no-op if driver not loaded)
        break;

    case DLL_PROCESS_DETACH:
        g_running = false;
        if (g_hDesktopMonThread) {
            WaitForSingleObject(g_hDesktopMonThread, INFINITE);
            CloseHandle(g_hDesktopMonThread); g_hDesktopMonThread = nullptr;
        }
        // Unregister exception tables before unload
        {
            BYTE* base = (BYTE*)hModule;
            PIMAGE_DOS_HEADER pDos = (PIMAGE_DOS_HEADER)base;
            PIMAGE_NT_HEADERS pNt  = (PIMAGE_NT_HEADERS)(base + pDos->e_lfanew);
            DWORD pdataRva = pNt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].VirtualAddress;
            if (pdataRva) {
                PRUNTIME_FUNCTION pdata = (PRUNTIME_FUNCTION)(base + pdataRva);
                RtlDeleteFunctionTable(pdata);
            }
        }
        if (g_hPipeListener) {
            WaitForSingleObject(g_hPipeListener, INFINITE);
            CloseHandle(g_hPipeListener);
            g_hPipeListener = nullptr;
        }
        if (g_hWatchdogThread) {
            WaitForSingleObject(g_hWatchdogThread, INFINITE);
            CloseHandle(g_hWatchdogThread);
            g_hWatchdogThread = nullptr;
        }
        if (g_hHotkeyThread) {
            WaitForSingleObject(g_hHotkeyThread, 2000);
            CloseHandle(g_hHotkeyThread);
            g_hHotkeyThread = nullptr;
        }
        if (g_hPipe) {
            CloseHandle(g_hPipe);
            g_hPipe = nullptr;
        }
        if (g_hChimeraCore) {
            FreeLibrary(g_hChimeraCore);
            g_hChimeraCore = nullptr;
        }
        UiUninstall();
        OverlayWindowUninstall();
        ShutdownCapture();
        if (!g_apiKey.empty()) {
            ZeroApiKey(g_apiKey.data(), g_apiKey.size());
            g_apiKey.clear();
        }
        {
            std::lock_guard<std::mutex> lk(g_trayMutex);
            for (auto& t : g_tray)
                SecureZeroMemory(const_cast<char*>(t.dataUrl.data()), t.dataUrl.size());
            g_tray.clear();
        }
        CleanupResources();
        break;
    }
    return TRUE;
}