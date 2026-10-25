#include "Protocol.h"
#include "../CollectorInternal.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include "WindowsPipe.h"
#endif

namespace dyf::Platform
{
#ifdef _WIN32
	namespace
	{
		class Server;
		std::atomic<Server*> activeServer{nullptr};

		class Server final
		{
		public:
			Server() { activeServer = this; }
			~Server()
			{
				activeServer = nullptr;
				Stop();
			}

			void Start()
			{
				std::lock_guard<std::mutex> lock(lifecycleMutex);
				if(worker.joinable())
				{
					if(!finished) return;
					worker.join();
				}
				const auto snapshot = ReadLiveSnapshot();
				if(!snapshot) return;
				const auto name = Detail::MakePipeName(snapshot->session);
				stop = false;
				finished = false;
				worker = std::thread([this, name]
				{
					Run(name);
					finished = true;
				});
			}

			void Stop() noexcept
			{
				std::lock_guard<std::mutex> lock(lifecycleMutex);
				stop = true;
				if(const auto handle = pipe.load(); handle && handle != INVALID_HANDLE_VALUE)
					CancelIoEx(handle, nullptr);
				if(worker.joinable()) worker.join();
			}

		private:
			bool Accept(HANDLE handle)
			{
				Detail::PipeHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
				if(!event) return false;
				OVERLAPPED operation{};
				operation.hEvent = event.Get();
				BOOL connected = ConnectNamedPipe(handle, &operation);
				const DWORD error = connected ? ERROR_SUCCESS : GetLastError();
				if(error == ERROR_IO_PENDING)
				{
					while(!stop && IsLiveCollectorEnabled() &&
						WaitForSingleObject(event.Get(), 20) == WAIT_TIMEOUT) {}
					if(stop || !IsLiveCollectorEnabled())
					{
						CancelIoEx(handle, &operation);
						WaitForSingleObject(event.Get(), INFINITE);
						return false;
					}
					DWORD transferred = 0;
					connected = GetOverlappedResult(handle, &operation, &transferred, FALSE);
				}
				else
				{
					connected = connected || error == ERROR_PIPE_CONNECTED;
				}
				return connected && Detail::IsPeerInSameLogin(handle, true);
			}

			bool ReceiveControl(HANDLE handle)
			{
				Detail::PacketHeader header;
				std::vector<std::uint8_t> payload;
				ControlRequest request;
				std::string error;
				if(!Detail::ReceivePacket(handle, header, payload, stop)) return false;
				if(header.kind == static_cast<std::uint32_t>(Detail::PacketKind::FrameCapture))
				{
					Detail::FrameCaptureRequest capture;
					if(!Detail::DecodeFrameCapture(payload, capture, error)) return false;
					const bool accepted = RequestLiveFrameCapture(capture.device, capture.source, capture.request) != 0;
					const auto acknowledgement = Detail::EncodeAck({capture.request, accepted,
						accepted ? "" : "Frame capture rejected: missing source, disabled collector or capture pending"});
					return Detail::SendPacket(handle, Detail::PacketKind::ControlAck, acknowledgement, stop);
				}
				if(header.kind != static_cast<std::uint32_t>(Detail::PacketKind::Control) || !Detail::DecodeControl(payload, request, error)) return false;

				const bool accepted = Detail::EnqueueControl(request) != 0;
				const auto acknowledgement = Detail::EncodeAck({request.request, accepted,
					accepted ? "" : "Duplicate request or control queue full"});
				return Detail::SendPacket(handle, Detail::PacketKind::ControlAck, acknowledgement, stop);
			}

			bool PublishSnapshot(HANDLE handle)
			{
				const auto snapshot = ReadLiveSnapshot();
				if(!snapshot) return false;
				const auto payload = Detail::EncodeBoundedSnapshot(*snapshot);
				return Detail::SendPacket(handle, Detail::PacketKind::Snapshot, payload, stop);
			}

			void Serve(HANDLE handle)
			{
				std::uint64_t lastPublication = 0;
				while(!stop && IsLiveCollectorEnabled())
				{
					if(Detail::HasPacket(handle) && !ReceiveControl(handle)) return;
					const auto now = LiveNowNs();
					if(now - lastPublication >= LivePublishIntervalNs)
					{
						if(!PublishSnapshot(handle)) return;
						lastPublication = now;
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(5));
				}
			}

			void Run(const std::wstring& name) noexcept
			{
				try
				{
					auto handle = Detail::CreateServerPipe(name);
					if(!handle)
					{
						std::fprintf(stderr, "dyf live: named pipe creation failed (%lu)\n", GetLastError());
						return;
					}
					pipe = handle.Get();
					while(!stop && IsLiveCollectorEnabled())
					{
						if(Accept(handle.Get())) Serve(handle.Get());
						CancelIoEx(handle.Get(), nullptr);
						DisconnectNamedPipe(handle.Get());
					}
					pipe = nullptr;
				}
				catch(const std::exception& error)
				{
					std::fprintf(stderr, "dyf live: pipe server failed: %s\n", error.what());
					pipe = nullptr;
				}
				catch(...)
				{
					std::fprintf(stderr, "dyf live: pipe server failed\n");
					pipe = nullptr;
				}
			}

			std::mutex lifecycleMutex;
			std::atomic<bool> stop{false};
			std::atomic<bool> finished{false};
			std::atomic<HANDLE> pipe{nullptr};
			std::thread worker;
		};
	}
#endif

	void StartProfilerConnection()
	{
#ifdef _WIN32
		if(IsLiveCollectorEnabled() && ReadLiveSnapshot())
		{
			static Server server;
			server.Start();
		}
#endif
	}

	void StopProfilerConnection() noexcept
	{
#ifdef _WIN32
		if(auto* server = activeServer.load()) server->Stop();
#endif
	}
}
