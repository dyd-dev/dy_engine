#pragma once
#include <cstdint>
#include <functional>
#include <thread>
#include <mutex>
#include <vector>
#include <queue>
#include <atomic>
#include <condition_variable>

namespace dy
{
	class JobSystem
	{
	private:
		std::vector<std::thread> m_workers;
		std::queue<std::function<void()>> m_tasks;
		
		std::mutex m_queueMutex;
		std::condition_variable m_condition;
		bool m_stop = false;

		std::atomic<uint32_t> m_pendingTaskCount{0};

	public:
		void Initialize();
		void Shutdown();

		[[nodiscard]] uint32_t GetWorkerCount() const;

		void ParallelDispatch(uint32_t threadCount, const std::function<void(uint32_t threadIndex)>& task);

		//
		void ParallelFor(uint32_t totalCount, const std::function<void(uint32_t startIdx, uint32_t endIdx)>& task);

		// Main thread blocks until all submitted tasks in the pool reach 0.
		void WaitForAll();
	};
}