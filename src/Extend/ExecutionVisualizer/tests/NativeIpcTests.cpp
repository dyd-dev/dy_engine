#include "dyf/Diagnostics/LiveConnection.h"
#include "../Process.h"
#include "NativeMetrics.h"
#include "dyf/Diagnostics/LiveProfiling.h"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>
#include <tuple>

namespace D=dyf::Diagnostics;
using Clock=std::chrono::steady_clock;
using FrameKey=std::tuple<D::LiveId,D::LiveId,D::LiveId>;
static FrameKey Key(D::LiveFrameKey key) { return {key.device,key.source,key.frame}; }
static void Check(bool value,const std::string& reason) { if(!value) throw std::runtime_error(reason); }
static uint32_t Positive(const std::wstring& value,bool allowZero=false)
{
    const auto text=viewer::ToUtf8(value); uint32_t result=0;
    const auto parsed=std::from_chars(text.data(),text.data()+text.size(),result);
    Check(parsed.ec==std::errc{} && parsed.ptr==text.data()+text.size() && (result || allowZero),"Expected a valid 32-bit integer (positive except --cycles)");
    return result;
}
struct Options
{
    int example=0; uint32_t pid=0,cycles=2,frames=900,timeout=10,exitTimeout=30;
    std::string executable,arguments,directory;
    std::filesystem::path report;
    bool selfCheck=false,noConnect=false;
};
static Options Parse(int argc,wchar_t** argv)
{
    Options options;
    for(int i=1;i<argc;++i)
    {
        const std::wstring name(argv[i]);
        if(name==L"--self-check") { options.selfCheck=true; continue; }
        if(name==L"--no-connect") { options.noConnect=true; continue; }
        Check(i+1<argc,"Each option requires a value"); const std::wstring value(argv[++i]);
        if(name==L"--example") options.example=int(Positive(value));
        else if(name==L"--pid") options.pid=Positive(value);
        else if(name==L"--cycles") options.cycles=Positive(value,true);
        else if(name==L"--frames") options.frames=Positive(value);
        else if(name==L"--timeout") options.timeout=Positive(value);
        else if(name==L"--exit-timeout") options.exitTimeout=Positive(value);
        else if(name==L"--target") options.executable=viewer::ToUtf8(value);
        else if(name==L"--arguments") options.arguments=viewer::ToUtf8(value);
        else if(name==L"--working-directory") options.directory=viewer::ToUtf8(value);
        else if(name==L"--report") options.report=value;
        else throw std::runtime_error("Unknown option: "+viewer::ToUtf8(name));
    }
    if(!options.selfCheck)
    {
        Check(options.example==14 || options.example==15,"--example must be 14 or 15");
        Check((options.pid!=0)!=(!options.executable.empty()),"Select exactly one --target or --pid");
        Check(options.cycles<=32 && options.timeout<=120 && options.exitTimeout<=120,"Cycles <=32; timeout and exit-timeout <=120 seconds");
        Check(!options.report.empty(),"--report is required for native evidence");
        if(options.noConnect) { Check(!options.executable.empty(),"--no-connect requires an owned --target"); options.cycles=0; }
    }
    return options;
}
static void SelfCheck()
{
    Check(Positive(L"1")==1 && Positive(L"4294967295")==UINT32_MAX,"Unsigned argument boundaries");
    for(const auto* value:{L"",L"0",L"-1",L"1x",L"4294967296"})
    {
        bool rejected=false; try { (void)Positive(value); } catch(const std::runtime_error&) { rejected=true; }
        Check(rejected,"Invalid numeric argument rejected");
    }
    Check(Key({1,2,3})!=Key({1,9,3}),"Frame joins include device and source, not only frame ID");
    std::vector<std::wstring> observation{L"driver",L"--example",L"14",L"--pid",L"123",L"--cycles",L"0",L"--report",L"not-created.txt"};
    std::vector<wchar_t*> arguments; for(auto& value:observation) arguments.push_back(value.data());
    Check(Parse(int(arguments.size()),arguments.data()).cycles==0,"Zero cycles selects observation without control exercise");
    D::LiveSourceSnapshot history;
    D::LiveAggregate bucket; bucket.device=1; bucket.source=2; bucket.appliedRevision=3; bucket.beginNs=100; bucket.endNs=200; bucket.frameCount=5;
    history.device=1; history.source=2; history.history={bucket,bucket};
    bucket.appliedRevision=4; bucket.frameCount=2; history.history.push_back(bucket);
    Check(native_metrics::HistoryFrames(history)==7,"Count revision partitions once and deduplicate repeated history buckets");
    auto overlap=bucket; overlap.appliedRevision=3; overlap.beginNs=150; overlap.endNs=250; history.history.push_back(overlap);
    bool overlapRejected=false; try { (void)native_metrics::HistoryFrames(history); } catch(const std::runtime_error&) { overlapRejected=true; }
    Check(overlapRejected,"Reject overlapping windows within one revision instead of double-counting");
    D::LiveSourceSnapshot failed; failed.device=1; failed.source=2;
    auto& failure=failed.latest; failure.key={1,2,3}; failure.appliedRevision=4;
    failure.beginNs=100; failure.endNs=200; failure.outcome=D::LiveOutcome::Failed;
    failed.flow.key=failure.key;
    D::LiveEvent acquire; acquire.context.frame=failure.key; acquire.context.appliedRevision=4;
    acquire.kind=D::LiveEventKind::Wait; acquire.stage=D::LiveStage::Output; acquire.outcome=D::LiveOutcome::Skipped;
    std::memcpy(acquire.label,"BeginFrame / acquire",sizeof("BeginFrame / acquire")); failed.flow.events={acquire};
    native_metrics::RetryAudit audit; audit.Observe(failed); audit.Observe(failed);
    Check(audit.Candidates()==1 && audit.Confirmed()==0,"Old Failed/acquire-only frame is one candidate, not an automatic retry");
    auto normal=failed; normal.latest.key.frame=99999; normal.latest.beginNs=201; normal.latest.endNs=300;
    normal.latest.outcome=D::LiveOutcome::Completed; normal.latest.recorded.draw=1; normal.latest.submitted.submissions=1;
    normal.flow.key=normal.latest.key; auto& present=normal.flow.events.front(); present.context.frame=normal.latest.key;
    present.kind=D::LiveEventKind::Output; present.commandKind=D::LiveCommandKind::Present; present.outcome=D::LiveOutcome::Completed; present.submission=9;
    auto other=normal; other.source=7; other.latest.key.source=7; other.flow.key=other.latest.key; other.flow.events.front().context.frame=other.latest.key;
    audit.Observe(other); Check(audit.Confirmed()==0,"Another source completing work cannot classify a candidate");
    audit.Observe(normal); audit.Observe(normal);
    Check(audit.Confirmed()==1,"Later actual work/Present confirms old Failed retry once without frame ID subtraction");
    native_metrics::RetryAudit excluded; excluded.Observe(failed,true);
    Check(!excluded.Candidates(),"An actual applied settings transition is not a retry candidate");
    auto terminal=failed; std::memcpy(terminal.flow.events.front().label,"Submission failure",sizeof("Submission failure"));
    excluded.Observe(terminal); excluded.Observe(normal);
    Check(!excluded.Candidates() && !excluded.Confirmed(),"Other Failed errors are not acquire retries");
    auto missing=failed; missing.flow.events.front().context.frame.source=7; excluded.Observe(missing);
    Check(!excluded.Candidates(),"A mismatched original flow frame cannot classify a retry");
    std::vector<std::wstring> baseline{L"driver",L"--example",L"14",L"--target",L"not-executed.exe",L"--no-connect",L"--report",L"not-created.txt"};
    arguments.clear(); for(auto& value:baseline) arguments.push_back(value.data());
    const auto noConnect=Parse(int(arguments.size()),arguments.data());
    Check(noConnect.noConnect && noConnect.cycles==0,"Baseline never connects or exercises controls");
    const auto cost=native_metrics::DriverCutoff(); Check(cost.creation && cost.exit>=cost.creation,"Actual Windows process CPU/cutoff timing");
    std::puts("PASS native driver argument boundaries and composite frame join; no native producer executed");
}
struct Captured
{
    D::LiveFrameSummary frame;
    uint64_t batchEvents=0,batchElements=0,drawEvents=0,drawInstances=0,uploadBytes=0;
    D::LiveId commandList=0,submission=0;
    uint64_t generation=0,buffersCreated=0;
    bool buffersKnown=false,details=false;
};
struct GpuIdentity { D::LiveFrameKey frame; D::LiveId revision; D::LiveValueState state; };

