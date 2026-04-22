// chromecapture — captures a single Google Chrome window into the same shared
// D3D12 texture used by screencapture, so screendisplay (or any other consumer
// that speaks the sharing contract) can attach unchanged.
//
// Startup flow:
//   1. Enumerate top-level Chrome windows.
//   2. Show a popup so the user can pick one.
//   3. Capture that window via Windows.Graphics.Capture (the only supported
//      GPU-accelerated per-window capture API on Windows) and copy every
//      arriving frame into the shared D3D12 texture.
//
// Sharing contract (consumer side): identical to screencapture —
//   L"streamco_shared_texture" / L"streamco_shared_fence" / L"streamco_shared_info".

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <dwmapi.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cwctype>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace wgc   = winrt::Windows::Graphics::Capture;
namespace wgdx  = winrt::Windows::Graphics::DirectX;
namespace wgd11 = winrt::Windows::Graphics::DirectX::Direct3D11;

namespace {

struct SharedInfo {
    UINT32 width;
    UINT32 height;
    UINT32 format;
    UINT32 frameCount;
    UINT64 fenceValue;
};

struct ChromeWindow {
    HWND         hwnd;
    std::wstring title;
};

// The Windows SDK's windows.graphics.directx.direct3d11.interop.h only exposes
// this interface in some WINAPI_FAMILY partitions, so we redeclare it under a
// local name — QueryInterface only cares about the GUID.
struct __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1"))
IDxgiInterfaceAccess : ::IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetInterface(REFIID iid, void** p) = 0;
};

void ThrowIfFailed(HRESULT hr, const char* where) {
    if (FAILED(hr)) {
        char msg[256];
        std::snprintf(msg, sizeof(msg), "%s failed: 0x%08lX",
                      where, static_cast<unsigned long>(hr));
        throw std::runtime_error(msg);
    }
}

std::atomic<bool> g_running{true};

BOOL WINAPI ConsoleHandler(DWORD sig) {
    if (sig == CTRL_C_EVENT || sig == CTRL_BREAK_EVENT || sig == CTRL_CLOSE_EVENT) {
        g_running = false;
        return TRUE;
    }
    return FALSE;
}

// ── Chrome window discovery ─────────────────────────────────────────────────

bool IsChromeProcess(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    wchar_t path[MAX_PATH]{};
    DWORD   size = MAX_PATH;
    bool    isChrome = false;
    if (QueryFullProcessImageNameW(h, 0, path, &size)) {
        std::wstring p(path);
        auto pos = p.find_last_of(L"\\/");
        std::wstring name = (pos == std::wstring::npos) ? p : p.substr(pos + 1);
        for (auto& c : name) c = static_cast<wchar_t>(std::towlower(c));
        isChrome = (name == L"chrome.exe");
    }
    CloseHandle(h);
    return isChrome;
}

BOOL CALLBACK EnumChromeWindowsProc(HWND hwnd, LPARAM lp) {
    auto* out = reinterpret_cast<std::vector<ChromeWindow>*>(lp);
    if (!IsWindowVisible(hwnd))         return TRUE;
    if (GetWindow(hwnd, GW_OWNER))      return TRUE;
    wchar_t cls[64]{};
    if (!GetClassNameW(hwnd, cls, 64))  return TRUE;
    // Chromium's top-level browser window class.
    if (wcscmp(cls, L"Chrome_WidgetWin_1") != 0) return TRUE;
    wchar_t title[512]{};
    if (GetWindowTextW(hwnd, title, 512) == 0)   return TRUE;
    // Filter out Edge / Brave / Opera / etc. that share the Chromium class.
    if (!IsChromeProcess(hwnd))                  return TRUE;
    out->push_back({ hwnd, std::wstring(title) });
    return TRUE;
}

// ── Picker dialog ───────────────────────────────────────────────────────────

struct PickerState {
    const std::vector<ChromeWindow>* windows;
    HWND listbox;
    HWND result;
};

