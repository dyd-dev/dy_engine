#include "RendererInternal.h"
#include "dyf/Canvas.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/Platform/Profiler.h"
#include <algorithm>
#include <cstdio>
#include <cmath>
namespace dyf
{
namespace
{
	[[nodiscard]] std::array<uint8_t, 5> Glyph(char character)
	{
		if(character >= 'a' && character <= 'z') character = static_cast<char>(character - 'a' + 'A');
		switch(character)
		{
		case 'A': return { 0x7e, 0x11, 0x11, 0x11, 0x7e };
		case 'B': return { 0x7f, 0x49, 0x49, 0x49, 0x36 };
		case 'C': return { 0x3e, 0x41, 0x41, 0x41, 0x22 };
		case 'D': return { 0x7f, 0x41, 0x41, 0x22, 0x1c };
		case 'E': return { 0x7f, 0x49, 0x49, 0x49, 0x41 };
		case 'F': return { 0x7f, 0x09, 0x09, 0x09, 0x01 };
		case 'G': return { 0x3e, 0x41, 0x49, 0x49, 0x7a };
		case 'H': return { 0x7f, 0x08, 0x08, 0x08, 0x7f };
		case 'I': return { 0x00, 0x41, 0x7f, 0x41, 0x00 };
		case 'J': return { 0x20, 0x40, 0x41, 0x3f, 0x01 };
		case 'K': return { 0x7f, 0x08, 0x14, 0x22, 0x41 };
		case 'L': return { 0x7f, 0x40, 0x40, 0x40, 0x40 };
		case 'M': return { 0x7f, 0x02, 0x0c, 0x02, 0x7f };
		case 'N': return { 0x7f, 0x04, 0x08, 0x10, 0x7f };
		case 'O': return { 0x3e, 0x41, 0x41, 0x41, 0x3e };
		case 'P': return { 0x7f, 0x09, 0x09, 0x09, 0x06 };
		case 'Q': return { 0x3e, 0x41, 0x51, 0x21, 0x5e };
		case 'R': return { 0x7f, 0x09, 0x19, 0x29, 0x46 };
		case 'S': return { 0x46, 0x49, 0x49, 0x49, 0x31 };
		case 'T': return { 0x01, 0x01, 0x7f, 0x01, 0x01 };
		case 'U': return { 0x3f, 0x40, 0x40, 0x40, 0x3f };
		case 'V': return { 0x1f, 0x20, 0x40, 0x20, 0x1f };
		case 'W': return { 0x3f, 0x40, 0x38, 0x40, 0x3f };
		case 'X': return { 0x63, 0x14, 0x08, 0x14, 0x63 };
		case 'Y': return { 0x07, 0x08, 0x70, 0x08, 0x07 };
		case 'Z': return { 0x61, 0x51, 0x49, 0x45, 0x43 };
		case '0': return { 0x3e, 0x51, 0x49, 0x45, 0x3e };
		case '1': return { 0x00, 0x42, 0x7f, 0x40, 0x00 };
		case '2': return { 0x42, 0x61, 0x51, 0x49, 0x46 };
		case '3': return { 0x21, 0x41, 0x45, 0x4b, 0x31 };
		case '4': return { 0x18, 0x14, 0x12, 0x7f, 0x10 };
		case '5': return { 0x27, 0x45, 0x45, 0x45, 0x39 };
		case '6': return { 0x3c, 0x4a, 0x49, 0x49, 0x30 };
		case '7': return { 0x01, 0x71, 0x09, 0x05, 0x03 };
		case '8': return { 0x36, 0x49, 0x49, 0x49, 0x36 };
		case '9': return { 0x06, 0x49, 0x49, 0x29, 0x1e };
		case '.': return { 0x00, 0x60, 0x60, 0x00, 0x00 };
		case ':': return { 0x00, 0x36, 0x36, 0x00, 0x00 };
		case '-': return { 0x08, 0x08, 0x08, 0x08, 0x08 };
		case '/': return { 0x20, 0x10, 0x08, 0x04, 0x02 };
		case '%': return { 0x23, 0x13, 0x08, 0x64, 0x62 };
		default: return { 0, 0, 0, 0, 0 };
		}
	}

