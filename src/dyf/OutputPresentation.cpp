#include "OutputPresentation.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/ICommandList.h"
#include "dyf/RHI/Pipeline.h"
#include "dyf/RHI/Binding.h"
#include "dyf/RHI/Shader.h"
#include "dyf/RHI/Texture.h"
#include <algorithm>
#include <cmath>
#include <limits>

#if defined(ENABLE_METAL) // Metal 빌드에서는 복사용 metallib 헤더를 포함한다.
#include "PresentationCopyLibrary.h"
#elif defined(ENABLE_D3D12) || defined(ENABLE_VULKAN) // D3D12/Vulkan 빌드에서는 생성된 셰이더 바이트 헤더를 포함한다.
#include "ToneMapVertex.h"
#include "PresentationCopyFragment.h"
#endif

namespace dyf
{
// 소유한 세트부터 파이프라인·셰이더·타깃 순으로 RHI에 반납한다. 참조 중인 네이티브 자원 수명은 RHI가 관리한다.
OutputPresentation::~OutputPresentation()
{
    m_device.DestroyResourceSet(m_resources);
    m_device.DestroyPipeline(m_pipeline);
    m_device.DestroyShader(m_vertex);
    m_device.DestroyShader(m_fragment);
    m_device.DestroyTexture(m_target);
}

// 실제 백버퍼 형식에 맞는 기본 복사 파이프라인을 준비한다. 새 생성에 성공한 뒤 이전 파이프라인을 교체한다.
bool OutputPresentation::PreparePipeline(RHI::Format format)
{
    using namespace RHI;
    if(m_pipeline && m_format==format) return true; // 기존 파이프라인의 출력 형식이 같으면 재사용한다.
#if defined(ENABLE_METAL) // Metal 빌드에서 라이브러리 안의 지정 진입점으로 셰이더를 만든다.
    if(!m_vertex) m_vertex=m_device.CreateShader({ShaderStage::Vertex,"presentationVertex",kPresentationCopyLibrary,kPresentationCopyLibrarySize}); // 정점 셰이더가 없으면 생성한다.
    if(!m_fragment) m_fragment=m_device.CreateShader({ShaderStage::Fragment,"presentationFragment",kPresentationCopyLibrary,kPresentationCopyLibrarySize}); // 픽셀 셰이더가 없으면 생성한다.
#elif defined(ENABLE_D3D12) || defined(ENABLE_VULKAN) // D3D12/Vulkan 빌드에서 main 진입점을 가진 기본 셰이더를 만든다.
    if(!m_vertex) m_vertex=m_device.CreateShader({ShaderStage::Vertex,"main",kToneMapVertex,kToneMapVertexSize}); // 사용자 재정의와 무관한 내장 전체 화면 정점 셰이더를 아직 없을 때 생성한다.
    if(!m_fragment) m_fragment=m_device.CreateShader({ShaderStage::Fragment,"main",kPresentationCopyFragment,kPresentationCopyFragmentSize}); // 복사 픽셀 셰이더가 없으면 생성한다.
#endif
    if(!m_vertex || !m_fragment) return false; // 필요한 셰이더가 없으면 파이프라인 준비를 실패시킨다.

    SamplerDesc sampler;
    sampler.minFilter=sampler.magFilter=SamplerFilter::Linear;
    sampler.mipFilter=SamplerFilter::Nearest;
    sampler.addressU=sampler.addressV=sampler.addressW=SamplerAddressMode::ClampToEdge;
    sampler.mipLodBias=sampler.minLod=sampler.maxLod=0;
    // 완성 이미지 한 장과 선형 ClampToEdge 샘플러만 연결한다. 추가 상수나 깊이 타깃은 필요 없다.
    const ResourceBindingLayout bindings[]={
        {0,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}},
        {1,ResourceBindingType::StaticSampler,1,ShaderStageFlags::Fragment,sampler}};
    const ColorAttachmentDesc color{format,{},ColorWriteMask::All};
    GraphicsPipelineDesc desc;
    desc.vertexShader=m_vertex;
    desc.fragmentShader=m_fragment;
    desc.topology=PrimitiveTopology::TriangleList;
    desc.raster={FillMode::Solid,CullMode::None,FrontFace::CounterClockwise,0,0,0};
    desc.colorAttachments=&color;
    desc.colorAttachmentCount=1;
    desc.layout={bindings,2,0,ShaderStageFlags::None,0};
    const auto pipeline=m_device.CreateGraphicsPipeline(desc);
    if(!pipeline) return false; // 새 파이프라인 생성 실패 시 기존 소유자를 교체하지 않는다.

    // 이전 세트와 파이프라인의 소유권을 RHI에 반납한다. 이미 제출된 명령이 참조하는 네이티브 자원은 RHI가 필요한 실행 기간 동안 보존한다.
    m_device.DestroyResourceSet(m_resources);
    m_resources=nullptr;
    m_device.DestroyPipeline(m_pipeline);
    m_pipeline=pipeline;
    m_format=format;
    return true;
}

