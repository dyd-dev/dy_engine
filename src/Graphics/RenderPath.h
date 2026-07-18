#pragma once
/* RenderPath
 *
 * 바인딩 전략(per-draw bind / batched bind / bindless)을 캡슐화하는 인터페이스.
 * Renderer 는 공유 리소스(파이프라인, 조명/그림자 버퍼, GpuScene, 머티리얼 상태)만
 * 소유하고, 지오메트리·드로우 리소스의 레지던시와 메인 패스 기록은 path 가 담당한다.
 * 이렇게 해서 단일 Renderer 가 갓 클래스가 되지 않고 전략별로 응집된다.
 */
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "Core/Types.h"
#include "Graphics/GpuScene.h"
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

	inline constexpr uint32_t kInvalidDescriptorIndex = 0xFFFFFFFFu;

	// 머티리얼 텍스처 슬롯 인덱스(셰이더 레이아웃의 텍스처 순서와 일치).
	inline constexpr uint32_t kMaterialBaseColorTextureSlot = 0;
	inline constexpr uint32_t kMaterialMetallicRoughnessTextureSlot = 1;
	inline constexpr uint32_t kMaterialNormalTextureSlot = 2;
	inline constexpr uint32_t kMaterialOcclusionTextureSlot = 3;
	inline constexpr uint32_t kMaterialEmissiveTextureSlot = 4;

	// 머티리얼별로 해석된 GPU 텍스처/디스크립터 인덱스 캐시(per-draw / batched path 가 공유).
	struct SceneMaterialState
	{
		SceneMaterialState()
		{
			textureDescriptorIndices.fill(kInvalidDescriptorIndex);
		}

		std::array<RHI::ITexture*, RendererShaderLayout::kMaterialTextureBindingCount> textures = {};
		std::array<uint32_t, RendererShaderLayout::kMaterialTextureBindingCount> textureDescriptorIndices = {};
		uint32_t textureFlags = 0;
	};

	// Renderer 가 path 에 넘기는 공유 리소스 묶음(소유권은 Renderer 가 유지).
	struct RenderPathContext
	{
		const RendererDesc* config = nullptr;
		RHI::IPipelineState* pipeline = nullptr;
		RHI::ITexture* depthStencil = nullptr;
		RHI::IBuffer* lightingBuffer = nullptr;
		RHI::IBuffer* shadowMatrixBuffer = nullptr;
		GpuScene* gpuScene = nullptr;
		const std::vector<SceneMaterialState>* materialStates = nullptr;

		// 둘 다 non-null 이면 RenderPath 가 메인 패스 전에 깊이 전용 패스를 기록한다.
		RHI::IPipelineState* shadowPipeline = nullptr;
		RHI::ITexture* shadowDepth = nullptr;
	};

	class IRenderPath
	{
	public:
		virtual ~IRenderPath() = default;

		// 씬 지오메트리/드로우 리소스를 GPU 에 준비한다(전략별 레이아웃).
		virtual void PrepareResources(const Scene& scene, RHI::IDevice* device, const RenderPathContext& context) = 0;
		// 메인 포워드 패스의 드로우 명령을 기록/제출한다.
		virtual void RecordMainPass(const Scene& scene, RHI::IDevice* device, const RenderPathContext& context) = 0;
		// 보유한 GPU 리소스를 해제한다.
		virtual void Shutdown(RHI::IDevice* device) = 0;
	};

	[[nodiscard]] std::unique_ptr<IRenderPath> CreateRenderPath(RendererBindingMode bindingMode);
}
