#pragma once

#include "ResourceHandles.h"
#include "ResourceState.h"
#include "Texture.h"

namespace dyf::RHI
{
	struct ResourceBarrierDesc
	{
		// before는 실제 현재 상태와 정확히 일치해야 한다.
		// Undefined는 자원의 최초 상태이며 현재 상태를 무시하거나 내용을 폐기하는 wildcard가 아니다.
		// before == after도 유효하다(Undefined 제외). 상태를 유지하면서 해당 자원의
		// 앞선 접근과 뒤따르는 접근 사이에 필요한 메모리 의존성을 보장한다.
		// 같은 CopyDestination/RenderTarget/DepthWrite의 연속 쓰기에도 사용한다.
		// 읽기 전용 접근만 있거나 네이티브 API가 이미 순서를 보장하면 추가 명령을 생략할 수 있다.
		BufferHandle buffer = nullptr;
		TextureHandle texture = nullptr;
		ResourceState before = ResourceState::Undefined;
		ResourceState after = ResourceState::Undefined;
		TextureSubresourceRange subresources = {};
	};
}
