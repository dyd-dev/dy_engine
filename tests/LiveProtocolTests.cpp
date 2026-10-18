#include "LiveInternal.h"
#include "LiveGpuTiming.h"
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace dyf::Diagnostics;
using namespace dyf::Diagnostics::Detail;
#define CHECK(x) do{if(!(x)){std::cerr<<__LINE__<<": " #x "\n";return 1;}}while(false)
int main()
{
    LiveSnapshot input;input.session={123,456};input.clock=GetLiveClockInfo();input.sequence=7;input.producerTimeNs=2000000000;
    LiveSourceSnapshot source;source.device=10;source.source=11;source.latest.key={10,11,12};source.latest.objects={LiveValueState::Ready,0};source.recent.objects.state=LiveValueState::Ready;source.recent.objects.ready=1;
    source.recent.mixedRevision=true;source.recent.beginNs=1000000000;source.recent.endNs=2000000000;
    source.recent.device=10;source.recent.source=11;source.recentSeconds.push_back(source.recent);
    source.runRanges.drawCalls={LiveValueState::Ready,0,100,12};source.runRanges.gpuMeasured={LiveValueState::Ready,1.5,3.5,7};
    LiveGpuSpan span;span.id=13;span.frame=source.latest.key;span.appliedRevision=4;span.begin.command={14,2,3};span.end.command={14,2,9};span.begin.submission=span.end.submission=15;span.duration={LiveValueState::Ready,2.5};span.hasQueueInterval=true;span.beginGpuNs=20;span.endGpuNs=30;span.gpuClockDomain=99;span.hasCpuInterval=true;span.beginCpuNs=100;span.endCpuNs=110;span.cpuCalibrationErrorKnown=false;
    source.flow.gpuSpans.push_back(span);source.recentGpuSpans.push_back(span);input.sources.push_back(source);
    auto& evidence=input.sources[0].comparisonEvidence;evidence.request=25;evidence.before.available=true;
    evidence.before.summary=source.latest;evidence.before.summary.appliedRevision=4;evidence.before.summary.beginNs=1000000001;evidence.before.summary.endNs=1000000002;
    evidence.before.flow.key=source.latest.key;evidence.before.flow.gpuSpans.push_back(span);evidence.before.windowBeginNs=1000000000;evidence.before.windowEndNs=2000000000;evidence.before.windowRevision=4;
    evidence.before.objects.push_back({10,{LiveObjectKind::Device,0,0,0,0,"referenced device"}});evidence.after=evidence.before;
    ControlDesc metadata;metadata.id=24;metadata.device=10;metadata.source=metadata.owner=11;metadata.key="optimization.test";metadata.category=ControlCategory::BuildOrStructural;metadata.kind=ControlKind::Integer;metadata.appliedValue=std::int64_t(5);metadata.maximum=10;
    metadata.description="Algorithm";metadata.onPath="cached";metadata.offPath="reference";metadata.conditions="same inputs";metadata.tradeoffs="lookup and memory";input.controls.push_back(metadata);
    ControlStatus status;status.request=25;status.control=24;status.device=10;status.source=11;status.previousActual=status.actual=status.requested=std::int64_t(5);status.state=ControlState::Applied;status.appliedFrame=source.latest.key;status.appliedRevision=4;
    status.baseline.device=10;status.baseline.source=11;status.baseline.appliedRevision=4;status.baseline.beginNs=1000000000;status.baseline.endNs=2000000000;status.baseline.frameCount=1;status.after=status.baseline;input.controlStatus.push_back(status);
    input.sources[0].latest.recorded.uploadPageAllocations=7;input.sources[0].latest.recorded.uploadPageReuses=9;input.sources[0].latest.recorded.uploadPageAllocationBytes=4096;
    auto bytes=EncodeSnapshot(input);LiveSnapshot output;std::string error;
    CHECK(DecodeSnapshot(bytes,output,error));CHECK(output.sources.size()==1);CHECK(output.sources[0].recentGpuSpans[0].begin.command.index==3);CHECK(output.sources[0].flow.gpuSpans[0].hasCpuInterval);CHECK(!output.sources[0].flow.gpuSpans[0].cpuCalibrationErrorKnown);CHECK(output.sources[0].latest.objects.state==LiveValueState::Ready&&output.sources[0].latest.objects.count==0);
    CHECK(output.sources[0].recent.mixedRevision&&output.sources[0].recent.endNs-output.sources[0].recent.beginNs==LiveAverageWindowNs);
    CHECK(output.sources[0].runRanges.drawCalls.low==0&&output.sources[0].runRanges.drawCalls.high==100&&output.sources[0].runRanges.drawCalls.windows==12&&output.sources[0].runRanges.gpuMeasured.low==1.5);
    CHECK(output.sources[0].recentSeconds.size()==1&&output.sources[0].recentSeconds[0].mixedRevision);
    CHECK(output.controls[0].category==ControlCategory::BuildOrStructural&&output.controls[0].description=="Algorithm"&&output.controls[0].offPath=="reference"&&output.controls[0].tradeoffs=="lookup and memory");
    CHECK(std::get<std::int64_t>(output.controlStatus[0].previousActual)==5&&output.sources[0].latest.recorded.uploadPageAllocations==7&&output.sources[0].latest.recorded.uploadPageReuses==9&&output.sources[0].latest.recorded.uploadPageAllocationBytes==4096);
    CHECK(output.sources[0].comparisonEvidence.request==25&&output.sources[0].comparisonEvidence.before.available&&output.sources[0].comparisonEvidence.before.summary.key==source.latest.key&&output.sources[0].comparisonEvidence.before.objects[0].desc.label=="referenced device");
    input.sources[0].comparisonEvidence.before.flow.gpuSpans[0].appliedRevision=9;CHECK(!DecodeSnapshot(EncodeSnapshot(input),output,error));input.sources[0].comparisonEvidence.before.flow.gpuSpans[0]=span;
    LiveEvent mixedEvent;mixedEvent.context.frame=source.latest.key;mixedEvent.context.appliedRevision=3;input.sources[0].comparisonEvidence.before.flow.events.push_back(mixedEvent);CHECK(!DecodeSnapshot(EncodeSnapshot(input),output,error));input.sources[0].comparisonEvidence.before.flow.events.clear();
    ++input.sources[0].comparisonEvidence.before.windowBeginNs;CHECK(!DecodeSnapshot(EncodeSnapshot(input),output,error));--input.sources[0].comparisonEvidence.before.windowBeginNs;
    bytes.pop_back();CHECK(!DecodeSnapshot(bytes,output,error));
    input.sources[0].runRanges.drawCalls.high=-1;CHECK(!DecodeSnapshot(EncodeSnapshot(input),output,error));input.sources[0].runRanges.drawCalls=source.runRanges.drawCalls;
    input.sources[0].recentSeconds[0].endNs=2000000001;CHECK(!DecodeSnapshot(EncodeSnapshot(input),output,error));input.sources[0].recentSeconds=source.recentSeconds;
    input.sources[0].recent.appliedRevision=9;CHECK(!DecodeSnapshot(EncodeSnapshot(input),output,error));input.sources[0].recent.appliedRevision=0;
    input.sources[0].flow.gpuSpans[0].endCpuNs=90;CHECK(!DecodeSnapshot(EncodeSnapshot(input),output,error));
    input.sources[0].flow.gpuSpans[0]=span;input.sources[0].recentGpuSpans[0].endCpuNs=90;CHECK(!DecodeSnapshot(EncodeSnapshot(input),output,error));
    input.sources[0].recentGpuSpans[0]=span;input.sources[0].recentGpuSpans[0].frame.device=999;CHECK(!DecodeSnapshot(EncodeSnapshot(input),output,error));
#ifdef _WIN32
    dyf::RHI::TimestampCalibration pair;pair.cpuDomain=dyf::RHI::CpuTimestampDomain::QueryPerformanceCounter;pair.cpuFrequency=GetLiveClockInfo().frequency;pair.cpuTicks=pair.cpuFrequency*10;pair.gpuTicks=1000;pair.gpuValidBits=64;pair.gpuPeriodNanoseconds=1;
    LiveGpuSpan calibrated;calibrated.duration={LiveValueState::Ready,0.0002};CalibrateGpuSpan(calibrated,pair,900,1100,99,1,100);
    CHECK(calibrated.hasCpuInterval&&calibrated.hasQueueInterval&&calibrated.endCpuNs-calibrated.beginCpuNs==200&&!calibrated.cpuCalibrationErrorKnown);
    pair.gpuValidBits=8;pair.gpuTicks=250;pair.maximumDeviationNanoseconds=17;calibrated={};CalibrateGpuSpan(calibrated,pair,250,10,99,1,100);
    CHECK(calibrated.hasCpuInterval&&!calibrated.hasQueueInterval&&calibrated.endCpuNs-calibrated.beginCpuNs==16&&calibrated.cpuCalibrationErrorNs==17);
    calibrated={};calibrated.duration={LiveValueState::Ready,0.000016};CalibrateGpuSpan(calibrated,pair,250,10,99,1,129);
    CHECK(!calibrated.hasCpuInterval&&calibrated.duration.state==LiveValueState::Ready);
    pair.gpuValidBits=64;++pair.cpuFrequency;CalibrateGpuSpan(calibrated,pair,900,1100,99,1,100);CHECK(!calibrated.hasCpuInterval);
#endif
    LiveSnapshot empty;bytes=EncodeSnapshot(empty);CHECK(bytes.size()>53);bytes[50]=bytes[51]=bytes[52]=bytes[53]=255;CHECK(!DecodeSnapshot(bytes,output,error));
    PacketHeader header;header.kind=1;header.size=20;CHECK(ValidateHeader(header));++header.version;CHECK(!ValidateHeader(header));header.version=LiveProtocolVersion;header.size=std::uint32_t(LiveTransferLimit);CHECK(!ValidateHeader(header));
    ControlRequest request{22,23,std::int64_t(-7)},decoded;bytes=EncodeControl(request);CHECK(DecodeControl(bytes,decoded,error));CHECK(std::get<std::int64_t>(decoded.value)==-7);
    bytes[16]=255;CHECK(!DecodeControl(bytes,decoded,error));request.value=std::numeric_limits<double>::quiet_NaN();CHECK(!DecodeControl(EncodeControl(request),decoded,error));
    ControlAck ack{22,false,"duplicate"},decodedAck;CHECK(DecodeAck(EncodeAck(ack),decodedAck,error));CHECK(!decodedAck.accepted&&decodedAck.message=="duplicate");
    LiveSnapshot large;
    large.controls.resize(2);
    const auto count=LiveTransferLimit/(2*sizeof(ControlChoice))+1;
    for(auto& control:large.controls)control.choices.resize(count);
    bool budgetRejected=false;try{(void)EncodeSnapshot(large);}catch(const std::length_error&){budgetRejected=true;}
    CHECK(budgetRejected);bytes=EncodeBoundedSnapshot(large);CHECK(bytes.size()<LiveTransferLimit);CHECK(large.loss.transferTruncated);CHECK(DecodeSnapshot(bytes,output,error));
    std::cout<<"Live protocol contracts passed\n";
}
