/* Resource.h
* 
* Resource는 그래픽스 리소스를 나타내는 클래스입니다.
* 텍스처, 버퍼, 쉐이더 리소스 뷰(SRV), 렌더 타겟 뷰(RTV) 등과 같은 다양한 그래픽스 리소스를 관리하는 역할을 합니다.
*/
#pragma once
#include <cstdint>

namespace dy::RHI
{
	class IResource
	{
	public:
		virtual ~IResource() = default;
	};

	/* Buffer
	* Vertex, Index, Constant, Storage Buffer 등을 위한 버퍼 객체입니다. GPU 메모리에 데이터를 저장하고, 렌더링 과정에서 이를 참조할 수 있도록 합니다.
	* GPU 메모리에 데이터를 업로드하고, 필요한 경우 CPU에서 데이터를 읽거나 쓸 수 있도록 하는 기능을 제공합니다. 또한, 버퍼의 크기, 형식, 사용 용도 등을 정의할 수 있습니다.
	*/
	class Buffer : public IResource
	{
	public:
		virtual uint32_t GetSize() const = 0;
	};

	/* Texture
	* 
	* 1D/2D/3D Texture, Render Target, Depth Stencil 등을 할당합니다.
	* Texture는 이미지 데이터를 GPU 메모리에 저장하고, 셰이더에서 이를 참조하여 렌더링에 활용할 수 있도록 합니다.
	*/
	class ITexture : public IResource
	{
	public:
		virtual uint32_t GetWidth() const = 0;
		virtual uint32_t GetHeight() const = 0;
	};
}