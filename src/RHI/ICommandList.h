/* RHICommandList.h
* 
* 그리기 명령, 파이프라인 배리어를 기록하는 객체입니다.
* CPU에서 명령을 작성하고 GPU가 이를 실행합니다.
*/
#pragma once
#include <cstdint>

namespace dy::RHI
{
	class IBuffer;
	class ITexture;
	class IPipelineState;

	class ICommandList
	{
	public:
		virtual ~ICommandList() = default;
		
		// 명령 리스트를 시작합니다. 이 함수는 명령 리스트를 초기화하고, 명령을 기록할 준비를 합니다.
		virtual void Begin() = 0;
		//
		virtual void ClearScreen(float r, float g, float b, float a) = 0;
		// 명령 리스트를 종료합니다. 이 함수는 명령 리스트에 기록된 명령을 완료하고, GPU가 이를 실행할 수 있도록 합니다.
		virtual void End() = 0;
		// 명령 리스트에 렌더링 명령을 기록하는 함수입니다. 예를 들어, 드로우 콜, 버퍼 바인딩, 텍스처 바인딩 등의 명령을 기록할 수 있습니다.
		//virtual void Draw() = 0;

		//virtual void SetPipelineState(IPipelineState *pipeline) = 0;
		//virtual void SetVertexBuffer(IBuffer *vertexBuffer) = 0;

		//virtual void DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex, uint32_t startInstance) = 0;
	};
}