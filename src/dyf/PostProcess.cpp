#include "RendererInternal.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/ICommandList.h"
#include "dyf/RHI/Texture.h"
#include "dyf/RHI/Pipeline.h"
#include "../RHI/ResourceSet.h"
#include "dyf/RHI/Shader.h"
#include "OutputColor.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
namespace dyf
{
// HDR과 톤 매핑은 별도 소유 클래스 없이 RHI 텍스처와 전체 화면 draw로 구성한다.
bool Renderer::Impl::PreparePostProcess(RHI::TextureHandle output)
{
    using namespace RHI;
    const auto width=static_cast<uint64_t>(std::ceil(output->GetDesc().width*static_cast<double>(config.hdrRenderScale)));
    const auto height=static_cast<uint64_t>(std::ceil(output->GetDesc().height*static_cast<double>(config.hdrRenderScale)));
    const auto limit=std::min<uint64_t>(device->GetLimit(Limit::Texture2DDimension),std::numeric_limits<uint32_t>::max());
    if(width>limit || height>limit)
    {
        std::fprintf(stderr,"dyf: HDR render scale %.2f requests %llu x %llu, exceeding texture dimension limit %llu.\n",
            config.hdrRenderScale,static_cast<unsigned long long>(width),static_cast<unsigned long long>(height),
            static_cast<unsigned long long>(limit));
        return false;
    }
    if(tonePipeline && toneColorFormat!=output->GetDesc().format)
    {device->DestroyResourceSet(toneSet);toneSet=nullptr;device->DestroyPipeline(tonePipeline);tonePipeline=nullptr;}
    // Advanced rendering supplies its own final display pass. Both paths still
    // need the HDR target below; create the stock tone pipeline only when used.
    if(!HasOutputExtension() && !tonePipeline)
    {
        const auto shaders=DefaultShaders();
        if(!toneVertexShader) toneVertexShader=device->CreateShader(ShaderDescription(ToneVertex,shaders.toneMapVertex));
        if(!toneFragmentShader) toneFragmentShader=device->CreateShader(ShaderDescription(ToneFragment,shaders.toneMapFragment));
        if(!toneVertexShader || !toneFragmentShader)return false;
        SamplerDesc sampler;
        sampler.minFilter=sampler.magFilter=SamplerFilter::Linear;
        sampler.mipFilter=SamplerFilter::Nearest;
        sampler.addressU=sampler.addressV=sampler.addressW=SamplerAddressMode::ClampToEdge;
        sampler.mipLodBias=sampler.minLod=sampler.maxLod=0;
        const std::array<ResourceBindingLayout,2> bindings={{{0,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}},
            {1,ResourceBindingType::StaticSampler,1,ShaderStageFlags::Fragment,sampler}}};
        const ColorAttachmentDesc color{output->GetDesc().format,{},ColorWriteMask::All};
        GraphicsPipelineDesc desc;
        desc.vertexShader=toneVertexShader;desc.fragmentShader=toneFragmentShader;desc.topology=PrimitiveTopology::TriangleList;
        desc.raster={FillMode::Solid,CullMode::None,FrontFace::CounterClockwise,0,0,0};
        desc.colorAttachments=&color;desc.colorAttachmentCount=1;
        desc.layout={bindings.data(),static_cast<uint32_t>(bindings.size()),96,ShaderStageFlags::Fragment,15};
        tonePipeline=device->CreateGraphicsPipeline(desc);
        if(!tonePipeline)return false;
        toneColorFormat=output->GetDesc().format;
    }
    if(hdrTarget && (hdrTarget->GetDesc().width!=width || hdrTarget->GetDesc().height!=height))
    {device->DestroyResourceSet(toneSet);toneSet=nullptr;device->DestroyTexture(hdrTarget);hdrTarget=nullptr;}
    if(!hdrTarget)
    {
        TextureDesc desc;
        desc.width=static_cast<uint32_t>(width);desc.height=static_cast<uint32_t>(height);
        desc.format=Format::R16G16B16A16_FLOAT;
        desc.usage=TextureUsage::RenderTarget|TextureUsage::ShaderResource;
        hdrTarget=device->CreateTexture(desc);
        hdrState=ResourceState::Undefined;
    }
    return hdrTarget!=nullptr;
}
bool Renderer::Impl::RecordToneMap(RHI::ICommandList& commands,RHI::TextureHandle output,float exposure,RHI::TextureHandle source)
{
    using namespace RHI;
    ResourceBinding image;image.binding=0;image.texture=source ? source : hdrTarget;
    // Immutable descriptors may remain referenced by earlier submissions. Retire
    // the previous owner through RHI; never mutate a descriptor in flight.
    if(toneSet && (toneSet->GetPipeline()!=tonePipeline || toneSet->GetBindings()[0].texture!=image.texture))
    {device->DestroyResourceSet(toneSet);toneSet=nullptr;}
    if(!toneSet)toneSet=device->CreateResourceSet({tonePipeline,&image,1});
    if(!toneSet)return false;
    ColorAttachment color; color.texture=output;color.loadOp=LoadOp::Discard;color.storeOp=StoreOp::Store;
    commands.BeginRendering({&color,1,nullptr});
    commands.BindGraphicsPipeline(tonePipeline);
    commands.BindResourceSet(toneSet);
    commands.SetViewport({0,0,static_cast<float>(output->GetDesc().width),static_cast<float>(output->GetDesc().height),0,1});
    commands.SetScissor({0,0,output->GetDesc().width,output->GetDesc().height});
    const float settings[4]={exposure,IsSrgbFormat(output->GetDesc().format)?0.f:1.f,m_environmentCapture?1.f:0.f,lodDebugActive?1.f:0.f};
    commands.SetInlineConstants(0,sizeof(settings),settings);
    const auto grading=OutputConstants(config.postProcess,
        output->GetDesc().width,output->GetDesc().height,m_environmentCapture || lodDebugActive);
    commands.SetInlineConstants(sizeof(settings),sizeof(grading),grading.data());
    commands.DrawInstanced(3,1,0,0);
    commands.EndRendering();
    return true;
}
}
