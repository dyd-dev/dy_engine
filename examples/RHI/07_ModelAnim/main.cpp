#include "dyf.h"
#include "dyf/RHI.h"
#include <dyf/Extends/Model.h>

#include "vertex.h"
#include "fragment.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <cstdio>
#include <vector>
#include <iostream>
#include <chrono>

#define DY_EXAMPLE_MODEL "Models/Fox.glb"

namespace
{
	struct LightingConstants
	{
		dyf::Math::float4 cameraPosition;
		dyf::Math::float4 directionalLightDirection;
		dyf::Math::float4 directionalLightColor;
		dyf::Math::float4 ambientColor;
		dyf::Math::float4 pbrParams;
		dyf::Math::float4 environmentColor;
	};

	static_assert(sizeof(LightingConstants) == 96);

	struct DrawConstants
	{
		dyf::Math::float4x4 viewProjectionMatrix;
		dyf::Math::float4x4 modelMatrix;
		uint32_t textureFlags = 0;
		uint32_t padding0 = 0;
		uint32_t padding1 = 0;
		uint32_t padding2 = 0;
		dyf::Math::float4 emissiveColor;
		dyf::Math::float4 baseColor;
		dyf::Math::float4 materialParams;
	};

	static_assert(offsetof(DrawConstants, textureFlags) == 128u);
	static_assert(offsetof(DrawConstants, emissiveColor) == 144u);
	static_assert(offsetof(DrawConstants, baseColor) == 160u);
	static_assert(offsetof(DrawConstants, materialParams) == 176u);
	static_assert(sizeof(DrawConstants) == 192u);
}

