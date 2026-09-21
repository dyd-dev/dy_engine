#include "dyf/Image.h"
#include "dyf/Renderer.h"
#include "dyf/Platform/Profiler.h"
#include "dyf/Canvas.h"

#include "dyf/RHI/Buffer.h"
#include "dyf/RHI/ICommandList.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/Pipeline.h"
#include "dyf/RHI/ResourceSet.h"
#include "dyf/RHI/Shader.h"
#include "dyf/RHI/Texture.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <cstdio>
#include <vector>

namespace dyf
{
// Canvas의 CPU 정점과 이미지를 그리는 기본 파이프라인을 공개 RHI로 작성한다.
bool Renderer::InitializeCanvas(RHI::Format format)
{
    using namespace RHI;
    if(canvasPipeline) return true;
    const auto shaders=DefaultShaders();
    canvasVertexShader=device->CreateShader(ShaderDescription(CanvasVertex,shaders.canvasVertex));
    canvasFragmentShader=device->CreateShader(ShaderDescription(CanvasFragment,shaders.canvasFragment));
    if(!canvasVertexShader || !canvasFragmentShader) return false;
    const VertexBufferLayout buffer{0,sizeof(Canvas::Vertex),VertexStepMode::Vertex};
    const std::array<VertexAttribute,3> attributes={{{0,0,Format::R32G32_FLOAT,0},{1,0,Format::R32G32_FLOAT,8},{2,0,Format::R32G32B32A32_FLOAT,16}}};
    SamplerDesc sampler;
    sampler.minFilter=sampler.magFilter=sampler.mipFilter=SamplerFilter::Linear;
    sampler.addressU=sampler.addressV=sampler.addressW=SamplerAddressMode::ClampToEdge;
    sampler.mipLodBias=sampler.minLod=sampler.maxLod=0;
    const std::array<ResourceBindingLayout,2> bindings={{{0,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}},
        {1,ResourceBindingType::StaticSampler,1,ShaderStageFlags::Fragment,sampler}}};
    const ColorAttachmentDesc color={format,{true,BlendFactor::SourceAlpha,BlendFactor::OneMinusSourceAlpha,BlendOp::Add,
        BlendFactor::One,BlendFactor::OneMinusSourceAlpha,BlendOp::Add},ColorWriteMask::All};
    GraphicsPipelineDesc desc;
    desc.vertexShader=canvasVertexShader; desc.fragmentShader=canvasFragmentShader; desc.topology=PrimitiveTopology::TriangleList;
    desc.vertexBuffers=&buffer; desc.vertexBufferCount=1; desc.vertexAttributes=attributes.data(); desc.vertexAttributeCount=attributes.size();
    desc.raster={FillMode::Solid,CullMode::None,FrontFace::CounterClockwise,0,0,0};
    desc.colorAttachments=&color; desc.colorAttachmentCount=1;
    desc.layout={bindings.data(),static_cast<uint32_t>(bindings.size()),32,
        ShaderStageFlags::Vertex | ShaderStageFlags::Fragment,15};
    canvasPipeline=device->CreateGraphicsPipeline(desc);
    return canvasPipeline!=nullptr;
}
bool Renderer::RecordCanvas(const Canvas& canvas, RHI::ICommandList& commandList, RHI::TextureHandle target, bool overlay, bool graphManagedTarget)
{
    using namespace RHI;
    if(!canvas.IsValid()) {std::fprintf(stderr,"dyf: Invalid canvas dimensions.\n");return false;}
    if(!target || !InitializeCanvas(target->GetDesc().format)) return false;
    BufferHandle vertexBuffer=nullptr;
    std::vector<RHI::TextureHandle> textures;
    std::vector<ResourceSetHandle> sets;
    const Image white{1,1,{255,255,255,255},dyf::ColorSpace::Linear};
    std::vector<const Image*> images{&white};
    std::map<std::pair<const uint8_t*,dyf::ColorSpace>,uint32_t> imageSlots;
    for(const auto& draw:canvas.draws)
    {
        if(!draw.image.IsValid())continue;
        const auto key=std::make_pair(draw.image.GetPixels().data(),draw.image.GetColorSpace());
        if(imageSlots.emplace(key,static_cast<uint32_t>(images.size())).second)
            images.push_back(&draw.image);
    }
    bool prepared=true;
    for(const auto* source:images)
    {
        const auto& image=*source;
        if(!image.GetWidth() || !image.GetHeight() || static_cast<uint64_t>(image.GetWidth())*image.GetHeight()*4!=image.GetPixels().size()
            || image.GetPixels().size()>UINT32_MAX) { prepared=false; break; }
        TextureDesc desc;
        desc.width=image.GetWidth();desc.height=image.GetHeight();desc.depthOrArraySize=desc.mipLevels=1;
        desc.format=image.GetColorSpace()==dyf::ColorSpace::Srgb ? Format::R8G8B8A8_UNORM_SRGB : Format::R8G8B8A8_UNORM;
        desc.usage=TextureUsage::ShaderResource;
        auto* texture=device->CreateTexture(desc);
        if(!texture) { prepared=false;break; }
        textures.push_back(texture);
        ResourceBarrierDesc barrier{nullptr,texture,ResourceState::Undefined,ResourceState::CopyDestination,{}};
        commandList.ResourceBarrier(&barrier,1);
        if(!device->UpdateTexture(commandList,texture,0,0,image.GetPixels().data(),static_cast<uint32_t>(image.GetPixels().size()),image.GetWidth()*4,image.GetWidth()*image.GetHeight()*4)) { prepared=false;break; }
        barrier.before=ResourceState::CopyDestination;barrier.after=ResourceState::ShaderResource;commandList.ResourceBarrier(&barrier,1);
        ResourceBinding binding;binding.binding=0;binding.texture=texture;
        auto* set=device->CreateResourceSet({canvasPipeline,&binding,1});
        if(!set) { prepared=false;break; }
        sets.push_back(set);
    }
    if(prepared && !canvas.vertices.empty())
    {
        const uint64_t bytes=canvas.vertices.size()*sizeof(Canvas::Vertex);
        if(bytes>UINT32_MAX) prepared=false;
        else vertexBuffer=device->CreateBuffer({static_cast<uint32_t>(bytes),sizeof(Canvas::Vertex),BufferUsage::Vertex,ResourceState::CopyDestination});
        prepared=prepared && vertexBuffer && device->UpdateBuffer(commandList,vertexBuffer,0,canvas.vertices.data(),static_cast<uint32_t>(bytes));
        if(prepared) { const ResourceBarrierDesc barrier{vertexBuffer,nullptr,ResourceState::CopyDestination,ResourceState::VertexBuffer,{}};commandList.ResourceBarrier(&barrier,1); }
    }
    if(prepared)
    {
        const ResourceBarrierDesc before{nullptr,target,ResourceState::Present,ResourceState::RenderTarget,{}};
        if(!graphManagedTarget) commandList.ResourceBarrier(&before,1);
        ColorAttachment color; color.texture=target;color.loadOp=overlay ? LoadOp::Load : LoadOp::Clear;color.storeOp=StoreOp::Store;
        color.clearColor[0]=canvas.clearColor.x;
        color.clearColor[1]=canvas.clearColor.y;
        color.clearColor[2]=canvas.clearColor.z;
        color.clearColor[3]=canvas.clearColor.w;
        if(IsSrgbFormat(target->GetDesc().format))
        {
            // sRGB 대상의 하드웨어 인코딩 전에 화면 색상을 선형 값으로 바꾼다.
            for(uint32_t channel=0;channel<3;++channel)
            {
                const float value=color.clearColor[channel];
                color.clearColor[channel]=value<=0.04045f ? value/12.92f : std::pow((value+0.055f)/1.055f,2.4f);
            }
        }
        commandList.BeginRendering({&color,1,nullptr});
        if(vertexBuffer)
        {
            commandList.BindGraphicsPipeline(canvasPipeline);commandList.BindVertexBuffer(0,vertexBuffer,0);
            for(const auto& draw:canvas.draws)
            {
                const auto& v=draw.viewport;const auto& c=draw.clip;
                const float left=std::max({0.0f,v.x,v.x+c.x}),top=std::max({0.0f,v.y,v.y+c.y});
                const float right=std::min({static_cast<float>(target->GetDesc().width),v.x+v.width,v.x+c.x+c.width});
                const float bottom=std::min({static_cast<float>(target->GetDesc().height),v.y+v.height,v.y+c.y+c.height});
                if(right<=left || bottom<=top) continue;
                commandList.SetViewport({v.x,v.y,v.width,v.height,0,1});
                commandList.SetScissor({static_cast<int32_t>(left),static_cast<int32_t>(top),static_cast<uint32_t>(right-left),static_cast<uint32_t>(bottom-top)});
                const float transform[8]={2/v.width,-2/v.height,-1,1,
                    IsSrgbFormat(target->GetDesc().format)?1.f:0.f,0,0,0};
                commandList.SetInlineConstants(0,sizeof(transform),transform);
                const uint32_t imageSlot=draw.image.IsValid()
                    ? imageSlots.at({draw.image.GetPixels().data(),draw.image.GetColorSpace()}) : 0;
                commandList.BindResourceSet(sets[imageSlot]);
                commandList.DrawInstanced(draw.count,1,draw.first,0);
            }
        }
        commandList.EndRendering();
        const ResourceBarrierDesc after{nullptr,target,ResourceState::RenderTarget,ResourceState::Present,{}};
        if(!graphManagedTarget) commandList.ResourceBarrier(&after,1);
    }
    for(auto* set:sets)device->DestroyResourceSet(set);
    for(auto* texture:textures)device->DestroyTexture(texture);
    if(vertexBuffer)device->DestroyBuffer(vertexBuffer);
    return prepared;
}

bool Renderer::RenderCanvas(const Canvas& canvas, Image* readback)
{
    if(!device->BeginFrame())
    {
        if(device->IsLost()) {std::fprintf(stderr,"dyf: Canvas device lost.\n");return false;}
        if(readback)*readback={};
        return true;
    }
    auto* commands=device->AcquireCommandList();
    if(!commands) {std::fprintf(stderr,"dyf: Canvas command list acquisition failed.\n");return false;}
    const bool recorded=RecordCanvas(canvas,*commands,device->GetBackBuffer(),false);
    const bool closed=commands->Close();
    const bool submitted=device->Submit(&commands,1);
    device->DestroyCommandList(commands);
    if(!recorded || !closed || !submitted) {std::fprintf(stderr,"dyf: Canvas command assembly/submission failed.\n");return false;}
    if(readback && !CaptureFrame(*readback))return false;
    if(!device->Present()) {std::fprintf(stderr,"dyf: Canvas presentation failed.\n");return false;}
    DY_PROFILE_FRAME_MARK();
    return true;
}

}
