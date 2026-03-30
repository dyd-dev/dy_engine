/* RHIDevice.h

GPU와 통신하는 최상위 클래스입니다.
스왑 체인, 화면 Present, 텍스처/버퍼 같은 리소스 관리, 렌더링 명령을 GPU에 전달하는 역할을 합니다.
*/
#pragma once
#include <cstdint>

class RHIBuffer;
class RHITexture;
class RHICommandList;

struct WindowHandle { void* windowPointer; };

class RHIDevice
{
public:
	virtual ~RHIDevice() = default;

	virtual bool Initialize(const WindowHandle& windowHandle, uint32_t width, uint32_t height) = 0;

	virtual void BeginFrame() = 0;
	virtual void EndFrame() = 0;

	virtual void SubmitCommandList(RHICommandList* cmd) = 0;
	virtual void Present() = 0;

	virtual RHICommandList* GetCommandList() = 0;

	// 자원 생성
	//virtual RHIBuffer* CreateBuffer(uint32_t size, const void* initialData) = 0;
	//virtual RHITexture* CreateTexture(const char* filePath) = 0;
	
	// 스왑 체인을 생성하는 함수입니다. 스왑 체인은 화면에 렌더링된 이미지를 표시하는 데 사용됩니다.
	//virtual void CreateSwapChain() = 0;
	// 명령 리스트를 생성하는 함수입니다. 명령 리스트는 GPU에 렌더링 명령을 기록하는 객체입니다.
	//virtual void CreateCommandList() = 0;
};