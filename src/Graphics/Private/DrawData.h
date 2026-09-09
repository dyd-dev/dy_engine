#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "Graphics/Private/MaterialTexture.h"
#include "Math/Math.h"
#include "RHI/ResourceHandles.h"
#include "RHI/ResourceState.h"

namespace dy::RHI
{
	class ICommandList;
	class IDevice;
}

namespace dy::Graphics
{
	class Scene;
}

namespace dy::Graphics::Private
{
	struct SceneMaterialState
	{
		std::array<RHI::TextureHandle, kMaterialTextureCount> textures = {};
		uint32_t textureFlags = 0;
	};

	struct DrawContext
	{
		Math::float4 clearColor = {};
		Math::float4x4 viewProjection = Math::float4x4::Identity();
		RHI::PipelineHandle pipeline = nullptr;
		RHI::TextureHandle depthStencil = nullptr;
		RHI::ResourceState depthStencilState = RHI::ResourceState::Undefined;
		RHI::BufferHandle lightingBuffer = nullptr;
		RHI::BufferHandle shadowMatrixBuffer = nullptr;
		const std::vector<SceneMaterialState>* materialStates = nullptr;
		RHI::PipelineHandle shadowPipeline = nullptr;
		RHI::TextureHandle shadowDepth = nullptr;
		RHI::ResourceState shadowDepthState = RHI::ResourceState::Undefined;
	};

	class DrawData
	{
	public:
		[[nodiscard]] bool Prepare(const Scene& scene, RHI::IDevice* device);
		[[nodiscard]] bool PrepareDeformations(const Scene& scene, RHI::IDevice* device);
		[[nodiscard]] bool Submit(const Scene& scene, RHI::IDevice* device, const DrawContext& context);
		void Shutdown(RHI::IDevice* device);

	private:
		struct SceneMeshState
		{
			RHI::BufferHandle vertexBuffer = nullptr;
			RHI::BufferHandle indexBuffer = nullptr;
			uint32_t indexCount = 0;
			bool vertexReady = false;
			bool indexReady = false;
			bool prepared = false;
		};

		void DestroyMeshState(RHI::IDevice* device, SceneMeshState& mesh);
		[[nodiscard]] bool SubmitShadow(
			RHI::ICommandList& commandList,
			const Scene& scene,
			const DrawContext& context,
			RHI::ResourceSetHandle resourceSet);

		std::vector<SceneMeshState> m_meshes;
		std::vector<uint32_t> m_skinOffsets;
		std::vector<RHI::BufferHandle> m_morphVertices;
		RHI::BufferHandle m_influences = nullptr;
		RHI::BufferHandle m_palette = nullptr;
	};
}
