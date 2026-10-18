#pragma once

#include "Protocol.h"
#include <atomic>
#include <utility>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace dyf::Platform::Detail
{
	class PipeHandle final
	{
	public:
		PipeHandle() = default;
		explicit PipeHandle(HANDLE handle) noexcept : handle(handle) {}
		~PipeHandle() { Reset(); }
		PipeHandle(const PipeHandle&) = delete;
		PipeHandle& operator=(const PipeHandle&) = delete;
		PipeHandle(PipeHandle&& other) noexcept : handle(std::exchange(other.handle, nullptr)) {}
		PipeHandle& operator=(PipeHandle&& other) noexcept
		{
			if(this != &other) Reset(std::exchange(other.handle, nullptr));
			return *this;
		}

		HANDLE Get() const noexcept { return handle; }
		explicit operator bool() const noexcept { return handle && handle != INVALID_HANDLE_VALUE; }
		void Reset(HANDLE replacement = nullptr) noexcept
		{
			if(*this) CloseHandle(handle);
			handle = replacement;
		}

	private:
		HANDLE handle = nullptr;
	};

	std::wstring ToWide(const std::string&);
	std::wstring MakePipeName(LiveSessionId);
	PipeHandle CreateServerPipe(const std::wstring& name);
	bool IsPeerInSameLogin(HANDLE pipe, bool server, DWORD expectedProcess = 0);
	bool HasPacket(HANDLE pipe);
	bool SendPacket(HANDLE pipe, PacketKind, const std::vector<std::uint8_t>&, const std::atomic<bool>& stop);
	bool ReceivePacket(HANDLE pipe, PacketHeader&, std::vector<std::uint8_t>&, const std::atomic<bool>& stop);
	std::vector<ProfilerTarget> DiscoverTargets();
	bool IsLocalEndpoint(const std::string&);
}
