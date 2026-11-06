// language: C++, file: ScreenCapture.cpp, target: Windows 11, MSVC
// *Screen capture via GDI BitBlt -- NO DXGI, NO D3D11, NO DWM interaction.
// *The original DXGI DuplicateOutput() approach forced DWM to reconfigure its
//  composition pipeline each time AcquireNextFrame was called. During that
//  reconfiguration, WDA_EXCLUDEFROMCAPTURE windows are removed from the DWM
//  render tree -- making the overlay disappear from the user's screen (not
//  only from captures). GDI BitBlt avoids this entirely.
// *Works from any process/session without D3D device creation or DXGi init.
// *WDA_EXCLUDEFROMCAPTURE windows appear as opaque black rectangles in the
//  captured image (per Windows 10 2004+ docs for BitBlt + CAPTUREBLT).
//  The overlay is therefore not visible in screenshots sent to the AI, which
//  is the desired behaviour.
// *PNG encoding: WIC (same as before). Base64: inline encoder (unchanged).

#include <windows.h>
#include <tlhelp32.h>
#include <wincodec.h>
#include <vector>
#include <string>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

// ---------- InitCapture: GDI needs no init ----------
HRESULT InitCapture() {
    return S_OK;
}

// ---------- ShutdownCapture: nothing to release ----------
void ShutdownCapture() {}

