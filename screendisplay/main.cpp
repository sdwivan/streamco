// screendisplay — 840x460 window that shows the shared D3D12 texture produced
// by the screencapture app. Opens the shared texture/fence by name, waits on
// the fence each frame, and draws the texture via a fullscreen-triangle PSO.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdio>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kWindowWidth  = 840;
constexpr UINT kWindowHeight = 460;
constexpr UINT kFrameCount   = 2;

struct SharedInfo {
    UINT32 width;
    UINT32 height;
    UINT32 format;
    UINT32 frameCount;
    UINT64 fenceValue;
};

void ThrowIfFailed(HRESULT hr, const char* where) {
    if (FAILED(hr)) {
        char msg[256];
        std::snprintf(msg, sizeof(msg), "%s failed: 0x%08lX",
                      where, static_cast<unsigned long>(hr));
        throw std::runtime_error(msg);
    }
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hWnd, msg, wp, lp);
}

const char kShaderSource[] = R"(
Texture2D    g_tex : register(t0);
SamplerState g_smp : register(s0);

// Producers may reserve a larger shared texture than the current valid
// content (so the handle can stay stable across window resizes). uvScale
// clamps sampling to the valid top-left region reported in SharedInfo.
cbuffer Params : register(b0) { float2 uvScale; };

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv  = uv;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float4 PSMain(VSOut i) : SV_Target {
    return g_tex.Sample(g_smp, i.uv * uvScale);
}
)";

} // namespace

