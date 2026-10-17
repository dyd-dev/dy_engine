#include "CanvasPerformanceSupport.h"
#include <RHI/ThreadPool.h>

using namespace CanvasPerformance;
namespace
{
constexpr uint32_t Size=320;
struct Fixture : Workload
{
    dyf::Image red=Solid(255,0,0),blue=Solid(0,0,255),gray=Solid(128,128,128);
    dyf::Image replacement=Solid(230,35,75);
    bool linearView=false;
    int probeTop=8;
    Fixture(uint32_t extent=Size) : Workload(1024,extent) {}
    int X() const { return 8+int(tick%17); }
    int Y() const { return probeTop+int(tick%7); }
    void Pose(uint32_t next)
    {
        Update(next);
        const float x=float(X()),y=float(Y());
        canvas.FillRect({x,y,270,66},{0,0,0,1});
        // Nonadjacent A/B/A images must remain in source alpha order.
        canvas.Image(red,{x+8,y+8,32,28},{1,1,1,.5f});
        canvas.Image(blue,{x+8,y+8,32,28},{1,1,1,.5f});
        canvas.Image(red,{x+8,y+8,32,28},{1,1,1,.5f});
        Check(canvas.SetViewport({x+50,y+8,50,28}),"Probe viewport rejected");
        canvas.SetClipRect({10,4,24,18});
        canvas.FillRect({-5,-5,65,40},{0,1,0,1});
        Check(canvas.SetViewport({0,0,float(size),float(size)}),"Full viewport restore rejected");
        auto view=gray; view.SetColorSpace(linearView ? dyf::ColorSpace::Linear : dyf::ColorSpace::Srgb);
        auto linear=gray; linear.SetColorSpace(dyf::ColorSpace::Linear);
        canvas.Image(view,{x+112,y+8,24,24});
        canvas.Image(linear,{x+142,y+8,24,24});
        canvas.Image(replacement,{x+178,y+8,24,24});
        // Distinct Image wrappers share the immutable pixel storage.
        const auto ownedCopy=replacement;
        canvas.Image(ownedCopy,{x+210,y+8,24,24});
        for(unsigned i=0;i<4;++i)
        {
            const float left=i==3 ? float(size)-12 : x+8+42*i,top=y+44;
            canvas.FillRect({left,top,12,12},{1,0,0,1});
            // Positive float intersections truncate to zero integer width/height.
            // At the target edge .75 also covers the last pixel center, so a
            // forced one-pixel scissor would visibly overwrite the red probe.
            Check(canvas.SetViewport({i==3 ? float(size)-.75f : left,top,i==2 ? .75f : 12,12}),
                "Subpixel viewport rejected");
            if(i==0) canvas.SetClipRect({0,0,.75f,12});
            if(i==1) canvas.SetClipRect({0,0,12,.75f});
            canvas.FillRect({-1,-1,16,16},{0,1,0,1});
            Check(canvas.SetViewport({0,0,float(size),float(size)}),"Subpixel viewport restore rejected");
            canvas.FillRect({left+8,top+8,4,4},{0,0,1,1});
        }
    }
};
std::array<int,3> Pixel(const std::vector<uint8_t>& pixels,uint32_t size,int x,int y)
{
    Check(x>=0 && y>=0 && unsigned(x)<size && unsigned(y)<size,"Probe outside capture");
    const size_t p=(size_t(y)*size+unsigned(x))*4;
    return {pixels[p],pixels[p+1],pixels[p+2]};
}
void Probe(const Fixture& f,const std::vector<uint8_t>& pixels)
{
    const auto at=[&](int x,int y) {return Pixel(pixels,f.size,f.X()+x,f.Y()+y);};
    const auto alpha=at(20,20);
    // Over black: red .5, blue .5, red .5 => (.625,0,.25).
    Check(alpha[0]>=157 && alpha[0]<=162 && alpha[1]<=2 && alpha[2]>=62 && alpha[2]<=66,
        "Nonadjacent image draws changed alpha order");
    const auto inside=at(66,18),outside=at(54,18),outsideViewport=at(46,18);
    Check(inside[0]<=2 && inside[1]>=253 && inside[2]<=2,"Clipped viewport interior is missing");
    Check(outside[0]<=2 && outside[1]<=2 && outside[2]<=2 && outsideViewport[1]<=2,
        "Viewport/clip boundary leaked pixels");
    const auto srgb=at(120,20),linear=at(150,20);
    Check(linear[0]>=186 && linear[0]<=189,"Linear image view lost its color space");
    Check(f.linearView ? std::abs(srgb[0]-linear[0])<=1 : srgb[0]>=126 && srgb[0]<=130,
        "Shared immutable image storage ignored changed color-space view");
    const auto replaced=at(188,20),retained=at(220,20);
    const auto& expected=f.replacement.GetPixels();
    for(unsigned c=0;c<3;++c)
        Check(std::abs(replaced[c]-int(expected[c]))<=2 && replaced[c]==retained[c],
            "New Image replacement or retained CPU pixel ownership changed output");
    for(unsigned i=0;i<4;++i)
    {
        const int left=i==3 ? int(f.size)-12 : f.X()+8+42*int(i),top=f.Y()+44;
        const int x=i==3 ? int(f.size)-1 : left+(i==1 ? 4 : 0),y=top+(i==1 ? 0 : 4);
        const auto skipped=Pixel(pixels,f.size,x,y),following=Pixel(pixels,f.size,left+10,top+10);
        Check(skipped[0]>=253 && skipped[1]<=2 && skipped[2]<=2,
            "Zero-size integer scissor drew green or lost the preceding red draw");
        Check(following[0]<=2 && following[1]<=2 && following[2]>=253,
            "Skipped subpixel draw lost the following normal blue draw");
    }
}
std::vector<uint8_t> Fresh(Fixture& fixture,bool hdr=false,bool validation=false,bool trace=false)
{
    if(trace)std::printf("Canvas Fresh context begin validation=%d\n",validation);
    Context fresh(fixture.size,fixture.size,true,false,validation);
    if(trace)std::puts("Canvas Fresh context ready");
    if(hdr) Check(fresh.renderer->SetHdrEnabled(true),"Fresh HDR renderer setup failed");
    SubmissionCounts counts;
    if(trace)std::puts("Canvas Fresh capture begin");
    auto pixels=Capture(fresh,fixture,counts);
    if(trace)std::puts("Canvas Fresh capture ready; context teardown next");
    return pixels;
}
void CheckPose(Context& context,Fixture& fixture,uint32_t tick,SubmissionCounts& counts,const char* label,bool validation)
{
    fixture.Pose(tick);
    const auto reference=Fresh(fixture,false,validation);
    const auto actual=Capture(context,fixture,counts);
    Probe(fixture,actual);
    Equivalent(label,reference,actual);
}
void Resize(Context& context,uint32_t size)
{
    RECT rectangle{0,0,LONG(size),LONG(size)};
    Check(AdjustWindowRect(&rectangle,WS_OVERLAPPEDWINDOW,FALSE)!=0,"Resize dimensions failed");
    Check(SetWindowPos(context.window.Get(),nullptr,0,0,rectangle.right-rectangle.left,
        rectangle.bottom-rectangle.top,SWP_NOMOVE|SWP_NOACTIVATE|SWP_NOZORDER)!=0,"Hidden window resize failed");
    Pump();
}
#ifdef DY_LIVE_PROFILING_ENABLED
dyf::Diagnostics::LiveFrameSummary LatestFrame(const Context& context)
{
    const auto snapshot=dyf::Diagnostics::ReadLiveSnapshot();
    for(const auto& source:snapshot->sources)if(source.source==context.renderer->GetLiveSourceId())return source.latest;
    throw std::runtime_error("Missing Canvas optimization frame");
}
void OptimizationBypasses(bool validation)
{
    Context context(Size,Size,true,false,validation);Fixture fixture;SubmissionCounts counts;
    unsigned pose=0;
    for(const auto* key:{"canvas.batching","canvas.imageCache","canvas.vertexBufferReuse"}){
        std::printf("Canvas optimization baseline begin key=%s\n",key);
        fixture.Pose(30+pose++*17);const auto reference=Capture(context,fixture,counts);(void)Capture(context,fixture,counts);
        const auto on=LatestFrame(context);
        std::printf("Canvas optimization OFF begin key=%s\n",key);
        const auto before=context.device->GetResourceAllocationCounters();const auto request=RequestBoolean(context,key,false);
        const auto actual=Capture(context,fixture,counts);const auto off=AppliedBoolean(context,request,false);const auto after=context.device->GetResourceAllocationCounters();Probe(fixture,actual);Equivalent(key,reference,actual);
        {const auto snapshot=dyf::Diagnostics::ReadLiveSnapshot();for(const auto& status:snapshot->controlStatus)if(status.request==request)
            Check(status.appliedFrame==off.key&&status.appliedRevision==off.appliedRevision,"Canvas request must match its captured source frame and revision");}
        std::printf("Canvas optimization steady OFF begin key=%s\n",key);
        const auto repeated=Capture(context,fixture,counts);const auto steadyOff=LatestFrame(context);const auto repeatedCounters=context.device->GetResourceAllocationCounters();
        Probe(fixture,repeated);Equivalent((std::string(key)+"-steady-off").c_str(),reference,repeated);
        if(std::string(key)=="canvas.batching")Check(off.recorded.draw>on.recorded.draw&&steadyOff.recorded.draw==off.recorded.draw,"Batching OFF must emit the same unmerged draws on every frame");
        if(std::string(key)=="canvas.imageCache"){
            // Two icons, white, red, blue, two gray color-space views and replacement.
            // Repeated replacement wrappers and all solid draws must still deduplicate.
            constexpr uint64_t images=8;
            Check(after.textures.created-before.textures.created==images&&repeatedCounters.textures.created-after.textures.created==images,"Image cache OFF must recreate each unique image once per frame");
            Check(steadyOff.recorded.uploadCalls==on.recorded.uploadCalls+images,"Image cache OFF must add exactly the unique texture uploads");
        }
        if(std::string(key)=="canvas.vertexBufferReuse"){
            Check(after.buffers.created-before.buffers.created==1&&repeatedCounters.buffers.created-after.buffers.created==1,"Vertex reuse OFF must recreate the Canvas buffer on every frame");
            Check(steadyOff.recorded.uploadCalls==on.recorded.uploadCalls&&steadyOff.recorded.uploadBytes==on.recorded.uploadBytes,"Vertex buffer recreation must preserve upload work");
        }
        std::printf("Canvas optimization restore begin key=%s\n",key);
        const auto restore=RequestBoolean(context,key,true);Equivalent((std::string(key)+"-restored").c_str(),reference,Capture(context,fixture,counts));(void)AppliedBoolean(context,restore,true);
        std::printf("native optimization key=%s on_draws=%llu off_draws=%llu on_upload_calls=%llu off_upload_calls=%llu steady_off_created=%llu\n",key,static_cast<unsigned long long>(on.recorded.draw),static_cast<unsigned long long>(off.recorded.draw),static_cast<unsigned long long>(on.recorded.uploadCalls),static_cast<unsigned long long>(steadyOff.recorded.uploadCalls),static_cast<unsigned long long>(steadyOff.recorded.objectsCreated));
    }
    std::puts("Canvas all OFF queued HUD begin");
    for(const auto* key:{"canvas.batching","canvas.imageCache","canvas.vertexBufferReuse"})Check(RequestBoolean(context,key,false)!=0,"Combined Canvas bypass request failed");
    Check(context.renderer->SetProfilerVisible(true),"Bypass profiler HUD enable failed");fixture.probeTop=128;
    const auto queuedBefore=context.device->GetResourceAllocationCounters();const auto submittedBefore=counts.submitted;
    for(unsigned i=0;i<6;++i){std::printf("Canvas all OFF queued HUD submit=%u\n",i);fixture.Pose(120+i);Submit(context,fixture,counts);}
    std::puts("Canvas all OFF HUD capture begin");
    fixture.Pose(126);const auto withHud=Capture(context,fixture,counts);
    std::puts("Canvas all OFF HUD capture ready; Fresh reference begin");
    const auto withoutHud=Fresh(fixture,false,validation,true);
    std::puts("Canvas Fresh context teardown ready; pixel comparison begin");
    Probe(fixture,withHud);const size_t belowHud=size_t(110)*fixture.size*4;
    Equivalent("all-Canvas-bypasses-overlay-and-HUD",std::vector<uint8_t>(withoutHud.begin()+belowHud,withoutHud.end()),std::vector<uint8_t>(withHud.begin()+belowHud,withHud.end()));
    Check(RegionChanged(withoutHud,withHud,fixture.size,0,106)>100,"Bypass fixture omitted its second Canvas pass");
    const auto queuedAfter=context.device->GetResourceAllocationCounters();
    Check(queuedAfter.buffers.created-queuedBefore.buffers.created>=2*(counts.submitted-submittedBefore),"Every queued overlay/HUD pass must own a distinct replacement vertex buffer");
    std::puts("Canvas all OFF final drain begin");
    Check(context.device->WaitIdle(),"Optimization Canvas native drain");context.renderer.reset();
    Check(context.device->WaitIdle(),"Optimization Canvas teardown drain");
    const auto final=context.device->GetResourceAllocationCounters();
    Check(!final.buffers.live&&!final.textures.live&&!final.pipelines.live,"Canvas bypass teardown retained tracked resources");
    std::puts("Canvas optimization control pixel contracts passed (live ON)");
}
#endif
void Run(bool allocations,bool validation)
{
    Context context(Size,Size,true,false,validation);
    Fixture fixture;
    SubmissionCounts counts;
    std::vector<uint8_t> first,last;
    for(unsigned pose=0;pose<4;++pose)
    {
        // Successful queued submissions advance both Scene and UI. These have
        // no readback/wait; the fourth frame captures the latest distinct pose.
        for(unsigned frame=0;frame<3;++frame)
        { fixture.Pose(1+pose*17+frame); Submit(context,fixture,counts); }
        const uint32_t tick=4+pose*17;
        CheckPose(context,fixture,tick,counts,"queued-moving-scene-and-overlay",validation);
        last=Capture(context,fixture,counts);
        if(first.empty()) first=last;
    }
    const auto uiChanged=RegionChanged(first,last,Size,0,Size/2);
    const auto sceneChanged=RegionChanged(first,last,Size,Size*2/3,Size);
    Check(uiChanged>100 && sceneChanged>100,"Scene and Canvas must both visibly move");
    std::printf("motion ui_region_changed=%llu scene_region_changed=%llu\n",
        static_cast<unsigned long long>(uiChanged),static_cast<unsigned long long>(sceneChanged));

    fixture.replacement=Solid(20,225,160);
    CheckPose(context,fixture,81,counts,"new-image-replacement",validation);
    fixture.linearView=true;
    CheckPose(context,fixture,82,counts,"same-pixel-storage-new-color-space",validation);
    fixture.Pose(83);
    const auto retainedColor=fixture.replacement.GetPixels();
    // The original CPU Image releases its storage after recording. Only the
    // Canvas draws (and any pending GPU cache owner) retain the old pixels.
    fixture.replacement=Solid(180,75,235);
    const auto retained=Capture(context,fixture,counts);
    const auto retainedPixel=Pixel(retained,fixture.size,fixture.X()+188,fixture.Y()+20);
    for(unsigned c=0;c<3;++c)
        Check(std::abs(retainedPixel[c]-int(retainedColor[c]))<=2,
            "Canvas lost immutable pixels after the caller replaced the original Image");
    Equivalent("caller-replacement-retains-recorded-image",Fresh(fixture,false,validation),retained);
    // The overlay and built-in HUD record two Canvas passes into the same list.
    // Compare below the compact HUD, whose text changes with measured timings.
    // A single reused vertex buffer overwritten by the HUD breaks this region.
    fixture.probeTop=128;
    Check(context.renderer->SetProfilerVisible(true),"Profiler HUD enable failed");
    for(unsigned i=0;i<6;++i) {fixture.Pose(84+i);Submit(context,fixture,counts);}
    fixture.Pose(90);
    const auto withHud=Capture(context,fixture,counts),withoutHud=Fresh(fixture,false,validation);
    Probe(fixture,withHud);
    const size_t belowHud=size_t(110)*fixture.size*4;
    Equivalent("overlay-and-hud-shared-command-list",
        std::vector<uint8_t>(withoutHud.begin()+belowHud,withoutHud.end()),
        std::vector<uint8_t>(withHud.begin()+belowHud,withHud.end()));
    Check(RegionChanged(withoutHud,withHud,fixture.size,0,106)>100,
        "HUD coexistence fixture did not render the second Canvas pass");
    {
        dyf::RHI::ThreadPool pool(2);
        auto settings=context.renderer->GetConfig();
        settings.enableParallelRenderGraph=true; settings.recordingThreadCount=pool.GetThreadCount();
        Check(context.renderer->SetConfig(settings),"Parallel Canvas/HUD graph configuration failed");
        for(unsigned i=0;i<3;++i) {fixture.Pose(91+i);Submit(context,fixture,counts);}
        fixture.Pose(94);
        const auto parallelHud=Capture(context,fixture,counts),reference=Fresh(fixture,false,validation);
        Probe(fixture,parallelHud);
        Equivalent("parallel-graph-overlay-and-hud",
            std::vector<uint8_t>(reference.begin()+belowHud,reference.end()),
            std::vector<uint8_t>(parallelHud.begin()+belowHud,parallelHud.end()));
        Check(RegionChanged(reference,parallelHud,fixture.size,0,106)>100,
            "Parallel graph omitted its profiler Canvas pass");
        settings.enableParallelRenderGraph=false; settings.recordingThreadCount=1;
        Check(context.renderer->SetConfig(settings),"Parallel Canvas/HUD graph reset failed");
        // Apply the reset in a successful Render while the caller-owned pool
        // is still alive; no stored pointer survives into later test frames.
        fixture.Pose(95); Submit(context,fixture,counts);
    }
    Check(context.renderer->SetProfilerVisible(false),"Profiler HUD disable failed");
    fixture.probeTop=8;
    // Reapplying GetShaders() is a no-op. HDR changes actually rebuild and swap
    // Renderer resources. Submit each setting before scheduling the next one.
    Check(context.renderer->SetHdrEnabled(true),"HDR settings rebuild enable failed");
    fixture.Pose(96);
    const auto hdr=Capture(context,fixture,counts);
    Probe(fixture,hdr); Equivalent("hdr-settings-rebuild-retains-moving-canvas",Fresh(fixture,true,validation),hdr);
    Check(context.renderer->SetHdrEnabled(false),"HDR settings rebuild disable failed");
    CheckPose(context,fixture,97,counts,"hdr-disabled-rebuild-retains-moving-canvas",validation);
    Resize(context,384);
    fixture.size=384; fixture.canvas=dyf::Canvas(384,384);
    // Swapchain resize may take one presentation to apply. Submit moving frames
    // before comparing dimensions and the fresh-renderer reference.
    for(unsigned i=0;i<4;++i) {fixture.Pose(98+i); Submit(context,fixture,counts);}
    CheckPose(context,fixture,102,counts,"resized-moving-canvas",validation);

    for(unsigned tick=103;tick<133;++tick) {fixture.Pose(tick); Submit(context,fixture,counts);}
    Check(context.device->WaitIdle(),"Canvas warmup drain failed");
    const auto before=context.device->GetResourceAllocationCounters();
    const auto uploadBefore=context.device->GetUploadMemoryStatistics();
    for(unsigned tick=133;tick<163;++tick) {fixture.Pose(tick); Submit(context,fixture,counts);}
    Check(context.device->WaitIdle(),"Canvas steady-state drain failed");
    const auto after=context.device->GetResourceAllocationCounters();
    const auto uploadAfter=context.device->GetUploadMemoryStatistics();
    std::printf("warm frames=30 buffers_created=%llu textures_created=%llu pipelines_created=%llu upload_requests=%llu upload_bytes=%llu\n",
        static_cast<unsigned long long>(after.buffers.created-before.buffers.created),
        static_cast<unsigned long long>(after.textures.created-before.textures.created),
        static_cast<unsigned long long>(after.pipelines.created-before.pipelines.created),
        static_cast<unsigned long long>(uploadAfter.requestCount-uploadBefore.requestCount),
        static_cast<unsigned long long>(uploadAfter.requestedBytes-uploadBefore.requestedBytes));
    Check(uploadAfter.requestedBytes>uploadBefore.requestedBytes,"Moving Scene/Canvas stopped uploading mutable data");
    const auto actual=Capture(context,fixture,counts);
    Probe(fixture,actual); Equivalent("post-warm-latest-pose",Fresh(fixture,false,validation),actual);
    if(allocations)
    {
        Check(after.buffers.created==before.buffers.created,"Warmed moving Canvas recreates vertex buffers");
        Check(after.textures.created==before.textures.created,"Warmed immutable Canvas images recreate textures");
        Check(after.pipelines.created==before.pipelines.created,"Warmed Canvas recreates pipelines");
    }
    Check(context.device->WaitIdle(),"Final Canvas drain failed");
    context.renderer.reset();
    Check(context.device->WaitIdle(),"Canvas teardown drain failed");
    const auto final=context.device->GetResourceAllocationCounters();
    Check(!final.buffers.live && !final.textures.live && !final.pipelines.live,
        "Canvas renderer teardown retained tracked resources");
    std::printf("submitted=%llu skipped_acquire_calls=%llu native Canvas contracts passed\n",
        static_cast<unsigned long long>(counts.submitted),static_cast<unsigned long long>(counts.skipped));
}
}
int main(int argc,char** argv)
{
    std::setvbuf(stdout,nullptr,_IONBF,0);
    try
    {
        bool allocations=true,validation=false,optimizationOnly=false;
        for(int i=1;i<argc;++i)
        {
            const std::string argument=argv[i];
            if(argument=="--pixels-only") allocations=false;
            else if(argument=="--validation") validation=true;
            else if(argument=="--optimization-only") optimizationOnly=true;
            else throw std::runtime_error("Unknown Canvas native argument: "+argument);
        }
        if(optimizationOnly){
#ifdef DY_LIVE_PROFILING_ENABLED
            OptimizationBypasses(validation);return 0;
#else
            throw std::runtime_error("--optimization-only requires DY_ENABLE_LIVE_PROFILING=ON");
#endif
        }
        Run(allocations,validation); return 0;
    }
    catch(const std::exception& error)
    { std::fprintf(stderr,"Canvas native contract failure: %s\n",error.what()); return 1; }
}