    void Text(Canvas& canvas,const char* text,float x,float y,Math::float4 color={1,1,1,1},float scale=2)
    {
        for(const char* at=text;*at;++at,x+=6*scale)
        {
            const auto columns=Glyph(*at);
            for(uint32_t column=0;column<5;++column)for(uint32_t row=0;row<7;++row)
                if(columns[column]&(1u<<row))canvas.FillRect({x+column*scale,y+row*scale,scale,scale},color);
        }
    }
}
// 측정은 RHI timestamp query를 사용하고, 표시는 아래 Canvas 명령으로 작성한다.
RHI::TimestampQueryHandle Renderer::Impl::AcquireProfilerQuery(uint32_t count)
{
    const auto bits=device->GetTimestampValidBits();
    const auto period=device->GetTimestampPeriodNanoseconds();
    if(!device->Supports(RHI::Feature::TimestampQuery) || !bits || bits>64 || !std::isfinite(period) || period<=0 ||
        m_pending.size()>device->GetDesc().maxFramesInFlight+1) return nullptr;
    for(auto it=m_availableQueries.begin();it!=m_availableQueries.end();++it) if(it->second==count)
    {
        auto* query=it->first;
        m_availableQueries.erase(it);
        return query;
    }
    return device->CreateTimestampQuery({count});
}
void Renderer::Impl::PollGpuSamples()
{
    while(!m_pending.empty() && m_pending.front().completion && device->IsComplete(m_pending.front().completion))
    {
        struct Completed {
            RHI::IDevice& device;
            GpuSample sample;
            ~Completed() {device.DestroyTimestampQuery(sample.query);device.DestroyTimestampQuery(sample.start);}
        } completed{*device,std::move(m_pending.front())};
        m_pending.pop_front();
        auto& sample=completed.sample;
        const auto count=2+2*static_cast<uint32_t>(sample.passes.size());
        std::vector<uint64_t> ticks(count);
        uint64_t start=0;
        const bool valid=device->ReadTimestamps(sample.query,0,count,ticks.data());
        const bool hasStart=sample.start && device->ReadTimestamps(sample.start,0,1,&start);
        const auto bits=device->GetTimestampValidBits();
        const auto mask=bits==64 ? UINT64_MAX : bits>0 && bits<64 ? (uint64_t{1}<<bits)-1 : 0;
        const auto period=device->GetTimestampPeriodNanoseconds();
        if(valid && mask && std::isfinite(period) && period>0)
        {
            const auto milliseconds=[&](uint64_t begin,uint64_t end){return ((end-begin)&mask)*period/1e6;};
            std::vector<Platform::ProfilerPassTiming> passes;
            passes.reserve(sample.passes.size());
            for(size_t i=0;i<sample.passes.size();++i)
                passes.push_back({sample.passes[i],milliseconds(ticks[2+2*i],ticks[3+2*i]),1});
            m_profilerSampler.AddGpuSample(hasStart ? milliseconds(start,ticks[1]) : -1.0,passes);
        }
        const auto recycle=[&](RHI::TimestampQueryHandle& query,uint32_t slots) {
            if(!query) return;
            if(m_availableQueries.size()<16) m_availableQueries.push_back({query,slots});
            else device->DestroyTimestampQuery(query);
            query=nullptr;
        };
        recycle(sample.query,count);recycle(sample.start,1);
    }
}
void Renderer::Impl::RecordProfilerFrame(Platform::ProfilerTimingSample sample,uint32_t entities)
{
    const auto now=std::chrono::steady_clock::now();
    sample.frameMilliseconds=m_previous.time_since_epoch().count()==0?0:std::chrono::duration<double,std::milli>(now-m_previous).count();
    m_previous=now;
    DY_PROFILE_RAW_VALUE("Frame.Raw.ms",sample.frameMilliseconds);
    DY_PROFILE_RAW_VALUE("CPU.Render.Raw.ms",sample.cpuRenderMilliseconds);
    m_profilerStatistics=lastStatistics;
    if(!m_profilerSampler.AddSample(sample,m_profilerSnapshot)) return;
    m_profilerSnapshot.gpuTimingSupported=device->Supports(RHI::Feature::TimestampQuery) &&
        device->GetTimestampValidBits()>0 && device->GetTimestampPeriodNanoseconds()>0;
    m_history[m_cursor]=static_cast<float>(m_profilerSnapshot.frameAverageMilliseconds);
    m_cpuHistory[m_cursor]=static_cast<float>(m_profilerSnapshot.cpuRenderAverageMilliseconds);
    m_gpuHistory[m_cursor]=static_cast<float>(m_profilerSnapshot.gpuFrameAverageMilliseconds);
    m_gpuHistoryValid[m_cursor]=m_profilerSnapshot.hasGpuFrame ? 1 : 0;
    m_cursor=(m_cursor+1)%m_history.size();
    m_count=std::min<uint32_t>(m_count+1,m_history.size());
    float scale=33.33f;
    for(uint32_t i=0;i<m_count;++i)
        scale=std::max(scale,std::max({m_history[i],m_cpuHistory[i],m_gpuHistoryValid[i]?m_gpuHistory[i]:0.f})*1.15f);
    m_graphScaleMilliseconds=std::max(scale,m_graphScaleMilliseconds*0.97f);
    m_entities=entities;
    DY_PROFILE_GPU_MILLISECONDS("Frame.Average.ms",m_profilerSnapshot.frameAverageMilliseconds);
    DY_PROFILE_GPU_MILLISECONDS("Frame.Maximum.ms",m_profilerSnapshot.frameMaximumMilliseconds);
    DY_PROFILE_GPU_MILLISECONDS("CPU.Render.Average.ms",m_profilerSnapshot.cpuRenderAverageMilliseconds);
    if(m_profilerSnapshot.hasGpuFrame) DY_PROFILE_GPU_MILLISECONDS("GPU.Frame.Average.ms",m_profilerSnapshot.gpuFrameAverageMilliseconds);
    if(m_profilerSnapshot.hasGpuMain) DY_PROFILE_GPU_MILLISECONDS("GPU.MainForward.Average.ms",m_profilerSnapshot.gpuMainAverageMilliseconds);
    if(m_profilerSnapshot.hasGpuShadow) DY_PROFILE_GPU_MILLISECONDS("GPU.Shadow.Average.ms",m_profilerSnapshot.gpuShadowAverageMilliseconds);
    m_profilerResourceSnapshot=device->GetResourceAllocationCounters();
    const auto& counters=m_profilerResourceSnapshot;
    DY_PROFILE_RESOURCE_COUNT("GPU.Resources.Buffers.Live",counters.buffers.live);
    DY_PROFILE_RESOURCE_COUNT("GPU.Resources.Textures.Live",counters.textures.live);
    DY_PROFILE_RESOURCE_COUNT("GPU.Resources.Pipelines.Live",counters.pipelines.live);
}
Canvas Renderer::Impl::BuildProfilerOverlay(uint32_t width,uint32_t height,bool expanded) const
{
    Canvas canvas(width,height);
    if(width<160 || height<100) return canvas;
    const float panelWidth=std::max(200.f,std::min(920.f,float(width)-20));
    const uint32_t columns=panelWidth>=780 ? 2 : 1;
    const uint32_t rows=(static_cast<uint32_t>(m_profilerSnapshot.gpuPasses.size())+columns-1)/columns;
    const float graphTop=296.f+rows*18;
    const float panelHeight=expanded ? std::min(float(height)-20,graphTop+134) : 156.f;
    canvas.FillRect({10,10,expanded?panelWidth:std::min(panelWidth,650.f),panelHeight},{.015f,.02f,.03f,.84f});
    char line[128];
    std::snprintf(line,sizeof(line),"FPS %.1f  FRAME %.2f MS  MAX %.2f",m_profilerSnapshot.fps,
        m_profilerSnapshot.frameAverageMilliseconds,m_profilerSnapshot.frameMaximumMilliseconds);
    Text(canvas,line,18,18);
    if(m_profilerSnapshot.hasGpuFrame)
        std::snprintf(line,sizeof(line),"CPU WORK %.2f MS  GPU SPAN %.2f MS",m_profilerSnapshot.cpuRenderAverageMilliseconds,m_profilerSnapshot.gpuFrameAverageMilliseconds);
    else std::snprintf(line,sizeof(line),"CPU WORK %.2f MS  GPU %s",m_profilerSnapshot.cpuRenderAverageMilliseconds,
        device->Supports(RHI::Feature::TimestampQuery) ? "PENDING" : "UNSUPPORTED");
    Text(canvas,line,18,38);
    std::snprintf(line,sizeof(line),"MAIN DRAWS %u  INST %u  TRI %llu",m_profilerStatistics.mainDrawCount,
        m_profilerStatistics.mainInstanceCount,static_cast<unsigned long long>(m_profilerStatistics.mainTriangleCount));
    if(m_profilerStatistics.gpuVisibility)
        std::snprintf(line,sizeof(line),"GPU INDIRECT BATCHES %u  VISIBLE INST/TRI N/A",m_profilerStatistics.indirectBatchCount);
    Text(canvas,line,18,58);
    std::snprintf(line,sizeof(line),"CPU CULLED %u  SHADOW L0 %u DRAWS %s",m_profilerStatistics.culledEntityCount,
        m_profilerStatistics.shadowDrawCount,m_profilerStatistics.reusedShadowAtlas ? "CACHED" : "");
    Text(canvas,line,18,78);
    const auto& lod=m_profilerStatistics.lodInstanceCounts;
    std::snprintf(line,sizeof(line),"LOD %.2fPX  L0 %u  L1 %u  L2 %u  L3 %u",config.lodPixelError,lod[0],lod[1],lod[2],lod[3]);
    if(m_profilerStatistics.gpuVisibility)
        std::snprintf(line,sizeof(line),"GPU LOD %.2fPX  COUNTS N/A  COLOR VIEW AVAILABLE",config.lodPixelError);
    Text(canvas,line,18,98,{1,1,1,1},1.6f);
    std::snprintf(line,sizeof(line),"MAIN TRI ORIGINAL %llu > SELECTED %llu",
        static_cast<unsigned long long>(m_profilerStatistics.mainOriginalTriangleCount),
        static_cast<unsigned long long>(m_profilerStatistics.mainTriangleCount));
    if(m_profilerStatistics.gpuVisibility)
        std::snprintf(line,sizeof(line),"GPU COUNTS REMAIN ON DEVICE; COMPARE PASS TIMES");
    Text(canvas,line,18,118,{1,1,1,1},1.6f);
    std::snprintf(line,sizeof(line),"RENDER %u X %u / OUTPUT %u X %u",m_profilerStatistics.renderWidth,
        m_profilerStatistics.renderHeight,m_profilerStatistics.outputWidth,m_profilerStatistics.outputHeight);
    // 전체 구간에는 명령 준비 대기가 섞일 수 있으므로 측정된 패스 합계도 함께 표시한다.
    if(!m_profilerSnapshot.gpuPasses.empty())
    {
        double passMilliseconds=0;
        for(const auto& pass:m_profilerSnapshot.gpuPasses)passMilliseconds+=pass.averageMilliseconds;
        std::snprintf(line,sizeof(line),"RENDER %u X %u / OUTPUT %u X %u / GPU PASS %.2f MS",
            m_profilerStatistics.renderWidth,m_profilerStatistics.renderHeight,
            m_profilerStatistics.outputWidth,m_profilerStatistics.outputHeight,passMilliseconds);
    }
    Text(canvas,line,18,138,{1,1,1,1},1.6f);
    const auto& counters=m_profilerResourceSnapshot;
    if(!expanded) return canvas;
    std::snprintf(line,sizeof(line),"CPU ACQUIRE %.2f  PREPARE %.2f  RECORD %.2f MS",m_profilerSnapshot.cpuAcquireAverageMilliseconds,
        m_profilerSnapshot.cpuPrepareAverageMilliseconds,m_profilerSnapshot.cpuRecordAverageMilliseconds);
    Text(canvas,line,18,160,{1,1,1,1},1.6f);
    std::snprintf(line,sizeof(line),"CPU SUBMIT %.2f  PRESENT %.2f  READBACK %.2f MS",m_profilerSnapshot.cpuSubmitAverageMilliseconds,
        m_profilerSnapshot.cpuPresentAverageMilliseconds,m_profilerSnapshot.cpuReadbackAverageMilliseconds);
    Text(canvas,line,18,180,{1,1,1,1},1.6f);
    std::snprintf(line,sizeof(line),"ENTITIES %u  SHADOW INST %u  TRI %llu",m_entities,m_profilerStatistics.shadowInstanceCount,
        static_cast<unsigned long long>(m_profilerStatistics.shadowTriangleCount));
    Text(canvas,line,18,200,{1,1,1,1},1.6f);
    std::snprintf(line,sizeof(line),"LIVE BUF %llu TEX %llu PSO %llu  GPU SAMPLES %u",
        static_cast<unsigned long long>(counters.buffers.live),static_cast<unsigned long long>(counters.textures.live),
        static_cast<unsigned long long>(counters.pipelines.live),m_profilerSnapshot.gpuFrameSampleCount);
    Text(canvas,line,18,220,{1,1,1,1},1.6f);
    std::snprintf(line,sizeof(line),"GPU CULL %s  HIZ %s  CLUSTER %s  SURFACE %s",
        m_profilerStatistics.gpuVisibility ? "ON" : "OFF",m_profilerStatistics.gpuOcclusion ? "ON" : "OFF",
        m_profilerStatistics.clusteredLighting ? "ON" : "OFF",m_profilerStatistics.surfacePrepass ? "ON" : "OFF");
    Text(canvas,line,18,240,{1,1,1,1},1.6f);
    std::snprintf(line,sizeof(line),"SHADOW VIEWS %u  TILE %u PX  DISTANCE %.1f",
        m_profilerStatistics.shadowViewCount,m_profilerStatistics.shadowViewResolution,m_profilerStatistics.directionalShadowDistance);
    Text(canvas,line,18,260,{1,1,1,1},1.6f);
    const Math::float4 cyan{.1f,.8f,1,1},green{.3f,1,.4f,1},orange{1,.65f,.15f,1},red{1,.3f,.3f,1};
    const float columnWidth=(panelWidth-16)/columns;
    for(uint32_t i=0;i<m_profilerSnapshot.gpuPasses.size();++i)
    {
        const auto& pass=m_profilerSnapshot.gpuPasses[i];
        const float x=18+(i/rows)*columnWidth,y=284+(i%rows)*18;
        if(y+14>height-28) break;
        const auto fraction=std::clamp(pass.averageMilliseconds/std::max(m_profilerSnapshot.gpuFrameAverageMilliseconds,1.0),0.0,1.0);
        canvas.FillRect({x,y,columnWidth*static_cast<float>(fraction),14},{.15f,.28f,.4f,.7f});
        std::snprintf(line,sizeof(line),"%.28s",pass.name.c_str());
        Text(canvas,line,x,y,{1,1,1,1},1.5f);
        std::snprintf(line,sizeof(line),"%.2f MS",pass.averageMilliseconds);
        Text(canvas,line,x+columnWidth-84,y,orange,1.5f);
    }
    if(graphTop+124>height-20) return canvas;
    const auto y=[&](float milliseconds){return graphTop+100-100.f*std::clamp(milliseconds/m_graphScaleMilliseconds,0.f,1.f);};
    const float graphWidth=std::min(500.f,panelWidth-24);
    canvas.Line({18,y(16.67f)},{18+graphWidth,y(16.67f)},red,1.2f);
    for(uint32_t i=1;i<m_count;++i)
    {
        const uint32_t a=(m_cursor+m_history.size()-m_count+i-1)%m_history.size();
        const uint32_t b=(a+1)%m_history.size();
        const float xa=18+(i-1)*graphWidth/119,xb=18+i*graphWidth/119;
        // Canvas::Line emits triangles, including on D3D12; no unsupported line topology is used.
        canvas.Line({xa,y(m_history[a])},{xb,y(m_history[b])},cyan,1.5f);
        canvas.Line({xa,y(m_cpuHistory[a])},{xb,y(m_cpuHistory[b])},green,1.5f);
        if(m_gpuHistoryValid[a] && m_gpuHistoryValid[b])
            canvas.Line({xa,y(m_gpuHistory[a])},{xb,y(m_gpuHistory[b])},orange,1.5f);
    }
    Text(canvas,"FRAME",18,graphTop+110,cyan);Text(canvas,"CPU",108,graphTop+110,green);
    Text(canvas,"GPU SPAN",174,graphTop+110,orange);Text(canvas,"16.67 MS",300,graphTop+110,red);
    return canvas;
}
}
