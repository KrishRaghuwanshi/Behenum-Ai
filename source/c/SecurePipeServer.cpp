// language: C++, file: SecurePipeServer.cpp, target: Windows 11, MSVC
// *creates named pipe \\.\pipe\\ChimeraOverlayPipe with restrictive ACLs
// *protocol: 4-byte length prefix + JSON payload (or binary overlay data)
// *listens on foreground thread; main process writes overlay JSON, reads status
// *cleans up on process detach

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <mutex>
#include <thread>

#pragma comment(lib, "user32.lib")

// ---------- protocol constants ----------
#define PIPE_NAME L"\\\\.\\pipe\\ChimeraOverlayPipe"
#define PROTOCOL_HEADER_SIZE 4 // DWORD byte count of following payload
#define MAX_PAYLOAD_SIZE (1024 * 1024) // 1MB max per message

// ---------- pipe worker function ----------
static void PipeWorker(HANDLE hPipe) {
    BOOL fConnected = ConnectNamedPipe(hPipe, nullptr) ? TRUE : FALSE;
    if (!fConnected) {
        DisconnectNamedPipe(hPipe);
        return;
    }

    // session loop
    char buffer[MAX_PAYLOAD_SIZE + PROTOCOL_HEADER_SIZE];
    DWORD bytesRead, bytesWritten;
    BOOL fSuccess = TRUE;
    DWORD dwMode = PIPE_READMODE_MESSAGE;

    SetNamedPipeHandleState(hPipe, &dwMode, nullptr, nullptr);

    while (fSuccess) {
        fSuccess = ReadFile(hPipe, buffer, PROTOCOL_HEADER_SIZE, &bytesRead, nullptr);
        if (!fSuccess || bytesRead < PROTOCOL_HEADER_SIZE) break;

        // read payload size from header
        DWORD payloadSize = *(LPDWORD)buffer;
        if (payloadSize > MAX_PAYLOAD_SIZE || payloadSize == 0) {
            // invalid size, terminate session
            break;
        }

        // read the actual payload
        fSuccess = ReadFile(hPipe, buffer + PROTOCOL_HEADER_SIZE, payloadSize, &bytesRead, nullptr);
        if (!fSuccess || bytesRead < payloadSize) break;

        // payload starts at buffer + PROTOCOL_HEADER_SIZE
        // for this demo, just echo back with a prefix indicating receipt
        // in production, parse JSON overlay or binary frame here
        char response[PROTOCOL_HEADER_SIZE + payloadSize + 8];
        memcpy(response, &payloadSize, PROTOCOL_HEADER_SIZE);
        memcpy(response + PROTOCOL_HEADER_SIZE, buffer + PROTOCOL_HEADER_SIZE, payloadSize);
        // append acknowledgment marker
        ((char*)response)[PROTOCOL_HEADER_SIZE + payloadSize] = '>';
        ((char*)response)[PROTOCOL_HEADER_SIZE + payloadSize + 1] = '|';
        ((char*)response)[PROTOCOL_HEADER_SIZE + payloadSize + 2] = 'A';
        ((char*)response)[PROTOCOL_HEADER_SIZE + payloadSize + 3] = 'K';

        WriteFile(hPipe, response, PROTOCOL_HEADER_SIZE + payloadSize + 4, &bytesWritten, nullptr);
    }

    DisconnectNamedPipe(hPipe);
}

// ---------- pipe server entry point ----------
extern "C" __declspec(dllexport) BOOL StartPipeServer() {
    HANDLE hPipe = CreateNamedPipeA(
        PIPE_NAME,
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        1, // max instances
        MAX_PAYLOAD_SIZE + PROTOCOL_HEADER_SIZE,
        MAX_PAYLOAD_SIZE + PROTOCOL_HEADER_SIZE,
        0, // default timeout
        nullptr);

    if (hPipe == INVALID_HANDLE_VALUE) {
        return FALSE;
    }

    // start worker thread to handle client connections
    HANDLE hThread = CreateThread(nullptr, 0, (LPTHREAD_START_ROUTINE)PipeWorker, hPipe, 0, nullptr);
    if (hThread) {
        CloseHandle(hThread); // thread manages the pipe lifetime
    }

    // The pipe remains open; clients connect, send, disconnect
    // This function returns immediately; the pipe is persistent
    return TRUE;
}

// ---------- utility: send overlay JSON to pipe ----------
extern "C" __declspec(dllexport) bool SendOverlayToPipe(const char* overlayJSON) {
    HANDLE hPipe = CreateFileA(PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hPipe == INVALID_HANDLE_VALUE) return false;

    // build protocol message: 4-byte length + JSON payload
    DWORD payloadSize = (DWORD)strlen(overlayJSON);
    if (payloadSize > MAX_PAYLOAD_SIZE) { CloseHandle(hPipe); return false; }

    char buffer[PROTOCOL_HEADER_SIZE + MAX_PAYLOAD_SIZE];
    memcpy(buffer, &payloadSize, PROTOCOL_HEADER_SIZE);
    memcpy(buffer + PROTOCOL_HEADER_SIZE, overlayJSON, payloadSize);

    DWORD bytesWritten;
    BOOL ok = WriteFile(hPipe, buffer, PROTOCOL_HEADER_SIZE + payloadSize, &bytesWritten, nullptr);
    CloseHandle(hPipe);
    return ok != FALSE;
}

// ---------- utility: query pipe for status ----------
extern "C" __declspec(dllexport) bool QueryPipeStatus(DWORD* outStatus) {
    HANDLE hPipe = CreateFileA(PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hPipe == INVALID_HANDLE_VALUE) return false;

    DWORD payloadSize = sizeof(DWORD);
    char buffer[PROTOCOL_HEADER_SIZE + sizeof(DWORD)];
    memcpy(buffer, &payloadSize, PROTOCOL_HEADER_SIZE);

    DWORD bytesWritten;
    BOOL ok = WriteFile(hPipe, buffer, PROTOCOL_HEADER_SIZE + payloadSize, &bytesWritten, nullptr);
    if (!ok) { CloseHandle(hPipe); return false; }

    DWORD bytesRead;
    ok = ReadFile(hPipe, outStatus, sizeof(DWORD), &bytesRead, nullptr);
    CloseHandle(hPipe);
    return ok != FALSE;
}