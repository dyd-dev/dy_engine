#pragma once

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <cstdint>

namespace dy::Core
{
	class ThreadPool
	{
	public:
		explicit ThreadPool(uint32_t threadCount = 0);
		~ThreadPool();

		ThreadPool(const ThreadPool&) = delete;
		ThreadPool& operator=(const ThreadPool&) = delete;
		ThreadPool(ThreadPool&&) = delete;
		ThreadPool& operator=(ThreadPool&&) = delete;

		// 작업을 스레드 풀 큐에 추가
		void Enqueue(std::function<void()> task);

		// 현재 큐에 들어간 모든 작업이 완료될 때까지 대기
		void WaitAll();

		[[nodiscard]] uint32_t GetThreadCount() const { return static_cast<uint32_t>(m_workers.size()); }
		[[nodiscard]] bool IsRunning() const { return !m_stop.load(std::memory_order_acquire); }

	private:
		void WorkerLoop();

		std::vector<std::thread> m_workers;
		std::queue<std::function<void()>> m_tasks;

		mutable std::mutex m_queueMutex;
		std::condition_variable m_workerCv;
		std::condition_variable m_waitCv;

		std::atomic<uint32_t> m_busyWorkers{ 0 };
		std::atomic<bool> m_stop{ false };
	};
}