int main()
{
	constexpr uint32_t windowWidth = 640, windowHeight = 480;
	dyf::Platform::Window window(windowWidth, windowHeight, "RHI / Model and Animation");
	if(!window.GetHandle())
	{
		std::cerr << "Window creation failed.\n";
		return 1;
	}
	std::unique_ptr<dyf::RHI::IDevice> deviceOwner(dyf::RHI::IDevice::Create(dyf::RHI::DeviceDesc{}));
	if(!deviceOwner)
	{
		std::cerr << "RHI device creation failed." << '\n';
		return 1;
	}
	auto& device = *deviceOwner;
	dyf::RHI::ResourceScope resources(device);
	dyf::RHI::SwapchainDesc swapchain;
	swapchain.window = window.GetHandle();
	swapchain.format = dyf::RHI::Format::B8G8R8A8_UNORM;
	swapchain.minimumImageCount = 2;
	swapchain.presentMode = dyf::RHI::PresentMode::Fifo;
	if(!device.CreateSwapchain(swapchain))
	{
		std::cerr << "Swapchain creation failed." << '\n';
		return 1;
	}
	auto* vertexShader = resources.Keep(device.CreateShader({dyf::RHI::ShaderStage::Vertex,
		ShaderData::vertexEntryPoint, ShaderData::vertex, ShaderData::vertexSize}));
	auto* fragmentShader = resources.Keep(device.CreateShader({dyf::RHI::ShaderStage::Fragment,
		ShaderData::fragmentEntryPoint, ShaderData::fragment, ShaderData::fragmentSize}));
	const dyf::RHI::VertexBufferLayout vertexLayout{0, sizeof(dyf::Vertex), dyf::RHI::VertexStepMode::Vertex};
	const std::array<dyf::RHI::VertexAttribute, 4> attributes = {{
		{0, 0, dyf::RHI::Format::R32G32B32_FLOAT, offsetof(dyf::Vertex, position)},
		{1, 0, dyf::RHI::Format::R32G32B32_FLOAT, offsetof(dyf::Vertex, normal)},
		{2, 0, dyf::RHI::Format::R32G32_FLOAT, offsetof(dyf::Vertex, uv)},
		{3, 0, dyf::RHI::Format::R32G32B32A32_FLOAT, offsetof(dyf::Vertex, tangent)}
	}};
	dyf::RHI::SamplerDesc sampler;
	sampler.minFilter = dyf::RHI::SamplerFilter::Linear;
	sampler.magFilter = dyf::RHI::SamplerFilter::Linear;
	sampler.mipFilter = dyf::RHI::SamplerFilter::Linear;
	sampler.addressU = dyf::RHI::SamplerAddressMode::Repeat;
	sampler.addressV = dyf::RHI::SamplerAddressMode::Repeat;
	sampler.addressW = dyf::RHI::SamplerAddressMode::Repeat;
	sampler.minLod = 0;
	sampler.maxLod = 0;
	sampler.mipLodBias = 0;
	const std::array<dyf::RHI::ResourceBindingLayout, 9> bindingLayout = {{
		{0, dyf::RHI::ResourceBindingType::SampledTexture, 1, dyf::RHI::ShaderStageFlags::Fragment, {}},
		{1, dyf::RHI::ResourceBindingType::ConstantBuffer, 1, dyf::RHI::ShaderStageFlags::Fragment, {}},
		{4, dyf::RHI::ResourceBindingType::SampledTexture, 1, dyf::RHI::ShaderStageFlags::Fragment, {}},
		{5, dyf::RHI::ResourceBindingType::SampledTexture, 1, dyf::RHI::ShaderStageFlags::Fragment, {}},
		{6, dyf::RHI::ResourceBindingType::SampledTexture, 1, dyf::RHI::ShaderStageFlags::Fragment, {}},
		{7, dyf::RHI::ResourceBindingType::SampledTexture, 1, dyf::RHI::ShaderStageFlags::Fragment, {}},
		{8, dyf::RHI::ResourceBindingType::StaticSampler, 1, dyf::RHI::ShaderStageFlags::Fragment, sampler},
		{11, dyf::RHI::ResourceBindingType::ReadOnlyStorageBuffer, 1, dyf::RHI::ShaderStageFlags::Vertex, {}},
		{12, dyf::RHI::ResourceBindingType::ReadOnlyStorageBuffer, 1, dyf::RHI::ShaderStageFlags::Vertex, {}}
	}};
	const dyf::RHI::ColorAttachmentDesc colorOutput{swapchain.format,
		{true, dyf::RHI::BlendFactor::SourceAlpha, dyf::RHI::BlendFactor::OneMinusSourceAlpha, dyf::RHI::BlendOp::Add,
		dyf::RHI::BlendFactor::One, dyf::RHI::BlendFactor::Zero, dyf::RHI::BlendOp::Add}, dyf::RHI::ColorWriteMask::All};
	dyf::RHI::GraphicsPipelineDesc pipelineDesc;
	pipelineDesc.vertexShader = vertexShader;
	pipelineDesc.fragmentShader = fragmentShader;
	pipelineDesc.topology = dyf::RHI::PrimitiveTopology::TriangleList;
	pipelineDesc.vertexBuffers = &vertexLayout;
	pipelineDesc.vertexBufferCount = 1;
	pipelineDesc.vertexAttributes = attributes.data();
	pipelineDesc.vertexAttributeCount = attributes.size();
	pipelineDesc.raster = {dyf::RHI::FillMode::Solid, dyf::RHI::CullMode::Back, dyf::RHI::FrontFace::CounterClockwise, 0, 0, 0};
	pipelineDesc.depthStencil.format = dyf::RHI::Format::D32_FLOAT;
	pipelineDesc.depthStencil.depthWriteEnabled = true;
	pipelineDesc.depthStencil.depthTestEnabled = true;
	pipelineDesc.depthStencil.depthCompareOp = dyf::RHI::CompareOp::Less;
	pipelineDesc.colorAttachments = &colorOutput;
	pipelineDesc.colorAttachmentCount = 1;
	pipelineDesc.layout = {bindingLayout.data(), static_cast<uint32_t>(bindingLayout.size()), sizeof(DrawConstants),
		dyf::RHI::ShaderStageFlags::Vertex | dyf::RHI::ShaderStageFlags::Fragment, 10};
	auto* pipeline = resources.Keep(device.CreateGraphicsPipeline(pipelineDesc));
	if(!pipeline)
	{
		std::cerr << "Mesh pipeline creation failed." << '\n';
		return 1;
	}
	dyf::RHI::TextureDesc depthDesc;
	depthDesc.width = device.GetBackBuffer()->GetDesc().width;
	depthDesc.height = device.GetBackBuffer()->GetDesc().height;
	depthDesc.format = dyf::RHI::Format::D32_FLOAT;
	depthDesc.usage = dyf::RHI::TextureUsage::DepthStencil;
	const auto destroyDepth = [&device](dyf::RHI::Texture* texture) { device.DestroyTexture(texture); };
	std::unique_ptr<dyf::RHI::Texture, decltype(destroyDepth)> depthOwner(device.CreateTexture(depthDesc), destroyDepth);
	auto* depthTexture = depthOwner.get();
	if(!depthTexture)
	{
		std::cerr << "Mesh depth target creation failed." << '\n';
		return 1;
	}
	dyf::RHI::ResourceState depthState = dyf::RHI::ResourceState::Undefined;
	dyf::ModelData model;
	if(!dyf::LoadModel(DY_EXAMPLE_MODEL, model) || model.animations.empty())
	{
		std::cerr << "Animated model loading failed." << '\n';
		return 1;
	}

	struct Batch
	{
		dyf::RHI::BufferHandle vertices = nullptr, indices = nullptr;
		dyf::RHI::ResourceSetHandle bindings = nullptr;
		uint32_t indexCount = 0, meshIndex = 0;
		DrawConstants draw{};
	};
	std::vector<Batch> batches;
	std::vector<dyf::SkinInfluence> influences;
	std::vector<dyf::SkinJointMatrices> palette;
	for(uint32_t index = 0; index < model.meshes.size(); ++index)
	{
		const auto& source = model.meshes[index];
		if(source.mesh.vertices.empty() || source.mesh.indices.empty()) continue;
		Batch batch;
		batch.draw.modelMatrix = dyf::Math::float4x4::Identity();
		batch.draw.padding1 = UINT32_MAX;
		batch.draw.padding0 = UINT32_MAX;
		batch.draw.baseColor = {1, 1, 1, 1};
		batch.draw.materialParams = {0, 0.5f, 1, 1};
		batch.meshIndex = index;
		batch.indexCount = source.mesh.indices.size();
		if(!source.skinInfluences.empty())
		{
			if(source.skinIndex >= model.skins.size())
			{
				std::cerr << "Invalid model skin." << '\n';
				return 1;
			}
			batch.draw.padding0 = influences.size();
			batch.draw.padding1 = palette.size();
			influences.insert(influences.end(), source.skinInfluences.begin(), source.skinInfluences.end());
			palette.resize(palette.size() + model.skins[source.skinIndex].jointNodeIndices.size());
		}
		batches.push_back(batch);
	}
	if(influences.empty() || palette.empty())
	{
		std::cerr << "Example model has no skinning data." << '\n';
		return 1;
	}

	dyf::RHI::ResourceScope uploadScope(device);
	auto* upload = uploadScope.Keep(device.AcquireCommandList());
	if(!upload)
	{
		std::cerr << "Upload command list unavailable." << '\n';
		return 1;
	}
	const auto influenceBufferBytes = influences.size() * sizeof(dyf::SkinInfluence);
	if(influenceBufferBytes > UINT32_MAX)
	{
		std::cerr << "Buffer data exceeds the RHI size range." << '\n';
		return 1;
	}
	auto* influenceBuffer = resources.Keep(device.CreateBuffer({static_cast<uint32_t>(influenceBufferBytes),
		sizeof(dyf::SkinInfluence), dyf::RHI::BufferUsage::Storage, dyf::RHI::ResourceState::CopyDestination}));
	if(!influenceBuffer || !device.UpdateBuffer(*upload, influenceBuffer, 0, influences.data(), influenceBuffer->GetDesc().size))
	{
		std::cerr << "Buffer upload failed." << '\n';
		return 1;
	}
	const dyf::RHI::ResourceBarrierDesc influenceBufferReady{influenceBuffer, nullptr, dyf::RHI::ResourceState::CopyDestination, dyf::RHI::ResourceState::ShaderResource, {}};
	upload->ResourceBarrier(&influenceBufferReady, 1);
	const auto paletteBufferBytes = palette.size() * sizeof(dyf::SkinJointMatrices);
	if(paletteBufferBytes > UINT32_MAX)
	{
		std::cerr << "Buffer data exceeds the RHI size range." << '\n';
		return 1;
	}
	auto* paletteBuffer = resources.Keep(device.CreateBuffer({static_cast<uint32_t>(paletteBufferBytes),
		sizeof(dyf::SkinJointMatrices), dyf::RHI::BufferUsage::Storage, dyf::RHI::ResourceState::CopyDestination}));
	if(!paletteBuffer || !device.UpdateBuffer(*upload, paletteBuffer, 0, palette.data(), paletteBuffer->GetDesc().size))
	{
		std::cerr << "Buffer upload failed." << '\n';
		return 1;
	}
	const dyf::RHI::ResourceBarrierDesc paletteBufferReady{paletteBuffer, nullptr, dyf::RHI::ResourceState::CopyDestination, dyf::RHI::ResourceState::ShaderResource, {}};
	upload->ResourceBarrier(&paletteBufferReady, 1);
	LightingConstants lighting{};
	lighting.cameraPosition = {2.6f, -3, 1.8f, 0};
	lighting.directionalLightDirection = {0, -1, 2, 0};
	lighting.directionalLightColor = {1, 1, 1, 3};
	lighting.ambientColor = {1, 1, 1, 0.035f};
	lighting.pbrParams = {0.04f, 0.25f, 1, 0};
	lighting.environmentColor = {1, 1, 1, 1};

	auto* lightBuffer = resources.Keep(device.CreateBuffer({sizeof(lighting),
		sizeof(lighting), dyf::RHI::BufferUsage::Constant, dyf::RHI::ResourceState::CopyDestination}));
	if(!lightBuffer || !device.UpdateBuffer(*upload, lightBuffer, 0, &lighting, lightBuffer->GetDesc().size))
	{
		std::cerr << "Buffer upload failed." << '\n';
		return 1;
	}
	const dyf::RHI::ResourceBarrierDesc lightBufferReady{lightBuffer, nullptr, dyf::RHI::ResourceState::CopyDestination, dyf::RHI::ResourceState::ConstantBuffer, {}};
	upload->ResourceBarrier(&lightBufferReady, 1);
	// 이미지가 없는 슬롯은 플래그가 꺼져 있어 샘플링하지 않는다. 바인딩만 흰색 텍스처로 채운다.
	const uint8_t whitePixel[] = {255, 255, 255, 255};
	dyf::RHI::TextureDesc whiteDesc;
	whiteDesc.width = 1;
	whiteDesc.height = 1;
	whiteDesc.format = dyf::RHI::Format::R8G8B8A8_UNORM;
	whiteDesc.usage = dyf::RHI::TextureUsage::ShaderResource;
	auto* white = resources.Keep(device.CreateTexture(whiteDesc));
	dyf::RHI::ResourceBarrierDesc whiteReady{nullptr, white, dyf::RHI::ResourceState::Undefined, dyf::RHI::ResourceState::CopyDestination, {}};
	upload->ResourceBarrier(&whiteReady, 1);
	if(!white || !device.UpdateTexture(*upload, white, 0, 0, whitePixel, sizeof(whitePixel), sizeof(whitePixel), sizeof(whitePixel)))
	{
		std::cerr << "Texture upload failed." << '\n';
		return 1;
	}
	whiteReady.before = dyf::RHI::ResourceState::CopyDestination;
	whiteReady.after = dyf::RHI::ResourceState::ShaderResource;
	upload->ResourceBarrier(&whiteReady, 1);
	std::vector<dyf::Image> preparedImages = model.textures;
	// 같은 원본도 색상과 데이터 용도에 함께 쓰이면 서로 다른 GPU 포맷이 필요하다.
	// 슬롯 0은 선형 데이터, 슬롯 1은 sRGB 색상이며 실제 쓰는 조합만 업로드한다.
	std::vector<std::array<dyf::RHI::TextureHandle, 2>> textures(model.textures.size());
	for(auto& batch : batches)
	{
		const auto& source = model.meshes[batch.meshIndex];
		const auto& vertices = source.mesh.vertices;
		const auto verticesBytes = vertices.size() * sizeof(dyf::Vertex);
		if(verticesBytes > UINT32_MAX)
		{
			std::cerr << "Buffer data exceeds the RHI size range." << '\n';
			return 1;
		}
		batch.vertices = resources.Keep(device.CreateBuffer({static_cast<uint32_t>(verticesBytes),
			sizeof(dyf::Vertex), dyf::RHI::BufferUsage::Vertex, dyf::RHI::ResourceState::CopyDestination}));
		if(!batch.vertices || !device.UpdateBuffer(*upload, batch.vertices, 0, vertices.data(), batch.vertices->GetDesc().size))
		{
			std::cerr << "Buffer upload failed." << '\n';
			return 1;
		}
		const dyf::RHI::ResourceBarrierDesc verticesReady{batch.vertices, nullptr, dyf::RHI::ResourceState::CopyDestination, dyf::RHI::ResourceState::VertexBuffer, {}};
		upload->ResourceBarrier(&verticesReady, 1);
		const auto indicesBytes = source.mesh.indices.size() * sizeof(uint32_t);
		if(indicesBytes > UINT32_MAX)
		{
			std::cerr << "Buffer data exceeds the RHI size range." << '\n';
			return 1;
		}
		batch.indices = resources.Keep(device.CreateBuffer({static_cast<uint32_t>(indicesBytes),
			sizeof(uint32_t), dyf::RHI::BufferUsage::Index, dyf::RHI::ResourceState::CopyDestination}));
		if(!batch.indices || !device.UpdateBuffer(*upload, batch.indices, 0, source.mesh.indices.data(), batch.indices->GetDesc().size))
		{
			std::cerr << "Buffer upload failed." << '\n';
			return 1;
		}
		const dyf::RHI::ResourceBarrierDesc indicesReady{batch.indices, nullptr, dyf::RHI::ResourceState::CopyDestination, dyf::RHI::ResourceState::IndexBuffer, {}};
		upload->ResourceBarrier(&indicesReady, 1);
		if(source.materialIndex >= model.materials.size())
		{
			std::cerr << "Invalid material index." << '\n';
			return 1;
		}
		const auto& material = model.materials[source.materialIndex];
		const auto& materialConfig = material.material;
		batch.draw.baseColor = materialConfig.baseColor;
		batch.draw.emissiveColor = {materialConfig.emissiveColor.x, materialConfig.emissiveColor.y, materialConfig.emissiveColor.z, 0};
		batch.draw.materialParams = {materialConfig.metallicFactor, materialConfig.roughnessFactor, materialConfig.normalScale, materialConfig.occlusionStrength};
		std::array<dyf::RHI::TextureHandle, 5> materialTextures{white, white, white, white, white};
		for(uint32_t kind = 0; kind < materialTextures.size(); ++kind)
		{
			const uint32_t textureIndex = material.textureIndices[kind];
			if(textureIndex >= textures.size()) continue;

			const auto textureKind = static_cast<dyf::MaterialTextureKind>(kind);
			const bool isColorTexture = textureKind == dyf::MaterialTextureKind::BaseColor
				|| textureKind == dyf::MaterialTextureKind::Emissive;
			auto& texture = textures[textureIndex][isColorTexture ? 1 : 0];
			if(!texture)
			{
				auto& prepared = preparedImages[textureIndex];
				if(!prepared.IsValid())
				{
					// 외부 이미지 경로만 있는 모델은 GPU 업로드 전에 한 번 디코딩한다.
					const auto sourcePath = prepared.GetSourcePath();
					if(sourcePath.empty() || !dyf::LoadImage(sourcePath, prepared))
					{
						std::cerr << "Model texture decode failed." << '\n';
						return 1;
					}
				}
				dyf::RHI::TextureDesc textureDesc;
				textureDesc.width = prepared.GetWidth();
				textureDesc.height = prepared.GetHeight();
				textureDesc.format = isColorTexture
					? dyf::RHI::Format::R8G8B8A8_UNORM_SRGB : dyf::RHI::Format::R8G8B8A8_UNORM;
				textureDesc.usage = dyf::RHI::TextureUsage::ShaderResource;
				const uint64_t textureBytes = static_cast<uint64_t>(textureDesc.width) * textureDesc.height * 4;
				if(!textureDesc.width || !textureDesc.height || textureBytes > UINT32_MAX)
				{
					std::cerr << "Texture data exceeds the RHI upload range." << '\n';
					return 1;
				}
				texture = resources.Keep(device.CreateTexture(textureDesc));
				if(!texture)
				{
					std::cerr << "Model texture creation failed." << '\n';
					return 1;
				}
				dyf::RHI::ResourceBarrierDesc textureReady{nullptr, texture, dyf::RHI::ResourceState::Undefined, dyf::RHI::ResourceState::CopyDestination, {}};
				upload->ResourceBarrier(&textureReady, 1);
				if(!device.UpdateTexture(*upload, texture, 0, 0, prepared.GetPixels().data(), static_cast<uint32_t>(textureBytes),
					textureDesc.width * 4, static_cast<uint32_t>(textureBytes)))
				{
					std::cerr << "Texture upload failed." << '\n';
					return 1;
				}
				textureReady.before = dyf::RHI::ResourceState::CopyDestination;
				textureReady.after = dyf::RHI::ResourceState::ShaderResource;
				upload->ResourceBarrier(&textureReady, 1);
			}
			materialTextures[kind] = texture;
			batch.draw.textureFlags |= 1u << kind;
		}
		const std::array<dyf::RHI::ResourceBinding, 8> materialBindings = {{
			{0, 0, nullptr, materialTextures[0], 0, 0, {}},
			{1, 0, lightBuffer, nullptr, 0, sizeof(LightingConstants), {}},
			{4, 0, nullptr, materialTextures[1], 0, 0, {}},
			{5, 0, nullptr, materialTextures[2], 0, 0, {}},
			{6, 0, nullptr, materialTextures[3], 0, 0, {}},
			{7, 0, nullptr, materialTextures[4], 0, 0, {}},
			{11, 0, influenceBuffer, nullptr, 0, influenceBuffer->GetDesc().size, {}},
			{12, 0, paletteBuffer, nullptr, 0, paletteBuffer->GetDesc().size, {}}
		}};
		batch.bindings = resources.Keep(device.CreateResourceSet({pipeline, materialBindings.data(), materialBindings.size()}));
		if(!batch.bindings)
		{
			std::cerr << "Model resource bindings failed.\n";
			return 1;
		}
	}
	if(!upload->Close() || !device.Submit(&upload, 1))
	{
		std::cerr << "Model resource upload failed." << '\n';
		return 1;
	}

	dyf::Camera camera;
	const dyf::Math::float3 cameraPosition = {2.6f, -3, 1.8f};
	const dyf::Math::float3 cameraTarget = {0, 0, 0.4f};
	camera.LookAt(cameraPosition, cameraTarget);
	camera.SetPerspective(depthDesc.width / static_cast<float>(depthDesc.height));
	// dyf 예제와 같은 크기와 표시 방향을 일반 변환으로 지정한다.
	const float modelScale = 0.012f, modelRotationX = 1.5707963f, modelRotationY = 3.14159265f;
	const auto modelTransform = dyf::Math::Scaling(modelScale)
		* dyf::Math::RotationX(modelRotationX) * dyf::Math::RotationY(modelRotationY) * model.assetTransform;
	std::vector<dyf::NodeTransform> pose;
	std::vector<dyf::Math::float4x4> globalPose;
	float animationTime = 0;
	auto lastFrame = std::chrono::steady_clock::now();
	while(true)
	{
		window.PollEvents();
		if(!window.IsRunning()) break;

		const auto now = std::chrono::steady_clock::now();
		const float deltaSeconds = std::chrono::duration<float>(now - lastFrame).count();
		lastFrame = now;
		animationTime = std::fmod(animationTime + deltaSeconds, model.animations[0].duration);

		if(!device.BeginFrame()) continue;
		// BeginFrame이 교체한 backbuffer와 depth target의 크기를 맞춘다.
		const auto& frameTargetDesc = device.GetBackBuffer()->GetDesc();
		if (depthDesc.width != frameTargetDesc.width || depthDesc.height != frameTargetDesc.height)
		{
			depthDesc.width = frameTargetDesc.width;
			depthDesc.height = frameTargetDesc.height;
			auto* resizedDepth = device.CreateTexture(depthDesc);
			if (!resizedDepth)
			{
				std::fprintf(stderr, "Resized depth target creation failed.\n");
				return 1;
			}
			depthOwner.reset(resizedDepth);
			depthTexture = depthOwner.get();
			depthState = dyf::RHI::ResourceState::Undefined;
			camera.SetPerspective(depthDesc.width / static_cast<float>(depthDesc.height));
		}
		pose.clear();
		for(const auto& node : model.nodes)
		{
			pose.push_back(node.bindTransform);
		}
		if(!dyf::SampleAnimationClip(model.animations[0], animationTime, pose)
			|| !dyf::BuildGlobalNodeMatrices(model.nodes, pose, globalPose))
		{
			std::cerr << "Animation pose evaluation failed." << '\n';
			return 1;
		}
		for(auto& batch : batches)
		{
			const auto& mesh = model.meshes[batch.meshIndex];
			if(mesh.nodeIndex >= globalPose.size())
			{
				std::cerr << "Invalid mesh node." << '\n';
				return 1;
			}
			batch.draw.modelMatrix = modelTransform * globalPose[mesh.nodeIndex];
			batch.draw.viewProjectionMatrix = camera.projection * camera.view;
			if(batch.draw.padding0 != UINT32_MAX && mesh.skinIndex < model.skins.size())
			{
				std::vector<dyf::SkinJointMatrices> joints;
				if(!dyf::BuildSkinPalette(globalPose[mesh.nodeIndex], globalPose, model.skins[mesh.skinIndex], joints))
				{
					std::cerr << "Skin palette evaluation failed." << '\n';
					return 1;
				}
				std::copy(joints.begin(), joints.end(), palette.begin() + batch.draw.padding1);
			}
		}
		dyf::RHI::ResourceScope commandsScope(device);
		auto* commands = commandsScope.Keep(device.AcquireCommandList());
		if(!commands)
		{
			std::cerr << "Model command list unavailable." << '\n';
			return 1;
		}
		dyf::RHI::ResourceBarrierDesc paletteBarrier{paletteBuffer, nullptr, dyf::RHI::ResourceState::ShaderResource, dyf::RHI::ResourceState::CopyDestination, {}};
		commands->ResourceBarrier(&paletteBarrier, 1);
		if(!device.UpdateBuffer(*commands, paletteBuffer, 0, palette.data(), paletteBuffer->GetDesc().size))
		{
			std::cerr << "Joint palette upload failed." << '\n';
			return 1;
		}
		std::swap(paletteBarrier.before, paletteBarrier.after);
		commands->ResourceBarrier(&paletteBarrier, 1);
		auto* backBuffer = device.GetBackBuffer();
		const std::array<dyf::RHI::ResourceBarrierDesc, 2> before = {{
			{nullptr, backBuffer, dyf::RHI::ResourceState::Present, dyf::RHI::ResourceState::RenderTarget, {}},
			{nullptr, depthTexture, depthState, dyf::RHI::ResourceState::DepthWrite, {}}
		}};
		commands->ResourceBarrier(before.data(), depthState == dyf::RHI::ResourceState::DepthWrite?1:2);
		dyf::RHI::ColorAttachment color;
		color.texture = backBuffer;
		color.loadOp = dyf::RHI::LoadOp::Clear;
		color.storeOp = dyf::RHI::StoreOp::Store;
		color.clearColor[0] = 0.08f;
		color.clearColor[1] = 0.10f;
		color.clearColor[2] = 0.14f;
		color.clearColor[3] = 1;
		dyf::RHI::DepthStencilAttachment depth;
		depth.texture = depthTexture;
		depth.state = dyf::RHI::ResourceState::DepthWrite;
		depth.depthLoadOp = dyf::RHI::LoadOp::Clear;
		depth.depthStoreOp = dyf::RHI::StoreOp::Discard;
		depth.clearDepth = 1;
		commands->BeginRendering({&color, 1, &depth});
		commands->BindGraphicsPipeline(pipeline);
		const dyf::RHI::Viewport viewport{0, 0, static_cast<float>(backBuffer->GetDesc().width), static_cast<float>(backBuffer->GetDesc().height), 0, 1};
		const dyf::RHI::Rect scissor{0, 0, backBuffer->GetDesc().width, backBuffer->GetDesc().height};
		commands->SetViewport(viewport);
		commands->SetScissor(scissor);
		for(const auto& batch : batches)
		{
			commands->BindResourceSet(batch.bindings);
			commands->BindVertexBuffer(0, batch.vertices, 0);
			commands->BindIndexBuffer(batch.indices, dyf::RHI::Format::R32_UINT, 0);
			commands->SetInlineConstants(0, sizeof(batch.draw), &batch.draw);
			commands->DrawIndexedInstanced(batch.indexCount, 1, 0, 0, 0);
		}
		commands->EndRendering();
		const dyf::RHI::ResourceBarrierDesc present{nullptr, backBuffer, dyf::RHI::ResourceState::RenderTarget, dyf::RHI::ResourceState::Present, {}};
		commands->ResourceBarrier(&present, 1);
		if(!commands->Close() || !device.Submit(&commands, 1))
		{
			std::cerr << "Model draw submission failed." << '\n';
			return 1;
		}
		depthState = dyf::RHI::ResourceState::DepthWrite;
		if(!device.Present())
		{
			std::cerr << "Presentation failed.\n";
			return 1;
		}
	}
	return 0;
}
