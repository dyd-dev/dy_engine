#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "Core/Types.h"
#include "Graphics/ShadowMap.h"
#include "RHI/Enums.h"

namespace dy::RHI
{
	class IBuffer;
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
		const char* shadowVertexShaderPath = nullptr;
		RHI::Format renderTargetFormat = RHI::Format::R8G8B8A8_UNORM;
		RHI::Format depthStencilFormat = RHI::Format::D32_FLOAT;
		Math::float4 clearColor = Math::float4(0.08f, 0.10f, 0.14f, 1.0f);
		Math::float4x4 viewProjectionMatrix = Math::float4x4::Identity();
		Math::float3 cameraPosition = Math::float3(0.0f, 0.0f, 2.2f);
		Math::float3 directionalLightDirection = Math::float3(0.35f, 0.65f, 0.68f);
		Math::float3 directionalLightColor = Math::float3(1.0f, 0.94f, 0.82f);
		float directionalLightIntensity = 4.0f;
		Math::float3 ambientColor = Math::float3(1.0f, 1.0f, 1.0f);
		float ambientIntensity = 0.035f;
		bool enableShadows = false;
		float shadowStrength = 0.45f;
		ShadowMapDesc shadowMap = {};
	};

	class Renderer
	{
	public:
		Renderer() = default;
		~Renderer() = default;

		bool Initialize(RHI::IDevice* device, const RendererConfig& config = {});
		void Shutdown(RHI::IDevice* device);
		void Render(const Scene& scene, RHI::IDevice* device);
		void SetViewProjection(const Math::float4x4& viewProjection);
		void SetCameraPosition(const Math::float3& cameraPosition);
		void SetDirectionalLight(const Math::float3& direction, const Math::float3& color, float intensity);
		void SetAmbientLight(const Math::float3& color, float intensity);

	private:
		static constexpr uint32_t kInvalidDescriptorIndex = 0xFFFFFFFFu;

		struct SceneTextureState
		{
			RHI::ITexture* texture = nullptr;
			uint32_t descriptorIndex = kInvalidDescriptorIndex;
		};

		struct SceneMeshState
		{
			RHI::IBuffer* vertexBuffer = nullptr;
			RHI::IBuffer* indexBuffer = nullptr;
			uint32_t vertexBytes = 0;
			uint32_t indexBytes = 0;
			uint32_t indexCount = 0;
		};

		struct RendererLightingConstants
		{
			Math::float4 cameraPosition;
			Math::float4 directionalLightDirection;
			Math::float4 directionalLightColor;
			Math::float4 ambientColor;
		};

		struct RendererShadowConstants
		{
			Math::float4x4 lightViewProjectionMatrix;
		};

		struct DrawConstants
		{
			Math::float4x4 viewProjectionMatrix;
			Math::float4x4 modelMatrix;
			float drawMode = 0.0f;
			uint32_t firstIndex = 0;
			int32_t vertexOffset = 0;
			uint32_t firstVertex = 0;
			Math::float3 emissiveColor = Math::float3(0.0f, 0.0f, 0.0f);
			Math::float4 baseColor;
			Math::float4 materialParams;
		};

		void BuildPipelineStates(RHI::IDevice* device);
		void PrepareSceneResources(const Scene& scene, RHI::IDevice* device);
		void RecordScenePass(const Scene& scene, RHI::IDevice* device);
		void EnsureTextureStateCapacity(std::size_t textureCount);
		void EnsureMeshStateCapacity(std::size_t meshCount);
		void UpdateLightingBuffer(RHI::IDevice* device);
		void UpdateShadowBuffer(RHI::IDevice* device);
		void DestroyMeshState(RHI::IDevice* device, SceneMeshState& meshState);
		[[nodiscard]] bool IsShadowEnabled() const;

		RendererConfig m_config = {};
		std::vector<char> m_vertexShaderSource;
		std::vector<char> m_pixelShaderSource;
		std::vector<char> m_shadowVertexShaderSource;
		RHI::IPipelineState* m_texturedTrianglePipeline = nullptr;
		RHI::IBuffer* m_lightingBuffer = nullptr;
		RHI::IBuffer* m_shadowMatrixBuffer = nullptr;
		std::vector<SceneTextureState> m_textureStates;
		std::vector<SceneMeshState> m_meshStates;
	};
}
