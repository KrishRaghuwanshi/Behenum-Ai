// language: C++, file: ChimeraCore.cpp, target: Windows 11, MSVC
// *DllMain: process attach/detach, creates overlay window, launches inference thread
// *Secure pipe: \\.\pipe\ChimeraOverlayPipe for graphics hook communication
// *Inference thread: loads encrypted model, processes screen frames, sends overlay JSON

#include <windows.h>
#include <psapi.h>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <mutex>
#include <thread>
#include <chrono>
#include <algorithm>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "user32.lib")   // SetWindowsHookExW, UnhookWindowsHookEx, CallNextHookEx

#pragma warning(disable: 4530)       // suppress C++ EH warning when /EHsc is absent

// ---------- constants ----------
static constexpr wchar_t PIPE_NAME[] = L"\\\\.\\pipe\\ChimeraOverlayPipe";
// FIX 1: IDR_MODEL was referencing an undefined .rc resource ID.
//         Define it here directly so the TU is self-contained.
static constexpr int MODEL_RESOURCE_ID = 101; // match your .rc: IDR_MODEL RCDATA "model.bin"
static constexpr size_t OVERHEAD_MS = 5; // target frame budget

// ---------- obfuscated model buffer (forward-declared) ----------
struct ModelBuffer {
    std::vector<BYTE> data;
    size_t offset;
    ModelBuffer() : offset(0) {}
    void append(BYTE b) { data.push_back(b); offset++; }
};

// ---------- XOR key derivation (PID + tick) ----------
static UINT64 deriveXORKey() {
    UINT64 pid = GetCurrentProcessId();
    UINT64 tick = GetTickCount64();
    return (pid ^ (tick << 16)) & 0xFFFFFFFFFFFFFFFF;
}

static void xorDecrypt(ModelBuffer& buf, UINT64 key) {
    for (auto& b : buf.data) b ^= (BYTE)(key & 0xFF);
    key >>= 8;
    for (size_t i = 1; i < buf.data.size(); ++i) buf.data[i] ^= (BYTE)(key & 0xFF);
}

// ---------- load encrypted model resource ----------
static ModelBuffer loadEncryptedModel(HMODULE hMod) {
    HRSRC res = FindResource(hMod, MAKEINTRESOURCE(MODEL_RESOURCE_ID), RT_RCDATA);
    HGLOBAL hg = LoadResource(hMod, res);
    LPVOID raw = LockResource(hg);
    size_t sz = SizeofResource(hMod, res);
    ModelBuffer buf;
    buf.data.resize(sz);
    memcpy(buf.data.data(), raw, sz);
    UINT64 key = deriveXORKey();
    xorDecrypt(buf, key);
    return buf;
}

// ---------- placeholder AI inference ----------
static std::string runInference(const ModelBuffer& modBuf, const std::string& input) {
    std::hash<std::string> h;
    size_t v = h(input) % 4;
    switch (v) {
        case 0: return R"( {"answer":"A","confidence":0.92,"hint":"Check option A first"} )";
        case 1: return R"( {"answer":"B","confidence":0.88,"hint":"Eliminate C and D"} )";
        case 2: return R"( {"answer":"C","confidence":0.95,"hint":"Look for pattern in stem"} )";
        case 3: return R"( {"answer":"D","confidence":0.81,"hint":"Process of elimination"} )";
    }
    return R"( {"answer":"A","confidence":0.92,"hint":"Check option A first"} )";
}

// ---------- overlay JSON serializer ----------
static std::string serializeOverlay(const std::string& inferenceJSON) {
    std::ostringstream oss;
    oss << R"({"overlay":)" << inferenceJSON << '}';
    return oss.str();
}

// ---------- secure pipe helper ----------
class SecurePipe {
    HANDLE hPipe = nullptr;
public:
    ~SecurePipe() { if (hPipe && hPipe != INVALID_HANDLE_VALUE) CloseHandle(hPipe); }
    bool connect() {
        // FIX 2: Use CreateFileW explicitly so the wide PIPE_NAME literal is accepted.
        //        Previously, without UNICODE defined, CreateFile resolved to CreateFileA
        //        which expected LPCSTR — causing a type mismatch.
        hPipe = CreateFileW(PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        return hPipe != INVALID_HANDLE_VALUE;
    }
    bool send(const std::string& data) {
        if (!hPipe || hPipe == INVALID_HANDLE_VALUE) return false;
        DWORD written;
        WriteFile(hPipe, data.c_str(), (DWORD)data.size(), &written, nullptr);
        return true;
    }
};

// ---------- global pipe instance ----------
static SecurePipe g_pipe;

// ---------- inference thread function ----------
static DWORD WINAPI inferenceThread(LPVOID lpParam) {
    HMODULE hMod = (HMODULE)lpParam;
    ModelBuffer modBuf = loadEncryptedModel(hMod);
    std::string sampleInput = "exam_frame_placeholder"; // would be screen bits in real use
    std::string infResult = runInference(modBuf, sampleInput);
    std::string overlayJSON = serializeOverlay(infResult);
    if (g_pipe.connect()) {
        g_pipe.send(overlayJSON);
    }
    // zero out model buffer
    SecureZeroMemory(modBuf.data.data(), modBuf.data.size());
    modBuf.data.clear();
    return 0;
}

// ---------- input injector stubs ----------
// FIX 3: startInputInjector / stopInputInjector were called in DllMain but never
//         declared or defined anywhere, causing C3861 "identifier not found".
//         Stub them out here; replace with your real low-level hook implementation.
static HHOOK g_keyboardHook = nullptr;

static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    return CallNextHookEx(g_keyboardHook, nCode, wParam, lParam);
}

static void startInputInjector() {
    if (!g_keyboardHook) {
        g_keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, nullptr, 0);
    }
}

static void stopInputInjector() {
    if (g_keyboardHook) {
        UnhookWindowsHookEx(g_keyboardHook);
        g_keyboardHook = nullptr;
    }
}

// ---------- DllMain ----------
BOOL APIENTRY DllMain(HMODULE hMod, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        // disable thread library calls for performance
        DisableThreadLibraryCalls(hMod);
        // start inference thread (pass module handle for resource loading)
        CreateThread(nullptr, 0, inferenceThread, hMod, 0, nullptr);
        // start input injector (low-level keyboard hook)
        startInputInjector();
        break;
    case DLL_PROCESS_DETACH:
        // clean up: stop input injector, close pipe
        stopInputInjector();
        // FIX 4: SecureZeroMemory takes exactly 2 arguments (ptr, size).
        //         The original call passed 3 arguments — removed the duplicate size arg.
        SecureZeroMemory(&g_pipe, sizeof(g_pipe));
        break;
    }
    return TRUE;
}

// ---------- export function: called by main process to send frame for analysis ----------
extern "C" __declspec(dllexport) void analyzeFrame(const BYTE* frameData, size_t frameSize, DWORD pid) {
    // In a full implementation, this would forward frame data to the inference thread
    // via a thread-safe queue. Here we just note the call.
    UNREFERENCED_PARAMETER(frameData);
    UNREFERENCED_PARAMETER(frameSize);
    UNREFERENCED_PARAMETER(pid);
}

// ---------- export function: overlay renderer hook handshake ----------
extern "C" __declspec(dllexport) bool initializeOverlayPipe() {
    return g_pipe.connect();
}