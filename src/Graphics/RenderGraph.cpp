#include "Graphics/RenderGraph.h"
#include "RHI/ICommandList.h"
#include "RHI/IDevice.h"
#include "Core/ThreadPool.h"
#include <algorithm>
#include <cassert>

namespace dy::Graphics
{
	// ----------------------------------------------------------------------------------
	// RenderGraphPass Implementation
	// ----------------------------------------------------------------------------------

	RenderGraphPass& RenderGraphPass::Read(RGResourceHandle resource, RGResourceAccess access)
	{
		m_reads.push_back({ resource, access });
		++m_revision;
		return *this;
	}

	RenderGraphPass& RenderGraphPass::Write(RGResourceHandle resource, RGResourceAccess access)
	{
		m_writes.push_back({ resource, access });
		++m_revision;
		return *this;
	}

	RenderGraphPass& RenderGraphPass::SetPipeline(RHI::IPipelineState* pipeline)
	{
		m_pipeline = pipeline;
		++m_revision;
		return *this;
	}

	RenderGraphPass& RenderGraphPass::SetExecute(RGPassExecuteCallback callback)
	{
		m_executeCallback = std::move(callback);
		++m_revision;
		return *this;
	}

	RenderGraphPass& RenderGraphPass::TextureBarrier(RGResourceHandle texture, RGResourceAccess beforeAccess, RGResourceAccess afterAccess)
	{
		m_barriers.push_back({ RGBarrierType::Texture, texture, beforeAccess, afterAccess });
		++m_revision;
		return *this;
	}

	RenderGraphPass& RenderGraphPass::BufferBarrier(RGResourceHandle buffer, RGResourceAccess beforeAccess, RGResourceAccess afterAccess)
	{
		m_barriers.push_back({ RGBarrierType::Buffer, buffer, beforeAccess, afterAccess });
		++m_revision;
		return *this;
	}

	RenderGraphPass& RenderGraphPass::GlobalBarrier(RGResourceAccess beforeAccess, RGResourceAccess afterAccess)
	{
		m_barriers.push_back({ RGBarrierType::Global, {}, beforeAccess, afterAccess });
		++m_revision;
		return *this;
	}

	void RenderGraphPass::Execute(RHI::ICommandList* cmdList) const
	{
		if (m_pipeline && cmdList)
		{
			cmdList->BindGraphicsPipeline(m_pipeline);
		}

		if (m_executeCallback)
		{
			m_executeCallback(cmdList);
		}
	}

	// ----------------------------------------------------------------------------------
	// RenderGraph Implementation
	// ----------------------------------------------------------------------------------

	RGResourceHandle RenderGraph::ImportTexture(const std::string& name, RHI::ITexture* texture)
	{
		auto it = m_resourceNameToHandle.find(name);
		if (it != m_resourceNameToHandle.end())
		{
			return it->second;
		}

		const uint32_t newId = static_cast<uint32_t>(m_resources.size());
		RGResourceDesc desc;
		desc.name = name;
		desc.type = RGResourceType::Texture;
		desc.texturePtr = texture;
		m_resources.push_back(desc);
		m_compiled = false;

		RGResourceHandle handle{ newId };
		m_resourceNameToHandle[name] = handle;
		return handle;
	}

	RGResourceHandle RenderGraph::ImportBuffer(const std::string& name, RHI::IBuffer* buffer)
	{
		auto it = m_resourceNameToHandle.find(name);
		if (it != m_resourceNameToHandle.end())
		{
			return it->second;
		}

		const uint32_t newId = static_cast<uint32_t>(m_resources.size());
		RGResourceDesc desc;
		desc.name = name;
		desc.type = RGResourceType::Buffer;
		desc.bufferPtr = buffer;
		m_resources.push_back(desc);
		m_compiled = false;

		RGResourceHandle handle{ newId };
		m_resourceNameToHandle[name] = handle;
		return handle;
	}

