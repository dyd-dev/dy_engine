#include "dyf/Renderer.h"
#include "dyf/Scene.h"
#include "dyf/Canvas.h"
#include "dyf/Camera.h"
#include "dyf/Image.h"
#include "dyf/Core/ThreadPool.h"
#include "dyf/Platform/Window.h"
#include "dyf/RHI.h"
#include "dyf/Diagnostics/LiveProfiling.h"
#include <chrono>
#include <charconv>
#include <iostream>
#include <map>
#include <thread>
#include <vector>
namespace D=dyf::Diagnostics;
namespace R=dyf::RHI;
#define CHECK(x) do{if(!(x)){std::cerr<<__LINE__<<": " #x "\n";return 1;}}while(false)
struct Options { bool render=false,canvasMode=false,verify=false,features=false;unsigned limit=0; };
bool ParseOptions(int argc,const char* const* argv,Options& options)
{
    bool otherMode=false;
    for(int i=1;i<argc;++i){
        const std::string arg=argv[i];
        if(arg=="--features")options.features=true;
        else if(arg=="--renderer"){options.render=true;options.canvasMode=false;}
        else if(arg=="--canvas"){options.render=true;options.canvasMode=true;otherMode=true;}
        else if(arg=="--raw"){options.render=false;options.canvasMode=false;otherMode=true;}
        else if(arg=="--verify")options.verify=true;
        else if(arg=="--frames"&&i+1<argc){
            const std::string value=argv[++i];const auto parsed=std::from_chars(value.data(),value.data()+value.size(),options.limit);
            if(parsed.ec!=std::errc{}||parsed.ptr!=value.data()+value.size())return false;
        }else return false;
    }
    if(options.features){if(otherMode)return false;options.render=true;options.canvasMode=false;}
    if(options.verify)options.limit=options.features?9:options.render&&!options.canvasMode?16:8;
    return true;
}
dyf::Math::float3 FeaturePosition(unsigned index)
{ return {float(index%16)*.42f-3.15f+(index<128?0.f:100.f),float((index%128)/16)*.42f-1.47f,0}; }
#ifdef DY_CONSUMER_OPTIONS_TEST
int main()
{
    const auto parse=[](std::initializer_list<const char*> arguments,Options& options){std::vector<const char*> argv{"consumer"};argv.insert(argv.end(),arguments);return ParseOptions(int(argv.size()),argv.data(),options);};
    Options options;
    CHECK(parse({"--features"},options)&&options.features&&options.render&&!options.canvasMode&&!options.verify&&options.limit==0);
    options={};CHECK(parse({"--renderer","--features","--verify","--frames","200"},options)&&options.features&&options.verify&&options.limit==9);
    options={};CHECK(parse({"--renderer","--verify"},options)&&options.render&&!options.features&&options.limit==16);
    options={};CHECK(parse({"--canvas","--verify"},options)&&options.canvasMode&&options.limit==8);
    options={};CHECK(parse({"--raw","--frames","0"},options)&&!options.render&&options.limit==0);
    for(const auto mode:{"--raw","--canvas"}){
        options={};CHECK(!parse({"--features",mode},options));options={};CHECK(!parse({mode,"--features"},options));
    }
    for(const auto value:{"","-1","1x","4294967296"}){options={};CHECK(!parse({"--frames",value},options));}
    options={};CHECK(!parse({"--frames"},options));options={};CHECK(!parse({"--unknown"},options));
    for(unsigned i=0;i<256;++i){
        const auto position=FeaturePosition(i);
        CHECK(std::abs(position.y)+.14f<3 && position.z==0);
        if(i<128)CHECK(std::abs(position.x)+.14f<4);else CHECK(position.x-.14f>4);
    }
    std::cout<<"Feature/legacy option validation and 128 visible/128 offscreen bounds passed\n";
}
#else
D::LiveId Control(const D::LiveSnapshot& s,D::LiveId source,const char* key){for(const auto& c:s.controls)if(c.source==source&&c.key==key)return c.id;return 0;}
const D::LiveSourceSnapshot* Source(const D::LiveSnapshot& s,D::LiveId id){for(const auto& source:s.sources)if(source.source==id)return &source;return nullptr;}
int main(int argc,char** argv)
{
    Options options;if(!ParseOptions(argc,argv,options)){
        std::cerr<<"Usage: LiveExternalConsumer [--raw|--renderer|--canvas|--features] [--verify] [--frames N]\n--features selects Renderer and cannot be combined with --raw/--canvas.\n";return 2;
    }
    const auto render=options.render,canvasMode=options.canvasMode,verify=options.verify,features=options.features;const auto limit=options.limit;
#ifdef DY_CONSUMER_NULL
    if(render){std::cout<<"Stock Renderer shaders are unavailable in the Null backend; validate this path with D3D12/Vulkan\n";return 77;}
#endif
    std::unique_ptr<R::IDevice> device(R::IDevice::Create({}));CHECK(device);
    const auto deviceId=D::RegisterLiveObject(device.get(),{D::LiveObjectKind::Device,0,0,0,0,"external consumer"});
    int objectIdentity=0;const auto object=D::RegisterLiveObject(&objectIdentity,{D::LiveObjectKind::Entity,deviceId,0,256,1024,"consumer CPU array"});
    std::vector<std::uint32_t> values(256);bool full=true;
    std::unique_ptr<dyf::Platform::Window> window;
    std::unique_ptr<dyf::Renderer> renderer;
    dyf::Scene scene;dyf::Core::ThreadPool pool(2);dyf::Camera featureCamera;
    dyf::Canvas canvas(320,240);dyf::Image image(1,1,{255,0,0,255});
    if(render){
        const void* handle=nullptr;
        window=std::make_unique<dyf::Platform::Window>(320,240,features?"Live Renderer features / 128 visible + 128 offscreen":"Live external consumer");handle=window->GetHandle();
        dyf::RendererConfig config;config.enableProfilerHud=false;config.enableFrustumCulling=false;config.lighting.enabled=features;config.vsync=false;config.threadPool=&pool;
        renderer=dyf::Renderer::Create(*device,handle,config);CHECK(renderer);
        if(!canvasMode){
            const auto first=scene.Add(dyf::CreateCubeMesh(features?.28f:1.f));CHECK(first);
            for(unsigned i=1;i<256;++i)CHECK(scene.AddInstance(first));
            if(features){
                CHECK(featureCamera.LookAt({0,0,8},{0,0,0},{0,1,0})&&featureCamera.SetOrthographic(8,6));
                for(unsigned i=0;i<256;++i)CHECK(scene.GetEntity(static_cast<dyf::EntityID>(i)).SetPosition(FeaturePosition(i)));
            }
            CHECK(scene.Add(dyf::DirectionalLight{}));
        }
    }
    const auto sourceId=render?renderer->GetLiveSourceId():D::AllocateLiveId();
    D::ControlRegistration registration;
    if(!render){
        D::ControlDesc control;control.device=deviceId;control.source=sourceId;control.owner=object;control.key="consumer.uploadFull";control.label="Consumer full upload";control.appliedValue=full;
        registration=D::RegisterControl(control,[&](const D::ControlRequest& r,D::LiveFrameKey){full=std::get<bool>(r.value);return D::ControlApplyResult{D::ControlState::Applied,full,{}};});
    }
    R::BufferDesc desc;desc.size=1024;desc.stride=4;desc.usage=R::BufferUsage::Vertex;desc.initialState=R::ResourceState::CopyDestination;
    auto* buffer=device->CreateBuffer(desc);CHECK(buffer);bool ready=false;
    D::LiveId request=0,revision=0;bool dirtyFull=false;std::vector<D::LiveId> canvasBatches;
    unsigned frame=0,attempts=0;auto stepStarted=D::LiveNowNs();
    for(;!limit||frame<limit;){
        if(window){dyf::Platform::Window::PollEvents();if(!window->IsRunning())break;}
        if(!attempts&&verify&&D::IsLiveCollectorEnabled()&&features){
            const char* key=frame<5?(frame==1||frame==3?"culling":"instancing"):
                (frame==5||frame==7?"rhi.uploadPageReuse":"rhi.uploadSuballocation");
            if(frame){const auto s=D::ReadLiveSnapshot();request=D::RequestLiveControl(Control(*s,sourceId,key),frame==1||frame==4||frame>=7);CHECK(request);}
        }
        if(!attempts&&verify&&D::IsLiveCollectorEnabled()&&render&&!canvasMode&&!features){
            if(frame==1)CHECK(scene.GetEntity(static_cast<dyf::EntityID>(0)).SetPosition({0.2f,0,0}));
            if(frame==2)for(unsigned i=1;i<67;i+=2)CHECK(scene.GetEntity(static_cast<dyf::EntityID>(i)).SetPosition({float(i)*0.01f,0,0}));
            if(frame==4){auto s=D::ReadLiveSnapshot();const auto* source=Source(*s,sourceId);CHECK(source);revision=source->latest.appliedRevision;}
            if(frame==6)CHECK(scene.AddInstance(scene.GetEntity(static_cast<dyf::EntityID>(0))));
            if(frame==7)CHECK(scene.GetEntity(static_cast<dyf::EntityID>(0)).SetPosition({1000000,0,0}));
            const char* key=nullptr;D::ControlValue value=false;
            switch(frame){case 3:case 4:key="instancing";break;case 5:key="exposure";value=0.1;break;case 6:key="instancing";value=true;break;case 7:key="culling";value=true;break;case 8:key="lighting";value=true;break;case 9:key="shadows";value=true;break;case 10:key="shadowQuality";value=std::int64_t(dyf::ShadowQuality::High);break;case 11:key="hdr";value=true;break;case 12:key="hdr";break;case 13:key="vsync";value=true;break;case 14:key="vsync";break;case 15:key="parallelRecording";value=true;break;default:break;}
            request=0;if(key){auto s=D::ReadLiveSnapshot();request=D::RequestLiveControl(Control(*s,sourceId,key),value);CHECK(request);}
        }
        if(!attempts&&verify&&D::IsLiveCollectorEnabled()&&canvasMode&&frame==5){auto s=D::ReadLiveSnapshot();request=D::RequestLiveControl(Control(*s,sourceId,"exposure"),0.1);CHECK(request);}
        if(!attempts&&verify&&D::IsLiveCollectorEnabled()&&!render&&frame==3){request=D::RequestLiveControl(registration.GetId(),false);CHECK(request);}
        const auto previous=render?renderer->GetLastRenderCompletion():R::FenceHandle{};
        {
            D::LiveFrameScope outer(deviceId,sourceId);device->PrepareLiveControls(outer.GetKey());D::PollControls(object,outer.GetKey());
            D::SetLiveObjectGauge(outer.GetKey(),{D::LiveValueState::Ready,values.size()});
            {D::LiveCpuScope cpu(D::LiveStage::Data,"Consumer CPU array update");for(unsigned i=0;i<(full?256u:64u);++i)values[i]=frame;}
            if(render){
                if(features&&verify&&frame>=5){
                    auto context=D::GetLiveContext();context.owner=object;context.object=object;context.batch=D::AllocateLiveId();D::LiveContextScope probe(context);
                    auto* commands=device->AcquireCommandList();CHECK(commands);
                    if(ready){const R::ResourceBarrierDesc before{buffer,nullptr,R::ResourceState::VertexBuffer,R::ResourceState::CopyDestination,{}};commands->ResourceBarrier(&before,1);}
                    for(unsigned update=0;update<32;++update)CHECK(device->UpdateBuffer(*commands,buffer,update*4,&values[update],4));
                    const R::ResourceBarrierDesc after{buffer,nullptr,R::ResourceState::CopyDestination,R::ResourceState::VertexBuffer,{}};commands->ResourceBarrier(&after,1);CHECK(commands->Close());
                    R::FenceHandle fence;CHECK(device->Submit({&commands,1,nullptr,0},fence));device->DestroyCommandList(commands);CHECK(device->Wait(fence,UINT64_MAX));ready=true;
                }
                canvas.Clear();canvas.FillRect({0,0,6,6});canvas.FillRect({6,0,6,6});canvas.SetClipRect({0,0,10,10});canvas.FillRect({0,6,6,6});canvas.Image(image,{0,0,6,6});CHECK(canvas.SetViewport({0,0,8,8}));canvas.Image(image,{0,0,6,6});
                if(canvasMode)CHECK(renderer->Render(canvas));else if(features)CHECK(renderer->Render(scene,featureCamera,canvas));else CHECK(renderer->Render(scene,canvas));
            }
            else{
                auto* commands=device->AcquireCommandList();CHECK(commands);
                if(ready){const R::ResourceBarrierDesc before{buffer,nullptr,R::ResourceState::VertexBuffer,R::ResourceState::CopyDestination,{}};commands->ResourceBarrier(&before,1);}
                const auto count=full?256u:64u;
                auto context=D::GetLiveContext();context.object=object;context.batch=D::GetLiveBatchId(object,count);D::LiveContextScope batch(context);
                D::LiveEvent e;e.id=context.batch;e.context=context;e.kind=D::LiveEventKind::Batch;e.stage=D::LiveStage::Data;e.elements=count;e.bytes=count*4;e.stride=4;e.resource=D::FindLiveObject(buffer);D::RecordLiveEvent(e);D::RecordLiveLink({object,e.id});
                CHECK(device->UpdateBuffer(*commands,buffer,0,values.data(),count*4));
                const R::ResourceBarrierDesc after{buffer,nullptr,R::ResourceState::CopyDestination,R::ResourceState::VertexBuffer,{}};commands->ResourceBarrier(&after,1);CHECK(commands->Close());
                R::FenceHandle fence;CHECK(device->Submit({&commands,1,nullptr,0},fence));device->DestroyCommandList(commands);CHECK(device->Wait(fence,UINT64_MAX));ready=true;
            }
            outer.SetOutcome(D::LiveOutcome::Completed);
        }
        if(render&&renderer->GetLastRenderCompletion()==previous){++attempts;CHECK(D::LiveNowNs()-stepStarted<5000000000ull);std::this_thread::sleep_for(std::chrono::milliseconds(1));continue;}
        if(verify&&D::IsLiveCollectorEnabled()){
            const auto s=D::ReadLiveSnapshot();const auto* source=Source(*s,sourceId);CHECK(source);CHECK(bool(Control(*s,sourceId,"consumer.uploadFull"))==!render);CHECK(source->latest.objects.state==D::LiveValueState::Ready&&source->latest.objects.count==(canvasMode?5u:render&&!features&&frame>=6?257u:256u));
            if(features){
                const unsigned instances[]={256,128,128,256,256},draws[]={1,1,128,256,1},culled[]={0,128,128,0,0};
                const auto index=std::min(frame,4u);
                const auto& stats=renderer->GetLastRenderStatistics();const auto& config=renderer->GetAppliedConfig();
                CHECK(stats.mainInstanceCount==instances[index]&&stats.mainDrawCount==draws[index]&&stats.culledEntityCount==culled[index]);
                CHECK(source->latest.recorded.instances==instances[index]+4&&source->latest.recorded.drawIndexed==draws[index]);
                CHECK(config.enableFrustumCulling==(frame==1||frame==2)&&config.enableInstancing==(frame<2||frame>=4));
                if(frame>=5){
                    const bool reuse=frame>=7,suballocate=frame==5||frame==8;
                    for(const auto* key:{"rhi.uploadPageReuse","rhi.uploadSuballocation"}){
                        bool found=false;for(const auto& entry:s->controls)if(entry.source==sourceId&&entry.key==key){found=true;CHECK(entry.writable&&std::get<bool>(entry.appliedValue)==(std::string(key)=="rhi.uploadPageReuse"?reuse:suballocate));}CHECK(found);
                    }
                    std::map<D::LiveId,std::pair<uint64_t,uint64_t>> pageGroups;
                    uint64_t allocations=0,reuses=0;
                    for(const auto& event:source->flow.events)if(event.kind==D::LiveEventKind::UploadMemory&&event.context.object==object){
                        CHECK(event.context.frame==source->latest.key&&event.context.appliedRevision==source->latest.appliedRevision);
                        CHECK(event.arguments[0]==0||event.arguments[0]==1);
                        auto& group=pageGroups[event.context.batch];if(event.arguments[0]){++group.second;++reuses;}else{++group.first;++allocations;}
                    }
                    CHECK(!pageGroups.empty());for(const auto& group:pageGroups){CHECK(group.second.first+group.second.second==(suballocate?1u:32u));if(!reuse)CHECK(group.second.second==0);}
                    CHECK(source->latest.recorded.uploadPageAllocations>=allocations&&source->latest.recorded.uploadPageReuses>=reuses);
                    std::cout<<"uploadPolicy reuse="<<reuse<<" suballocate="<<suballocate<<" probeAllocations="<<allocations<<" probeReuses="<<reuses<<'\n';
                }
                std::cout<<"features culling="<<config.enableFrustumCulling<<" instancing="<<config.enableInstancing<<" mainDraws="<<stats.mainDrawCount<<" mainInstances="<<stats.mainInstanceCount<<" culled="<<stats.culledEntityCount<<'\n';
            }else if(render&&!canvasMode){
                if(frame==0)CHECK(source->latest.recorded.drawIndexed==1);
                if(frame>=3&&frame<6)CHECK(source->latest.recorded.drawIndexed==256);if(frame==4)CHECK(source->latest.appliedRevision==revision);
                if(frame==2)for(const auto& e:source->flow.events)if(e.reason==D::LiveSplitReason::DirtyRangeLimit&&e.bytes==256*64)dirtyFull=true;
                const auto& config=renderer->GetAppliedConfig();const auto& stats=renderer->GetLastRenderStatistics();
                if(frame>=5)CHECK(config.exposure==0.1f);
                if(frame==6){CHECK(stats.mainInstanceCount==257&&stats.mainDrawCount==1);bool resized=false;for(const auto& e:source->flow.events)if(e.kind==D::LiveEventKind::DataRange&&e.fullUpload&&e.bytes==257*64)resized=true;CHECK(resized);}
                if(frame>=7)CHECK(config.enableFrustumCulling&&stats.mainInstanceCount==256&&stats.culledEntityCount==1);
                if(frame>=8)CHECK(config.lighting.enabled);
                if(frame>=9){CHECK(config.lighting.shadows);bool shadow=false;for(const auto& span:source->flow.gpuSpans)shadow|=span.scope==D::LiveGpuScope::RendererShadow;CHECK(shadow&&source->latest.recorded.drawIndexed>stats.mainDrawCount);}
                if(frame>=10)CHECK(config.lighting.shadowQuality==dyf::ShadowQuality::High);
                if(frame>=11)CHECK(config.enableHdrRendering==(frame==11));
                if(frame>=13)CHECK(config.vsync==(frame==13));
                if(frame==15)CHECK(config.enableParallelRenderGraph&&config.threadPool==&pool);
            }else if(!render)CHECK(source->latest.recorded.uploadBytes==(full?1024u:256u));
            if(request){bool applied=false;for(const auto& status:s->controlStatus)if(status.request==request)applied=status.state==D::ControlState::Applied&&status.appliedRevision==source->latest.appliedRevision&&status.appliedFrame.device==deviceId&&status.appliedFrame.source==sourceId;CHECK(applied);}
            if(render){const auto toneMap=!canvasMode&&renderer->GetAppliedConfig().enableHdrRendering?1u:0u;const auto expected=4u+toneMap;if(source->latest.recorded.draw!=expected)std::cerr<<"frame="<<frame<<" Draw actual="<<source->latest.recorded.draw<<" expected="<<expected<<" (Canvas=4, ToneMap="<<toneMap<<")\n";CHECK(source->latest.recorded.draw==expected);unsigned fullscreen=0;for(const auto& e:source->flow.events)if(e.kind==D::LiveEventKind::Command&&e.commandKind==D::LiveCommandKind::Draw&&e.arguments[0]==3)++fullscreen;CHECK(fullscreen==toneMap);if(canvasMode){std::vector<D::LiveId> batches;bool scissor=false,texture=false,viewport=false;for(const auto& e:source->flow.events)if(e.kind==D::LiveEventKind::Batch){batches.push_back(e.id);CHECK(e.context.object==D::FindLiveObject(&canvas)&&e.resource&&e.stride==32);scissor|=e.reason==D::LiveSplitReason::Scissor;texture|=e.reason==D::LiveSplitReason::Texture;viewport|=e.reason==D::LiveSplitReason::Viewport;}CHECK(batches.size()==4&&scissor&&texture&&viewport);if(frame==0)canvasBatches=batches;else CHECK(batches==canvasBatches);}}
            std::cout<<"frame="<<frame<<" source="<<sourceId<<" revision="<<source->latest.appliedRevision<<" drawIndexed="<<source->latest.recorded.drawIndexed<<" uploadBytes="<<source->latest.recorded.uploadBytes<<" objects="<<source->latest.objects.count<<"\n";
        }else if(verify)CHECK(!D::ReadLiveSnapshot());
        ++frame;attempts=0;stepStarted=D::LiveNowNs();
        if(!features)std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    if(verify&&render&&!canvasMode&&!features&&D::IsLiveCollectorEnabled())CHECK(dirtyFull);
    if(verify)CHECK(frame==limit);
    CHECK(device->WaitIdle());device->DestroyBuffer(buffer);device->ReleaseLiveControls(sourceId);D::RetireLiveObject(object);
    std::cout<<"External consumer public "<<(canvasMode?"Canvas":render?"Renderer":"raw RHI")<<" contracts passed\n";
}
#endif
