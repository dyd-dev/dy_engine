#include "OutputPresentation.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/ICommandList.h"
#include "dyf/RHI/Pipeline.h"
#include "dyf/RHI/Binding.h"
#include "dyf/RHI/Shader.h"
#include "dyf/RHI/Texture.h"
#include <algorithm>
#include <cmath>
#include <limits>

#if defined(ENABLE_METAL)
#include "PresentationCopyLibrary.h"
#elif defined(ENABLE_D3D12) || defined(ENABLE_VULKAN)
#include "ToneMapVertex.h"
#include "PresentationCopyFragment.h"
#endif

namespace dyf
{
OutputPresentation::~OutputPresentation()
{
    m_device.DestroyResourceSet(m_resources);
    m_device.DestroyPipeline(m_pipeline);
    m_device.DestroyShader(m_vertex);
    m_device.DestroyShader(m_fragment);
    m_device.DestroyTexture(m_target);
}

bool OutputPresentation::PreparePipeline(RHI::Format format)
{
    using namespace RHI;
    if(m_pipeline && m_format==format) return true;
#if defined(ENABLE_METAL)
    if(!m_vertex) m_vertex=m_device.CreateShader({ShaderStage::Vertex,"presentationVertex",kPresentationCopyLibrary,kPresentationCopyLibrarySize});
    if(!m_fragment) m_fragment=m_device.CreateShader({ShaderStage::Fragment,"presentationFragment",kPresentationCopyLibrary,kPresentationCopyLibrarySize});
#elif defined(ENABLE_D3D12) || defined(ENABLE_VULKAN)
    // 사용자 셰이더 재정의와 무관하게 기본 전체 화면 정점 셰이더를 재사용한다.
    if(!m_vertex) m_vertex=m_device.CreateShader({ShaderStage::Vertex,"main",kToneMapVertex,kToneMapVertexSize});
    if(!m_fragment) m_fragment=m_device.CreateShader({ShaderStage::Fragment,"main",kPresentationCopyFragment,kPresentationCopyFragmentSize});
#endif
    if(!m_vertex || !m_fragment) return false;

    SamplerDesc sampler;
    sampler.minFilter=sampler.magFilter=SamplerFilter::Linear;
    sampler.mipFilter=SamplerFilter::Nearest;
    sampler.addressU=sampler.addressV=sampler.addressW=SamplerAddressMode::ClampToEdge;
    sampler.mipLodBias=sampler.minLod=sampler.maxLod=0;
    const ResourceBindingLayout bindings[]={
        {0,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}},
        {1,ResourceBindingType::StaticSampler,1,ShaderStageFlags::Fragment,sampler}};
    const ColorAttachmentDesc color{format,{},ColorWriteMask::All};
    GraphicsPipelineDesc desc;
    desc.vertexShader=m_vertex;
    desc.fragmentShader=m_fragment;
    desc.topology=PrimitiveTopology::TriangleList;
    desc.raster={FillMode::Solid,CullMode::None,FrontFace::CounterClockwise,0,0,0};
    desc.colorAttachments=&color;
    desc.colorAttachmentCount=1;
    desc.layout={bindings,2,0,ShaderStageFlags::None,0};
    const auto pipeline=m_device.CreateGraphicsPipeline(desc);
    if(!pipeline) return false;

    // 비행 중인 세트와 파이프라인은 공통 RHI의 참조 수명으로 회수한다.
    m_device.DestroyResourceSet(m_resources);
    m_resources=nullptr;
    m_device.DestroyPipeline(m_pipeline);
    m_pipeline=pipeline;
    m_format=format;
    return true;
}

bool OutputPresentation::Prepare(uint32_t width,uint32_t height,RHI::Format format)
{
    using namespace RHI;
    m_pending=false;
    const auto limit=m_device.GetLimit(Limit::Texture2DDimension);
    if(!width || !height || width>limit || height>limit) return false;
    if(!PreparePipeline(format)) return false;

    if(!m_target || m_target->GetDesc().width!=width || m_target->GetDesc().height!=height || m_target->GetDesc().format!=format)
    {
        TextureDesc desc;
        desc.width=width;
        desc.height=height;
        desc.format=format;
        desc.usage=TextureUsage::RenderTarget|TextureUsage::ShaderResource;
        const auto target=m_device.CreateTexture(desc);
        if(!target) return false;
        m_device.DestroyResourceSet(m_resources);
        m_resources=nullptr;
        m_device.DestroyTexture(m_target);
        m_target=target;
        m_state=ResourceState::Undefined;
    }
    if(!m_resources)
    {
        ResourceBinding image;
        image.binding=0;
        image.texture=m_target;
        m_resources=m_device.CreateResourceSet({m_pipeline,&image,1});
    }
    return m_resources!=nullptr;
}

bool OutputPresentation::AddPass(RHI::RenderGraph& graph,RHI::RGResourceHandle source,RHI::TextureHandle actualBackBuffer)
{
    using namespace RHI;
    if(!source.IsValid() || !actualBackBuffer || actualBackBuffer==m_target || !m_target || !m_pipeline || !m_resources) return false;
    const auto& output=actualBackBuffer->GetDesc();
    const auto& input=m_target->GetDesc();
    if(!output.width || !output.height || output.format!=m_format ||
        output.width>std::numeric_limits<int32_t>::max() || output.height>std::numeric_limits<int32_t>::max()) return false;

    // 한쪽 크기를 창에 맞추고 다른 쪽은 원본 종횡비로 계산한다.
    double width=output.width,height=output.height;
    if(uint64_t(output.width)*input.height<=uint64_t(output.height)*input.width)
        height=double(output.width)*input.height/input.width;
    else
        width=double(output.height)*input.width/input.height;
    // 원점을 픽셀에 맞춰 원본 크기 표시에서 반 픽셀 이동에 의한 흐림을 피한다.
    const auto x=static_cast<int32_t>(std::floor((output.width-width)*.5));
    const auto y=static_cast<int32_t>(std::floor((output.height-height)*.5));
    const Viewport viewport{float(x),float(y),float(width),float(height),0,1};
    const Rect scissor{x,y,
        static_cast<uint32_t>(std::min(double(output.width-x),std::ceil(width))),
        static_cast<uint32_t>(std::min(double(output.height-y),std::ceil(height)))};
    const auto destination=graph.ImportTexture("Presentation backbuffer",actualBackBuffer,ResourceState::Present,ResourceState::Present);
    if(!destination.IsValid()) return false;
    const auto pipeline=m_pipeline;
    const auto resources=m_resources;
    graph.AddPass("Output presentation",RGRecordingMode::Independent)
        .Read(source,ResourceState::ShaderResource)
        .Write(destination,ResourceState::RenderTarget)
        .SetPipeline(pipeline)
        .SetExecute([actualBackBuffer,pipeline,resources,viewport,scissor](ICommandList* commands) {
            ColorAttachment color;
            color.texture=actualBackBuffer;
            color.loadOp=LoadOp::Clear;
            color.storeOp=StoreOp::Store;
            color.clearColor[3]=1;
            commands->BeginRendering({&color,1,nullptr});
            commands->BindGraphicsPipeline(pipeline);
            commands->BindResourceSet(resources);
            commands->SetViewport(viewport);
            commands->SetScissor(scissor);
            commands->DrawInstanced(3,1,0,0);
            commands->EndRendering();
        });
    m_pending=true;
    return true;
}

void OutputPresentation::CommitFrame()
{
    if(m_pending) m_state=RHI::ResourceState::ShaderResource;
    m_pending=false;
}
}
