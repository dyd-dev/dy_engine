#include "Graphics/Camera.h"
#include "Graphics/Canvas.h"
#include "Graphics/Font.h"
#include "Graphics/Renderer.h"
#include "Graphics/Scene.h"
#include "Graphics/ShaderLayout.h"
#include "RHI/Buffer.h"
#include "RHI/IDevice.h"
#include "RHI/Shader.h"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

using namespace dy;
using namespace dy::RHI;
using namespace dy::Graphics;

namespace
{
void Require(bool value, const char* message)
{
    if(!value) throw std::runtime_error(message);
}

struct Statistics
{
    unsigned shaders = 0, pipelines = 0, buffers = 0, textures = 0, sets = 0;
    unsigned destroyedShaders = 0, destroyedPipelines = 0, destroyedBuffers = 0, destroyedTextures = 0, destroyedSets = 0;
    unsigned uploads = 0, submits = 0, presents = 0;
    ShaderLayout::RendererLightingConstants lighting{};
};

// Public-interface decorator. Graphics cannot reach the underlying Null device
// except through IDevice; Null validates the assembled commands on Submit.
class ObservedDevice final : public IDevice
{
public:
    ObservedDevice(std::unique_ptr<IDevice> device, Statistics& stats) : inner(std::move(device)), stats(stats) {}
    bool CreateSwapchain(const SwapchainDesc& desc) override
    {
        auto validation = desc;
        validation.initialWidth = validation.initialHeight = 64;
        return inner->CreateSwapchain(validation);
    }
    bool BeginFrame() override { return inner->BeginFrame(); }
    ICommandList* AcquireCommandList() override { return inner->AcquireCommandList(); }
    bool Submit(ICommandList** lists, uint32_t count) override
    {
        ++stats.submits;
        const bool valid = inner->Submit(lists, count);
        Require(valid, "Graphics assembled an invalid RHI submission");
        return valid;
    }
    void Present() override { ++stats.presents; inner->Present(); }
    TextureHandle GetBackBuffer() override { return inner->GetBackBuffer(); }
    bool ReadTexture(TextureHandle texture, TextureReadback& result) override { return inner->ReadTexture(texture, result); }
    BufferHandle CreateBuffer(const BufferDesc& desc) override { ++stats.buffers; return inner->CreateBuffer(desc); }
    TextureHandle CreateTexture(const TextureDesc& desc) override { ++stats.textures; return inner->CreateTexture(desc); }
    ShaderHandle CreateShader(const ShaderDesc& desc) override { ++stats.shaders; return inner->CreateShader(desc); }
    PipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) override
    {
        ++stats.pipelines;
        auto* pipeline = inner->CreateGraphicsPipeline(desc);
        Require(pipeline != nullptr, "Graphics generated an invalid RHI pipeline");
        return pipeline;
    }
    ResourceSetHandle CreateResourceSet(const ResourceSetDesc& desc) override { ++stats.sets; return inner->CreateResourceSet(desc); }
    void DestroyBuffer(BufferHandle h) override { ++stats.destroyedBuffers; inner->DestroyBuffer(h); }
    void DestroyTexture(TextureHandle h) override { ++stats.destroyedTextures; inner->DestroyTexture(h); }
    void DestroyShader(ShaderHandle h) override { ++stats.destroyedShaders; inner->DestroyShader(h); }
    void DestroyPipeline(PipelineHandle h) override { ++stats.destroyedPipelines; inner->DestroyPipeline(h); }
    void DestroyResourceSet(ResourceSetHandle h) override { ++stats.destroyedSets; inner->DestroyResourceSet(h); }
    bool UpdateBuffer(ICommandList& list, BufferHandle buffer, uint32_t offset, const void* data, uint32_t size) override
    {
        ++stats.uploads;
        if(size == sizeof(stats.lighting) && buffer->GetDesc().usage == BufferUsage::Constant)
            std::memcpy(&stats.lighting, data, size);
        return inner->UpdateBuffer(list, buffer, offset, data, size);
    }
    bool UpdateTexture(ICommandList& list, TextureHandle texture, uint32_t mip, uint32_t layer,
        const void* data, uint32_t size, uint32_t row, uint32_t slice) override
    { ++stats.uploads; return inner->UpdateTexture(list, texture, mip, layer, data, size, row, slice); }
protected:
    int Initialize(const void*, const DeviceDesc&) override { return -1; }
private:
    std::unique_ptr<IDevice> inner;
    Statistics& stats;
};
}

