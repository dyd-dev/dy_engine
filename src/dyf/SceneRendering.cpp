#include "../Platform/Profiler/CollectorInternal.h"
#include "RendererInternal.h"
#include "OutputPresentation.h"
#include "dyf/RHI/ICommandList.h"
#include "RendererDiagnostics.h"
#include "BasicEnvironmentResources.h"
#include "BasicLightingResources.h"
#include "GpuVisibility.h"
#include "dyf/Canvas.h"
#include "dyf/Camera.h"
#include "dyf/Scene.h"
#include "dyf/Platform/Profiler.h"
#include "dyf/RHI/Readback.h"
#include "dyf/RHI/IDevice.h"
#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace dyf;

bool Renderer::Impl::RenderToTexture(const Scene& scene,const Camera& camera,RHI::TextureHandle target,RHI::ResourceState before,
    const std::vector<RendererDrawDesc>* draws)
{
    lastStatistics={};
    if(!target) return RendererFailure("RenderToTexture requires a target.");
    const auto& desc=target->GetDesc();
    const auto usage=RHI::TextureUsage::RenderTarget|RHI::TextureUsage::ShaderResource;
    if(!desc.width || !desc.height || desc.depthOrArraySize!=1 || desc.mipLevels!=1 ||
        (desc.usage&usage)!=usage || !RHI::IsReadbackFormat(desc.format))
        return RendererFailure("RenderToTexture requires a single-mip 2D RGBA/BGRA8 renderable sampled texture.");
    if(before!=RHI::ResourceState::Undefined && before!=RHI::ResourceState::ShaderResource &&
        before!=RHI::ResourceState::RenderTarget)
        return RendererFailure("RenderToTexture before state must be Undefined, ShaderResource or RenderTarget.");
    return RenderScene(scene,&camera,nullptr,nullptr,draws,target,before);
}

