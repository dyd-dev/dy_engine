#include "Graphics/RenderGraph.h"
#include "RHI/ICommandList.h"
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

		// 2. Kahn's Algorithm 기반 위상 정렬
		std::vector<uint32_t> zeroInDegreeQueue;
		for (uint32_t i = 0; i < static_cast<uint32_t>(numPasses); ++i)
		{
			if (inDegree[i] == 0)
			{
				zeroInDegreeQueue.push_back(i);
			}
		}

		m_executionOrder.reserve(numPasses);

		while (!zeroInDegreeQueue.empty())
		{
			// 선언 순서(작은 인덱스)를 우선하여 선택
			auto minIt = std::min_element(zeroInDegreeQueue.begin(), zeroInDegreeQueue.end());
			uint32_t u = *minIt;
			zeroInDegreeQueue.erase(minIt);

			m_executionOrder.push_back(u);

			for (uint32_t v : adjList[u])
			{
				inDegree[v]--;
				if (inDegree[v] == 0)
				{
					zeroInDegreeQueue.push_back(v);
				}
			}
		}

		// 정렬된 패스의 수가 전체 패스 수와 다르면 순환 의존성(Cycle) 존재
		if (m_executionOrder.size() != numPasses)
		{
			m_executionOrder.clear();
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
			if (passIndex < m_passes.size())
			{
				m_passes[passIndex]->Execute(commandList);
			}
		}
	}

	void RenderGraph::Reset()
	{
		m_resources.clear();
		m_resourceNameToHandle.clear();
		m_passes.clear();
		m_compiledRevisions.clear();
		m_executionOrder.clear();
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
}
