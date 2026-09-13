#include "Core/ThreadPool.h"
#include <algorithm>

namespace dy::Core
{
	ThreadPool::ThreadPool(uint32_t threadCount)
	{
		if (threadCount == 0)
		{
			uint32_t hardwareThreads = std::thread::hardware_concurrency();
			// 하드웨어 스레드가 0을 반환할 수 있으므로 최소 2개, 일반적으로 4개 이상으로 안전하게 설정
			threadCount = hardwareThreads > 0 ? hardwareThreads : 4;
		}

		m_workers.reserve(threadCount);
		for (uint32_t i = 0; i < threadCount; ++i)
		{
			m_workers.emplace_back(&ThreadPool::WorkerLoop, this);
		}
	}

	ThreadPool::~ThreadPool()
	{
		{
			std::unique_lock<std::mutex> lock(m_queueMutex);
			m_stop.store(true, std::memory_order_release);
		}
		m_workerCv.notify_all();

		for (std::thread& worker : m_workers)
		{
			if (worker.joinable())
			{
				worker.join();
			}
		}
	}

	void ThreadPool::Enqueue(std::function<void()> task)
	{
		if (!task) return;

		{
			std::unique_lock<std::mutex> lock(m_queueMutex);
			if (m_stop.load(std::memory_order_acquire)) return;
			m_tasks.push(std::move(task));
		}
		m_workerCv.notify_one();
	}

	void ThreadPool::WaitAll()
	{
		std::unique_lock<std::mutex> lock(m_queueMutex);
		m_waitCv.wait(lock, [this]() {
			return m_tasks.empty() && m_busyWorkers.load(std::memory_order_acquire) == 0;
		});
	}

	void ThreadPool::WorkerLoop()
	{
		while (true)
		{
			std::function<void()> task;
			{
				std::unique_lock<std::mutex> lock(m_queueMutex);
				m_workerCv.wait(lock, [this]() {
					return m_stop.load(std::memory_order_acquire) || !m_tasks.empty();
				});

				if (m_stop.load(std::memory_order_acquire) && m_tasks.empty())
				{
					return;
				}

				task = std::move(m_tasks.front());
				m_tasks.pop();
				m_busyWorkers.fetch_add(1, std::memory_order_acq_rel);
			}

			// 작업 실행 (예외 발생 시에도 카운터가 정상 감소하도록 보호)
			try
			{
				task();
			}
			catch (...)
			{
			}

			{
				std::unique_lock<std::mutex> lock(m_queueMutex);
				m_busyWorkers.fetch_sub(1, std::memory_order_acq_rel);
				if (m_tasks.empty() && m_busyWorkers.load(std::memory_order_acquire) == 0)
				{
					m_waitCv.notify_all();
				}
			}
		}
	}
}