int main(int argc, char** argv) {
    std::wstring prefix = L"streamco_shared";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if ((a == "-n" || a == "--name") && i + 1 < argc) {
            std::string p = argv[++i];
            prefix.assign(p.begin(), p.end());
        } else if (a == "-h" || a == "--help") {
            std::puts("screendisplay [-n <prefix>]   (default prefix: streamco_shared)");
            return 0;
        }
    }
    const std::wstring texNameS   = prefix + L"_texture";
    const std::wstring fenceNameS = prefix + L"_fence";
    const std::wstring infoNameS  = prefix + L"_info";
    const wchar_t* kTextureName = texNameS.c_str();
    const wchar_t* kFenceName   = fenceNameS.c_str();
    const wchar_t* kInfoMapName = infoNameS.c_str();

    try {
        HINSTANCE hInst = GetModuleHandleW(nullptr);

        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = WndProc;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"StreamcoDisplay";
        RegisterClassExW(&wc);

        const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        RECT r{ 0, 0, (LONG)kWindowWidth, (LONG)kWindowHeight };
        AdjustWindowRect(&r, style, FALSE);
        HWND hWnd = CreateWindowExW(0, wc.lpszClassName, L"streamco display",
                                    style | WS_VISIBLE,
                                    CW_USEDEFAULT, CW_USEDEFAULT,
                                    r.right - r.left, r.bottom - r.top,
                                    nullptr, nullptr, hInst, nullptr);
        if (!hWnd) throw std::runtime_error("CreateWindowExW");

        ComPtr<IDXGIFactory6> factory;
        ThrowIfFailed(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)),
                      "CreateDXGIFactory2");

        // Prefer NVIDIA so we can open shared textures produced by a consumer
        // on the NVIDIA dGPU (e.g. streamreceiver binds to NVIDIA for NVDEC/CUDA
        // interop). On hybrid laptops where the panel is on the iGPU, DXGI
        // flip-model presents from the dGPU are composited to the iGPU's
        // display by the OS, so picking NVIDIA here is safe even without a
        // direct display output. Fall back to first-with-output otherwise.
        ComPtr<IDXGIAdapter1> adapter;
        ComPtr<IDXGIAdapter1> fallback;
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> a;
            if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 desc{};
            a->GetDesc1(&desc);
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            if (desc.VendorId == 0x10DE) { adapter = a; break; }
            if (!fallback) {
                ComPtr<IDXGIOutput> o;
                if (SUCCEEDED(a->EnumOutputs(0, &o))) fallback = a;
            }
        }
        if (!adapter) adapter = fallback;
        if (!adapter) throw std::runtime_error("no suitable adapter");

        DXGI_ADAPTER_DESC1 adapterDesc{};
        adapter->GetDesc1(&adapterDesc);
        std::wprintf(L"Adapter: %ls\n", adapterDesc.Description);

        ComPtr<ID3D12Device> device;
        ThrowIfFailed(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                        IID_PPV_ARGS(&device)),
                      "D3D12CreateDevice");

        D3D12_COMMAND_QUEUE_DESC qdesc{};
        qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ComPtr<ID3D12CommandQueue> queue;
        ThrowIfFailed(device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&queue)),
                      "CreateCommandQueue");

        DXGI_SWAP_CHAIN_DESC1 scd{};
        scd.Width            = kWindowWidth;
        scd.Height           = kWindowHeight;
        scd.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
        scd.BufferCount      = kFrameCount;
        scd.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scd.SampleDesc.Count = 1;
        ComPtr<IDXGISwapChain1> sc1;
        ThrowIfFailed(factory->CreateSwapChainForHwnd(
                          queue.Get(), hWnd, &scd, nullptr, nullptr, &sc1),
                      "CreateSwapChainForHwnd");
        factory->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER);
        ComPtr<IDXGISwapChain3> swapChain;
        ThrowIfFailed(sc1.As(&swapChain), "QI IDXGISwapChain3");

        // RTVs for the swapchain back buffers.
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
        rtvHeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.NumDescriptors = kFrameCount;
        ComPtr<ID3D12DescriptorHeap> rtvHeap;
        ThrowIfFailed(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap)),
                      "CreateDescriptorHeap(RTV)");
        const UINT rtvInc = device->GetDescriptorHandleIncrementSize(
                                D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        ComPtr<ID3D12Resource> backBuffers[kFrameCount];
        {
            D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
            for (UINT i = 0; i < kFrameCount; ++i) {
                ThrowIfFailed(swapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffers[i])),
                              "GetBuffer");
                device->CreateRenderTargetView(backBuffers[i].Get(), nullptr, h);
                h.ptr += rtvInc;
            }
        }

        // Shader-visible SRV heap (1 entry for the shared texture).
        D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
        srvHeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvHeapDesc.NumDescriptors = 1;
        srvHeapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ComPtr<ID3D12DescriptorHeap> srvHeap;
        ThrowIfFailed(device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&srvHeap)),
                      "CreateDescriptorHeap(SRV)");

        // Root sig: 1 descriptor table (SRV), 1 set of 32-bit constants
        // (uvScale — float2), plus 1 static linear-clamp sampler.
        D3D12_DESCRIPTOR_RANGE1 range{};
        range.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors     = 1;
        range.BaseShaderRegister = 0;
        range.Flags              = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC;

        D3D12_ROOT_PARAMETER1 params[2]{};
        params[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 1;
        params[0].DescriptorTable.pDescriptorRanges   = &range;
        params[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        params[1].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants.ShaderRegister = 0;
        params[1].Constants.RegisterSpace  = 0;
        params[1].Constants.Num32BitValues = 2;  // float2 uvScale
        params[1].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.ShaderRegister   = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.Version                    = D3D_ROOT_SIGNATURE_VERSION_1_1;
        rsDesc.Desc_1_1.NumParameters     = 2;
        rsDesc.Desc_1_1.pParameters       = params;
        rsDesc.Desc_1_1.NumStaticSamplers = 1;
        rsDesc.Desc_1_1.pStaticSamplers   = &sampler;
        rsDesc.Desc_1_1.Flags =
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> rsBlob, rsErr;
        if (FAILED(D3D12SerializeVersionedRootSignature(&rsDesc, &rsBlob, &rsErr))) {
            throw std::runtime_error(rsErr
                ? static_cast<const char*>(rsErr->GetBufferPointer())
                : "SerializeVersionedRootSignature");
        }
        ComPtr<ID3D12RootSignature> rootSig;
        ThrowIfFailed(device->CreateRootSignature(
                          0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                          IID_PPV_ARGS(&rootSig)),
                      "CreateRootSignature");

        ComPtr<ID3DBlob> vs, ps, err;
        if (FAILED(D3DCompile(kShaderSource, sizeof(kShaderSource), nullptr, nullptr,
                              nullptr, "VSMain", "vs_5_0", 0, 0, &vs, &err))) {
            throw std::runtime_error(err ? (const char*)err->GetBufferPointer()
                                         : "D3DCompile VS");
        }
        if (FAILED(D3DCompile(kShaderSource, sizeof(kShaderSource), nullptr, nullptr,
                              nullptr, "PSMain", "ps_5_0", 0, 0, &ps, &err))) {
            throw std::runtime_error(err ? (const char*)err->GetBufferPointer()
                                         : "D3DCompile PS");
        }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = rootSig.Get();
        psoDesc.VS             = { vs->GetBufferPointer(), vs->GetBufferSize() };
        psoDesc.PS             = { ps->GetBufferPointer(), ps->GetBufferSize() };
        psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask =
            D3D12_COLOR_WRITE_ENABLE_ALL;
        psoDesc.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
        psoDesc.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
        psoDesc.DepthStencilState.DepthEnable   = FALSE;
        psoDesc.DepthStencilState.StencilEnable = FALSE;
        psoDesc.SampleMask                      = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets                = 1;
        psoDesc.RTVFormats[0]                   = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoDesc.SampleDesc.Count                = 1;
        ComPtr<ID3D12PipelineState> pso;
        ThrowIfFailed(device->CreateGraphicsPipelineState(
                          &psoDesc, IID_PPV_ARGS(&pso)),
                      "CreateGraphicsPipelineState");

        ComPtr<ID3D12CommandAllocator> cmdAlloc[kFrameCount];
        for (UINT i = 0; i < kFrameCount; ++i) {
            ThrowIfFailed(device->CreateCommandAllocator(
                              D3D12_COMMAND_LIST_TYPE_DIRECT,
                              IID_PPV_ARGS(&cmdAlloc[i])),
                          "CreateCommandAllocator");
        }
        ComPtr<ID3D12GraphicsCommandList> cmdList;
        ThrowIfFailed(device->CreateCommandList(
                          0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                          cmdAlloc[0].Get(), nullptr, IID_PPV_ARGS(&cmdList)),
                      "CreateCommandList");
        cmdList->Close();

        ComPtr<ID3D12Fence> frameFence;
        ThrowIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                          IID_PPV_ARGS(&frameFence)),
                      "CreateFence");
        UINT64 frameFenceVal = 0;
        UINT64 perFrameVal[kFrameCount] = { 0, 0 };
        HANDLE frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!frameEvent) throw std::runtime_error("CreateEvent");

        // Wait for screencapture to be up, then open the shared texture, the
        // shared fence and the info mapping.
        std::wprintf(L"Waiting for screencapture (%ls)...\n", kTextureName);
        ComPtr<ID3D12Resource> sharedTex;
        for (;;) {
            HANDLE h = nullptr;
            if (SUCCEEDED(device->OpenSharedHandleByName(
                    kTextureName, GENERIC_ALL, &h))) {
                HRESULT hr = device->OpenSharedHandle(h, IID_PPV_ARGS(&sharedTex));
                CloseHandle(h);
                if (SUCCEEDED(hr)) break;
            }
            Sleep(200);
        }
        ComPtr<ID3D12Fence> sharedFence;
        for (;;) {
            HANDLE h = nullptr;
            if (SUCCEEDED(device->OpenSharedHandleByName(
                    kFenceName, GENERIC_ALL, &h))) {
                HRESULT hr = device->OpenSharedHandle(h, IID_PPV_ARGS(&sharedFence));
                CloseHandle(h);
                if (SUCCEEDED(hr)) break;
            }
            Sleep(200);
        }
        HANDLE hInfoMap = nullptr;
        for (;;) {
            hInfoMap = OpenFileMappingW(FILE_MAP_READ, FALSE, kInfoMapName);
            if (hInfoMap) break;
            Sleep(200);
        }
        auto* info = static_cast<const SharedInfo*>(
            MapViewOfFile(hInfoMap, FILE_MAP_READ, 0, 0, sizeof(SharedInfo)));
        if (!info) throw std::runtime_error("MapViewOfFile");

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format                    = static_cast<DXGI_FORMAT>(info->format);
        srvDesc.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels       = 1;
        device->CreateShaderResourceView(
            sharedTex.Get(), &srvDesc,
            srvHeap->GetCPUDescriptorHandleForHeapStart());

        // The producer may have reserved a larger texture than the current
        // valid content (to absorb resizes without recreating the handle);
        // we sample only the info->width x info->height top-left region.
        const D3D12_RESOURCE_DESC texDesc = sharedTex->GetDesc();
        const float texPxW = static_cast<float>(texDesc.Width);
        const float texPxH = static_cast<float>(texDesc.Height);

        std::wprintf(L"Connected: valid %ux%u within %ux%u (format=%u)\n",
                     info->width, info->height,
                     static_cast<UINT>(texDesc.Width),
                     texDesc.Height, info->format);

        D3D12_VIEWPORT vp{ 0, 0, (float)kWindowWidth, (float)kWindowHeight, 0, 1 };
        D3D12_RECT     scissor{ 0, 0, (LONG)kWindowWidth, (LONG)kWindowHeight };

        UINT frameIndex = swapChain->GetCurrentBackBufferIndex();

        MSG  msg{};
        bool running = true;
        while (running) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) { running = false; break; }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (!running) break;

            if (frameFence->GetCompletedValue() < perFrameVal[frameIndex]) {
                ThrowIfFailed(frameFence->SetEventOnCompletion(
                                  perFrameVal[frameIndex], frameEvent),
                              "SetEventOnCompletion");
                WaitForSingleObject(frameEvent, INFINITE);
            }

            // GPU-side wait on the producer fence so we only sample a frame
            // that has finished being written.
            const UINT64 latest = info->fenceValue;
            if (latest > 0) {
                ThrowIfFailed(queue->Wait(sharedFence.Get(), latest),
                              "queue->Wait(sharedFence)");
            }

            cmdAlloc[frameIndex]->Reset();
            cmdList->Reset(cmdAlloc[frameIndex].Get(), pso.Get());

            cmdList->SetGraphicsRootSignature(rootSig.Get());
            ID3D12DescriptorHeap* heaps[] = { srvHeap.Get() };
            cmdList->SetDescriptorHeaps(1, heaps);
            cmdList->SetGraphicsRootDescriptorTable(
                0, srvHeap->GetGPUDescriptorHandleForHeapStart());

            const float uvScale[2] = {
                static_cast<float>(info->width)  / texPxW,
                static_cast<float>(info->height) / texPxH,
            };
            cmdList->SetGraphicsRoot32BitConstants(1, 2, uvScale, 0);

            cmdList->RSSetViewports(1, &vp);
            cmdList->RSSetScissorRects(1, &scissor);

            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource   = backBuffers[frameIndex].Get();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
            cmdList->ResourceBarrier(1, &barrier);

            D3D12_CPU_DESCRIPTOR_HANDLE rtv =
                rtvHeap->GetCPUDescriptorHandleForHeapStart();
            rtv.ptr += frameIndex * rtvInc;
            cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            cmdList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);

            if (latest > 0) {
                cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                cmdList->DrawInstanced(3, 1, 0, 0);
            }

            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
            cmdList->ResourceBarrier(1, &barrier);

            cmdList->Close();
            ID3D12CommandList* lists[] = { cmdList.Get() };
            queue->ExecuteCommandLists(1, lists);

            swapChain->Present(1, 0);

            ++frameFenceVal;
            ThrowIfFailed(queue->Signal(frameFence.Get(), frameFenceVal),
                          "queue->Signal(frameFence)");
            perFrameVal[frameIndex] = frameFenceVal;

            frameIndex = swapChain->GetCurrentBackBufferIndex();
        }

        // Drain GPU before teardown.
        ++frameFenceVal;
        queue->Signal(frameFence.Get(), frameFenceVal);
        if (frameFence->GetCompletedValue() < frameFenceVal) {
            frameFence->SetEventOnCompletion(frameFenceVal, frameEvent);
            WaitForSingleObject(frameEvent, INFINITE);
        }
        CloseHandle(frameEvent);

        UnmapViewOfFile(info);
        CloseHandle(hInfoMap);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FATAL: %s\n", e.what());
        MessageBoxA(nullptr, e.what(), "screendisplay FATAL", MB_ICONERROR);
        return 1;
    }
    return 0;
}
