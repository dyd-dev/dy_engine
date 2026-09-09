#pragma once

#include "RHI/Pipeline.h"
#include "RHI/ResourceSet.h"
#include "RHI/Shader.h"
#include "VulkanContext.h"

#include <vector>

namespace dy::Backends
{
	struct VulkanObjectDeleter;

	class VulkanShader final : public dy::RHI::Shader
	{
	public:
		VulkanShader(const VulkanContext& context, const dy::RHI::ShaderDesc& desc);

		[[nodiscard]] VkShaderModule GetModule() const { return m_module; }

	private:
		friend struct VulkanObjectDeleter;

		~VulkanShader() override;

		VkDevice m_device = VK_NULL_HANDLE;
		VkShaderModule m_module = VK_NULL_HANDLE;
	};

	class VulkanPipeline final : public dy::RHI::Pipeline
	{
	public:
		VulkanPipeline(const VulkanContext& context, const dy::RHI::GraphicsPipelineDesc& desc);

		[[nodiscard]] VkPipeline GetHandle() const { return m_pipeline; }
		[[nodiscard]] VkPipelineLayout GetPipelineLayout() const { return m_pipelineLayout; }
		[[nodiscard]] VkDescriptorSetLayout GetSetLayout() const { return m_setLayout; }
		[[nodiscard]] const std::vector<dy::RHI::VertexBufferLayout>& GetVertexBuffers() const { return m_vertexBuffers; }
		[[nodiscard]] const std::vector<dy::RHI::Format>& GetColorFormats() const { return m_colorFormats; }
		[[nodiscard]] dy::RHI::Format GetDepthFormat() const { return m_depthFormat; }
		[[nodiscard]] bool UsesStencil() const { return m_usesStencil; }
		[[nodiscard]] bool RequiresDepthWrite() const { return m_requiresDepthWrite; }

	private:
		friend class VulkanResourceSet;
		friend struct VulkanObjectDeleter;

		~VulkanPipeline() override;

		void CreateDescriptorLayouts(const VulkanContext& context, const dy::RHI::PipelineLayoutDesc& desc);
		void CreatePipelineLayout(const dy::RHI::PipelineLayoutDesc& desc);
		void CreatePipeline(const dy::RHI::GraphicsPipelineDesc& desc);
		void Cleanup();

		VkDevice m_device = VK_NULL_HANDLE;
		VkPipeline m_pipeline = VK_NULL_HANDLE;
		VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
		VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
		std::vector<VkSampler> m_staticSamplers;
		std::vector<dy::RHI::VertexBufferLayout> m_vertexBuffers;
		std::vector<dy::RHI::Format> m_colorFormats;
		dy::RHI::Format m_depthFormat = dy::RHI::Format::Unknown;
		bool m_usesStencil = false;
		bool m_requiresDepthWrite = false;
	};

	class VulkanResourceSet final : public dy::RHI::ResourceSet
	{
	public:
		VulkanResourceSet(const VulkanContext& context, const dy::RHI::ResourceSetDesc& desc);

		[[nodiscard]] VulkanPipeline* GetVulkanPipeline() const { return m_pipeline; }
		[[nodiscard]] VkDescriptorSet GetSet() const { return m_set; }

	private:
		friend struct VulkanObjectDeleter;

		~VulkanResourceSet() override;

		VkDevice m_device = VK_NULL_HANDLE;
		VulkanPipeline* m_pipeline = nullptr;
		VkDescriptorPool m_pool = VK_NULL_HANDLE;
		VkDescriptorSet m_set = VK_NULL_HANDLE;
	};
}
