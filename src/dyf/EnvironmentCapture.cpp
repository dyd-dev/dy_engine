#include "RendererInternal.h"
#include "EnvironmentDepthCapture.h"
#include "dyf/Camera.h"
#include "dyf/RHI/Readback.h"
#include "dyf/RHI/Texture.h"
#include "dyf/RHI/IDevice.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <exception>
#include <limits>
#include <utility>

namespace dyf
{
// 실시간 프레임 중간에 끼어드는 호출이 아니라 별도의 캡처 호출이다. 장치·셰이더·범위·출력 용량을 확인한 뒤 렌더러 설정을 잠시 바꾼다.
bool Renderer::Impl::CaptureEnvironment(const Scene& scene,Math::float3 position,uint32_t faceSize,
    float nearPlane,float farPlane,HdrImage& result,bool includeEmitters,HdrImage* visibility)
{
    // 실패 원인을 표준 오류에 남기고 false를 반환하는 지역 도우미이며, 외부 출력의 교체는 캡처와 설정 복원이 모두 성공한 뒤 수행한다.
    const auto fail=[](const char* message) {
        std::fprintf(stderr,"dyf: Environment capture: %s\n",message);return false;
    };
    if(!device || m_environmentCapture || framePending) // 장치가 없거나 캡처 재진입 중이거나 윈도우 프레임을 이미 획득했으면 실행을 거부한다.
        return fail("requires an idle renderer invocation with no acquired window frame.");
    if(visibility==&result) return fail("radiance and visibility outputs must be distinct."); // 두 결과가 같은 객체면 최종 이동 대입이 서로 덮어쓰므로 거부한다.
    if(!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z) || // 캡처 좌표·near/far의 유한성, 양수 near, far>near, 0이 아닌 면 크기를 요구한다.
        !std::isfinite(nearPlane) || !std::isfinite(farPlane) || nearPlane<=0 || farPlane<=nearPlane || !faceSize)
        return fail("invalid position, face size or clipping planes.");
    if(visibility && double(farPlane)*farPlane*3>=std::numeric_limits<float>::max()) // 정육면체 모서리 방향의 최대 거리 제곱 3*far²가 float 범위를 넘으면 거리 모멘트 출력을 거부한다.
        return fail("visibility distance moments exceed float32 storage.");
    if(shadersPending || !UsesStockGeometry() || !shaderSources.bytes[ToneVertex].empty() || // 캡처용 RGBE 및 깊이 규약이 유지되도록 기본 기하/그림자/톤 셰이더만 허용한다.
        !shaderSources.bytes[ToneFragment].empty())
        return fail("requires stock geometry, shadow and tone-map shaders.");
    const auto limit=device->GetLimit(RHI::Limit::Texture2DDimension);
    if(faceSize>UINT32_MAX/4 || uint64_t(faceSize)*4>limit) // 4배 폭 파노라마의 정수 오버플로와 장치 텍스처 크기 한도를 함께 검사한다.
        return fail("face or output panorama dimensions exceed the device texture limit.");
    const uint64_t facePixels=uint64_t(faceSize)*faceSize;
    // 여섯 면의 색·4×2 배율 RGBA 파노라마·읽기 자료·선택 거리 자료를 합쳐 CPU 크기 곱셈과 할당 전 오버플로를 검사한다.
    // 품질은 호출자가 정하고 임의 RAM 상한은 두지 않으므로, 크기 검사를 통과해도 실제 할당은 실패할 수 있다.
    const size_t bytesPerFacePixel=6*sizeof(Math::float3)+8*4*sizeof(float)+4+
        (visibility ? 6*sizeof(float)+8*4*sizeof(float) : 0);
    if(facePixels>std::numeric_limits<size_t>::max()/bytesPerFacePixel) // 픽셀 수와 저장량의 곱이 size_t에 들어가지 않으면 메모리를 할당하기 전에 중단한다.
        return fail("dimensions overflow CPU image storage.");

