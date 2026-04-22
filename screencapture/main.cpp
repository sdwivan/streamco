// screencapture — continuously captures the primary desktop and publishes it
// into a D3D12 shared texture that another process can open.
//
// Sharing contract (consumer side):
//   1. ID3D12Device::OpenSharedHandleByName(L"streamco_shared_texture", GENERIC_ALL, &h)
//      then OpenSharedHandle(h, IID_PPV_ARGS(&texture))
//   2. ID3D12Device::OpenSharedHandleByName(L"streamco_shared_fence",   GENERIC_ALL, &h)
//      then OpenSharedHandle(h, IID_PPV_ARGS(&fence))
//   3. OpenFileMappingW + MapViewOfFile on L"streamco_shared_info" to read the
//      SharedInfo struct (width / height / DXGI format / frameCount / fenceValue).
//      Consumer waits for fence >= last seen fenceValue before reading.
//
// Implementation: Desktop Duplication is run on a plain D3D11 device (it does
// not work on a D3D11-on-12 device — DuplicateOutput succeeds but AcquireNextFrame
// returns DXGI_ERROR_INVALID_CALL). The shared resource lives on a D3D12 device
// on the same adapter; the D3D11 side opens it via OpenSharedResource1 so the
// duplication frame can be copied into the D3D12 resource directly.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {

struct SharedInfo {
    UINT32 width;
    UINT32 height;
    UINT32 format;      // DXGI_FORMAT
    UINT32 frameCount;
    UINT64 fenceValue;  // latest value signaled on the shared fence
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

} // namespace

