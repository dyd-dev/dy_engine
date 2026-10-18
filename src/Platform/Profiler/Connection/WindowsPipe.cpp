#include "Protocol.h"
#include "WindowsPipe.h"
#include <sddl.h>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace dyf::Platform::Detail
{
	namespace
	{
		struct LocalMemory
		{
			HLOCAL value = nullptr;
			~LocalMemory() { if(value) LocalFree(value); }
		};

		std::vector<std::uint8_t> ReadTokenInfo(HANDLE token, TOKEN_INFORMATION_CLASS kind)
		{
			DWORD size = 0;
			GetTokenInformation(token, kind, nullptr, 0, &size);
			if(!size || size > 65536) return {};

			std::vector<std::uint8_t> data(size);
			if(!GetTokenInformation(token, kind, data.data(), size, &size)) return {};
			return data;
		}

		PSID FindLogonSid(std::vector<std::uint8_t>& data)
		{
			if(data.empty()) return nullptr;
			auto* groups = reinterpret_cast<TOKEN_GROUPS*>(data.data());
			for(DWORD i = 0; i < groups->GroupCount; ++i)
			{
				if((groups->Groups[i].Attributes & SE_GROUP_LOGON_ID) == SE_GROUP_LOGON_ID)
					return groups->Groups[i].Sid;
			}
			return nullptr;
		}

		bool IsSameLogin(DWORD processId)
		{
			PipeHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId));
			if(!process) return false;

			HANDLE peerToken = nullptr, currentToken = nullptr;
			if(!OpenProcessToken(process.Get(), TOKEN_QUERY, &peerToken)) return false;
			PipeHandle peer(peerToken);
			if(!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &currentToken)) return false;
			PipeHandle current(currentToken);

			auto peerUser = ReadTokenInfo(peer.Get(), TokenUser);
			auto currentUser = ReadTokenInfo(current.Get(), TokenUser);
			auto peerGroups = ReadTokenInfo(peer.Get(), TokenGroups);
			auto currentGroups = ReadTokenInfo(current.Get(), TokenGroups);
			auto peerSession = ReadTokenInfo(peer.Get(), TokenSessionId);
			auto currentSession = ReadTokenInfo(current.Get(), TokenSessionId);
			const auto peerSid = FindLogonSid(peerGroups);
			const auto currentSid = FindLogonSid(currentGroups);
			if(peerUser.empty() || currentUser.empty() || !peerSid || !currentSid ||
				peerSession.size() != sizeof(DWORD) || currentSession.size() != sizeof(DWORD))
				return false;

			return EqualSid(reinterpret_cast<TOKEN_USER*>(peerUser.data())->User.Sid,
					reinterpret_cast<TOKEN_USER*>(currentUser.data())->User.Sid) &&
				EqualSid(peerSid, currentSid) &&
				std::memcmp(peerSession.data(), currentSession.data(), sizeof(DWORD)) == 0;
		}

		std::string ToUtf8(const std::wstring& value)
		{
			if(value.empty()) return {};
			const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()),
				nullptr, 0, nullptr, nullptr);
			if(!size) return {};
			std::string result(size, '\0');
			WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()),
				result.data(), size, nullptr, nullptr);
			return result;
		}

		bool Transfer(HANDLE pipe, bool write, void* data, DWORD size, const std::atomic<bool>& stop)
		{
			constexpr ULONGLONG timeoutMilliseconds = 1000;
			auto* bytes = static_cast<std::uint8_t*>(data);
			DWORD transferred = 0;
			const auto started = GetTickCount64();
			while(transferred < size && !stop)
			{
				PipeHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
				if(!event) return false;
				OVERLAPPED operation{};
				operation.hEvent = event.Get();
				DWORD count = 0;
				const BOOL completed = write
					? WriteFile(pipe, bytes + transferred, size - transferred, &count, &operation)
					: ReadFile(pipe, bytes + transferred, size - transferred, &count, &operation);
				if(!completed)
				{
					if(GetLastError() != ERROR_IO_PENDING) return false;
					while(WaitForSingleObject(event.Get(), 20) == WAIT_TIMEOUT)
					{
						if(stop || GetTickCount64() - started >= timeoutMilliseconds)
						{
							CancelIoEx(pipe, &operation);
							WaitForSingleObject(event.Get(), INFINITE);
							return false;
						}
					}
					if(!GetOverlappedResult(pipe, &operation, &count, FALSE)) return false;
				}
				if(!count) return false;
				transferred += count;
			}
			return transferred == size;
		}
	}

	std::wstring ToWide(const std::string& value)
	{
		if(value.empty()) return {};
		const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
			value.data(), int(value.size()), nullptr, 0);
		if(!size) return {};
		std::wstring result(size, L'\0');
		MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), int(value.size()),
			result.data(), size);
		return result;
	}

	std::wstring MakePipeName(LiveSessionId session)
	{
		std::wostringstream name;
		name << L"\\\\.\\pipe\\dy_engine.live.v3." << GetCurrentProcessId() << L'.'
			<< std::hex << session.high << L'.' << session.low;
		return name.str();
	}

	PipeHandle CreateServerPipe(const std::wstring& name)
	{
		HANDLE tokenValue = nullptr;
		if(!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tokenValue))
			throw std::runtime_error("Live connection: process token query failed");
		PipeHandle token(tokenValue);
		auto groups = ReadTokenInfo(token.Get(), TokenGroups);
		auto sid = FindLogonSid(groups);
		if(!sid) throw std::runtime_error("Live connection: logon SID unavailable");

		LPWSTR sidText = nullptr;
		if(!ConvertSidToStringSidW(sid, &sidText))
			throw std::runtime_error("Live connection: SID conversion failed");
		LocalMemory ownedSid{sidText};
		const std::wstring acl = L"D:P(A;;0x12019b;;;" + std::wstring(sidText) + L")";
		PSECURITY_DESCRIPTOR descriptor = nullptr;
		if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(
			acl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
			throw std::runtime_error("Live connection: pipe security descriptor failed");
		LocalMemory ownedDescriptor{descriptor};
		SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
		return PipeHandle(CreateNamedPipeW(name.c_str(),
			PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
			PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
			1, 65536, 65536, 0, &security));
	}

	bool IsPeerInSameLogin(HANDLE pipe, bool server, DWORD expectedProcess)
	{
		ULONG processId = 0;
		const BOOL identified = server
			? GetNamedPipeClientProcessId(pipe, &processId)
			: GetNamedPipeServerProcessId(pipe, &processId);
		return identified && (!expectedProcess || processId == expectedProcess) && IsSameLogin(processId);
	}

	bool HasPacket(HANDLE pipe)
	{
		DWORD size = 0;
		return PeekNamedPipe(pipe, nullptr, 0, nullptr, &size, nullptr) && size >= sizeof(PacketHeader);
	}

	bool SendPacket(HANDLE pipe, PacketKind kind, const std::vector<std::uint8_t>& payload,
		const std::atomic<bool>& stop)
	{
		PacketHeader header;
		header.kind = static_cast<std::uint32_t>(kind);
		header.size = static_cast<std::uint32_t>(payload.size());
		if(!ValidateHeader(header)) return false;
		return Transfer(pipe, true, &header, sizeof(header), stop) &&
			Transfer(pipe, true, const_cast<std::uint8_t*>(payload.data()), header.size, stop);
	}

	bool ReceivePacket(HANDLE pipe, PacketHeader& header, std::vector<std::uint8_t>& payload,
		const std::atomic<bool>& stop)
	{
		if(!Transfer(pipe, false, &header, sizeof(header), stop) || !ValidateHeader(header)) return false;
		payload.resize(header.size);
		return Transfer(pipe, false, payload.data(), header.size, stop);
	}

	std::vector<ProfilerTarget> DiscoverTargets()
	{
		struct PipeSearch
		{
			HANDLE handle;
			~PipeSearch() { if(handle != INVALID_HANDLE_VALUE) FindClose(handle); }
		};
		std::vector<ProfilerTarget> targets;
		WIN32_FIND_DATAW entry{};
		PipeSearch search{FindFirstFileW(L"\\\\.\\pipe\\*", &entry)};
		if(search.handle == INVALID_HANDLE_VALUE) return targets;
		do
		{
			const auto fullName = ToUtf8(entry.cFileName);
			const auto slash = fullName.find_last_of('\\');
			const auto leaf = slash == std::string::npos ? fullName : fullName.substr(slash + 1);
			unsigned processId = 0;
			unsigned long long high = 0, low = 0;
			if(std::sscanf(leaf.c_str(), "dy_engine.live.v3.%u.%llx.%llx", &processId, &high, &low) != 3 ||
				!IsSameLogin(processId))
				continue;

			ProfilerTarget target;
			target.session = {high, low};
			target.processId = processId;
			target.endpoint = "\\\\.\\pipe\\" + fullName;
			PipeHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId));
			wchar_t executable[32768]{};
			DWORD length = 32768;
			if(process && QueryFullProcessImageNameW(process.Get(), 0, executable, &length))
				target.executable = ToUtf8(std::wstring(executable, length));
			targets.push_back(std::move(target));
		} while(FindNextFileW(search.handle, &entry));
		return targets;
	}

	bool IsLocalEndpoint(const std::string& endpoint)
	{
		if(endpoint.rfind("\\\\.\\pipe\\", 0) != 0) return false;
		const auto suffix = endpoint.substr(9);
		const auto slash = suffix.find_last_of('\\');
		const auto leaf = slash == std::string::npos ? suffix : suffix.substr(slash + 1);
		if(leaf.rfind("dy_engine.live.v3.", 0) != 0) return false;
		return slash == std::string::npos ||
			(suffix.rfind("Sessions\\", 0) == 0 &&
				suffix.find("\\AppContainerNamedObjects\\S-1-15-2-") != std::string::npos);
	}
}