    try
    {
        // 원래 설정 사본을 먼저 확보한다. 캡처 중 메모리가 부족해져도 복원용 설정 자체를 새로 할당할 필요가 없게 한다.
        auto original=std::make_unique<RendererConfig>(GetConfig());
        auto capture=*original;
        // 노출·톤 매핑 전 HDR 값을 캡처하도록 고급 경로를 끄고 HDR 배율을 1로 맞춘다. HUD·주변광·기존 프로브의 재조명을 끄며, clear 색은 검정으로 두되 활성 환경 하늘은 유지한다.
        capture.advanced.enabled=false;
        capture.enableHdrRendering=true;capture.hdrRenderScale=1;
        capture.clearColor={0,0,0,0};capture.enableProfilerHud=false;
        capture.lighting.ambientColor={0,0,0};capture.lighting.ambientIntensity=0;
        // 캡처한 프로브를 다시 캡처하지 않는다. 하늘은 바깥 장면의 원본 환경을 유지한다.
        capture.lighting.environment.probes.clear();
        capture.lighting.environment.diffuse=false;
        capture.lighting.environment.specular=false;
        if(!SetConfig(capture)) return false; // 캡처 설정을 요청하지 못하면 렌더링을 시작하지 않는다.
        bool restored=false;
        HdrImage captured,capturedVisibility;
        {
            // 이 범위를 벗어나는 모든 경로에서 캡처 플래그를 해제하고 원래 설정을 pendingConfig로 돌려놓는 RAII 복원 객체다.
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
                    // 소멸자에서 복원을 시도하되 예외는 바깥으로 내보내지 않는다. 적용 실패 시 원래 요청 설정은 pending 상태로 남는다.
                    try {restored=renderer.ApplySettings();}
                    catch(...) {restored=false;}
                    if(!restored) std::fprintf(stderr, // 설정 복원 실패를 알리고 성공 결과를 공개하지 않게 restored=false를 유지한다.
                        "dyf: Environment capture: settings restoration failed; original settings remain pending.\n");
                }
            } restore{*this,std::move(original),restored,m_environmentCaptureEmitters};
            m_environmentCapture=true;m_environmentCaptureEmitters=includeEmitters;
            m_environmentCaptureVisibility=visibility!=nullptr;
            // 한 면 크기를 기준으로 가로 4배·세로 2배 파노라마를 만든다. 여섯 축 방향마다 90도 시야의 카메라를 사용한다.
            const uint32_t width=faceSize*4,height=faceSize*2;
            const std::array<Math::float3,6> directions={{{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}}};
            std::array<Math::float4x4,6> projections;
            std::array<std::vector<Math::float3>,6> faces;
            std::array<std::vector<float>,6> distanceFaces;
            // 가림 출력이 요청된 경우에만 깊이→거리 변환 객체를 만든다. 색상만 필요하면 거리 자원과 회수를 생략한다.
            auto distanceCapture=visibility ? std::make_unique<EnvironmentDepthCapture>(*device) : nullptr;
            // 임시 색상 대상은 ResourceScope에 맡겨 정상 종료와 조기 반환 모두에서 정리되게 한다.
            RHI::ResourceScope resources(*device);
            RHI::TextureDesc targetDesc;
            targetDesc.width=targetDesc.height=faceSize;
            targetDesc.format=RHI::Format::R8G8B8A8_UNORM;
            targetDesc.usage=RHI::TextureUsage::RenderTarget|RHI::TextureUsage::ShaderResource;
            const auto target=resources.Keep(device->CreateTexture(targetDesc));
            if(!target) return fail("capture target allocation failed."); // 임시 렌더 대상 생성 실패 시 복원 객체를 거쳐 캡처를 종료한다.
            auto before=RHI::ResourceState::Undefined;
            // 동일한 대상에 여섯 면을 차례로 그리고 매번 CPU로 회수한다. 첫 사용만 Undefined이며 이후에는 직전 ShaderResource 상태를 넘긴다.
            for(uint32_t face=0;face<6;++face)
            {
                Camera camera;
                if(!camera.LookAt(position,position+directions[face],face>=4?Math::float3{0,1,0}:Math::float3{0,0,1}) || // 각 방향과 평행하지 않은 up축으로 카메라를 만들고, 시점 또는 90도 투영 설정 실패 시 중단한다.
                    !camera.SetPerspective(1,1.5707963267948966f,nearPlane,farPlane)) return false;
                projections[face]=camera.projection*camera.view;
                if(!RenderToTexture(scene,camera,target,before)) return false; // 해당 면의 렌더링 실패를 불완전한 파노라마로 숨기지 않는다.
                before=RHI::ResourceState::ShaderResource;
                if(distanceCapture && !distanceCapture->Read(depthStencilTarget,camera,nearPlane,farPlane,distanceFaces[face])) // 가림 출력 요청 시 같은 면에서 이미 생성된 깊이를 거리로 읽는다. 장면의 여섯 면을 다시 그리지는 않는다.
                    return fail("probe distance capture failed.");
                RHI::TextureReadback readback;
                if(!device->ReadTexture(target,readback)) return fail("RGBE readback failed."); // 색상 회수 실패 시 남은 면의 처리와 출력 생성을 중단한다.
                const bool bgra=readback.format==RHI::Format::B8G8R8A8_UNORM;
                const uint64_t rowBytes=uint64_t(faceSize)*4;
                if((readback.format!=RHI::Format::R8G8B8A8_UNORM && !bgra) || // RGBA/BGRA 형식, 예상 면 크기, 행 간격과 픽셀 범위를 확인한 뒤에만 CPU 배열을 읽는다.
                    readback.width!=faceSize || readback.height!=faceSize || readback.rowPitch<rowBytes ||
                    uint64_t(faceSize-1)*readback.rowPitch+rowBytes>readback.pixels.size())
                    return fail("invalid linear RGBE readback layout.");
                auto& pixels=faces[face];pixels.resize(static_cast<size_t>(facePixels));
                for(uint32_t y=0;y<faceSize;++y)for(uint32_t x=0;x<faceSize;++x)
                {
                    const auto* rgbe=readback.pixels.data()+size_t(y)*readback.rowPitch+size_t(x)*4;
                    // RGBE의 A 바이트는 불투명도가 아니라 공통 지수다. 지수 0은 검정이며 나머지는 2^(E-128)/255 배율로 선형 HDR RGB를 복원한다.
                    const float scale=rgbe[3]?std::ldexp(1.0f,int(rgbe[3])-128)/255.0f:0.0f;
                    pixels[size_t(y)*faceSize+x]={rgbe[bgra?2:0]*scale,rgbe[1]*scale,rgbe[bgra?0:2]*scale};
                }
            }

