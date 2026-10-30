#include "EnvironmentDepthCapture.h"
#include "dyf/Camera.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/ICommandList.h"
#include "dyf/RHI/Pipeline.h"
#include "dyf/RHI/Shader.h"
#include "dyf/RHI/Texture.h"
#include "dyf/RHI/Readback.h"
#include "dyf/RHI/RenderGraph.h"
#include "../RHI/ResourceSet.h"
#include <cmath>
#include <cstring>

#if defined(ENABLE_METAL) // 컴파일 시 Metal용 깊이 변환 라이브러리를 포함한다.
#include "EnvironmentDepthLibrary.h"
#elif defined(ENABLE_VULKAN) || defined(ENABLE_D3D12) // 컴파일 시 Vulkan/D3D12용 전체 화면 정점 셰이더와 깊이 변환 픽셀 셰이더를 포함한다.
#include "ToneMapVertex.h"
#include "EnvironmentDepthFragment.h"
#endif

namespace dyf
{
// 바인딩·파이프라인·셰이더·소유 출력만 파괴한다. 외부 입력인 m_depth는 파괴하지 않는다.
EnvironmentDepthCapture::~EnvironmentDepthCapture()
{
    m_device.DestroyResourceSet(m_resources);
    m_device.DestroyPipeline(m_pipeline);
    m_device.DestroyShader(m_vertex);m_device.DestroyShader(m_fragment);
    m_device.DestroyTexture(m_target);
}

// 변환 파이프라인은 재사용하고, 출력 크기 또는 입력 핸들이 바뀐 부분만 갱신한다.
bool EnvironmentDepthCapture::Prepare(RHI::TextureHandle depth)
{
    using namespace RHI;
    if(!depth) return false; // 읽을 깊이 텍스처가 없으면 자원 구성을 시작하지 않는다.
    const auto& size=depth->GetDesc();
    if(!m_pipeline) // 처음 사용하거나 이전 생성이 실패한 경우에만 파이프라인을 준비한다.
    {
#if defined(ENABLE_METAL) // Metal 라이브러리 안의 정점·프래그먼트 진입점 이름을 사용한다.
        if(!m_vertex) m_vertex=m_device.CreateShader({ShaderStage::Vertex,"environmentDepthVertex",kEnvironmentDepthLibrary,kEnvironmentDepthLibrarySize}); // 아직 없는 정점 셰이더만 생성한다.
        if(!m_fragment) m_fragment=m_device.CreateShader({ShaderStage::Fragment,"environmentDepthFragment",kEnvironmentDepthLibrary,kEnvironmentDepthLibrarySize}); // 아직 없는 거리 변환 프래그먼트 셰이더만 생성한다.
#elif defined(ENABLE_VULKAN) || defined(ENABLE_D3D12) // Vulkan/D3D12에는 각각 main 진입점의 생성된 셰이더 바이트를 사용한다.
        if(!m_vertex) m_vertex=m_device.CreateShader({ShaderStage::Vertex,"main",kToneMapVertex,kToneMapVertexSize}); // 아직 없는 정점 셰이더만 생성한다.
        if(!m_fragment) m_fragment=m_device.CreateShader({ShaderStage::Fragment,"main",kEnvironmentDepthFragment,kEnvironmentDepthFragmentSize}); // 아직 없는 거리 변환 프래그먼트 셰이더만 생성한다.
#endif
        if(!m_vertex || !m_fragment) return false; // 필수 셰이더가 하나라도 없으면 파이프라인을 만들 수 없다.
        SamplerDesc sampler;
        // 깊이 샘플을 선형 혼합하지 않고 해당 픽셀 값으로 읽으며, 텍스처 경계 밖은 가장자리로 제한한다.
        sampler.minFilter=sampler.magFilter=sampler.mipFilter=SamplerFilter::Nearest;
        sampler.addressU=sampler.addressV=sampler.addressW=SamplerAddressMode::ClampToEdge;
        sampler.mipLodBias=0;
        sampler.minLod=sampler.maxLod=0;
        const ResourceBindingLayout bindings[]={
            {0,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}},
            {1,ResourceBindingType::StaticSampler,1,ShaderStageFlags::Fragment,sampler}};
        // RGBA8는 표시용 색 버퍼가 아니라 float32를 네 바이트로 옮기는 전송용 그릇이다.
        const ColorAttachmentDesc color{Format::R8G8B8A8_UNORM,{},ColorWriteMask::All};
        GraphicsPipelineDesc desc;
        desc.vertexShader=m_vertex;desc.fragmentShader=m_fragment;
        desc.topology=PrimitiveTopology::TriangleList;
        desc.raster={FillMode::Solid,CullMode::None,FrontFace::CounterClockwise,0,0,0};
        desc.colorAttachments=&color;desc.colorAttachmentCount=1;
        desc.layout={bindings,2,16,ShaderStageFlags::Fragment,15};
        m_pipeline=m_device.CreateGraphicsPipeline(desc);
        if(!m_pipeline) return false; // 장치가 파이프라인을 생성하지 못하면 이번 읽기를 중단한다.
    }
    if(!m_target || m_target->GetDesc().width!=size.width || m_target->GetDesc().height!=size.height) // 출력 대상이 없거나 입력과 가로·세로가 다를 때만 새 대상을 할당한다.
    {
        TextureDesc desc;
        desc.width=size.width;desc.height=size.height;desc.format=Format::R8G8B8A8_UNORM;
        desc.usage=TextureUsage::RenderTarget|TextureUsage::ShaderResource;
        const auto target=m_device.CreateTexture(desc);
        if(!target) return false; // 새 할당 실패 시 기존 대상을 파괴하지 않고 실패를 반환한다.
        m_device.DestroyTexture(m_target);m_target=target;m_state=ResourceState::Undefined;
    }
    if(m_depth!=depth) // 입력 핸들이 바뀌면 이전 깊이를 가리키던 바인딩을 폐기한다.
    {
        m_device.DestroyResourceSet(m_resources);m_resources=nullptr;m_depth=depth;
    }
    if(!m_resources) // 현재 입력에 대한 바인딩이 없을 때만 다시 만든다.
    {
        const ResourceBinding binding{0,0,nullptr,depth,0,0,{}};
        m_resources=m_device.CreateResourceSet({m_pipeline,&binding,1});
    }
    return m_resources!=nullptr;
}

