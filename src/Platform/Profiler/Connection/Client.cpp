#include "Protocol.h"
#include "../CollectorInternal.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <thread>

#ifdef _WIN32
#include "WindowsPipe.h"
#endif

namespace dyf::Platform
{
	struct ProfilerClient::Impl
	{
		mutable std::mutex mutex;
		ProfilerClientState state = ProfilerClientState::Disconnected;
		std::string error, captureError;
		std::shared_ptr<const LiveSnapshot> snapshot;
		std::uint64_t delivered = 0;
		std::deque<ControlRequest> outbound;
		std::deque<Detail::FrameCaptureRequest> captures;
		std::map<LiveId, ControlStatus> localRequests;
		LiveId nextRequest = []
		{
			std::random_device random;
			return ((std::uint64_t(random()) << 32) ^ random()) | (std::uint64_t(1) << 63);
		}();
		std::atomic<bool> stop{false};
		std::thread worker;

		~Impl() { Disconnect(); }

		void Fail(std::string message)
		{
			std::lock_guard<std::mutex> lock(mutex);
			state = ProfilerClientState::Failed;
			error = std::move(message);
			outbound.clear();
			captures.clear();
		}

		void Disconnect() noexcept
		{
			stop = true;
			if(worker.joinable()) worker.join();
			std::lock_guard<std::mutex> lock(mutex);
			state = ProfilerClientState::Disconnected;
			outbound.clear();
			captures.clear();
			captureError.clear();
			localRequests.clear();
			snapshot.reset();
			delivered = 0;
		}

#ifdef _WIN32
		Detail::PipeHandle Open(const ProfilerTarget& target)
		{
			const auto name = Detail::ToWide(target.endpoint);
			const auto deadline = LiveNowNs() + 2000000000ull;
			DWORD openError = 0;
			while(!stop)
			{
				Detail::PipeHandle pipe(CreateFileW(name.c_str(),
					FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
					0, nullptr, OPEN_EXISTING,
					FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
				if(pipe) return pipe;
				openError = ::GetLastError();
				if((openError != ERROR_PIPE_BUSY && openError != ERROR_FILE_NOT_FOUND) ||
					LiveNowNs() >= deadline)
					break;
				WaitNamedPipeW(name.c_str(), 50);
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			if(!stop) Fail("Named pipe open failed (" + std::to_string(openError) + ")");
			return {};
		}

		bool SendControls(HANDLE pipe)
		{
			std::deque<ControlRequest> requests;
			std::deque<Detail::FrameCaptureRequest> captureRequests;
			{
				std::lock_guard<std::mutex> lock(mutex);
				requests.swap(outbound);
				captureRequests.swap(captures);
			}
			for(const auto& request : requests)
			{
				const auto payload = Detail::EncodeControl(request);
				if(!Detail::SendPacket(pipe, Detail::PacketKind::Control, payload, stop))
				{
					Fail("Control write failed");
					return false;
				}
			}
			for(const auto& request : captureRequests)
			{
				const auto payload=Detail::EncodeFrameCapture(request);
				if(!Detail::SendPacket(pipe,Detail::PacketKind::FrameCapture,payload,stop))
				{
					Fail("Frame capture request write failed");
					return false;
				}
			}
			return true;
		}

		bool ReceiveAcknowledgement(const std::vector<std::uint8_t>& payload)
		{
			Detail::ControlAck acknowledgement;
			std::string reason;
			if(!Detail::DecodeAck(payload, acknowledgement, reason))
			{
				Fail(reason);
				return false;
			}
			if(!acknowledgement.accepted)
			{
				std::lock_guard<std::mutex> lock(mutex);
				const auto found = localRequests.find(acknowledgement.request);
				if(found != localRequests.end())
				{
					found->second.state = ControlState::Rejected;
					found->second.message = acknowledgement.message;
				}
				else captureError = acknowledgement.message;
			}
			return true;
		}

		void Publish(std::shared_ptr<LiveSnapshot> value)
		{
			std::lock_guard<std::mutex> lock(mutex);
			for(const auto& [requestId, status] : localRequests)
			{
				if(status.state != ControlState::Rejected) continue;
				auto& statuses = value->controlStatus;
				statuses.erase(std::remove_if(statuses.begin(), statuses.end(),
					[requestId](const auto& entry) { return entry.request == requestId; }), statuses.end());
				statuses.push_back(status);
			}
			snapshot = std::move(value);
			state = ProfilerClientState::Connected;
			error.clear();
		}

		bool Receive(HANDLE pipe, const ProfilerTarget& target, std::uint64_t& lastSnapshot)
		{
			Detail::PacketHeader header;
			std::vector<std::uint8_t> payload;
			if(!Detail::ReceivePacket(pipe, header, payload, stop))
			{
				Fail("Invalid live packet");
				return false;
			}
			if(header.kind == static_cast<std::uint32_t>(Detail::PacketKind::ControlAck))
				return ReceiveAcknowledgement(payload);

			auto value = std::make_shared<LiveSnapshot>();
			std::string reason;
			if(header.kind != static_cast<std::uint32_t>(Detail::PacketKind::Snapshot) ||
				!Detail::DecodeSnapshot(payload, *value, reason) || value->session != target.session)
			{
				Fail(reason.empty() ? "Invalid live session/protocol" : reason);
				return false;
			}
			Publish(std::move(value));
			lastSnapshot = LiveNowNs();
			return true;
		}

		void Run(const ProfilerTarget& target)
		{
			try
			{
				auto pipe = Open(target);
				if(!pipe || stop) return;
				if(!Detail::IsPeerInSameLogin(pipe.Get(), false, target.processId))
				{
					Fail("Named pipe peer is outside this login/session");
					return;
				}
				auto lastSnapshot = LiveNowNs();
				while(!stop)
				{
					if(!SendControls(pipe.Get())) return;
					if(Detail::HasPacket(pipe.Get()) && !Receive(pipe.Get(), target, lastSnapshot)) return;
					if(LiveNowNs() - lastSnapshot > 2000000000ull)
					{
						Fail("Live target disconnected or unresponsive");
						return;
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(5));
				}
			}
			catch(const std::exception& failure)
			{
				Fail(failure.what());
			}
			catch(...)
			{
				Fail("Live client failed");
			}
		}
#endif
	};

	ProfilerClient::ProfilerClient() : impl(std::make_unique<Impl>()) {}
	ProfilerClient::~ProfilerClient() = default;
	ProfilerClient::ProfilerClient(ProfilerClient&&) noexcept = default;
	ProfilerClient& ProfilerClient::operator=(ProfilerClient&&) noexcept = default;

	std::vector<ProfilerTarget> ProfilerClient::Discover()
	{
#ifdef _WIN32
		return Detail::DiscoverTargets();
#else
		return {};
#endif
	}

	bool ProfilerClient::Connect(const ProfilerTarget& target)
	{
		if(!impl) impl = std::make_unique<Impl>();
		impl->Disconnect();
#ifdef _WIN32
		if(!Detail::IsLocalEndpoint(target.endpoint)) return false;
		{
			std::lock_guard<std::mutex> lock(impl->mutex);
			impl->state = ProfilerClientState::Connecting;
			impl->error.clear();
		}
		impl->stop = false;
		impl->worker = std::thread([state = impl.get(), target] { state->Run(target); });
		return true;
#else
		(void)target;
		std::lock_guard<std::mutex> lock(impl->mutex);
		impl->state = ProfilerClientState::Unsupported;
		return false;
#endif
	}

	void ProfilerClient::Disconnect() noexcept
	{
		if(impl) impl->Disconnect();
	}

	ProfilerClientState ProfilerClient::GetState() const noexcept
	{
		if(!impl) return ProfilerClientState::Disconnected;
		std::lock_guard<std::mutex> lock(impl->mutex);
		return impl->state;
	}

	std::string ProfilerClient::GetLastError() const
	{
		if(!impl) return {};
		std::lock_guard<std::mutex> lock(impl->mutex);
		return impl->error.empty() ? impl->captureError : impl->error;
	}

	std::shared_ptr<const LiveSnapshot> ProfilerClient::Poll()
	{
		if(!impl) return {};
		std::lock_guard<std::mutex> lock(impl->mutex);
		if(!impl->snapshot || impl->delivered == impl->snapshot->sequence) return {};
		impl->delivered = impl->snapshot->sequence;
		return impl->snapshot;
	}

	LiveId ProfilerClient::RequestControl(LiveId control, const ControlValue& value)
	{
		if(!impl || !control) return 0;
		std::lock_guard<std::mutex> lock(impl->mutex);
		if(impl->state != ProfilerClientState::Connected || impl->outbound.size() >= 64) return 0;
		if(!impl->nextRequest)
		{
			impl->error = "Request ID space exhausted";
			return 0;
		}
		const auto id = impl->nextRequest++;
		impl->outbound.push_back({id, control, value});
		ControlStatus status;
		status.request = id;
		status.control = control;
		status.requested = value;
		impl->localRequests[id] = std::move(status);
		while(impl->localRequests.size() > 256) impl->localRequests.erase(impl->localRequests.begin());
		return id;
	}
	LiveId ProfilerClient::RequestFrameCapture(LiveId device, LiveId source)
	{
		if(!impl || !device || !source) return 0;
		std::lock_guard<std::mutex> lock(impl->mutex);
		if(impl->state != ProfilerClientState::Connected || impl->captures.size() >= 64) return 0;
		if(!impl->nextRequest)
		{
			impl->error = "Request ID space exhausted";
			return 0;
		}
		const auto request=impl->nextRequest++;
		impl->captures.push_back({request,device,source});
		impl->captureError.clear();
		return request;
	}
}
