#pragma once

#include "dyf/RHI.h"
#include "dyf/RenderExtension.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace RenderingProbe
{
using namespace dyf;
using namespace dyf::RHI;
inline void Check(bool value,const char* message)
{if(!value)throw std::runtime_error(message);}
template<class F> void Reject(const char* label,F operation)
{
    std::fprintf(stderr,"EXPECTED_COMMON_REJECTION_BEGIN %s\n",label);
    const bool rejected=!operation();
    std::fprintf(stderr,"EXPECTED_COMMON_REJECTION_END %s rejected=%u\n",label,unsigned(rejected));
    Check(rejected,label);
}
inline void Barrier(ICommandList& commands,BufferHandle buffer,ResourceState before,ResourceState after)
{const ResourceBarrierDesc barrier{buffer,nullptr,before,after,{}};commands.ResourceBarrier(&barrier,1);}
inline void Barrier(ICommandList& commands,TextureHandle texture,ResourceState before,ResourceState after)
{const ResourceBarrierDesc barrier{nullptr,texture,before,after,{}};commands.ResourceBarrier(&barrier,1);}

class NativeRhiTests
{
    struct Vertex {float x,y,u,v,r,g,b,a;};
    IDevice& m_device;
    ResourceScope m_own;
    ShaderHandle m_vertex=nullptr,m_fragment=nullptr;
    PipelineHandle m_pipeline=nullptr,m_depthPipeline=nullptr;
    ResourceSetHandle m_whiteSet=nullptr,m_depthSet=nullptr;
    TextureHandle m_color=nullptr,m_depth=nullptr,m_white=nullptr;
    BufferHandle m_vertices=nullptr,m_fullscreen=nullptr,m_source=nullptr,m_indices=nullptr;
    BufferHandle m_packed=nullptr,m_padded=nullptr;

    ShaderHandle Shader(const std::filesystem::path& path,ShaderStage stage)
    {
        std::ifstream stream(path,std::ios::binary|std::ios::ate);
        Check(bool(stream),"Cannot open a generated Canvas shader binary.");
        const auto end=stream.tellg();
        Check(end>0 && end<=16*1024*1024,"Unexpected Canvas shader binary size.");
        std::vector<uint8_t> bytes(static_cast<size_t>(end));
        stream.seekg(0);stream.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
        Check(bool(stream),"Cannot read a complete Canvas shader binary.");
        return m_own.Keep(m_device.CreateShader({stage,"main",bytes.data(),bytes.size()}));
    }
    PipelineHandle Pipeline(bool depth)
    {
        const VertexBufferLayout vertex{0,sizeof(Vertex),VertexStepMode::Vertex};
        const VertexAttribute attributes[]={{0,0,Format::R32G32_FLOAT,0},{1,0,Format::R32G32_FLOAT,8},
            {2,0,Format::R32G32B32A32_FLOAT,16}};
        SamplerDesc sampler;sampler.minFilter=sampler.magFilter=sampler.mipFilter=SamplerFilter::Nearest;
        sampler.addressU=sampler.addressV=sampler.addressW=SamplerAddressMode::ClampToEdge;
        sampler.mipLodBias=sampler.minLod=sampler.maxLod=0;
        const ResourceBindingLayout bindings[]={{0,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}},
            {1,ResourceBindingType::StaticSampler,1,ShaderStageFlags::Fragment,sampler}};
        // Surface alpha is material/history data, so copy it without coverage blending.
        const ColorAttachmentDesc color{Format::R8G8B8A8_UNORM,{},ColorWriteMask::All};
        GraphicsPipelineDesc desc;desc.vertexShader=m_vertex;desc.fragmentShader=m_fragment;
        desc.topology=PrimitiveTopology::TriangleList;desc.vertexBuffers=&vertex;desc.vertexBufferCount=1;
        desc.vertexAttributes=attributes;desc.vertexAttributeCount=3;
        desc.raster={FillMode::Solid,CullMode::None,FrontFace::CounterClockwise,0,0,0};
        desc.colorAttachments=&color;desc.colorAttachmentCount=1;
        desc.layout={bindings,2,32,ShaderStageFlags::Vertex|ShaderStageFlags::Fragment,15};
        if(depth){desc.depthStencil.format=Format::D32_FLOAT;desc.depthStencil.depthTestEnabled=true;
            desc.depthStencil.depthWriteEnabled=true;desc.depthStencil.depthCompareOp=CompareOp::Less;}
        return m_own.Keep(m_device.CreateGraphicsPipeline(desc));
    }
    BufferHandle Upload(ICommandList& commands,const void* bytes,uint32_t size,BufferUsage usage,ResourceState state)
    {
        auto* buffer=m_own.Keep(m_device.CreateBuffer({size,0,usage,ResourceState::CopyDestination}));
        Check(m_device.UpdateBuffer(commands,buffer,0,bytes,size),"Probe buffer upload failed.");
        if(state!=ResourceState::CopyDestination)Barrier(commands,buffer,ResourceState::CopyDestination,state);
        return buffer;
    }
    void Bind(ICommandList& commands,bool depth=false,BufferHandle vertices=nullptr,ResourceSetHandle set=nullptr)
    {
        commands.BindGraphicsPipeline(depth?m_depthPipeline:m_pipeline);
        commands.BindResourceSet(set?set:depth?m_depthSet:m_whiteSet);
        commands.BindVertexBuffer(0,vertices?vertices:m_vertices,0);
        commands.BindIndexBuffer(m_indices,Format::R32_UINT,8);
        const float constants[8]={2.f/Side,-2.f/Side,-1,1,0,0,0,0};
        commands.SetInlineConstants(0,sizeof(constants),constants);
    }
    void Begin(ICommandList& commands,LoadOp load=LoadOp::Clear,bool depth=false,Rect area={},
        std::array<float,4> clear={0,0,0,1},float clearDepth=1,float drawDepth=0)
    {
        ColorAttachment color;color.texture=m_color;color.loadOp=load;color.storeOp=StoreOp::Store;
        std::copy(clear.begin(),clear.end(),color.clearColor);
        DepthStencilAttachment z;z.texture=m_depth;z.state=ResourceState::DepthWrite;
        z.depthLoadOp=load;z.depthStoreOp=StoreOp::Store;z.clearDepth=clearDepth;
        commands.BeginRendering({&color,1,depth?&z:nullptr,area});
        commands.SetViewport({0,0,float(Side),float(Side),drawDepth,drawDepth});
        commands.SetScissor(area.width?area:Rect{0,0,Side,Side});Bind(commands,depth);
    }
    void Mdi(BufferHandle arguments,uint32_t offset,uint32_t count,uint32_t stride)
    {
        auto* commands=Commands();Begin(*commands);
        commands->DrawIndexedIndirect(arguments,offset,count,stride);commands->EndRendering();Submit(commands);
    }
    void InvalidMdi(const char* label,BufferHandle arguments,uint32_t offset,uint32_t count,uint32_t stride)
    {
        Reject(label,[&]{auto* commands=Commands();Begin(*commands);
            commands->DrawIndexedIndirect(arguments,offset,count,stride);commands->EndRendering();return commands->Close();});
    }
public:
    static constexpr uint32_t Side=64;
    explicit NativeRhiTests(IDevice& device,const std::filesystem::path& shaderDirectory):m_device(device),m_own(device)
    {
        m_vertex=Shader(shaderDirectory/"CanvasVertex.bin",ShaderStage::Vertex);
        m_fragment=Shader(shaderDirectory/"CanvasFragment.bin",ShaderStage::Fragment);
        m_pipeline=Pipeline(false);m_depthPipeline=Pipeline(true);m_color=ColorTarget();
        TextureDesc depth;depth.width=depth.height=Side;depth.format=Format::D32_FLOAT;depth.usage=TextureUsage::DepthStencil;
        m_depth=m_own.Keep(device.CreateTexture(depth));
        TextureDesc white;white.width=white.height=1;white.format=Format::R8G8B8A8_UNORM;white.usage=TextureUsage::ShaderResource;
        m_white=m_own.Keep(device.CreateTexture(white));
        auto* commands=Commands();
        Barrier(*commands,m_color,ResourceState::Undefined,ResourceState::RenderTarget);
        Barrier(*commands,m_depth,ResourceState::Undefined,ResourceState::DepthWrite);
        Barrier(*commands,m_white,ResourceState::Undefined,ResourceState::CopyDestination);
        const uint8_t pixel[4]={255,255,255,255};
        Check(device.UpdateTexture(*commands,m_white,0,0,pixel,4,4,4),"Probe white texture upload failed.");
        Barrier(*commands,m_white,ResourceState::CopyDestination,ResourceState::ShaderResource);
        const Vertex pair[]={{4,4,0,0,1,0,0,1},{28,4,1,0,1,0,0,1},{28,28,1,1,1,0,0,1},{4,28,0,1,1,0,0,1},
            {36,4,0,0,0,1,0,1},{60,4,1,0,0,1,0,1},{60,28,1,1,0,1,0,1},{36,28,0,1,0,1,0,1}};
        const Vertex full[]={{0,0,0,0,1,1,1,1},{Side,0,1,0,1,1,1,1},
            {Side,Side,1,1,1,1,1,1},{0,Side,0,1,1,1,1,1}};
        m_vertices=Upload(*commands,pair,sizeof(pair),BufferUsage::Vertex,ResourceState::VertexBuffer);
        m_fullscreen=Upload(*commands,full,sizeof(full),BufferUsage::Vertex,ResourceState::VertexBuffer);
        const uint32_t indices[]={0,0,1,2,0,2,3,4,5,6,4,6,7,0,0},zeros[16]={};
        m_source=Upload(*commands,indices,sizeof(indices),BufferUsage::Index,ResourceState::CopySource);
        m_indices=Upload(*commands,zeros,sizeof(zeros),BufferUsage::Index,ResourceState::CopyDestination);
        commands->CopyBuffer(m_source,4,m_indices,8,48);
        Barrier(*commands,m_indices,ResourceState::CopyDestination,ResourceState::IndexBuffer);
        const DrawIndexedIndirectArguments packed[]={{6,1,0,0,0},{6,1,6,0,0}};
        std::array<uint8_t,48> padded={};
        std::memcpy(padded.data()+4,&packed[0],20);std::memcpy(padded.data()+28,&packed[1],20);
        m_packed=Upload(*commands,packed,sizeof(packed),BufferUsage::Indirect,ResourceState::IndirectArgument);
        m_padded=Upload(*commands,padded.data(),uint32_t(padded.size()),BufferUsage::Indirect,ResourceState::IndirectArgument);
        m_whiteSet=SampleSet(m_white);m_depthSet=SampleSet(m_white,true);Submit(commands);
    }
    ~NativeRhiTests(){if(!m_device.WaitIdle())std::fprintf(stderr,"FAIL probe cleanup GPU wait.\n");}
    ICommandList* Commands(){return m_own.Keep(m_device.AcquireCommandList());}
    TextureHandle ColorTarget()
    {
        TextureDesc desc;desc.width=desc.height=Side;desc.format=Format::R8G8B8A8_UNORM;
        desc.usage=TextureUsage::RenderTarget|TextureUsage::ShaderResource;
        return m_own.Keep(m_device.CreateTexture(desc));
    }
    ResourceSetHandle SampleSet(TextureHandle image,bool depth=false)
    {
        const ResourceBinding binding{0,0,nullptr,image,0,0,{}};
        return m_own.Keep(m_device.CreateResourceSet({depth?m_depthPipeline:m_pipeline,&binding,1}));
    }
    void Submit(ICommandList* commands)
    {
        Check(commands->Close(),"Probe positive command recording failed.");
        FenceHandle completion;const SubmitDesc desc{&commands,1,nullptr,0};
        Check(m_device.Submit(desc,completion),"Probe positive submission failed.");
        Check(bool(completion) && m_device.Wait(completion,10000000000ull),"Probe submission did not complete within ten seconds.");
        Check(m_device.IsComplete(completion),"Completed probe fence was not observable.");
    }
    TextureReadback Read(TextureHandle texture)
    {
        TextureReadback result;Check(m_device.ReadTexture(texture,result),"Probe RGBA8 readback failed.");
        Check(result.width==Side && result.height==Side && result.rowPitch>=Side*4 &&
            result.format==Format::R8G8B8A8_UNORM && result.pixels.size()>=size_t(result.rowPitch)*(Side-1)+Side*4,
            "Probe RGBA8 layout changed.");return result;
    }
    static void Pixel(const TextureReadback& image,uint32_t x,uint32_t y,std::array<uint8_t,3> expected,const char* label,int tolerance=2)
    {
        const auto* actual=image.pixels.data()+size_t(y)*image.rowPitch+x*4;
        for(uint32_t c=0;c<3;++c)if(std::abs(int(actual[c])-int(expected[c]))>tolerance)
        {
            std::fprintf(stderr,"FAIL %s (%u,%u) actual=%u,%u,%u expected=%u,%u,%u\n",label,x,y,
                actual[0],actual[1],actual[2],expected[0],expected[1],expected[2]);throw std::runtime_error(label);
        }
    }
    void Project(RenderGraph& graph,RenderTexture source,TextureHandle target,ResourceState before)
    {
        const auto output=graph.ImportTexture("Probe selected surface "+std::to_string(reinterpret_cast<uintptr_t>(target)),target,before,ResourceState::ShaderResource);
        const auto vertex=graph.ImportBuffer("Probe projection VB",m_fullscreen,ResourceState::VertexBuffer,ResourceState::VertexBuffer);
        const auto index=graph.ImportBuffer("Probe copied IB",m_indices,ResourceState::IndexBuffer,ResourceState::IndexBuffer);
        Check(output.IsValid() && vertex.IsValid() && index.IsValid(),"Surface projection resource import failed.");
        auto* set=SampleSet(source.texture);
        graph.AddPass("Probe selected Surface pixel",RGRecordingMode::Independent)
            .Read(source.resource,ResourceState::ShaderResource).Read(vertex,ResourceState::VertexBuffer)
            .Read(index,ResourceState::IndexBuffer).Write(output,ResourceState::RenderTarget)
            .SetExecute([this,target,set](ICommandList* commands){
                ColorAttachment color;color.texture=target;color.loadOp=LoadOp::Clear;color.storeOp=StoreOp::Store;
                commands->BeginRendering({&color,1,nullptr});commands->SetViewport({0,0,float(Side),float(Side),0,1});
                commands->SetScissor({0,0,Side,Side});Bind(*commands,false,m_fullscreen,set);
                commands->DrawIndexedInstanced(6,1,0,0,0);commands->EndRendering();
            });
    }
    void Run()
    {
        Check(m_device.Supports(Feature::IndirectDraw),"RHI probe needs native indirect drawing.");
        for(const auto& mode:std::array<std::array<uint32_t,3>,2>{{{0,2,20},{4,2,24}}})
        {
            Mdi(mode[0]?m_padded:m_packed,mode[0],mode[1],mode[2]);const auto image=Read(m_color);
            for(uint32_t y:{10u,22u})for(uint32_t x:{10u,22u})
            {Pixel(image,x,y,{255,0,0},"CopyBuffer + first MDI record");Pixel(image,x+32,y,{0,255,0},"second MDI record");}
        }
        Mdi(m_padded,48,0,24);Pixel(Read(m_color),16,16,{0,0,0},"zero-count MDI must emit no draw");
        InvalidMdi("MDI end range",m_padded,8,2,24);
        InvalidMdi("MDI too short stride",m_padded,4,2,16);
        InvalidMdi("MDI unaligned stride",m_padded,4,1,22);
        InvalidMdi("MDI unaligned offset",m_padded,2,1,20);
        InvalidMdi("MDI count overflow",m_packed,0,UINT32_MAX,20);
        InvalidMdi("zero MDI offset past end",m_padded,52,0,24);
        InvalidMdi("zero MDI still validates stride",m_padded,48,0,16);
        Reject("zero MDI still validates rendering bindings",[&]{auto* c=Commands();Begin(*c);c->EndRendering();
            c->DrawIndexedIndirect(m_packed,40,0,20);return c->Close();});
        ICommandList* commands=nullptr;
        auto* wrong=m_own.Keep(m_device.CreateBuffer({40,0,BufferUsage::Indirect,ResourceState::CopyDestination}));
        commands=Commands();Begin(*commands);commands->DrawIndexedIndirect(wrong,40,0,20);commands->EndRendering();
        Check(commands->Close(),"Wrong-state MDI must remain a submission error.");
        Reject("zero MDI still validates buffer state",[&]{return m_device.Submit(&commands,1);});
        for(const auto& range:std::array<std::array<uint32_t,3>,7>{{{0,0,0},{2,0,4},{0,2,4},{0,0,2},{56,0,8},{0,64,4},{UINT32_MAX-3,0,8}}})
        {
            Reject("CopyBuffer aligned complete ranges",[&]{auto* c=Commands();
                c->CopyBuffer(m_source,range[0],m_indices,range[1],range[2]);return c->Close();});
        }
        Reject("CopyBuffer distinct resources",[&]{auto* c=Commands();c->CopyBuffer(m_source,0,m_source,4,4);return c->Close();});
        Reject("CopyBuffer outside rendering",[&]{auto* c=Commands();Begin(*c);
            c->CopyBuffer(m_source,0,m_indices,0,4);c->EndRendering();return c->Close();});
        commands=Commands();commands->CopyBuffer(m_source,0,m_indices,0,4);
        Check(commands->Close(),"Wrong-state copy should record.");
        Reject("CopyBuffer destination state",[&]{return m_device.Submit(&commands,1);});
        commands=Commands();commands->CopyBuffer(m_indices,0,wrong,0,4);
        Check(commands->Close(),"Wrong-source-state copy should record.");
        Reject("CopyBuffer source state",[&]{return m_device.Submit(&commands,1);});
        std::puts("PASS RHI copy/MDI: source+destination offsets, packed/padded exact-end draws, zero-count and rejected ranges/bindings/states.");
        States();Areas();
    }
    void States()
    {
        auto* buffer=m_own.Keep(m_device.CreateBuffer({16,0,BufferUsage::Vertex,ResourceState::VertexBuffer}));
        auto* a=Commands();Barrier(*a,buffer,ResourceState::VertexBuffer,ResourceState::CopyDestination);
        auto* b=Commands();Barrier(*b,buffer,ResourceState::CopyDestination,ResourceState::CopySource);
        auto* bad=Commands();Barrier(*bad,buffer,ResourceState::CopySource,ResourceState::VertexBuffer);
        Check(a->Close() && b->Close() && bad->Close(),"State summary fixtures did not close.");
        ICommandList* reversed[]={b,a};
        Reject("state summaries reversed lists",[&]{return m_device.Submit(reversed,2);});
        ICommandList* partial[]={a,bad};
        Reject("failed batch cannot partially commit states",[&]{return m_device.Submit(partial,2);});
        ICommandList* ordered[]={a,b};
        Check(m_device.Submit(ordered,2) && m_device.WaitIdle(),"Ordered lists failed after rejected batches changed no states.");
        auto* internal=Commands();Barrier(*internal,buffer,ResourceState::CopySource,ResourceState::CopyDestination);
        Barrier(*internal,buffer,ResourceState::CopySource,ResourceState::VertexBuffer);
        Check(internal->Close(),"Internal state mismatch should remain a submission error.");
        Reject("state summary internal mismatch",[&]{return m_device.Submit(&internal,1);});
        Check(m_device.ResetCommandList(internal),"Invalid summary list could not reset.");
        Barrier(*internal,buffer,ResourceState::CopySource,ResourceState::VertexBuffer);Submit(internal);
        auto* cycle=Commands();Barrier(*cycle,buffer,ResourceState::VertexBuffer,ResourceState::CopyDestination);
        Barrier(*cycle,buffer,ResourceState::CopyDestination,ResourceState::VertexBuffer);Submit(cycle);
        Check(m_device.ResetCommandList(cycle),"Completed summary list could not reset.");
        Barrier(*cycle,buffer,ResourceState::VertexBuffer,ResourceState::CopySource);Submit(cycle);
        auto* verify=Commands();Barrier(*verify,buffer,ResourceState::CopySource,ResourceState::VertexBuffer);Submit(verify);
        std::puts("PASS RHI state summaries: ordered/reverse lists, no partial commit, internal mismatch, transition cycle and reset.");
    }
    void Areas()
    {
        if(!m_device.Supports(Feature::RenderArea))
        {std::puts("SKIP partial RenderArea: backend does not expose Feature::RenderArea.");return;}
        auto* commands=Commands();Begin(*commands,LoadOp::Clear,true,{}, {0,0,1,1},.25f);
        commands->EndRendering();Submit(commands);const Rect inner{16,16,32,32};
        commands=Commands();
        Barrier(*commands,m_color,ResourceState::RenderTarget,ResourceState::RenderTarget);
        Barrier(*commands,m_depth,ResourceState::DepthWrite,ResourceState::DepthWrite);
        Begin(*commands,LoadOp::Clear,true,inner,{1,0,0,1},1);
        commands->EndRendering();Submit(commands);
        auto image=Read(m_color);Pixel(image,8,8,{0,0,255},"partial color clear preserves exterior");
        Pixel(image,32,32,{255,0,0},"partial color clear updates interior");
        commands=Commands();
        Barrier(*commands,m_color,ResourceState::RenderTarget,ResourceState::RenderTarget);
        Barrier(*commands,m_depth,ResourceState::DepthWrite,ResourceState::DepthWrite);
        Begin(*commands,LoadOp::Load,true,{}, {0,0,0,1},1,.5f);
        Bind(*commands,true,m_fullscreen);commands->DrawIndexedInstanced(6,1,0,0,0);commands->EndRendering();Submit(commands);
        image=Read(m_color);Pixel(image,8,8,{0,0,255},"partial depth clear preserves exterior depth");
        Pixel(image,32,32,{255,255,255},"partial depth clear updates interior depth");
        for(const Rect area:std::array<Rect,3>{{{-1,0,1,1},{0,0,Side+1,Side},{0,0,Side,0}}})
        {
            Reject("RenderArea contained nonempty extents",[&]{auto* c=Commands();
                Begin(*c,LoadOp::Clear,false,area);return c->Close();});
        }
        Reject("partial RenderArea scissor containment",[&]{auto* c=Commands();Begin(*c,LoadOp::Clear,false,inner);
            c->SetScissor({0,0,Side,Side});return c->Close();});
        std::puts("PASS RHI RenderArea: interior clear/draw, exterior RGBA+depth preservation and rejected area/scissor.");
    }
};