// ---------- CaptureFrame: GDI BitBlt of the full virtual screen ----------
// outPixels: BGRA buffer (width * height * 4 bytes, top-down).
// outW / outH: dimensions in pixels.
// timeoutMs: ignored -- GDI capture is immediate (no frame-change polling).
HRESULT CaptureFrame(std::vector<BYTE>& outPixels, UINT& outW, UINT& outH,
                     DWORD /*timeoutMs*/, bool primaryOnly) {
    // primaryOnly=true  → primary monitor only (SM_CXSCREEN / SM_CYSCREEN, origin 0,0)
    // primaryOnly=false → full virtual screen covering all monitors
    int sx = primaryOnly ? 0                              : GetSystemMetrics(SM_XVIRTUALSCREEN);
    int sy = primaryOnly ? 0                              : GetSystemMetrics(SM_YVIRTUALSCREEN);
    int sw = primaryOnly ? GetSystemMetrics(SM_CXSCREEN)  : GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int sh = primaryOnly ? GetSystemMetrics(SM_CYSCREEN)  : GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (sw <= 0 || sh <= 0) return E_FAIL;

    HDC hdcScreen = GetDC(NULL);
    if (!hdcScreen) return HRESULT_FROM_WIN32(GetLastError());

    HDC     hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hbm    = CreateCompatibleBitmap(hdcScreen, sw, sh);
    if (!hdcMem || !hbm) {
        if (hbm)    DeleteObject(hbm);
        if (hdcMem) DeleteDC(hdcMem);
        ReleaseDC(NULL, hdcScreen);
        return E_OUTOFMEMORY;
    }
    HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, hbm);

    // SRCCOPY | CAPTUREBLT: include layered windows in the capture.
    BOOL ok = BitBlt(hdcMem, 0, 0, sw, sh, hdcScreen, sx, sy, SRCCOPY | CAPTUREBLT);
    if (!ok) ok = BitBlt(hdcMem, 0, 0, sw, sh, hdcScreen, sx, sy, SRCCOPY);

    // ── Content-protected window capture ──────────────────────────────────────
    // Apps like HackerRank use SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)
    // which makes BitBlt see through the window to the background.
    // Fix: Use PrintWindow(PW_RENDERFULLCONTENT) which asks the window to RENDER
    // ITSELF into our DC. This bypasses WDA because it's the window's own paint
    // code running, not a screen-level capture. No injection needed.
    if (ok) {
        HWND hwndFg = GetForegroundWindow();
        if (hwndFg && IsWindowVisible(hwndFg)) {
            DWORD fgPid = 0;
            GetWindowThreadProcessId(hwndFg, &fgPid);
            // Only attempt on other processes (not our own overlay)
            if (fgPid && fgPid != GetCurrentProcessId()) {
                RECT fgRc; GetWindowRect(hwndFg, &fgRc);
                int fw = fgRc.right - fgRc.left;
                int fh = fgRc.bottom - fgRc.top;
                if (fw > 100 && fh > 100) {
                    // Create a separate DC+bitmap for the PrintWindow output
                    HDC hdcPW = CreateCompatibleDC(hdcScreen);
                    HBITMAP bmpPW = CreateCompatibleBitmap(hdcScreen, fw, fh);
                    if (hdcPW && bmpPW) {
                        HBITMAP oldPW = (HBITMAP)SelectObject(hdcPW, bmpPW);

                        // Try PW_RENDERFULLCONTENT (2) first — captures GPU/compositor content
                        BOOL pw = PrintWindow(hwndFg, hdcPW, 2);

                        if (!pw) {
                            // Fallback: try basic PrintWindow
                            pw = PrintWindow(hwndFg, hdcPW, 0);
                        }

                        if (pw) {
                            // Check if PrintWindow actually rendered content (not all black)
                            COLORREF c1 = GetPixel(hdcPW, fw/3, fh/3);
                            COLORREF c2 = GetPixel(hdcPW, fw/2, fh/2);
                            COLORREF c3 = GetPixel(hdcPW, fw*2/3, fh*2/3);
                            bool allBlack = (c1 == RGB(0,0,0) && c2 == RGB(0,0,0) && c3 == RGB(0,0,0));

                            if (!allBlack) {
                                // PrintWindow succeeded with real content — paste over BitBlt result
                                int dx = fgRc.left - sx;
                                int dy = fgRc.top - sy;
                                if (dx < 0) dx = 0;
                                if (dy < 0) dy = 0;
                                BitBlt(hdcMem, dx, dy, fw, fh, hdcPW, 0, 0, SRCCOPY);
                            }
                        }

                        // If PrintWindow failed or returned black, try child windows
                        // Electron renders in Chrome_RenderWidgetHostHWND child
                        if (!pw || true) {
                            HWND hChild = hwndFg;
                            // Walk child windows looking for the actual render surface
                            EnumChildWindows(hwndFg, [](HWND hw, LPARAM lp) -> BOOL {
                                HWND* pBest = (HWND*)lp;
                                RECT rc; GetClientRect(hw, &rc);
                                int area = (rc.right - rc.left) * (rc.bottom - rc.top);
                                if (area > 100000) { // reasonably large child
                                    *pBest = hw;
                                }
                                return TRUE;
                            }, (LPARAM)&hChild);

                            if (hChild != hwndFg) {
                                // Try PrintWindow on the largest child window
                                RECT childRc; GetWindowRect(hChild, &childRc);
                                int cw = childRc.right - childRc.left;
                                int ch2 = childRc.bottom - childRc.top;
                                if (cw > 100 && ch2 > 100) {
                                    HDC hdcChild = CreateCompatibleDC(hdcScreen);
                                    HBITMAP bmpChild = CreateCompatibleBitmap(hdcScreen, cw, ch2);
                                    if (hdcChild && bmpChild) {
                                        HBITMAP oldChild = (HBITMAP)SelectObject(hdcChild, bmpChild);
                                        BOOL cpw = PrintWindow(hChild, hdcChild, 2);
                                        if (!cpw) cpw = PrintWindow(hChild, hdcChild, 0);
                                        if (cpw) {
                                            COLORREF cc = GetPixel(hdcChild, cw/2, ch2/2);
                                            if (cc != RGB(0,0,0)) {
                                                int cdx = childRc.left - sx;
                                                int cdy = childRc.top - sy;
                                                if (cdx < 0) cdx = 0;
                                                if (cdy < 0) cdy = 0;
                                                BitBlt(hdcMem, cdx, cdy, cw, ch2, hdcChild, 0, 0, SRCCOPY);
                                            }
                                        }
                                        SelectObject(hdcChild, oldChild);
                                    }
                                    if (bmpChild) DeleteObject(bmpChild);
                                    if (hdcChild) DeleteDC(hdcChild);
                                }
                            }
                        }

                        SelectObject(hdcPW, oldPW);
                    }
                    if (bmpPW) DeleteObject(bmpPW);
                    if (hdcPW) DeleteDC(hdcPW);
                }
            }
        }
    }

    SelectObject(hdcMem, hOld);

    HRESULT hr = E_FAIL;
    if (ok) {
        BITMAPINFOHEADER bi = {};
        bi.biSize        = sizeof(bi);
        bi.biWidth       = sw;
        bi.biHeight      = -sh;  // negative = top-down scanline order
        bi.biPlanes      = 1;
        bi.biBitCount    = 32;
        bi.biCompression = BI_RGB;

        outPixels.resize((size_t)sw * sh * 4);
        int lines = GetDIBits(hdcMem, hbm, 0, (UINT)sh,
                              outPixels.data(), (BITMAPINFO*)&bi, DIB_RGB_COLORS);
        if (lines > 0) {
            // GDI GetDIBits with BI_RGB stores BGRA but leaves the alpha byte
            // undefined (often 0). Force alpha = 0xFF for correct PNG output.
            BYTE* px = outPixels.data();
            for (int i = 0, n = sw * sh; i < n; i++, px += 4) px[3] = 0xFF;
            outW = (UINT)sw;
            outH = (UINT)sh;
            hr   = S_OK;
        }
    }

    DeleteObject(hbm);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
    return hr;
}

