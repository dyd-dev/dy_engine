#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace dy::RHI
{
	enum class ShaderStage : uint8_t
	{
		Unknown,
		Vertex,
		Fragment
	};

	struct ShaderDesc
	{
		ShaderStage stage = ShaderStage::Unknown;
		const char* entryPoint = nullptr;
		const void* binary = nullptr;
		std::size_t binarySize = 0;
	};

	class Shader
	{
	public:
		[[nodiscard]] ShaderStage GetStage() const { return m_stage; }
		[[nodiscard]] const char* GetEntryPoint() const { return m_entryPoint.c_str(); }
		[[nodiscard]] const void* GetBinary() const { return m_binary.data(); }
		[[nodiscard]] std::size_t GetBinarySize() const { return m_binary.size(); }

	protected:
		virtual ~Shader() = default;
		explicit Shader(const ShaderDesc& desc)
			: m_stage(desc.stage)
			, m_entryPoint(desc.entryPoint == nullptr ? "" : desc.entryPoint)
			, m_binary(desc.binarySize)
		{
			if(desc.binary != nullptr && desc.binarySize != 0)
			{
				std::memcpy(m_binary.data(), desc.binary, desc.binarySize);
			}
		}

	private:
		ShaderStage m_stage = ShaderStage::Unknown;
		std::string m_entryPoint;
		std::vector<uint8_t> m_binary;
	};
}
