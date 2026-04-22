// streamsender — attaches to the shared D3D12 texture (streamco_shared_*),
// encodes each new frame via NVENC-D3D12 (H.264, ultra-low-latency), and
// ships the bitstream over UDP as reassembly-framed packets.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "NvEncoder/NvEncoderD3D12.h"
#include "Logger.h"
#include "../streamnet_protocol.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

using Microsoft::WRL::ComPtr;

// Required by NvCodecUtils.h (it declares logger as extern).
simplelogger::Logger* logger = simplelogger::LoggerFactory::CreateConsoleLogger();

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

std::atomic<bool> g_running{ true };
BOOL WINAPI ConsoleHandler(DWORD sig) {
    if (sig == CTRL_C_EVENT || sig == CTRL_BREAK_EVENT || sig == CTRL_CLOSE_EVENT) {
        g_running = false;
        return TRUE;
    }
    return FALSE;
}

struct Args {
    std::string destIp   = "127.0.0.1";
    int         destPort = 50000;
    int         bitrateKbps = 15000;    // 15 Mbps default
    int         idrPeriod   = 120;      // frames
    int         fps         = 60;
    std::wstring prefix  = L"streamco_shared";
};

Args ParseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        auto next = [&](std::string const& flag) -> const char* {
            if (++i >= argc) throw std::runtime_error("missing value for " + flag);
            return argv[i];
        };
        if (s == "-r") {
            std::string addr = next(s);
            auto c = addr.find(':');
            if (c == std::string::npos) throw std::runtime_error("-r wants ip:port");
            a.destIp   = addr.substr(0, c);
            a.destPort = std::atoi(addr.c_str() + c + 1);
        } else if (s == "-b")   a.bitrateKbps = std::atoi(next(s));
        else if (s == "-idr")   a.idrPeriod   = std::atoi(next(s));
        else if (s == "-fps")   a.fps         = std::atoi(next(s));
        else if (s == "-n" || s == "--name") {
            std::string p = next(s);
            a.prefix.assign(p.begin(), p.end());
        }
        else if (s == "-h" || s == "--help") {
            std::puts("streamsender [-r ip:port] [-b kbps] [-idr N] [-fps N] [-n prefix]");
            std::exit(0);
        }
    }
    return a;
}

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
    if (WSAStartup(MAKEWORD(2, 2), &ws) != 0) { std::fprintf(stderr, "WSAStartup\n"); return 1; }
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) { std::fprintf(stderr, "socket\n"); return 1; }
    int sndBuf = 4 * 1024 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (char*)&sndBuf, sizeof(sndBuf));

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port   = htons((u_short)args.destPort);
    if (inet_pton(AF_INET, args.destIp.c_str(), &dest.sin_addr) != 1) {
        std::fprintf(stderr, "bad ip: %s\n", args.destIp.c_str());
        return 1;
    }

    try {
        ComPtr<IDXGIFactory6> factory;
        ThrowIfFailed(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");

        // NVENC only runs on NVIDIA, and the shared texture (plain HEAP_FLAG_SHARED,
        // not cross-adapter) must live on the same GPU as the encoder. Prefer an
        // NVIDIA adapter; fall back to first-with-output only when none exists.
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

        std::wprintf(L"Waiting for producer (%ls)...\n", kTextureName);
        ComPtr<ID3D12Resource> sharedTex;
        HRESULT lastOpenErr = S_OK;
        bool lastErrWasMismatch = false;
        while (g_running) {
            HANDLE h = nullptr;
            HRESULT hrName = device->OpenSharedHandleByName(kTextureName, GENERIC_ALL, &h);
            if (SUCCEEDED(hrName)) {
                HRESULT hr = device->OpenSharedHandle(h, IID_PPV_ARGS(&sharedTex));
                CloseHandle(h);
                if (SUCCEEDED(hr)) break;
                if (hr != lastOpenErr || !lastErrWasMismatch) {
                    std::fprintf(stderr,
                        "producer texture exists but OpenSharedHandle failed: 0x%08lX "
                        "(adapter mismatch? producer may be on a different GPU)\n",
                        static_cast<unsigned long>(hr));
                    lastOpenErr = hr;
                    lastErrWasMismatch = true;
                }
            } else {
                lastErrWasMismatch = false;
            }
            Sleep(200);
        }
        if (!g_running) return 0;

        ComPtr<ID3D12Fence> sharedFence;
        while (g_running) {
            HANDLE h = nullptr;
            if (SUCCEEDED(device->OpenSharedHandleByName(kFenceName, GENERIC_ALL, &h))) {
                HRESULT hr = device->OpenSharedHandle(h, IID_PPV_ARGS(&sharedFence));
                CloseHandle(h);
                if (SUCCEEDED(hr)) break;
            }
            Sleep(200);
        }
        if (!g_running) return 0;

        HANDLE hMap = nullptr;
        while (g_running) {
            hMap = OpenFileMappingW(FILE_MAP_READ, FALSE, kInfoMapName);
            if (hMap) break;
            Sleep(200);
        }
        if (!g_running) return 0;
        auto* info = static_cast<const SharedInfo*>(
            MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, sizeof(SharedInfo)));
        if (!info) throw std::runtime_error("MapViewOfFile");

        D3D12_RESOURCE_DESC texDesc = sharedTex->GetDesc();
        const UINT texW = static_cast<UINT>(texDesc.Width);
        const UINT texH = texDesc.Height;

        // Encoder dimensions = the reserved texture (covers any resize up to
        // that bound). The receiver reads width/height from each packet
        // header so it knows the valid content sub-region.
        const UINT encW = texW;
        const UINT encH = texH;

        std::wprintf(L"Connected: tex %ux%u, valid %ux%u, encode @ %ux%u\n",
                     texW, texH, info->width, info->height, encW, encH);

        NvEncoderD3D12 enc(device.Get(), encW, encH, NV_ENC_BUFFER_FORMAT_ARGB);

        NV_ENC_INITIALIZE_PARAMS initParams{ NV_ENC_INITIALIZE_PARAMS_VER };
        NV_ENC_CONFIG            cfg{ NV_ENC_CONFIG_VER };
        initParams.encodeConfig = &cfg;
        enc.CreateDefaultEncoderParams(&initParams,
            NV_ENC_CODEC_H264_GUID,
            NV_ENC_PRESET_P3_GUID,
            NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY);

        initParams.frameRateNum = (uint32_t)args.fps;
        initParams.frameRateDen = 1;

        cfg.gopLength      = NVENC_INFINITE_GOPLENGTH;
        cfg.frameIntervalP = 1;  // IPPP, no B frames
        cfg.encodeCodecConfig.h264Config.idrPeriod      = NVENC_INFINITE_GOPLENGTH;
        cfg.encodeCodecConfig.h264Config.repeatSPSPPS   = 1;
        cfg.encodeCodecConfig.h264Config.sliceMode      = 0;
        cfg.encodeCodecConfig.h264Config.sliceModeData  = 0;
        cfg.encodeCodecConfig.h264Config.enableIntraRefresh = 1;
        cfg.encodeCodecConfig.h264Config.intraRefreshPeriod = args.idrPeriod;
        cfg.encodeCodecConfig.h264Config.intraRefreshCnt    = args.idrPeriod / 2;

        const uint32_t bps = (uint32_t)args.bitrateKbps * 1000u;
        cfg.rcParams.rateControlMode    = NV_ENC_PARAMS_RC_CBR;
        cfg.rcParams.multiPass          = NV_ENC_MULTI_PASS_DISABLED;
        cfg.rcParams.averageBitRate     = bps;
        cfg.rcParams.maxBitRate         = bps;
        cfg.rcParams.vbvBufferSize      = bps / (uint32_t)args.fps;
        cfg.rcParams.vbvInitialDelay    = cfg.rcParams.vbvBufferSize;
        cfg.rcParams.lowDelayKeyFrameScale = 1;

        enc.CreateEncoder(&initParams);

        std::wprintf(L"NVENC: H.264 ULL preset P3, %d kbps, IR period %d, %d fps\n",
                     args.bitrateKbps, args.idrPeriod, args.fps);
        std::wprintf(L"Sending to %hs:%d — Ctrl+C to stop.\n",
                     args.destIp.c_str(), args.destPort);

        const UINT numBfrs = enc.GetNumBfrs();
        std::vector<ComPtr<ID3D12CommandAllocator>> cmdAllocs(numBfrs);
        for (UINT i = 0; i < numBfrs; ++i) {
            ThrowIfFailed(device->CreateCommandAllocator(
                              D3D12_COMMAND_LIST_TYPE_DIRECT,
                              IID_PPV_ARGS(&cmdAllocs[i])),
                          "CreateCommandAllocator");
        }
        ComPtr<ID3D12GraphicsCommandList> cmdList;
        ThrowIfFailed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                cmdAllocs[0].Get(), nullptr,
                                                IID_PPV_ARGS(&cmdList)),
                      "CreateCommandList");
        cmdList->Close();

        std::vector<char> sendBuf(streamnet::kMaxPacketBytes);
        uint32_t frameId = 0;
        UINT64 lastProducerFence = 0;

        while (g_running) {
            // Wait for a new producer frame. Short sleep keeps this from
            // spinning at 100% when the producer stalls.
            UINT64 latest = info->fenceValue;
            if (latest <= lastProducerFence) { Sleep(1); continue; }
            lastProducerFence = latest;

            // GPU-side sync against the producer's shared fence before we
            // read the texture.
            ThrowIfFailed(queue->Wait(sharedFence.Get(), latest),
                          "queue->Wait(sharedFence)");

            const NvEncInputFrame* inputFrame = enc.GetNextInputFrame();
            ID3D12Resource* nvInput = (ID3D12Resource*)inputFrame->inputPtr;
            UINT bfrIdx = frameId % numBfrs;

            ThrowIfFailed(cmdAllocs[bfrIdx]->Reset(), "cmdAlloc Reset");
            ThrowIfFailed(cmdList->Reset(cmdAllocs[bfrIdx].Get(), nullptr), "cmdList Reset");

            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource   = nvInput;
            barrier.Transition.Subresource = 0;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
            cmdList->ResourceBarrier(1, &barrier);

            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource        = nvInput;
            dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource        = sharedTex.Get();
            src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = 0;
            // Copy the full texture — encoder is sized to texW/texH; regions
            // outside info->width/height are unchanged noise but receiver
            // masks them via uvScale the same way screendisplay does.
            cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
            cmdList->ResourceBarrier(1, &barrier);
            ThrowIfFailed(cmdList->Close(), "cmdList Close");

            ID3D12CommandList* lists[] = { cmdList.Get() };
            queue->ExecuteCommandLists(1, lists);

            // Tell NVENC our copy is done.
            InterlockedIncrement(enc.GetInpFenceValPtr());
            queue->Signal(enc.GetInpFence(), *enc.GetInpFenceValPtr());

            std::vector<std::vector<uint8_t>> packets;
            NV_ENC_PIC_PARAMS picParams{ NV_ENC_PIC_PARAMS_VER };
            const bool forceIdr = (frameId > 0) && (frameId % (uint32_t)args.idrPeriod) == 0;
            if (forceIdr) {
                picParams.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
            }
            enc.EncodeFrame(packets, &picParams);

            // Packetize each encoded bitstream chunk and send via UDP.
            const UINT32 validW = info->width  ? info->width  : encW;
            const UINT32 validH = info->height ? info->height : encH;
            for (auto const& pkt : packets) {
                if (pkt.empty()) continue;
                const uint32_t size = (uint32_t)pkt.size();
                const uint16_t total = (uint16_t)((size + streamnet::kMaxPayload - 1)
                                                  / streamnet::kMaxPayload);
                for (uint16_t i = 0; i < total; ++i) {
                    streamnet::PacketHeader hdr{};
                    hdr.magic        = streamnet::kMagic;
                    hdr.frameId      = frameId;
                    hdr.frameSize    = size;
                    hdr.packetIdx    = i;
                    hdr.totalPackets = total;
                    hdr.width        = validW;
                    hdr.height       = validH;
                    hdr.flags        = forceIdr ? streamnet::kFlagKeyframe : 0;

                    uint32_t off     = (uint32_t)i * streamnet::kMaxPayload;
                    uint32_t payload = (size - off < streamnet::kMaxPayload)
                                         ? (size - off) : streamnet::kMaxPayload;
                    std::memcpy(sendBuf.data(), &hdr, sizeof(hdr));
                    std::memcpy(sendBuf.data() + sizeof(hdr), pkt.data() + off, payload);
                    int sent = sendto(sock, sendBuf.data(),
                                      (int)(sizeof(hdr) + payload), 0,
                                      (sockaddr*)&dest, sizeof(dest));
                    if (sent == SOCKET_ERROR) {
                        std::fprintf(stderr, "sendto err %d\n", WSAGetLastError());
                        break;
                    }
                }
            }

            ++frameId;
        }

        // Drain the encoder.
        std::vector<std::vector<uint8_t>> tail;
        enc.EndEncode(tail);
        enc.DestroyEncoder();

        UnmapViewOfFile(info);
        CloseHandle(hMap);
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
