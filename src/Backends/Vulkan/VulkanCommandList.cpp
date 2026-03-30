#include "VulkanCommandList.h"

void VulkanCommandList::Begin()
{
	m_clearColor = { { 0.4f, 0.7f, 1.0f, 1.0f } };
}

void VulkanCommandList::ClearScreen(float r, float g, float b, float a)
{
	m_clearColor = { { r, g, b, a } };
}

void VulkanCommandList::End()
{
}
