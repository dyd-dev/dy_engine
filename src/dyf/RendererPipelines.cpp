#include "RendererInternal.h"
#include "dyf/RHI/Buffer.h"
#include "dyf/RHI/ICommandList.h"
#include "dyf/RHI/Pipeline.h"
#include "dyf/RHI/Texture.h"
#include "dyf/RHI/IDevice.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

using namespace dyf;

// 셰이더의 binding 번호·종류·개수·가시 단계·상수 크기를 하나의 계약으로 만든다. 지원 질의와 실제 파이프라인 생성이 이 함수를 공유한다.
RHI::PipelineLayoutDesc Renderer::Impl::MeshLayout(bool bindless, std::vector<RHI::ResourceBindingLayout>& bindings) const
{
	// 재질은 반복 주소와 선형 mip 필터를 사용한다. maxAnisotropy는 장치 한도를 읽어 1 이상 8 이하로 제한한다.
	RHI::SamplerDesc materialSampler = {};
	materialSampler.minFilter = RHI::SamplerFilter::Linear;
	materialSampler.magFilter = RHI::SamplerFilter::Linear;
	materialSampler.mipFilter = RHI::SamplerFilter::Linear;
	materialSampler.addressU = RHI::SamplerAddressMode::Repeat;
	materialSampler.addressV = RHI::SamplerAddressMode::Repeat;
	materialSampler.addressW = RHI::SamplerAddressMode::Repeat;
	materialSampler.mipLodBias = 0.0f;
	materialSampler.minLod = 0.0f;
	materialSampler.maxLod = 32.0f;
	materialSampler.maxAnisotropy = static_cast<uint32_t>(std::clamp<uint64_t>(
		device->GetLimit(RHI::Limit::SamplerAnisotropy), 1, 8));

	// 그림자는 텍셀 깊이 비교용 nearest/clamp/LOD 0으로 바꾼다. 재질 텍스처 샘플링과 다른 목적의 샘플러다.
	RHI::SamplerDesc shadowSampler = materialSampler;
	shadowSampler.minFilter = shadowSampler.magFilter = shadowSampler.mipFilter = RHI::SamplerFilter::Nearest;
	shadowSampler.addressU = RHI::SamplerAddressMode::ClampToEdge;
	shadowSampler.addressV = RHI::SamplerAddressMode::ClampToEdge;
	shadowSampler.addressW = RHI::SamplerAddressMode::ClampToEdge;
	shadowSampler.maxLod = 0.0f;
	shadowSampler.maxAnisotropy = 1;

	const RHI::ShaderStageFlags vertexAndFragment =
		RHI::ShaderStageFlags::Vertex | RHI::ShaderStageFlags::Fragment;
    if(bindless) // bindless=true이면 텍스처 19개 배열과 재질 인덱스 버퍼의 레이아웃을 만든다. 이 함수는 지원 질의에도 쓰이므로 여기 진입했다는 것만으로 장치 지원이 확정되지는 않는다.
    {
        bindings = {
            {0,RHI::ResourceBindingType::SampledTexture,19,RHI::ShaderStageFlags::Fragment,{}},
            {1,RHI::ResourceBindingType::ConstantBuffer,1,RHI::ShaderStageFlags::Fragment,{}},
            {8,RHI::ResourceBindingType::StaticSampler,1,RHI::ShaderStageFlags::Fragment,materialSampler},
            {29,RHI::ResourceBindingType::ReadOnlyStorageBuffer,1,RHI::ShaderStageFlags::Fragment,{}}
        };
    }
    else
    {
        bindings = {
            {0u,RHI::ResourceBindingType::SampledTexture,1,RHI::ShaderStageFlags::Fragment,{}},
            {1u,RHI::ResourceBindingType::ConstantBuffer,1,RHI::ShaderStageFlags::Fragment,{}},
            {4u,RHI::ResourceBindingType::SampledTexture,1,RHI::ShaderStageFlags::Fragment,{}},
            {5u,RHI::ResourceBindingType::SampledTexture,1,RHI::ShaderStageFlags::Fragment,{}},
            {6u,RHI::ResourceBindingType::SampledTexture,1,RHI::ShaderStageFlags::Fragment,{}},
            {7u,RHI::ResourceBindingType::SampledTexture,1,RHI::ShaderStageFlags::Fragment,{}},
            {8u,RHI::ResourceBindingType::StaticSampler,1,RHI::ShaderStageFlags::Fragment,materialSampler}
        };
    }
    if(shaderSources.bytes[MeshFragment].empty()) // 기본 fragment에만 AO·환경광·지역광 목록과 면적광 LUT 입력을 선언한다.
    {
        // The material array occupies t0..t18 in HLSL/Metal. Environment
        // textures use separate slots in the paged stock path on every backend.
        auto screenSampler=shadowSampler;
        screenSampler.minFilter=screenSampler.magFilter=RHI::SamplerFilter::Linear;
        bindings.push_back({bindless ? 22u : 16u,RHI::ResourceBindingType::SampledTexture,1,RHI::ShaderStageFlags::Fragment,{}});
        bindings.push_back({bindless ? 23u : 13u,RHI::ResourceBindingType::SampledTexture,1,RHI::ShaderStageFlags::Fragment,{}});
        bindings.push_back({bindless ? 24u : 17u,RHI::ResourceBindingType::SampledTexture,1,RHI::ShaderStageFlags::Fragment,{}});
        bindings.push_back({14u,RHI::ResourceBindingType::ConstantBuffer,1,RHI::ShaderStageFlags::Fragment,{}});
        bindings.push_back({15u,RHI::ResourceBindingType::StaticSampler,1,RHI::ShaderStageFlags::Fragment,screenSampler});
        bindings.push_back({19,RHI::ResourceBindingType::ReadOnlyStorageBuffer,1,RHI::ShaderStageFlags::Fragment,{}});
        bindings.push_back({20,RHI::ResourceBindingType::ReadOnlyStorageBuffer,1,RHI::ShaderStageFlags::Fragment,{}});
        bindings.push_back({21,RHI::ResourceBindingType::ConstantBuffer,1,RHI::ShaderStageFlags::Fragment,{}});
        bindings.push_back({26,RHI::ResourceBindingType::SampledTexture,1,RHI::ShaderStageFlags::Fragment,{}});
        bindings.push_back({27,RHI::ResourceBindingType::SampledTexture,1,RHI::ShaderStageFlags::Fragment,{}});
    }
    if(config.lighting.enabled && config.lighting.direct && config.lighting.shadows) // 직접 그림자가 실제 활성일 때만 그림자 상수·깊이·샘플러를 선언한다.
    {
        bindings.push_back({3u,RHI::ResourceBindingType::ConstantBuffer,1,vertexAndFragment,{}});
        bindings.push_back({bindless ? 28u : 2u,RHI::ResourceBindingType::SampledTexture,1,RHI::ShaderStageFlags::Fragment,{}});
        bindings.push_back({9u,RHI::ResourceBindingType::StaticSampler,1,RHI::ShaderStageFlags::Fragment,shadowSampler});
    }
    // 사용자 정점 바인딩은 기본 레이아웃 뒤에 추가한다. 기본 DrawConstants 뒤의 추가 바이트와 함께 사용자 셰이더가 맞춰야 할 계약이다.
    bindings.insert(bindings.end(),shaderSources.vertexBindings.begin(),shaderSources.vertexBindings.end());
    // 지원 조회와 실제 생성이 같은 바인딩·상수 크기를 사용한다.
    return {bindings.data(),static_cast<uint32_t>(bindings.size()),
        static_cast<uint32_t>(sizeof(DrawConstants))+shaderSources.additionalConstantBytes,
        vertexAndFragment,10u};
}

