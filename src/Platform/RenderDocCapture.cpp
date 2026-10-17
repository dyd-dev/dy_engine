#include "dyf/Platform/RenderDocCapture.h"

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

namespace dyf::Platform
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

bool RenderDocCapture::IsEnabled()
{
#if defined(DY_RENDERDOC_ENABLED)
	return true;
#else
	return false;
#endif
}

bool RenderDocCapture::IsReplayUIConnected()
{
#if defined(DY_RENDERDOC_ENABLED)
	return Initialize() && g_renderDocApi->IsTargetControlConnected && g_renderDocApi->IsTargetControlConnected();
#else
	return false;
#endif
}

bool RenderDocCapture::TriggerNextFrame()
{
	if(!Initialize()) return false;
#if defined(DY_RENDERDOC_ENABLED)
	if(!g_renderDocApi->TriggerCapture) return false;
	g_renderDocApi->TriggerCapture();
	std::cout << "[dy_engine] RenderDoc capture requested for the next frame.\n";
	return true;
#else
	return false;
#endif
}

std::uint32_t RenderDocCapture::GetCaptureCount()
{
#if defined(DY_RENDERDOC_ENABLED)
	if(Initialize() && g_renderDocApi->GetNumCaptures) return g_renderDocApi->GetNumCaptures();
#endif
	return 0;
}

RenderDocCapture::ReplayUIResult RenderDocCapture::ConnectReplayUI()
{
#if defined(DY_RENDERDOC_ENABLED)
	if(!Initialize()) return ReplayUIResult::Unavailable;
	if(IsReplayUIConnected()) return ReplayUIResult::AlreadyConnected;
	if(g_renderDocApi->LaunchReplayUI && g_renderDocApi->LaunchReplayUI(1, nullptr))
		return ReplayUIResult::Launched;
	return ReplayUIResult::LaunchFailed;
#else
	return ReplayUIResult::Unavailable;
#endif
}

RenderDocCapture::ReplayUIResult RenderDocCapture::OpenReplayUI()
{
	if(!Initialize()) return ReplayUIResult::Unavailable;
	if(!GetCaptureCount()) return ReplayUIResult::NoCaptures;
	return ConnectReplayUI();
}
}
