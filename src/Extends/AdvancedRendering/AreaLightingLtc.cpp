#include "AreaLightingLtc.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/ICommandList.h"
#include "dyf/RHI/Texture.h"
#include "AreaLtcMatrix.h"
#include "AreaLtcAmplitude.h"
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace dyf
{
namespace
{
// 원저자 LTC 표와 배포 조건은 Assets/ltc/LICENSE에 보존한다.
// 논문: Heitz 외, Real-Time Polygonal-Light Shading with Linearly Transformed Cosines, SIGGRAPH 2016.
// 출처: https://github.com/selfshadow/ltc_code/tree/31e5e96b54f98f33098f8503003119ba2231a1c6
uint32_t LittleEndian32(const uint8_t* bytes)
{
    return uint32_t(bytes[0]) | (uint32_t(bytes[1])<<8) | (uint32_t(bytes[2])<<16) | (uint32_t(bytes[3])<<24);
}

bool UploadTable(RHI::IDevice& device,RHI::ICommandList& commands,RHI::TextureHandle& texture,
    const uint8_t* bytes,size_t size)
{
    constexpr uint32_t dimension=64,headerSize=128,rowPitch=dimension*8,payloadSize=rowPitch*dimension;
    // 일반 DDS 로더가 아니라 원저자의 64² RGBA16F·FourCC113 표만 받는다.
    if(size<headerSize+payloadSize || LittleEndian32(bytes)!=0x20534444 || LittleEndian32(bytes+4)!=124 ||
        LittleEndian32(bytes+12)!=dimension || LittleEndian32(bytes+16)!=dimension ||
        LittleEndian32(bytes+76)!=32 || LittleEndian32(bytes+84)!=113)
    {
        std::fputs("dyf: Invalid authored LTC lookup table.\n",stderr);return false;
    }
    RHI::TextureDesc desc;
    desc.width=desc.height=dimension;desc.format=RHI::Format::R16G16B16A16_FLOAT;
    desc.usage=RHI::TextureUsage::ShaderResource;
    texture=device.CreateTexture(desc);
    if(!texture) return false;
    const RHI::ResourceBarrierDesc before{nullptr,texture,RHI::ResourceState::Undefined,RHI::ResourceState::CopyDestination,{}};
    commands.ResourceBarrier(&before,1);
    if(!device.UpdateTexture(commands,texture,0,0,bytes+headerSize,payloadSize,rowPitch,payloadSize)) return false;
    const RHI::ResourceBarrierDesc after{nullptr,texture,RHI::ResourceState::CopyDestination,RHI::ResourceState::ShaderResource,{}};
    commands.ResourceBarrier(&after,1);
    return true;
}
}

AreaLightingLtc::~AreaLightingLtc() { Reset(); }

void AreaLightingLtc::Reset()
{
    if(m_device) {m_device->DestroyTexture(m_matrix);m_device->DestroyTexture(m_amplitude);}
    m_device=nullptr;m_matrix=m_amplitude=nullptr;
    m_matrixHandle=m_amplitudeHandle={};m_uploaded=m_prepared=false;
}

bool AreaLightingLtc::Prepare(RHI::IDevice& device,RHI::ICommandList& uploads)
{
    if(m_device && m_device!=&device) Reset();
    m_device=&device;m_prepared=false;
    if(!m_uploaded)
    {
        if(m_matrix || m_amplitude) return false;
        if(!UploadTable(device,uploads,m_matrix,kAreaLtcMatrix,kAreaLtcMatrixSize) ||
            !UploadTable(device,uploads,m_amplitude,kAreaLtcAmplitude,kAreaLtcAmplitudeSize)) return false;
    }
    m_prepared=true;return true;
}

bool AreaLightingLtc::Import(RHI::RenderGraph& graph)
{
    if(!m_prepared) return false;
    m_matrixHandle=graph.ImportTexture("Area LTC inverse matrix",m_matrix,RHI::ResourceState::ShaderResource,RHI::ResourceState::ShaderResource);
    m_amplitudeHandle=graph.ImportTexture("Area LTC GGX amplitude Fresnel",m_amplitude,RHI::ResourceState::ShaderResource,RHI::ResourceState::ShaderResource);
    return m_matrixHandle.IsValid() && m_amplitudeHandle.IsValid();
}

void AreaLightingLtc::CommitFrame()
{
    if(m_prepared)m_uploaded=true;
    m_prepared=false;
}
}