// ---------- base64 encoder (unchanged from original) ----------
std::string Base64EncodeBytes(const BYTE* data, size_t len) {
    static const char* B64 =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = (uint32_t)data[i] << 16;
        if (i + 1 < len) n |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < len) n |= data[i + 2];
        out.push_back(B64[(n >> 18) & 63]);
        out.push_back(B64[(n >> 12) & 63]);
        out.push_back(i + 1 < len ? B64[(n >> 6) & 63] : '=');
        out.push_back(i + 2 < len ? B64[n & 63] : '=');
    }
    return out;
}

// ---------- PNG encode via WIC (unchanged from original) ----------
HRESULT EncodePngMemory(const BYTE* pixels, UINT w, UINT h, UINT stride,
                        std::vector<BYTE>& outPng) {
    HRESULT hrCo  = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool comMine  = SUCCEEDED(hrCo);

    IWICImagingFactory* factory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) { if (comMine) CoUninitialize(); return hr; }

    IStream* stream = nullptr;
    hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    if (SUCCEEDED(hr)) {
        IWICBitmapEncoder*     encoder = nullptr;
        IWICBitmapFrameEncode* frame   = nullptr;
        hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
        if (SUCCEEDED(hr)) hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
        if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, nullptr);
        if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
        if (SUCCEEDED(hr)) hr = frame->SetSize(w, h);
        if (SUCCEEDED(hr)) {
            WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
            hr = frame->SetPixelFormat(&fmt);
        }
        if (SUCCEEDED(hr)) hr = frame->WritePixels(h, stride, stride * h, (BYTE*)pixels);
        if (SUCCEEDED(hr)) hr = frame->Commit();
        if (frame)   frame->Release();
        if (SUCCEEDED(hr)) hr = encoder->Commit();
        if (encoder) encoder->Release();
        if (SUCCEEDED(hr)) {
            HGLOBAL mem = nullptr;
            hr = GetHGlobalFromStream(stream, &mem);
            if (SUCCEEDED(hr)) {
                SIZE_T sz     = GlobalSize(mem);
                void*  locked = GlobalLock(mem);
                if (locked) {
                    outPng.assign((BYTE*)locked, (BYTE*)locked + sz);
                    GlobalUnlock(mem);
                } else hr = E_FAIL;
            }
        }
        stream->Release();
    }
    factory->Release();
    if (comMine) CoUninitialize();
    return hr;
}


// ─── Base64 Decode (for preview) ──────────────────────────────────────────────
static int B64Val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
std::vector<BYTE> Base64DecodeBytes(const char* b64, size_t len) {
    std::vector<BYTE> out;
    out.reserve(len * 3 / 4);
    int buf = 0, bits = 0;
    for (size_t i = 0; i < len; i++) {
        int v = B64Val(b64[i]);
        if (v < 0) continue;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back((BYTE)(buf >> bits)); buf &= (1 << bits) - 1; }
    }
    return out;
}

