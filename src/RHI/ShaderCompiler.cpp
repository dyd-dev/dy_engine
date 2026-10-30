#include "dyf/RHI/Shader.h"

#ifdef DY_SHADER_COMPILER_ENABLED
#include <climits>
#include <mutex>

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>
#include <StandAlone/DirStackFileIncluder.h>
#endif

namespace dyf::RHI
{
bool IsShaderCompilerAvailable() noexcept
{
#ifdef DY_SHADER_COMPILER_ENABLED
    return true;
#else
    return false;
#endif
}

ShaderCompileResult CompileShader(const ShaderCompileDesc& desc)
{
    ShaderCompileResult result;
#ifndef DY_SHADER_COMPILER_ENABLED
    (void)desc;
    result.diagnostics = "Shader compilation requires a Vulkan engine built with DY_ENABLE_SHADER_COMPILER=ON.";
#else
    EShLanguage stage;
    switch(desc.stage)
    {
    case ShaderStage::Vertex: stage = EShLangVertex; break;
    case ShaderStage::Fragment: stage = EShLangFragment; break;
    case ShaderStage::Compute: stage = EShLangCompute; break;
    case ShaderStage::Hull: stage = EShLangTessControl; break;
    case ShaderStage::Domain: stage = EShLangTessEvaluation; break;
    case ShaderStage::Mesh: stage = EShLangMesh; break;
    default:
        result.diagnostics = "CompileShader: unknown shader stage.";
        return result;
    }
    if(desc.source.empty() || desc.source.size() > INT_MAX ||
        desc.entryPoint == nullptr || *desc.entryPoint == '\0' ||
        (desc.includeDirectoryCount != 0 && desc.includeDirectories == nullptr))
    {
        result.diagnostics = "CompileShader: invalid source, entry point or include directories.";
        return result;
    }
    for(uint32_t i = 0; i < desc.includeDirectoryCount; ++i)
    {
        if(desc.includeDirectories[i] == nullptr || *desc.includeDirectories[i] == '\0')
        {
            result.diagnostics = "CompileShader: an include directory is empty.";
            return result;
        }
    }

    // 프로세스 초기화와 해제를 한 번만 수행하며 컴파일 호출도 같은 수명 안에서 직렬화한다.
    struct Compiler
    {
        std::mutex mutex;
        bool initialized = glslang::InitializeProcess();
        ~Compiler() { if(initialized) glslang::FinalizeProcess(); }
    };
    static Compiler compiler;
    std::lock_guard<std::mutex> lock(compiler.mutex);
    if(!compiler.initialized)
    {
        result.diagnostics = "CompileShader: glslang initialization failed.";
        return result;
    }

    const char* source = desc.source.data();
    const int length = static_cast<int>(desc.source.size());
    const char* sourceName = desc.sourceName != nullptr && *desc.sourceName != '\0'
        ? desc.sourceName : "shader.glsl";
    const bool mesh = desc.stage == ShaderStage::Mesh;
    glslang::TShader shader(stage);
    shader.setStringsWithLengthsAndNames(&source, &length, &sourceName, 1);
    shader.setSourceEntryPoint("main");
    shader.setEntryPoint(desc.entryPoint);
    shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan,
        mesh ? glslang::EShTargetVulkan_1_3 : glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv,
        mesh ? glslang::EShTargetSpv_1_4 : glslang::EShTargetSpv_1_0);

    DirStackFileIncluder includer;
    for(uint32_t i = 0; i < desc.includeDirectoryCount; ++i)
        includer.pushExternalLocalDirectory(desc.includeDirectories[i]);
    const auto messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
    const bool parsed = shader.parse(GetDefaultResources(), 450, false, messages, includer);
    result.diagnostics = shader.getInfoLog();
    result.diagnostics += shader.getInfoDebugLog();
    if(!parsed)
    {
        if(result.diagnostics.empty()) result.diagnostics = "CompileShader: GLSL parsing failed.";
        return result;
    }

    // TProgram은 자신이 참조한 TShader보다 먼저 파괴해야 한다.
    glslang::TProgram program;
    program.addShader(&shader);
    const bool linked = program.link(messages);
    result.diagnostics += program.getInfoLog();
    result.diagnostics += program.getInfoDebugLog();
    if(!linked || program.getIntermediate(stage) == nullptr)
    {
        if(result.diagnostics.empty()) result.diagnostics = "CompileShader: GLSL linking failed.";
        return result;
    }

    std::vector<unsigned int> words;
    spv::SpvBuildLogger logger;
    glslang::SpvOptions options;
    options.disableOptimizer = true;
    glslang::GlslangToSpv(*program.getIntermediate(stage), words, &logger, &options);
    result.diagnostics += logger.getAllMessages();
    if(words.empty())
    {
        if(result.diagnostics.empty()) result.diagnostics = "CompileShader: SPIR-V generation failed.";
        return result;
    }
    result.binary.resize(words.size() * sizeof(words[0]));
    std::memcpy(result.binary.data(), words.data(), result.binary.size());
#endif
    return result;
}
}
