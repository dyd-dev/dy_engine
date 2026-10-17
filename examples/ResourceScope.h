#pragma once

#include "dyf/RHI/IDevice.h"
#include <stdexcept>
#include <vector>

namespace example
{
namespace RHI = dyf::RHI;

// Example-owned cleanup using only public RHI creation/destruction contracts.
// The device must outlive this scope. Adopt each owned handle exactly once.
class ResourceScope
{
public:
    explicit ResourceScope(RHI::IDevice& device) : m_device(device) {}
    ResourceScope(const ResourceScope&) = delete;
    ResourceScope& operator=(const ResourceScope&) = delete;

    ~ResourceScope()
    {
        for(auto* query : m_queries) m_device.DestroyTimestampQuery(query);
        for(auto* commands : m_commands) m_device.DestroyCommandList(commands);
        for(auto it = m_sets.rbegin(); it != m_sets.rend(); ++it) m_device.DestroyResourceSet(*it);
        for(auto it = m_pipelines.rbegin(); it != m_pipelines.rend(); ++it) m_device.DestroyPipeline(*it);
        for(auto it = m_shaders.rbegin(); it != m_shaders.rend(); ++it) m_device.DestroyShader(*it);
        for(auto it = m_textures.rbegin(); it != m_textures.rend(); ++it) m_device.DestroyTexture(*it);
        for(auto it = m_buffers.rbegin(); it != m_buffers.rend(); ++it) m_device.DestroyBuffer(*it);
    }

    RHI::ICommandList* Keep(RHI::ICommandList* value) { return Keep(value, m_commands, &RHI::IDevice::DestroyCommandList); }
    RHI::TimestampQueryHandle Keep(RHI::TimestampQueryHandle value) { return Keep(value, m_queries, &RHI::IDevice::DestroyTimestampQuery); }
    RHI::BufferHandle Keep(RHI::BufferHandle value) { return Keep(value, m_buffers, &RHI::IDevice::DestroyBuffer); }
    RHI::TextureHandle Keep(RHI::TextureHandle value) { return Keep(value, m_textures, &RHI::IDevice::DestroyTexture); }
    RHI::ShaderHandle Keep(RHI::ShaderHandle value) { return Keep(value, m_shaders, &RHI::IDevice::DestroyShader); }
    RHI::PipelineHandle Keep(RHI::PipelineHandle value) { return Keep(value, m_pipelines, &RHI::IDevice::DestroyPipeline); }
    RHI::ResourceSetHandle Keep(RHI::ResourceSetHandle value) { return Keep(value, m_sets, &RHI::IDevice::DestroyResourceSet); }

private:
    template<class Handle>
    Handle Keep(Handle value, std::vector<Handle>& handles, void (RHI::IDevice::*destroy)(Handle))
    {
        if(!value) throw std::runtime_error("RHI resource creation failed.");
        try { handles.push_back(value); }
        catch(...) { (m_device.*destroy)(value); throw; }
        return value;
    }

    RHI::IDevice& m_device;
    std::vector<RHI::TimestampQueryHandle> m_queries;
    std::vector<RHI::ICommandList*> m_commands;
    std::vector<RHI::BufferHandle> m_buffers;
    std::vector<RHI::TextureHandle> m_textures;
    std::vector<RHI::ShaderHandle> m_shaders;
    std::vector<RHI::PipelineHandle> m_pipelines;
    std::vector<RHI::ResourceSetHandle> m_sets;
};
}
