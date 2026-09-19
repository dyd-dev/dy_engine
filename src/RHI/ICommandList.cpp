#include "dyf/RHI/ICommandList.h"
#include "dyf/RHI/IDevice.h"
#include "RHI/Validation.h"
#include <string>

namespace dyf::RHI
{
void ICommandList::BeginDebugEvent(const char* name, const DebugLabelColor& color)
{
    if(!CanRecordCommands()) return;
    if(!name || !*name || !std::isfinite(color.r) || !std::isfinite(color.g) ||
        !std::isfinite(color.b) || !std::isfinite(color.a)) { m_recordingFailed = true; return; }
    ++m_debugEventDepth;
    BeginDebugEventNative(name, color);
}
void ICommandList::EndDebugEvent()
{
    if(!CanRecordCommands()) return;
    if(!m_debugEventDepth) { m_recordingFailed = true; return; }
    --m_debugEventDepth;
    EndDebugEventNative();
}
void ICommandList::InsertDebugMarker(const char* name, const DebugLabelColor& color)
{
    if(!CanRecordCommands()) return;
    if(!name || !*name || !std::isfinite(color.r) || !std::isfinite(color.g) ||
        !std::isfinite(color.b) || !std::isfinite(color.a)) { m_recordingFailed = true; return; }
    InsertDebugMarkerNative(name, color);
}
void ICommandList::ResetTimestamps(TimestampQueryHandle query,uint32_t first,uint32_t count)
{
    if(!Track(query)) return;
    if(m_rendering || !count || first>=query->GetCount() || count>query->GetCount()-first) {m_recordingFailed=true;return;}
    ResetTimestampsNative(query,first,count);
}
void ICommandList::WriteTimestamp(TimestampQueryHandle query,uint32_t index)
{
    if(!Track(query)) return;
    if(index>=query->GetCount()) {m_recordingFailed=true;return;}
    WriteTimestampNative(query,index);
}
ICommandList::~ICommandList()
{
    if(m_owner)
    {
        std::lock_guard<std::recursive_mutex> lock(m_owner->m_resourceMutex);
        m_owner->m_recordedCommands.erase(this);
        m_references.clear();
        m_owner = nullptr;
    }
}

bool ICommandList::CanRecordCommands()
{
    if(!m_owner || m_recordingClosed) m_recordingFailed = true;
    return !m_recordingFailed;
}

bool ICommandList::Track(const void* handle)
{
    if(!CanRecordCommands()) return false;
    auto reference = m_owner->Reference(handle);
    if(!reference) { m_recordingFailed = true; return false; }
    for(const auto& existing : m_references)
        if(existing.get() == handle) return true;
    if(m_owner->m_borrowedTextures.count(handle)) m_imageGeneration=m_owner->m_imageGeneration;
    m_references.push_back(std::move(reference));
    return true;
}

void ICommandList::ResourceBarrier(const ResourceBarrierDesc* barriers, uint32_t count)
{
    if(!CanRecordCommands()) return;
    if(count && !barriers) { m_recordingFailed = true; return; }
    for(uint32_t i = 0; i < count; ++i)
    {
        const auto& barrier = barriers[i];
        if((barrier.buffer != nullptr) == (barrier.texture != nullptr))
        { m_recordingFailed = true; return; }
        if(!Track(barrier.buffer ? static_cast<const void*>(barrier.buffer) : barrier.texture)) return;

        const uintptr_t object=reinterpret_cast<uintptr_t>(barrier.buffer?static_cast<const void*>(barrier.buffer):barrier.texture);
        uint32_t mip=0,mips=1,layer=0,layers=1;
        if(barrier.buffer)
        {
            if(!IsBufferStateAllowed(barrier.buffer->GetDesc(),barrier.after))
            {m_recordingFailed=true;return;}
        }
        else if(!IsTextureStateAllowed(barrier.texture->GetDesc(),barrier.after) ||
            !ResolveSubresources(barrier.texture,barrier.subresources,mip,mips,layer,layers))
        {m_recordingFailed=true;return;}
        for(uint32_t l=layer;l<layer+layers;++l)for(uint32_t m=mip;m<mip+mips;++m)
        {
            const auto key=std::make_tuple(object,m,l);
            m_stateOperations.push_back([key,before=barrier.before,after=barrier.after](StateMap& states) {
                const auto found=states.find(key);
                if(found==states.end() || found->second!=before)return false;
                found->second=after;return true;
            });
        }

    }
    ResourceBarrierNative(barriers, count);
}


void ICommandList::RequireState(const void* resource,uint32_t mip,uint32_t layer,ResourceState state)
{
    const auto key=std::make_tuple(reinterpret_cast<uintptr_t>(resource),mip,layer);
    m_stateOperations.push_back([key,state](StateMap& states){
        const auto found=states.find(key);
        return found!=states.end() && found->second==state;
    });
}
void ICommandList::BeginRendering(const RenderingDesc& desc)
{
    if(!CanRecordCommands())return;
    if(m_rendering || (desc.colorAttachmentCount && !desc.colorAttachments) ||
        (!desc.colorAttachmentCount && !desc.depthStencilAttachment)) {m_recordingFailed=true;return;}
    uint32_t width=0,height=0;
    const auto attachment=[&](TextureHandle texture,uint32_t mip,uint32_t layer,ResourceState state,LoadOp load,StoreOp store) {
        if(!Track(texture))return false;
        const auto& d=texture->GetDesc();
        if(mip>=d.mipLevels || layer>=d.depthOrArraySize ||
            load<LoadOp::Load || load>LoadOp::Discard || store<StoreOp::Store || store>StoreOp::Discard ||
            !IsTextureStateAllowed(d,state))return false;
        const uint32_t w=std::max(1u,d.width>>mip),h=std::max(1u,d.height>>mip);
        if(width && (width!=w || height!=h))return false;
        width=w;height=h;RequireState(texture,mip,layer,state);return true;
    };
    std::vector<ColorAttachment> colors;
    if(desc.colorAttachmentCount)colors.assign(desc.colorAttachments,desc.colorAttachments+desc.colorAttachmentCount);
    for(uint32_t i=0;i<desc.colorAttachmentCount;++i)
    {
        auto& color=colors[i];
        if(!attachment(color.texture,color.mipLevel,color.arrayLayer,ResourceState::RenderTarget,color.loadOp,color.storeOp))
        {m_recordingFailed=true;return;}
        if(color.loadOp==LoadOp::Clear)
        {
            const auto format=color.texture->GetDesc().format;
            const uint32_t channels=(format==Format::R16_UINT || format==Format::R32_UINT) ? 1u :
                format==Format::R32G32_FLOAT ? 2u : format==Format::R32G32B32_FLOAT ? 3u : 4u;
            for(uint32_t channel=0;channel<channels;++channel)
                if(!std::isfinite(color.clearColor[channel])){m_recordingFailed=true;return;}
            // 없는 채널은 계약상 사용하지 않는다. native에도 무의미한 NaN/Inf를 전달하지 않는다.
            for(uint32_t channel=channels;channel<4;++channel)color.clearColor[channel]=0;
            if(format==Format::R16_UINT || format==Format::R32_UINT)
            {
                const double value=color.clearColor[0];
                const double maximum=format==Format::R16_UINT ? 65535.0 : 4294967295.0;
                if(value<0 || value>maximum || std::trunc(value)!=value)
                {
                    m_owner->ReportDiagnostic(IDevice::DiagnosticSeverity::Error,"BeginRendering: UINT clear requires a nonnegative integral value within the target format range. Values are not rounded, clamped or reinterpreted.");
                    m_recordingFailed=true;return;
                }
            }
        }
    }
    if(desc.depthStencilAttachment)
    {
        const auto& depth=*desc.depthStencilAttachment;
        if((depth.state!=ResourceState::DepthWrite && depth.state!=ResourceState::DepthRead) ||
            !attachment(depth.texture,depth.mipLevel,depth.arrayLayer,depth.state,depth.depthLoadOp,depth.depthStoreOp) ||
            (depth.depthLoadOp==LoadOp::Clear &&
                (!std::isfinite(depth.clearDepth) || depth.clearDepth<0 || depth.clearDepth>1 || depth.state!=ResourceState::DepthWrite)))
        {m_recordingFailed=true;return;}
        if(depth.texture->GetDesc().format==Format::D24_UNORM_S8_UINT &&
            (depth.stencilLoadOp<LoadOp::Load || depth.stencilLoadOp>LoadOp::Discard ||
             depth.stencilStoreOp<StoreOp::Store || depth.stencilStoreOp>StoreOp::Discard))
        {m_recordingFailed=true;return;}
        if(depth.texture->GetDesc().format==Format::D24_UNORM_S8_UINT && depth.stencilLoadOp==LoadOp::Clear &&
            (depth.clearStencil>255 || depth.state!=ResourceState::DepthWrite))
        {
            m_owner->ReportDiagnostic(IDevice::DiagnosticSeverity::Error,"BeginRendering: stencil clear requires DepthWrite and a value in [0,255]; high bits are not silently discarded.");
            m_recordingFailed=true;return;
        }
    }
    m_colorFormats.clear();
    for(uint32_t i=0;i<desc.colorAttachmentCount;++i)
        m_colorFormats.push_back(desc.colorAttachments[i].texture->GetDesc().format);
    m_depthStencilFormat=desc.depthStencilAttachment ? desc.depthStencilAttachment->texture->GetDesc().format : Format::Unknown;
    DepthStencilAttachment depthCopy;
    const auto* nativeDepth=desc.depthStencilAttachment;
    if(nativeDepth && m_depthStencilFormat==Format::D32_FLOAT &&
        (nativeDepth->stencilLoadOp!=LoadOp::Undefined || nativeDepth->stencilStoreOp!=StoreOp::Undefined || nativeDepth->clearStencil!=0))
    {
        m_owner->ReportDiagnostic(IDevice::DiagnosticSeverity::Warning,
            "BeginRendering: D32_FLOAT has no stencil aspect; stencil load/store/clear settings are ignored.");
        depthCopy=*nativeDepth;
        depthCopy.stencilLoadOp=LoadOp::Undefined;
        depthCopy.stencilStoreOp=StoreOp::Undefined;
        depthCopy.clearStencil=0;
        nativeDepth=&depthCopy;
    }
    BeginRenderingNative({colors.data(),desc.colorAttachmentCount,nativeDepth});
}

void ICommandList::EndRendering() { if(CanRecordCommands()) EndRenderingNative(); }
void ICommandList::BindGraphicsPipeline(PipelineHandle pipeline) {if(!Track(pipeline))return;if(pipeline->IsCompute()){m_recordingFailed=true;return;}m_resourceSet=nullptr;BindGraphicsPipelineNative(pipeline);}
void ICommandList::BindComputePipeline(PipelineHandle pipeline) {if(!Track(pipeline))return;if(m_rendering||!pipeline->IsCompute()){m_recordingFailed=true;return;}m_resourceSet=nullptr;BindComputePipelineNative(pipeline);}
void ICommandList::Dispatch(uint32_t x,uint32_t y,uint32_t z) {if(!CanRecordCommands())return;if(m_rendering||!m_pipeline||!m_pipeline->IsCompute()||!x||!y||!z){m_recordingFailed=true;return;}if(ValidateBindings(false,true))DispatchNative(x,y,z);}

void ICommandList::BindResourceSet(ResourceSetHandle resources)
{
    if(!Track(resources))return;
    if(!m_pipeline || resources->GetPipeline()!=m_pipeline || (!m_pipeline->IsCompute() && !m_rendering))
    {m_recordingFailed=true;return;}
    if(!RequireResourceSetStates(resources))return;
    m_resourceSet=resources;
    BindResourceSetNative(resources);
}

bool ICommandList::RequireResourceSetStates(ResourceSetHandle resources)
{
    for(uint32_t i=0;i<resources->GetBindingCount();++i)
    {
        const auto& binding=resources->GetBindings()[i];
        const auto* layout=FindLayoutBinding(m_pipeline->GetLayout(),binding.binding);
        if(!layout){m_recordingFailed=true;return false;}
        if(binding.buffer)
        {
            const auto state=layout->type==ResourceBindingType::ConstantBuffer?ResourceState::ConstantBuffer:
                layout->type==ResourceBindingType::ReadWriteStorageBuffer?ResourceState::UnorderedAccess:ResourceState::ShaderResource;
            RequireState(binding.buffer,0,0,state);
        }
        if(binding.texture)
        {
            uint32_t mip,mips,layer,layers;
            if(!ResolveBindingSubresources(binding.texture,binding,layout->type,mip,mips,layer,layers))
            {m_recordingFailed=true;return false;}
            for(uint32_t l=layer;l<layer+layers;++l)for(uint32_t m=mip;m<mip+mips;++m)
                RequireState(binding.texture,m,l,layout->type==ResourceBindingType::StorageTexture?
                    ResourceState::UnorderedAccess:ResourceState::ShaderResource);
        }
    }
    return true;
}
void ICommandList::BindVertexBuffer(uint32_t binding,BufferHandle buffer,uint32_t offset)
{
    if(!Track(buffer))return;
    if(!m_rendering || !m_pipeline || m_pipeline->IsCompute() || offset>=buffer->GetDesc().size || !HasUsage(buffer->GetDesc().usage,BufferUsage::Vertex))
    {m_recordingFailed=true;return;}
    RequireState(buffer,0,0,ResourceState::VertexBuffer);
    m_vertexBuffers[binding]=buffer;
    BindVertexBufferNative(binding,buffer,offset);
}
void ICommandList::BindIndexBuffer(BufferHandle buffer,Format format,uint32_t offset)
{
    if(!Track(buffer))return;
    if(!m_rendering || (format!=Format::R16_UINT && format!=Format::R32_UINT) || offset>=buffer->GetDesc().size ||
        offset%FormatSize(format) || !HasUsage(buffer->GetDesc().usage,BufferUsage::Index))
    {m_recordingFailed=true;return;}
    RequireState(buffer,0,0,ResourceState::IndexBuffer);
    BindIndexBufferNative(buffer,format,offset);
}
void ICommandList::SetInlineConstants(uint32_t offset, uint32_t size, const void* data)
{
    if(!CanRecordCommands()) return;
    if(!data || !size || !m_pipeline || offset%4 || size%4 || offset>m_pipeline->GetLayout().inlineConstantSize ||
        size>m_pipeline->GetLayout().inlineConstantSize-offset) { m_recordingFailed = true; return; }
    SetInlineConstantsNative(offset, size, data);
}
void ICommandList::SetViewport(const Viewport& viewport) {
    if(!CanRecordCommands())return;
    if(!std::isfinite(viewport.x+viewport.y+viewport.width+viewport.height+viewport.minDepth+viewport.maxDepth) ||
        viewport.width<=0 || viewport.height<=0 || viewport.minDepth<0 || viewport.maxDepth>1 || viewport.minDepth>viewport.maxDepth)
    {m_recordingFailed=true;return;}
    SetViewportNative(viewport);
}
void ICommandList::SetScissor(const Rect& rect) {
    if(!CanRecordCommands())return;
    if(rect.x<0 || rect.y<0 || !rect.width || !rect.height) {m_recordingFailed=true;return;}
    SetScissorNative(rect);
}
void ICommandList::SetStencilReference(uint32_t reference)
{
    if(!CanRecordCommands())return;
    if(reference>255)
    {
        m_owner->ReportDiagnostic(IDevice::DiagnosticSeverity::Error,"SetStencilReference: the 8-bit stencil reference must be in [0,255]; high bits are not silently discarded.");
        m_recordingFailed=true;return;
    }
    SetStencilReferenceNative(reference);
}

bool ICommandList::ValidateBindings(bool indexed, bool compute)
{
    const auto fail=[&](const char* message) {
        m_owner->ReportDiagnostic(IDevice::DiagnosticSeverity::Error,message);
        m_recordingFailed=true;
        return false;
    };
    if(!m_pipeline || m_pipeline->IsCompute()!=compute)
        return fail("Draw/Dispatch: bind a pipeline of the matching type before execution.");
    if(!compute)
    {
        if(!m_rendering || m_colorFormats!=m_pipeline->m_colorFormats || m_depthStencilFormat!=m_pipeline->m_depthStencilFormat)
            return fail("Draw: rendering attachment formats must match the graphics pipeline.");
        for(uint32_t binding:m_pipeline->m_requiredVertexBindings)
        {
            const auto found=m_vertexBuffers.find(binding);
            if(found==m_vertexBuffers.end())return fail("Draw: a vertex attribute binding has no vertex buffer.");
            RequireState(found->second,0,0,ResourceState::VertexBuffer);
        }
        if(indexed)
        {
            if(!m_indexBuffer)return fail("DrawIndexed: no index buffer is bound.");
            RequireState(m_indexBuffer,0,0,ResourceState::IndexBuffer);
        }
    }
    const auto& layout=m_pipeline->GetLayout();
    bool requiresSet=false;
    for(uint32_t i=0;i<layout.bindingCount;++i)
        if(layout.bindings[i].type!=ResourceBindingType::StaticSampler)requiresSet=true;
    if(requiresSet && (!m_resourceSet || m_resourceSet->GetPipeline()!=m_pipeline))
        return fail("Draw/Dispatch: bind a complete ResourceSet for the current pipeline after selecting that pipeline.");
    return !m_resourceSet || RequireResourceSetStates(m_resourceSet);
}
void ICommandList::DrawInstanced(uint32_t vertices, uint32_t instances, uint32_t firstVertex, uint32_t firstInstance)
{ if(CanRecordCommands() && ValidateBindings(false,false)) DrawInstancedNative(vertices, instances, firstVertex, firstInstance); }
void ICommandList::DrawIndexedInstanced(uint32_t indices, uint32_t instances, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
{ if(CanRecordCommands() && ValidateBindings(true,false)) DrawIndexedInstancedNative(indices, instances, firstIndex, vertexOffset, firstInstance); }

bool ICommandList::Close()
{
    if(m_recordingClosed) { m_recordingFailed = true; return false; }
    if(m_debugEventDepth) m_recordingFailed = true;
    const bool nativeClosed = CloseNative();
    m_recordingClosed = true;
    m_recordingFailed = m_recordingFailed || !nativeClosed;
    if(m_recordingFailed && m_owner)m_owner->ReportDiagnostic(IDevice::DiagnosticSeverity::Error,"Close: command recording failed. Invalid commands will not be submitted.");
    return !m_recordingFailed;
}
}


namespace dyf::RHI
{
class RecordedCommandList final : public ICommandList
{
    void BeginDebugEventNative(const char* name, const DebugLabelColor& color) override
    { m_commands.push_back([name=std::string(name),color](ICommandList& n) { n.BeginDebugEventNative(name.c_str(),color); return true; }); }
    void EndDebugEventNative() override
    { m_commands.push_back([](ICommandList& n) { n.EndDebugEventNative(); return true; }); }
    void InsertDebugMarkerNative(const char* name, const DebugLabelColor& color) override
    { m_commands.push_back([name=std::string(name),color](ICommandList& n) { n.InsertDebugMarkerNative(name.c_str(),color); return true; }); }
    void ResourceBarrierNative(const ResourceBarrierDesc* values, uint32_t count) override
    {
        std::vector<ResourceBarrierDesc> copy;
        if(count) copy.assign(values,values+count);
        m_commands.push_back([copy=std::move(copy)](ICommandList& native) { native.ResourceBarrierNative(copy.data(),static_cast<uint32_t>(copy.size())); return true; });
    }
    void BeginRenderingNative(const RenderingDesc& desc) override
    {
        if(m_rendering) { m_recordingFailed=true; return; }
        m_rendering=true;
        const auto pipeline=m_pipeline && !m_pipeline->IsCompute() ? m_pipeline : nullptr;
        m_pipeline=pipeline;
        std::vector<ColorAttachment> colors;
        if(desc.colorAttachmentCount) colors.assign(desc.colorAttachments,desc.colorAttachments+desc.colorAttachmentCount);
        const bool hasDepth=desc.depthStencilAttachment!=nullptr;
        const DepthStencilAttachment depth=hasDepth?*desc.depthStencilAttachment:DepthStencilAttachment{};
        m_commands.push_back([colors=std::move(colors),depth,hasDepth,pipeline](ICommandList& native) {
            native.BeginRenderingNative({colors.data(),static_cast<uint32_t>(colors.size()),hasDepth?&depth:nullptr});
            if(pipeline)native.BindGraphicsPipelineNative(pipeline);
            return true;
        });
    }
    void EndRenderingNative() override
    {
        if(!m_rendering) { m_recordingFailed=true; return; }
        m_rendering=false;
        m_pipeline=nullptr;
        m_resourceSet=nullptr;
        m_vertexBuffers.clear();
        m_indexBuffer=nullptr;
        m_colorFormats.clear();
        m_depthStencilFormat=Format::Unknown;
        m_viewport=m_scissor=false;
        m_commands.push_back([](ICommandList& n){n.EndRenderingNative();return true;});
    }
    void BindComputePipelineNative(PipelineHandle pipeline) override
    {
        m_pipeline=pipeline;
        m_commands.push_back([=](ICommandList& n){n.BindComputePipelineNative(pipeline);return true;});
    }
    void ResetTimestampsNative(TimestampQueryHandle query,uint32_t first,uint32_t count) override
    {m_commands.push_back([=](ICommandList& n){n.ResetTimestampsNative(query,first,count);return true;});}
    void WriteTimestampNative(TimestampQueryHandle query,uint32_t index) override
    {m_commands.push_back([=](ICommandList& n){n.WriteTimestampNative(query,index);return true;});}
    void DispatchNative(uint32_t x,uint32_t y,uint32_t z) override
    {m_commands.push_back([=](ICommandList& n){n.DispatchNative(x,y,z);return true;});}
    void BindGraphicsPipelineNative(PipelineHandle pipeline) override
    {
        m_pipeline=pipeline;
        // A graph may select its pipeline before the callback opens rendering.
        if(m_rendering)m_commands.push_back([=](ICommandList& n){n.BindGraphicsPipelineNative(pipeline);return true;});
    }
    void BindResourceSetNative(ResourceSetHandle resources) override
    { m_commands.push_back([=](ICommandList& n){n.BindResourceSetNative(resources);return true;}); }
    void BindVertexBufferNative(uint32_t binding, BufferHandle buffer, uint32_t offset) override
    { m_commands.push_back([=](ICommandList& n){n.BindVertexBufferNative(binding,buffer,offset);return true;}); }
    void BindIndexBufferNative(BufferHandle buffer, Format format, uint32_t offset) override
    {
        m_indexBuffer=buffer;
        m_commands.push_back([=](ICommandList& n){n.BindIndexBufferNative(buffer,format,offset);return true;});
    }
    void SetInlineConstantsNative(uint32_t offset,uint32_t size,const void* data) override
    {
        const auto* first=static_cast<const uint8_t*>(data);
        std::vector<uint8_t> bytes(first,first+size);
        m_commands.push_back([offset,bytes=std::move(bytes)](ICommandList& n){n.SetInlineConstantsNative(offset,static_cast<uint32_t>(bytes.size()),bytes.data());return true;});
    }
    void SetViewportNative(const Viewport& viewport) override
    { m_viewport=true; m_commands.push_back([=](ICommandList& n){n.SetViewportNative(viewport);return true;}); }
    void SetScissorNative(const Rect& rect) override
    { m_scissor=true; m_commands.push_back([=](ICommandList& n){n.SetScissorNative(rect);return true;}); }
    void SetStencilReferenceNative(uint32_t reference) override
    { m_commands.push_back([=](ICommandList& n){n.SetStencilReferenceNative(reference);return true;}); }
    void DrawInstancedNative(uint32_t vertices,uint32_t instances,uint32_t firstVertex,uint32_t firstInstance) override
    {
        if(!m_rendering || !m_pipeline || !m_viewport || !m_scissor) {m_recordingFailed=true;return;}
        m_commands.push_back([=](ICommandList& n){n.DrawInstancedNative(vertices,instances,firstVertex,firstInstance);return true;});
    }
    void DrawIndexedInstancedNative(uint32_t indices,uint32_t instances,uint32_t firstIndex,int32_t vertexOffset,uint32_t firstInstance) override
    {
        if(!m_rendering || !m_pipeline || !m_indexBuffer || !m_viewport || !m_scissor) {m_recordingFailed=true;return;}
        m_commands.push_back([=](ICommandList& n){n.DrawIndexedInstancedNative(indices,instances,firstIndex,vertexOffset,firstInstance);return true;});
    }
    bool CloseNative() override {return !m_rendering && !m_recordingFailed;}
};
ICommandList* ICommandList::CreateRecorded() {return new RecordedCommandList();}
}
