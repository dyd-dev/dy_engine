#include "RendererInternal.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/ICommandList.h"
#include "dyf/RHI/Texture.h"
#include "dyf/RHI/Pipeline.h"
#include "../RHI/ResourceSet.h"
#include "dyf/RHI/Shader.h"
#include "OutputColor.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
namespace dyf
{
// HDR과 톤 매핑은 별도 소유 클래스 없이 RHI 텍스처와 전체 화면 draw로 구성한다.
// 출력 해상도에 hdrRenderScale을 곱해 올림한 크기로 float HDR 타깃을 확보한다. 장치 한도를 넘으면 실패한다.
bool Renderer::Impl::PreparePostProcess(RHI::TextureHandle output)
{
    using namespace RHI;
    const auto width=static_cast<uint64_t>(std::ceil(output->GetDesc().width*static_cast<double>(config.hdrRenderScale)));
    const auto height=static_cast<uint64_t>(std::ceil(output->GetDesc().height*static_cast<double>(config.hdrRenderScale)));
    // RHI의 2D 텍스처 한도와 32비트 크기 표현 범위 중 작은 값으로 제한한다.
    const auto limit=std::min<uint64_t>(device->GetLimit(Limit::Texture2DDimension),std::numeric_limits<uint32_t>::max());
    if(width>limit || height>limit) // 확대한 HDR 해상도가 장치 한도를 넘으면 오류를 출력하고 실패한다.
    {
        std::fprintf(stderr,"dyf: HDR render scale %.2f requests %llu x %llu, exceeding texture dimension limit %llu.\n",
            config.hdrRenderScale,static_cast<unsigned long long>(width),static_cast<unsigned long long>(height),
            static_cast<unsigned long long>(limit));
        return false;
    }
    if(tonePipeline && toneColorFormat!=output->GetDesc().format) // 출력 형식이 바뀌면 이전 세트와 파이프라인을 반납한다.
    {device->DestroyResourceSet(toneSet);toneSet=nullptr;device->DestroyPipeline(tonePipeline);tonePipeline=nullptr;}
    // 두 출력 경로 모두 HDR 타깃을 쓰며, 기본 톤 파이프라인은 자체 최종 패스가 있는 출력 확장이 없고 아직 생성되지 않았을 때만 만든다.
    if(!HasOutputExtension() && !tonePipeline)
    {
        const auto shaders=DefaultShaders();
        if(!toneVertexShader) toneVertexShader=device->CreateShader(ShaderDescription(ToneVertex,shaders.toneMapVertex)); // 기본 톤 정점 셰이더가 없으면 생성한다.
        if(!toneFragmentShader) toneFragmentShader=device->CreateShader(ShaderDescription(ToneFragment,shaders.toneMapFragment)); // 기본 톤 픽셀 셰이더가 없으면 생성한다.
        if(!toneVertexShader || !toneFragmentShader)return false; // 셰이더 생성이 하나라도 실패하면 준비를 중단한다.
        SamplerDesc sampler;
        sampler.minFilter=sampler.magFilter=SamplerFilter::Linear;
        sampler.mipFilter=SamplerFilter::Nearest;
        sampler.addressU=sampler.addressV=sampler.addressW=SamplerAddressMode::ClampToEdge;
        sampler.mipLodBias=sampler.minLod=sampler.maxLod=0;
        const std::array<ResourceBindingLayout,2> bindings={{{0,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}},
            {1,ResourceBindingType::StaticSampler,1,ShaderStageFlags::Fragment,sampler}}};
        const ColorAttachmentDesc color{output->GetDesc().format,{},ColorWriteMask::All};
        GraphicsPipelineDesc desc;
        desc.vertexShader=toneVertexShader;desc.fragmentShader=toneFragmentShader;desc.topology=PrimitiveTopology::TriangleList;
        desc.raster={FillMode::Solid,CullMode::None,FrontFace::CounterClockwise,0,0,0};
        desc.colorAttachments=&color;desc.colorAttachmentCount=1;
        // HDR 샘플 텍스처·정적 샘플러와 총 96바이트 픽셀 상수를 연결한다.
        desc.layout={bindings.data(),static_cast<uint32_t>(bindings.size()),96,ShaderStageFlags::Fragment,15};
        tonePipeline=device->CreateGraphicsPipeline(desc);
        if(!tonePipeline)return false; // 파이프라인 생성 실패를 상위 경로에 전달한다.
        toneColorFormat=output->GetDesc().format;
    }
    if(hdrTarget && (hdrTarget->GetDesc().width!=width || hdrTarget->GetDesc().height!=height)) // HDR 크기가 달라지면 이전 입력 세트와 HDR 타깃을 반납한다.
    {device->DestroyResourceSet(toneSet);toneSet=nullptr;device->DestroyTexture(hdrTarget);hdrTarget=nullptr;}
    if(!hdrTarget) // 타깃이 없으면 새 크기로 만들고 상태를 Undefined부터 시작한다.
    {
        TextureDesc desc;
        desc.width=static_cast<uint32_t>(width);desc.height=static_cast<uint32_t>(height);
        // 화면에 바로 표시할 범위를 넘는 광량을 저장하도록 채널별 16비트 float를 사용한다.
        desc.format=Format::R16G16B16A16_FLOAT;
        desc.usage=TextureUsage::RenderTarget|TextureUsage::ShaderResource;
        hdrTarget=device->CreateTexture(desc);
        hdrState=ResourceState::Undefined;
    }
    return hdrTarget!=nullptr;
}
// 명시한 source가 있으면 그것을, 없으면 내부 HDR 타깃을 샘플링해 output 전체를 덮는다.
bool Renderer::Impl::RecordToneMap(RHI::ICommandList& commands,RHI::TextureHandle output,float exposure,RHI::TextureHandle source)
{
    using namespace RHI;
    // 진행 중인 제출이 기존 리소스 세트를 참조할 수 있으므로 그 내용을 수정하지 않는다. 파이프라인이나 입력 텍스처가 달라지면 기존 세트를 RHI에 반납하고 새 세트를 만든다.
    ResourceBinding image;image.binding=0;image.texture=source ? source : hdrTarget;
    if(toneSet && (toneSet->GetPipeline()!=tonePipeline || toneSet->GetBindings()[0].texture!=image.texture))
    {device->DestroyResourceSet(toneSet);toneSet=nullptr;}
    if(!toneSet)toneSet=device->CreateResourceSet({tonePipeline,&image,1}); // 현재 입력용 resource set이 없으면 생성한다.
    if(!toneSet)return false; // 세트 생성이 실패하면 그리기 기록을 시작하지 않는다.
    ColorAttachment color; color.texture=output;color.loadOp=LoadOp::Discard;color.storeOp=StoreOp::Store;
    commands.BeginRendering({&color,1,nullptr});
    commands.BindGraphicsPipeline(tonePipeline);
    commands.BindResourceSet(toneSet);
    commands.SetViewport({0,0,static_cast<float>(output->GetDesc().width),static_cast<float>(output->GetDesc().height),0,1});
    commands.SetScissor({0,0,output->GetDesc().width,output->GetDesc().height});
    // 앞의 16바이트는 노출·출력 감마 적용 여부·RGBE 캡처 여부·LOD 진단의 톤 매핑 우회 여부다. sRGB 타깃은 하드웨어 인코딩을 쓰고, 그 외 일반 출력은 셰이더에서 1/2.2 거듭제곱을 적용한다.
    const float settings[4]={exposure,IsSrgbFormat(output->GetDesc().format)?0.f:1.f,m_environmentCapture?1.f:0.f,lodDebugActive?1.f:0.f};
    commands.SetInlineConstants(0,sizeof(settings),settings);
    // 뒤의 80바이트에는 색 보정과 선명도 상수를 넣는다. 캡처나 LOD 진단에서는 해당 보정을 비활성화한다.
    const auto grading=OutputConstants(config.postProcess,
        output->GetDesc().width,output->GetDesc().height,m_environmentCapture || lodDebugActive);
    commands.SetInlineConstants(sizeof(settings),sizeof(grading),grading.data());
    // 정점 버퍼 없이 정점 ID로 만드는 전체 화면 삼각형 한 개를 기록한다.
    commands.DrawInstanced(3,1,0,0);
    commands.EndRendering();
    return true;
}
}
