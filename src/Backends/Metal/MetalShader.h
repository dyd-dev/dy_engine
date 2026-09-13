#pragma once

#include "dyf/RHI/Shader.h"

namespace dyf::Backends
{
	struct MetalObjectDeleter;

	class MetalShader final : public RHI::Shader
	{
	public:
		MetalShader(const RHI::ShaderDesc& desc, void* device);

		[[nodiscard]] void* GetNativeFunction() const;

	private:
		friend struct MetalObjectDeleter;

		~MetalShader() override;

		struct Impl;
		Impl* m_impl = nullptr;
	};
}