	RenderGraphPass& RenderGraph::AddPass(const std::string& name)
	{
		const uint32_t passIndex = static_cast<uint32_t>(m_passes.size());
		auto pass = std::make_unique<RenderGraphPass>(name, passIndex);
		RenderGraphPass* rawPassPtr = pass.get();
		m_passes.push_back(std::move(pass));
		m_compiled = false;
		return *rawPassPtr;
	}

	bool RenderGraph::Compile()
	{
		m_compiled = false;
		m_compiledRevisions.clear();
		const std::size_t numPasses = m_passes.size();
		m_executionOrder.clear();

		if (numPasses == 0)
		{
			m_compiled = true;
			return true;
		}

		// 1. 의존성 간선(Edge) 그래프 생성
		// adjList[u] = u 패스 다음에 실행되어야 하는 v 패스 목록 (u -> v)
		std::vector<std::vector<uint32_t>> adjList(numPasses);

		// 리소스별 생산자(Writers) 및 소비자(Readers) 패스 추적
		// resourceWriters[resId] = 이 리소스에 Write를 수행하는 패스 인덱스 목록 (등록 순서대로)
		// resourceReaders[resId] = 이 리소스에 Read를 수행하는 패스 인덱스 목록
		std::unordered_map<uint32_t, std::vector<uint32_t>> resourceWriters;
		std::unordered_map<uint32_t, std::vector<uint32_t>> resourceReaders;

		for (uint32_t i = 0; i < static_cast<uint32_t>(numPasses); ++i)
		{
			const auto& pass = m_passes[i];

			for (const auto& readBinding : pass->GetReads())
			{
				if (!readBinding.handle.IsValid() || readBinding.handle.id >= m_resources.size()) return false;
				auto& passes = resourceReaders[readBinding.handle.id];
				if(passes.empty() || passes.back() != i) passes.push_back(i);
			}

			for (const auto& writeBinding : pass->GetWrites())
			{
				if (!writeBinding.handle.IsValid() || writeBinding.handle.id >= m_resources.size()) return false;
				auto& passes = resourceWriters[writeBinding.handle.id];
				if(passes.empty() || passes.back() != i) passes.push_back(i);
			}
		}

		// A single writer may be declared after its consumers. With multiple
		// writes, declaration order identifies successive resource versions.
		// Readers consume the preceding version and must finish before overwrite.
		for (const auto& [resId, readers] : resourceReaders)
		{
			const auto writerIt = resourceWriters.find(resId);
			if (writerIt == resourceWriters.end()) continue;
			const auto& writers = writerIt->second;
			for (uint32_t reader : readers)
			{
				auto next = std::lower_bound(writers.begin(), writers.end(), reader);
				if (next != writers.begin()) adjList[*(next - 1)].push_back(reader);
				else if (next != writers.end() && *next != reader)
				{
					adjList[*next].push_back(reader);
					++next;
				}
				while (next != writers.end() && *next == reader) ++next;
				if (next != writers.end()) adjList[reader].push_back(*next);
			}
		}

		// (B) WAW (Write After Write) 의존성 생성:
		// 동일한 리소스에 여러 쓰기가 발생하는 경우 등록 순서 유지
		for (const auto& [resId, writers] : resourceWriters)
		{
			for (std::size_t k = 0; k + 1 < writers.size(); ++k)
			{
				uint32_t firstWriter = writers[k];
				uint32_t nextWriter = writers[k + 1];
				if (firstWriter != nextWriter)
				{
					adjList[firstWriter].push_back(nextWriter);
				}
			}
		}

		// 중복 간선(Edge) 제거
		for (uint32_t u = 0; u < static_cast<uint32_t>(numPasses); ++u)
		{
			auto& neighbors = adjList[u];
			std::sort(neighbors.begin(), neighbors.end());
			neighbors.erase(std::unique(neighbors.begin(), neighbors.end()), neighbors.end());
		}

		// inDegree 계산
		std::vector<uint32_t> inDegree(numPasses, 0);
		for (uint32_t u = 0; u < static_cast<uint32_t>(numPasses); ++u)
		{
			for (uint32_t v : adjList[u])
			{
				inDegree[v]++;
			}
		}

		// 2. Kahn's Algorithm 기반 위상 정렬 및 병렬 Stage 분할
		m_executionOrder.clear();
		m_executionStages.clear();
		m_executionOrder.reserve(numPasses);

		std::vector<uint32_t> currentStage;
		for (uint32_t i = 0; i < static_cast<uint32_t>(numPasses); ++i)
		{
			if (inDegree[i] == 0)
			{
				currentStage.push_back(i);
			}
		}

		while (!currentStage.empty())
		{
			// 선언 순서대로 정렬하여 일관된 순서 유지
			std::sort(currentStage.begin(), currentStage.end());

			RGExecutionStage stage;
			stage.passIndices = currentStage;
			m_executionStages.push_back(stage);

			std::vector<uint32_t> nextStage;
			for (uint32_t u : currentStage)
			{
				m_executionOrder.push_back(u);

				for (uint32_t v : adjList[u])
				{
					inDegree[v]--;
					if (inDegree[v] == 0)
					{
						nextStage.push_back(v);
					}
				}
			}

			currentStage = std::move(nextStage);
		}

		// 정렬된 패스의 수가 전체 패스 수와 다르면 순환 의존성(Cycle) 존재
		if (m_executionOrder.size() != numPasses)
		{
			m_executionOrder.clear();
			m_executionStages.clear();
			m_compiled = false;
			return false;
		}

		for (const auto& pass : m_passes) m_compiledRevisions.push_back(pass->GetRevision());
		m_compiled = true;
		return true;
	}

