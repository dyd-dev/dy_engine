#include "Graphics/Camera.h"
#include "Graphics/Canvas.h"
#include "Graphics/Renderer.h"
#include "Graphics/Scene.h"
#include "Graphics/ShaderLayout.h"
#include "IO/ImageOutput.h"
#include "Platform/Window.h"
#include "RHI/Buffer.h"
#include "RHI/ICommandList.h"
#include "RHI/Pipeline.h"
#include "RHI/Readback.h"
#include "RHI/ResourceScope.h"
#include "RHI/ResourceSet.h"
#include "RHI/Shader.h"
#include "RHI/Texture.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace dy;
using namespace dy::Graphics;

namespace
{
void Require(bool value, const char* message)
{
    if(!value) throw std::runtime_error(message);
}

template<class Frame>
void AwaitFrame(Platform::Window& window, Frame frame)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while(std::chrono::steady_clock::now() < deadline && window.IsRunning())
    {
        window.PollEvents();
        if(frame()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    throw std::runtime_error("Drawable timed out");
}

TextureAsset DrawRhi(Platform::Window& window, const TextureAsset& image)
{
    using namespace RHI;
    std::unique_ptr<IDevice> device(IDevice::Create(window.GetHandle()));
    Require(device != nullptr, "RHI device creation failed");
    ResourceScope resources(*device);
    SwapchainDesc swapchain;
    swapchain.format = Format::B8G8R8A8_UNORM;
    swapchain.allowReadback = true;
    Require(device->CreateSwapchain(swapchain), "Readback swapchain creation failed");
    const auto shaders = GetCanvasShaderAssets();
    std::vector<uint8_t> byteView(shaders.vertex.binarySize + 1);
    std::memcpy(byteView.data() + 1, shaders.vertex.binary, shaders.vertex.binarySize);
    auto* vertex = resources.Keep(device->CreateShader({ShaderStage::Vertex, shaders.vertex.entryPoint, byteView.data() + 1, shaders.vertex.binarySize}));
    auto* fragment = resources.Keep(device->CreateShader({ShaderStage::Fragment, shaders.fragment.entryPoint, shaders.fragment.binary, shaders.fragment.binarySize}));
    using Input = ShaderLayout::CanvasVertex;
    const VertexBufferLayout layout{0, sizeof(Input), VertexStepMode::Vertex};
    const std::array<VertexAttribute, 3> attributes{{
        {0, 0, Format::R32G32_FLOAT, 0}, {1, 0, Format::R32G32_FLOAT, 8}, {2, 0, Format::R32G32B32A32_FLOAT, 16}}};
    SamplerDesc sampler;
    sampler.minFilter = sampler.magFilter = sampler.mipFilter = SamplerFilter::Linear;
    sampler.addressU = sampler.addressV = sampler.addressW = SamplerAddressMode::ClampToEdge;
    sampler.minLod = sampler.maxLod = sampler.mipLodBias = 0;
    const std::array<ResourceBindingLayout, 2> bindings{{
        {0, ResourceBindingType::SampledTexture, 1, ShaderStageFlags::Fragment, {}},
        {1, ResourceBindingType::StaticSampler, 1, ShaderStageFlags::Fragment, sampler}}};
    const ColorAttachmentDesc attachment{swapchain.format,
        {true, BlendFactor::SourceAlpha, BlendFactor::OneMinusSourceAlpha, BlendOp::Add,
            BlendFactor::One, BlendFactor::OneMinusSourceAlpha, BlendOp::Add}, ColorWriteMask::All};
    GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.vertexShader = vertex; pipelineDesc.fragmentShader = fragment;
    pipelineDesc.topology = PrimitiveTopology::TriangleList;
    pipelineDesc.vertexBuffers = &layout; pipelineDesc.vertexBufferCount = 1;
    pipelineDesc.vertexAttributes = attributes.data(); pipelineDesc.vertexAttributeCount = attributes.size();
    pipelineDesc.raster = {FillMode::Solid, CullMode::None, FrontFace::CounterClockwise, 0, 0, 0};
    pipelineDesc.colorAttachments = &attachment; pipelineDesc.colorAttachmentCount = 1;
    pipelineDesc.layout = {bindings.data(), static_cast<uint32_t>(bindings.size()), 16, ShaderStageFlags::Vertex, 15};
    auto* pipeline = resources.Keep(device->CreateGraphicsPipeline(pipelineDesc));
    std::vector<Input> data;
    for(auto p : {Math::float2{0, 0}, {1, 0}, {0, 1}, {0, 1}, {1, 0}, {1, 1}})
        data.push_back({8 + p.x * 104, 8 + p.y * 104, p.x, p.y, 0.8f, 0.6f, 1, 0.75f});
    auto* vertices = resources.Keep(device->CreateBuffer(
        {static_cast<uint32_t>(data.size() * sizeof(Input)), sizeof(Input), BufferUsage::Vertex, ResourceState::CopyDestination}));
    TextureDesc textureDesc;
    textureDesc.width = image.width; textureDesc.height = image.height;
    textureDesc.format = Format::R8G8B8A8_UNORM; textureDesc.usage = TextureUsage::ShaderResource;
    auto* texture = resources.Keep(device->CreateTexture(textureDesc));
    auto* upload = device->AcquireCommandList();
    Require(upload != nullptr, "Upload list missing");
    const ResourceBarrierDesc toCopy{nullptr, texture, ResourceState::Undefined, ResourceState::CopyDestination, {}};
    upload->ResourceBarrier(&toCopy, 1);
    Require(device->UpdateBuffer(*upload, vertices, 0, data.data(), vertices->GetDesc().size), "Vertex upload failed");
    Require(device->UpdateTexture(*upload, texture, 0, 0, image.rgba8.data(),
        static_cast<uint32_t>(image.rgba8.size()), image.width * 4, image.width * image.height * 4), "Texture upload failed");
    const std::array<ResourceBarrierDesc, 2> ready{{
        {vertices, nullptr, ResourceState::CopyDestination, ResourceState::VertexBuffer, {}},
        {nullptr, texture, ResourceState::CopyDestination, ResourceState::ShaderResource, {}}}};
    upload->ResourceBarrier(ready.data(), ready.size());
    upload->Close();
    Require(device->Submit(&upload, 1), "Upload submit failed");
    TextureReadback uploadedImage;
    Require(device->ReadTexture(texture, uploadedImage) && uploadedImage.pixels == image.rgba8,
        "Uploaded texture readback failed or changed its data");
    ResourceBinding sampled;
    sampled.binding = 0; sampled.texture = texture;
    auto* set = resources.Keep(device->CreateResourceSet({pipeline, &sampled, 1}));
    AwaitFrame(window, [&] { return device->BeginFrame(); });
    auto* target = device->GetBackBuffer();
    auto* commands = device->AcquireCommandList();
    Require(commands != nullptr, "Draw list missing");
    const ResourceBarrierDesc before{nullptr, target, ResourceState::Present, ResourceState::RenderTarget, {}};
    commands->ResourceBarrier(&before, 1);
    ColorAttachment color;
    color.texture = target; color.loadOp = LoadOp::Clear; color.storeOp = StoreOp::Store;
    color.clearColor[0] = 0.04f; color.clearColor[1] = 0.06f; color.clearColor[2] = 0.10f; color.clearColor[3] = 1;
    commands->BeginRendering({&color, 1, nullptr});
    commands->BindGraphicsPipeline(pipeline);
    commands->BindVertexBuffer(0, vertices, 0);
    commands->BindResourceSet(set);
    commands->SetViewport({0, 0, 128, 128, 0, 1});
    commands->SetScissor({12, 16, 96, 80});
    const float transform[4]{2.0f / 128, -2.0f / 128, -1, 1};
    commands->SetInlineConstants(0, sizeof(transform), transform);
    commands->DrawInstanced(6, 1, 0, 0);
    commands->EndRendering();
    const ResourceBarrierDesc after{nullptr, target, ResourceState::RenderTarget, ResourceState::Present, {}};
    commands->ResourceBarrier(&after, 1);
    commands->Close();
    Require(device->Submit(&commands, 1), "Draw submit failed");
    TextureReadback captured;
    Require(device->ReadTexture(target, captured), "RHI readback failed");
    // A second read must preserve state and presentation synchronization.
    TextureReadback repeated;
    Require(device->ReadTexture(target, repeated) && repeated.pixels == captured.pixels, "Readback changed resource state/data");
    device->Present();
    TextureAsset result{"", captured.width, captured.height, std::move(captured.pixels)};
    if(captured.format == Format::B8G8R8A8_UNORM || captured.format == Format::B8G8R8A8_UNORM_SRGB)
        for(size_t i = 0; i < result.rgba8.size(); i += 4) std::swap(result.rgba8[i], result.rgba8[i + 2]);
    return result;
}

void Save(const std::filesystem::path& path, const TextureAsset& image)
{
    IO::WritePpm(path.string(), {image.width, image.height, image.width * 4,
        IO::PixelLayout::RGBA8, image.rgba8.data(), image.rgba8.size()});
}
}

int main(int argc, char** argv)
{
    try
    {
        Require(argc == 3, "Pass an animated model and output directory");
        Platform::Window window(128, 128, "RHI / Graphics composition test");
        const TextureAsset image{"", 2, 2, {255, 20, 30, 255, 20, 255, 40, 255, 30, 40, 255, 255, 255, 255, 255, 128}};
        RendererDesc desc;
        desc.allowReadback = true;
        TextureAsset graphics;
        {
            auto renderer = Renderer::Create(window.GetHandle(), desc);
            Require(renderer != nullptr, "Graphics renderer creation failed");
            Canvas canvas(128, 128);
            canvas.Clear({0.04f, 0.06f, 0.10f, 1});
            canvas.SetClipRect({12, 16, 96, 80});
            canvas.Image(image, {8, 8, 104, 104}, {0.8f, 0.6f, 1, 0.75f});
            AwaitFrame(window, [&] { return renderer->Render(canvas, &graphics); });
        }
        const TextureAsset rhi = DrawRhi(window, image);
        const std::filesystem::path output(argv[2]);
        std::filesystem::create_directories(output);
        Save(output / "graphics.ppm", graphics);
        Save(output / "rhi.ppm", rhi);
        Require(graphics.width == rhi.width && graphics.height == rhi.height && graphics.rgba8 == rhi.rgba8,
            "Graphics and RHI canvas pixels differ");
        Require(graphics.rgba8.at((32 * graphics.width + 32) * 4) != graphics.rgba8.front(), "Both paths rendered only a clear");
        auto renderer = Renderer::Create(window.GetHandle(), desc);
        Require(renderer != nullptr, "Model renderer creation failed");
        Scene scene;
        ModelSceneDesc model;
        model.path = argv[1];
        ModelInstanceID instance;
        Require(AddModelToScene(scene, model, &instance) && scene.PlayAnimation(instance, 1), "Model animation setup failed");
        DirectionalLight light;
        light.direction = {0, -1, 2}; light.intensity = 3;
        (void)scene.CreateDirectionalLight(light);
        Camera camera;
        camera.position = {2.6f, -3, 1.8f};
        camera.view = Math::LookAtRH(camera.position, {0, 0, 0.4f}, {0, 0, 1});
        camera.projection = Math::PerspectiveRH_ZO(1, 1, 0.1f, 100);
        TextureAsset frame1, frame2;
        Require(scene.UpdateAnimations(0.25f).Succeeded(), "Animation update failed");
        AwaitFrame(window, [&] { return renderer->Render(scene, camera, &frame1); });
        Require(scene.UpdateAnimations(0.25f).Succeeded(), "Animation update failed");
        AwaitFrame(window, [&] { return renderer->Render(scene, camera, &frame2); });
        Save(output / "skinning-1.ppm", frame1);
        Save(output / "skinning-2.ppm", frame2);
        Require(frame1.width == frame2.width && frame1.height == frame2.height,
            "Window resized during the skinning comparison; rerun at a stable size");
        Require(frame1.rgba8 != frame2.rgba8, "Animated skinning did not change rendered pixels");
        std::cout << "Graphics/RHI textured canvas pixels are identical; skinning changes pixels across frames.\n";
        return 0;
    }
    catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