// 고정 48바이트 정점과 선택적 128바이트 인스턴스 입력을 선언하고 불투명/블렌드/양면/반전 조합의 파이프라인을 만든다.
bool Renderer::Impl::BuildPipelineStates(RHI::IDevice* device,RHI::Format colorFormat,bool compositeAlpha)
{
	if(colorFormat == RHI::Format::Unknown) return false; // 유효한 색 출력 형식이 없으면 파이프라인을 만들지 않는다.

	const RHI::VertexBufferLayout vertexBuffer = {
		0,
		static_cast<uint32_t>(sizeof(RendererVertex)),
		RHI::VertexStepMode::Vertex
	};
	const std::array<RHI::VertexAttribute, 5> vertexAttributes = {{
		{ 0, 0, RHI::Format::R32G32B32_FLOAT, static_cast<uint32_t>(offsetof(RendererVertex, px)) },
		{ 1, 0, RHI::Format::R32G32B32_FLOAT, static_cast<uint32_t>(offsetof(RendererVertex, nx)) },
		{ 2, 0, RHI::Format::R32G32_FLOAT, static_cast<uint32_t>(offsetof(RendererVertex, u)) },
		{ 3, 0, RHI::Format::R32G32B32A32_FLOAT, static_cast<uint32_t>(offsetof(RendererVertex, tx)) },
		{ 8, 0, RHI::Format::R32G32B32A32_FLOAT, static_cast<uint32_t>(offsetof(RendererVertex, cr)) }
	}};

    std::vector<RHI::VertexBufferLayout> vertexBuffers={vertexBuffer};
    std::vector<RHI::VertexAttribute> attributes(vertexAttributes.begin(),vertexAttributes.end());
    if(UsesInstanceStream()) { // 표준 인스턴싱 경로면 슬롯 1에 128바이트 간격의 모델 행렬 입력을 추가한다.
        vertexBuffers.push_back({1,128,RHI::VertexStepMode::Instance});
        for(uint32_t column=0;column<4;++column)
            attributes.push_back({4+column,1,RHI::Format::R32G32B32A32_FLOAT,column*16});
    }
	// 알파 합성의 RGB는 source*sourceAlpha + destination*(1-sourceAlpha)다.
	// 출력 알파는 compositeAlpha이면 sourceAlpha + destinationAlpha*(1-sourceAlpha), 아니면 sourceAlpha다.
	// 기본 불투명 경로는 뒤에서 알파 합성을 끈다.
	RHI::ColorAttachmentDesc colorAttachment = {
		colorFormat,
		{ true, RHI::BlendFactor::SourceAlpha, RHI::BlendFactor::OneMinusSourceAlpha, RHI::BlendOp::Add,
			RHI::BlendFactor::One, compositeAlpha ? RHI::BlendFactor::OneMinusSourceAlpha : RHI::BlendFactor::Zero, RHI::BlendOp::Add },
		RHI::ColorWriteMask::All
	};

	RHI::GraphicsPipelineDesc desc = {};
	desc.vertexShader = vertexShader;
	desc.fragmentShader = fragmentShader;
	desc.topology = RHI::PrimitiveTopology::TriangleList;
	desc.vertexBuffers = vertexBuffers.data();
	desc.vertexBufferCount = static_cast<uint32_t>(vertexBuffers.size());
	desc.vertexAttributes = attributes.data();
	desc.vertexAttributeCount = static_cast<uint32_t>(attributes.size());
	desc.raster = { RHI::FillMode::Solid, RHI::CullMode::Back, RHI::FrontFace::CounterClockwise, 0.0f, 0.0f, 0.0f };
	desc.depthStencil.format = RHI::Format::D32_FLOAT;
	desc.depthStencil.depthTestEnabled = true;
	desc.depthStencil.depthWriteEnabled = true;
	// 고급 렌더 경로이면 LessEqual로 먼저 쓴 surface 깊이와 같은 깊이도 통과시킨다. 일반 경로는 Less를 사용하며, 이 조건 자체가 surface 패스 등록 여부를 조회하지는 않는다.
	desc.depthStencil.depthCompareOp = UsesAdvancedRendering() ? RHI::CompareOp::LessEqual : RHI::CompareOp::Less;
	desc.colorAttachments = &colorAttachment;
	desc.colorAttachmentCount = 1;
    std::vector<RHI::ResourceBindingLayout> bindings;
    desc.layout=MeshLayout(UsesBindlessMaterials(),bindings);
    const bool stockFragment=shaderSources.bytes[MeshFragment].empty();
    if(stockFragment) colorAttachment.blend.enabled=false; // 기본 불투명 파이프라인은 알파 합성을 끄고 색을 직접 쓴다.
    pipeline = device->CreateGraphicsPipeline(desc);
	if(pipeline == nullptr) return false; // 기본 파이프라인 생성 실패면 변형 생성도 중단한다.
    if(stockFragment) // 기본 fragment일 때만 재질 성질별 일곱 변형을 만든다.
    {
        // variant 비트 0은 양면, 1은 블렌드, 2는 반전이다. 투명 표면은 깊이를 읽되 쓰지 않아 뒤쪽 표면의 합성을 막지 않는다.
        for(uint32_t variant=1;variant<=materialPipelines.size();++variant)
        {
            const bool transparent=(variant&2u)!=0;
            colorAttachment.blend.enabled=transparent;
            desc.raster.cullMode=(variant&1u) ? RHI::CullMode::None : RHI::CullMode::Back;
            desc.raster.frontFace=(variant&4u) ? RHI::FrontFace::Clockwise : RHI::FrontFace::CounterClockwise;
            desc.depthStencil.depthWriteEnabled=!transparent;
            materialPipelines[variant-1]=device->CreateGraphicsPipeline(desc);
            if(!materialPipelines[variant-1]) return false; // 필요한 재질 변형 하나라도 생성하지 못하면 실패한다.
        }
    }

    if(noAreaFragmentShader && UsesStockGeometry())
    {
        desc.fragmentShader=noAreaFragmentShader;
        for(uint32_t variant=0;variant<noAreaMaterialPipelines.size();++variant)
        {
            const bool transparent=(variant&2u)!=0;
            colorAttachment.blend.enabled=transparent;
            desc.raster.cullMode=(variant&1u) ? RHI::CullMode::None : RHI::CullMode::Back;
            desc.raster.frontFace=(variant&4u) ? RHI::FrontFace::Clockwise : RHI::FrontFace::CounterClockwise;
            desc.depthStencil.depthWriteEnabled=!transparent;
            noAreaMaterialPipelines[variant]=device->CreateGraphicsPipeline(desc);
            if(!noAreaMaterialPipelines[variant])
            {
                for(auto& value:noAreaMaterialPipelines) {device->DestroyPipeline(value);value=nullptr;}
                break;
            }
        }
        desc.fragmentShader=fragmentShader;
    }

    if(depthPrepassFragmentShader && UsesStockGeometry() && UsesAdvancedRendering())
    {
        desc.fragmentShader=depthPrepassFragmentShader;
        desc.depthStencil.depthWriteEnabled=false;
        desc.depthStencil.depthCompareOp=RHI::CompareOp::LessEqual;
        colorAttachment.blend.enabled=false;
        for(uint32_t variant=0;variant<depthPrepassPipelines.size();++variant)
        {
            if(variant&2u)continue;
            desc.raster.cullMode=(variant&1u) ? RHI::CullMode::None : RHI::CullMode::Back;
            desc.raster.frontFace=(variant&4u) ? RHI::FrontFace::Clockwise : RHI::FrontFace::CounterClockwise;
            depthPrepassPipelines[variant]=device->CreateGraphicsPipeline(desc);
            if(!depthPrepassPipelines[variant])return false;
        }
        desc.fragmentShader=fragmentShader;
    }
    if(noAreaDepthPrepassFragmentShader && UsesStockGeometry() && UsesAdvancedRendering())
    {
        desc.fragmentShader=noAreaDepthPrepassFragmentShader;
        desc.depthStencil.depthWriteEnabled=false;
        desc.depthStencil.depthCompareOp=RHI::CompareOp::LessEqual;
        colorAttachment.blend.enabled=false;
        for(uint32_t variant=0;variant<noAreaDepthPrepassPipelines.size();++variant)
        {
            if(variant&2u)continue;
            desc.raster.cullMode=(variant&1u) ? RHI::CullMode::None : RHI::CullMode::Back;
            desc.raster.frontFace=(variant&4u) ? RHI::FrontFace::Clockwise : RHI::FrontFace::CounterClockwise;
            noAreaDepthPrepassPipelines[variant]=device->CreateGraphicsPipeline(desc);
            if(!noAreaDepthPrepassPipelines[variant])
            {
                for(auto& value:noAreaDepthPrepassPipelines) {device->DestroyPipeline(value);value=nullptr;}
                break;
            }
        }
        desc.fragmentShader=fragmentShader;
    }
    if(config.debugWireframe && UsesAdvancedRendering()) // 고급 HDR 진단 경로에서만 와이어프레임 파이프라인을 만든다.
    {
        if(!device->Supports(RHI::Feature::Wireframe)) return false; // 장치가 선 렌더를 지원하지 않으면 요청 경로를 실패 처리한다.
        desc.raster.fillMode=RHI::FillMode::Wireframe;
        desc.depthStencil.depthWriteEnabled=false;
        desc.depthStencil.depthCompareOp=RHI::CompareOp::LessEqual;
        colorAttachment.blend.enabled=false;
        for(uint32_t variant=0;variant<wireframePipelines.size();++variant)
        {
            desc.raster.cullMode=(variant&1u) ? RHI::CullMode::None : RHI::CullMode::Back;
            desc.raster.frontFace=(variant&2u) ? RHI::FrontFace::Clockwise : RHI::FrontFace::CounterClockwise;
            wireframePipelines[variant]=device->CreateGraphicsPipeline(desc);
            if(!wireframePipelines[variant]) return false; // 필요한 와이어프레임 변형 생성 실패를 전달한다.
        }
    }

	if(!config.lighting.enabled || !config.lighting.direct || !config.lighting.shadows || shadowPipeline) return true; // 그림자를 쓰지 않거나 이미 만들었으면 그림자 파이프라인 생성을 생략한다.
    const bool stockShadow=UsesStockGeometry();
	if(shadowVertexShader == nullptr || (stockShadow && !shadowFragmentShader)) return false; // 필수 그림자 셰이더가 없으면 그림자 파이프라인을 만들 수 없다.

	std::vector<RHI::ResourceBindingLayout> shadowBindings = {{
		{ 3u, RHI::ResourceBindingType::ConstantBuffer, 1, RHI::ShaderStageFlags::Vertex, {} },
	}};
    shadowBindings.insert(shadowBindings.end(), shaderSources.vertexBindings.begin(), shaderSources.vertexBindings.end());
    // 그림자 불투명은 정점/상수만으로 깊이를 쓴다. 알파 마스크 경로만 fragment 텍스처/샘플러 바인딩이 추가된 레이아웃을 선택한다.
    const auto shadowVertexBindings=shadowBindings;
	if(shadowFragmentShader) // fragment가 있으면 알파 커버리지에 필요한 기본색 텍스처와 샘플러도 선언한다.
	{
		shadowBindings.push_back({0u,RHI::ResourceBindingType::SampledTexture,1,RHI::ShaderStageFlags::Fragment,{}});
		RHI::SamplerDesc sampler;
		sampler.minFilter=sampler.magFilter=sampler.mipFilter=RHI::SamplerFilter::Linear;
		sampler.addressU=sampler.addressV=sampler.addressW=RHI::SamplerAddressMode::Repeat;
		sampler.mipLodBias=0;
		sampler.minLod=0;
		sampler.maxLod=32;
		sampler.maxAnisotropy=static_cast<uint32_t>(std::clamp<uint64_t>(device->GetLimit(RHI::Limit::SamplerAnisotropy),1,8));
		shadowBindings.push_back({8u,RHI::ResourceBindingType::StaticSampler,1,RHI::ShaderStageFlags::Fragment,sampler});
	}
	RHI::GraphicsPipelineDesc shadowDesc = {};
	auto shadowAttributes=attributes;
	for(auto& attribute:shadowAttributes)if(attribute.location==8 && shaderSources.bytes[ShadowVertex].empty())
	{
		attribute.format=RHI::Format::R32_FLOAT;
		attribute.offset=static_cast<uint32_t>(offsetof(RendererVertex,ca));
	}
	shadowDesc.vertexShader = shadowVertexShader;
	shadowDesc.topology = RHI::PrimitiveTopology::TriangleList;
	shadowDesc.vertexBuffers = vertexBuffers.data();
	shadowDesc.vertexBufferCount = static_cast<uint32_t>(vertexBuffers.size());
	shadowDesc.vertexAttributes = attributes.data();
	shadowDesc.vertexAttributeCount = static_cast<uint32_t>(attributes.size());
	shadowDesc.raster = {
		RHI::FillMode::Solid,
		RHI::CullMode::None,
		RHI::FrontFace::CounterClockwise,
		0.0f,
		1.75f,
		0.0f
	};
	shadowDesc.depthStencil.format = RHI::Format::D32_FLOAT;
	shadowDesc.depthStencil.depthTestEnabled = true;
	shadowDesc.depthStencil.depthWriteEnabled = true;
	shadowDesc.depthStencil.depthCompareOp = RHI::CompareOp::Less;
    auto compactVertexBuffers=vertexBuffers;
    compactVertexBuffers[0].stride=sizeof(CompactShadowVertex);
    std::vector<RHI::VertexAttribute> compactAttributes={
        {0,0,RHI::Format::R32G32B32_FLOAT,static_cast<uint32_t>(offsetof(CompactShadowVertex,px))},
        {2,0,RHI::Format::R32G32_FLOAT,static_cast<uint32_t>(offsetof(CompactShadowVertex,u))},
        {8,0,RHI::Format::R32_FLOAT,static_cast<uint32_t>(offsetof(CompactShadowVertex,alpha))}};
    for(const auto& attribute:attributes)if(attribute.binding==1)compactAttributes.push_back(attribute);
    // 기본 그림자는 일반/압축 정점마다 양면·알파 커버리지·반전의 여덟 조합을 만든다. 사용자 경로는 한 공통 조합을 사용한다.
    for(uint32_t compact=0;compact<(stockShadow?2u:1u);++compact)
    for(uint32_t variant=0;variant<(stockShadow?8u:1u);++variant)
    {
        shadowDesc.vertexBuffers=compact ? compactVertexBuffers.data() : vertexBuffers.data();
        shadowDesc.vertexBufferCount=static_cast<uint32_t>(vertexBuffers.size());
        shadowDesc.vertexAttributes=compact ? compactAttributes.data() : shadowAttributes.data();
        shadowDesc.vertexAttributeCount=static_cast<uint32_t>(compact ? compactAttributes.size() : shadowAttributes.size());
        // 표준 그림자 경로에서는 불투명 변형이 깊이만 기록하고 알파 마스크 변형만 fragment를 사용한다. 사용자 경로에서는 shadowFragmentShader가 있으면 그 fragment와 바인딩을 유지한다.
        const bool coverage=stockShadow ? (variant&2u)!=0 : shadowFragmentShader!=nullptr;
        shadowDesc.fragmentShader=coverage ? shadowFragmentShader : nullptr;
        shadowDesc.raster.cullMode=stockShadow && !(variant&1u) ? RHI::CullMode::Back : RHI::CullMode::None;
        shadowDesc.raster.frontFace=(variant&4u) ? RHI::FrontFace::Clockwise : RHI::FrontFace::CounterClockwise;
        const auto& selectedBindings=coverage ? shadowBindings : shadowVertexBindings;
        shadowDesc.layout={selectedBindings.data(),static_cast<uint32_t>(selectedBindings.size()),
            static_cast<uint32_t>(sizeof(DrawConstants))+shaderSources.additionalConstantBytes,
            coverage ? RHI::ShaderStageFlags::Vertex|RHI::ShaderStageFlags::Fragment : RHI::ShaderStageFlags::Vertex,10u};
        auto& selected=compact ? compactShadowPipelines[variant] : variant ? shadowMaterialPipelines[variant-1] : shadowPipeline;
        selected=device->CreateGraphicsPipeline(shadowDesc);
        if(!selected) return false; // 선택한 그림자 변형 생성 실패면 전체 준비를 실패로 돌린다.
    }
    return true;
}

