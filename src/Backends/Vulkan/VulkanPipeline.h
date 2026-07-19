#pragma once
#include <string>

#include "VulkanContext.h"
#include "RHI/IPipelineState.h"

namespace dy::Backends
{

class VulkanShader final : public dy::RHI::IShader {
public:
    VulkanShader(VkDevice device, const dy::RHI::ShaderDesc& desc);
    ~VulkanShader() override;

    [[nodiscard]] dy::RHI::ShaderStage GetStage() const { return m_stage; }
    [[nodiscard]] const char* GetEntryPoint() const { return m_entryPoint.c_str(); }
    [[nodiscard]] VkShaderModule GetModule() const { return m_module; }

private:
    VkDevice m_device = VK_NULL_HANDLE;
    VkShaderModule m_module = VK_NULL_HANDLE;
    dy::RHI::ShaderStage m_stage = dy::RHI::ShaderStage::Unknown;
    std::string m_entryPoint;
};

class VulkanPipeline {
public:
    void Initialize(
        const VulkanContext& context,
        VkRenderPass renderPass,
        VkExtent2D extent,
        VkDescriptorSetLayout descriptorSetLayout,
        const dy::RHI::GraphicsPipelineDesc& desc,
        uint32_t pushConstantSize,
        VkDescriptorSetLayout bindlessDescriptorSetLayout = VK_NULL_HANDLE);
    void Cleanup(VkDevice device);

    VkPipeline GetPipeline() const { return m_graphicsPipeline; }
    VkPipelineLayout GetLayout() const { return m_pipelineLayout; }

private:
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_graphicsPipeline = VK_NULL_HANDLE;
};

}
