#pragma once

#include "dyf/RHI/Pipeline.h"
#include "dyf/RHI/ResourceSet.h"
#include "dyf/RHI/Shader.h"
#include "VulkanContext.h"

#include <vector>

namespace dyf::Backends
{
	struct VulkanObjectDeleter;

	class VulkanShader final : public dyf::RHI::Shader
	{
	public:
		VulkanShader(const VulkanContext& context, const dyf::RHI::ShaderDesc& desc);

		[[nodiscard]] VkShaderModule GetModule() const { return m_module; }

	private:
		friend struct VulkanObjectDeleter;

		~VulkanShader() override;

		VkDevice m_device = VK_NULL_HANDLE;
		VkShaderModule m_module = VK_NULL_HANDLE;
	};

	class VulkanPipeline final : public dyf::RHI::Pipeline
	{
	public:
		VulkanPipeline(const VulkanContext& context, const dyf::RHI::GraphicsPipelineDesc& desc);
        VulkanPipeline(const VulkanContext&,const RHI::ComputePipelineDesc&);

		[[nodiscard]] VkPipeline GetHandle() const { return m_pipeline; }
		[[nodiscard]] VkPipelineLayout GetPipelineLayout() const { return m_pipelineLayout; }
		[[nodiscard]] VkDescriptorSetLayout GetSetLayout() const { return m_setLayout; }
		[[nodiscard]] const std::vector<dyf::RHI::VertexBufferLayout>& GetVertexBuffers() const { return m_vertexBuffers; }
		[[nodiscard]] const std::vector<dyf::RHI::Format>& GetColorFormats() const { return m_colorFormats; }
		[[nodiscard]] dyf::RHI::Format GetDepthFormat() const { return m_depthFormat; }
		[[nodiscard]] bool UsesStencil() const { return m_usesStencil; }
		[[nodiscard]] bool RequiresDepthWrite() const { return m_requiresDepthWrite; }

	private:
		friend class VulkanResourceSet;
		friend struct VulkanObjectDeleter;

		~VulkanPipeline() override;

		void CreateDescriptorLayouts(const VulkanContext& context, const dyf::RHI::PipelineLayoutDesc& desc);
		void CreatePipelineLayout(const dyf::RHI::PipelineLayoutDesc& desc);
		void CreatePipeline(const dyf::RHI::GraphicsPipelineDesc& desc);
		void Cleanup();

		VkDevice m_device = VK_NULL_HANDLE;
		VkPipeline m_pipeline = VK_NULL_HANDLE;
		VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
		VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
		std::vector<VkSampler> m_staticSamplers;
		std::vector<dyf::RHI::VertexBufferLayout> m_vertexBuffers;
		std::vector<dyf::RHI::Format> m_colorFormats;
		dyf::RHI::Format m_depthFormat = dyf::RHI::Format::Unknown;
		bool m_usesStencil = false;
		bool m_requiresDepthWrite = false;
	};

	class VulkanResourceSet final : public dyf::RHI::ResourceSet
	{
	public:
		VulkanResourceSet(const VulkanContext& context, const dyf::RHI::ResourceSetDesc& desc);

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
