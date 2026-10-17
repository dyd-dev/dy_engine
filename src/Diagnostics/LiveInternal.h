#pragma once
#include "dyf/Diagnostics/LiveProfiling.h"

namespace dyf::Diagnostics::Detail
{
#ifdef DY_LIVE_TEST_CLOCK
void SetTestLiveNowNs(std::uint64_t) noexcept;
void FailTestLiveAggregationAllocationAfter(std::int64_t) noexcept;
#endif
enum class PacketKind : std::uint32_t { Snapshot=1,Control=2,ControlAck=3 };
struct ControlAck { LiveId request=0; bool accepted=false; std::string message; };
struct PacketHeader { std::uint32_t magic=0x564c5944,version=LiveProtocolVersion,kind=0,size=0; };
std::vector<std::uint8_t> EncodeSnapshot(const LiveSnapshot&);
std::vector<std::uint8_t> EncodeBoundedSnapshot(LiveSnapshot&);
bool DecodeSnapshot(const std::vector<std::uint8_t>&,LiveSnapshot&,std::string&);
std::vector<std::uint8_t> EncodeControl(const ControlRequest&);
bool DecodeControl(const std::vector<std::uint8_t>&,ControlRequest&,std::string&);
std::vector<std::uint8_t> EncodeAck(const ControlAck&);
bool DecodeAck(const std::vector<std::uint8_t>&,ControlAck&,std::string&);
bool ValidateHeader(const PacketHeader&) noexcept;
LiveId EnqueueControl(ControlRequest);
void StartServer();
void StopServer() noexcept;
}