// 표면에 수직인 방향(법선)의 기본값을 담은 텍스처를 사용한다.
// 흰색·기본 법선·검정 1x1 텍스처의 생성·업로드 기록·제출이 모두 성공한 뒤 함께 공개해 일부 생성 결과가 캐시로 쓰이지 않게 한다.
bool Renderer::Impl::CreateDefaultMaterialTextures(RHI::IDevice* device)
{
	if(device == nullptr) return false; // 기본 텍스처를 생성할 장치가 없으면 실패한다.
	// 임시 텍스처 소유자다. committed 전 조기 반환하면 이미 생성한 텍스처도 모두 정리한다.
	struct PendingTextures
	{
		RHI::IDevice& device;
		std::array<RHI::TextureHandle,3> textures={};
		bool committed=false;
		~PendingTextures()
		{
			if(!committed) for(auto* texture:textures) device.DestroyTexture(texture); // 아직 묶음을 확정하지 못했으면 생성한 임시 텍스처를 모두 반환한다.
		}
	} pending{*device};
	RHI::ResourceScope resources(*device);
	// 흰색은 기본색/스칼라 입력의 중립값, (128,128,255)는 평평한 법선, 검정은 발광/빈 효과 입력의 대체값이다.
	const std::array<std::array<uint8_t, 4>, 3> pixels = {{
		{{ 255, 255, 255, 255 }},
		{{ 128, 128, 255, 255 }},
		{{ 0, 0, 0, 255 }}
	}};

	RHI::TextureDesc desc = {};
	desc.width = 1;
	desc.height = 1;
	desc.format = RHI::Format::R8G8B8A8_UNORM;
	desc.usage = RHI::TextureUsage::ShaderResource;
	for(RHI::TextureHandle& texture : pending.textures)
	{
		texture = device->CreateTexture(desc);
		if(texture == nullptr) return false; // 세 대체 텍스처 중 하나라도 생성되지 않으면 전체 묶음을 확정하지 않는다.
	}

	RHI::ICommandList* commandList = device->AcquireCommandList();
	if(commandList == nullptr) return false; // 업로드 명령 목록을 얻지 못하면 임시 텍스처를 정리한다.
	resources.Keep(commandList);
	std::array<RHI::ResourceBarrierDesc, 3> beforeCopy = {};
	std::array<RHI::ResourceBarrierDesc, 3> barriers = {};
	uint32_t barrierCount = 0;
	for(uint32_t index = 0; index < pending.textures.size(); ++index)
	{
		beforeCopy[index].texture = pending.textures[index];
		beforeCopy[index].after = RHI::ResourceState::CopyDestination;
	}
	// 새 텍스처를 복사 목적 상태로 바꾼 뒤 1x1 픽셀을 기록한다. 기록 후에는 셰이더 읽기 상태로 전환한다.
	commandList->ResourceBarrier(beforeCopy.data(), static_cast<uint32_t>(beforeCopy.size()));
	for(uint32_t index = 0; index < pending.textures.size(); ++index)
	{
		if(!device->UpdateTexture( // 픽셀 업로드 기록이 실패하면 대체 텍스처 묶음 전체를 실패 처리한다.
				*commandList,
				pending.textures[index],
				0,
				0,
				pixels[index].data(),
				static_cast<uint32_t>(pixels[index].size()),
				4,
				4))
		{
			return false;
		}
		barriers[barrierCount].texture = pending.textures[index];
		barriers[barrierCount].before = RHI::ResourceState::CopyDestination;
		barriers[barrierCount].after = RHI::ResourceState::ShaderResource;
		++barrierCount;
	}
	if(barrierCount != 0) commandList->ResourceBarrier(barriers.data(), barrierCount); // 기록한 텍스처가 있을 때만 셰이더 읽기 전환 배리어를 넣는다.
	if(!commandList->Close() || !device->Submit(&commandList,1)) return false; // 명령 닫기/제출 중 실패가 있으면 준비된 기본 텍스처로 공개하지 않는다.
	// 모든 생성·업로드 기록·명령 제출 성공 뒤에만 Impl의 기본 텍스처 배열에 공개한다.
	defaultMaterialTextures=pending.textures;
	pending.committed=true;
	return true;
}

