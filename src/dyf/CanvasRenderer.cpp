#include "../Platform/Profiler/CollectorInternal.h"
#include "dyf/Image.h"
#include "RendererInternal.h"
#include "dyf/Platform/Profiler.h"
#include "dyf/Canvas.h"

#include "dyf/RHI/Buffer.h"
#include "dyf/RHI/ICommandList.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/Pipeline.h"
#include "dyf/RHI/Binding.h"
#include "dyf/RHI/Shader.h"
#include "dyf/RHI/Texture.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

namespace dyf
{
// 기존 정점 버퍼 목록을 작업 목록에 복사하고 이미지 사용 표시를 지운다. 시작 이미지 수는 실패 정리의 경계다.
Renderer::Impl::CanvasFrame::CanvasFrame(Impl& renderer):owner(renderer),imageCount(renderer.canvasImages.size())
{
    owner.canvasPendingVertices=owner.canvasVertices;
    owner.canvasVertexCursor=0;
    for(auto& image:owner.canvasImages)image.used=false;
}
// 확정하지 못한 프레임은 새 버퍼와 추가 이미지 자원만 반납한다. 이 함수가 이미 제출한 GPU 명령을 취소하지는 않는다.
Renderer::Impl::CanvasFrame::~CanvasFrame()
{
    if(committed)return; // 제출 성공으로 캐시를 확정했으면 실패 정리를 건너뛴다.
    for(size_t i=0;i<owner.canvasPendingVertices.size();++i)
        if(i>=owner.canvasVertices.size() || owner.canvasPendingVertices[i].buffer!=owner.canvasVertices[i].buffer) // 기존 슬롯에 없거나 핸들이 바뀐 버퍼만 이번 후보로 반납한다.
            owner.device->DestroyBuffer(owner.canvasPendingVertices[i].buffer);
    owner.canvasPendingVertices.clear();
    while(owner.canvasImages.size()>imageCount) {
        const auto& image=owner.canvasImages.back();
        owner.device->DestroyResourceSet(image.set);owner.device->DestroyTexture(image.texture);
        owner.canvasImages.pop_back();
    }
}
// 제출 성공 뒤 교체된 이전 버퍼를 놓고 작업 목록을 정식 캐시로 바꾼다. 사용하지 않은 이미지는 보존 창에 따라 제거한다.
void Renderer::Impl::CanvasFrame::Commit()
{
    for(size_t i=0;i<owner.canvasVertices.size();++i)
        if(owner.canvasVertices[i].buffer!=owner.canvasPendingVertices[i].buffer) // 교체된 이전 버퍼만 반납한다.
            owner.device->DestroyBuffer(owner.canvasVertices[i].buffer);
    owner.canvasVertices.swap(owner.canvasPendingVertices);
    owner.canvasPendingVertices.clear();
    // 이미지를 쓰지 않은 Canvas 프레임의 연속 확정 횟수가 최대 동시 진행 프레임 수에 도달하면 캐시에서 제거하며, 캐시 OFF이면 미사용 이미지를 바로 반납한다.
    // 캐시의 Image가 불변 픽셀을 소유해 주소 재사용에 따른 오인식을 막고, 제거한 자원을 실제 GPU 사용 종료까지 보존하는 책임은 RHI에 있다.
    const auto window=std::max(1u,owner.device->GetDesc().maxFramesInFlight);
    owner.canvasImages.erase(std::remove_if(owner.canvasImages.begin(),owner.canvasImages.end(),[&](CanvasImageSlot& image) {
        if(image.used) {image.unusedFrames=0;return false;} // 이번 프레임에 쓰였으면 미사용 횟수를 0으로 되돌리고 남긴다.
        if(owner.config.enableCanvasImageCache&&++image.unusedFrames<window)return false; // 캐시가 켜져 있고 보존 창이 지나지 않았으면 유지한다.
        owner.device->DestroyResourceSet(image.set);owner.device->DestroyTexture(image.texture);return true;
    }),owner.canvasImages.end());
    committed=true;
}

// Canvas의 위치·UV·색 정점, 선형 샘플러, 알파 합성 규칙과 32바이트 상수를 사용하는 RHI 파이프라인을 준비한다.
bool Renderer::Impl::InitializeCanvas(RHI::Format format)
{
    using namespace RHI;
    if(canvasPipeline) return true; // 이미 준비된 Canvas 파이프라인을 재사용한다.
    const auto shaders=DefaultShaders();
    if(!canvasVertexShader) canvasVertexShader=device->CreateShader(ShaderDescription(CanvasVertex,shaders.canvasVertex)); // 정점 셰이더가 없으면 선택된 기본/재정의 바이트로 생성한다.
    if(!canvasFragmentShader) canvasFragmentShader=device->CreateShader(ShaderDescription(CanvasFragment,shaders.canvasFragment)); // 픽셀 셰이더가 없으면 생성한다.
    if(!canvasVertexShader || !canvasFragmentShader) return false; // 둘 중 하나라도 생성 실패하면 파이프라인을 만들지 않는다.
    const VertexBufferLayout buffer{0,sizeof(Canvas::Vertex),VertexStepMode::Vertex};
    const std::array<VertexAttribute,3> attributes={{{0,0,Format::R32G32_FLOAT,0},{1,0,Format::R32G32_FLOAT,8},{2,0,Format::R32G32B32A32_FLOAT,16}}};
    SamplerDesc sampler;
    sampler.minFilter=sampler.magFilter=sampler.mipFilter=SamplerFilter::Linear;
    sampler.addressU=sampler.addressV=sampler.addressW=SamplerAddressMode::ClampToEdge;
    sampler.mipLodBias=sampler.minLod=sampler.maxLod=0;
    const std::array<ResourceBindingLayout,2> bindings={{{0,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}},
        {1,ResourceBindingType::StaticSampler,1,ShaderStageFlags::Fragment,sampler}}};
    // RGB는 새 RGB×새 알파 + 기존 RGB×(1-새 알파)로 합성한다. 출력 알파는 새 알파 + 기존 알파×(1-새 알파)다.
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
// 이미지와 정점 버퍼를 확보한 뒤 각 Draw의 viewport·clip으로 명령을 기록한다. 제출과 Present는 호출자가 수행한다.
bool Renderer::Impl::RecordCanvas(const Canvas& canvas, RHI::ICommandList& commandList, RHI::TextureHandle target, bool overlay, bool graphManagedTarget)
{
#if defined(DY_LIVE_PROFILING_ENABLED) || defined(DY_PROFILER_TRACING_ENABLED) // 관측 또는 추적 빌드에서만 CPU 기록 구간을 포함한다.
    Platform::CpuScope cpuScope(Platform::LiveStage::Record,"Canvas recording");
#endif
    using namespace RHI;
    if(!canvas.IsValid()) {std::fprintf(stderr,"dyf: Invalid canvas dimensions.\n");return false;} // Canvas 크기가 0이면 진단하고 기록을 실패시킨다.
    if(!target || !InitializeCanvas(target->GetDesc().format)) return false; // 출력 타깃 또는 파이프라인 준비가 없으면 실패한다.
    BufferHandle vertexBuffer=nullptr;
    if(!canvasWhite.IsValid())canvasWhite=Image(1,1,{255,255,255,255},dyf::ColorSpace::Linear); // 단색 도형에도 텍스처 경로를 쓰도록 흰색 한 픽셀을 준비한다.
    // 불변 Image 픽셀 주소·크기·색 공간으로 텍스처를 재사용한다. 신규 소유자를 먼저 캐시에 넣어 중간 실패도 CanvasFrame이 정리하게 한다.
    const auto resolve=[&](const Image& image)->uint32_t {
        for(uint32_t i=0;i<canvasImages.size();++i) {
            auto& cached=canvasImages[i];
            if((config.enableCanvasImageCache||cached.used) && cached.source.GetPixels().data()==image.GetPixels().data() && // 캐시 허용 또는 이번 프레임 내 재사용이고 이미지 정체성이 같으면 기존 슬롯을 쓴다.
                cached.source.GetColorSpace()==image.GetColorSpace() &&
                cached.source.GetWidth()==image.GetWidth() && cached.source.GetHeight()==image.GetHeight()) {
                cached.used=true;return i;
            }
        }
        if(image.GetPixels().size()>UINT32_MAX || canvasImages.size()>=UINT32_MAX)return UINT32_MAX; // 업로드 크기나 슬롯 인덱스가 32비트 범위를 넘으면 실패 표식을 반환한다.
        // 예외 시 모든 핸들을 회수할 수 있도록 새 Image·텍스처·세트의 소유권을 다음 의존 자원의 생성·기록 전에 프레임 슬롯에 등록한다.
        canvasImages.push_back({image,nullptr,nullptr,0,true});
        auto& cached=canvasImages.back();
        TextureDesc desc;
        desc.width=image.GetWidth();desc.height=image.GetHeight();
        desc.format=image.GetColorSpace()==dyf::ColorSpace::Srgb ? Format::R8G8B8A8_UNORM_SRGB : Format::R8G8B8A8_UNORM;
        desc.usage=TextureUsage::ShaderResource;
        auto* texture=cached.texture=device->CreateTexture(desc);
        if(!texture)return UINT32_MAX; // 텍스처 생성 실패는 유효 인덱스가 아닌 최댓값으로 알린다.
        ResourceBarrierDesc barrier{nullptr,texture,ResourceState::Undefined,ResourceState::CopyDestination,{}};
        commandList.ResourceBarrier(&barrier,1);
        if(!device->UpdateTexture(commandList,texture,0,0,image.GetPixels().data(),static_cast<uint32_t>(image.GetPixels().size()),image.GetWidth()*4,static_cast<uint32_t>(image.GetPixels().size())))return UINT32_MAX; // 픽셀 업로드 기록이 실패하면 이미지 해결도 실패한다.
        barrier.before=ResourceState::CopyDestination;barrier.after=ResourceState::ShaderResource;commandList.ResourceBarrier(&barrier,1);
        ResourceBinding binding;binding.texture=texture;
        cached.set=device->CreateResourceSet({canvasPipeline,&binding,1});
        return cached.set ? static_cast<uint32_t>(canvasImages.size()-1) : UINT32_MAX;
    };
    if(resolve(canvasWhite)==UINT32_MAX)return false; // 기본 흰색 이미지조차 준비하지 못하면 기록을 중단한다.
    // 각 Draw의 실제 이미지 또는 단색용 흰색 이미지를 resolve로 준비하고 캐시 인덱스를 저장한다. 뒤의 그리기 순회는 이 인덱스로 텍스처와 리소스 세트를 찾아 쓴다.
    canvasDrawImages.clear();canvasDrawImages.reserve(canvas.draws.size());
    for(const auto& draw:canvas.draws) {
        const auto index=resolve(draw.image.IsValid()?draw.image:canvasWhite);
        if(index==UINT32_MAX)return false;canvasDrawImages.push_back(index); // Draw에 필요한 이미지가 실패하면 제출하지 않는다.
    }
    if(!canvas.vertices.empty()) // 정점이 있을 때만 버퍼를 확보하고 업로드한다.
    {
        // 정점이 있으면 필요한 바이트 수를 계산한다. 여러 Canvas 기록은 커서로 서로 다른 슬롯을 쓴다.
        const uint64_t bytes=canvas.vertices.size()*sizeof(Canvas::Vertex);
        if(bytes>UINT32_MAX)return false; // RHI 업로드 크기 표현 범위를 넘으면 실패한다.
        if(canvasVertexCursor==canvasPendingVertices.size())canvasPendingVertices.push_back({}); // 사용할 버퍼 슬롯이 없으면 작업 목록에 하나 추가한다.
        auto& slot=canvasPendingVertices[canvasVertexCursor++];
        if(!config.enableCanvasVertexBufferReuse || !slot.buffer || slot.buffer->GetDesc().size<bytes) { // 재사용을 껐거나 버퍼가 없거나 작으면 새 버퍼를 만든다.
            // 재사용을 켜면 필요한 용량까지 두 배씩 늘리고, 끄면 이번 정점 수만큼만 할당한다.
            uint64_t capacity=!config.enableCanvasVertexBufferReuse?bytes:slot.buffer ? slot.buffer->GetDesc().size : sizeof(Canvas::Vertex)*64;
            while(capacity<bytes)capacity=std::min(uint64_t(UINT32_MAX),capacity*2);
            auto* replacement=device->CreateBuffer({static_cast<uint32_t>(capacity),sizeof(Canvas::Vertex),BufferUsage::Vertex,ResourceState::CopyDestination});
            if(!replacement)return false; // 새 버퍼 생성이 실패하면 기존 캐시를 확정하지 않는다.
            slot.buffer=replacement;slot.ready=false;
        }
        vertexBuffer=slot.buffer;
        if(slot.ready) {const ResourceBarrierDesc barrier{vertexBuffer,nullptr,ResourceState::VertexBuffer,ResourceState::CopyDestination,{}};commandList.ResourceBarrier(&barrier,1);} // ready는 이 버퍼를 VertexBuffer 상태로 사용하도록 기록했다는 표시다. 다시 업로드하기 전에 CopyDestination으로 전이하며 GPU 실행 완료를 뜻하지는 않는다.
        if(!device->UpdateBuffer(commandList,vertexBuffer,0,canvas.vertices.data(),static_cast<uint32_t>(bytes)))return false; // 정점 업로드를 기록하지 못하면 실패한다.
        const ResourceBarrierDesc barrier{vertexBuffer,nullptr,ResourceState::CopyDestination,ResourceState::VertexBuffer,{}};commandList.ResourceBarrier(&barrier,1);
        slot.ready=true;
    }
    {
        // 그래프가 타깃 상태를 관리하지 않을 때만 Present에서 RenderTarget으로 직접 전이한다.
        const ResourceBarrierDesc before{nullptr,target,ResourceState::Present,ResourceState::RenderTarget,{}};
        if(!graphManagedTarget) commandList.ResourceBarrier(&before,1);
        // overlay이면 기존 색을 Load하고, 단독 Canvas이면 배경색으로 Clear한다.
        ColorAttachment color; color.texture=target;color.loadOp=overlay ? LoadOp::Load : LoadOp::Clear;color.storeOp=StoreOp::Store;
        color.clearColor[0]=canvas.clearColor.x;
        color.clearColor[1]=canvas.clearColor.y;
        color.clearColor[2]=canvas.clearColor.z;
        color.clearColor[3]=canvas.clearColor.w;
        if(IsSrgbFormat(target->GetDesc().format)) // sRGB 하드웨어 인코딩 전 clear 색을 선형 값으로 바꾼다.
        {
            for(uint32_t channel=0;channel<3;++channel)
            {
                const float value=color.clearColor[channel];
                color.clearColor[channel]=value<=0.04045f ? value/12.92f : std::pow((value+0.055f)/1.055f,2.4f);
            }
        }
        commandList.BeginRendering({&color,1,nullptr});
        if(vertexBuffer) // 정점 버퍼가 있을 때만 Draw를 실행한다. 정점이 없어도 Clear는 가능하다.
        {
            commandList.BindGraphicsPipeline(canvasPipeline);commandList.BindVertexBuffer(0,vertexBuffer,0);
            for(size_t drawIndex=0;drawIndex<canvas.draws.size();)
            {
                const auto& draw=canvas.draws[drawIndex];
                const auto imageSlot=canvasDrawImages[drawIndex];
                uint32_t count=draw.count;
                auto splitReason=config.enableCanvasBatching?Platform::LiveSplitReason::EndOfInput:Platform::LiveSplitReason::None;
                size_t next=drawIndex+1;
                const auto sameRect=[](const Rectangle& a,const Rectangle& b) {return a.x==b.x && a.y==b.y && a.width==b.width && a.height==b.height;};
                // 같은 이미지·viewport·clip의 인접 정점 구간만 병합해 정점·알파 순서와 정점별 tint를 유지한다.
                while(config.enableCanvasBatching&&next<canvas.draws.size()) {
                    const auto& candidate=canvas.draws[next];
                    if(canvasDrawImages[next]!=imageSlot){splitReason=Platform::LiveSplitReason::Texture;break;} // 다른 텍스처가 필요하면 배치를 분리한다.
                    if(uint64_t(draw.first)+count!=candidate.first){splitReason=Platform::LiveSplitReason::Noncontiguous;break;} // 정점 범위가 연속하지 않으면 병합하지 않는다.
                    if(!sameRect(draw.viewport,candidate.viewport)){splitReason=Platform::LiveSplitReason::Viewport;break;} // viewport가 다르면 별도 Draw로 남긴다.
                    if(!sameRect(draw.clip,candidate.clip)){splitReason=Platform::LiveSplitReason::Scissor;break;} // clip이 다르면 별도 Draw로 남긴다.
                    if(uint64_t(count)+candidate.count>UINT32_MAX){splitReason=Platform::LiveSplitReason::Capacity;break;} // 병합 정점 수가 32비트를 넘기 전에 끊는다.
                    count+=candidate.count;++next;
                }
                drawIndex=next;
                const auto& v=draw.viewport;const auto& c=draw.clip;
                // 화면·viewport·clip의 교집합을 scissor로 만든다. 잘린 영역이 비면 Draw를 생략한다.
                const float left=std::max({0.0f,v.x,v.x+c.x}),top=std::max({0.0f,v.y,v.y+c.y});
                const float right=std::min({static_cast<float>(target->GetDesc().width),v.x+v.width,v.x+c.x+c.width});
                const float bottom=std::min({static_cast<float>(target->GetDesc().height),v.y+v.height,v.y+c.y+c.height});
                if(right<=left || bottom<=top) continue; // 실수 좌표의 교집합이 비었으면 건너뛴다.
                const Rect scissor{static_cast<int32_t>(left),static_cast<int32_t>(top),static_cast<uint32_t>(right-left),static_cast<uint32_t>(bottom-top)};
                if(!scissor.width || !scissor.height) continue; // 정수 픽셀로 바꾼 뒤 면적이 없으면 건너뛴다.
                // Canvas와 배치·정점 버퍼의 관계를 관측 이벤트에 연결한다. 이 메타데이터가 그리기 결과를 대신하지 않는다.
                auto batchContext=Platform::GetLiveContext();batchContext.object=canvas.liveId;
                if(Platform::IsLiveCollectorEnabled()){ // 수집이 켜져 있을 때만 배치와 자원 관계를 기록한다.
                    const auto canvasBatch=Platform::GetLiveBatchId(liveId,canvas.liveId);batchContext.batch=Platform::GetLiveBatchId(canvasBatch,(std::uint64_t(draw.first)<<32)|count);
                    Platform::LiveEvent batch;batch.id=batchContext.batch;batch.context=batchContext;batch.kind=Platform::LiveEventKind::Batch;batch.stage=Platform::LiveStage::Data;batch.first=draw.first;batch.elements=count;batch.bytes=std::uint64_t(count)*sizeof(Canvas::Vertex);batch.stride=sizeof(Canvas::Vertex);batch.resource=Platform::FindLiveObject(vertexBuffer);batch.reason=splitReason;Platform::RecordLiveEvent(batch);Platform::RecordLiveLink({canvas.liveId,batch.id});
                }
                Platform::LiveContextScope linkedBatch(batchContext);
                commandList.SetViewport({v.x,v.y,v.width,v.height,0,1});
                commandList.SetScissor(scissor);
                // 픽셀 좌표를 clip 좌표로 바꾸는 배율·이동량과 대상의 sRGB 변환 플래그를 전달한다.
                const float transform[8]={2/v.width,-2/v.height,-1,1,
                    IsSrgbFormat(target->GetDesc().format)?1.f:0.f,0,0,0};
                commandList.SetInlineConstants(0,sizeof(transform),transform);
                commandList.BindResourceSet(canvasImages[imageSlot].set);
                commandList.DrawInstanced(count,1,draw.first,0);
            }
        }
        commandList.EndRendering();
        const ResourceBarrierDesc after{nullptr,target,ResourceState::RenderTarget,ResourceState::Present,{}};
        if(!graphManagedTarget) commandList.ResourceBarrier(&after,1); // 직접 관리한 타깃은 다시 Present 상태로 돌린다.
    }
    return true;
}

// 프레임 시작 → 명령 기록/닫기 → 제출 → 캐시 확정 → 선택적 캡처 → Present 순서로 진행한다.
bool Renderer::Impl::RenderCanvas(const Canvas& canvas, Image* readback)
{
    Platform::SetLiveObjectGauge(Platform::GetLiveContext().frame,{Platform::LiveValueState::Ready,canvas.draws.size()});
    if(!canvas.IsValid()) {std::fprintf(stderr,"dyf: Invalid canvas dimensions.\n");return false;} // 유효한 Canvas가 아니면 프레임을 시작하지 않는다.
    if(!device->BeginFrame()) // 프레임을 시작하지 못하면 장치 손실 여부를 구분한다.
    {
        if(device->IsLost()) {std::fprintf(stderr,"dyf: Canvas device lost.\n");return false;} // 장치가 손실되었으면 실패를 반환한다.
        livePreparedFrame=0;
        if(readback)*readback={}; // 출력 불가로 이번 프레임을 건너뛴 경우 캡처 결과도 비운다.
        return true;
    }
    framePending=true;
    // Canvas의 단일 명령 목록은 Present로 끝나며, 제출 전이나 기록 실패 시에도 획득한 이미지는 Present 상태다.
    // 프레임 종료 보호자를 명령·캐시 소유자보다 먼저 만들어 readback 실패를 포함한 조기 반환에서도 기록 소유권 정리 뒤 프레임을 닫는다.
    FrameEnd frameEnd{*device,framePending};
    CanvasFrame canvasFrame(*this);
    RHI::ResourceScope resources(*device);
    auto* commands=device->AcquireCommandList();
    if(!commands) {std::fprintf(stderr,"dyf: Canvas command list acquisition failed.\n");return false;} // 명령 리스트를 얻지 못하면 보호자들이 자원을 정리한다.
    resources.Keep(commands);
    const bool recorded=RecordCanvas(canvas,*commands,device->GetBackBuffer(),false);
    const bool closed=recorded && commands->Close();
    RHI::FenceHandle completion;
    const bool submitted=closed && device->Submit({&commands,1,nullptr,0},completion);
    if(!recorded || !closed || !submitted) {std::fprintf(stderr,"dyf: Canvas command assembly/submission failed.\n");return false;} // 기록·닫기·제출 중 하나라도 실패하면 캐시를 확정하지 않는다.
    // 최신 제출 fence를 저장한다. 캐시 Commit은 제출 성공에 대한 확정이고 fence 완료 확인 자체는 아니다.
    lastRenderCompletion=completion;
    canvasFrame.Commit();
    if(readback && !CaptureFrame(*readback))return false; // 캡처를 요청했고 실패했으면 Present 전 실패로 반환한다.
    const bool presented=device->Present();
    framePending=false;
    if(!presented) {std::fprintf(stderr,"dyf: Canvas presentation failed.\n");return false;} // Present 실패를 호출자에게 전달한다.
    DY_PROFILE_FRAME_MARK();
    return true;
}

}
