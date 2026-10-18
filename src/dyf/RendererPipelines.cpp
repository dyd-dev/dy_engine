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

RHI::PipelineLayoutDesc Renderer::Impl::MeshLayout(bool bindless, std::vector<RHI::ResourceBindingLayout>& bindings) const
{
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

	RHI::SamplerDesc shadowSampler = materialSampler;
	shadowSampler.minFilter = shadowSampler.magFilter = shadowSampler.mipFilter = RHI::SamplerFilter::Nearest;
	shadowSampler.addressU = RHI::SamplerAddressMode::ClampToEdge;
	shadowSampler.addressV = RHI::SamplerAddressMode::ClampToEdge;
	shadowSampler.addressW = RHI::SamplerAddressMode::ClampToEdge;
	shadowSampler.maxLod = 0.0f;
	shadowSampler.maxAnisotropy = 1;

	const RHI::ShaderStageFlags vertexAndFragment =
		RHI::ShaderStageFlags::Vertex | RHI::ShaderStageFlags::Fragment;
    if(bindless)
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
        if(shaderSources.bytes[MeshFragment].empty())
        {
            auto screenSampler=shadowSampler;
            screenSampler.minFilter=screenSampler.magFilter=RHI::SamplerFilter::Linear;
            bindings.push_back({16u,RHI::ResourceBindingType::SampledTexture,1,RHI::ShaderStageFlags::Fragment,{}});
            bindings.push_back({13u,RHI::ResourceBindingType::SampledTexture,1,RHI::ShaderStageFlags::Fragment,{}});
            bindings.push_back({17u,RHI::ResourceBindingType::SampledTexture,1,RHI::ShaderStageFlags::Fragment,{}});
            bindings.push_back({14u,RHI::ResourceBindingType::ConstantBuffer,1,RHI::ShaderStageFlags::Fragment,{}});
            bindings.push_back({15u,RHI::ResourceBindingType::StaticSampler,1,RHI::ShaderStageFlags::Fragment,screenSampler});
        }
    }
    if(shaderSources.bytes[MeshFragment].empty())
    {
        bindings.push_back({19,RHI::ResourceBindingType::ReadOnlyStorageBuffer,1,RHI::ShaderStageFlags::Fragment,{}});
        bindings.push_back({20,RHI::ResourceBindingType::ReadOnlyStorageBuffer,1,RHI::ShaderStageFlags::Fragment,{}});
        bindings.push_back({21,RHI::ResourceBindingType::ConstantBuffer,1,RHI::ShaderStageFlags::Fragment,{}});
        bindings.push_back({26,RHI::ResourceBindingType::SampledTexture,1,RHI::ShaderStageFlags::Fragment,{}});
        bindings.push_back({27,RHI::ResourceBindingType::SampledTexture,1,RHI::ShaderStageFlags::Fragment,{}});
    }
    if(config.lighting.enabled && config.lighting.direct && config.lighting.shadows)
    {
        bindings.push_back({3u,RHI::ResourceBindingType::ConstantBuffer,1,vertexAndFragment,{}});
        bindings.push_back({bindless ? 28u : 2u,RHI::ResourceBindingType::SampledTexture,1,RHI::ShaderStageFlags::Fragment,{}});
        bindings.push_back({9u,RHI::ResourceBindingType::StaticSampler,1,RHI::ShaderStageFlags::Fragment,shadowSampler});
    }
    bindings.insert(bindings.end(),shaderSources.vertexBindings.begin(),shaderSources.vertexBindings.end());
    // 지원 조회와 실제 생성이 같은 바인딩·상수 크기를 사용한다.
    return {bindings.data(),static_cast<uint32_t>(bindings.size()),
        static_cast<uint32_t>(sizeof(DrawConstants))+shaderSources.additionalConstantBytes,
        vertexAndFragment,10u};
}