class SurfaceObserver final:public RenderExtension
{
    std::array<TextureHandle,3> m_capture={};
    std::array<ResourceState,3> m_states={};
public:
    NativeRhiTests rhi;
    bool active=false,observed=false;
    uint32_t mask=0;
    SurfaceObserver(IDevice& device,const std::filesystem::path& shaders):rhi(device,shaders)
    {for(auto& texture:m_capture)texture=rhi.ColorTarget();}
    const char* GetName() const override{return "Conformance Surface demand observer";}
    RenderRequirements GetRequirements(const Renderer&) const override
    {
        RenderRequirements result;if(!active)return result;
        result.needsDepth=true;result.needsNormal=bool(mask&1);result.needsMaterial=bool(mask&2);result.needsMotion=bool(mask&4);
        return result;
    }
    bool AddPasses(RenderStage stage,RenderFrame& frame,RenderGraph& graph) override
    {
        if(!active || stage!=RenderStage::BeforeOpaque)return true;
        Check(frame.requirements.SurfaceOutputMask()==mask && !frame.requirements.needsSurface,
            "Surface requirements lost the raw demand mask.");
        const std::array<RenderTexture,3> outputs={frame.normal,frame.albedo,frame.motion};
        uint32_t count=0;for(uint32_t bit=0;bit<3;++bit)
        {
            const bool requested=bool(mask&(1u<<bit));
            Check(bool(outputs[bit].texture)==requested && outputs[bit].resource.IsValid()==requested,
                "Surface allocated a missing/unrequested output after a mask change.");count+=requested;
        }
        bool found=false;
        for(uint32_t p=0;const auto* pass=graph.GetPass(p);++p)
        {
            if(pass->GetName()!="Surface requested outputs depth" && pass->GetName()!="Opaque masked depth only")continue;
            found=true;uint32_t colors=0;bool depth=false;
            for(const auto& write:pass->GetWrites())
            {if(write.state==ResourceState::RenderTarget)++colors;if(write.handle==frame.depth.resource && write.state==ResourceState::DepthWrite)depth=true;}
            Check(colors==count && depth,"Surface pass MRT count/depth does not match its demand.");
        }
        Check(found,"Surface/depth-only graph pass was not added.");
        for(uint32_t bit=0;bit<3;++bit)if(mask&(1u<<bit))
        {rhi.Project(graph,outputs[bit],m_capture[bit],m_states[bit]);m_states[bit]=ResourceState::ShaderResource;}
        observed=true;return true;
    }
    void Pixels()
    {
        Check(observed,"Surface observer did not execute.");
        const std::array<std::array<uint8_t,3>,3> covered={{{188,188,0},{137,188,225},{0,0,255}}};
        for(uint32_t bit=0;bit<3;++bit)if(mask&(1u<<bit))
        {
            const auto image=rhi.Read(m_capture[bit]);
            NativeRhiTests::Pixel(image,16,32,{0,0,0},"MASK hole leaves selected Surface clear");
            NativeRhiTests::Pixel(image,48,32,covered[bit],"selected Surface output has actual covered pixels",3);
        }
    }
    void MotionDepth(float current,float previous)
    {
        Check(observed && (mask&4),"Motion observer did not execute.");
        const auto image=rhi.Read(m_capture[2]);
        const auto* pixel=image.pixels.data()+size_t(32)*image.rowPitch+32*4;
        // Canvas copies alpha unchanged while encoding RGB with the sRGB transfer function.
        const auto encoded=uint8_t(std::lround(255*(current<=.0031308f?12.92f*current:1.055f*std::pow(current,1.f/2.4f)-.055f)));
        NativeRhiTests::Pixel(image,32,32,{0,0,encoded},"Current orthographic linear depth",2);
        const int expectedAlpha=int(std::lround(255*previous));
        if(std::abs(int(pixel[3])-expectedAlpha)>2)
            std::fprintf(stderr,"Orthographic motion alpha=%u expected=%d current-depth-byte=%u\n",pixel[3],expectedAlpha,pixel[2]);
        Check(std::abs(int(pixel[3])-expectedAlpha)<=2,"Previous orthographic depth must match history linear depth");
    }
};
class DepthOnlyProvider final:public RenderExtension
{
public:
    const char* GetName() const override{return "Conformance depth-only provider without AO";}
    RenderRequirements GetRequirements(const Renderer&) const override
    {RenderRequirements r;r.providesSurface=true;r.needsDepth=true;return r;}
    bool AddPasses(RenderStage stage,RenderFrame& frame,RenderGraph& graph) override
    {
        if(stage!=RenderStage::BeforeOpaque)return true;
        Check(!frame.ao.texture && !frame.ao.resource.IsValid(),"Depth-only provider unexpectedly has AO.");
        const auto texture=frame.depth.texture;
        graph.AddPass("Conformance unproven zero depth",RGRecordingMode::Independent).Write(frame.depth.resource,ResourceState::DepthWrite)
            .SetExecute([texture](ICommandList* commands){
                DepthStencilAttachment depth;depth.texture=texture;depth.state=ResourceState::DepthWrite;
                depth.depthLoadOp=LoadOp::Clear;depth.depthStoreOp=StoreOp::Store;depth.clearDepth=0;
                commands->BeginRendering({nullptr,0,&depth});commands->EndRendering();
            });
        frame.opaqueDepthComplete=false;return true;
    }
};
}
