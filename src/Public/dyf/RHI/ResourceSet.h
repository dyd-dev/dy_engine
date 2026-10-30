#pragma once

#include "dyf/RHI/Binding.h"
#include <vector>

namespace dyf::RHI
{
// Immutable binding metadata shared by the recorder and native API adapters.
class ResourceSet
{
public:
    struct StateRequirement
    {
        BufferHandle buffer=nullptr;
        TextureHandle texture=nullptr;
        ResourceState state=ResourceState::Undefined;
        uint32_t firstMip=0,mipCount=1,firstLayer=0,layerCount=1;
    };
    ResourceSet(const ResourceSet&) = delete;
    ResourceSet& operator=(const ResourceSet&) = delete;
    [[nodiscard]] PipelineHandle GetPipeline() const { return m_pipeline; }
    [[nodiscard]] const ResourceBinding* GetBindings() const { return m_bindings.data(); }
    [[nodiscard]] uint32_t GetBindingCount() const { return static_cast<uint32_t>(m_bindings.size()); }
    [[nodiscard]] const std::vector<StateRequirement>& GetStateRequirements() const { return m_stateRequirements; }

protected:
    virtual ~ResourceSet() = default;
    explicit ResourceSet(const ResourceSetDesc& desc);

private:
    PipelineHandle m_pipeline = nullptr;
    std::vector<ResourceBinding> m_bindings;
    std::vector<StateRequirement> m_stateRequirements;
};
}
