// ChimeraHost.exe — minimal host process for overlay DLL on SEB's desktop.
// Started by ChimeraHelper with STARTUPINFO.lpDesktop set to SEB's desktop name.
// ChimeraMain.dll is injected into this process via LoadLibrary.
// The DLL creates the overlay window — which works because THIS process
// was started ON SEB's desktop (unlike explorer.exe which is on Default).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    // Load user32 and gdi32 so the DLL can use them immediately
    LoadLibraryW(L"user32.dll");
    LoadLibraryW(L"gdi32.dll");
    // Sleep forever — the DLL runs in this process
    while (true) Sleep(60000);
    return 0;
}
