#pragma once
#include "VulkanContext.h"
#include <vector>
#include <string>

class VulkanPipeline {
public:
    void Initialize(const VulkanContext& context, VkFormat swapchainFormat, VkExtent2D extent, VkDescriptorSetLayout descriptorSetLayout, const std::string& shaderDir, bool useVertexInput = false);
    void Cleanup(VkDevice device);

    VkRenderPass GetRenderPass() const { return m_renderPass; }
    VkPipeline GetPipeline() const { return m_graphicsPipeline; }
    VkPipelineLayout GetLayout() const { return m_pipelineLayout; }

private:
    VkShaderModule LoadShaderModule(VkDevice device, const std::string& filename);

    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_graphicsPipeline = VK_NULL_HANDLE;
};