class Run
{
public:
    explicit Run(Options value):options(std::move(value)),report(options.report)
    {
        Check(bool(report),"Cannot open --report");
        report<<"evidence="<<(options.noConnect ? "native_process_baseline" : options.cycles ? "native_ipc_correctness" : "native_ipc_observation")<<"\ngui_input=not_measured\ngui_p95=not_measured\n";
        if(!options.cycles) report<<"control_correctness=not_exercised\n";
    }
    int Execute()
    {
        int result=0;
        try
        {
            Start();
            if(options.cycles) { Exercise(); report<<"control_correctness=PASS\ncorrectness=PASS\n"; }
            else report<<(options.noConnect ? "baseline=no discovery, connection or control requests\nframe_retry_audit=unavailable_disconnected\n" : "observation=connected; controls not sent\n");
        }
        catch(const std::exception& error)
        {
            report<<(options.cycles ? "correctness=FAIL\n" : "observation=FAIL\n")<<"reason="<<error.what()<<'\n'; std::fprintf(stderr,"FAIL native IPC: %s\n",error.what());
            if(options.cycles) Restore(); result=1;
        }
        if(!WaitForExit()) result=1;
        if(retryAuditFailed) result=1;
        client.Disconnect();
        report<<"snapshots="<<snapshots<<"\nskipped_snapshot_sequences="<<skippedSequences<<"\ngpu_pending_observations="<<pending
            <<"\ngpu_ready_observations="<<ready<<"\ngpu_unsupported_observations="<<unsupported<<"\ngpu_unavailable_observations="<<unavailable
            <<"\ngpu_pending_to_ready="<<pendingToReady<<"\nloss_max_events="<<loss.events<<"\nloss_max_frames="<<loss.frames
            <<"\nloss_max_links="<<loss.links<<"\nloss_max_inventory="<<loss.inventory<<"\nloss_max_gpu_spans="<<loss.gpuSpans
            <<"\ntransfer_truncated_observed="<<loss.transferTruncated<<"\nresult="<<(result ? "FAIL" : "PASS")<<'\n';
        report<<"gpu_fence_original_frame_joins="<<fenceJoins<<"\ngpu_ready_original_frame_joins="<<readyJoins<<'\n';
        if(!options.noConnect) report<<"history_frames_max_single_snapshot="<<maxHistoryFrames<<"\nrecent_frames_max="<<maxRecentFrames
            <<"\nframe_budget_excess_observed="<<budgetExcess<<"\nempty_completed_steady_observations="<<emptyCompleted
            <<"\nempty_failed_or_skipped_observations="<<emptyFailed<<"\ntransition_frames_observed="<<transitions
            <<"\nno_work_acquire_failure_candidates="<<retryAudit.Candidates()<<"\nconfirmed_no_work_acquire_retries="<<retryAudit.Confirmed()
            <<"\nretry_audit="<<(retryAuditFailed ? "FAIL" : budgetExcess ? "excess_history_requires_classification" : snapshots && haveControl ? "no_excess_observed_in_history_window" : "unavailable")<<'\n';
        native_metrics::Report(report,"driver",native_metrics::DriverCutoff(),false);
        report.flush(); if(!report.good()) return 1;
        if(!result) std::puts(options.cycles ? "PASS real native IPC control/revision, command/batch/upload trace and original-frame Ready GPU completion; GUI/p95 not measured"
            : options.noConnect ? "PASS owned finite baseline and process timing; IPC/control/GUI not exercised"
            : "PASS native IPC observation and natural exit; control correctness and GUI/p95 not exercised");
        return result;
    }
private:
    Options options;
    std::ofstream report;
    viewer::TargetProcess process;
    native_metrics::Meter targetMeter;
    D::LiveClient client;
    std::shared_ptr<const D::LiveSnapshot> snapshot;
    D::ControlDesc control;
    bool haveControl=false,initial=false;
    bool retryAuditFailed=false;
    bool budgetExcess=false;
    native_metrics::RetryAudit retryAudit;
    uint64_t emptyCompleted=0,emptyFailed=0,transitions=0,connectedNs=0;
    std::set<FrameKey> classified;
    uint64_t maxHistoryFrames=0,maxRecentFrames=0;
    uint64_t snapshots=0,skippedSequences=0,pending=0,ready=0,unsupported=0,unavailable=0,pendingToReady=0,fenceJoins=0,readyJoins=0;
    D::LiveLoss loss;
    std::map<FrameKey,Captured> captured;
    std::map<D::LiveId,GpuIdentity> gpu;
    std::set<FrameKey> completed;
    std::set<FrameKey> timedCompleted;

