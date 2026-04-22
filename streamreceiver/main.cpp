// streamreceiver — receives UDP packets from streamsender, reassembles
// encoded frames, decodes via NVDEC, converts NV12 → BGRA with a CUDA
// kernel that writes into a D3D12 shared texture (imported as CUDA
// external memory), and signals a shared D3D12 fence via an imported
// external semaphore. Publishes the standard streamco_shared_* contract
// so screendisplay attaches to the decoded stream unchanged.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cuda.h>
#include <cuda_runtime.h>

#include "NvDecoder/NvDecoder.h"
#include "Logger.h"
#include "../streamnet_protocol.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

using Microsoft::WRL::ComPtr;

simplelogger::Logger* logger = simplelogger::LoggerFactory::CreateConsoleLogger();

// Implemented in colorconv.cu.
extern "C" cudaError_t launchNv12ToBgra(
    const uint8_t* y, int yPitch,
    const uint8_t* uv, int uvPitch,
    unsigned long long dstSurf,
    int dstX, int dstY,
    int width, int height,
    cudaStream_t stream);

namespace {

struct SharedInfo {
    UINT32 width;
    UINT32 height;
    UINT32 format;
    UINT32 frameCount;
    UINT64 fenceValue;
};

void ThrowIfFailed(HRESULT hr, const char* where) {
    if (FAILED(hr)) {
        char m[256];
        std::snprintf(m, sizeof(m), "%s failed: 0x%08lX",
                      where, static_cast<unsigned long>(hr));
        throw std::runtime_error(m);
    }
}

void CUCheck(CUresult r, const char* where) {
    if (r != CUDA_SUCCESS) {
        const char* msg = nullptr;
        cuGetErrorName(r, &msg);
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s failed: %s",
                      where, msg ? msg : "unknown");
        throw std::runtime_error(buf);
    }
}

std::atomic<bool> g_running{ true };
BOOL WINAPI ConsoleHandler(DWORD sig) {
    if (sig == CTRL_C_EVENT || sig == CTRL_BREAK_EVENT || sig == CTRL_CLOSE_EVENT) {
        g_running = false;
        return TRUE;
    }
    return FALSE;
}

struct Args {
    int  listenPort = 50000;
    UINT maxW       = 3840;
    UINT maxH       = 2160;
    std::wstring prefix = L"streamco_shared";
};

Args ParseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        auto next = [&](std::string const& flag) -> const char* {
            if (++i >= argc) throw std::runtime_error("missing value for " + flag);
            return argv[i];
        };
        if (s == "-p") a.listenPort = std::atoi(next(s));
        else if (s == "-maxw") a.maxW = (UINT)std::atoi(next(s));
        else if (s == "-maxh") a.maxH = (UINT)std::atoi(next(s));
        else if (s == "-n" || s == "--name") {
            std::string p = next(s);
            a.prefix.assign(p.begin(), p.end());
        }
        else if (s == "-h" || s == "--help") {
            std::puts("streamreceiver [-p port] [-maxw W] [-maxh H] [-n prefix]");
            std::exit(0);
        }
    }
    return a;
}

// Enumerates local IPv4 addresses on up adapters and prints them with the
// listening port. The socket itself is bound to INADDR_ANY, so any of these
// addresses (plus loopback) will receive packets.
void PrintListeningAddresses(int port) {
    ULONG bufSize = 0;
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST
                      | GAA_FLAG_SKIP_DNS_SERVER;
    GetAdaptersAddresses(AF_INET, flags, nullptr, nullptr, &bufSize);
    if (!bufSize) { std::printf("  0.0.0.0:%d\n", port); return; }

    std::vector<char> buf(bufSize);
    auto* first = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, first, &bufSize) != NO_ERROR) {
        std::printf("  0.0.0.0:%d\n", port);
        return;
    }

    bool any = false;
    for (auto* a = first; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr
                || u->Address.lpSockaddr->sa_family != AF_INET) continue;
            auto* sin = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
            char ip[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
            std::printf("  %s:%d  (%ls)\n", ip, port,
                        a->FriendlyName ? a->FriendlyName : L"");
            any = true;
        }
    }
    if (!any) std::printf("  0.0.0.0:%d\n", port);
}

