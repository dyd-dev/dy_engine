#pragma once

#include <cstdint>

namespace dy::RHI
{
	enum class SamplerFilter : uint32_t
	{
		Nearest,
		Linear
	};

	enum class SamplerAddressMode : uint32_t
	{
		Repeat,
		MirroredRepeat,
		ClampToEdge
	};

	struct SamplerDesc
	{
		SamplerFilter minFilter = SamplerFilter::Linear;
		SamplerFilter magFilter = SamplerFilter::Linear;
		SamplerFilter mipFilter = SamplerFilter::Linear;
		SamplerAddressMode addressU = SamplerAddressMode::Repeat;
		SamplerAddressMode addressV = SamplerAddressMode::Repeat;
		SamplerAddressMode addressW = SamplerAddressMode::Repeat;
		float minLod = 0.0f;
		float maxLod = 1000.0f;
	};

	class ISampler
	{
	public:
		virtual ~ISampler() = default;
		[[nodiscard]] const SamplerDesc& GetDesc() const { return m_desc; }

	protected:
		explicit ISampler(const SamplerDesc& desc) : m_desc(desc) {}

	private:
		SamplerDesc m_desc = {};
	};
}
