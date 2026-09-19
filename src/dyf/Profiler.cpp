#include "dyf/Renderer.h"
#include "dyf/Canvas.h"
#include "dyf/RHI/IDevice.h"
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

    void Text(Canvas& canvas,const char* text,float x,float y)
    {
        for(const char* at=text;*at;++at,x+=12)
        {
            const auto columns=Glyph(*at);
            for(uint32_t column=0;column<5;++column)for(uint32_t row=0;row<7;++row)
                if(columns[column]&(1u<<row))canvas.FillRect({x+column*2.f,y+row*2.f,2,2});
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
    m_previous=now;m_history[m_cursor]=static_cast<float>(frame);m_cursor=(m_cursor+1)%m_history.size();
    m_count=std::min<uint32_t>(m_count+1,m_history.size());
    m_cpuMilliseconds=cpu;m_entities=entities;
}
Canvas Renderer::BuildProfilerOverlay(uint32_t width,uint32_t height,bool expanded) const
{
    const double frame=m_count ? m_history[(m_cursor+m_history.size()-1)%m_history.size()] : 0;
    Canvas canvas(width,height);
    canvas.FillRect({10,10,410,expanded?138.f:56.f},{.015f,.02f,.03f,.84f});
    char line[96];
    std::snprintf(line,sizeof(line),"FPS %.1f  FRAME %.2f MS",frame>0?1000/frame:0,frame);
    Text(canvas,line,18,18);
    std::snprintf(line,sizeof(line),"CPU %.2f MS  ENTITIES %u",m_cpuMilliseconds,m_entities);
    Text(canvas,line,18,38);
    if(expanded)
    {
        if(m_gpuMilliseconds[1]>=0)
        {
            std::snprintf(line,sizeof(line),"GPU MAIN %.2f SHADOW %.2f MS",m_gpuMilliseconds[1],std::max(0.0,m_gpuMilliseconds[0]));
            Text(canvas,line,18,58);
        }
        else Text(canvas,"GPU TIMING UNAVAILABLE",18,58);
        for(uint32_t i=1;i<m_count;++i)
        {
            const uint32_t a=(m_cursor+m_history.size()-m_count+i-1)%m_history.size();
            const uint32_t b=(a+1)%m_history.size();
            canvas.Line({18+(i-1)*3.2f,138-std::min(m_history[a],50.f)},
                {18+i*3.2f,138-std::min(m_history[b],50.f)},{.1f,.8f,1,1});
        }
    }
    return canvas;
}
}
