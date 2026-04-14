#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "Core/Types.h"
#include "RHI/Enums.h"

namespace dy::RHI
{
	class IDevice;
	class ITexture;
	class IPipelineState;
}

namespace dy::Graphics
{
	class Scene;

	struct RendererConfig
	{
		const char* vertexShaderPath = nullptr;
		const char* pixelShaderPath = nullptr;
		RHI::Format renderTargetFormat = RHI::Format::R8G8B8A8_UNORM;
		RHI::Format depthStencilFormat = RHI::Format::Unknown;
		Math::float4 clearColor = Math::float4(0.08f, 0.10f, 0.14f, 1.0f);
	};

	class Renderer
	{
	public:
		Renderer() = default;
		~Renderer() = default;

		bool Initialize(RHI::IDevice* device, const RendererConfig& config = {});
		void Shutdown(RHI::IDevice* device);
		void Render(const Scene& scene, RHI::IDevice* device);

	private:
		static constexpr uint32_t kInvalidDescriptorIndex = 0xFFFFFFFFu;

		struct SceneTextureState
		{
			RHI::ITexture* texture = nullptr;
			uint32_t descriptorIndex = kInvalidDescriptorIndex;
		};

		struct DrawConstants
		{
			Math::float4x4 worldMatrix;
			Math::float4 baseColor;
			uint32_t baseColorTextureIndex = kInvalidDescriptorIndex;
			float padding[3] = {};
		};

		void BuildPipelineStates(RHI::IDevice* device);
		void PrepareSceneResources(const Scene& scene, RHI::IDevice* device);
		void RecordScenePass(const Scene& scene, RHI::IDevice* device);
		void EnsureTextureStateCapacity(std::size_t textureCount);

		RendererConfig m_config = {};
		std::vector<char> m_vertexShaderSource;
		std::vector<char> m_pixelShaderSource;
		RHI::IPipelineState* m_texturedTrianglePipeline = nullptr;
		std::vector<SceneTextureState> m_textureStates;
	};
}
