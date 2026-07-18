#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "Core/Types.h"
#include "Graphics/GpuScene.h"
#include "Graphics/PerFrameBufferSet.h"
#include "Graphics/RenderPass.h"
#include "Graphics/RenderPath.h"
#include "Graphics/RendererConfig.h"
#include "Graphics/RendererShaderLayout.h"

namespace dy::RHI
{
	class IBuffer;
	class IDevice;
	class IPipelineState;
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
		
		void SetCamera(const CameraDesc& camera); // 고수준 카메라 설정: view·proj·cameraPosition 생성 + 백엔드 Y 뒤집기 처리.
		void SetViewProjection(const Math::float4x4& viewProjection); // 저수준(deep) 우회: 행렬/위치를 직접 지정.
		void SetCameraPosition(const Math::float3& cameraPosition);
		void SetDirectionalLight(const Math::float3& direction, const Math::float3& color, float intensity);
		void SetAmbientLight(const Math::float3& color, float intensity);
		void SetPBR(const PBRDesc& pbr);
		void SetEnvironmentLight(const EnvironmentDesc& environment);

	private:
		void BuildPipelineStates(RHI::IDevice* device);
		void BuildRenderPassPlan();
		void EnsureDepthStencilTarget(RHI::IDevice* device);
		void EnsureHdrColorTarget(RHI::IDevice* device);
		void EnsureShadowDepthTarget(RHI::IDevice* device);
		void RecordToneMapPass(RHI::IDevice* device);
		void EnsureMaterialStateCapacity(std::size_t materialCount);
		void UpdateMaterialStates(const Scene& scene);
		void UpdateLightingBuffer(const Scene& scene, RHI::IDevice* device);
		void UpdateShadowBuffer(const Scene& scene, RHI::IDevice* device);
		[[nodiscard]] bool IsRenderPassEnabled(RenderPassKind passKind) const;
		[[nodiscard]] bool IsShadowEnabled() const;

		RendererDesc m_config = {};
		std::vector<RenderPassDesc> m_renderPasses;
		std::vector<char> m_vertexShaderSource;
		std::vector<char> m_pixelShaderSource;
		std::vector<char> m_shadowVertexShaderSource;
		std::vector<char> m_toneMapVertexShaderSource;
		std::vector<char> m_toneMapPixelShaderSource;
		RHI::IPipelineState* m_pipeline = nullptr;
		RHI::IPipelineState* m_shadowPipeline = nullptr;
		RHI::IPipelineState* m_toneMapPipeline = nullptr;
		RHI::ITexture* m_depthStencilTarget = nullptr;
		RHI::ITexture* m_hdrColorTarget = nullptr;
		RHI::ITexture* m_shadowDepthTarget = nullptr;
		PerFrameBufferSet m_lightingBuffers;
		PerFrameBufferSet m_shadowMatrixBuffers;
		uint32_t m_shadowDescriptorIndex = 0xFFFFFFFFu;
		bool m_useExplicitShadowPass = false;
		GpuScene m_gpuScene;
		std::vector<SceneMaterialState> m_materialStates;
		std::unique_ptr<IRenderPath> m_path;
		RendererBindingMode m_initialBindingMode = RendererBindingMode::PerDrawBind;
		bool m_hasInitialConfig = false;
		bool m_clipYFlip = false; // 백엔드 클립공간 Y 뒤집기 필요 여부(Initialize 에서 device 질의)
	};
}