int main(int argc, char** argv)
{
    try
    {
        Require(argc >= 2, "Pass an animated model path");
        Statistics stats;
        int windowToken = 0; // Null never dereferences the window; no GLFW or display is involved.
        auto device = std::unique_ptr<IDevice>(IDevice::Create(&windowToken));
        Require(device != nullptr, "Null device creation failed");
        RendererDesc desc;
        // Explicit Null test bytecode, not a native-language fallback in examples.
        static constexpr uint8_t opaqueValidationProgram[] = {1};
        const ShaderAsset program{opaqueValidationProgram, sizeof(opaqueValidationProgram), "validation"};
        desc.canvasShaders = {program, program, {}};
        desc.meshShaders = {program, program, program};
        desc.enableShadows = true;
        auto renderer = Renderer::Create(std::make_unique<ObservedDevice>(std::move(device), stats), desc);
        Require(renderer != nullptr && stats.shaders == 0, "Renderer must initialize programs lazily");
        TextureAsset pixel{"", 1, 1, {255, 80, 30, 255}};
        Canvas canvas(64, 64);
        canvas.Point({0, 0}, {1, 1, 1, 1}, 1);
        canvas.Line({0, 0}, {10, 10}, {1, 1, 1, 1}, 1);
        canvas.Rect({0, 0, 10, 10}, {1, 1, 1, 1}, 1);
        canvas.FillRect({0, 0, 10, 10}, {1, 1, 1, 1});
        canvas.Circle({0, 0}, 10, {1, 1, 1, 1}, 1);
        canvas.FillCircle({0, 0}, 10, {1, 1, 1, 1});
        canvas.Image(pixel, {0, 0, 10, 10});
        auto font = argc > 2 ? Font::Load(argv[2], 16, 256, 256) : nullptr;
        if(argc > 2) Require(font != nullptr, "Test font could not be loaded");
        if(font) canvas.Text(*font, "RHI / 조합", {0, 0});
        Require(renderer->Render(canvas) && stats.shaders == 2, "Canvas must only require its own two shaders");
        TextureAsset capture;
        bool rejectedReadback = false;
        try { (void)renderer->Render(canvas, &capture); }
        catch(const std::invalid_argument&) { rejectedReadback = true; }
        Require(rejectedReadback, "Readback must require explicit opt-in");

        Scene scene;
        MaterialDesc material;
        material.baseColorTexture = scene.CreateTexture(pixel);
        (void)scene.CreateEntity(scene.CreateMesh(CreateCubeMesh(1)), scene.CreateMaterial(material));
        DirectionalLight directional;
        directional.intensity = 2;
        directional.castShadow = true;
        (void)scene.CreateDirectionalLight(directional);
        Camera camera;
        Require(renderer->Render(scene, camera), "Mesh and shadow composition failed");
        (void)scene.CreatePointLight(PointLight{});
        (void)scene.CreateSpotLight(SpotLight{});
        (void)scene.CreateRectAreaLight(RectAreaLight{});
        (void)scene.CreateDiscAreaLight(DiscAreaLight{});
        Require(renderer->Render(scene, camera), "Five-light composition failed");
        Require(stats.lighting.lightCounts.x == 1 && stats.lighting.lightCounts.y == 1 &&
            stats.lighting.lightCounts.z == 1 && stats.lighting.areaLightCounts.x == 1 &&
            stats.lighting.areaLightCounts.y == 1, "All five light types must reach RHI buffer data");

        Scene animated;
        ModelSceneDesc model;
        model.path = argv[1];
        ModelInstanceID instance;
        Require(AddModelToScene(animated, model, &instance) && animated.PlayAnimation(instance, 0), "Model scene setup failed");
        Require(animated.UpdateAnimations(0.125f).Succeeded() && !animated.JointPaletteMatrices().empty(), "Skinning pose missing");
        Require(renderer->Render(animated, camera), "Skinning composition failed");
        Require(animated.UpdateAnimations(0.125f).Succeeded() && renderer->Render(animated, camera), "Subsequent skinning frame failed");
        Require(renderer->Render(canvas), "Switching back to Canvas failed");
        renderer.reset();
        Require(stats.shaders == stats.destroyedShaders && stats.pipelines == stats.destroyedPipelines &&
            stats.buffers == stats.destroyedBuffers && stats.textures == stats.destroyedTextures &&
            stats.sets == stats.destroyedSets, "Renderer leaked an owned RHI resource");
        Require(stats.presents == 6 && stats.uploads > 0 && stats.submits >= stats.presents, "Rendering bypassed RHI");
        std::cout << "Canvas, five lights, shadows and animated skinning used validated public RHI commands.\n";
        return 0;
    }
    catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
