#include "JobSystem.h"
#include <algorithm>

using namespace dy;

void JobSystem::Initialize()
{
	uint32_t numThreads = std::max(1u, std::thread::hardware_concurrency() - 1);
	for(uint32_t i=0; i<numThreads; i++)
	{
		m_workers.emplace_back([this]()
		{
			while(true)
			{
				std::function<void()> task;
				{
					std::unique_lock<std::mutex> lock(this->m_queueMutex);
					this->m_condition.wait(lock, [this] { return this->m_stop; });
					
					if(this->m_stop && this->m_tasks.empty()) return;

					task = std::move(this->m_tasks.front());
					this->m_tasks.pop();
				}

				task();

				m_pendingTaskCount.fetch_sub(1, std::memory_order_release);
			}
		});
	}
}

void JobSystem::Shutdown()
{
	{
		std::unique_lock<std::mutex> lock(m_queueMutex);
		m_stop = true;
	}
	m_condition.notify_all();
	for(std::thread& worker : m_workers)
	{
		if( worker.joinable()) worker.join();
	}
}

uint32_t JobSystem::GetWorkerCount() const
{
	return static_cast<uint32_t>(m_workers.size());
}

