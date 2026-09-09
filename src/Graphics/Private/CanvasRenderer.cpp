#include "Graphics/Private/CanvasRenderer.h"
#include "Graphics/Private/CanvasData.h"
#include "Graphics/Private/FrameCapture.h"
#include "Graphics/Private/StockShaderAssets.h"
#include "RHI/Buffer.h"
#include "RHI/ICommandList.h"
#include "RHI/Pipeline.h"
#include "RHI/ResourceSet.h"
#include "RHI/Shader.h"
#include "RHI/Texture.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <vector>

namespace dy::Graphics::Private
{
CanvasRenderer::~CanvasRenderer()
{
    if(m_pipeline) m_device.DestroyPipeline(m_pipeline);
    if(m_vertex) m_device.DestroyShader(m_vertex);
    if(m_fragment) m_device.DestroyShader(m_fragment);
}
bool CanvasRenderer::Initialize(RHI::Format format)
{
    using namespace RHI;
    if(m_pipeline) return true;
    const auto shaders=m_shaders.vertex.binary || m_shaders.fragment.binary
        ? m_shaders : GetCanvasShaderAssets();
    m_vertex=m_device.CreateShader({ShaderStage::Vertex,shaders.vertex.entryPoint,shaders.vertex.binary,shaders.vertex.binarySize});
    m_fragment=m_device.CreateShader({ShaderStage::Fragment,shaders.fragment.entryPoint,shaders.fragment.binary,shaders.fragment.binarySize});
    if(!m_vertex || !m_fragment) return false;
    const VertexBufferLayout buffer{0,sizeof(Canvas::Impl::Vertex),VertexStepMode::Vertex};
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
    desc.vertexShader=m_vertex; desc.fragmentShader=m_fragment; desc.topology=PrimitiveTopology::TriangleList;
    desc.vertexBuffers=&buffer; desc.vertexBufferCount=1; desc.vertexAttributes=attributes.data(); desc.vertexAttributeCount=attributes.size();
    desc.raster={FillMode::Solid,CullMode::None,FrontFace::CounterClockwise,0,0,0};
    desc.colorAttachments=&color; desc.colorAttachmentCount=1;
    desc.layout={bindings.data(),static_cast<uint32_t>(bindings.size()),16,ShaderStageFlags::Vertex,15};
    m_pipeline=m_device.CreateGraphicsPipeline(desc);
    return m_pipeline!=nullptr;
}
bool CanvasRenderer::Render(const Canvas& canvas, TextureAsset* readback)
{
    using namespace RHI;
    const auto& data=*canvas.m_impl;
    if(!m_device.BeginFrame()) return false;
    if(!Initialize(m_device.GetBackBuffer()->GetDesc().format)) throw std::runtime_error("Canvas pipeline creation failed.");
    auto* commands=m_device.AcquireCommandList();
    if(!commands) throw std::runtime_error("Canvas command list acquisition failed.");
    auto* target=m_device.GetBackBuffer();
    BufferHandle vertexBuffer=nullptr;
    std::vector<TextureHandle> textures;
    std::vector<ResourceSetHandle> sets;
    std::map<const TextureAsset*,uint32_t> images;
    const TextureAsset white{"",1,1,{255,255,255,255}};
    images.emplace(nullptr,0);
    for(const auto& draw:data.draws) if(draw.image) images.emplace(draw.image,0);
    bool prepared=true;
    for(auto& entry:images)
    {
        const auto& image=entry.first?*entry.first:white;
        if(!image.width || !image.height || static_cast<uint64_t>(image.width)*image.height*4!=image.rgba8.size()
            || image.rgba8.size()>UINT32_MAX) { prepared=false; break; }
        TextureDesc desc;
        desc.width=image.width;desc.height=image.height;desc.depthOrArraySize=desc.mipLevels=1;
        desc.format=Format::R8G8B8A8_UNORM;desc.usage=TextureUsage::ShaderResource;
        auto* texture=m_device.CreateTexture(desc);
        if(!texture) { prepared=false;break; }
        textures.push_back(texture);
        ResourceBarrierDesc barrier{nullptr,texture,ResourceState::Undefined,ResourceState::CopyDestination,{}};
        commands->ResourceBarrier(&barrier,1);
        if(!m_device.UpdateTexture(*commands,texture,0,0,image.rgba8.data(),static_cast<uint32_t>(image.rgba8.size()),image.width*4,image.width*image.height*4)) { prepared=false;break; }
        barrier.before=ResourceState::CopyDestination;barrier.after=ResourceState::ShaderResource;commands->ResourceBarrier(&barrier,1);
        ResourceBinding binding;binding.binding=0;binding.texture=texture;
        auto* set=m_device.CreateResourceSet({m_pipeline,&binding,1});
        if(!set) { prepared=false;break; }
        entry.second=sets.size();sets.push_back(set);
    }
    if(prepared && !data.vertices.empty())
    {
        const uint64_t bytes=data.vertices.size()*sizeof(Canvas::Impl::Vertex);
        if(bytes>UINT32_MAX) prepared=false;
        else vertexBuffer=m_device.CreateBuffer({static_cast<uint32_t>(bytes),sizeof(Canvas::Impl::Vertex),BufferUsage::Vertex,ResourceState::CopyDestination});
        prepared=prepared && vertexBuffer && m_device.UpdateBuffer(*commands,vertexBuffer,0,data.vertices.data(),static_cast<uint32_t>(bytes));
        if(prepared) { const ResourceBarrierDesc barrier{vertexBuffer,nullptr,ResourceState::CopyDestination,ResourceState::VertexBuffer,{}};commands->ResourceBarrier(&barrier,1); }
    }
    if(prepared)
    {
        const ResourceBarrierDesc before{nullptr,target,ResourceState::Present,ResourceState::RenderTarget,{}};
        commands->ResourceBarrier(&before,1);
        ColorAttachment color; color.texture=target;color.loadOp=LoadOp::Clear;color.storeOp=StoreOp::Store;
        color.clearColor[0]=data.clearColor.x;color.clearColor[1]=data.clearColor.y;color.clearColor[2]=data.clearColor.z;color.clearColor[3]=data.clearColor.w;
        commands->BeginRendering({&color,1,nullptr});
        if(vertexBuffer)
        {
            commands->BindGraphicsPipeline(m_pipeline);commands->BindVertexBuffer(0,vertexBuffer,0);
            for(const auto& draw:data.draws)
            {
                const auto& v=draw.viewport;const auto& c=draw.clip;
                const float left=std::max({0.0f,v.x,v.x+c.x}),top=std::max({0.0f,v.y,v.y+c.y});
                const float right=std::min({static_cast<float>(target->GetDesc().width),v.x+v.width,v.x+c.x+c.width});
                const float bottom=std::min({static_cast<float>(target->GetDesc().height),v.y+v.height,v.y+c.y+c.height});
                if(right<=left || bottom<=top) continue;
                commands->SetViewport({v.x,v.y,v.width,v.height,0,1});
                commands->SetScissor({static_cast<int32_t>(left),static_cast<int32_t>(top),static_cast<uint32_t>(right-left),static_cast<uint32_t>(bottom-top)});
                const float transform[4]={2/v.width,-2/v.height,-1,1};
                commands->SetInlineConstants(0,sizeof(transform),transform);
                commands->BindResourceSet(sets.at(images.at(draw.image)));
                commands->DrawInstanced(draw.count,1,draw.first,0);
            }
        }
        commands->EndRendering();
        const ResourceBarrierDesc after{nullptr,target,ResourceState::RenderTarget,ResourceState::Present,{}};commands->ResourceBarrier(&after,1);
    }
    commands->Close();
    const bool submitted=m_device.Submit(&commands,1);
    for(auto* set:sets)m_device.DestroyResourceSet(set);
    for(auto* texture:textures)m_device.DestroyTexture(texture);
    if(vertexBuffer)m_device.DestroyBuffer(vertexBuffer);
    if(!prepared || !submitted) throw std::runtime_error("Canvas command assembly/submission failed.");
    CaptureFrame(m_device, readback);
    m_device.Present();return true;
}
}
