#include "RendererInternal.h"
#include "dyf/Canvas.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/Platform/Profiler.h"
#include "../Platform/Windows/WindowBackend.h"
#include <algorithm>
#include <cstdio>
#include <cmath>
namespace dyf
{
namespace
{
	// 파일 폰트 없이 숫자·영문·일부 기호를 표시하는 5열×7행 비트맵이다. 지원하지 않는 문자는 빈 칸이다.
	[[nodiscard]] std::array<uint8_t, 5> Glyph(char character)
	{
		if(character >= 'a' && character <= 'z') character = static_cast<char>(character - 'a' + 'A'); // 소문자는 대응하는 대문자 비트맵을 사용한다.
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

	// 글자 열의 비트를 작은 FillRect로 풀고 여섯 픽셀 폭씩 진행한다. 이 HUD 텍스트 함수는 줄바꿈·한글 처리를 하지 않는다.
	void Text(Canvas& canvas,const char* text,float x,float y,Math::float4 color={1,1,1,1},float scale=2)
	{
		for(const char* at=text;*at;++at,x+=6*scale)
		{
			const auto columns=Glyph(*at);
			for(uint32_t column=0;column<5;++column)for(uint32_t row=0;row<7;++row)
				if(columns[column]&(1u<<row))canvas.FillRect({x+column*scale,y+row*scale,scale,scale},color); // 해당 비트가 켜진 위치에만 작은 사각형을 기록한다.
		}
	}
}
// 공통 Profiler가 측정·집계하며, 여기서는 마지막 렌더 통계를 보관하고 집계기가 받아들인 표본만 HUD용 원형 이력에 추가한다.
void Renderer::Impl::RecordProfilerFrame(Platform::ProfilerTimingSample sample,uint32_t entities)
{
	m_profilerStatistics=lastStatistics;
	if(!m_profiler.RecordFrame(sample))return; // 집계기가 받아들이지 않은 표본은 표시 이력에도 추가하지 않는다.
	const auto& snapshot=m_profiler.GetSnapshot();
	m_history[m_cursor]=static_cast<float>(snapshot.frameAverageMilliseconds);
	m_cpuHistory[m_cursor]=static_cast<float>(snapshot.cpuRenderAverageMilliseconds);
	m_gpuHistory[m_cursor]=static_cast<float>(snapshot.gpuFrameAverageMilliseconds);
	// GPU 시간 숫자와 유효 여부를 따로 저장해 없는 측정을 실제 0ms로 그래프에 연결하지 않는다.
	m_gpuHistoryValid[m_cursor]=snapshot.hasGpuFrame ? 1 : 0;
	m_cursor=(m_cursor+1)%m_history.size();
	m_count=std::min<uint32_t>(m_count+1,m_history.size());
	// 저장한 프레임·CPU·유효 GPU 평균값 중 최댓값에 15% 여유를 두고 그래프 높이를 정한다. 최소 높이는 33.33ms이며, 높이를 줄일 때는 직전 높이의 97%보다 작아지지 않게 한다.
	float scale=33.33f;
	for(uint32_t i=0;i<m_count;++i)
		scale=std::max(scale,std::max({m_history[i],m_cpuHistory[i],m_gpuHistoryValid[i]?m_gpuHistory[i]:0.f})*1.15f);
	m_graphScaleMilliseconds=std::max(scale,m_graphScaleMilliseconds*0.97f);
	m_entities=entities;
}
// 크기에 맞춰 요약/확장 HUD를 구성해 반환한다. 실제 GPU 출력은 일반 Canvas 경로가 수행한다.
Canvas Renderer::Impl::BuildProfilerOverlay(uint32_t width,uint32_t height,bool expanded)
{
	Canvas canvas(width,height);
	if(width<160 || height<100) return canvas; // HUD를 놓기 어려운 크기이면 빈 Canvas를 반환한다.
	const float panelWidth=std::max(200.f,std::min(920.f,float(width)-20));
    expanded=profilerHudExpanded.value_or(expanded);
    const auto buttonFor=[&](bool details) {
        return Rectangle{10+(details?panelWidth:std::min(panelWidth,650.f))-100,10,100,28};
    };
    const auto pressedButton=buttonFor(expanded);
    for(const auto* window:Platform::RegisteredWindows())
    {
        if(window->GetHandle()!=windowHandle || !window->HasFocus() ||
            !window->GetInput().WasPressed(Platform::MouseButton::Left)) continue;
        const auto size=window->GetSize();
        if(size.width<=0 || size.height<=0) continue;
        const auto pointer=window->GetInput().GetCursorPosition();
        const float x=float(pointer.x)*width/size.width, y=float(pointer.y)*height/size.height;
        if(x>=pressedButton.x && y>=pressedButton.y && x<pressedButton.x+pressedButton.width &&
            y<pressedButton.y+pressedButton.height) profilerHudExpanded=expanded=!expanded;
        break;
    }
	// 패널이 넓으면 GPU 패스 목록을 두 열로 놓고 좁으면 한 열로 놓는다.
	const uint32_t columns=panelWidth>=780 ? 2 : 1;
	const uint32_t rows=(static_cast<uint32_t>(m_profiler.GetSnapshot().gpuPasses.size())+columns-1)/columns;
	const float graphTop=296.f+rows*18;
	const float panelHeight=expanded ? std::min(float(height)-20,graphTop+134) : 156.f;
	canvas.FillRect({10,10,expanded?panelWidth:std::min(panelWidth,650.f),panelHeight},{.015f,.02f,.03f,.84f});
	char line[128];
	// FPS와 프레임 평균/최댓값은 집계 스냅샷에서 가져온다. 현재 Draw 호출 하나의 시간과 혼동하지 않는다.
	std::snprintf(line,sizeof(line),"FPS %.1f  FRAME %.2f MS  MAX %.2f",m_profiler.GetSnapshot().fps,
		m_profiler.GetSnapshot().frameAverageMilliseconds,m_profiler.GetSnapshot().frameMaximumMilliseconds);
    const auto modeButton=buttonFor(expanded);
    canvas.SetClipRect({18,18,std::max(0.f,modeButton.x-26),16});
	Text(canvas,line,18,18);
    canvas.ResetClipRect();
    canvas.FillRect(modeButton,{.08f,.15f,.20f,1});
    Text(canvas,expanded?"DEFAULT":"DETAILS",modeButton.x+8,modeButton.y+7,{.6f,.85f,1,1},2);
	if(m_profiler.GetSnapshot().hasGpuFrame) // 유효한 GPU 표본이 있을 때만 시간을 숫자로 표시한다.
		std::snprintf(line,sizeof(line),"CPU WORK %.2f MS  GPU SPAN %.2f MS",m_profiler.GetSnapshot().cpuRenderAverageMilliseconds,m_profiler.GetSnapshot().gpuFrameAverageMilliseconds);
	else std::snprintf(line,sizeof(line),"CPU WORK %.2f MS  GPU %s",m_profiler.GetSnapshot().cpuRenderAverageMilliseconds,
		device->Supports(RHI::Feature::TimestampQuery) ? "PENDING" : "UNSUPPORTED");
	Text(canvas,line,18,38);
	std::snprintf(line,sizeof(line),"MAIN DRAWS %u  INST %u  TRI %llu",m_profilerStatistics.mainDrawCount,
		m_profilerStatistics.mainInstanceCount,static_cast<unsigned long long>(m_profilerStatistics.mainTriangleCount));
	if(m_profilerStatistics.gpuVisibility) // GPU 간접 경로의 실제 가시 개수는 읽어오지 않으므로 N/A 문구로 바꾼다.
		std::snprintf(line,sizeof(line),"GPU INDIRECT BATCHES %u  VISIBLE INST/TRI N/A",m_profilerStatistics.indirectBatchCount);
	Text(canvas,line,18,58);
	std::snprintf(line,sizeof(line),"CPU CULLED %u  SHADOW L0 %u DRAWS %s",m_profilerStatistics.culledEntityCount,
		m_profilerStatistics.shadowDrawCount,m_profilerStatistics.reusedShadowAtlas ? "CACHED" : "");
	Text(canvas,line,18,78);
	const auto& lod=m_profilerStatistics.lodInstanceCounts;
	std::snprintf(line,sizeof(line),"LOD %.2fPX  L0 %u  L1 %u  L2 %u  L3 %u",config.lodPixelError,lod[0],lod[1],lod[2],lod[3]);
	if(m_profilerStatistics.gpuVisibility) // GPU가 고른 LOD 개수 대신 진단 색상 안내를 표시한다.
		std::snprintf(line,sizeof(line),"GPU LOD %.2fPX  COUNTS N/A  COLOR VIEW AVAILABLE",config.lodPixelError);
	Text(canvas,line,18,98,{1,1,1,1},1.6f);
	std::snprintf(line,sizeof(line),"MAIN TRI ORIGINAL %llu > SELECTED %llu",
		static_cast<unsigned long long>(m_profilerStatistics.mainOriginalTriangleCount),
		static_cast<unsigned long long>(m_profilerStatistics.mainTriangleCount));
	if(m_profilerStatistics.gpuVisibility) // GPU에 남아 있는 삼각형 개수를 CPU 통계로 추정하지 않는다.
		std::snprintf(line,sizeof(line),"GPU COUNTS REMAIN ON DEVICE; COMPARE PASS TIMES");
	Text(canvas,line,18,118,{1,1,1,1},1.6f);
	std::snprintf(line,sizeof(line),"RENDER %u X %u / OUTPUT %u X %u",m_profilerStatistics.renderWidth,
		m_profilerStatistics.renderHeight,m_profilerStatistics.outputWidth,m_profilerStatistics.outputHeight);
	// 전체 구간에는 명령 준비 대기가 섞일 수 있으므로 측정된 패스 합계도 함께 표시한다.
	if(!m_profiler.GetSnapshot().gpuPasses.empty()) // 패스 표본이 있으면 패스 평균 합계를 별도로 표시한다.
	{
		double passMilliseconds=0;
		for(const auto& pass:m_profiler.GetSnapshot().gpuPasses)passMilliseconds+=pass.averageMilliseconds;
		std::snprintf(line,sizeof(line),"RENDER %u X %u / OUTPUT %u X %u / GPU PASS %.2f MS",
			m_profilerStatistics.renderWidth,m_profilerStatistics.renderHeight,
			m_profilerStatistics.outputWidth,m_profilerStatistics.outputHeight,passMilliseconds);
	}
	Text(canvas,line,18,138,{1,1,1,1},1.6f);
	const auto& counters=m_profiler.GetSnapshot().liveResources;
	if(!expanded) return canvas; // 요약 모드이면 여기까지의 HUD를 반환한다.
	// 확장 화면에서 프레임 단계별 CPU 평균을 나누어 표시한다.
	std::snprintf(line,sizeof(line),"CPU ACQUIRE %.2f  PREPARE %.2f  RECORD %.2f MS",m_profiler.GetSnapshot().cpuAcquireAverageMilliseconds,
		m_profiler.GetSnapshot().cpuPrepareAverageMilliseconds,m_profiler.GetSnapshot().cpuRecordAverageMilliseconds);
	Text(canvas,line,18,160,{1,1,1,1},1.6f);
	std::snprintf(line,sizeof(line),"CPU SUBMIT %.2f  PRESENT %.2f  READBACK %.2f MS",m_profiler.GetSnapshot().cpuSubmitAverageMilliseconds,
		m_profiler.GetSnapshot().cpuPresentAverageMilliseconds,m_profiler.GetSnapshot().cpuReadbackAverageMilliseconds);
	Text(canvas,line,18,180,{1,1,1,1},1.6f);
	std::snprintf(line,sizeof(line),"ENTITIES %u  SHADOW INST %u  TRI %llu",m_entities,m_profilerStatistics.shadowInstanceCount,
		static_cast<unsigned long long>(m_profilerStatistics.shadowTriangleCount));
	Text(canvas,line,18,200,{1,1,1,1},1.6f);
	// 자원 카운터와 GPU 표본 수를 함께 표시해 시간 값의 근거를 볼 수 있게 한다.
	std::snprintf(line,sizeof(line),"LIVE BUF %llu TEX %llu PSO %llu  GPU SAMPLES %u",
		static_cast<unsigned long long>(counters[0]),static_cast<unsigned long long>(counters[1]),
		static_cast<unsigned long long>(counters[2]),m_profiler.GetSnapshot().gpuFrameSampleCount);
	Text(canvas,line,18,220,{1,1,1,1},1.6f);
	std::snprintf(line,sizeof(line),"GPU CULL %s  HIZ %s  CLUSTER %s  SURFACE %s",
		m_profilerStatistics.gpuVisibility ? "ON" : "OFF",m_profilerStatistics.gpuOcclusion ? "ON" : "OFF",
		m_profilerStatistics.clusteredLighting ? "ON" : "OFF",m_profilerStatistics.surfacePrepass ? "ON" : "OFF");
	Text(canvas,line,18,240,{1,1,1,1},1.6f);
	std::snprintf(line,sizeof(line),"SHADOW VIEWS %u  TILE %u PX  DISTANCE %.1f",
		m_profilerStatistics.shadowViewCount,m_profilerStatistics.shadowViewResolution,m_profilerStatistics.directionalShadowDistance);
	Text(canvas,line,18,260,{1,1,1,1},1.6f);
	// 프레임·CPU·GPU 선과 16.67ms 기준선을 서로 다른 색으로 구분한다.
	const Math::float4 cyan{.1f,.8f,1,1},green{.3f,1,.4f,1},orange{1,.65f,.15f,1},red{1,.3f,.3f,1};
	const float columnWidth=(panelWidth-16)/columns;
	// 각 GPU 패스의 측정 평균을 막대와 숫자로 표시한다. 전체 GPU 구간 대비 막대 비율은 0..1로 제한한다.
	for(uint32_t i=0;i<m_profiler.GetSnapshot().gpuPasses.size();++i)
	{
		const auto& pass=m_profiler.GetSnapshot().gpuPasses[i];
		const float x=18+(i/rows)*columnWidth,y=284+(i%rows)*18;
		if(y+14>height-28) break; // 패스 항목이 화면 하단을 넘기 전에 목록을 멈춘다.
		const auto fraction=std::clamp(pass.averageMilliseconds/std::max(m_profiler.GetSnapshot().gpuFrameAverageMilliseconds,1.0),0.0,1.0);
		canvas.FillRect({x,y,columnWidth*static_cast<float>(fraction),14},{.15f,.28f,.4f,.7f});
		std::snprintf(line,sizeof(line),"%.28s",pass.name.c_str());
		Text(canvas,line,x,y,{1,1,1,1},1.5f);
		std::snprintf(line,sizeof(line),"%.2f MS",pass.averageMilliseconds);
		Text(canvas,line,x+columnWidth-84,y,orange,1.5f);
	}
	if(graphTop+124>height-20) return canvas; // 그래프를 놓을 세로 공간이 없으면 목록까지만 반환한다.
	// 현재 시간 배율을 그래프 세로 위치로 변환한다. 16.67ms 선은 약 60Hz의 프레임 간격 기준이다.
	const auto y=[&](float milliseconds){return graphTop+100-100.f*std::clamp(milliseconds/m_graphScaleMilliseconds,0.f,1.f);};
	const float graphWidth=std::min(500.f,panelWidth-24);
	canvas.Line({18,y(16.67f)},{18+graphWidth,y(16.67f)},red,1.2f);
	for(uint32_t i=1;i<m_count;++i)
	{
		// 원형 버퍼를 오래된 표본부터 시간 순으로 읽어 이웃한 두 지점을 잇는다.
		const uint32_t a=(m_cursor+m_history.size()-m_count+i-1)%m_history.size();
		const uint32_t b=(a+1)%m_history.size();
		const float xa=18+(i-1)*graphWidth/119,xb=18+i*graphWidth/119;
		// Canvas::Line emits triangles, including on D3D12; no unsupported line topology is used.
		canvas.Line({xa,y(m_history[a])},{xb,y(m_history[b])},cyan,1.5f);
		canvas.Line({xa,y(m_cpuHistory[a])},{xb,y(m_cpuHistory[b])},green,1.5f);
		if(m_gpuHistoryValid[a] && m_gpuHistoryValid[b]) // 양 끝 표본이 모두 유효할 때만 GPU 선을 연결한다.
			canvas.Line({xa,y(m_gpuHistory[a])},{xb,y(m_gpuHistory[b])},orange,1.5f);
	}
	Text(canvas,"FRAME",18,graphTop+110,cyan);Text(canvas,"CPU",108,graphTop+110,green);
	Text(canvas,"GPU SPAN",174,graphTop+110,orange);Text(canvas,"16.67 MS",300,graphTop+110,red);
	return canvas;
}
}
