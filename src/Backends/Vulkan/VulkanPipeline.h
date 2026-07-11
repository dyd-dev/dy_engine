#pragma once
#include "VulkanContext.h"
#include "RHI/IPipelineState.h"

namespace dy::Backends
{

class VulkanPipeline {
public:
    void Initialize(
        const VulkanContext& context,
        VkRenderPass renderPass,
        VkExtent2D extent,
        VkDescriptorSetLayout descriptorSetLayout,
        const dy::RHI::GraphicsPipelineDesc& desc,
        VkDescriptorSetLayout bindlessDescriptorSetLayout = VK_NULL_HANDLE);
    void Cleanup(VkDevice device);

    VkPipeline GetPipeline() const { return m_graphicsPipeline; }
    VkPipelineLayout GetLayout() const { return m_pipelineLayout; }

private:
    VkShaderModule CreateShaderModule(VkDevice device, const void* shaderCode, size_t shaderSize);

    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_graphicsPipeline = VK_NULL_HANDLE;
};

}
