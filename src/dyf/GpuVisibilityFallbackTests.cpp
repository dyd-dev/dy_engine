#include "dyf/Renderer.h"
#include "dyf/Camera.h"
#include "dyf/Scene.h"
#include "dyf/RHI/Pipeline.h"
#include "Backends/Null/NullDevice.h"
#include <cstdio>
#include <stdexcept>

namespace
{
namespace RHI=dyf::RHI;

void Check(bool value,const char* message)
{
    if(!value)throw std::runtime_error(message);
}

class LimitedDevice final : public dyf::Backends::NullDevice
{
public:
    uint32_t storageLimit=4;
    bool compute=true;
    mutable uint32_t rejectedLayouts=0;

    LimitedDevice() {Check(Initialize(nullptr,{})==0,"Null device initialization failed");}

protected:
    bool SupportsNative(RHI::Feature feature) const override
    {
        if(feature==RHI::Feature::Compute)return compute;
        return NullDevice::SupportsNative(feature);
    }

    bool SupportsPipelineLayoutNative(const RHI::PipelineLayoutDesc& layout) const override
    {
        uint32_t storageCount=0;
        for(uint32_t i=0;i<layout.bindingCount;++i)
        {
            const auto& binding=layout.bindings[i];
            if((binding.stages&RHI::ShaderStageFlags::Compute)!=RHI::ShaderStageFlags::None &&
                (binding.type==RHI::ResourceBindingType::ReadOnlyStorageBuffer ||
                    binding.type==RHI::ResourceBindingType::ReadWriteStorageBuffer))
                storageCount+=binding.count;
        }
        if(storageCount>storageLimit) {++rejectedLayouts;return false;}
        return NullDevice::SupportsPipelineLayoutNative(layout);
    }

    bool CreateSwapchainNative(const RHI::SwapchainDesc& input) override
    {
        auto desc=input;desc.initialWidth=desc.initialHeight=32;
        return NullDevice::CreateSwapchainNative(desc);
    }
};

void RenderWithLimitedStorage(uint32_t limit,bool shadows,bool compute)
{
    LimitedDevice device;device.storageLimit=limit;device.compute=compute;
    dyf::RendererConfig config;
    config.enableProfilerHud=false;config.texturePreparationThreadCount=1;
    config.enableGpuCulling=true;config.enableInstancing=true;config.enableLod=false;
    config.lighting.shadows=shadows;config.lighting.shadowQuality=dyf::ShadowQuality::Low;
    config.lighting.reuseStaticShadows=false;
    auto renderer=dyf::Renderer::Create(device,reinterpret_cast<void*>(1),config);
    Check(bool(renderer),"Renderer initialization failed");
    dyf::Scene scene;
    Check(bool(scene.Add(dyf::CreateCubeMesh(.5f))),"Cube creation failed");
    dyf::DirectionalLight light;light.direction={0,0,1};light.castShadow=true;
    Check(bool(scene.Add(light)),"Light creation failed");
    dyf::Camera camera;
    Check(camera.LookAt({0,0,2},{0,0,0},{0,1,0}) && camera.SetOrthographic(2,2,.1f,10),"Camera creation failed");
    for(uint32_t frame=0;frame<3;++frame)
    {
        Check(renderer->Render(scene,camera),"Unsupported GPU visibility layout must use CPU rendering");
        const auto stats=renderer->GetLastRenderStatistics();
        Check(!stats.gpuVisibility && !stats.gpuShadowCulling,"Unsupported GPU path remained active");
        Check(stats.mainInstanceCount==1 && stats.mainTriangleCount>0,"CPU fallback lost the visible mesh");
        if(shadows)Check(stats.shadowDrawCount>0 && stats.shadowTriangleCount>0,"CPU fallback lost the shadow caster");
    }
    Check(!compute || device.rejectedLayouts>0,"Fixture did not exercise the storage-buffer limit");
    Check(device.WaitIdle(),"Device drain failed");
}
}

int main() try
{
    for(uint32_t limit:{4u,5u})for(bool shadows:{false,true})RenderWithLimitedStorage(limit,shadows,true);
    RenderWithLimitedStorage(4,true,false);
    std::puts("PASS GPU visibility fallback: four/five storage bindings and missing compute retain CPU meshes and shadows.");
    return 0;
}
catch(const std::exception& error)
{
    std::fprintf(stderr,"FAIL GPU visibility fallback: %s\n",error.what());return 1;
}