	bool RenderGraph::IsCompiled() const
	{
		if (!m_compiled || m_compiledRevisions.size() != m_passes.size()) return false;
		for (size_t i = 0; i < m_passes.size(); ++i)
			if (m_compiledRevisions[i] != m_passes[i]->GetRevision()) return false;
		return true;
	}

	void RenderGraph::ExecutePassWithBarriers(uint32_t passIndex, RHI::ICommandList* commandList) const
	{
		if (passIndex >= m_passes.size()) return;
		const auto& pass = m_passes[passIndex];

		// 멀티 플랫폼 정석 3대 리소스 배리어 (Texture, Buffer, Global) 방출
		if (commandList != nullptr)
		{
			for (const auto& barrier : pass->GetBarriers())
			{
				const RGResourceDesc* resDesc = GetResourceDesc(barrier.resourceHandle);

				if (barrier.type == RGBarrierType::Texture)
				{
					RHI::ITexture* texPtr = resDesc ? resDesc->texturePtr : nullptr;
					commandList->TextureBarrier(texPtr, static_cast<uint32_t>(barrier.beforeAccess), static_cast<uint32_t>(barrier.afterAccess));
				}
				else if (barrier.type == RGBarrierType::Buffer)
				{
					RHI::IBuffer* bufPtr = resDesc ? resDesc->bufferPtr : nullptr;
					commandList->BufferBarrier(bufPtr, static_cast<uint32_t>(barrier.beforeAccess), static_cast<uint32_t>(barrier.afterAccess));
				}
				else if (barrier.type == RGBarrierType::Global)
				{
					commandList->GlobalBarrier(static_cast<uint32_t>(barrier.beforeAccess), static_cast<uint32_t>(barrier.afterAccess));
				}
			}
		}

		pass->Execute(commandList);
	}

	void RenderGraph::Execute(RHI::ICommandList* commandList)
	{
		if (!IsCompiled())
		{
			if (!Compile())
			{
				return;
			}
		}

		for (uint32_t passIndex : m_executionOrder)
		{
			ExecutePassWithBarriers(passIndex, commandList);
		}
	}