int main(int argc, char** argv) {
    std::wstring prefix = L"streamco_shared";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if ((a == "-n" || a == "--name") && i + 1 < argc) {
            std::string p = argv[++i];
            prefix.assign(p.begin(), p.end());
        } else if (a == "-h" || a == "--help") {
            std::puts("screencapture [-n <prefix>]   (default prefix: streamco_shared)");
            return 0;
        }
    }
    const std::wstring texNameS   = prefix + L"_texture";
    const std::wstring fenceNameS = prefix + L"_fence";
    const std::wstring infoNameS  = prefix + L"_info";
    const wchar_t* kTextureName = texNameS.c_str();
    const wchar_t* kFenceName   = fenceNameS.c_str();
    const wchar_t* kInfoMapName = infoNameS.c_str();

    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    try {
        ComPtr<IDXGIFactory6> factory;
        ThrowIfFailed(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)),
                      "CreateDXGIFactory2");

        // Prefer an NVIDIA adapter so the shared texture lives on the same GPU
        // as the downstream NVENC sender. Fall back to first-with-output when
        // no NVIDIA GPU is present (e.g. AMD/Intel-only box). Both candidates
        // must actually own a display output, otherwise DuplicateOutput fails.
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

        // D3D12 device — owns the shared texture + shared fence.
        ComPtr<ID3D12Device> d3d12Device;
        ThrowIfFailed(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                        IID_PPV_ARGS(&d3d12Device)),
                      "D3D12CreateDevice");

        // Plain D3D11 device — used by Desktop Duplication.
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

        // Desktop Duplication.
        ComPtr<IDXGIOutput> output;
        ThrowIfFailed(adapter->EnumOutputs(0, &output), "EnumOutputs(0)");
        ComPtr<IDXGIOutput1> output1;
        ThrowIfFailed(output.As(&output1), "QI IDXGIOutput1");

        ComPtr<IDXGIOutputDuplication> duplication;
        ThrowIfFailed(output1->DuplicateOutput(d3d11Device.Get(), &duplication),
                      "IDXGIOutput1::DuplicateOutput");

        DXGI_OUTDUPL_DESC dupDesc{};
        duplication->GetDesc(&dupDesc);
        const UINT        width  = dupDesc.ModeDesc.Width;
        const UINT        height = dupDesc.ModeDesc.Height;
        const DXGI_FORMAT format = dupDesc.ModeDesc.Format != DXGI_FORMAT_UNKNOWN
                                       ? dupDesc.ModeDesc.Format
                                       : DXGI_FORMAT_B8G8R8A8_UNORM;

        std::wprintf(L"Desktop: %ux%u  format=%u\n",
                     width, height, static_cast<unsigned>(format));

        // Shared D3D12 texture — cross-adapter so a sender on a different GPU
        // (e.g. NVIDIA dGPU on a hybrid laptop whose panel is on the iGPU) can
        // open it for NVENC. Cross-adapter requires row-major layout and
        // forbids RENDER_TARGET/SIMULTANEOUS_ACCESS; this texture is only ever
        // a CopyResource destination so that's fine. The fence enforces
        // producer→consumer ordering.
        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC texDesc{};
        texDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width            = width;
        texDesc.Height           = height;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels        = 1;
        texDesc.Format           = format;
        texDesc.SampleDesc.Count = 1;
        texDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        texDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;

        ComPtr<ID3D12Resource> sharedTex;
        ThrowIfFailed(d3d12Device->CreateCommittedResource(
                          &heapProps,
                          D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER,
                          &texDesc, D3D12_RESOURCE_STATE_COMMON,
                          nullptr, IID_PPV_ARGS(&sharedTex)),
                      "CreateCommittedResource(sharedTex)");

        HANDLE hTexture = nullptr;
        ThrowIfFailed(d3d12Device->CreateSharedHandle(
                          sharedTex.Get(), nullptr, GENERIC_ALL,
                          kTextureName, &hTexture),
                      "CreateSharedHandle(tex)");

        // Open the same D3D12 resource on the D3D11 side so CopyResource can
        // write duplication frames straight into it.
        ComPtr<ID3D11Texture2D> sharedTexD3D11;
        ThrowIfFailed(d3d11Device1->OpenSharedResource1(
                          hTexture, IID_PPV_ARGS(&sharedTexD3D11)),
                      "ID3D11Device1::OpenSharedResource1");

        // Shared D3D12 fence, also opened on the D3D11 side so we can signal
        // it from the duplication thread after each copy.
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
                      "ID3D11Device5::OpenSharedFence");
        UINT64 fenceValue = 0;

        HANDLE hMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                         PAGE_READWRITE, 0,
                                         sizeof(SharedInfo), kInfoMapName);
        if (!hMap) throw std::runtime_error("CreateFileMappingW");
        auto* info = static_cast<SharedInfo*>(
            MapViewOfFile(hMap, FILE_MAP_WRITE, 0, 0, sizeof(SharedInfo)));
        if (!info) throw std::runtime_error("MapViewOfFile");

        info->width      = width;
        info->height     = height;
        info->format     = static_cast<UINT32>(format);
        info->frameCount = 0;
        info->fenceValue = 0;

        std::wprintf(
            L"Shared texture: %ls\n"
            L"Shared fence:   %ls\n"
            L"Shared info:    %ls\n"
            L"Capturing - Ctrl+C to stop.\n",
            kTextureName, kFenceName, kInfoMapName);

        while (g_running) {
            ComPtr<IDXGIResource>   desktopRes;
            DXGI_OUTDUPL_FRAME_INFO frameInfo{};
            HRESULT hr = duplication->AcquireNextFrame(500, &frameInfo, &desktopRes);
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
            if (hr == DXGI_ERROR_ACCESS_LOST) {
                duplication.Reset();
                hr = output1->DuplicateOutput(d3d11Device.Get(), &duplication);
                if (FAILED(hr)) { Sleep(100); continue; }
                continue;
            }
            ThrowIfFailed(hr, "AcquireNextFrame");

            ComPtr<ID3D11Texture2D> desktopTex;
            ThrowIfFailed(desktopRes.As(&desktopTex), "QI ID3D11Texture2D");

            d3d11Ctx->CopyResource(sharedTexD3D11.Get(), desktopTex.Get());

            ++fenceValue;
            ThrowIfFailed(d3d11Ctx4->Signal(d3d11Fence.Get(), fenceValue),
                          "ID3D11DeviceContext4::Signal");
            d3d11Ctx->Flush();

            info->frameCount++;
            info->fenceValue = fenceValue;

            duplication->ReleaseFrame();
        }

        UnmapViewOfFile(info);
        CloseHandle(hMap);
        CloseHandle(hFence);
        CloseHandle(hTexture);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FATAL: %s\n", e.what());
        return 1;
    }

    return 0;
}
