#include "RendererInternal.h"
#include "dyf/RenderExtension.h"
#include "dyf/Camera.h"
#include "dyf/Scene.h"
#include "BasicEnvironmentResources.h"
#include "BasicLightingResources.h"
#include "GpuVisibility.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dyf
{
bool Renderer::Impl::UsesAdvancedRendering() const
{
    return config.advanced.enabled && config.enableHdrRendering && UsesStockGeometry();
}

bool Renderer::Impl::NeedsSurfacePass() const
{
    return CollectRequirements().needsSurface;
}

void Renderer::Impl::InvalidateFrameHistory(bool release)
{
    frameHistoryValid=false;frameAmbientOcclusion=nullptr;
    for(auto& extension:extensions) extension->InvalidateHistory(release);
    if(release)
    {
        gpuVisibility.reset();visibilityActive=false;visibilityKeys.clear();visibilityScene.reset();
        basicLighting.reset();basicEnvironment.reset();frameLighting={};
        historyScene.reset();previousModels.clear();pendingModels.clear();frameIndex=0;
    }
}

bool Renderer::Impl::PrepareFrameResources(RenderFrame& frame,RHI::ICommandList& uploads)
{
    const auto& scene=frame.scene;
    auto& camera=frame.camera;
    const bool active=UsesAdvancedRendering();
    const auto& target=frame.color.texture->GetDesc();
    if(active)
    {
        const std::weak_ptr<Scene> owner=scene.m_lifetime;
        bool cut=!frameHistoryValid || historyScene.expired() || historyScene.owner_before(owner) ||
            owner.owner_before(historyScene) || previousModels.size()!=scene.GetEntityCount();
        cut=cut || Math::LengthSquared(camera.position-previousCameraPosition)>16.0f;
        float facing=0;
        for(uint32_t i:{2u,6u,10u}) facing+=camera.view.m[i]*previousCameraView.m[i];
        cut=cut || facing<.8f;
        for(uint32_t i:{0u,5u,10u,14u})
            cut=cut || std::fabs(camera.projection.m[i]-previousCameraProjection.m[i])>.05f;
        if(cut) {InvalidateFrameHistory();visibilityKeys.clear();}
        pendingCameraView=camera.view;pendingCameraProjection=camera.projection;
        pendingModels.resize(scene.GetEntityCount());
        for(uint32_t i=0;i<scene.GetEntityCount();++i)
            pendingModels[i]=scene.GetTransform(static_cast<EntityID>(i));
    }
    else InvalidateFrameHistory();
    frame.history={previousCameraView,previousCameraProjection,frameIndex,frameHistoryValid};
    for(auto& extension:extensions)
        if(!extension->PrepareCamera(frame)) return false;
    if(!PrepareVisibility(scene,camera,uploads,target.width,target.height,frame.draws)) return false;
    if(!PrepareCpuBatches(scene,camera,uploads,frame.draws,frame.requirements.needsGeometry)) return false;
    if(shaderSources.bytes[MeshFragment].empty())
    {
        if(!frame.requirements.providesEnvironment)
        {
            if(!basicEnvironment) basicEnvironment=std::make_unique<BasicEnvironmentResources>();
            if(!basicEnvironment->Prepare(*device,uploads)) return false;
            frame.lighting.environment=basicEnvironment->GetResources(defaultMaterialTextures[2]);
        }
        if(!frame.requirements.providesLocalLights)
        {
            if(!basicLighting) basicLighting=std::make_unique<BasicLightingResources>();
            if(!basicLighting->Prepare(*device,uploads,scene,camera,
                config.lighting.enabled && config.lighting.direct && !lodDebugActive)) return false;
            frame.lighting.clusterLights.buffer=basicLighting->GetLights();
            frame.lighting.clusterList.buffer=basicLighting->GetList();
            frame.lighting.clusterConstants.buffer=basicLighting->GetConstants();
        }
    }
    UpdateFrameLighting(frame);
    PrepareRenderGeometry(frame);
    for(auto stage:{RenderPreparation::Lighting,RenderPreparation::Effects})
        for(auto& extension:extensions)
            if(extension->GetPreparationStage()==stage && !extension->Prepare(frame,uploads))
            {
                std::fprintf(stderr,"dyf: Render module %s preparation failed.\n",extension->GetName());
                return false;
            }
    frameAmbientOcclusion=frame.ao.texture;
    frameLighting=frame.lighting;
    return true;
}

void Renderer::Impl::CommitFrameResources(const RenderFrame& frame)
{
    for(auto& extension:extensions) extension->CommitFrame(frame);
    if(!UsesAdvancedRendering()) return;
    previousModels.swap(pendingModels);
    previousViewProjection=frame.camera.projection*frame.camera.view;
    previousCameraView=pendingCameraView;previousCameraProjection=pendingCameraProjection;
    previousCameraPosition=frame.camera.position;historyScene=frame.scene.m_lifetime;
    frameHistoryValid=true;++frameIndex;
}
}
