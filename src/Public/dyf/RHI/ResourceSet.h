#pragma once

#include <cstdint>
#include <vector>

#include "Binding.h"
#include "ResourceHandles.h"

namespace dyf::RHI
{
	struct ResourceSetDesc
	{
		// RHI retains the pipeline and binding resources for this set.
		PipelineHandle pipeline = nullptr;
		const ResourceBinding* bindings = nullptr;
		uint32_t bindingCount = 0;
	};

	class ResourceSet
	{
	public:
		[[nodiscard]] PipelineHandle GetPipeline() const { return m_pipeline; }
		[[nodiscard]] const ResourceBinding* GetBindings() const { return m_bindings.data(); }
		[[nodiscard]] uint32_t GetBindingCount() const { return static_cast<uint32_t>(m_bindings.size()); }

	protected:
		virtual ~ResourceSet() = default;
		explicit ResourceSet(const ResourceSetDesc& desc)
			: m_pipeline(desc.pipeline)
		{
			if(desc.bindings != nullptr && desc.bindingCount != 0)
			{
				m_bindings.assign(desc.bindings, desc.bindings + desc.bindingCount);
			}
		}

	private:
		PipelineHandle m_pipeline = nullptr;
		std::vector<ResourceBinding> m_bindings;
	};
}
