#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace dyf::RHI
{
	enum class ShaderStage : uint8_t
	{
		Unknown,
		Vertex,
		Fragment,
		Compute,
		Hull,
		Domain,
		Mesh
	};

	struct ShaderDesc
	{
		ShaderStage stage = ShaderStage::Unknown;
		const char* entryPoint = nullptr;
		const void* binary = nullptr;
		std::size_t binarySize = 0;
	};

    // GLSL의 main을 entryPoint 이름으로 내보낸다. 입력 문자열은 호출 중에만 참조한다.
    // sourceName은 진단과 상대 #include의 기준 경로이며, includeDirectories는 추가 검색 경로다.
    struct ShaderCompileDesc
    {
        ShaderStage stage = ShaderStage::Unknown;
        std::string_view source;
        const char* entryPoint = "main";
        const char* sourceName = "shader.glsl";
        const char* const* includeDirectories = nullptr;
        uint32_t includeDirectoryCount = 0;
    };

    struct ShaderCompileResult
    {
        // 성공한 SPIR-V를 소유한다. CreateShader 호출이 끝나면 결과를 해제해도 된다.
        std::vector<uint8_t> binary;
        // 실패 이유 또는 성공 시의 경고다. 실패하면 binary는 비어 있다.
        std::string diagnostics;
    };

    // DY_ENABLE_SHADER_COMPILER=ON인 Vulkan 엔진에서 지원한다.
    // 공개 선언은 옵션과 무관하다. OFF에서도 링크할 수 있으며 CompileShader는 실패 이유를 반환한다.
    [[nodiscard]] bool IsShaderCompilerAvailable() noexcept;
    // 기본 대상은 Vulkan 1.0/SPIR-V 1.0이며 Mesh는 Vulkan 1.3/SPIR-V 1.4다.
    [[nodiscard]] ShaderCompileResult CompileShader(const ShaderCompileDesc& desc);

	class Shader
	{
	public:
		Shader(const Shader&) = delete;
		Shader& operator=(const Shader&) = delete;
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