bool Renderer::Impl::RenderScene(const Scene& scene, const Camera* selectedCamera, const Canvas* overlay, Image* readback,
    const std::vector<RendererDrawDesc>* draws,RHI::TextureHandle selectedOutput,RHI::ResourceState before)
{
    InitializeLiveProfiling();PublishLiveGeometryAvailability(draws!=nullptr);
    Platform::LiveFrameScope liveFrame(Platform::FindLiveObject(device),liveId);
    device->PrepareLiveControls(liveFrame.GetKey());
    Platform::SetLiveObjectGauge(liveFrame.GetKey(),{Platform::LiveValueState::Ready,scene.GetEntityCount()});
    struct LivePending {Impl& r;Platform::LiveFrameKey key;~LivePending(){r.m_profiler.SubmitGpuScopes({},key);}} livePending{*this,liveFrame.GetKey()};
    try
    {
        struct ShadowFrame {
            Impl& renderer;
            bool committed=false;
            ~ShadowFrame() {
                if(!committed) {
                    renderer.shadowAtlasCache.valid=false;
                    renderer.shadowAtlasCache.pending=false;
                }
            }
        } shadowFrame{*this};
        lastStatistics={};
        DY_PROFILE_CPU_ZONE_NAMED("Renderer::Impl::RenderScene");
        if(!PrepareLiveFrame(liveFrame.GetKey()))return false;
        if(draws && draws->size() != scene.GetEntityCount()) return RendererFailure("Draw input count must match the scene.");
        const bool stockDraws=!draws || std::all_of(draws->begin(),draws->end(),[](const RendererDrawDesc& draw) {
            return draw.stockGeometry && draw.vertexResources.empty() && draw.inlineConstants.empty();
        });
        stockDrawInputs=stockDraws && UsesStockGeometry();
        if(config.advanced.enabled && config.enableHdrRendering && (!stockDraws || !UsesStockGeometry()))
            return RendererFailure("Advanced raster passes require stock-compatible geometry with matching surface and motion inputs.");
        const auto requirements=CollectRequirements();
        if(!ValidateModules(requirements)) return false;
        if(draws) for(const auto& draw : *draws)
            if(draw.inlineConstants.size() != shaderSources.additionalConstantBytes)
                return RendererFailure("Draw constants do not match the selected shader layout.");
        Platform::CpuTimer acquireTimer;
        Platform::ProfilerTimingSample timing;
        if(readback && !config.allowReadback) return RendererFailure("Renderer readback was not enabled.");
        RHI::IDevice* nativeDevice = device;
        if(!selectedOutput && !nativeDevice->BeginFrame())
        {
            m_profiler.AddAcquireWait(acquireTimer.ElapsedMilliseconds());
            if(nativeDevice->IsLost())return RendererFailure("RHI device was lost.");
            if(readback)*readback={};
            // No commands or atlas writes occurred on an ordinary acquire skip.
            shadowFrame.committed=true;
            livePreparedFrame=0;liveFrame.SetOutcome(Platform::LiveOutcome::Skipped);
            return true;
        }
        if(!selectedOutput) framePending=true;
        FrameEnd frameEnd{*nativeDevice,framePending};
        timing.cpuAcquireMilliseconds=selectedOutput?0.0:m_profiler.TakeAcquireWait()+acquireTimer.ElapsedMilliseconds();
        Platform::CpuTimer prepareTimer;
        if(!m_environmentCapture)m_profiler.PollGpuSamples();
        auto* windowOutput=selectedOutput ? nullptr : nativeDevice->GetBackBuffer();
        auto* output=selectedOutput ? selectedOutput : windowOutput;
        const bool fixedOutput=!selectedOutput && config.renderWidth && config.renderHeight;
        if(fixedOutput)
        {
            if(!outputPresentation) outputPresentation=std::make_unique<OutputPresentation>(*nativeDevice);
            if(!outputPresentation->Prepare(config.renderWidth,config.renderHeight,windowOutput->GetDesc().format))
                return RendererFailure("Fixed render output preparation failed.");
            output=outputPresentation->GetTarget();
        }
        const auto outputBefore=fixedOutput ? outputPresentation->GetState() : before;
        const auto after=selectedOutput || fixedOutput ? RHI::ResourceState::ShaderResource : RHI::ResourceState::Present;
        Camera defaultCamera;
        const auto& targetDesc = output->GetDesc();
        if(!selectedCamera) defaultCamera.SetPerspective(targetDesc.width / static_cast<float>(targetDesc.height));
        Camera camera = selectedCamera ? *selectedCamera : defaultCamera;
        if(!PrepareGeometry(scene, nativeDevice)) return RendererFailure("Mesh upload failed.");
        ShadowData shadows;
        BuildShadows(shadows,scene,camera,draws);
        if(config.enableHdrRendering && !PreparePostProcess(output))
            return RendererFailure("HDR pass creation failed.");
        if(!InitializeMesh(output,selectedOutput!=nullptr)) return RendererFailure("Mesh pipeline creation failed.");
        const auto* lodTarget=config.enableHdrRendering ? hdrTarget : output;
        SelectLods(scene,camera,lodTarget->GetDesc().width,lodTarget->GetDesc().height,draws);


        if(!SyncTextures(scene, nativeDevice)) return RendererFailure("Scene texture upload failed.");
        materialStates.resize(scene.Materials().size());
        UpdateMaterialStates(scene);
        auto* sceneTarget=config.enableHdrRendering ? hdrTarget : output;
        const bool advanced=UsesAdvancedRendering();
        const bool captureSky=m_environmentCapture && config.lighting.environment.background &&
            config.lighting.environment.radiance.IsValid();
        // 생성 용도와 그래프의 경계 상태는 같은 깊이 읽기 요구를 사용한다.
        const bool sampledDepth=advanced || captureSky || m_environmentCaptureVisibility;
        if(!EnsureDepthStencilTarget(nativeDevice,sceneTarget,sampledDepth) ||
            !EnsureShadowDepthTarget(nativeDevice,shadows.columns,shadows.rows,shadows.resolution))
            return RendererFailure("Scene depth target creation failed.");
        RenderFrame renderFrame{owner,scene,camera};
        renderFrame.requirements=requirements;
        renderFrame.lodDebug=lodDebugActive;renderFrame.wireframe=wireframeActive;
        renderFrame.blackTexture=defaultMaterialTextures[2];
        renderFrame.environmentCapture=m_environmentCapture;
        renderFrame.draws=draws;renderFrame.output.texture=output;
        renderFrame.color.texture=config.enableHdrRendering ? hdrTarget : output;
        renderFrame.depth.texture=depthStencilTarget;renderFrame.shadow.texture=shadowDepthTarget;
        struct FrameResources {
            Impl& renderer;
            bool committed=false;
            ~FrameResources() {if(!committed) renderer.InvalidateFrameHistory(true);}
        } frameResources{*this};
        struct GpuSamples {
            RHI::IDevice& device;
            Platform::FrameProfiler::GpuSample packet;
            ~GpuSamples() {device.DestroyTimestampQuery(packet.query);device.DestroyTimestampQuery(packet.start);}
        } samples{*nativeDevice,{}};
        if(!m_environmentCapture) samples.packet.start=m_profiler.AcquireQuery(1);

        {
            // Replace only size-mismatched buffers. Roll back ownership and committed
            // states if any upload/close/submit step fails (including exceptions).
            struct FrameBuffers {
                Impl& r;
                RHI::BufferHandle light,shadow,instances,cpuInstances;
                bool lightReady,shadowReady,instancesReady,cpuInstancesReady,committed=false;
                explicit FrameBuffers(Impl& owner):r(owner),light(r.lightingBuffer),shadow(r.shadowMatrixBuffer),instances(r.instanceBuffer),
                    cpuInstances(r.cpuInstanceBuffer),lightReady(r.lightingReady),shadowReady(r.shadowReady),
                    instancesReady(r.instanceReady),cpuInstancesReady(r.cpuInstanceReady) {}
                ~FrameBuffers() {
                    const auto finish=[&](RHI::BufferHandle& current,RHI::BufferHandle previous,bool& ready,bool oldReady) {
                        if(committed) {if(current!=previous)r.device->DestroyBuffer(previous);}
                        else {if(current!=previous)r.device->DestroyBuffer(current);current=previous;ready=oldReady;}
                    };
                    finish(r.lightingBuffer,light,r.lightingReady,lightReady);
                    finish(r.shadowMatrixBuffer,shadow,r.shadowReady,shadowReady);
                    finish(r.instanceBuffer,instances,r.instanceReady,instancesReady);
                    finish(r.cpuInstanceBuffer,cpuInstances,r.cpuInstanceReady,cpuInstancesReady);
                }
            } buffers(*this);
            RHI::ResourceScope resources(*nativeDevice);
            auto* frameDataCommand=nativeDevice->AcquireCommandList();
            if(!frameDataCommand) return RendererFailure("Frame command list acquisition failed.");
            resources.Keep(frameDataCommand);
            if(samples.packet.start) {
                frameDataCommand->ResetTimestamps(samples.packet.start,0,1);
                frameDataCommand->WriteTimestamp(samples.packet.start,0);
            }
            if(!PrepareFrameResources(renderFrame,*frameDataCommand))
                return RendererFailure("Frame resource preparation failed.");
            if(!UpdateShadowBuffer(nativeDevice,*frameDataCommand,shadows))
                return RendererFailure("Shadow constants upload recording failed.");
            if(!UpdateLightingBuffer(scene,camera,nativeDevice,*frameDataCommand))
                return RendererFailure("Lighting constants upload recording failed.");
            if(!UpdateInstanceBuffer(scene,*frameDataCommand))
                return RendererFailure("Instance transforms upload recording failed.");
            if(!frameDataCommand->Close())
                return RendererFailure("Frame upload command list close failed.");
            if(!nativeDevice->PrepareCommandLists(&frameDataCommand,1,1))
                return RendererFailure("Frame upload command list native preparation failed.");
            Platform::CpuTimer uploadSubmitTimer;
            const bool submitted=nativeDevice->Submit(&frameDataCommand,1);
            timing.cpuSubmitMilliseconds+=uploadSubmitTimer.ElapsedMilliseconds();
            if(!submitted) return RendererFailure("Frame upload submission failed.");
            lightingReady=true;shadowReady=shadowMatrixBuffer!=nullptr;instanceReady=instanceBuffer!=nullptr;
            cpuInstanceReady=cpuInstanceBuffer!=nullptr;
            if(visibilityActive) gpuVisibility->CommitUploads();
            if(basicEnvironment) basicEnvironment->CommitUploads();
            CommitFrameData();
            buffers.committed=true;
        }

        CanvasFrame canvasFrame(*this);
        recordingStatistics={};materialIndexRecorded=false;
        const auto frameWindow=std::max(1u,device->GetDesc().maxFramesInFlight);
        for(auto& set:mainSets)if(set.unusedFrames<frameWindow)++set.unusedFrames;
        for(auto& set:shadowSets)if(set.unusedFrames<frameWindow)++set.unusedFrames;
        struct CacheFrame {
            Impl& owner;
            bool empty;
            ~CacheFrame() {
                if(empty) {
                    for(auto& set:owner.mainSets)set.unusedFrames=UINT32_MAX;
                    for(auto& set:owner.shadowSets)set.unusedFrames=UINT32_MAX;
                }
                owner.PruneResourceSets(owner.mainSets);owner.PruneResourceSets(owner.shadowSets);
            }
        } cacheFrame{*this,scene.GetEntityCount()==0};
        const bool hasShadows = shadowPipeline && shadows.viewCount && shadowDepthTarget &&
            shadowMatrixBuffer && shadowDepthTarget->GetDesc().width;
        const bool cachedShadows=hasShadows && ReuseShadowAtlas(scene,shadows,draws);
        const bool drawShadows=hasShadows && !cachedShadows;
        using State = RHI::ResourceState;
        RHI::RenderGraph graph;
        const auto outputHandle = graph.ImportTexture("Output", output, outputBefore, after);
        if(!outputHandle.IsValid()) return RendererFailure("Output resource import failed.");
        auto* overlayTarget=fixedOutput ? windowOutput : output;
        const auto overlayHandle=fixedOutput
            ? graph.ImportTexture("Window output",windowOutput,State::Present,State::Present) : outputHandle;
        if(!overlayHandle.IsValid()) return RendererFailure("Window output resource import failed.");
        const bool surfaceNeeded=requirements.needsSurface;
        recordingStatistics.renderWidth=sceneTarget->GetDesc().width;
        recordingStatistics.renderHeight=sceneTarget->GetDesc().height;
        recordingStatistics.outputWidth=overlayTarget->GetDesc().width;
        recordingStatistics.outputHeight=overlayTarget->GetDesc().height;
        recordingStatistics.surfacePrepass=surfaceNeeded;
        recordingStatistics.clusteredLighting=renderFrame.lighting.clustered;
        recordingStatistics.shadowViewCount=hasShadows ? shadows.viewCount : 0;
        recordingStatistics.shadowViewResolution=hasShadows ? shadows.resolution : 0;
        recordingStatistics.directionalShadowDistance=hasShadows ? shadows.directionalDistance : 0;
        recordingStatistics.reusedShadowAtlas=cachedShadows;
        const auto depth = graph.ImportTexture("Depth", depthStencilTarget, depthStencilState,
            sampledDepth ? State::ShaderResource : State::DepthWrite);
        if(!depth.IsValid()) return RendererFailure("Scene depth resource import failed.");
        const auto color = config.enableHdrRendering
            ? graph.ImportTexture("HDR", hdrTarget, hdrState, State::ShaderResource) : outputHandle;
        if(!color.IsValid()) return RendererFailure("Scene color resource import failed.");
        const auto shadow = shadowDepthTarget
            ? graph.ImportTexture("ShadowDepth", shadowDepthTarget, shadowDepthState, State::ShaderResource)
            : RHI::RGResourceHandle{};
        if(shadowDepthTarget && !shadow.IsValid()) return RendererFailure("Shadow depth resource import failed.");
        renderFrame.output.resource=outputHandle;renderFrame.color.resource=color;
        renderFrame.depth.resource=depth;renderFrame.shadow.resource=shadow;
        renderFrame.hasShadows=hasShadows;
        const auto require = [](bool success) {
            if(!success) throw std::runtime_error("Renderer pass recording failed.");
        };
        if(visibilityActive && !gpuVisibility->AddPasses(graph))
            return RendererFailure("GPU visibility pass creation failed.");
        if(cpuBatchesActive && !cpuDraws.empty())
        {
            renderFrame.visibleInstances={cpuInstanceBuffer,graph.ImportBuffer("CPU visible instances",cpuInstanceBuffer,
                State::VertexBuffer,State::VertexBuffer)};
            if(!renderFrame.visibleInstances.resource.IsValid())return RendererFailure("CPU instance import failed.");
        }
        if(!requirements.providesLocalLights && basicLighting && !basicLighting->Import(graph,renderFrame.lighting))
            return RendererFailure("Basic local light resource import failed.");
        if(drawShadows)
            graph.AddPass("Shadow", RHI::RGRecordingMode::Independent).Write(shadow,State::DepthWrite)
                .SetExecute([&](RHI::ICommandList* commands) {
                    require(RecordShadowPass(scene,camera,shadows,*commands,draws));
                });
        UpdateFrameLighting(renderFrame,true);
        if(!AddExtensionPasses(RenderStage::BeforeOpaque,renderFrame,graph)) return false;
        auto& environment=renderFrame.lighting.environment;
        if(!environment.diffuse.resource.IsValid() && environment.diffuse.texture)
            environment.diffuse.resource=graph.ImportTexture("Basic environment diffuse",environment.diffuse.texture,State::ShaderResource,State::ShaderResource);
        if(!environment.specular.resource.IsValid() && environment.specular.texture)
            environment.specular.resource=graph.ImportTexture("Basic environment specular",environment.specular.texture,State::ShaderResource,State::ShaderResource);
        frameAmbientOcclusion=renderFrame.ao.texture;
        if(surfaceNeeded)
        {
            if(!renderFrame.normal.resource.IsValid() || !renderFrame.albedo.resource.IsValid() || !renderFrame.ao.resource.IsValid())
                return RendererFailure("The surface extension did not provide normal, albedo and AO resources.");
            if(visibilityActive && config.enableOcclusionCulling)
            {
                if(!gpuVisibility->AddOcclusionPasses(graph,depthStencilTarget,depth,camera))
                    return RendererFailure("GPU occlusion pass creation failed.");
                recordingStatistics.gpuOcclusion=true;
            }
        }
        if(!AddExtensionPasses(RenderStage::OpaqueLighting,renderFrame,graph)) return false;
        auto& mainPass = graph.AddPass("MainForward", RHI::RGRecordingMode::Independent);
        mainPass.Write(color, State::RenderTarget).Write(depth, State::DepthWrite);
        if(renderFrame.lighting.areaMatrix.resource.IsValid())
            mainPass.Read(renderFrame.lighting.areaMatrix.resource,State::ShaderResource);
        if(renderFrame.lighting.areaAmplitude.resource.IsValid())
            mainPass.Read(renderFrame.lighting.areaAmplitude.resource,State::ShaderResource);
        if(renderFrame.lighting.clusterLights.resource.IsValid())
            mainPass.Read(renderFrame.lighting.clusterLights.resource,State::ShaderResource)
                .Read(renderFrame.lighting.clusterList.resource,State::ShaderResource)
                .Read(renderFrame.lighting.clusterConstants.resource,State::ConstantBuffer);
        if(visibilityActive)
            mainPass.Read(gpuVisibility->GetInstancesHandle(),State::VertexBuffer)
                .Read(gpuVisibility->GetArgumentsHandle(),State::IndirectArgument);
        else if(cpuBatchesActive && !cpuDraws.empty())
            mainPass.Read(renderFrame.visibleInstances.resource,State::VertexBuffer);
        if(shadowDepthTarget) mainPass.Read(shadow, State::ShaderResource);
        if(environment.diffuse.resource.IsValid()) mainPass.Read(environment.diffuse.resource,State::ShaderResource);
        if(environment.specular.resource.IsValid()) mainPass.Read(environment.specular.resource,State::ShaderResource);
        if(surfaceNeeded)
            mainPass.Read(renderFrame.ao.resource,State::ShaderResource);
        mainPass.SetExecute([&](RHI::ICommandList* commands) {
            require(RecordMainPass(scene, camera, *commands, draws, output));
        });
        for(auto stage:{RenderStage::Background,RenderStage::AfterOpaque,RenderStage::Atmosphere,
            RenderStage::Transparency,RenderStage::PostProcess})
            if(!AddExtensionPasses(stage,renderFrame,graph)) return false;
        if(config.enableHdrRendering && !renderFrame.outputWritten)
            graph.AddPass("ToneMap", RHI::RGRecordingMode::Independent).Read(renderFrame.color.resource,State::ShaderResource).Write(outputHandle,State::RenderTarget)
                .SetExecute([&,source=renderFrame.color.texture](RHI::ICommandList* commands) {
                    require(RecordToneMap(*commands,output,config.exposure,source));
                });
        if(fixedOutput && !outputPresentation->AddPass(graph,outputHandle,windowOutput))
            return RendererFailure("Fixed render presentation pass creation failed.");
        if(overlay)
            graph.AddPass("Overlay").Write(overlayHandle, State::RenderTarget)
                .SetExecute([&](RHI::ICommandList* commands) {
                    require(RecordCanvas(*overlay, *commands, overlayTarget, true, true));
                });
        if(!selectedOutput && config.enableProfilerHud)
            graph.AddPass("Profiler HUD").Write(overlayHandle, State::RenderTarget)
                .SetExecute([&](RHI::ICommandList* commands) {
#ifdef DY_RENDERDOC_ENABLED
                    commands->BeginDebugEvent("Profiler HUD");
#endif
                    auto hud = BuildProfilerOverlay(overlayTarget->GetDesc().width, overlayTarget->GetDesc().height,
                        config.profilerStartsExpanded);
                    require(RecordCanvas(hud, *commands, overlayTarget, true, true));
#ifdef DY_RENDERDOC_ENABLED
                    commands->EndDebugEvent();
#endif
                });
        if(!graph.Compile()) return RendererFailure("Scene RenderGraph compilation failed.");
        if(!m_environmentCapture)
        {
            samples.packet.passes=graph.GetExecutionOrderNames();
            samples.packet.query=m_profiler.AcquireQuery(2+2*static_cast<uint32_t>(samples.packet.passes.size()));
            graph.SetTimestampQuery(samples.packet.query);
        }
        struct FrameCommands
        {
            RHI::IDevice& device;
            std::vector<RHI::ICommandList*> lists;
            ~FrameCommands() { for(auto* commands : lists) device.DestroyCommandList(commands); }
        } frame{*nativeDevice, {}};
        timing.cpuPrepareMilliseconds=prepareTimer.ElapsedMilliseconds()-timing.cpuSubmitMilliseconds;
        Platform::CpuTimer recordTimer;
        bool drawn;
        if(config.enableParallelRenderGraph)
            drawn = graph.ExecuteParallel(nativeDevice, frame.lists, config.recordingThreadCount);
        else
        {
            frame.lists.reserve(1);
            auto* commands = nativeDevice->AcquireCommandList();
            frame.lists.push_back(commands);
            drawn = commands && graph.Execute(commands) && commands->Close();
            // Both modes include native translation in the recording stage, so
            // serial/parallel comparisons do not merely move work into Submit.
            drawn = drawn && nativeDevice->PrepareCommandLists(frame.lists.data(),1,1);
        }
        timing.cpuRecordMilliseconds=recordTimer.ElapsedMilliseconds();
        Platform::CpuTimer submitTimer;
        RHI::FenceHandle completion;
        drawn = drawn && nativeDevice->Submit({frame.lists.data(),
            static_cast<uint32_t>(frame.lists.size()), nullptr, 0}, completion);
        timing.cpuSubmitMilliseconds+=submitTimer.ElapsedMilliseconds();
        if(drawn) {
            m_profiler.SubmitGpuScopes(completion,liveFrame.GetKey());
            if(fixedOutput) outputPresentation->CommitFrame();
            canvasFrame.Commit();
            if(samples.packet.query && completion) {
                samples.packet.completion=completion;
                m_profiler.SubmitGpuSample(std::move(samples.packet));
                samples.packet={};
            }
            lastRenderCompletion=completion;
            lastStatistics=recordingStatistics;
            if(materialIndexRecorded) {materialIndexReady=true;materialIndexBytes.swap(materialIndexScratch);}
            CommitShadowAtlas(scene);
            CommitFrameResources(renderFrame);
            if(visibilityActive) gpuVisibility->CommitFrame();
            frameResources.committed=true;
        }
        if(!drawn)return RendererFailure("Scene draw submission failed.");
        if(config.enableHdrRendering) hdrState=RHI::ResourceState::ShaderResource;
        depthStencilState = sampledDepth ? RHI::ResourceState::ShaderResource : RHI::ResourceState::DepthWrite;
        shadowDepthState = shadowDepthTarget != nullptr
            ? RHI::ResourceState::ShaderResource
            : RHI::ResourceState::Undefined;
        Platform::CpuTimer readbackTimer;
        if(readback && !CaptureFrame(*readback))return false;
        if(readback) timing.cpuReadbackMilliseconds=readbackTimer.ElapsedMilliseconds();
        if(!selectedOutput)
        {
            Platform::CpuTimer presentTimer;
            const bool presented=nativeDevice->Present();
            timing.cpuPresentMilliseconds=presentTimer.ElapsedMilliseconds();
            framePending=false;
            if(!presented) return RendererFailure("Scene presentation failed.");
        }
        if(!m_environmentCapture)
        {
            timing.cpuRenderMilliseconds=timing.cpuPrepareMilliseconds+timing.cpuRecordMilliseconds+timing.cpuSubmitMilliseconds;
            RecordProfilerFrame(timing,scene.GetEntityCount());
            DY_PROFILE_FRAME_MARK();
        }
        shadowFrame.committed=true;
        liveFrame.SetOutcome(Platform::LiveOutcome::Completed);
        return true;
    }
    catch(const std::exception& error)
    {
        std::fprintf(stderr,"dyf: Render: %s\n",error.what());
        return false;
    }
}