LRESULT CALLBACK PickerProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* s = reinterpret_cast<PickerState*>(
        GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    switch (msg) {
        case WM_COMMAND: {
            if (!s) break;
            WORD id  = LOWORD(wp);
            WORD ntf = HIWORD(wp);
            if (id == 100 && ntf == LBN_DBLCLK) id = 1;  // double-click = OK
            if (id == 1) {
                int sel = (int)SendMessageW(s->listbox, LB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < (int)s->windows->size()) {
                    s->result = (*s->windows)[sel].hwnd;
                }
                DestroyWindow(hWnd);
                return 0;
            }
            if (id == 2) {
                s->result = nullptr;
                DestroyWindow(hWnd);
                return 0;
            }
            break;
        }
        case WM_CLOSE:   DestroyWindow(hWnd); return 0;
        case WM_DESTROY: PostQuitMessage(0);  return 0;
    }
    return DefWindowProcW(hWnd, msg, wp, lp);
}

HWND SelectChromeWindow(const std::vector<ChromeWindow>& windows) {
    if (windows.empty()) {
        MessageBoxW(nullptr,
            L"No Chrome windows found. Make sure Google Chrome is running "
            L"with at least one visible window.",
            L"chromecapture", MB_ICONERROR);
        return nullptr;
    }

    HINSTANCE            hInst  = GetModuleHandleW(nullptr);
    static const wchar_t kClass[] = L"ChromeCapturePicker";

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc   = PickerProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    const int W = 620, H = 380;
    RECT r{ 0, 0, W, H };
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);

    HWND hWnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME, kClass,
        L"chromecapture - select a Chrome window",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top,
        nullptr, nullptr, hInst, nullptr);
    if (!hWnd) return nullptr;

    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    HWND lbl = CreateWindowExW(0, L"STATIC",
        L"Select the Chrome window to capture:",
        WS_CHILD | WS_VISIBLE,
        10, 10, W - 20, 20, hWnd, nullptr, hInst, nullptr);
    SendMessageW(lbl, WM_SETFONT, (WPARAM)font, TRUE);

    HWND list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY,
        10, 35, W - 20, H - 95,
        hWnd, (HMENU)(INT_PTR)100, hInst, nullptr);
    SendMessageW(list, WM_SETFONT, (WPARAM)font, TRUE);
    for (auto& w : windows) {
        SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)w.title.c_str());
    }
    SendMessageW(list, LB_SETCURSEL, 0, 0);

    HWND ok = CreateWindowExW(0, L"BUTTON", L"Capture",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        W - 200, H - 45, 90, 30,
        hWnd, (HMENU)(INT_PTR)1, hInst, nullptr);
    SendMessageW(ok, WM_SETFONT, (WPARAM)font, TRUE);

    HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancel",
        WS_CHILD | WS_VISIBLE,
        W - 105, H - 45, 90, 30,
        hWnd, (HMENU)(INT_PTR)2, hInst, nullptr);
    SendMessageW(cancel, WM_SETFONT, (WPARAM)font, TRUE);

    PickerState state{ &windows, list, nullptr };
    SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state));

    ShowWindow(hWnd, SW_SHOW);
    UpdateWindow(hWnd);
    SetForegroundWindow(hWnd);
    SetFocus(list);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hWnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return state.result;
}

// ── Crop computation ────────────────────────────────────────────────────────
// Figure out which part of a captured-window frame is the HTML viewport.
// Runs at startup and again on every window resize.

