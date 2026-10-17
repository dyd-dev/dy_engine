#include "RendererInternal.h"
#include "EnvironmentDepthCapture.h"
#include "dyf/Camera.h"
#include "dyf/RHI/Readback.h"
#include "dyf/RHI/Texture.h"
#include "../RHI/ResourceScope.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <exception>
#include <limits>
#include <utility>

namespace dyf
{
bool Renderer::Impl::CaptureEnvironment(const Scene& scene,Math::float3 position,uint32_t faceSize,
    float nearPlane,float farPlane,HdrImage& result,bool includeEmitters,HdrImage* visibility)
{
    const auto fail=[](const char* message) {
        std::fprintf(stderr,"dyf: Environment capture: %s\n",message);return false;
    };
    if(!device || m_environmentCapture || framePending)
        return fail("requires an idle renderer invocation with no acquired window frame.");
    if(visibility==&result) return fail("radiance and visibility outputs must be distinct.");
    if(!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z) ||
        !std::isfinite(nearPlane) || !std::isfinite(farPlane) || nearPlane<=0 || farPlane<=nearPlane || !faceSize)
        return fail("invalid position, face size or clipping planes.");
    if(visibility && double(farPlane)*farPlane*3>=std::numeric_limits<float>::max())
        return fail("visibility distance moments exceed float32 storage.");
    if(shadersPending || !UsesStockGeometry() || !shaderSources.bytes[ToneVertex].empty() ||
        !shaderSources.bytes[ToneFragment].empty())
        return fail("requires stock geometry, shadow and tone-map shaders.");
    const auto limit=device->GetLimit(RHI::Limit::Texture2DDimension);
    if(faceSize>UINT32_MAX/4 || uint64_t(faceSize)*4>limit)
        return fail("face or output panorama dimensions exceed the device texture limit.");
    const uint64_t facePixels=uint64_t(faceSize)*faceSize;
    // Validate aggregate CPU image sizes before multiplication/allocation. There
    // is no arbitrary RAM cap: the caller chooses quality; allocation may fail.
    const size_t bytesPerFacePixel=6*sizeof(Math::float3)+8*4*sizeof(float)+4+
        (visibility ? 6*sizeof(float)+8*4*sizeof(float) : 0);
    if(facePixels>std::numeric_limits<size_t>::max()/bytesPerFacePixel)
        return fail("dimensions overflow CPU image storage.");

