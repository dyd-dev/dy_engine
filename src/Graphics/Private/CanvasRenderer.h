#pragma once
#include "RHI/IDevice.h"
#include "RHI/Format.h"
#include "RHI/ResourceHandles.h"
#include "Graphics/Shaders.h"
namespace dy::Graphics { class Canvas; struct TextureAsset; }
namespace dy::Graphics::Private
{
class CanvasRenderer
{
public:
    explicit CanvasRenderer(RHI::IDevice& device, const ShaderAssets& shaders)
        : m_device(device), m_shaders(shaders) {}
    ~CanvasRenderer();
    bool Render(const Canvas& canvas, TextureAsset* readback);
private:
    bool Initialize(RHI::Format format);
    RHI::IDevice& m_device;
    ShaderAssets m_shaders;
    RHI::ShaderHandle m_vertex = nullptr, m_fragment = nullptr;
    RHI::PipelineHandle m_pipeline = nullptr;
};
}