bool Renderer::Impl::BuildPipelineStates(RHI::IDevice* device,RHI::Format colorFormat,bool compositeAlpha)
{
	if(colorFormat == RHI::Format::Unknown) return false;

	const RHI::VertexBufferLayout vertexBuffer = {
		0,
		static_cast<uint32_t>(sizeof(RendererVertex)),
		RHI::VertexStepMode::Vertex
	};
	const std::array<RHI::VertexAttribute, 4> vertexAttributes = {{
		{ 0, 0, RHI::Format::R32G32B32_FLOAT, static_cast<uint32_t>(offsetof(RendererVertex, px)) },
		{ 1, 0, RHI::Format::R32G32B32_FLOAT, static_cast<uint32_t>(offsetof(RendererVertex, nx)) },
		{ 2, 0, RHI::Format::R32G32_FLOAT, static_cast<uint32_t>(offsetof(RendererVertex, u)) },
		{ 3, 0, RHI::Format::R32G32B32A32_FLOAT, static_cast<uint32_t>(offsetof(RendererVertex, tx)) }
	}};

    std::vector<RHI::VertexBufferLayout> vertexBuffers={vertexBuffer};
    std::vector<RHI::VertexAttribute> attributes(vertexAttributes.begin(),vertexAttributes.end());
    if(UsesInstanceStream()) {
        vertexBuffers.push_back({1,128,RHI::VertexStepMode::Instance});
        for(uint32_t column=0;column<4;++column)
            attributes.push_back({4+column,1,RHI::Format::R32G32B32A32_FLOAT,column*16});
    }
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
	desc.depthStencil.depthCompareOp = UsesAdvancedRendering() ? RHI::CompareOp::LessEqual : RHI::CompareOp::Less;
	desc.colorAttachments = &colorAttachment;
	desc.colorAttachmentCount = 1;
    std::vector<RHI::ResourceBindingLayout> bindings;
    desc.layout=MeshLayout(UsesBindlessMaterials(),bindings);
    const bool stockFragment=shaderSources.bytes[MeshFragment].empty();
    if(stockFragment) colorAttachment.blend.enabled=false;
    pipeline = device->CreateGraphicsPipeline(desc);
	if(pipeline == nullptr) return false;
    if(stockFragment)
    {
        for(uint32_t variant=1;variant<=materialPipelines.size();++variant)
        {
            const bool transparent=(variant&2u)!=0;
            colorAttachment.blend.enabled=transparent;
            desc.raster.cullMode=(variant&1u) ? RHI::CullMode::None : RHI::CullMode::Back;
            desc.raster.frontFace=(variant&4u) ? RHI::FrontFace::Clockwise : RHI::FrontFace::CounterClockwise;
            desc.depthStencil.depthWriteEnabled=!transparent;
            materialPipelines[variant-1]=device->CreateGraphicsPipeline(desc);
            if(!materialPipelines[variant-1]) return false;
        }
    }

    if(config.debugWireframe && UsesAdvancedRendering())
    {
        if(!device->Supports(RHI::Feature::Wireframe)) return false;
        desc.raster.fillMode=RHI::FillMode::Wireframe;
        desc.depthStencil.depthWriteEnabled=false;
        desc.depthStencil.depthCompareOp=RHI::CompareOp::LessEqual;
        colorAttachment.blend.enabled=false;
        for(uint32_t variant=0;variant<wireframePipelines.size();++variant)
        {
            desc.raster.cullMode=(variant&1u) ? RHI::CullMode::None : RHI::CullMode::Back;
            desc.raster.frontFace=(variant&2u) ? RHI::FrontFace::Clockwise : RHI::FrontFace::CounterClockwise;
            wireframePipelines[variant]=device->CreateGraphicsPipeline(desc);
            if(!wireframePipelines[variant]) return false;
        }
    }

	if(!config.lighting.enabled || !config.lighting.direct || !config.lighting.shadows || shadowPipeline) return true;
    const bool stockShadow=UsesStockGeometry();
	if(shadowVertexShader == nullptr || (stockShadow && !shadowFragmentShader)) return false;

	std::vector<RHI::ResourceBindingLayout> shadowBindings = {{
		{ 3u, RHI::ResourceBindingType::ConstantBuffer, 1, RHI::ShaderStageFlags::Vertex, {} },
	}};
    shadowBindings.insert(shadowBindings.end(), shaderSources.vertexBindings.begin(), shaderSources.vertexBindings.end());
    const auto shadowVertexBindings=shadowBindings;
	if(shadowFragmentShader)
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
    for(uint32_t variant=0;variant<(stockShadow?8u:1u);++variant)
    {
        // 불투명 그림자는 깊이만 기록하고 알파 마스크만 기존 fragment 판정을 유지한다.
        const bool coverage=stockShadow ? (variant&2u)!=0 : shadowFragmentShader!=nullptr;
        shadowDesc.fragmentShader=coverage ? shadowFragmentShader : nullptr;
        shadowDesc.raster.cullMode=stockShadow && !(variant&1u) ? RHI::CullMode::Back : RHI::CullMode::None;
        shadowDesc.raster.frontFace=(variant&4u) ? RHI::FrontFace::Clockwise : RHI::FrontFace::CounterClockwise;
        const auto& selectedBindings=coverage ? shadowBindings : shadowVertexBindings;
        shadowDesc.layout={selectedBindings.data(),static_cast<uint32_t>(selectedBindings.size()),
            static_cast<uint32_t>(sizeof(DrawConstants))+shaderSources.additionalConstantBytes,
            coverage ? RHI::ShaderStageFlags::Vertex|RHI::ShaderStageFlags::Fragment : RHI::ShaderStageFlags::Vertex,10u};
        auto& selected=variant ? shadowMaterialPipelines[variant-1] : shadowPipeline;
        selected=device->CreateGraphicsPipeline(shadowDesc);
        if(!selected) return false;
    }
    return true;
}

