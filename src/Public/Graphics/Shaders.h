#pragma once

#include <cstddef>

namespace dy::Graphics
{
// Compiled assets only. Callers choose pipeline state, bindings and resources.
// The Null backend has no native shaders and returns empty assets.
struct ShaderAsset
{
    const void* binary = nullptr;
    std::size_t binarySize = 0;
    const char* entryPoint = nullptr;
};

struct ShaderAssets
{
    ShaderAsset vertex;
    ShaderAsset fragment;
    ShaderAsset shadowVertex;
};

[[nodiscard]] ShaderAssets GetMeshShaderAssets(bool shadowsEnabled);
[[nodiscard]] ShaderAssets GetCanvasShaderAssets();
}
