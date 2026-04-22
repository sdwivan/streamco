// Wire protocol shared by streamsender and streamreceiver.
//
// One encoded frame is split into N ≤ kMaxPacketBytes UDP packets, each
// prefixed with a PacketHeader. Receiver reassembles by frameId. Any
// incomplete frame is dropped; sender relies on periodic IDR + gradual
// intra-refresh to recover from loss rather than retransmission.

#pragma once
#include <cstdint>

namespace streamnet {

constexpr uint32_t kMagic       = 0x53544D43;  // 'STMC'
constexpr uint16_t kMaxPayload  = 1400;        // fits a 1500-byte Ethernet MTU
constexpr uint32_t kFlagKeyframe = 0x1;

#pragma pack(push, 1)
struct PacketHeader {
    uint32_t magic;
    uint32_t frameId;
    uint32_t frameSize;     // total encoded bytes in this frame
    uint16_t packetIdx;     // 0 .. totalPackets-1
    uint16_t totalPackets;
    uint32_t width;         // valid content size (may be < texture size)
    uint32_t height;
    uint32_t flags;         // bit 0: keyframe/IDR
    uint32_t reserved;
};
#pragma pack(pop)

static_assert(sizeof(PacketHeader) == 32, "PacketHeader must be 32 bytes");

constexpr int kMaxPacketBytes = sizeof(PacketHeader) + kMaxPayload;

} // namespace streamnet