// 출력 폭/높이와 샘플링 필요 여부를 캐시 키로 삼아 깊이 텍스처를 재사용한다. surface/화면 효과가 읽어야 하면 ShaderResource 사용 플래그도 필요하다.
bool Renderer::Impl::EnsureDepthStencilTarget(RHI::IDevice* device,RHI::TextureHandle output,bool sampled)
{
	if(device == nullptr) return false; // 깊이 텍스처 준비에 장치가 필요하다.
	if(output == nullptr || output->GetDesc().width == 0u || output->GetDesc().height == 0u) // 출력이 없거나 크기가 0이면 대응 깊이 타깃을 만들 수 없다.
		return false;

	const auto usage=sampled
        ? RHI::TextureUsage::DepthStencil|RHI::TextureUsage::ShaderResource : RHI::TextureUsage::DepthStencil;
	const bool recreate =
		depthStencilTarget == nullptr ||
		depthStencilTarget->GetDesc().width != output->GetDesc().width ||
		depthStencilTarget->GetDesc().height != output->GetDesc().height ||
        depthStencilTarget->GetDesc().usage != usage;

	if(!recreate) return true; // 크기와 사용 용도가 같으면 기존 깊이 자원을 재사용한다.

	if(depthStencilTarget != nullptr) // 크기나 용도가 달라 교체할 깊이 텍스처를 먼저 해제하고 추적 상태를 Undefined로 바꾼다. 뒤의 생성이 실패하면 이전 텍스처는 복구되지 않고 nullptr로 남는다.
	{
		device->DestroyTexture(depthStencilTarget);
		depthStencilTarget = nullptr;
		depthStencilState = RHI::ResourceState::Undefined;
	}

	RHI::TextureDesc depthDesc = {};
	depthDesc.width = output->GetDesc().width;
	depthDesc.height = output->GetDesc().height;
	depthDesc.format = RHI::Format::D32_FLOAT;
	depthDesc.usage = usage;
	depthStencilTarget = device->CreateTexture(depthDesc);
	return depthStencilTarget != nullptr;
}

