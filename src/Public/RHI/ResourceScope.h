#pragma once

#include "IDevice.h"
#include <stdexcept>
#include <vector>

namespace dy::RHI
{
// Owns explicitly supplied resources. The device must outlive the scope; recorded
// commands must be submitted or discarded before their resources are released.
// Allocation, descriptors, uploads, barriers and submission remain caller-owned.
class ResourceScope
{
public:
    explicit ResourceScope(IDevice& device) : m_device(device) {}
    ResourceScope(const ResourceScope&) = delete;
    ResourceScope& operator=(const ResourceScope&) = delete;

    ~ResourceScope()
    {
        for (auto it = m_sets.rbegin(); it != m_sets.rend(); ++it) m_device.DestroyResourceSet(*it);
        for (auto it = m_pipelines.rbegin(); it != m_pipelines.rend(); ++it) m_device.DestroyPipeline(*it);
        for (auto it = m_shaders.rbegin(); it != m_shaders.rend(); ++it) m_device.DestroyShader(*it);
        for (auto it = m_textures.rbegin(); it != m_textures.rend(); ++it) m_device.DestroyTexture(*it);
        for (auto it = m_buffers.rbegin(); it != m_buffers.rend(); ++it) m_device.DestroyBuffer(*it);
    }

    // Takes ownership exactly once. Null handles report allocation failure.
    // Swapchain-owned textures must not be adopted.
    BufferHandle Keep(BufferHandle value) { return Keep(value, m_buffers, &IDevice::DestroyBuffer); }
    TextureHandle Keep(TextureHandle value) { return Keep(value, m_textures, &IDevice::DestroyTexture); }
    ShaderHandle Keep(ShaderHandle value) { return Keep(value, m_shaders, &IDevice::DestroyShader); }
    PipelineHandle Keep(PipelineHandle value) { return Keep(value, m_pipelines, &IDevice::DestroyPipeline); }
    ResourceSetHandle Keep(ResourceSetHandle value) { return Keep(value, m_sets, &IDevice::DestroyResourceSet); }

private:
    template<class Handle>
    Handle Keep(Handle value, std::vector<Handle>& handles, void (IDevice::*destroy)(Handle))
    {
        if (!value) throw std::runtime_error("RHI resource creation failed.");
        try { handles.push_back(value); }
        catch (...) { (m_device.*destroy)(value); throw; }
        return value;
    }

    IDevice& m_device;
    std::vector<BufferHandle> m_buffers;
    std::vector<TextureHandle> m_textures;
    std::vector<ShaderHandle> m_shaders;
    std::vector<PipelineHandle> m_pipelines;
    std::vector<ResourceSetHandle> m_sets;
};
}
