#pragma once
#include "VulkanContext.h"
#include "RHI/IPipelineState.h"

namespace dy::Backends
{

class VulkanPipeline {
public:
    void Initialize(
        const VulkanContext& context,
        const VkFormat* colorAttachmentFormats,
        uint32_t colorAttachmentCount,
        VkFormat depthAttachmentFormat,
        VkDescriptorSetLayout descriptorSetLayout,
        const dy::RHI::GraphicsPipelineDesc& desc,
        VkDescriptorSetLayout bindlessDescriptorSetLayout = VK_NULL_HANDLE);
    void Cleanup(VkDevice device);

    VkPipeline GetPipeline() const { return m_graphicsPipeline; }
    VkPipelineLayout GetLayout() const { return m_pipelineLayout; }

private:
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_graphicsPipeline = VK_NULL_HANDLE;
};

}
