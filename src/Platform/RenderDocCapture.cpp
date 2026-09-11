#include "Platform/RenderDocCapture.h"

#include <iostream>
#include <mutex>

#if defined(DY_RENDERDOC_ENABLED)
	#include <renderdoc_app.h>
	#if defined(_WIN32)
		#ifndef NOMINMAX
			#define NOMINMAX
		#endif
		#include <Windows.h>
	#elif defined(__linux__)
		#include <dlfcn.h>
	#endif
#endif

namespace dy::Platform
{
namespace
{
	std::once_flag g_initializeOnce;
	bool g_available = false;

#if defined(DY_RENDERDOC_ENABLED)
	RENDERDOC_API_1_1_2* g_renderDocApi = nullptr;

	void InitializeRenderDocApi()
	{
		pRENDERDOC_GetAPI getApi = nullptr;
	#if defined(_WIN32)
		if(HMODULE module = GetModuleHandleA("renderdoc.dll"))
		{
			getApi = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(module, "RENDERDOC_GetAPI"));
		}
	#elif defined(__linux__)
		if(void* module = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD))
		{
			getApi = reinterpret_cast<pRENDERDOC_GetAPI>(dlsym(module, "RENDERDOC_GetAPI"));
			dlclose(module);
		}
	#endif

		if(getApi != nullptr && getApi(eRENDERDOC_API_Version_1_1_2, reinterpret_cast<void**>(&g_renderDocApi)) == 1)
		{
			g_available = g_renderDocApi != nullptr;
		}
		std::cout << (g_available
			? "[dy_engine] RenderDoc capture ready (F12 captures the next frame).\n"
			: "[dy_engine] RenderDoc API not injected; F12 capture is inactive.\n");
	}
#else
	void InitializeRenderDocApi() {}
#endif
}

bool RenderDocCapture::Initialize()
{
	std::call_once(g_initializeOnce, InitializeRenderDocApi);
	return g_available;
}

bool RenderDocCapture::IsAvailable()
{
	return Initialize();
}

bool RenderDocCapture::TriggerNextFrame()
{
	if(!Initialize()) return false;
#if defined(DY_RENDERDOC_ENABLED)
	g_renderDocApi->TriggerCapture();
	std::cout << "[dy_engine] RenderDoc capture requested for the next frame.\n";
	return true;
#else
	return false;
#endif
}
}
