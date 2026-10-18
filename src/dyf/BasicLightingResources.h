#pragma once

#include "LocalLightData.h"
#include "dyf/RenderExtension.h"
#include "dyf/RHI/Buffer.h"
#include <array>

namespace dyf
{
struct Camera;

// 기본 셰이더에 전체 local-light 목록만 제공한다. 공간 분류와 compute는 확장의 책임이다.
class BasicLightingResources
{
public:
    BasicLightingResources()=default;
    BasicLightingResources(const BasicLightingResources&)=delete;
    BasicLightingResources& operator=(const BasicLightingResources&)=delete;
    ~BasicLightingResources();

    // 업로드 기록이나 제출이 실패하면 Reset 뒤 다시 준비한다.
    bool Prepare(RHI::IDevice&,RHI::ICommandList&,const Scene&,const Camera&,bool active);
    bool Import(RHI::RenderGraph&,RenderLightingResources&);
    void Reset();
    RHI::BufferHandle GetLights() const {return m_buffers[0].buffer;}
    RHI::BufferHandle GetList() const {return m_buffers[1].buffer;}
    RHI::BufferHandle GetConstants() const {return m_buffers[2].buffer;}

private:
    struct Buffer
    {
        RHI::BufferHandle buffer=nullptr;
        std::vector<uint8_t> bytes;
        bool ready=false;
    };
    bool Upload(RHI::ICommandList&,uint32_t slot,const void*,uint32_t bytes,
        uint32_t stride,RHI::BufferUsage,RHI::ResourceState);
    RHI::IDevice* m_device=nullptr;
    std::array<Buffer,3> m_buffers;
    LocalLightData m_localLights;
    RendererClusterConstants m_data={};
    bool m_prepared=false;
};
}
