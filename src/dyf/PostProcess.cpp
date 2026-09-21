#include "dyf/Renderer.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/ICommandList.h"
#include "dyf/RHI/Texture.h"
#include "dyf/RHI/Pipeline.h"
#include "dyf/RHI/ResourceSet.h"
#include "dyf/RHI/Shader.h"
#include <array>
namespace dyf
{
// HDR과 톤 매핑은 별도 소유 클래스 없이 RHI 텍스처와 전체 화면 draw로 구성한다.
bool Renderer::PreparePostProcess(RHI::TextureHandle output)
{
    using namespace RHI;
    if(!output)return false;
    if(!tonePipeline)
    {
        const auto shaders=DefaultShaders();
        toneVertexShader=device->CreateShader(ShaderDescription(ToneVertex,shaders.toneMapVertex));
        toneFragmentShader=device->CreateShader(ShaderDescription(ToneFragment,shaders.toneMapFragment));
        if(!toneVertexShader || !toneFragmentShader)return false;
        SamplerDesc sampler;
        sampler.minFilter=sampler.magFilter=sampler.mipFilter=SamplerFilter::Nearest;
        sampler.addressU=sampler.addressV=sampler.addressW=SamplerAddressMode::ClampToEdge;
        sampler.mipLodBias=sampler.minLod=sampler.maxLod=0;
        const std::array<ResourceBindingLayout,2> bindings={{{0,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}},
            {1,ResourceBindingType::StaticSampler,1,ShaderStageFlags::Fragment,sampler}}};
        const ColorAttachmentDesc color{output->GetDesc().format,{},ColorWriteMask::All};
        GraphicsPipelineDesc desc;
        desc.vertexShader=toneVertexShader;desc.fragmentShader=toneFragmentShader;desc.topology=PrimitiveTopology::TriangleList;
        desc.raster={FillMode::Solid,CullMode::None,FrontFace::CounterClockwise,0,0,0};
        desc.colorAttachments=&color;desc.colorAttachmentCount=1;
        desc.layout={bindings.data(),static_cast<uint32_t>(bindings.size()),16,ShaderStageFlags::Fragment,15};
        tonePipeline=device->CreateGraphicsPipeline(desc);
        if(!tonePipeline)return false;
    }
    if(hdrTarget && (hdrTarget->GetDesc().width!=output->GetDesc().width || hdrTarget->GetDesc().height!=output->GetDesc().height))
    {device->DestroyTexture(hdrTarget);hdrTarget=nullptr;}
    if(!hdrTarget)
    {
        TextureDesc desc;
        desc.width=output->GetDesc().width;desc.height=output->GetDesc().height;
        desc.format=Format::R16G16B16A16_FLOAT;
        desc.usage=TextureUsage::RenderTarget|TextureUsage::ShaderResource;
        hdrTarget=device->CreateTexture(desc);
        hdrState=ResourceState::Undefined;
    }
    return hdrTarget!=nullptr;
}
bool Renderer::RecordToneMap(RHI::ICommandList& commands,RHI::TextureHandle output,float exposure)
{
    using namespace RHI;
    if(!tonePipeline || !hdrTarget || !output)return false;
    ResourceBinding image;image.binding=0;image.texture=hdrTarget;
    auto* set=device->CreateResourceSet({tonePipeline,&image,1});
    if(!set)return false;
    ColorAttachment color; color.texture=output;color.loadOp=LoadOp::Discard;color.storeOp=StoreOp::Store;
    commands.BeginRendering({&color,1,nullptr});
    commands.BindGraphicsPipeline(tonePipeline);
    commands.BindResourceSet(set);
    commands.SetViewport({0,0,static_cast<float>(output->GetDesc().width),static_cast<float>(output->GetDesc().height),0,1});
    commands.SetScissor({0,0,output->GetDesc().width,output->GetDesc().height});
    const float settings[4]={exposure,IsSrgbFormat(output->GetDesc().format)?0.f:1.f,0,0};
    commands.SetInlineConstants(0,sizeof(settings),settings);
    commands.DrawInstanced(3,1,0,0);
    commands.EndRendering();
    device->DestroyResourceSet(set);
    return true;
}
}
