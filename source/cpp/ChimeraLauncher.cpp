// Behenum AI Launcher — one-click start, one-click stop
// Uses relative paths — works from any folder location.
// Run as administrator.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")

static wchar_t g_dir[MAX_PATH] = {};

static void GetOwnDir() {
    GetModuleFileNameW(nullptr, g_dir, MAX_PATH);
    wchar_t* sl = wcsrchr(g_dir, L'\\');
    if (sl) *(sl + 1) = L'\0';
}

static void MakePath(wchar_t* out, const wchar_t* filename) {
    swprintf_s(out, MAX_PATH, L"%s%s", g_dir, filename);
}

static void KillByName(const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe = {}; pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name) == 0) {
                HANDLE hp = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                if (hp) { TerminateProcess(hp, 0); CloseHandle(hp); }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    GetOwnDir();

    // ═══ START ═══

    // 1. Start kernel driver
    {
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (scm) {
            SC_HANDLE svc = OpenServiceW(scm, L"WinNetSvc", SERVICE_START);
            if (svc) {
                StartServiceW(svc, 0, nullptr);
                CloseServiceHandle(svc);
            }
            CloseServiceHandle(scm);
        }
    }
    Sleep(500);

    // 2. Clear old protected PID
    {
        HANDLE hGuard = CreateFileW(L"\\\\.\\ChimeraGuard",
            GENERIC_READ | GENERIC_WRITE, 0, NULL,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hGuard != INVALID_HANDLE_VALUE) {
            ULONG zeroPid = 0; DWORD ret = 0;
            DeviceIoControl(hGuard, 0x80002000UL, &zeroPid, sizeof(zeroPid), NULL, 0, &ret, NULL);
            CloseHandle(hGuard);
        }
    }

    // 3. Inject overlay into explorer.exe
    wchar_t bootPath[MAX_PATH]; MakePath(bootPath, L"ChimeraBootstrap.exe");
    STARTUPINFOW si = {}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(bootPath, nullptr, nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, g_dir, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 15000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    Sleep(1000);

    // 4. Start background helper (hidden)
    wchar_t helperPath[MAX_PATH]; MakePath(helperPath, L"ChimeraHelper.exe");
    PROCESS_INFORMATION pi2 = {};
    CreateProcessW(helperPath, nullptr, nullptr, nullptr, FALSE,
                   CREATE_NO_WINDOW, nullptr, g_dir, &si, &pi2);
    if (pi2.hProcess) CloseHandle(pi2.hProcess);
    if (pi2.hThread) CloseHandle(pi2.hThread);

    // ═══ RUNNING ═══

    MessageBoxW(nullptr,
        L"Behenum AI is active\n\n"
        L"\x2714  Kernel level security enabled\n"
        L"\x2714  Overlay running\n"
        L"\x2714  Exam browser support ready\n\n"
        L"Ctrl+Alt+H  \x2014  hide / show\n"
        L"Ctrl+Alt+W  \x2014  screenshot\n"
        L"Type in chat bar + Enter  \x2014  send to AI\n\n"
        L"Press OK when done to stop everything.",
        L"Behenum AI", MB_OK | MB_ICONINFORMATION);

    // ═══ STOP ═══

    KillByName(L"ChimeraHelper.exe");
    KillByName(L"RuntimeBroker.exe");

    {
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (scm) {
            SC_HANDLE svc = OpenServiceW(scm, L"WinNetSvc", SERVICE_STOP);
            if (svc) {
                SERVICE_STATUS ss;
                ControlService(svc, SERVICE_CONTROL_STOP, &ss);
                CloseServiceHandle(svc);
            }
            CloseServiceHandle(scm);
        }
    }

    return 0;
}
