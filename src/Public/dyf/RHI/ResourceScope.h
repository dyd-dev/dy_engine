#pragma once

#include "dyf/RHI/IDevice.h"
#include <stdexcept>
#include <vector>

namespace dyf::RHI
{
// 명시적으로 등록한 자원만 소유하며 장치는 이 객체보다 오래 유지해야 한다.
// 기록된 네이티브 명령은 GPU 작업이 끝날 때까지 참조 자원을 유지한다.
// 자원 생성·설정·업로드·배리어·제출은 호출자가 담당한다.
class ResourceScope
{
public:
    explicit ResourceScope(IDevice& device) : m_device(device) {}
    ResourceScope(const ResourceScope&) = delete;
    ResourceScope& operator=(const ResourceScope&) = delete;

    ~ResourceScope()
    {
        for(auto* query:m_queries) m_device.DestroyTimestampQuery(query);
        for(auto* commands:m_commands) m_device.DestroyCommandList(commands);
        for (auto it = m_sets.rbegin(); it != m_sets.rend(); ++it) m_device.DestroyResourceSet(*it);
        for (auto it = m_pipelines.rbegin(); it != m_pipelines.rend(); ++it) m_device.DestroyPipeline(*it);
        for (auto it = m_shaders.rbegin(); it != m_shaders.rend(); ++it) m_device.DestroyShader(*it);
        for (auto it = m_textures.rbegin(); it != m_textures.rend(); ++it) m_device.DestroyTexture(*it);
        for (auto it = m_buffers.rbegin(); it != m_buffers.rend(); ++it) m_device.DestroyBuffer(*it);
    }

    // 소유권은 한 번만 등록하고 빈 핸들은 생성 실패로 처리한다.
    // 스왑체인이 소유한 텍스처는 등록하지 않는다.
    ICommandList* Keep(ICommandList* value) { return Keep(value,m_commands,&IDevice::DestroyCommandList); }
    TimestampQueryHandle Keep(TimestampQueryHandle value) {return Keep(value,m_queries,&IDevice::DestroyTimestampQuery);}
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
    std::vector<TimestampQueryHandle> m_queries;
    std::vector<ICommandList*> m_commands;
    std::vector<BufferHandle> m_buffers;
    std::vector<TextureHandle> m_textures;
    std::vector<ShaderHandle> m_shaders;
    std::vector<PipelineHandle> m_pipelines;
    std::vector<ResourceSetHandle> m_sets;
};
}