	void RenderGraph::ExecuteParallel(RHI::IDevice* device, Core::ThreadPool* threadPool)
	{
		if (!IsCompiled())
		{
			if (!Compile())
			{
				return;
			}
		}

		// 스레드 풀이 없거나 가용 스레드가 1개 이하인 경우 단일 스레드로 안전하게 폴백
		if (threadPool == nullptr || threadPool->GetThreadCount() <= 1)
		{
			RHI::ICommandList* mainCmd = device ? device->AcquireCommandList() : nullptr;
			Execute(mainCmd);
			if (device && mainCmd)
			{
				mainCmd->Close();
				RHI::ICommandList* submitList[] = { mainCmd };
				device->Submit(submitList, 1);
			}
			return;
		}

		if (device)
		{
			device->ResetCommandLists();
		}

		std::vector<RHI::ICommandList*> stageCmdLists;

		for (const auto& stage : m_executionStages)
		{
			if (stage.passIndices.empty()) continue;

			if (stage.passIndices.size() == 1)
			{
				// 하이브리드 최적화: Stage 내 패스가 1개뿐인 경우 스레드 풀 오버헤드 없이 메인 스레드에서 직접 녹화
				uint32_t passIdx = stage.passIndices[0];
				RHI::ICommandList* cmdList = device ? device->AcquireWorkerCommandList(passIdx) : nullptr;
				ExecutePassWithBarriers(passIdx, cmdList);
				if (cmdList)
				{
					cmdList->Close();
					stageCmdLists.push_back(cmdList);
				}
			}
			else
			{
				// Stage 내 패스가 2개 이상인 경우 워커 스레드 풀에서 병렬 녹화
				const size_t passCount = stage.passIndices.size();
				std::vector<RHI::ICommandList*> parallelCmds(passCount, nullptr);

				for (size_t i = 0; i < passCount; ++i)
				{
					uint32_t passIdx = stage.passIndices[i];
					RHI::ICommandList* workerCmd = device ? device->AcquireWorkerCommandList(passIdx) : nullptr;
					parallelCmds[i] = workerCmd;

					threadPool->Enqueue([this, passIdx, workerCmd]() {
						ExecutePassWithBarriers(passIdx, workerCmd);
						if (workerCmd)
						{
							workerCmd->Close();
						}
					});
				}

				// 해당 Stage의 모든 병렬 패스 녹화 완료 대기
				threadPool->WaitAll();

				for (auto* cmd : parallelCmds)
				{
					if (cmd)
					{
						stageCmdLists.push_back(cmd);
					}
				}
			}
		}

		// 모든 Stage의 녹화가 완료된 커맨드리스트들을 GPU 큐에 순서대로 일괄 제출
		if (device && !stageCmdLists.empty())
		{
			device->Submit(stageCmdLists.data(), static_cast<uint32_t>(stageCmdLists.size()));
		}
	}

	void RenderGraph::Reset()
	{
		m_resources.clear();
		m_resourceNameToHandle.clear();
		m_passes.clear();
		m_compiledRevisions.clear();
		m_executionOrder.clear();
		m_executionStages.clear();
		m_compiled = false;
	}

	std::vector<std::string> RenderGraph::GetExecutionOrderNames() const
	{
		std::vector<std::string> names;
		names.reserve(m_executionOrder.size());
		for (uint32_t idx : m_executionOrder)
		{
			if (idx < m_passes.size())
			{
				names.push_back(m_passes[idx]->GetName());
			}
		}
		return names;
	}

	const RenderGraphPass* RenderGraph::GetPass(uint32_t index) const
	{
		if (index < m_passes.size())
		{
			return m_passes[index].get();
		}
		return nullptr;
	}

	const RGResourceDesc* RenderGraph::GetResourceDesc(RGResourceHandle handle) const
	{
		if (handle.IsValid() && handle.id < m_resources.size())
		{
			return &m_resources[handle.id];
		}
		return nullptr;
	}
}
