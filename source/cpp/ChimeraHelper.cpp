// ChimeraHelper.exe — SEB desktop overlay spawner
// Monitors desktop changes. When SEB creates a new desktop:
// 1. Creates ChimeraHost.exe ON SEB's desktop (via STARTUPINFO.lpDesktop)
// 2. Injects ChimeraMain.dll into it via LoadLibrary injection
// 3. The DLL creates the overlay window — visible on SEB's desktop
//
// This bypasses the fundamental Windows restriction that processes started
// on the Default desktop cannot create windows on other desktops.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")

static wchar_t g_dir[MAX_PATH] = {};

static void GetOwnDir() {
    GetModuleFileNameW(nullptr, g_dir, MAX_PATH);
    wchar_t* sl = wcsrchr(g_dir, L'\\');
    if (sl) *(sl + 1) = L'\0';
}

static wchar_t g_dllPath[MAX_PATH] = {};
static wchar_t g_hostPath[MAX_PATH] = {};

// Inject DLL via LoadLibrary into a remote process
static BOOL InjectDll(HANDLE hProc, const wchar_t* dllPath) {
    SIZE_T pathSize = (wcslen(dllPath) + 1) * sizeof(wchar_t);

    // Allocate memory in remote process for the DLL path string
    LPVOID remoteBuf = VirtualAllocEx(hProc, nullptr, pathSize,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteBuf) {
        wprintf(L"  [-] VirtualAllocEx failed: %lu\n", GetLastError());
        return FALSE;
    }

    // Write DLL path into remote memory
    SIZE_T written = 0;
    if (!WriteProcessMemory(hProc, remoteBuf, dllPath, pathSize, &written) || written != pathSize) {
        wprintf(L"  [-] WriteProcessMemory failed: %lu\n", GetLastError());
        VirtualFreeEx(hProc, remoteBuf, 0, MEM_RELEASE);
        return FALSE;
    }

    // Get LoadLibraryW address (same in all processes due to ASLR being per-boot)
    FARPROC pLoadLib = GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryW");
    if (!pLoadLib) {
        wprintf(L"  [-] GetProcAddress(LoadLibraryW) failed\n");
        VirtualFreeEx(hProc, remoteBuf, 0, MEM_RELEASE);
        return FALSE;
    }

    // Create remote thread that calls LoadLibraryW(dllPath)
    HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0,
        (LPTHREAD_START_ROUTINE)pLoadLib, remoteBuf, 0, nullptr);
    if (!hThread) {
        wprintf(L"  [-] CreateRemoteThread failed: %lu\n", GetLastError());
        VirtualFreeEx(hProc, remoteBuf, 0, MEM_RELEASE);
        return FALSE;
    }

    // Wait for LoadLibrary to complete (max 10s)
    WaitForSingleObject(hThread, 10000);

    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    CloseHandle(hThread);
    // exitCode is the HMODULE returned by LoadLibraryW (0 = failed)
    wprintf(L"  [+] LoadLibraryW returned: 0x%lX\n", exitCode);
    return exitCode != 0;
}

// Spawn ChimeraHost.exe on the target desktop and inject the DLL
static BOOL SpawnOverlayOnDesktop(const wchar_t* desktopName) {
    wprintf(L"[Helper] Spawning ChimeraHost on desktop '%s'...\n", desktopName);
    fflush(stdout);

    // Open the target desktop with full access
    HDESK hDesk = OpenDesktopW(desktopName, 0, FALSE,
        DESKTOP_CREATEWINDOW | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS |
        READ_CONTROL | WRITE_DAC);
    if (hDesk) {
        // Set NULL DACL to ensure our spawned process can create windows
        SECURITY_DESCRIPTOR sd;
        InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
        SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE);
        SECURITY_INFORMATION si = DACL_SECURITY_INFORMATION;
        SetUserObjectSecurity(hDesk, &si, &sd);
        CloseDesktop(hDesk);
        wprintf(L"  [+] Desktop DACL set to unrestricted\n");
    }

    // Create ChimeraHost.exe on SEB's desktop
    STARTUPINFOW startInfo = {};
    startInfo.cb = sizeof(startInfo);
    startInfo.lpDesktop = const_cast<wchar_t*>(desktopName);
    startInfo.dwFlags = STARTF_USESHOWWINDOW;
    startInfo.wShowWindow = SW_HIDE; // no visible console/window for the host

    PROCESS_INFORMATION pi = {};
    wchar_t cmdLine[MAX_PATH + 4];
    swprintf_s(cmdLine, L"\"%s\"", g_hostPath);

    if (!CreateProcessW(g_hostPath, cmdLine, nullptr, nullptr, FALSE,
                        0, nullptr, nullptr, &startInfo, &pi)) {
        wprintf(L"  [-] CreateProcess ChimeraHost failed: %lu\n", GetLastError());
        fflush(stdout);
        return FALSE;
    }
    wprintf(L"  [+] ChimeraHost.exe started (PID %lu) on desktop '%s'\n", pi.dwProcessId, desktopName);
    fflush(stdout);

    // Wait a moment for the process to initialize
    Sleep(1000);

    // Inject ChimeraMain.dll
    HANDLE hProc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pi.dwProcessId);
    if (!hProc) {
        wprintf(L"  [-] OpenProcess failed: %lu\n", GetLastError());
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        return FALSE;
    }

    BOOL ok = InjectDll(hProc, g_dllPath);
    if (ok) {
        wprintf(L"  [+] ChimeraMain.dll injected (PID %lu)\n", pi.dwProcessId);

        // Register host PID with kernel driver for termination protection
        HANDLE hGuard = CreateFileW(L"\\\\.\\ChimeraGuard",
            GENERIC_READ | GENERIC_WRITE, 0, NULL,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hGuard != INVALID_HANDLE_VALUE) {
            ULONG pid = pi.dwProcessId;
            DWORD ret = 0;
            DeviceIoControl(hGuard, 0x80002000UL, &pid, sizeof(pid), NULL, 0, &ret, NULL);
            CloseHandle(hGuard);
            wprintf(L"  [+] Kernel driver protecting PID %lu\n", pi.dwProcessId);
        } else {
            wprintf(L"  [!] Driver not available\n");
        }

        wprintf(L"  [+] Overlay visible on SEB desktop!\n");
    } else {
        wprintf(L"  [-] DLL injection failed\n");
        TerminateProcess(pi.hProcess, 0);
    }
    fflush(stdout);

    CloseHandle(hProc);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return ok;
}

