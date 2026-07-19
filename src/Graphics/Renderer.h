#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "Core/Types.h"
#include "Graphics/GpuScene.h"
#include "Graphics/RenderPass.h"
#include "Graphics/RenderPath.h"
#include "Graphics/RendererConfig.h"
#include "Graphics/RendererShaderLayout.h"

namespace dy::RHI
{
	class IBuffer;
	class IDevice;
	class IPipelineState;
	class IShader;
	class ITexture;
}

namespace dy::Graphics
{
	class Scene;

	class ISceneRenderer
	{
	public:
		virtual ~ISceneRenderer() = default;

		virtual bool Initialize(RHI::IDevice* device, const RendererDesc& desc = {}) = 0;
		virtual void Shutdown(RHI::IDevice* device) = 0;
		virtual void Render(const Scene& scene, RHI::IDevice* device) = 0;
	};

	// 단일 Renderer. 바인딩 전략은 IRenderPath 로 위임하고, 공유 리소스와
	// 렌더 패스 플랜만 직접 소유한다(전략별 코드는 RenderPath.cpp).
	class Renderer : public ISceneRenderer
	{
	public:
		Renderer() = default;
		explicit Renderer(RendererBindingMode bindingMode);
		explicit Renderer(const RendererDesc& desc);
		~Renderer() = default;

		bool Initialize(RHI::IDevice* device, const RendererDesc& desc = {}) override;
		void Shutdown(RHI::IDevice* device) override;
		void Render(const Scene& scene, RHI::IDevice* device) override;
		
		void SetCamera(const CameraDesc& camera); // 고수준 카메라 설정: RHI 공통 clip-space의 view·proj와 cameraPosition 생성.
		void SetViewProjection(const Math::float4x4& viewProjection); // 저수준(deep) 우회: 행렬/위치를 직접 지정.
		void SetCameraPosition(const Math::float3& cameraPosition);
		void SetDirectionalLight(const Math::float3& direction, const Math::float3& color, float intensity);
		void SetAmbientLight(const Math::float3& color, float intensity);
		void SetPBR(const PBRDesc& pbr);
		void SetEnvironmentLight(const EnvironmentDesc& environment);

	private:
		[[nodiscard]] bool BuildPipelineStates(RHI::IDevice* device);
		void BuildRenderPassPlan();
		void EnsureDepthStencilTarget(RHI::IDevice* device);
		void EnsureShadowDepthTarget(RHI::IDevice* device);
		void EnsureMaterialStateCapacity(std::size_t materialCount);
		void UpdateMaterialStates(const Scene& scene);
		void UpdateLightingBuffer(const Scene& scene, RHI::IDevice* device);
		void UpdateShadowBuffer(const Scene& scene, RHI::IDevice* device);
		[[nodiscard]] bool IsRenderPassEnabled(RenderPassKind passKind) const;
		[[nodiscard]] bool IsShadowEnabled() const;

		RendererDesc m_config = {};
		std::vector<RenderPassDesc> m_renderPasses;
		std::vector<char> m_vertexShaderCode;
		std::vector<char> m_fragmentShaderCode;
		std::vector<char> m_shadowVertexShaderCode;
		RHI::IPipelineState* m_pipeline = nullptr;
		RHI::IPipelineState* m_shadowPipeline = nullptr;
		RHI::IShader* m_vertexShader = nullptr;
		RHI::IShader* m_fragmentShader = nullptr;
		RHI::IShader* m_shadowVertexShader = nullptr;
		RHI::ITexture* m_depthStencilTarget = nullptr;
		RHI::ITexture* m_shadowDepthTarget = nullptr;
		RHI::IBuffer* m_lightingBuffer = nullptr;
		RHI::IBuffer* m_shadowMatrixBuffer = nullptr;
		uint32_t m_shadowDescriptorIndex = 0xFFFFFFFFu;
		GpuScene m_gpuScene;
		std::vector<SceneMaterialState> m_materialStates;
		std::unique_ptr<IRenderPath> m_path;
		RendererBindingMode m_initialBindingMode = RendererBindingMode::PerDrawBind;
		bool m_hasInitialConfig = false;
	};
}
