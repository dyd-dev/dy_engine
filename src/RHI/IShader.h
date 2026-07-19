#pragma once

#include <cstddef>
#include <cstdint>

namespace dy::RHI
{
	enum class ShaderStage : uint8_t
	{
		Unknown = 0,
		Vertex,
		Fragment
	};

	struct ShaderDesc
	{
		ShaderStage stage = ShaderStage::Unknown;
		// 선택한 Device Backend가 소비하는 native binary의 non-owning view다.
		// binary는 CreateShader 호출 동안만 유효하면 된다.
		const void* binary = nullptr;
		size_t binarySize = 0;
		const char* entryPoint = nullptr;
	};

	class IShader
	{
	public:
		virtual ~IShader() = default;
	};
}