int main() {
    GetOwnDir();
    swprintf_s(g_dllPath, L"%sChimeraMain.dll", g_dir);
    swprintf_s(g_hostPath, L"%sRuntimeBroker.exe", g_dir);

    // Register ourselves with kernel driver for termination protection
    {
        HANDLE hGuard = CreateFileW(L"\\\\.\\ChimeraGuard",
            GENERIC_READ | GENERIC_WRITE, 0, NULL,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hGuard != INVALID_HANDLE_VALUE) {
            ULONG pid = GetCurrentProcessId();
            DWORD ret = 0;
            DeviceIoControl(hGuard, 0x80002000UL, &pid, sizeof(pid), NULL, 0, &ret, NULL);
            CloseHandle(hGuard);
            wprintf(L"[ChimeraHelper] Kernel protection active (PID %lu)\n", pid);
        }
    }

    wprintf(L"[ChimeraHelper] Monitoring desktop changes...\n");
    wprintf(L"[ChimeraHelper] Will spawn overlay on SEB's desktop automatically.\n");
    wprintf(L"[ChimeraHelper] Press Ctrl+C to stop.\n\n");
    fflush(stdout);

    wchar_t lastDesk[256] = {};
    {
        HDESK h = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
        if (h) {
            GetUserObjectInformationW(h, UOI_NAME, lastDesk, sizeof(lastDesk), nullptr);
            CloseDesktop(h);
        }
    }
    wprintf(L"[ChimeraHelper] Initial desktop: '%s'\n", lastDesk);
    fflush(stdout);

    DWORD hostPid = 0; // Track the spawned host process

    while (true) {
        Sleep(500);

        HDESK h = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
        if (!h) continue;

        wchar_t curDesk[256] = {};
        GetUserObjectInformationW(h, UOI_NAME, curDesk, sizeof(curDesk), nullptr);
        CloseDesktop(h);

        if (!curDesk[0]) continue;

        if (lastDesk[0] && wcscmp(curDesk, lastDesk) != 0) {
            wprintf(L"\n[ChimeraHelper] Desktop changed: '%s' -> '%s'\n", lastDesk, curDesk);
            fflush(stdout);

            // If switching TO a non-Default desktop (SEB launched)
            if (_wcsicmp(curDesk, L"Default") != 0) {
                // Wait for SEB to fully initialize its desktop
                wprintf(L"[ChimeraHelper] SEB desktop detected. Waiting 2s...\n");
                fflush(stdout);
                Sleep(2000);

                // Spawn overlay host on SEB's desktop
                if (SpawnOverlayOnDesktop(curDesk)) {
                    // hostPid tracked for cleanup
                }
            } else {
                // Switching back to Default desktop (SEB closed)
                wprintf(L"[ChimeraHelper] Back to Default desktop.\n");
                // Kill the host process if it exists (overlay is back on Default from explorer.exe)
                if (hostPid) {
                    HANDLE hp = OpenProcess(PROCESS_TERMINATE, FALSE, hostPid);
                    if (hp) { TerminateProcess(hp, 0); CloseHandle(hp); }
                    hostPid = 0;
                }
            }
            fflush(stdout);
        }

        wcscpy_s(lastDesk, curDesk);
    }
    return 0;
}
