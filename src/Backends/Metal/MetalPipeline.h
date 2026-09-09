#pragma once

#include <cstdint>
#include <vector>

#include "RHI/Pipeline.h"

namespace dy::Backends
{
	struct MetalObjectDeleter;

	struct MetalStaticSamplerBinding
	{
		uint32_t index = 0;
		RHI::ShaderStageFlags stages = RHI::ShaderStageFlags::None;
		void* sampler = nullptr;
	};

	class MetalPipeline final : public RHI::Pipeline
	{
	public:
		MetalPipeline(const RHI::GraphicsPipelineDesc& desc, void* device);

		[[nodiscard]] const RHI::GraphicsPipelineDesc& GetDesc() const;
		[[nodiscard]] void* GetNativePipeline() const;
		[[nodiscard]] void* GetNativeDepthStencil() const;
		[[nodiscard]] uint32_t GetNativePrimitiveType() const;
		[[nodiscard]] uint32_t GetNativeCullMode() const;
		[[nodiscard]] uint32_t GetNativeFrontFace() const;
		[[nodiscard]] uint32_t GetNativeFillMode() const;
		[[nodiscard]] const std::vector<MetalStaticSamplerBinding>&
			GetStaticSamplerBindings() const;

	private:
		friend struct MetalObjectDeleter;

		~MetalPipeline() override;

		struct Impl;
		Impl* m_impl = nullptr;
		RHI::GraphicsPipelineDesc m_desc = {};
		std::vector<RHI::VertexBufferLayout> m_vertexBuffers;
		std::vector<RHI::VertexAttribute> m_vertexAttributes;
		std::vector<RHI::ColorAttachmentDesc> m_colorAttachments;
	};
}
