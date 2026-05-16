#pragma once
#include <cstdint>
#include <vector>

#include "RenderTypes.h"
#include "RHI/Enums.h"

namespace dy::RHI
{
	class IDevice;
	class IPipelineState;
}

namespace dy::Graphics
{
	struct RendererConfig
	{
		Math::float4 clearColor = Math::float4(0.08f, 0.10f, 0.14f, 1.0f);
	};

	struct GraphicsPipelineFiles
	{
		const char* vertexShaderPath = nullptr;
		const char* pixelShaderPath = nullptr;
		RHI::Format renderTargetFormat = RHI::Format::R8G8B8A8_UNORM;
		RHI::Format depthStencilFormat = RHI::Format::Unknown;
		bool depthTestEnable = false;
		bool depthWriteEnable = false;
	};

	class Renderer
	{
	public:
		Renderer() = default;
		~Renderer() = default;

		bool Initialize(RHI::IDevice* device, const GraphicsPipelineFiles& mainPipeline, const RendererConfig& config = {});
		void Shutdown(RHI::IDevice* device);
		void Render(const Scene& scene, RHI::IDevice* device);

	private:
		static constexpr uint32_t kInvalidDescriptorIndex = 0xFFFFFFFFu;

		struct DrawConstants
		{
			Math::float4x4 worldMatrix;
			Math::float4 baseColor;
			uint32_t baseColorTextureIndex = kInvalidDescriptorIndex;
			float padding[3] = {};
		};

		struct DrawPacket
		{
			uint32_t meshIndex = 0;
			uint32_t materialIndex = 0;
			uint32_t transformIndex = 0;
			uint32_t vertexCount = 0;
		};

		bool BuildPipelineStates(RHI::IDevice* device, const GraphicsPipelineFiles& mainPipeline);
		void BuildDrawPackets(const Scene& scene);
		void RecordScenePass(const Scene& scene, RHI::IDevice* device);

		RendererConfig m_config = {};
		std::vector<char> m_mainVertexShader;
		std::vector<char> m_mainPixelShader;
		RHI::IPipelineState* m_mainPipeline = nullptr;
		std::vector<DrawPacket> m_drawPackets;
	};
}
