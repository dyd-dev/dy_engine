#include "EnvironmentDepthCapture.h"
#include "dyf/Camera.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/ICommandList.h"
#include "dyf/RHI/Pipeline.h"
#include "dyf/RHI/Shader.h"
#include "dyf/RHI/Texture.h"
#include "dyf/RHI/Readback.h"
#include "dyf/RHI/RenderGraph.h"
#include "dyf/RHI/ResourceScope.h"
#include "../RHI/ResourceSet.h"
#include <cmath>
#include <cstring>

#if defined(ENABLE_METAL)
#include "EnvironmentDepthLibrary.h"
#elif defined(ENABLE_VULKAN) || defined(ENABLE_D3D12)
#include "ToneMapVertex.h"
#include "EnvironmentDepthFragment.h"
#endif

namespace dyf
{
EnvironmentDepthCapture::~EnvironmentDepthCapture()
{
    m_device.DestroyResourceSet(m_resources);
    m_device.DestroyPipeline(m_pipeline);
    m_device.DestroyShader(m_vertex);m_device.DestroyShader(m_fragment);
    m_device.DestroyTexture(m_target);
}

bool EnvironmentDepthCapture::Prepare(RHI::TextureHandle depth)
{
    using namespace RHI;
    if(!depth) return false;
    const auto& size=depth->GetDesc();
    if(!m_pipeline)
    {
#if defined(ENABLE_METAL)
        if(!m_vertex) m_vertex=m_device.CreateShader({ShaderStage::Vertex,"environmentDepthVertex",kEnvironmentDepthLibrary,kEnvironmentDepthLibrarySize});
        if(!m_fragment) m_fragment=m_device.CreateShader({ShaderStage::Fragment,"environmentDepthFragment",kEnvironmentDepthLibrary,kEnvironmentDepthLibrarySize});
#elif defined(ENABLE_VULKAN) || defined(ENABLE_D3D12)
        if(!m_vertex) m_vertex=m_device.CreateShader({ShaderStage::Vertex,"main",kToneMapVertex,kToneMapVertexSize});
        if(!m_fragment) m_fragment=m_device.CreateShader({ShaderStage::Fragment,"main",kEnvironmentDepthFragment,kEnvironmentDepthFragmentSize});
#endif
        if(!m_vertex || !m_fragment) return false;
        SamplerDesc sampler;
        sampler.minFilter=sampler.magFilter=sampler.mipFilter=SamplerFilter::Nearest;
        sampler.addressU=sampler.addressV=sampler.addressW=SamplerAddressMode::ClampToEdge;
        sampler.minLod=sampler.maxLod=0;
        const ResourceBindingLayout bindings[]={
            {0,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}},
            {1,ResourceBindingType::StaticSampler,1,ShaderStageFlags::Fragment,sampler}};
        const ColorAttachmentDesc color{Format::R8G8B8A8_UNORM,{},ColorWriteMask::All};
        GraphicsPipelineDesc desc;
        desc.vertexShader=m_vertex;desc.fragmentShader=m_fragment;
        desc.topology=PrimitiveTopology::TriangleList;
        desc.raster={FillMode::Solid,CullMode::None,FrontFace::CounterClockwise,0,0,0};
        desc.colorAttachments=&color;desc.colorAttachmentCount=1;
        desc.layout={bindings,2,16,ShaderStageFlags::Fragment,15};
        m_pipeline=m_device.CreateGraphicsPipeline(desc);
        if(!m_pipeline) return false;
    }
    if(!m_target || m_target->GetDesc().width!=size.width || m_target->GetDesc().height!=size.height)
    {
        TextureDesc desc;
        desc.width=size.width;desc.height=size.height;desc.format=Format::R8G8B8A8_UNORM;
        desc.usage=TextureUsage::RenderTarget|TextureUsage::ShaderResource;
        const auto target=m_device.CreateTexture(desc);
        if(!target) return false;
        m_device.DestroyTexture(m_target);m_target=target;m_state=ResourceState::Undefined;
    }
    if(m_depth!=depth)
    {
        m_device.DestroyResourceSet(m_resources);m_resources=nullptr;m_depth=depth;
    }
    if(!m_resources)
    {
        const ResourceBinding binding{0,0,nullptr,depth,0,0,{}};
        m_resources=m_device.CreateResourceSet({m_pipeline,&binding,1});
    }
    return m_resources!=nullptr;
}

