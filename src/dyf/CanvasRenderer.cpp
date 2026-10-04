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
#include "dyf/RHI/ResourceScope.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <cstdio>
#include <vector>

namespace dyf
{
Renderer::CanvasFrame::CanvasFrame(Renderer& renderer):owner(renderer),imageCount(renderer.canvasImages.size())
{
    owner.canvasPendingVertices=owner.canvasVertices;
    owner.canvasVertexCursor=0;
    for(auto& image:owner.canvasImages)image.used=false;
}
Renderer::CanvasFrame::~CanvasFrame()
{
    if(committed)return;
    for(size_t i=0;i<owner.canvasPendingVertices.size();++i)
        if(i>=owner.canvasVertices.size() || owner.canvasPendingVertices[i].buffer!=owner.canvasVertices[i].buffer)
            owner.device->DestroyBuffer(owner.canvasPendingVertices[i].buffer);
    owner.canvasPendingVertices.clear();
    while(owner.canvasImages.size()>imageCount) {
        const auto& image=owner.canvasImages.back();
        owner.device->DestroyResourceSet(image.set);owner.device->DestroyTexture(image.texture);
        owner.canvasImages.pop_back();
    }
}
void Renderer::CanvasFrame::Commit()
{
    for(size_t i=0;i<owner.canvasVertices.size();++i)
        if(owner.canvasVertices[i].buffer!=owner.canvasPendingVertices[i].buffer)
            owner.device->DestroyBuffer(owner.canvasVertices[i].buffer);
    owner.canvasVertices.swap(owner.canvasPendingVertices);
    owner.canvasPendingVertices.clear();
    // Retain only a bounded window of recently used image identities. The Image
    // owns immutable CPU pixels, preventing data-address reuse from hitting an
    // unrelated texture. RHI retains evicted resources until GPU completion.
    const auto window=std::max(1u,owner.device->GetDesc().maxFramesInFlight);
    owner.canvasImages.erase(std::remove_if(owner.canvasImages.begin(),owner.canvasImages.end(),[&](CanvasImageSlot& image) {
        if(image.used) {image.unusedFrames=0;return false;}
        if(++image.unusedFrames<window)return false;
        owner.device->DestroyResourceSet(image.set);owner.device->DestroyTexture(image.texture);return true;
    }),owner.canvasImages.end());
    committed=true;
}

// Canvas의 CPU 정점과 이미지를 그리는 기본 파이프라인을 공개 RHI로 작성한다.
bool Renderer::InitializeCanvas(RHI::Format format)
{
    using namespace RHI;
    if(canvasPipeline) return true;
    const auto shaders=DefaultShaders();
    if(!canvasVertexShader) canvasVertexShader=device->CreateShader(ShaderDescription(CanvasVertex,shaders.canvasVertex));
    if(!canvasFragmentShader) canvasFragmentShader=device->CreateShader(ShaderDescription(CanvasFragment,shaders.canvasFragment));
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
    if(!canvasWhite.IsValid())canvasWhite=Image(1,1,{255,255,255,255},dyf::ColorSpace::Linear);
    const auto resolve=[&](const Image& image)->uint32_t {
        for(uint32_t i=0;i<canvasImages.size();++i) {
            auto& cached=canvasImages[i];
            if(cached.source.GetPixels().data()==image.GetPixels().data() &&
                cached.source.GetColorSpace()==image.GetColorSpace() &&
                cached.source.GetWidth()==image.GetWidth() && cached.source.GetHeight()==image.GetHeight()) {
                cached.used=true;return i;
            }
        }
        if(image.GetPixels().size()>UINT32_MAX || canvasImages.size()>=UINT32_MAX)return UINT32_MAX;
        // Publish each new owner to the frame transaction before recording or
        // creating its next dependency, so exceptions roll back every handle.
        canvasImages.push_back({image,nullptr,nullptr,0,true});
        auto& cached=canvasImages.back();
        TextureDesc desc;
        desc.width=image.GetWidth();desc.height=image.GetHeight();
        desc.format=image.GetColorSpace()==dyf::ColorSpace::Srgb ? Format::R8G8B8A8_UNORM_SRGB : Format::R8G8B8A8_UNORM;
        desc.usage=TextureUsage::ShaderResource;
        auto* texture=cached.texture=device->CreateTexture(desc);
        if(!texture)return UINT32_MAX;
        ResourceBarrierDesc barrier{nullptr,texture,ResourceState::Undefined,ResourceState::CopyDestination,{}};
        commandList.ResourceBarrier(&barrier,1);
        if(!device->UpdateTexture(commandList,texture,0,0,image.GetPixels().data(),static_cast<uint32_t>(image.GetPixels().size()),image.GetWidth()*4,static_cast<uint32_t>(image.GetPixels().size())))return UINT32_MAX;
        barrier.before=ResourceState::CopyDestination;barrier.after=ResourceState::ShaderResource;commandList.ResourceBarrier(&barrier,1);
        ResourceBinding binding;binding.texture=texture;
        cached.set=device->CreateResourceSet({canvasPipeline,&binding,1});
        return cached.set ? static_cast<uint32_t>(canvasImages.size()-1) : UINT32_MAX;
    };
    if(resolve(canvasWhite)==UINT32_MAX)return false;
    canvasDrawImages.clear();canvasDrawImages.reserve(canvas.draws.size());
    for(const auto& draw:canvas.draws) {
        const auto index=resolve(draw.image.IsValid()?draw.image:canvasWhite);
        if(index==UINT32_MAX)return false;canvasDrawImages.push_back(index);
    }
    if(!canvas.vertices.empty())
    {
        const uint64_t bytes=canvas.vertices.size()*sizeof(Canvas::Vertex);
        if(bytes>UINT32_MAX)return false;
        if(canvasVertexCursor==canvasPendingVertices.size())canvasPendingVertices.push_back({});
        auto& slot=canvasPendingVertices[canvasVertexCursor++];
        if(!slot.buffer || slot.buffer->GetDesc().size<bytes) {
            uint64_t capacity=slot.buffer ? slot.buffer->GetDesc().size : sizeof(Canvas::Vertex)*64;
            while(capacity<bytes)capacity=std::min(uint64_t(UINT32_MAX),capacity*2);
            auto* replacement=device->CreateBuffer({static_cast<uint32_t>(capacity),sizeof(Canvas::Vertex),BufferUsage::Vertex,ResourceState::CopyDestination});
            if(!replacement)return false;
            slot.buffer=replacement;slot.ready=false;
        }
        vertexBuffer=slot.buffer;
        if(slot.ready) {const ResourceBarrierDesc barrier{vertexBuffer,nullptr,ResourceState::VertexBuffer,ResourceState::CopyDestination,{}};commandList.ResourceBarrier(&barrier,1);}
        if(!device->UpdateBuffer(commandList,vertexBuffer,0,canvas.vertices.data(),static_cast<uint32_t>(bytes)))return false;
        const ResourceBarrierDesc barrier{vertexBuffer,nullptr,ResourceState::CopyDestination,ResourceState::VertexBuffer,{}};commandList.ResourceBarrier(&barrier,1);
        slot.ready=true;
    }
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
            for(size_t drawIndex=0;drawIndex<canvas.draws.size();)
            {
                const auto& draw=canvas.draws[drawIndex];
                const auto imageSlot=canvasDrawImages[drawIndex];
                uint32_t count=draw.count;
                size_t next=drawIndex+1;
                const auto sameRect=[](const Rectangle& a,const Rectangle& b) {return a.x==b.x && a.y==b.y && a.width==b.width && a.height==b.height;};
                // Only adjacent ranges with identical raster state may merge;
                // vertex/alpha order and per-vertex tint remain unchanged.
                while(next<canvas.draws.size()) {
                    const auto& candidate=canvas.draws[next];
                    if(canvasDrawImages[next]!=imageSlot || uint64_t(draw.first)+count!=candidate.first ||
                        !sameRect(draw.viewport,candidate.viewport) || !sameRect(draw.clip,candidate.clip) ||
                        uint64_t(count)+candidate.count>UINT32_MAX)break;
                    count+=candidate.count;++next;
                }
                drawIndex=next;
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
                commandList.BindResourceSet(canvasImages[imageSlot].set);
                commandList.DrawInstanced(count,1,draw.first,0);
            }
        }
        commandList.EndRendering();
        const ResourceBarrierDesc after{nullptr,target,ResourceState::RenderTarget,ResourceState::Present,{}};
        if(!graphManagedTarget) commandList.ResourceBarrier(&after,1);
    }
    return true;
}