// 그림자 뷰의 열·행·타일 해상도를 실제 아틀라스 크기로 바꾼다. 크기가 같으면 재사용하고 바뀌면 아틀라스 내용 캐시도 무효화한다.
bool Renderer::Impl::EnsureShadowDepthTarget(RHI::IDevice* device,uint32_t columns,uint32_t rows,uint32_t resolution)
{
	if(!config.lighting.enabled || !config.lighting.direct || !config.lighting.shadows) return true; // 직접 그림자가 꺼져 있으면 그림자 타깃이 필요 없으므로 성공으로 생략한다.
	if(device == nullptr) return false; // 그림자 타깃이 필요한데 장치가 없으면 실패한다.
    const uint32_t width=resolution*columns;
    const uint32_t height=resolution*rows;
	if(resolution == 0) return false; // 타일 해상도가 0이면 유효한 그림자 아틀라스를 만들 수 없다.
	if(shadowDepthTarget != nullptr && // 기존 아틀라스의 실제 폭/높이가 같으면 다시 만들지 않는다.
		shadowDepthTarget->GetDesc().width == width &&
		shadowDepthTarget->GetDesc().height == height)
	{
		return true;
	}

	if(shadowDepthTarget != nullptr) // 크기가 다른 아틀라스를 먼저 해제하고 내용 재사용 플래그와 추적 상태를 초기화한다. 새 텍스처 생성에 실패하면 이전 아틀라스는 복구되지 않고 nullptr로 남는다.
	{
		shadowAtlasCache.valid=false;shadowAtlasCache.pending=false;
		device->DestroyTexture(shadowDepthTarget);
		shadowDepthTarget = nullptr;
		shadowDepthState = RHI::ResourceState::Undefined;
	}

	RHI::TextureDesc shadowDesc = {};
	shadowDesc.width = width;
	shadowDesc.height = height;
	shadowDesc.format = RHI::Format::D32_FLOAT;
	shadowDesc.usage = RHI::TextureUsage::DepthStencil | RHI::TextureUsage::ShaderResource;
	shadowDepthTarget = device->CreateTexture(shadowDesc);
	return shadowDepthTarget != nullptr;
}