bool Renderer::Impl::CreateDefaultMaterialTextures(RHI::IDevice* device)
{
	if(device == nullptr) return false;
	// Publish the complete, uploaded fallback set together. Until then a retry
	// must not mistake a partially created first texture for a prepared cache.
	struct PendingTextures
	{
		RHI::IDevice& device;
		std::array<RHI::TextureHandle,3> textures={};
		bool committed=false;
		~PendingTextures()
		{
			if(!committed) for(auto* texture:textures) device.DestroyTexture(texture);
		}
	} pending{*device};
	RHI::ResourceScope resources(*device);
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
		if(texture == nullptr) return false;
	}

	RHI::ICommandList* commandList = device->AcquireCommandList();
	if(commandList == nullptr) return false;
	resources.Keep(commandList);
	std::array<RHI::ResourceBarrierDesc, 3> beforeCopy = {};
	std::array<RHI::ResourceBarrierDesc, 3> barriers = {};
	uint32_t barrierCount = 0;
	for(uint32_t index = 0; index < pending.textures.size(); ++index)
	{
		beforeCopy[index].texture = pending.textures[index];
		beforeCopy[index].after = RHI::ResourceState::CopyDestination;
	}
	commandList->ResourceBarrier(beforeCopy.data(), static_cast<uint32_t>(beforeCopy.size()));
	for(uint32_t index = 0; index < pending.textures.size(); ++index)
	{
		if(!device->UpdateTexture(
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
	if(barrierCount != 0) commandList->ResourceBarrier(barriers.data(), barrierCount);
	if(!commandList->Close() || !device->Submit(&commandList,1)) return false;
	defaultMaterialTextures=pending.textures;
	pending.committed=true;
	return true;
}

bool Renderer::Impl::EnsureDepthStencilTarget(RHI::IDevice* device,RHI::TextureHandle output,bool sampled)
{
	if(device == nullptr) return false;
	if(output == nullptr || output->GetDesc().width == 0u || output->GetDesc().height == 0u)
		return false;

	const auto usage=sampled
        ? RHI::TextureUsage::DepthStencil|RHI::TextureUsage::ShaderResource : RHI::TextureUsage::DepthStencil;
	const bool recreate =
		depthStencilTarget == nullptr ||
		depthStencilTarget->GetDesc().width != output->GetDesc().width ||
		depthStencilTarget->GetDesc().height != output->GetDesc().height ||
        depthStencilTarget->GetDesc().usage != usage;

	if(!recreate) return true;

	if(depthStencilTarget != nullptr)
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

bool Renderer::Impl::EnsureShadowDepthTarget(RHI::IDevice* device,uint32_t columns,uint32_t rows,uint32_t resolution)
{
	if(!config.lighting.enabled || !config.lighting.direct || !config.lighting.shadows) return true;
	if(device == nullptr) return false;
    const uint32_t width=resolution*columns;
    const uint32_t height=resolution*rows;
	if(resolution == 0) return false;
	if(shadowDepthTarget != nullptr &&
		shadowDepthTarget->GetDesc().width == width &&
		shadowDepthTarget->GetDesc().height == height)
	{
		return true;
	}

	if(shadowDepthTarget != nullptr)
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
