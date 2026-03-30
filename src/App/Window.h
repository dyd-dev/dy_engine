#pragma once
#include "RHI/RHIDevice.h"

struct GLFWwindow;

class Window
{
public:
	bool Initialize(const char* title, uint32_t width, uint32_t height, uint32_t flags = 0);
	void Release();

	bool PollEvents();

	WindowHandle GetHandle() { return { m_window }; }

private:
	GLFWwindow* m_window;
};