// 카메라 투영과 깊이 범위를 확인하고 변환 명령 제출 → 텍스처 회수 → float 복원의 순서로 처리한다.
bool EnvironmentDepthCapture::Read(RHI::TextureHandle depth,const Camera& camera,float nearPlane,float farPlane,
    std::vector<float>& distance)
{
    using namespace RHI;
    if(!std::isfinite(nearPlane) || !std::isfinite(farPlane) || nearPlane<=0 || farPlane<=nearPlane || // near/far가 유한하고 0<near<far여야 하며, 투영의 가로·세로 배율도 유한한 0 아닌 값이어야 한다. 하나라도 어기면 distance를 건드리기 전에 false를 반환한다.
        !std::isfinite(camera.projection.m[0]) || !std::isfinite(camera.projection.m[5]) ||
        camera.projection.m[0]==0 || camera.projection.m[5]==0) return false;
    if(!Prepare(depth)) return false; // 출력·파이프라인·바인딩 준비가 실패하면 더 진행하지 않는다.
    // 원근 깊이를 직접 거리로 바꿔 먼 평면의 동차 나눗셈과 큰 월드 좌표의 상쇄를 피한다.
    // 투영의 가로·세로 배율 역수와 near/far를 셰이더에 넘긴다. 깊이에서 시점 축 거리를 복원한 뒤 픽셀 광선 길이를 곱해 방사 거리로 바꾼다.
    // 깊이 1인 배경도 무한대가 아니라 far 평면의 거리로 표현되므로 상위 캡처가 거리 제곱의 저장 범위를 미리 검사한다.
    const Math::float4 constants{1/camera.projection.m[0],1/camera.projection.m[5],nearPlane,farPlane};
    static_assert(sizeof(constants)==16);
    RenderGraph graph;
    const auto input=graph.ImportTexture("Probe source depth",depth,ResourceState::ShaderResource,ResourceState::ShaderResource);
    const auto output=graph.ImportTexture("Probe distance bits",m_target,m_state,ResourceState::ShaderResource);
    if(!input.IsValid() || !output.IsValid()) return false; // 그래프에 입력이나 출력 자원을 연결하지 못하면 패스를 등록하지 않는다.
    // 깊이를 읽고 RGBA8 출력에 쓰는 의존성을 그래프에 선언한다. SetExecute 안에서는 화면 전체를 덮는 정점 3개만 그린다.
    graph.AddPass("Probe radial distance capture",RGRecordingMode::Independent)
        .Read(input,ResourceState::ShaderResource).Write(output,ResourceState::RenderTarget).SetPipeline(m_pipeline)
        .SetExecute([this,constants](ICommandList* commands) {
            const auto& size=m_target->GetDesc();
            ColorAttachment color;color.texture=m_target;color.loadOp=LoadOp::Discard;color.storeOp=StoreOp::Store;
            commands->BeginRendering({&color,1,nullptr});
            commands->BindGraphicsPipeline(m_pipeline);commands->BindResourceSet(m_resources);
            commands->SetViewport({0,0,float(size.width),float(size.height),0,1});
            commands->SetScissor({0,0,size.width,size.height});
            commands->SetInlineConstants(0,sizeof(constants),&constants);
            commands->DrawInstanced(3,1,0,0);commands->EndRendering();
        });
    if(!graph.Compile()) return false; // 자원 전환·패스 구성을 확정하지 못하면 명령 목록을 제출하지 않는다.
    ResourceScope resources(m_device);
    auto* commands=resources.Keep(m_device.AcquireCommandList());
    if(!commands || !graph.Execute(commands) || !commands->Close() || !m_device.Submit(&commands,1)) return false; // 목록 획득·실행 기록·종료·제출 중 하나라도 실패하면 읽기 결과를 성공으로 취급하지 않는다.
    // 성공적으로 제출한 출력의 상태를 다음 호출용으로 기억한다. 이 대입 자체가 GPU 완료 판정은 아니다.
    m_state=ResourceState::ShaderResource;
    TextureReadback readback;
    if(!m_device.ReadTexture(m_target,readback)) return false; // CPU에서 읽을 픽셀을 얻지 못하면 바이트 복원을 수행하지 않는다.
    const bool bgra=readback.format==Format::B8G8R8A8_UNORM;
    const auto& size=m_target->GetDesc();
    if((readback.format!=Format::R8G8B8A8_UNORM && !bgra) || readback.width!=size.width || // 형식·크기·행 간격·마지막 행 범위를 확인해 읽기 배열 밖 접근을 막는다.
        readback.height!=size.height || readback.rowPitch<uint64_t(size.width)*4 ||
        uint64_t(size.height-1)*readback.rowPitch+uint64_t(size.width)*4>readback.pixels.size()) return false;
    // rowPitch가 width*4보다 클 수 있어 각 행의 실제 시작 주소를 사용한다. BGRA라면 R/B 위치를 되돌려 같은 32비트 패턴을 만든다.
    distance.resize(size_t(size.width)*size.height);
    for(uint32_t y=0;y<size.height;++y) for(uint32_t x=0;x<size.width;++x)
    {
        const auto* pixel=readback.pixels.data()+size_t(y)*readback.rowPitch+size_t(x)*4;
        // RGBA8는 색이 아니라 float32 비트를 보존한다. 노출·감마와 무관한 실제 거리다.
        const uint32_t bits=uint32_t(pixel[bgra?2:0])|(uint32_t(pixel[1])<<8)|
            (uint32_t(pixel[bgra?0:2])<<16)|(uint32_t(pixel[3])<<24);
        auto& value=distance[size_t(y)*size.width+x];
        // 정수값을 float로 산술 변환하는 것이 아니라 동일한 4바이트 비트를 float로 해석한다.
        std::memcpy(&value,&bits,sizeof(value));
        if(!std::isfinite(value) || value<0) return false; // 유한한 0 이상 거리만 허용한다. 실패가 나면 호출부는 이 배열을 완성 결과로 사용하지 않는다.
    }
    return true;
}
}