            std::vector<float> panorama(static_cast<size_t>(facePixels)*8*4);
            std::vector<float> visibilityPixels;
            if(visibility) visibilityPixels.resize(panorama.size()); // 거리 출력이 있을 때만 색상과 같은 크기의 RGBA float 저장 공간을 추가로 마련한다.
            constexpr double pi=3.14159265358979323846;
            for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x)
            {
                // 각 파노라마 픽셀 중심의 위도/경도에서 Z축을 극으로 하는 단위 방향을 구한다. 가장 큰 절댓값 성분이 그 방향을 담는 큐브 면을 정한다.
                const double theta=pi*(double(y)+.5)/height,phi=2*pi*((double(x)+.5)/width-.5);
                const Math::float3 direction{float(std::sin(theta)*std::cos(phi)),float(std::sin(theta)*std::sin(phi)),float(std::cos(theta))};
                const auto magnitude=Math::float3{std::fabs(direction.x),std::fabs(direction.y),std::fabs(direction.z)};
                uint32_t face;
                if(magnitude.x>=magnitude.y && magnitude.x>=magnitude.z) face=direction.x>=0?0:1; // X 성분이 가장 크면 부호에 따라 +X 또는 -X 면을 고른다.
                else if(magnitude.y>=magnitude.z) face=direction.y>=0?2:3; // X 면이 아니면서 Y가 Z 이상이면 +Y/-Y 면을 고른다.
                else face=direction.z>=0?4:5;
                // 백엔드별 큐브 면 배치를 가정하지 않고 실제 면 카메라로 방향을 투영해 up/right축과 화면 Y 규약을 유지하며, 위치 평행이동은 적용하지 않는다.
                const auto& projection=projections[face];
                const auto clip=Math::TransformVector(projection,direction);
                const float w=projection.m[3]*direction.x+projection.m[7]*direction.y+projection.m[11]*direction.z;
                // 동차 나눗셈으로 NDC를 구하고 픽셀 중심 좌표로 옮긴다. 네 이웃을 면 경계로 clamp한 뒤 양선형 보간한다.
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
                if(visibility) // 색과 같은 방향·보간 가중치로 거리 모멘트도 합친다.
                {
                    // E[d]와 E[d²]를 각각 보간해야 E[d²]-E[d]²로 경계의 분산을 얻을 수 있다. 평균 거리를 먼저 제곱하면 그 정보가 사라진다.
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
            if(visibility) capturedVisibility=HdrImage(width,height,std::move(visibilityPixels)); // 거리 출력이 요청된 경우에만 임시 HdrImage를 완성한다.
        }
        if(!restored) return false; // 여섯 면 생성에 성공했더라도 원래 설정 복원에 실패하면 결과를 호출자에게 넘기지 않는다.
        // 캡처와 설정 복원 모두 끝난 뒤 큰 픽셀 배열을 복사하지 않고 결과 객체로 이동한다.
        result=std::move(captured);
        if(visibility) *visibility=std::move(capturedVisibility); // 선택 출력도 같은 성공 경로에서만 교체한다.
        return true;
    }
    // 할당 실패 등을 포함한 표준 예외를 메시지와 false로 바꾼다. 복원 객체가 만들어진 범위의 예외는 먼저 설정 복원을 거친다.
    catch(const std::exception& error)
    {
        std::fprintf(stderr,"dyf: Environment capture: %s\n",error.what());return false;
    }
}
}