void ComputeCrop(HWND target, UINT poolW, UINT poolH,
                 UINT& cropX, UINT& cropY, UINT& cropW, UINT& cropH) {
    cropX = 0; cropY = 0; cropW = poolW; cropH = poolH;

    HWND renderHwnd = nullptr;
    EnumChildWindows(target,
        [](HWND h, LPARAM lp) -> BOOL {
            wchar_t cls[128]{};
            if (GetClassNameW(h, cls, 128) &&
                wcscmp(cls, L"Chrome_RenderWidgetHostHWND") == 0) {
                *reinterpret_cast<HWND*>(lp) = h;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&renderHwnd));

    if (renderHwnd) {
        RECT wr{}, rr{};
        if (FAILED(DwmGetWindowAttribute(
                target, DWMWA_EXTENDED_FRAME_BOUNDS,
                &wr, sizeof(wr)))) {
            GetWindowRect(target, &wr);
        }
        GetWindowRect(renderHwnd, &rr);
        LONG dx = rr.left - wr.left;
        LONG dy = rr.top  - wr.top;
        LONG dw = rr.right - rr.left;
        LONG dh = rr.bottom - rr.top;
        if (dx < 0) { dw += dx; dx = 0; }
        if (dy < 0) { dh += dy; dy = 0; }
        if (dx + dw > (LONG)poolW) dw = (LONG)poolW - dx;
        if (dy + dh > (LONG)poolH) dh = (LONG)poolH - dy;
        if (dw > 0 && dh > 0) {
            cropX = (UINT)dx; cropY = (UINT)dy;
            cropW = (UINT)dw; cropH = (UINT)dh;
            return;
        }
    }

    // Heuristic fallback — only applies in normal windowed/maximized mode.
    // In F11 fullscreen Chrome strips WS_CAPTION and there is no tab strip
    // or URL bar to crop; applying the 88-px heuristic would eat into the
    // actual content.
    const LONG style = GetWindowLongW(target, GWL_STYLE);
    if (!(style & WS_CAPTION)) return;

    const UINT dpi = GetDpiForWindow(target);
    const UINT topCrop = 88u * dpi / 96u;
    if (topCrop < poolH) {
        cropY = topCrop;
        cropH = poolH - topCrop;
    }
}

// ── Red-border overlay ──────────────────────────────────────────────────────
// A layered, transparent, click-through topmost window that tracks the
// captured Chrome content region and paints a red frame around it. The
// overlay lives in its own thread with its own message loop so the capture
// thread stays responsive.

struct OverlayCtx {
    HWND               target;
    std::atomic<bool>* running;
    std::mutex         mtx;
    UINT               cropX = 0, cropY = 0, cropW = 0, cropH = 0;
};

LRESULT CALLBACK OverlayProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ctx = reinterpret_cast<OverlayCtx*>(
        GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc;
            GetClientRect(hWnd, &rc);
            FillRect(hdc, &rc, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            HBRUSH red = CreateSolidBrush(RGB(255, 0, 0));
            const int t = 3;
            RECT sides[] = {
                { rc.left,       rc.top,        rc.right,    rc.top + t },
                { rc.left,       rc.bottom - t, rc.right,    rc.bottom  },
                { rc.left,       rc.top,        rc.left + t, rc.bottom  },
                { rc.right - t,  rc.top,        rc.right,    rc.bottom  },
            };
            for (auto const& s : sides) FillRect(hdc, &s, red);
            DeleteObject(red);
            EndPaint(hWnd, &ps);
            return 0;
        }
        case WM_TIMER: {
            if (!ctx) break;
            if (!ctx->running->load() || !IsWindow(ctx->target)) {
                DestroyWindow(hWnd);
                return 0;
            }
            if (IsIconic(ctx->target)) {
                ShowWindow(hWnd, SW_HIDE);
                return 0;
            }
            RECT wr{};
            if (FAILED(DwmGetWindowAttribute(
                    ctx->target, DWMWA_EXTENDED_FRAME_BOUNDS,
                    &wr, sizeof(wr)))) {
                GetWindowRect(ctx->target, &wr);
            }
            UINT cx, cy, cw, ch;
            {
                std::lock_guard<std::mutex> lk(ctx->mtx);
                cx = ctx->cropX; cy = ctx->cropY;
                cw = ctx->cropW; ch = ctx->cropH;
            }
            SetWindowPos(hWnd, HWND_TOPMOST,
                wr.left + static_cast<int>(cx),
                wr.top  + static_cast<int>(cy),
                static_cast<int>(cw),
                static_cast<int>(ch),
                SWP_NOACTIVATE | SWP_SHOWWINDOW);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wp, lp);
}

void RunOverlay(OverlayCtx* ctx) {
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    static const wchar_t kOverlayClass[] = L"ChromeCaptureOverlay";
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc   = OverlayProc;
        wc.hInstance     = hInst;
        wc.lpszClassName = kOverlayClass;
        wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        RegisterClassExW(&wc);
        registered = true;
    }

    UINT cx, cy, cw, ch;
    {
        std::lock_guard<std::mutex> lk(ctx->mtx);
        cx = ctx->cropX; cy = ctx->cropY;
        cw = ctx->cropW; ch = ctx->cropH;
    }
    HWND hWnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST
            | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        kOverlayClass, L"", WS_POPUP,
        0, 0,
        static_cast<int>(cw),
        static_cast<int>(ch),
        nullptr, nullptr, hInst, nullptr);
    if (!hWnd) return;

    SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
    // Black is the color key — everything inside the frame stays see-through.
    SetLayeredWindowAttributes(hWnd, RGB(0, 0, 0), 0, LWA_COLORKEY);

    RECT wr{};
    if (FAILED(DwmGetWindowAttribute(
            ctx->target, DWMWA_EXTENDED_FRAME_BOUNDS, &wr, sizeof(wr)))) {
        GetWindowRect(ctx->target, &wr);
    }
    SetWindowPos(hWnd, HWND_TOPMOST,
        wr.left + static_cast<int>(cx),
        wr.top  + static_cast<int>(cy),
        static_cast<int>(cw),
        static_cast<int>(ch),
        SWP_NOACTIVATE | SWP_SHOWWINDOW);

    SetTimer(hWnd, 1, 16, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

} // namespace

