#include "Window.h"
#include <GLFW/glfw3.h>
#include <cstdio>

bool Window::Initialize(const char* title, uint32_t width, uint32_t height, [[maybe_unused]] uint32_t flags)
{
	if (!glfwInit())
	{
		printf("Failed to initialize GLFW\n");
		return false;
	}

#if defined(ENABLE_VULKAN) || defined(ENABLE_D3D12)
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
#endif

	m_window = glfwCreateWindow(width, height, title, nullptr, nullptr);
	if (!m_window)
	{
		printf("Failed to create window\n");
		glfwTerminate();
		return false;
	}
	
	return true;
}

void Window::Release()
{
	if (m_window)
	{
		glfwDestroyWindow(m_window);
		m_window = nullptr;
	}
	glfwTerminate();
}

bool Window::PollEvents()
{
	glfwPollEvents();
	if (glfwWindowShouldClose(m_window))
	{
		return true;
	}

	if (glfwGetKey(m_window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
	{
		return true;
	}

	return false;
}