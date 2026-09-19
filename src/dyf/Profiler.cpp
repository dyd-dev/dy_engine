#include "dyf/Renderer.h"
#include "dyf/Canvas.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/Platform/Profiler.h"
#include <algorithm>
#include <cstdio>
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

    void Text(Canvas& canvas,const char* text,float x,float y,Math::float4 color={1,1,1,1})
    {
        for(const char* at=text;*at;++at,x+=12)
        {
            const auto columns=Glyph(*at);
            for(uint32_t column=0;column<5;++column)for(uint32_t row=0;row<7;++row)
                if(columns[column]&(1u<<row))canvas.FillRect({x+column*2.f,y+row*2.f,2,2},color);
        }
    }
}
// 측정은 RHI timestamp query를 사용하고, 표시는 아래 Canvas 명령으로 작성한다.
RHI::TimestampQueryHandle Renderer::BeginGpuSample(uint32_t slot)
{
    while(!m_pending.empty() && m_pending.front().completion && device->IsComplete(m_pending.front().completion))
    {
        const auto sample=m_pending.front();
        uint64_t ticks[2];
        if(device->ReadTimestamps(sample.query,0,2,ticks))
        {
            const auto bits=device->GetTimestampValidBits();
            const auto mask=bits==64 ? UINT64_MAX : (uint64_t{1}<<bits)-1;
            m_gpuMilliseconds[sample.slot]=((ticks[1]-ticks[0])&mask)*device->GetTimestampPeriodNanoseconds()/1e6;
        }
        device->DestroyTimestampQuery(sample.query);
        m_pending.pop_front();
    }
    if(!device->Supports(RHI::Feature::TimestampQuery))return nullptr;
    auto* query=device->CreateTimestampQuery({2});
    if(query)m_pending.push_back({query,{},slot});
    return query;
}
void Renderer::SubmittedGpuSample(RHI::TimestampQueryHandle query,RHI::FenceHandle completion)
{
    for(auto it=m_pending.begin();it!=m_pending.end();++it)if(it->query==query)
    {
        if(completion)it->completion=completion;
        else {device->DestroyTimestampQuery(query);m_pending.erase(it);}
        return;
    }
}
void Renderer::RecordProfilerFrame(double cpu,uint32_t entities)
{
    const auto now=std::chrono::steady_clock::now();
    const double frame=m_previous.time_since_epoch().count()==0?0:std::chrono::duration<double,std::milli>(now-m_previous).count();
    m_previous=now;
    // BeginGpuSample drains completions for both slots. Consume each published
    // GPU result once; frames without a fresh completion must not repeat it.
    const auto gpuMilliseconds=m_gpuMilliseconds;
    m_gpuMilliseconds={-1,-1};
    DY_PROFILE_RAW_VALUE("Frame.Raw.ms",frame);
    DY_PROFILE_RAW_VALUE("CPU.Render.Raw.ms",cpu);
    if(gpuMilliseconds[1]>=0) DY_PROFILE_RAW_VALUE("GPU.MainForward.Raw.ms",gpuMilliseconds[1]);
    if(gpuMilliseconds[0]>=0) DY_PROFILE_RAW_VALUE("GPU.Shadow.Raw.ms",gpuMilliseconds[0]);
    if(!m_profilerSampler.AddSample({frame,cpu,gpuMilliseconds[1],gpuMilliseconds[0],
        gpuMilliseconds[1]>=0,gpuMilliseconds[0]>=0},m_profilerSnapshot)) return;
    m_history[m_cursor]=static_cast<float>(m_profilerSnapshot.frameAverageMilliseconds);
    m_cpuHistory[m_cursor]=static_cast<float>(m_profilerSnapshot.cpuRenderAverageMilliseconds);
    m_gpuHistory[m_cursor]=static_cast<float>(m_profilerSnapshot.gpuMainAverageMilliseconds);
    m_gpuHistoryValid[m_cursor]=m_profilerSnapshot.hasGpuMain ? 1 : 0;
    m_cursor=(m_cursor+1)%m_history.size();
    m_count=std::min<uint32_t>(m_count+1,m_history.size());
    float scale=33.33f;
    for(uint32_t i=0;i<m_count;++i)
        scale=std::max(scale,std::max({m_history[i],m_cpuHistory[i],m_gpuHistoryValid[i]?m_gpuHistory[i]:0.f})*1.15f);
    m_graphScaleMilliseconds=std::max(scale,m_graphScaleMilliseconds*0.97f);
    m_cpuMilliseconds=m_profilerSnapshot.cpuRenderAverageMilliseconds;m_entities=entities;
    DY_PROFILE_GPU_MILLISECONDS("Frame.Average.ms",m_profilerSnapshot.frameAverageMilliseconds);
    DY_PROFILE_GPU_MILLISECONDS("Frame.Maximum.ms",m_profilerSnapshot.frameMaximumMilliseconds);
    DY_PROFILE_GPU_MILLISECONDS("CPU.Render.Average.ms",m_profilerSnapshot.cpuRenderAverageMilliseconds);
    if(m_profilerSnapshot.hasGpuMain) DY_PROFILE_GPU_MILLISECONDS("GPU.MainForward.Average.ms",m_profilerSnapshot.gpuMainAverageMilliseconds);
    if(m_profilerSnapshot.hasGpuShadow) DY_PROFILE_GPU_MILLISECONDS("GPU.Shadow.Average.ms",m_profilerSnapshot.gpuShadowAverageMilliseconds);
    const auto counters=device->GetResourceAllocationCounters();
    DY_PROFILE_RESOURCE_COUNT("GPU.Resources.Buffers.Live",counters.buffers.live);
    DY_PROFILE_RESOURCE_COUNT("GPU.Resources.Textures.Live",counters.textures.live);
    DY_PROFILE_RESOURCE_COUNT("GPU.Resources.Pipelines.Live",counters.pipelines.live);
}
Canvas Renderer::BuildProfilerOverlay(uint32_t width,uint32_t height,bool expanded) const
{
    Canvas canvas(width,height);
    canvas.FillRect({10,10,560,expanded?314.f:96.f},{.015f,.02f,.03f,.84f});
    char line[128];
    std::snprintf(line,sizeof(line),"FPS AVG %.1f  FRAME AVG %.2f MS",m_profilerSnapshot.fps,m_profilerSnapshot.frameAverageMilliseconds);
    Text(canvas,line,18,18);
    std::snprintf(line,sizeof(line),"CPU AVG %.2f MS  ENTITIES %u  F11",m_cpuMilliseconds,m_entities);
    Text(canvas,line,18,38);
    if(m_profilerSnapshot.hasGpuMain)
    {
        if(m_profilerSnapshot.hasGpuShadow)
            std::snprintf(line,sizeof(line),"GPU MAIN %.2f SHADOW %.2f MS",m_profilerSnapshot.gpuMainAverageMilliseconds,m_profilerSnapshot.gpuShadowAverageMilliseconds);
        else std::snprintf(line,sizeof(line),"GPU MAIN %.2f MS  SHADOW PENDING",m_profilerSnapshot.gpuMainAverageMilliseconds);
        Text(canvas,line,18,58);
    }
    else Text(canvas,device->Supports(RHI::Feature::TimestampQuery) ? "GPU TIMING PENDING" : "GPU TIMING UNSUPPORTED",18,58);
    const auto counters=device->GetResourceAllocationCounters();
    if(!expanded)
    {
        std::snprintf(line,sizeof(line),"LIVE BUF %llu TEX %llu PSO %llu",
            static_cast<unsigned long long>(counters.buffers.live),static_cast<unsigned long long>(counters.textures.live),
            static_cast<unsigned long long>(counters.pipelines.live));
        Text(canvas,line,18,78);
        return canvas;
    }
    std::snprintf(line,sizeof(line),"FRAME MAX %.2f MS  WINDOW 200 MS",m_profilerSnapshot.frameMaximumMilliseconds);
    Text(canvas,line,18,78);
    const auto counterLine=[&](const char* name,const RHI::ResourceCounter& counter,float y) {
        std::snprintf(line,sizeof(line),"%s %llu LIVE %llu NEW %llu FREED",name,
            static_cast<unsigned long long>(counter.live),static_cast<unsigned long long>(counter.created),
            static_cast<unsigned long long>(counter.destroyed));
        Text(canvas,line,18,y);
    };
    counterLine("BUF",counters.buffers,98);
    counterLine("TEX",counters.textures,118);
    counterLine("PSO",counters.pipelines,138);
    std::snprintf(line,sizeof(line),"HISTORY 24 SEC  SCALE %.1f MS",m_graphScaleMilliseconds);
    Text(canvas,line,18,158);
    const Math::float4 cyan{.1f,.8f,1,1},green{.3f,1,.4f,1},orange{1,.65f,.15f,1},red{1,.3f,.3f,1};
    const auto y=[&](float milliseconds){return 284.f-100.f*std::clamp(milliseconds/m_graphScaleMilliseconds,0.f,1.f);};
    canvas.Line({18,y(16.67f)},{518,y(16.67f)},red,1.2f);
    for(uint32_t i=1;i<m_count;++i)
    {
        const uint32_t a=(m_cursor+m_history.size()-m_count+i-1)%m_history.size();
        const uint32_t b=(a+1)%m_history.size();
        const float xa=18+(i-1)*500.f/119,xb=18+i*500.f/119;
        // Canvas::Line emits triangles, including on D3D12; no unsupported line topology is used.
        canvas.Line({xa,y(m_history[a])},{xb,y(m_history[b])},cyan,1.5f);
        canvas.Line({xa,y(m_cpuHistory[a])},{xb,y(m_cpuHistory[b])},green,1.5f);
        if(m_gpuHistoryValid[a] && m_gpuHistoryValid[b])
            canvas.Line({xa,y(m_gpuHistory[a])},{xb,y(m_gpuHistory[b])},orange,1.5f);
    }
    Text(canvas,"FRAME",18,298,cyan);Text(canvas,"CPU",108,298,green);
    Text(canvas,"GPU",174,298,orange);Text(canvas,"16.67 MS",240,298,red);
    return canvas;
}
}
