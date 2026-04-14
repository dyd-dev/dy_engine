#include "VulkanPipeline.h"
#include "Graphics/Mesh.h"
#include <fstream>
#include <stdexcept>

void VulkanPipeline::Initialize(const VulkanContext& context, VkFormat swapchainFormat, VkExtent2D extent, VkDescriptorSetLayout descriptorSetLayout, const std::string& shaderDir) {
    // 1. Render Pass
    VkAttachmentDescription colorAttachment = {
        .format = swapchainFormat,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
    };

    VkAttachmentReference colorAttachmentRef = { .attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &colorAttachmentRef };
    VkSubpassDependency dependency = {
        .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
    };

    VkRenderPassCreateInfo renderPassInfo = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &colorAttachment,
        .subpassCount = 1, .pSubpasses = &subpass,
        .dependencyCount = 1, .pDependencies = &dependency
    };

    if (vkCreateRenderPass(context.device, &renderPassInfo, nullptr, &m_renderPass) != VK_SUCCESS) {
        throw std::runtime_error("failed to create render pass!");
    }

    // 2. Graphics Pipeline
    VkShaderModule vertShaderModule = LoadShaderModule(context.device, shaderDir + "/triangle.vert.spv");
    VkShaderModule fragShaderModule = LoadShaderModule(context.device, shaderDir + "/triangle.frag.spv");

    VkPipelineShaderStageCreateInfo shaderStages[] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vertShaderModule, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fragShaderModule, .pName = "main" }
    };

    VkVertexInputBindingDescription bindingDescription = { .binding = 0, .stride = sizeof(dy::Graphics::Vertex), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attributeDescriptions[] = {
        { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(dy::Graphics::Vertex, position) },
        { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(dy::Graphics::Vertex, normal) },
        { .location = 2, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(dy::Graphics::Vertex, uv) }
    };

    VkPipelineVertexInputStateCreateInfo vertexInputInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &bindingDescription,
        .vertexAttributeDescriptionCount = 3, .pVertexAttributeDescriptions = attributeDescriptions
    };

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkViewport viewport = { .x = 0.0f, .y = 0.0f, .width = (float)extent.width, .height = (float)extent.height, .minDepth = 0.0f, .maxDepth = 1.0f };
    VkRect2D scissor = { .offset = {0, 0}, .extent = extent };
    VkPipelineViewportStateCreateInfo viewportState = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor };
    VkPipelineRasterizationStateCreateInfo rasterizer = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_BACK_BIT, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo multisampling = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState colorBlendAttachment = { .blendEnable = VK_TRUE, .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .colorBlendOp = VK_BLEND_OP_ADD, .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO, .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT };
    VkPipelineColorBlendStateCreateInfo colorBlending = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &colorBlendAttachment };
    
    VkPushConstantRange pushConstantRange = { .stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .offset = 0, .size = sizeof(float) };

    VkPipelineLayoutCreateInfo pipelineLayoutInfo = { 
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 
        .setLayoutCount = 1, .pSetLayouts = &descriptorSetLayout,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &pushConstantRange
    };

    if (vkCreatePipelineLayout(context.device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("failed to create pipeline layout!");
    }

    VkGraphicsPipelineCreateInfo pipelineInfo = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = shaderStages, .pVertexInputState = &vertexInputInfo, .pInputAssemblyState = &inputAssembly,
        .pViewportState = &viewportState, .pRasterizationState = &rasterizer, .pMultisampleState = &multisampling,
        .pColorBlendState = &colorBlending, .layout = m_pipelineLayout, .renderPass = m_renderPass, .subpass = 0
    };

    if (vkCreateGraphicsPipelines(context.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_graphicsPipeline) != VK_SUCCESS) {
        throw std::runtime_error("failed to create graphics pipeline!");
    }

    vkDestroyShaderModule(context.device, fragShaderModule, nullptr);
    vkDestroyShaderModule(context.device, vertShaderModule, nullptr);
}

void VulkanPipeline::Cleanup(VkDevice device) {
    if (m_graphicsPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_graphicsPipeline, nullptr);
        m_graphicsPipeline = VK_NULL_HANDLE;
    }
    if (m_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
    if (m_renderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device, m_renderPass, nullptr);
        m_renderPass = VK_NULL_HANDLE;
    }
}

VkShaderModule VulkanPipeline::LoadShaderModule(VkDevice device, const std::string& filename) {
    std::ifstream file(filename, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("failed to open shader file!");
    }

    size_t fileSize = (size_t)file.tellg();
    std::vector<char> buffer(fileSize);
    file.seekg(0);
    file.read(buffer.data(), fileSize);
    file.close();

    VkShaderModuleCreateInfo createInfo = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = buffer.size(),
        .pCode = reinterpret_cast<const uint32_t*>(buffer.data())
    };

    VkShaderModule shaderModule;
    if (vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        throw std::runtime_error("failed to create shader module!");
    }

    return shaderModule;
}
