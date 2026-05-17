#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "Core/Types.h"
#include "Graphics/RenderPass.h"
#include "Graphics/ShadowMap.h"
#include "RHI/Enums.h"
#include "RHI/RendererShaderLayout.h"

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

	struct PBRDesc
	{
		float minRoughness = 0.04f;
		float ambientSpecularStrength = 0.25f;
	};

	struct EnvironmentDesc
	{
		Math::float3 diffuseColor = Math::float3(1.0f, 1.0f, 1.0f);
		float diffuseIntensity = 1.0f;
		Math::float3 specularColor = Math::float3(1.0f, 1.0f, 1.0f);
		float specularIntensity = 1.0f;
	};

	struct RendererDesc
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
		PBRDesc pbr = {};
		EnvironmentDesc environment = {};
		bool enableShadows = false;
		bool enableMainPass = true;
		float shadowStrength = 0.45f;
		ShadowMapDesc shadowMap = {};
		bool autoFitShadowMap = true;
		float shadowBoundsPadding = 0.25f;
		float shadowDepthBias = 0.0007f;
		float shadowSlopeBias = 0.003f;
		float shadowNormalBias = 0.0f;
		uint32_t shadowPcfRadius = 1;
	};
	using RendererConfig = RendererDesc;

	class Renderer
	{
	public:
		Renderer() = default;
		~Renderer() = default;

		bool Initialize(RHI::IDevice* device, const RendererDesc& desc = {});
		void Shutdown(RHI::IDevice* device);
		void Render(const Scene& scene, RHI::IDevice* device);
		void SetViewProjection(const Math::float4x4& viewProjection);
		void SetCameraPosition(const Math::float3& cameraPosition);
		void SetDirectionalLight(const Math::float3& direction, const Math::float3& color, float intensity);
		void SetAmbientLight(const Math::float3& color, float intensity);
		void SetPBR(const PBRDesc& pbr);
		void SetEnvironmentLight(const EnvironmentDesc& environment);

	private:
		static constexpr uint32_t kInvalidDescriptorIndex = 0xFFFFFFFFu;

		struct SceneTextureState
		{
			RHI::ITexture* texture = nullptr;
			uint32_t descriptorIndex = kInvalidDescriptorIndex;
		};

		struct SceneMaterialState
		{
			std::array<RHI::ITexture*, RHI::RendererShaderLayout::kMaterialTextureBindingCount> textures = {};
			uint32_t textureFlags = 0;
		};

		struct SceneMeshState
		{
			RHI::IBuffer* vertexBuffer = nullptr;
			RHI::IBuffer* indexBuffer = nullptr;
			uint32_t vertexBytes = 0;
			uint32_t indexBytes = 0;
			uint32_t indexCount = 0;
		};

		void BuildPipelineStates(RHI::IDevice* device);
		void BuildRenderPassPlan();
		void PrepareSceneResources(const Scene& scene, RHI::IDevice* device);
		void ExecuteRenderPasses(const Scene& scene, RHI::IDevice* device);
		void ExecuteRenderPass(const RenderPassDesc& pass, const Scene& scene, RHI::IDevice* device);
		void PrepareShadowPass(const Scene& scene, RHI::IDevice* device);
		void PrepareMainForwardPass(const Scene& scene, RHI::IDevice* device);
		void RecordMainForwardPass(const Scene& scene, RHI::IDevice* device);
		void EnsureTextureStateCapacity(std::size_t textureCount);
		void EnsureMaterialStateCapacity(std::size_t materialCount);
		void EnsureMeshStateCapacity(std::size_t meshCount);
		void UpdateMaterialStates(const Scene& scene);
		void UpdateLightingBuffer(const Scene& scene, RHI::IDevice* device);
		void UpdateShadowBuffer(const Scene& scene, RHI::IDevice* device);
		void DestroyMeshState(RHI::IDevice* device, SceneMeshState& meshState);
		[[nodiscard]] RHI::ITexture* ResolveTexture(TextureID textureId) const;
		[[nodiscard]] bool IsRenderPassEnabled(RenderPassKind passKind) const;
		[[nodiscard]] bool IsShadowEnabled() const;

		RendererDesc m_config = {};
		std::vector<RenderPassDesc> m_renderPasses;
		std::vector<char> m_vertexShaderSource;
		std::vector<char> m_pixelShaderSource;
		std::vector<char> m_shadowVertexShaderSource;
		RHI::IPipelineState* m_texturedTrianglePipeline = nullptr;
		RHI::IBuffer* m_lightingBuffer = nullptr;
		RHI::IBuffer* m_shadowMatrixBuffer = nullptr;
		std::vector<SceneTextureState> m_textureStates;
		std::vector<SceneMaterialState> m_materialStates;
		std::vector<SceneMeshState> m_meshStates;
	};
}