// 요청한 내부 출력 크기·형식으로 타깃과 샘플 세트를 준비한다. 매 호출의 패스 추가 여부를 먼저 지운다.
bool OutputPresentation::Prepare(uint32_t width,uint32_t height,RHI::Format format)
{
    using namespace RHI;
    m_pending=false;
    const auto limit=m_device.GetLimit(Limit::Texture2DDimension);
    if(!width || !height || width>limit || height>limit) return false; // 0 크기 또는 장치의 2D 크기 한도를 넘으면 거부한다.
    if(!PreparePipeline(format)) return false; // 대상 형식의 복사 파이프라인을 준비하지 못하면 실패한다.

    if(!m_target || m_target->GetDesc().width!=width || m_target->GetDesc().height!=height || m_target->GetDesc().format!=format) // 타깃이 없거나 크기·형식이 바뀌었으면 교체한다.
    {
        TextureDesc desc;
        desc.width=width;
        desc.height=height;
        desc.format=format;
        desc.usage=TextureUsage::RenderTarget|TextureUsage::ShaderResource;
        // 새 타깃부터 확보한 뒤 이전 타깃과 세트를 놓는다. 할당 실패만으로 기존 타깃을 먼저 잃지 않게 한다.
        const auto target=m_device.CreateTexture(desc);
        if(!target) return false; // 새 타깃을 만들지 못하면 기존 타깃을 유지하고 실패한다.
        m_device.DestroyResourceSet(m_resources);
        m_resources=nullptr;
        m_device.DestroyTexture(m_target);
        m_target=target;
        m_state=ResourceState::Undefined;
    }
    if(!m_resources) // 현재 파이프라인과 타깃을 묶은 세트가 없으면 만든다.
    {
        ResourceBinding image;
        image.binding=0;
        image.texture=m_target;
        m_resources=m_device.CreateResourceSet({m_pipeline,&image,1});
    }
    return m_resources!=nullptr;
}

// source는 Prepare로 만든 타깃을 그래프에 가져온 핸들이어야 한다. 실제 샘플 세트는 m_target을 읽고 source는 의존성을 선언한다.
bool OutputPresentation::AddPass(RHI::RenderGraph& graph,RHI::RGResourceHandle source,RHI::TextureHandle actualBackBuffer)
{
    using namespace RHI;
    if(!source.IsValid() || !actualBackBuffer || actualBackBuffer==m_target || !m_target || !m_pipeline || !m_resources) return false; // 유효한 입력과 준비된 자원이 없거나 입력/출력이 같은 타깃이면 패스를 등록하지 않는다.
    const auto& output=actualBackBuffer->GetDesc();
    const auto& input=m_target->GetDesc();
    if(!output.width || !output.height || output.format!=m_format || // 백버퍼의 크기·형식과 viewport 정수 표현 범위를 검사한다.
        output.width>std::numeric_limits<int32_t>::max() || output.height>std::numeric_limits<int32_t>::max()) return false;

    // 원본 종횡비를 유지하면서 창 안에 들어가는 가장 큰 표시 크기를 구한다.
    double width=output.width,height=output.height;
    if(uint64_t(output.width)*input.height<=uint64_t(output.height)*input.width) // 창이 상대적으로 좁으면 가로를 맞추고 세로 여백을 남긴다.
        height=double(output.width)*input.height/input.width;
    else
        width=double(output.height)*input.width/input.height;
    // 중앙 위치를 픽셀 경계에 내림 정렬해 원본 크기로 표시할 때 반 픽셀 이동에 따른 흐림을 막는다.
    const auto x=static_cast<int32_t>(std::floor((output.width-width)*.5));
    const auto y=static_cast<int32_t>(std::floor((output.height-height)*.5));
    const Viewport viewport{float(x),float(y),float(width),float(height),0,1};
    const Rect scissor{x,y,
        static_cast<uint32_t>(std::min(double(output.width-x),std::ceil(width))),
        static_cast<uint32_t>(std::min(double(output.height-y),std::ceil(height)))};
    // 실제 백버퍼는 Present로 시작하고 끝난다. 중간 RenderTarget 전이와 source 읽기 의존성은 그래프가 계산한다.
    const auto destination=graph.ImportTexture("Presentation backbuffer",actualBackBuffer,ResourceState::Present,ResourceState::Present);
    if(!destination.IsValid()) return false; // 백버퍼를 그래프에 가져오지 못하면 패스를 추가하지 않는다.
    const auto pipeline=m_pipeline;
    const auto resources=m_resources;
    // 자체 파이프라인과 상태를 갖는 독립 기록 패스로 등록하고 실행 시 필요한 핸들·영역을 값으로 캡처한다.
    graph.AddPass("Output presentation",RGRecordingMode::Independent)
        .Read(source,ResourceState::ShaderResource)
        .Write(destination,ResourceState::RenderTarget)
        .SetPipeline(pipeline)
        .SetExecute([actualBackBuffer,pipeline,resources,viewport,scissor](ICommandList* commands) {
            ColorAttachment color;
            color.texture=actualBackBuffer;
            color.loadOp=LoadOp::Clear;
            color.storeOp=StoreOp::Store;
            // 배경은 RGB 기본값 0, 알파 1인 불투명 검정으로 채운 뒤 중앙 영역만 그린다.
            color.clearColor[3]=1;
            commands->BeginRendering({&color,1,nullptr});
            commands->BindGraphicsPipeline(pipeline);
            commands->BindResourceSet(resources);
            commands->SetViewport(viewport);
            commands->SetScissor(scissor);
            commands->DrawInstanced(3,1,0,0);
            commands->EndRendering();
        });
    m_pending=true;
    return true;
}

// 호출자가 draw 제출 성공 뒤 이 함수를 호출하면 중간 타깃의 다음 시작 상태를 ShaderResource로 확정한다. 여기서 GPU 완료를 검사하거나 기다리지는 않는다.
void OutputPresentation::CommitFrame()
{
    if(m_pending) m_state=RHI::ResourceState::ShaderResource; // 이번 프레임에 표시 패스를 등록한 경우만 다음 상태를 갱신한다.
    m_pending=false;
}
}
