#include "ModelShaderAssets.h"

#if defined(DY_MODEL_SHADERS_METAL)
#include "ModelMetalLibrary.h"
#elif defined(DY_MODEL_SHADERS_NATIVE)
#include "ModelVertexShader.h"
#include "ModelVertexShaderNoShadows.h"
#include "ModelShadowShader.h"
#endif
#if defined(DY_MODEL_SHADERS_COMPUTE)
#include "ModelComputeShader.h"
#endif

namespace dyf
{
    RHI::ShaderDesc GetModelVertexShader(bool shadowsEnabled)
    {
#if defined(DY_MODEL_SHADERS_METAL)
        return {RHI::ShaderStage::Vertex,
            shadowsEnabled ? "modelVertexShader" : "modelVertexShaderNoShadows",
            kModelMetalLibrary, kModelMetalLibrarySize};
#elif defined(DY_MODEL_SHADERS_NATIVE)
        if (shadowsEnabled)
            return {RHI::ShaderStage::Vertex, "main", kModelVertexShader, kModelVertexShaderSize};
        return {RHI::ShaderStage::Vertex, "main", kModelVertexShaderNoShadows, kModelVertexShaderNoShadowsSize};
#else
        (void)shadowsEnabled;
        return {};
#endif
    }

    RHI::ShaderDesc GetModelShadowShader()
    {
#if defined(DY_MODEL_SHADERS_METAL)
        return {RHI::ShaderStage::Vertex, "modelShadowVertexShader", kModelMetalLibrary, kModelMetalLibrarySize};
#elif defined(DY_MODEL_SHADERS_NATIVE)
        return {RHI::ShaderStage::Vertex, "main", kModelShadowShader, kModelShadowShaderSize};
#else
        return {};
#endif
    }

    RHI::ShaderDesc GetModelComputeShader()
    {
#if defined(DY_MODEL_SHADERS_METAL)
        return {RHI::ShaderStage::Compute, "modelSkinningCompute", kModelMetalLibrary, kModelMetalLibrarySize};
#elif defined(DY_MODEL_SHADERS_COMPUTE)
        return {RHI::ShaderStage::Compute, "main", kModelComputeShader, kModelComputeShaderSize};
#else
        return {};
#endif
    }
}
