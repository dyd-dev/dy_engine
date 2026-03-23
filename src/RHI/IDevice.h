/* Device.h

* GPU와 통신하는 최상위 클래스입니다.
* 물리/논리 디바이스 초기화, 메모리 할당자 역할을 합니다.
*/
#pragma once

namespace dy::RHI
{
	class IBuffer;
	class ITexture;
	class ICommandList;

	class IDevice
	{
	public:
		static IDevice* Create(const void *windowHandle);
		virtual ~IDevice() = default;

		virtual void BeginFrame() = 0;
		virtual void EndFrame() = 0;

		virtual void SubmitCommandList(ICommandList* cmd) = 0;
		virtual void Present() = 0;

		virtual ICommandList* GetCommandList() = 0;

		// 자원 생성
		//virtual IBuffer* CreateBuffer(uint32_t size, const void* initialData) = 0;
		//virtual ITexture* CreateTexture(const char* filePath) = 0;

		// 스왑 체인을 생성하는 함수입니다. 스왑 체인은 화면에 렌더링된 이미지를 표시하는 데 사용됩니다.
		//virtual void CreateSwapChain() = 0;
		// 명령 리스트를 생성하는 함수입니다. 명령 리스트는 GPU에 렌더링 명령을 기록하는 객체입니다.
		//virtual void CreateCommandList() = 0;
		
	protected:
		virtual int Initialize(const void *windowHandle) = 0;
	};
}