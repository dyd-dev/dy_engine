#pragma once

#include "../CollectorInternal.h"

namespace dyf::Platform
{
	// An application explicitly owns the optional connection's lifetime.
	// Collection alone never starts it; re-enabling collection requires a new start.
	void StartProfilerConnection();
	void StopProfilerConnection() noexcept;

	class ProfilerConnection final
	{
	public:
		ProfilerConnection() { StartProfilerConnection(); }
		~ProfilerConnection() { StopProfilerConnection(); }
		ProfilerConnection(const ProfilerConnection&) = delete;
		ProfilerConnection& operator=(const ProfilerConnection&) = delete;
	};

	struct ProfilerTarget
	{
		LiveSessionId session;
		std::uint32_t processId = 0;
		std::string executable;
		std::string endpoint;
	};

	enum class ProfilerClientState : std::uint8_t
	{
		Disconnected,
		Connecting,
		Connected,
		Failed,
		Unsupported
	};

	class ProfilerClient final
	{
	public:
		ProfilerClient();
		~ProfilerClient();
		ProfilerClient(ProfilerClient&&) noexcept;
		ProfilerClient& operator=(ProfilerClient&&) noexcept;
		ProfilerClient(const ProfilerClient&) = delete;
		ProfilerClient& operator=(const ProfilerClient&) = delete;

		static std::vector<ProfilerTarget> Discover();
		bool Connect(const ProfilerTarget&);
		void Disconnect() noexcept;
		ProfilerClientState GetState() const noexcept;
		std::string GetLastError() const;
		// Returns each immutable snapshot once. Ownership survives disconnect.
		std::shared_ptr<const LiveSnapshot> Poll();
		LiveId RequestControl(LiveId control, const ControlValue&);
		// Capture the next new frame; Poll reports its request ID and retained evidence.
		LiveId RequestFrameCapture(LiveId device, LiveId source);

	private:
		struct Impl;
		std::unique_ptr<Impl> impl;
	};
}

namespace dyf::Platform::Detail
{
	constexpr std::uint32_t LiveProtocolVersion = 4;
	constexpr std::size_t LiveTransferLimit = 64u * 1024u * 1024u;

	enum class PacketKind : std::uint32_t
	{
		Snapshot = 1,
		Control = 2,
		ControlAck = 3,
		FrameCapture = 4
	};

	struct PacketHeader
	{
		std::uint32_t magic = 0x564c5944;
		std::uint32_t version = LiveProtocolVersion;
		std::uint32_t kind = 0;
		std::uint32_t size = 0;
	};

	struct ControlAck
	{
		LiveId request = 0;
		bool accepted = false;
		std::string message;
	};
	struct FrameCaptureRequest
	{
		LiveId request = 0, device = 0, source = 0;
	};

	std::vector<std::uint8_t> EncodeSnapshot(const LiveSnapshot&);
	std::vector<std::uint8_t> EncodeBoundedSnapshot(LiveSnapshot&);
	bool DecodeSnapshot(const std::vector<std::uint8_t>&, LiveSnapshot&, std::string& error);
	std::vector<std::uint8_t> EncodeControl(const ControlRequest&);
	bool DecodeControl(const std::vector<std::uint8_t>&, ControlRequest&, std::string& error);
	std::vector<std::uint8_t> EncodeFrameCapture(const FrameCaptureRequest&);
	bool DecodeFrameCapture(const std::vector<std::uint8_t>&, FrameCaptureRequest&, std::string& error);
	std::vector<std::uint8_t> EncodeAck(const ControlAck&);
	bool DecodeAck(const std::vector<std::uint8_t>&, ControlAck&, std::string& error);
	bool ValidateHeader(const PacketHeader&) noexcept;
}