bool Renderer::RenderCanvas(const Canvas& canvas, Image* readback)
{
    if(!canvas.IsValid()) {std::fprintf(stderr,"dyf: Invalid canvas dimensions.\n");return false;}
    if(!device->BeginFrame())
    {
        if(device->IsLost()) {std::fprintf(stderr,"dyf: Canvas device lost.\n");return false;}
        if(readback)*readback={};
        return true;
    }
    canvasFramePending=true;
    // Canvas submits a single list ending in Present. Before submission (including
    // failed recording) the acquired image also remains in Present. Close that
    // frame after releasing recording ownership, including readback failures.
    struct FrameEnd
    {
        RHI::IDevice& device;
        bool& pending;
        ~FrameEnd()
        {
            if(!pending) return;
            try { (void)device.Present(); pending=false; }
            catch(...) { /* ApplySettings retries recovery before changing output. */ }
        }
    } frameEnd{*device,canvasFramePending};
    CanvasFrame canvasFrame(*this);
    RHI::ResourceScope resources(*device);
    auto* commands=device->AcquireCommandList();
    if(!commands) {std::fprintf(stderr,"dyf: Canvas command list acquisition failed.\n");return false;}
    resources.Keep(commands);
    const bool recorded=RecordCanvas(canvas,*commands,device->GetBackBuffer(),false);
    const bool closed=recorded && commands->Close();
    const bool submitted=closed && device->Submit(&commands,1);
    if(!recorded || !closed || !submitted) {std::fprintf(stderr,"dyf: Canvas command assembly/submission failed.\n");return false;}
    canvasFrame.Commit();
    if(readback && !CaptureFrame(*readback))return false;
    const bool presented=device->Present();
    canvasFramePending=false;
    if(!presented) {std::fprintf(stderr,"dyf: Canvas presentation failed.\n");return false;}
    DY_PROFILE_FRAME_MARK();
    return true;
}

}