bool EnvironmentDepthCapture::Read(RHI::TextureHandle depth,const Camera& camera,float nearPlane,float farPlane,
    std::vector<float>& distance)
{
    using namespace RHI;
    if(!std::isfinite(nearPlane) || !std::isfinite(farPlane) || nearPlane<=0 || farPlane<=nearPlane ||
        !std::isfinite(camera.projection.m[0]) || !std::isfinite(camera.projection.m[5]) ||
        camera.projection.m[0]==0 || camera.projection.m[5]==0) return false;
    if(!Prepare(depth)) return false;
    // 원근 깊이를 직접 거리로 바꿔 먼 평면의 동차 나눗셈과 큰 월드 좌표의 상쇄를 피한다.
    const Math::float4 constants{1/camera.projection.m[0],1/camera.projection.m[5],nearPlane,farPlane};
    static_assert(sizeof(constants)==16);
    RenderGraph graph;
    const auto input=graph.ImportTexture("Probe source depth",depth,ResourceState::ShaderResource,ResourceState::ShaderResource);
    const auto output=graph.ImportTexture("Probe distance bits",m_target,m_state,ResourceState::ShaderResource);
    if(!input.IsValid() || !output.IsValid()) return false;
    graph.AddPass("Probe radial distance capture",RGRecordingMode::Independent)
        .Read(input,ResourceState::ShaderResource).Write(output,ResourceState::RenderTarget).SetPipeline(m_pipeline)
        .SetExecute([this,constants](ICommandList* commands) {
            const auto& size=m_target->GetDesc();
            ColorAttachment color;color.texture=m_target;color.loadOp=LoadOp::Discard;color.storeOp=StoreOp::Store;
            commands->BeginRendering({&color,1,nullptr});
            commands->BindGraphicsPipeline(m_pipeline);commands->BindResourceSet(m_resources);
            commands->SetViewport({0,0,float(size.width),float(size.height),0,1});
            commands->SetScissor({0,0,size.width,size.height});
            commands->SetInlineConstants(0,sizeof(constants),&constants);
            commands->DrawInstanced(3,1,0,0);commands->EndRendering();
        });
    if(!graph.Compile()) return false;
    ResourceScope resources(m_device);
    auto* commands=resources.Keep(m_device.AcquireCommandList());
    if(!commands || !graph.Execute(commands) || !commands->Close() || !m_device.Submit(&commands,1)) return false;
    m_state=ResourceState::ShaderResource;
    TextureReadback readback;
    if(!m_device.ReadTexture(m_target,readback)) return false;
    const bool bgra=readback.format==Format::B8G8R8A8_UNORM;
    const auto& size=m_target->GetDesc();
    if((readback.format!=Format::R8G8B8A8_UNORM && !bgra) || readback.width!=size.width ||
        readback.height!=size.height || readback.rowPitch<uint64_t(size.width)*4 ||
        uint64_t(size.height-1)*readback.rowPitch+uint64_t(size.width)*4>readback.pixels.size()) return false;
    distance.resize(size_t(size.width)*size.height);
    for(uint32_t y=0;y<size.height;++y) for(uint32_t x=0;x<size.width;++x)
    {
        const auto* pixel=readback.pixels.data()+size_t(y)*readback.rowPitch+size_t(x)*4;
        // RGBA8는 색이 아니라 float32 비트를 보존한다. 노출·감마와 무관한 실제 거리다.
        const uint32_t bits=uint32_t(pixel[bgra?2:0])|(uint32_t(pixel[1])<<8)|
            (uint32_t(pixel[bgra?0:2])<<16)|(uint32_t(pixel[3])<<24);
        auto& value=distance[size_t(y)*size.width+x];
        std::memcpy(&value,&bits,sizeof(value));
        if(!std::isfinite(value) || value<0) return false;
    }
    return true;
}
}