// ─── PNG data-URL → HBITMAP (for queue preview) ──────────────────────────────
HBITMAP DecodePngDataUrlToHBitmap(const char* dataUrl, size_t urlLen, UINT* outW, UINT* outH) {
    *outW = *outH = 0;
    const char* pfx = "data:image/png;base64,";
    size_t pfxLen = 22;
    if (urlLen <= pfxLen || memcmp(dataUrl, pfx, pfxLen) != 0) return nullptr;

    std::vector<BYTE> png = Base64DecodeBytes(dataUrl + pfxLen, urlLen - pfxLen);
    if (png.empty()) return nullptr;

    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool comMine = SUCCEEDED(hrCo);
    HBITMAP result = nullptr;

    IWICImagingFactory* fac = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac)))) {
        IStream* stm = nullptr;
        HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, png.size());
        if (hg) { void* p = GlobalLock(hg); memcpy(p, png.data(), png.size()); GlobalUnlock(hg);
                   CreateStreamOnHGlobal(hg, TRUE, &stm); }
        if (stm) {
            IWICBitmapDecoder* dec = nullptr;
            if (SUCCEEDED(fac->CreateDecoderFromStream(stm, nullptr, WICDecodeMetadataCacheOnLoad, &dec))) {
                IWICBitmapFrameDecode* frm = nullptr;
                if (SUCCEEDED(dec->GetFrame(0, &frm))) {
                    UINT w, h; frm->GetSize(&w, &h);
                    IWICFormatConverter* cvt = nullptr;
                    fac->CreateFormatConverter(&cvt);
                    if (cvt) {
                        cvt->Initialize(frm, GUID_WICPixelFormat32bppBGRA,
                            WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
                        BITMAPINFO bmi = {}; bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                        bmi.bmiHeader.biWidth = w; bmi.bmiHeader.biHeight = -(int)h;
                        bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32;
                        void* bits = nullptr;
                        HDC hdc = GetDC(nullptr);
                        result = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
                        ReleaseDC(nullptr, hdc);
                        if (result && bits) { cvt->CopyPixels(nullptr, w*4, w*h*4, (BYTE*)bits); *outW=w; *outH=h; }
                        cvt->Release();
                    }
                    frm->Release();
                }
                dec->Release();
            }
            stm->Release();
        }
        fac->Release();
    }
    if (comMine) CoUninitialize();
    return result;
}

// ─── Interactive Region Selection (snipping-tool style) ──────────────────────
struct RegSelState {
    HDC memDC, dimDC;
    HBITMAP memBmp, dimBmp, oldMem, oldDim;
    int scrW, scrH;
    POINT ptStart, ptCur;
    bool dragging, done, cancelled;
};
static RegSelState g_rsel = {};

static LRESULT CALLBACK RegionSelProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hw, &ps);
        BitBlt(hdc, 0, 0, g_rsel.scrW, g_rsel.scrH, g_rsel.dimDC, 0, 0, SRCCOPY);
        if (g_rsel.dragging) {
            int x1=min(g_rsel.ptStart.x,g_rsel.ptCur.x), y1=min(g_rsel.ptStart.y,g_rsel.ptCur.y);
            int x2=max(g_rsel.ptStart.x,g_rsel.ptCur.x), y2=max(g_rsel.ptStart.y,g_rsel.ptCur.y);
            if (x2>x1 && y2>y1) {
                BitBlt(hdc, x1, y1, x2-x1, y2-y1, g_rsel.memDC, x1, y1, SRCCOPY);
                HPEN pen=CreatePen(PS_SOLID,2,RGB(0,200,80)); HPEN op=(HPEN)SelectObject(hdc,pen);
                HBRUSH ob=(HBRUSH)SelectObject(hdc,GetStockObject(NULL_BRUSH));
                Rectangle(hdc,x1,y1,x2,y2);
                SelectObject(hdc,op); SelectObject(hdc,ob); DeleteObject(pen);
            }
        }
        EndPaint(hw, &ps); return 0;
    }
    case WM_LBUTTONDOWN:
        g_rsel.ptStart = {(short)LOWORD(lp),(short)HIWORD(lp)};
        g_rsel.ptCur = g_rsel.ptStart; g_rsel.dragging = true; SetCapture(hw); return 0;
    case WM_MOUSEMOVE:
        if (g_rsel.dragging) { g_rsel.ptCur={(short)LOWORD(lp),(short)HIWORD(lp)}; InvalidateRect(hw,nullptr,FALSE); }
        return 0;
    case WM_LBUTTONUP:
        if (g_rsel.dragging) { g_rsel.ptCur={(short)LOWORD(lp),(short)HIWORD(lp)};
            g_rsel.dragging=false; ReleaseCapture(); g_rsel.done=true; PostQuitMessage(0); }
        return 0;
    case WM_KEYDOWN:
        if (wp==VK_ESCAPE) { g_rsel.cancelled=true; g_rsel.done=true; PostQuitMessage(0); }
        return 0;
    case WM_RBUTTONDOWN:
        g_rsel.cancelled=true; g_rsel.done=true; PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

