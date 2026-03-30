#pragma once
#include "RHI/RHICommandList.h"
#include <vulkan/vulkan.h>

class VulkanCommandList : public RHICommandList
{
public:
	void Begin() override;
	void ClearScreen(float r, float g, float b, float a) override;
	void End() override;

	VkClearColorValue GetClearColor() const { return m_clearColor; }

private:
	VkClearColorValue m_clearColor = { { 0.4f, 0.7f, 1.0f, 1.0f } };
};
