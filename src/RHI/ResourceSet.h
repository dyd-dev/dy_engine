#pragma once

#include "dyf/RHI/ResourceSet.h"
#include <vector>

namespace dyf::RHI
{
class ResourceSet
{
public:
    ResourceSet(const ResourceSet&) = delete;
    ResourceSet& operator=(const ResourceSet&) = delete;
    [[nodiscard]] PipelineHandle GetPipeline() const { return m_pipeline; }
    [[nodiscard]] const ResourceBinding* GetBindings() const { return m_bindings.data(); }
    [[nodiscard]] uint32_t GetBindingCount() const { return static_cast<uint32_t>(m_bindings.size()); }

protected:
    virtual ~ResourceSet() = default;
    explicit ResourceSet(const ResourceSetDesc& desc) : m_pipeline(desc.pipeline)
    {
        if(desc.bindings && desc.bindingCount)
            m_bindings.assign(desc.bindings, desc.bindings + desc.bindingCount);
    }

private:
    PipelineHandle m_pipeline = nullptr;
    std::vector<ResourceBinding> m_bindings;
};
}