    try
    {
        auto original=std::make_unique<RendererConfig>(GetConfig());
        auto capture=*original;
        capture.advanced.enabled=false;
        capture.enableHdrRendering=true;capture.hdrRenderScale=1;
        capture.clearColor={0,0,0,0};capture.enableProfilerHud=false;
        capture.lighting.ambientColor={0,0,0};capture.lighting.ambientIntensity=0;
        // 캡처한 프로브를 다시 캡처하지 않는다. 하늘은 바깥 장면의 원본 환경을 유지한다.
        capture.lighting.environment.probes.clear();
        capture.lighting.environment.diffuse=false;
        capture.lighting.environment.specular=false;
        if(!SetConfig(capture)) return false;
        bool restored=false;
        HdrImage captured,capturedVisibility;
        {
            struct Restore
            {
                Impl& renderer;
                std::unique_ptr<RendererConfig> original;
                bool& restored;
                bool emitters;
                ~Restore()
                {
                    renderer.m_environmentCapture=false;
                    renderer.m_environmentCaptureVisibility=false;
                    renderer.m_environmentCaptureEmitters=emitters;
                    // Preallocated before any change, so restoring the requested
                    // settings cannot itself fail to allocate under memory pressure.
                    renderer.pendingConfig=std::move(original);
                    try {restored=renderer.ApplySettings();}
                    catch(...) {restored=false;}
                    if(!restored) std::fprintf(stderr,
                        "dyf: Environment capture: settings restoration failed; original settings remain pending.\n");
                }
            } restore{*this,std::move(original),restored,m_environmentCaptureEmitters};
            m_environmentCapture=true;m_environmentCaptureEmitters=includeEmitters;
            m_environmentCaptureVisibility=visibility!=nullptr;
            const uint32_t width=faceSize*4,height=faceSize*2;
            const std::array<Math::float3,6> directions={{{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}}};
            std::array<Math::float4x4,6> projections;
            std::array<std::vector<Math::float3>,6> faces;
            std::array<std::vector<float>,6> distanceFaces;
            auto distanceCapture=visibility ? std::make_unique<EnvironmentDepthCapture>(*device) : nullptr;
            RHI::ResourceScope resources(*device);
            RHI::TextureDesc targetDesc;
            targetDesc.width=targetDesc.height=faceSize;
            targetDesc.format=RHI::Format::R8G8B8A8_UNORM;
            targetDesc.usage=RHI::TextureUsage::RenderTarget|RHI::TextureUsage::ShaderResource;
            const auto target=resources.Keep(device->CreateTexture(targetDesc));
            if(!target) return fail("capture target allocation failed.");
            auto before=RHI::ResourceState::Undefined;
            for(uint32_t face=0;face<6;++face)
            {
                Camera camera;
                if(!camera.LookAt(position,position+directions[face],face>=4?Math::float3{0,1,0}:Math::float3{0,0,1}) ||
                    !camera.SetPerspective(1,1.5707963267948966f,nearPlane,farPlane)) return false;
                projections[face]=camera.projection*camera.view;
                if(!RenderToTexture(scene,camera,target,before)) return false;
                before=RHI::ResourceState::ShaderResource;
                if(distanceCapture && !distanceCapture->Read(depthStencilTarget,camera,nearPlane,farPlane,distanceFaces[face]))
                    return fail("probe distance capture failed.");
                RHI::TextureReadback readback;
                if(!device->ReadTexture(target,readback)) return fail("RGBE readback failed.");
                const bool bgra=readback.format==RHI::Format::B8G8R8A8_UNORM;
                const uint64_t rowBytes=uint64_t(faceSize)*4;
                if((readback.format!=RHI::Format::R8G8B8A8_UNORM && !bgra) ||
                    readback.width!=faceSize || readback.height!=faceSize || readback.rowPitch<rowBytes ||
                    uint64_t(faceSize-1)*readback.rowPitch+rowBytes>readback.pixels.size())
                    return fail("invalid linear RGBE readback layout.");
                auto& pixels=faces[face];pixels.resize(static_cast<size_t>(facePixels));
                for(uint32_t y=0;y<faceSize;++y)for(uint32_t x=0;x<faceSize;++x)
                {
                    const auto* rgbe=readback.pixels.data()+size_t(y)*readback.rowPitch+size_t(x)*4;
                    const float scale=rgbe[3]?std::ldexp(1.0f,int(rgbe[3])-128)/255.0f:0.0f;
                    pixels[size_t(y)*faceSize+x]={rgbe[bgra?2:0]*scale,rgbe[1]*scale,rgbe[bgra?0:2]*scale};
                }
            }

            std::vector<float> panorama(static_cast<size_t>(facePixels)*8*4);
            std::vector<float> visibilityPixels;
            if(visibility) visibilityPixels.resize(panorama.size());
            constexpr double pi=3.14159265358979323846;
            for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x)
            {
                const double theta=pi*(double(y)+.5)/height,phi=2*pi*((double(x)+.5)/width-.5);
                const Math::float3 direction{float(std::sin(theta)*std::cos(phi)),float(std::sin(theta)*std::sin(phi)),float(std::cos(theta))};
                const auto magnitude=Math::float3{std::fabs(direction.x),std::fabs(direction.y),std::fabs(direction.z)};
                uint32_t face;
                if(magnitude.x>=magnitude.y && magnitude.x>=magnitude.z) face=direction.x>=0?0:1;
                else if(magnitude.y>=magnitude.z) face=direction.y>=0?2:3;
                else face=direction.z>=0?4:5;
                // Project the world direction with the actual capture camera.
                // This retains its up/right conventions and framebuffer Y flip
                // instead of assuming a backend-specific cubemap face layout.
                const auto& projection=projections[face];
                const auto clip=Math::TransformVector(projection,direction);
                const float w=projection.m[3]*direction.x+projection.m[7]*direction.y+projection.m[11]*direction.z;
                const float px=(clip.x/w*.5f+.5f)*faceSize-.5f,py=(.5f-clip.y/w*.5f)*faceSize-.5f;
                const int x0=static_cast<int>(std::floor(px)),y0=static_cast<int>(std::floor(py));
                const float fx=px-x0,fy=py-y0;
                const auto pixel=[&](int ix,int iy) {
                    return faces[face][size_t(std::clamp(iy,0,int(faceSize)-1))*faceSize+std::clamp(ix,0,int(faceSize)-1)];
                };
                const auto radiance=(pixel(x0,y0)*(1-fx)+pixel(x0+1,y0)*fx)*(1-fy)+
                    (pixel(x0,y0+1)*(1-fx)+pixel(x0+1,y0+1)*fx)*fy;
                const size_t i=(size_t(y)*width+x)*4;
                panorama[i]=radiance.x;panorama[i+1]=radiance.y;panorama[i+2]=radiance.z;panorama[i+3]=1;
                if(visibility)
                {
                    // 깊이 경계에서도 평균 거리와 제곱 평균을 따로 보간해 가림 분산을 보존한다.
                    const auto moment=[&](int ix,int iy) {
                        const float distance=distanceFaces[face][size_t(std::clamp(iy,0,int(faceSize)-1))*faceSize+
                            std::clamp(ix,0,int(faceSize)-1)];
                        return Math::float3{distance,distance*distance,0};
                    };
                    const auto value=(moment(x0,y0)*(1-fx)+moment(x0+1,y0)*fx)*(1-fy)+
                        (moment(x0,y0+1)*(1-fx)+moment(x0+1,y0+1)*fx)*fy;
                    visibilityPixels[i]=value.x;visibilityPixels[i+1]=value.y;
                    visibilityPixels[i+2]=0;visibilityPixels[i+3]=1;
                }
            }
            captured=HdrImage(width,height,std::move(panorama));
            if(visibility) capturedVisibility=HdrImage(width,height,std::move(visibilityPixels));
        }
        if(!restored) return false;
        result=std::move(captured);
        if(visibility) *visibility=std::move(capturedVisibility);
        return true;
    }
    catch(const std::exception& error)
    {
        std::fprintf(stderr,"dyf: Environment capture: %s\n",error.what());return false;
    }
}
}