int main(int argc, char** argv) {
    std::wstring prefix = L"streamco_shared";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if ((a == "-n" || a == "--name") && i + 1 < argc) {
            std::string p = argv[++i];
            prefix.assign(p.begin(), p.end());
        } else if (a == "-h" || a == "--help") {
            std::puts("chromecapture [-n <prefix>]   (default prefix: streamco_shared)");
            return 0;
        }
    }
    const std::wstring texNameS   = prefix + L"_texture";
    const std::wstring fenceNameS = prefix + L"_fence";
    const std::wstring infoNameS  = prefix + L"_info";
    const wchar_t* kTextureName = texNameS.c_str();
    const wchar_t* kFenceName   = fenceNameS.c_str();
    const wchar_t* kInfoMapName = infoNameS.c_str();

    // Must be the first thing: Windows.Graphics.Capture delivers frames at
    // physical pixel size, and captureItem.Size() only reports physical pixels
    // if the process is DPI-aware. Without this call the shared texture would
    // be sized in virtual (logical) pixels while WGC writes physical pixels
    // into it — you'd see only the top-left corner of the window.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    try {
        if (!wgc::GraphicsCaptureSession::IsSupported()) {
            throw std::runtime_error(
                "Windows.Graphics.Capture is not supported on this system "
                "(requires Windows 10 1903 or later).");
        }

        std::vector<ChromeWindow> windows;
        EnumWindows(EnumChromeWindowsProc, reinterpret_cast<LPARAM>(&windows));

        HWND target = SelectChromeWindow(windows);
        if (!target) {
            std::fwprintf(stderr, L"No window selected. Exiting.\n");
            return 0;
        }

        wchar_t title[512]{};
        GetWindowTextW(target, title, 512);
        std::wprintf(L"Capturing: %ls\n", title);

        ComPtr<IDXGIFactory6> factory;
        ThrowIfFailed(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)),
                      "CreateDXGIFactory2");

        // Prefer an NVIDIA adapter so the shared texture lives on the same GPU
        // as the downstream NVENC sender. Fall back to first-with-output when
        // no NVIDIA GPU is present.
        ComPtr<IDXGIAdapter1> adapter;
        ComPtr<IDXGIAdapter1> fallback;
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> a;
            if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 desc{};
            a->GetDesc1(&desc);
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            ComPtr<IDXGIOutput> o;
            if (FAILED(a->EnumOutputs(0, &o))) continue;
            if (desc.VendorId == 0x10DE) { adapter = a; break; }
            if (!fallback) fallback = a;
        }
        if (!adapter) adapter = fallback;
        if (!adapter) throw std::runtime_error("no DXGI adapter with a display output");

        DXGI_ADAPTER_DESC1 adapterDesc{};
        adapter->GetDesc1(&adapterDesc);
        std::wprintf(L"Adapter: %ls\n", adapterDesc.Description);

        ComPtr<ID3D12Device> d3d12Device;
        ThrowIfFailed(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                        IID_PPV_ARGS(&d3d12Device)),
                      "D3D12CreateDevice");

        D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        ComPtr<ID3D11Device>        d3d11Device;
        ComPtr<ID3D11DeviceContext> d3d11Ctx;
        ThrowIfFailed(D3D11CreateDevice(
                          adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                          D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                          fls, _countof(fls), D3D11_SDK_VERSION,
                          &d3d11Device, nullptr, &d3d11Ctx),
                      "D3D11CreateDevice");

        ComPtr<ID3D11Device1>        d3d11Device1;
        ComPtr<ID3D11Device5>        d3d11Device5;
        ComPtr<ID3D11DeviceContext4> d3d11Ctx4;
        ThrowIfFailed(d3d11Device.As(&d3d11Device1), "QI ID3D11Device1");
        ThrowIfFailed(d3d11Device.As(&d3d11Device5), "QI ID3D11Device5");
        ThrowIfFailed(d3d11Ctx.As(&d3d11Ctx4),       "QI ID3D11DeviceContext4");

        // WinRT IDirect3DDevice wrapping our D3D11 device — this is what the
        // capture framepool is bound to.
        ComPtr<IDXGIDevice> dxgiDevice;
        ThrowIfFailed(d3d11Device.As(&dxgiDevice), "QI IDXGIDevice");
        winrt::com_ptr<::IInspectable> inspectable;
        ThrowIfFailed(CreateDirect3D11DeviceFromDXGIDevice(
                          dxgiDevice.Get(), inspectable.put()),
                      "CreateDirect3D11DeviceFromDXGIDevice");
        auto winrtDevice = inspectable.as<wgd11::IDirect3DDevice>();

        auto interopFactory = winrt::get_activation_factory<
            wgc::GraphicsCaptureItem, ::IGraphicsCaptureItemInterop>();
        wgc::GraphicsCaptureItem captureItem{ nullptr };
        winrt::check_hresult(interopFactory->CreateForWindow(
            target,
            winrt::guid_of<wgc::GraphicsCaptureItem>(),
            reinterpret_cast<void**>(winrt::put_abi(captureItem))));

        auto itemSize = captureItem.Size();
        if (itemSize.Width <= 0 || itemSize.Height <= 0) {
            RECT rc{};
            GetClientRect(target, &rc);
            itemSize.Width  = std::max<LONG>(1, rc.right - rc.left);
            itemSize.Height = std::max<LONG>(1, rc.bottom - rc.top);
        }
        UINT poolW = static_cast<UINT>(itemSize.Width);
        UINT poolH = static_cast<UINT>(itemSize.Height);
        std::wprintf(L"Window size: %ux%u\n", poolW, poolH);

        UINT cropX, cropY, cropW, cropH;
        ComputeCrop(target, poolW, poolH, cropX, cropY, cropW, cropH);
        std::wprintf(L"Initial content area: (%u,%u) %ux%u\n",
                     cropX, cropY, cropW, cropH);

        // Reserve an oversized shared texture so Chrome can resize freely
        // without us having to recreate the shared handle (that would break
        // any attached consumer). The monitor the window is on is a safe
        // upper bound for any single window on that display.
        HMONITOR hMon = MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{}; mi.cbSize = sizeof(mi);
        GetMonitorInfoW(hMon, &mi);
        const UINT texW = std::max<UINT>(cropW,
            static_cast<UINT>(mi.rcMonitor.right  - mi.rcMonitor.left));
        const UINT texH = std::max<UINT>(cropH,
            static_cast<UINT>(mi.rcMonitor.bottom - mi.rcMonitor.top));
        std::wprintf(L"Shared texture: %ux%u (reserved for resize headroom)\n",
                     texW, texH);

        // Shared D3D12 texture — cross-adapter so a sender on a different GPU
        // (e.g. NVIDIA dGPU on a hybrid laptop whose panel is on the iGPU) can
        // open it for NVENC. Cross-adapter requires row-major layout and
        // forbids RENDER_TARGET/SIMULTANEOUS_ACCESS; this texture is only ever
        // a CopyResource destination so that's fine.
        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC texDesc{};
        texDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width            = texW;
        texDesc.Height           = texH;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels        = 1;
        texDesc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        texDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;

        ComPtr<ID3D12Resource> sharedTex;
        ThrowIfFailed(d3d12Device->CreateCommittedResource(
                          &heapProps,
                          D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER,
                          &texDesc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                          IID_PPV_ARGS(&sharedTex)),
                      "CreateCommittedResource(sharedTex)");

        HANDLE hTexture = nullptr;
        ThrowIfFailed(d3d12Device->CreateSharedHandle(
                          sharedTex.Get(), nullptr, GENERIC_ALL,
                          kTextureName, &hTexture),
                      "CreateSharedHandle(tex)");

        ComPtr<ID3D11Texture2D> sharedTexD3D11;
        ThrowIfFailed(d3d11Device1->OpenSharedResource1(
                          hTexture, IID_PPV_ARGS(&sharedTexD3D11)),
                      "OpenSharedResource1");

        ComPtr<ID3D12Fence> d3d12Fence;
        ThrowIfFailed(d3d12Device->CreateFence(
                          0,
                          D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER,
                          IID_PPV_ARGS(&d3d12Fence)),
                      "CreateFence");
        HANDLE hFence = nullptr;
        ThrowIfFailed(d3d12Device->CreateSharedHandle(
                          d3d12Fence.Get(), nullptr, GENERIC_ALL,
                          kFenceName, &hFence),
                      "CreateSharedHandle(fence)");
        ComPtr<ID3D11Fence> d3d11Fence;
        ThrowIfFailed(d3d11Device5->OpenSharedFence(
                          hFence, IID_PPV_ARGS(&d3d11Fence)),
                      "OpenSharedFence");
        std::atomic<UINT64> fenceValue{ 0 };

        HANDLE hMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                         PAGE_READWRITE, 0,
                                         sizeof(SharedInfo), kInfoMapName);
        if (!hMap) throw std::runtime_error("CreateFileMappingW");
        auto* info = static_cast<SharedInfo*>(
            MapViewOfFile(hMap, FILE_MAP_WRITE, 0, 0, sizeof(SharedInfo)));
        if (!info) throw std::runtime_error("MapViewOfFile");
        info->width      = cropW;
        info->height     = cropH;
        info->format     = DXGI_FORMAT_B8G8R8A8_UNORM;
        info->frameCount = 0;
        info->fenceValue = 0;

        // Free-threaded pool so FrameArrived fires on the system thread pool
        // and we do not have to pump a message loop on the main thread.
        auto pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            winrtDevice,
            wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            2,
            itemSize);

        // The D3D11 immediate context is not thread-safe; FreeThreaded events
        // are serialized per subscription, but the lock also guards against
        // the main thread's teardown path touching the context.
        std::mutex ctxMutex;

        OverlayCtx overlayCtx;
        overlayCtx.target  = target;
        overlayCtx.running = &g_running;
        overlayCtx.cropX = cropX; overlayCtx.cropY = cropY;
        overlayCtx.cropW = cropW; overlayCtx.cropH = cropH;

        // Recompute the crop and publish if it changed (to overlay + info map).
        // Used on both the resize path and every frame — the latter catches
        // layout changes that don't change the outer window size, like the
        // bookmarks-bar toggle or a fullscreen transition where the outer
        // size settles before the inner HWND layout does.
        auto applyCrop = [&](UINT nx, UINT ny, UINT nw, UINT nh) {
            if (nx >= texW) nw = 0;
            else if (nx + nw > texW) nw = texW - nx;
            if (ny >= texH) nh = 0;
            else if (ny + nh > texH) nh = texH - ny;
            if (nx == cropX && ny == cropY && nw == cropW && nh == cropH) return;
            cropX = nx; cropY = ny; cropW = nw; cropH = nh;
            info->width  = cropW;
            info->height = cropH;
            std::lock_guard<std::mutex> lk(overlayCtx.mtx);
            overlayCtx.cropX = cropX; overlayCtx.cropY = cropY;
            overlayCtx.cropW = cropW; overlayCtx.cropH = cropH;
        };

        auto frameToken = pool.FrameArrived(
            [&, info](
                wgc::Direct3D11CaptureFramePool const& sender,
                winrt::Windows::Foundation::IInspectable const&) {
                auto frame = sender.TryGetNextFrame();
                if (!frame) return;
                auto contentSize = frame.ContentSize();

                // Resize path — Chrome changed size, so tell the pool to
                // allocate new-sized frame textures and recompute the crop.
                // The current frame's surface is still the old size, skip it.
                if (static_cast<UINT>(contentSize.Width)  != poolW ||
                    static_cast<UINT>(contentSize.Height) != poolH) {
                    if (contentSize.Width  <= 0) contentSize.Width  = 1;
                    if (contentSize.Height <= 0) contentSize.Height = 1;
                    poolW = static_cast<UINT>(contentSize.Width);
                    poolH = static_cast<UINT>(contentSize.Height);
                    sender.Recreate(winrtDevice,
                        wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                        2, contentSize);

                    UINT nx, ny, nw, nh;
                    ComputeCrop(target, poolW, poolH, nx, ny, nw, nh);
                    applyCrop(nx, ny, nw, nh);
                    return;
                }

                // Steady state — recompute crop so internal layout changes
                // (bookmarks bar toggle, fullscreen UI chrome settling on a
                // frame delay, etc.) get picked up even without a resize.
                {
                    UINT nx, ny, nw, nh;
                    ComputeCrop(target, poolW, poolH, nx, ny, nw, nh);
                    applyCrop(nx, ny, nw, nh);
                }

                auto surface = frame.Surface();
                auto access  = surface.as<IDxgiInterfaceAccess>();
                ComPtr<ID3D11Texture2D> srcTex;
                if (FAILED(access->GetInterface(IID_PPV_ARGS(&srcTex)))) return;

                D3D11_TEXTURE2D_DESC sd{};
                srcTex->GetDesc(&sd);

                const UINT x2 = std::min<UINT>(cropX + cropW, sd.Width);
                const UINT y2 = std::min<UINT>(cropY + cropH, sd.Height);
                if (cropX >= x2 || cropY >= y2) return;

                D3D11_BOX box{};
                box.left   = cropX;
                box.top    = cropY;
                box.right  = x2;
                box.bottom = y2;
                box.front  = 0;
                box.back   = 1;

                std::lock_guard<std::mutex> lock(ctxMutex);
                d3d11Ctx->CopySubresourceRegion(
                    sharedTexD3D11.Get(), 0, 0, 0, 0,
                    srcTex.Get(), 0, &box);

                const UINT64 v = ++fenceValue;
                d3d11Ctx4->Signal(d3d11Fence.Get(), v);
                d3d11Ctx->Flush();
                info->frameCount++;
                info->fenceValue = v;
            });

        auto closedToken = captureItem.Closed(
            [&](wgc::GraphicsCaptureItem const&,
                winrt::Windows::Foundation::IInspectable const&) {
                g_running = false;
            });

        auto session = pool.CreateCaptureSession(captureItem);

        // Ask Windows to drop its yellow "being captured" frame. Requires
        // user consent the first time on Windows 11 22H2+; silently ignored
        // on older builds.
        try {
            namespace appcap = winrt::Windows::Security::Authorization::AppCapabilityAccess;
            auto status = wgc::GraphicsCaptureAccess::RequestAccessAsync(
                wgc::GraphicsCaptureAccessKind::Borderless).get();
            if (status == appcap::AppCapabilityAccessStatus::Allowed) {
                session.IsBorderRequired(false);
                std::wprintf(L"Yellow capture border suppressed.\n");
            } else {
                std::wprintf(L"Borderless capture not granted (status=%d).\n",
                             static_cast<int>(status));
            }
        } catch (winrt::hresult_error const& e) {
            std::wprintf(L"Borderless request unavailable: 0x%08lX\n",
                         static_cast<unsigned long>(e.code()));
        }

        session.StartCapture();

        std::thread overlayThread([&overlayCtx] { RunOverlay(&overlayCtx); });

        std::wprintf(
            L"Shared texture: %ls\n"
            L"Shared fence:   %ls\n"
            L"Shared info:    %ls\n"
            L"Capturing - Ctrl+C to stop.\n",
            kTextureName, kFenceName, kInfoMapName);

        while (g_running) Sleep(100);

        if (overlayThread.joinable()) overlayThread.join();

        session.Close();
        pool.FrameArrived(frameToken);
        captureItem.Closed(closedToken);
        pool.Close();

        UnmapViewOfFile(info);
        CloseHandle(hMap);
        CloseHandle(hFence);
        CloseHandle(hTexture);
    } catch (winrt::hresult_error const& e) {
        std::fwprintf(stderr, L"FATAL (WinRT): 0x%08lX %ls\n",
                      static_cast<unsigned long>(e.code()), e.message().c_str());
        return 1;
    } catch (std::exception const& e) {
        std::fprintf(stderr, "FATAL: %s\n", e.what());
        return 1;
    }
    return 0;
}