    const D::LiveSourceSnapshot* Source() const
    {
        if(snapshot) for(const auto& source:snapshot->sources) if(source.device==control.device && source.source==control.source) return &source;
        return nullptr;
    }
    const D::LiveDeviceSnapshot* Device() const
    {
        if(snapshot) for(const auto& device:snapshot->devices) if(device.device==control.device) return &device;
        return nullptr;
    }
    void Start()
    {
        if(!options.executable.empty())
        {
            std::string error;
            const auto arguments=options.arguments+" --frames "+std::to_string(options.frames);
            Check(process.Launch(options.executable,arguments,options.directory,error),error);
            targetMeter.Open(process.Pid());
            options.pid=process.Pid(); report<<"owned_pid="<<options.pid<<"\ntarget="<<options.executable<<"\nfinite_frames="<<options.frames<<'\n';
        }
        else report<<"attached_pid="<<options.pid<<'\n';
        if(options.noConnect) return;
        const auto deadline=Clock::now()+std::chrono::seconds(options.timeout);
        bool connected=false;
        while(Clock::now()<deadline && !connected)
        {
            if(process.Pid()) Check(process.Running(),"Owned target exited before discovery");
            for(const auto& target:D::LiveClient::Discover()) if(target.processId==options.pid)
            { Check(client.Connect(target),"Connect could not be queued"); connected=true; break; }
            if(!connected) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        Check(connected,"No instrumented session for the target PID");
        const std::string expected=options.example==14 ? "example14.instancing" : "example15.reuse";
        while(Clock::now()<deadline && !haveControl)
        {
            Receive();
            if(snapshot) for(const auto& desc:snapshot->controls) if(desc.key==expected)
            {
                Check(desc.writable && desc.kind==D::ControlKind::Boolean && std::holds_alternative<bool>(desc.appliedValue),"Expected registered writable boolean control");
                control=desc; initial=std::get<bool>(desc.appliedValue); haveControl=true; break;
            }
            if(!haveControl) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        Check(haveControl,"Registered example control was not received");
        const auto* device=Device(); Check(device && (device->backend=="D3D12" || device->backend=="Vulkan"),"Native D3D12/Vulkan device metadata required");
        Check(device->queueCount==1,"Examples currently require one actual device queue");
        report<<"backend="<<device->backend<<"\ncontrol="<<control.id<<"\ncontrol_key="<<control.key<<"\ninitial_value="<<initial<<'\n';
        connectedNs=D::LiveNowNs();
        const auto warmUntil=Clock::now()+std::chrono::milliseconds(600);
        while(Clock::now()<warmUntil) { Receive(); std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
    }
    bool Receive()
    {
        if(options.noConnect) return false;
        if(client.GetState()==D::LiveClientState::Failed) throw std::runtime_error("IPC failed: "+client.GetLastError());
        auto next=client.Poll(); if(!next) return false;
        if(snapshot)
        {
            Check(next->session==snapshot->session,"Session changed during a single native run");
            Check(next->sequence>snapshot->sequence,"Snapshot sequence did not advance");
            skippedSequences+=next->sequence-snapshot->sequence-1;
        }
        snapshot=std::move(next); ++snapshots;
        if(!options.cycles && haveControl) for(const auto& desc:snapshot->controls) if(desc.id==control.id)
            Check(std::holds_alternative<bool>(desc.appliedValue) && std::get<bool>(desc.appliedValue)==initial,"Observed workload control changed during --cycles 0");
        const auto& value=snapshot->loss;
        loss.events=std::max(loss.events,value.events); loss.links=std::max(loss.links,value.links); loss.frames=std::max(loss.frames,value.frames);
        loss.inventory=std::max(loss.inventory,value.inventory); loss.gpuSpans=std::max(loss.gpuSpans,value.gpuSpans); loss.transferTruncated|=value.transferTruncated;
        if(const auto* source=Source())
        {
            try
            {
                const auto frames=native_metrics::HistoryFrames(*source);
                maxHistoryFrames=std::max(maxHistoryFrames,frames); maxRecentFrames=std::max(maxRecentFrames,source->recent.frameCount);
                report<<"history_window_frames="<<frames<<",recent_frames="<<source->recent.frameCount<<",device="<<source->device<<",source="<<source->source<<'\n';
                if(process.Pid() && (frames>options.frames || source->recent.frameCount>options.frames))
                {
                    budgetExcess=true; report<<"frame_budget_warning=finite "<<options.frames<<",history "<<frames<<",recent "<<source->recent.frameCount<<';'
                        <<" not an automatic failure: failed/transition frames require classification\n";
                }
            }
            catch(...) { retryAuditFailed=true; throw; }
            const auto& frame=source->latest;
            bool transition=false;
            for(const auto& status:snapshot->controlStatus) if(status.state==D::ControlState::Applied && status.appliedFrame==frame.key) transition=true;
            const auto candidateCount=retryAudit.Candidates(),confirmedCount=retryAudit.Confirmed();
            retryAudit.Observe(*source,transition);
            if(retryAudit.Candidates()!=candidateCount) report<<"no_work_acquire_retry_candidate="<<frame.key.frame<<",device="<<frame.key.device<<",source="<<frame.key.source<<",revision="<<frame.appliedRevision<<'\n';
            if(retryAudit.Confirmed()!=confirmedCount) report<<"confirmed_no_work_acquire_retries="<<retryAudit.Confirmed()<<",after_work_present_frame="<<frame.key.frame<<",device="<<frame.key.device<<",source="<<frame.key.source<<'\n';
            if(classified.insert(Key(frame.key)).second)
            {
                const bool work=native_metrics::HasWork(frame);
                if(transition) ++transitions;
                else if(!work && (frame.outcome==D::LiveOutcome::Failed || frame.outcome==D::LiveOutcome::Skipped)) ++emptyFailed;
                else if(!work && frame.outcome==D::LiveOutcome::Completed && connectedNs && frame.beginNs>=connectedNs+600000000 && !frame.incomplete)
                    ++emptyCompleted;
            }
            if(budgetExcess && (emptyCompleted || retryAudit.Confirmed()))
            {
                retryAuditFailed=true;
                throw std::runtime_error("History exceeded finite frame budget with a steady no-work Completed frame or confirmed acquire retry outside actual applied transitions");
            }
            report<<"snapshot="<<snapshot->sequence<<','<<frame.key.device<<','<<frame.key.source<<','<<frame.key.frame<<','<<frame.appliedRevision
                <<",recorded_draw="<<frame.recorded.draw<<",submitted_draw="<<frame.submitted.draw<<",completed_draw="<<frame.completed.draw
                <<",instances="<<frame.recorded.instances<<",batches="<<frame.recorded.batches<<",upload="<<frame.recorded.uploadBytes
                <<",incomplete="<<frame.incomplete<<",loss="<<value.events<<'/'<<value.frames<<'/'<<value.gpuSpans<<'\n';
            Captured current; current.frame=frame;
            if(const auto* device=Device()) for(const auto& resource:device->resources) if(resource.kind==D::LiveObjectKind::Buffer)
            { current.buffersKnown=true; current.buffersCreated=resource.created; }
            current.details=source->flow.key==frame.key && !source->flow.omittedEvents && !source->flow.omittedLinks && !frame.incomplete;
            if(current.details) for(const auto& event:source->flow.events)
            {
                if(event.context.frame!=frame.key || event.context.appliedRevision!=frame.appliedRevision) { current.details=false; continue; }
                if(event.kind==D::LiveEventKind::Batch) { ++current.batchEvents; current.batchElements+=event.elements; }
                if(event.kind==D::LiveEventKind::Upload) current.uploadBytes+=event.bytes;
                if(event.kind==D::LiveEventKind::Command && event.commandKind==D::LiveCommandKind::Draw)
                {
                    ++current.drawEvents; Check(event.arguments[0]==6 && event.arguments[1]>0,"Example draw arguments changed");
                    current.drawInstances+=uint64_t(event.arguments[1]);
                    if(current.commandList && (current.commandList!=event.command.commandList || current.generation!=event.command.generation)) current.details=false;
                    current.commandList=event.command.commandList; current.generation=event.command.generation;
                }
                if(event.kind==D::LiveEventKind::Submission && event.outcome==D::LiveOutcome::Submitted) current.submission=event.submission;
            }
            if(frame.key.frame) captured[Key(frame.key)]=current;
            const auto observe=[&](const auto& spans) { for(const auto& span:spans) ObserveGpu(span); };
            observe(source->flow.gpuSpans); observe(source->recentGpuSpans);
        }
        return true;
    }
    void ObserveGpu(const D::LiveGpuSpan& span)
    {
        if(span.frame.device!=control.device || span.frame.source!=control.source) return;
        const auto old=gpu.find(span.id);
        if(old!=gpu.end())
        {
            Check(old->second.frame==span.frame && old->second.revision==span.appliedRevision,"GPU result was relabelled to another original frame/revision");
            if(old->second.state==D::LiveValueState::Pending && span.duration.state==D::LiveValueState::Ready) ++pendingToReady;
        }
        gpu[span.id]={span.frame,span.appliedRevision,span.duration.state};
        if(span.duration.state==D::LiveValueState::Pending) ++pending;
        else if(span.duration.state==D::LiveValueState::Ready) ++ready;
        else if(span.duration.state==D::LiveValueState::Unsupported) ++unsupported;
        else ++unavailable;
        if(span.outcome!=D::LiveOutcome::Completed || span.aggregation!=D::LiveGpuAggregation::FrameCoverage) return;
        const auto found=captured.find(Key(span.frame)); if(found==captured.end()) return;
        const auto& observed=found->second;
        Check(span.appliedRevision==observed.frame.appliedRevision,"GPU and CPU revisions disagree for the original frame");
        if(!observed.details || !observed.commandList || !observed.submission || observed.frame.submitted.submissions!=1) return;
        Check(span.begin.command.commandList==observed.commandList && span.end.command.commandList==observed.commandList
            && span.begin.command.generation==observed.generation && span.end.command.generation==observed.generation
            && span.begin.submission==observed.submission && span.end.submission==observed.submission,"GPU completion endpoints do not match the recorded/submitted list generation");
        if(completed.insert(Key(span.frame)).second)
        {
            ++fenceJoins; report<<"gpu_completed_original_frame="<<span.frame.frame<<",revision="<<span.appliedRevision
                <<",sample="<<span.id<<",submission="<<observed.submission<<",timestamp_state="<<int(span.duration.state)<<'\n';
        }
        if(span.duration.state==D::LiveValueState::Ready)
        {
            Check(std::isfinite(span.duration.milliseconds) && span.duration.milliseconds>=0 && span.hasQueueInterval
                && span.gpuClockDomain && span.endGpuNs>=span.beginGpuNs,"Ready GPU timing lacks a valid actual clock/interval");
            if(timedCompleted.insert(Key(span.frame)).second)
            {
                ++readyJoins; report<<"gpu_ready_original_frame="<<span.frame.frame<<",revision="<<span.appliedRevision<<",sample="<<span.id
                    <<",duration_ms="<<span.duration.milliseconds<<",clock="<<span.gpuClockDomain<<'\n';
            }
        }
    }
    void Validate(const Captured& current,bool enabled)
    {
        const auto& frame=current.frame;
        const uint64_t draws=options.example==14 && !enabled ? 256 : 1;
        Check(frame.recorded.draw==draws && frame.recorded.drawIndexed==0 && frame.recorded.instances==256,"Recorded Draw/instances did not follow the actually applied value");
        Check(frame.submitted.draw==draws && frame.submitted.instances==256,"Submitted work did not match recorded Draw/instances");
        Check(frame.recorded.batches==draws && current.batchEvents==draws && current.batchElements==256,"Actual batch events/count/elements did not follow the applied value");
        Check(current.drawEvents==draws && current.drawInstances==256,"Logical command arguments disagree with frame counters");
        if(options.example==15) Check(frame.recorded.uploadBytes==8192 && current.uploadBytes==8192,"Example15 must still upload the actual 8192 instance bytes per steady frame in either mode");
    }
    void Exercise()
    {
        bool requested=initial;
        for(uint32_t change=0;change<options.cycles*2;++change)
        {
            requested=!requested; const auto* source=Source(); Check(source,"Target source missing");
            const auto previousRevision=source->latest.appliedRevision;
            const auto request=client.RequestControl(control.id,requested); Check(request,"Control request was not queued");
            captured.clear(); completed.clear(); timedCompleted.clear();
            const auto deadline=Clock::now()+std::chrono::seconds(options.timeout);
            std::optional<D::ControlStatus> applied;
            std::set<FrameKey> verified;
            std::vector<uint64_t> bufferTrend;
            while(Clock::now()<deadline)
            {
                if(process.Pid()) Check(process.Running(),"Owned target exited before all control phases were verified");
                if(Receive())
                {
                    for(const auto& status:snapshot->controlStatus) if(status.request==request && status.control==control.id)
                    {
                        if(status.state==D::ControlState::Rejected || status.state==D::ControlState::Failed || status.state==D::ControlState::Cancelled)
                            throw std::runtime_error("Control failed: "+status.message);
                        if(status.state==D::ControlState::Applied)
                        {
                            Check(std::holds_alternative<bool>(status.actual) && std::get<bool>(status.actual)==requested,"Actual control value differs from request");
                            Check(status.appliedFrame.device==control.device && status.appliedFrame.source==control.source && status.appliedFrame.frame
                                && status.appliedRevision && status.appliedRevision!=previousRevision,"Applied frame/revision did not identify a real changed epoch");
                            applied=status;
                        }
                    }
                    if(applied) for(const auto& key:timedCompleted)
                    {
                        const auto& current=captured.at(key);
                        if(current.frame.appliedRevision!=applied->appliedRevision || current.frame.endNs<applied->appliedNs+600000000) continue;
                        if(!verified.insert(key).second) continue;
                        Validate(current,requested);
                        if(options.example==15) { Check(current.buffersKnown,"Buffer creation counters missing"); bufferTrend.push_back(current.buffersCreated); }
                        report<<"verified_frame="<<current.frame.key.frame<<",revision="<<current.frame.appliedRevision<<",value="<<requested
                            <<",draw="<<current.frame.recorded.draw<<",instances="<<current.frame.recorded.instances<<",batches="<<current.batchEvents
                            <<",upload="<<current.frame.recorded.uploadBytes<<",buffers_created="<<current.buffersCreated<<'\n';
                    }
                    if(verified.size()>=3)
                    {
                        if(options.example==15) Check(requested ? bufferTrend.front()==bufferTrend.back() : bufferTrend.back()>bufferTrend.front(),
                            "Steady reuse must stop buffer creation; create must keep creating actual buffers");
                        report<<"phase=PASS,request="<<request<<",value="<<requested<<",revision="<<applied->appliedRevision<<",gpu_completed_observed_frames="<<verified.size()<<'\n';
                        std::printf("PASS example%d change%u value=%d revision=%llu completed_frames=%zu\n",options.example,change+1,requested,applied->appliedRevision,verified.size());
                        break;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            Check(verified.size()>=3,"Timed out waiting for three intact, original-frame Ready GPU timing samples; Unsupported/Unavailable completion is not a timing pass");
        }
    }
    void Restore()
    {
        if(!haveControl || client.GetState()!=D::LiveClientState::Connected) return;
        try
        {
            const auto request=client.RequestControl(control.id,initial); if(!request) return;
            const auto deadline=Clock::now()+std::chrono::seconds(2);
            while(Clock::now()<deadline)
            {
                Receive();
                if(snapshot) for(const auto& status:snapshot->controlStatus) if(status.request==request && status.state==D::ControlState::Applied)
                { report<<"failure_restore=applied\n"; return; }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            report<<"failure_restore=unconfirmed\n";
        }
        catch(...) { report<<"failure_restore=unconfirmed\n"; }
    }
    bool WaitForExit()
    {
        if(!process.Pid()) { report<<"external_process=left_running\n"; return true; }
        const auto deadline=Clock::now()+std::chrono::seconds(options.exitTimeout);
        bool disconnectReported=false;
        while(process.Running() && Clock::now()<deadline)
        {
            try { Receive(); }
            catch(const std::exception& error) { if(!disconnectReported) { report<<"exit_wait_ipc="<<error.what()<<'\n'; disconnectReported=true; } }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        const auto code=process.ExitCode();
        if(!code) { report<<"natural_exit=timeout; finite target left running; no forced termination\n"; std::fprintf(stderr,"FAIL finite target natural exit timed out\n"); return false; }
        report<<"natural_exit_code="<<*code<<'\n';
        native_metrics::Report(report,"target",targetMeter.Finished());
        return *code==0;
    }
};
int wmain(int argc,wchar_t** argv)
{
    try
    {
        D::SetLiveCollectorEnabled(false);
        auto options=Parse(argc,argv); if(options.selfCheck) { SelfCheck(); return 0; }
        return Run(std::move(options)).Execute();
    }
    catch(const std::exception& error) { std::fprintf(stderr,"FAIL native driver: %s\n",error.what()); return 1; }
}