HRESULT CaptureRegionInteractive(std::vector<BYTE>& outPixels, UINT& outW, UINT& outH) {
    outW = outH = 0;
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    if (sw <= 0 || sh <= 0) return E_FAIL;

    // 1. Capture full screen
    HDC hdcScr = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(hdcScr);
    HBITMAP memBmp = CreateCompatibleBitmap(hdcScr, sw, sh);
    HBITMAP oldMem = (HBITMAP)SelectObject(memDC, memBmp);
    BitBlt(memDC, 0, 0, sw, sh, hdcScr, 0, 0, SRCCOPY | CAPTUREBLT);

    // 2. Create dimmed copy (40% brightness)
    HDC dimDC = CreateCompatibleDC(hdcScr);
    HBITMAP dimBmp = CreateCompatibleBitmap(hdcScr, sw, sh);
    HBITMAP oldDim = (HBITMAP)SelectObject(dimDC, dimBmp);
    BitBlt(dimDC, 0, 0, sw, sh, memDC, 0, 0, SRCCOPY);
    ReleaseDC(nullptr, hdcScr);

    BITMAPINFO bmi = {}; bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = sw; bmi.bmiHeader.biHeight = -sh;
    bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32;
    std::vector<BYTE> dpx((size_t)sw * sh * 4);
    GetDIBits(dimDC, dimBmp, 0, sh, dpx.data(), &bmi, DIB_RGB_COLORS);
    for (size_t i = 0; i < dpx.size(); i += 4) {
        dpx[i] = dpx[i]*2/5; dpx[i+1] = dpx[i+1]*2/5; dpx[i+2] = dpx[i+2]*2/5;
    }
    SetDIBits(dimDC, dimBmp, 0, sh, dpx.data(), &bmi, DIB_RGB_COLORS);
    dpx.clear();

    // 3. Create fullscreen selection window
    g_rsel = {}; g_rsel.memDC=memDC; g_rsel.dimDC=dimDC;
    g_rsel.memBmp=memBmp; g_rsel.dimBmp=dimBmp; g_rsel.oldMem=oldMem; g_rsel.oldDim=oldDim;
    g_rsel.scrW=sw; g_rsel.scrH=sh;

    wchar_t cls[32]; swprintf_s(cls, L"RS%08X", GetTickCount());
    WNDCLASSW wc = {}; wc.lpfnWndProc = RegionSelProc;
    wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = cls;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32515)); // IDC_CROSS
    RegisterClassW(&wc);
    HWND hSel = CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW, cls, L"",
        WS_POPUP|WS_VISIBLE, 0, 0, sw, sh, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hSel) {
        SelectObject(memDC,oldMem); DeleteObject(memBmp); DeleteDC(memDC);
        SelectObject(dimDC,oldDim); DeleteObject(dimBmp); DeleteDC(dimDC);
        UnregisterClassW(cls, GetModuleHandleW(nullptr)); return E_FAIL;
    }
    SetForegroundWindow(hSel);

    // 4. Local message loop
    MSG m;
    while (!g_rsel.done && GetMessageW(&m, nullptr, 0, 0)) { TranslateMessage(&m); DispatchMessageW(&m); }
    DestroyWindow(hSel);
    UnregisterClassW(cls, GetModuleHandleW(nullptr));

    // 5. Crop selected region from the undimmed original
    HRESULT result = E_FAIL;
    if (!g_rsel.cancelled) {
        int x1=min(g_rsel.ptStart.x,g_rsel.ptCur.x), y1=min(g_rsel.ptStart.y,g_rsel.ptCur.y);
        int x2=max(g_rsel.ptStart.x,g_rsel.ptCur.x), y2=max(g_rsel.ptStart.y,g_rsel.ptCur.y);
        int cw=x2-x1, ch=y2-y1;
        if (cw > 10 && ch > 10) {
            std::vector<BYTE> full((size_t)sw*sh*4);
            GetDIBits(memDC, memBmp, 0, sh, full.data(), &bmi, DIB_RGB_COLORS);
            outW=(UINT)cw; outH=(UINT)ch;
            outPixels.resize((size_t)cw*ch*4);
            for (int r = 0; r < ch; r++)
                memcpy(outPixels.data() + r*cw*4, full.data() + ((y1+r)*sw+x1)*4, cw*4);
            for (size_t i = 3; i < outPixels.size(); i += 4) outPixels[i] = 0xFF;
            result = S_OK;
        }
    }

    // 6. Cleanup
    SelectObject(memDC,oldMem); DeleteObject(memBmp); DeleteDC(memDC);
    SelectObject(dimDC,oldDim); DeleteObject(dimBmp); DeleteDC(dimDC);
    return result;
}