struct FrameAssembly {
    uint32_t             frameSize    = 0;
    uint16_t             totalPackets = 0;
    uint16_t             recvCount    = 0;
    uint32_t             width        = 0;
    uint32_t             height       = 0;
    uint32_t             flags        = 0;
    std::vector<uint8_t> bits;
    std::vector<uint8_t> have;  // 0/1 per packet
};

} // namespace

int main(int argc, char** argv) {
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
    Args args = ParseArgs(argc, argv);

    const std::wstring texNameS   = args.prefix + L"_texture";
    const std::wstring fenceNameS = args.prefix + L"_fence";
    const std::wstring infoNameS  = args.prefix + L"_info";
    const wchar_t* kTextureName = texNameS.c_str();
    const wchar_t* kFenceName   = fenceNameS.c_str();
    const wchar_t* kInfoMapName = infoNameS.c_str();

    WSADATA ws;
    if (WSAStartup(MAKEWORD(2, 2), &ws) != 0) {
        std::fprintf(stderr, "WSAStartup\n");
        return 1;
    }
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) { std::fprintf(stderr, "socket\n"); return 1; }
    int rcvBuf = 8 * 1024 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char*)&rcvBuf, sizeof(rcvBuf));
    DWORD rcvTimeout = 200;  // ms — lets us notice Ctrl+C quickly
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&rcvTimeout, sizeof(rcvTimeout));

    sockaddr_in bindAddr{};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port   = htons((u_short)args.listenPort);
    bindAddr.sin_addr.s_addr = INADDR_ANY;
    if (bind(sock, (sockaddr*)&bindAddr, sizeof(bindAddr)) == SOCKET_ERROR) {
        std::fprintf(stderr, "bind %d failed: %d\n",
                     args.listenPort, WSAGetLastError());
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    std::printf("Listening on UDP port %d, reachable on:\n", args.listenPort);
    PrintListeningAddresses(args.listenPort);

    try {
        // --- D3D12 ---
        ComPtr<IDXGIFactory6> factory;
        ThrowIfFailed(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)),
                      "CreateDXGIFactory2");
        // NVDEC runs on CUDA which is NVIDIA-only, and the D3D12↔CUDA interop
        // import (cuImportExternalMemory) requires both sides on the same
        // adapter. Prefer NVIDIA; fall back to first-with-output only when no
        // NVIDIA GPU is present (in which case this binary can't really work).
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

        ComPtr<ID3D12Device> d3d12Device;
        ThrowIfFailed(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                        IID_PPV_ARGS(&d3d12Device)),
                      "D3D12CreateDevice");

        // --- CUDA init on the same adapter ---
        CUCheck(cuInit(0), "cuInit");
        CUdevice  cuDev = 0;
        CUcontext cuCtx = nullptr;
        // Match CUDA device to D3D12 adapter LUID so external interop works.
        {
            LUID d3dLuid = d3d12Device->GetAdapterLuid();
            int nDev = 0;
            CUCheck(cuDeviceGetCount(&nDev), "cuDeviceGetCount");
            bool found = false;
            for (int i = 0; i < nDev; ++i) {
                CUdevice d;
                CUCheck(cuDeviceGet(&d, i), "cuDeviceGet");
                char luidRaw[8] = {};
                unsigned int mask = 0;
                if (cuDeviceGetLuid(luidRaw, &mask, d) == CUDA_SUCCESS) {
                    if (std::memcmp(luidRaw, &d3dLuid, sizeof(LUID)) == 0) {
                        cuDev = d; found = true; break;
                    }
                }
            }
            if (!found) CUCheck(cuDeviceGet(&cuDev, 0), "cuDeviceGet fallback");
        }
        CUCheck(cuDevicePrimaryCtxRetain(&cuCtx, cuDev),
                "cuDevicePrimaryCtxRetain");
        CUCheck(cuCtxSetCurrent(cuCtx), "cuCtxSetCurrent");

        // --- D3D12 shared texture (the receiver's published output) ---
        const UINT texW = args.maxW;
        const UINT texH = args.maxH;

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
        texDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        texDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
                                 | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

        ComPtr<ID3D12Resource> sharedTex;
        ThrowIfFailed(d3d12Device->CreateCommittedResource(
                          &heapProps, D3D12_HEAP_FLAG_SHARED,
                          &texDesc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                          IID_PPV_ARGS(&sharedTex)),
                      "CreateCommittedResource(sharedTex)");

        HANDLE hTexture = nullptr;
        ThrowIfFailed(d3d12Device->CreateSharedHandle(
                          sharedTex.Get(), nullptr, GENERIC_ALL,
                          kTextureName, &hTexture),
                      "CreateSharedHandle(tex)");
        std::wprintf(L"Shared texture: name=%ls  handle=0x%p\n",
                     kTextureName, hTexture);

        // --- Shared fence (D3D12 side) ---
        ComPtr<ID3D12Fence> sharedFence;
        ThrowIfFailed(d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_SHARED,
                                               IID_PPV_ARGS(&sharedFence)),
                      "CreateFence");
        HANDLE hFence = nullptr;
        ThrowIfFailed(d3d12Device->CreateSharedHandle(
                          sharedFence.Get(), nullptr, GENERIC_ALL,
                          kFenceName, &hFence),
                      "CreateSharedHandle(fence)");
        std::wprintf(L"Shared fence:   name=%ls  handle=0x%p\n",
                     kFenceName, hFence);

        // --- SharedInfo mapping ---
        HANDLE hMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                         PAGE_READWRITE, 0,
                                         sizeof(SharedInfo), kInfoMapName);
        if (!hMap) throw std::runtime_error("CreateFileMappingW");
        auto* info = static_cast<SharedInfo*>(
            MapViewOfFile(hMap, FILE_MAP_WRITE, 0, 0, sizeof(SharedInfo)));
        if (!info) throw std::runtime_error("MapViewOfFile");
        info->width      = 0;
        info->height     = 0;
        info->format     = DXGI_FORMAT_B8G8R8A8_UNORM;
        info->frameCount = 0;
        info->fenceValue = 0;

        // --- Import D3D12 texture into CUDA as external memory ---
        HANDLE hTexDup = nullptr;
        DuplicateHandle(GetCurrentProcess(), hTexture,
                        GetCurrentProcess(), &hTexDup,
                        0, FALSE, DUPLICATE_SAME_ACCESS);

        D3D12_RESOURCE_ALLOCATION_INFO allocInfo =
            d3d12Device->GetResourceAllocationInfo(0, 1, &texDesc);

        CUexternalMemory extMem = nullptr;
        {
            CUDA_EXTERNAL_MEMORY_HANDLE_DESC d{};
            d.type  = CU_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE;
            d.handle.win32.handle = hTexDup;
            d.size  = allocInfo.SizeInBytes;
            d.flags = CUDA_EXTERNAL_MEMORY_DEDICATED;
            CUCheck(cuImportExternalMemory(&extMem, &d),
                    "cuImportExternalMemory(tex)");
        }

        CUmipmappedArray mmArr = nullptr;
        {
            CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC d{};
            d.offset = 0;
            d.arrayDesc.Width       = texW;
            d.arrayDesc.Height      = texH;
            d.arrayDesc.Depth       = 0;
            d.arrayDesc.Format      = CU_AD_FORMAT_UNSIGNED_INT8;
            d.arrayDesc.NumChannels = 4;
            d.arrayDesc.Flags       = CUDA_ARRAY3D_SURFACE_LDST
                                    | CUDA_ARRAY3D_COLOR_ATTACHMENT;
            d.numLevels = 1;
            CUCheck(cuExternalMemoryGetMappedMipmappedArray(&mmArr, extMem, &d),
                    "cuExternalMemoryGetMappedMipmappedArray");
        }
        CUarray cuArr = nullptr;
        CUCheck(cuMipmappedArrayGetLevel(&cuArr, mmArr, 0),
                "cuMipmappedArrayGetLevel");

        CUsurfObject surfObj = 0;
        {
            CUDA_RESOURCE_DESC r{};
            r.resType = CU_RESOURCE_TYPE_ARRAY;
            r.res.array.hArray = cuArr;
            CUCheck(cuSurfObjectCreate(&surfObj, &r), "cuSurfObjectCreate");
        }

        // --- Import D3D12 fence as CUDA external semaphore ---
        HANDLE hFenceDup = nullptr;
        DuplicateHandle(GetCurrentProcess(), hFence,
                        GetCurrentProcess(), &hFenceDup,
                        0, FALSE, DUPLICATE_SAME_ACCESS);
        CUexternalSemaphore extSem = nullptr;
        {
            CUDA_EXTERNAL_SEMAPHORE_HANDLE_DESC d{};
            d.type  = CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE;
            d.handle.win32.handle = hFenceDup;
            d.flags = 0;
            CUCheck(cuImportExternalSemaphore(&extSem, &d),
                    "cuImportExternalSemaphore");
        }

        CUstream stream = nullptr;
        CUCheck(cuStreamCreate(&stream, CU_STREAM_NON_BLOCKING),
                "cuStreamCreate");

        // --- NvDecoder (low-latency H.264) ---
        NvDecoder decoder(cuCtx,
            /*bUseDeviceFrame*/      true,
                                     cudaVideoCodec_H264,
            /*bLowLatency*/          true,
            /*bDeviceFramePitched*/  true,
            /*pCropRect*/            nullptr,
            /*pResizeDim*/           nullptr,
            /*extract_user_SEI*/     false,
            /*maxWidth*/             (int)args.maxW,
            /*maxHeight*/            (int)args.maxH,
            /*clkRate*/              1000,
            /*force_zero_latency*/   true);

        std::wprintf(L"Listening UDP %d — publishing %ls (%ux%u, BGRA).\n",
                     args.listenPort, kTextureName, texW, texH);
        std::wprintf(L"Ctrl+C to stop.\n");

        // --- Reassembly + decode loop ---
        std::map<uint32_t, FrameAssembly> pending;
        uint32_t nextDeliver = 0;
        bool     streamStarted = false;
        UINT64   fenceValue = 0;
        std::vector<char> rxBuf(streamnet::kMaxPacketBytes);

        auto deliverFrame = [&](FrameAssembly const& f) {
            int n = decoder.Decode(f.bits.data(), (int)f.frameSize);
            for (int i = 0; i < n; ++i) {
                uint8_t* pNv12 = decoder.GetFrame();
                int pitch = decoder.GetDeviceFramePitch();
                int decW  = decoder.GetWidth();
                int decH  = decoder.GetHeight();
                if (!pNv12 || decW <= 0 || decH <= 0) continue;
                int copyW = (std::min)(decW, (int)texW);
                int copyH = (std::min)(decH, (int)texH);

                const uint8_t* yPlane  = pNv12;
                const uint8_t* uvPlane = pNv12 + (size_t)pitch * decH;
                cudaError_t cerr = launchNv12ToBgra(
                    yPlane, pitch, uvPlane, pitch,
                    (unsigned long long)surfObj,
                    0, 0, copyW, copyH,
                    (cudaStream_t)stream);
                if (cerr != cudaSuccess) {
                    std::fprintf(stderr, "kernel err: %s\n",
                                 cudaGetErrorString(cerr));
                    continue;
                }

                // Signal the shared D3D12 fence once the kernel finishes.
                CUDA_EXTERNAL_SEMAPHORE_SIGNAL_PARAMS sp{};
                sp.params.fence.value = ++fenceValue;
                CUCheck(cuSignalExternalSemaphoresAsync(
                            &extSem, &sp, 1, stream),
                        "cuSignalExternalSemaphoresAsync");

                // Advertise the valid content region carried in the packet.
                UINT32 validW = (std::min)(f.width,  (uint32_t)copyW);
                UINT32 validH = (std::min)(f.height, (uint32_t)copyH);
                info->width  = validW ? validW : (UINT32)copyW;
                info->height = validH ? validH : (UINT32)copyH;
                info->frameCount++;
                info->fenceValue = fenceValue;
            }
        };

        while (g_running) {
            sockaddr_in from{};
            int fromLen = sizeof(from);
            int got = recvfrom(sock, rxBuf.data(), (int)rxBuf.size(), 0,
                               (sockaddr*)&from, &fromLen);
            if (got == SOCKET_ERROR) {
                int err = WSAGetLastError();
                if (err == WSAETIMEDOUT) continue;
                std::fprintf(stderr, "recvfrom err %d\n", err);
                continue;
            }
            if (got < (int)sizeof(streamnet::PacketHeader)) continue;

            auto* h = reinterpret_cast<const streamnet::PacketHeader*>(rxBuf.data());
            if (h->magic != streamnet::kMagic) continue;

            if (!streamStarted) {
                // Resync to the first keyframe we see, so the decoder gets a
                // valid starting point.
                if (!(h->flags & streamnet::kFlagKeyframe)) continue;
                nextDeliver = h->frameId;
                streamStarted = true;
            }
            if (h->frameId < nextDeliver) continue;  // stale

            FrameAssembly& fa = pending[h->frameId];
            if (fa.totalPackets == 0) {
                if (h->totalPackets == 0 || h->frameSize == 0) continue;
                fa.totalPackets = h->totalPackets;
                fa.frameSize    = h->frameSize;
                fa.width        = h->width;
                fa.height       = h->height;
                fa.flags        = h->flags;
                fa.bits.resize(h->frameSize);
                fa.have.assign(h->totalPackets, 0);
            }
            if (h->packetIdx < fa.totalPackets && !fa.have[h->packetIdx]) {
                uint32_t off = (uint32_t)h->packetIdx * streamnet::kMaxPayload;
                uint32_t payload = (uint32_t)got - (uint32_t)sizeof(*h);
                if (off + payload <= fa.bits.size()) {
                    std::memcpy(fa.bits.data() + off,
                                rxBuf.data() + sizeof(*h), payload);
                    fa.have[h->packetIdx] = 1;
                    fa.recvCount++;
                }
            }

            // Deliver contiguous complete frames; drop the head if we're
            // backlogged and it still isn't complete.
            while (!pending.empty()) {
                auto it = pending.begin();
                if (it->first != nextDeliver) {
                    // Skip missing frames up to the head entry — caller
                    // expected nextDeliver but the oldest in flight is later.
                    nextDeliver = it->first;
                }
                if (it->second.recvCount == it->second.totalPackets) {
                    deliverFrame(it->second);
                    nextDeliver = it->first + 1;
                    pending.erase(it);
                } else if (pending.size() > 12) {
                    // Incomplete and piling up — give up on this frame.
                    nextDeliver = it->first + 1;
                    pending.erase(it);
                } else {
                    break;
                }
            }
        }

        cuStreamSynchronize(stream);
        cuStreamDestroy(stream);
        cuSurfObjectDestroy(surfObj);
        cuMipmappedArrayDestroy(mmArr);
        cuDestroyExternalMemory(extMem);
        cuDestroyExternalSemaphore(extSem);
        cuDevicePrimaryCtxRelease(cuDev);

        UnmapViewOfFile(info);
        CloseHandle(hMap);
        CloseHandle(hFence);
        CloseHandle(hTexture);
    } catch (std::exception const& e) {
        std::fprintf(stderr, "FATAL: %s\n", e.what());
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    closesocket(sock);
    WSACleanup();
    return 0;
}
