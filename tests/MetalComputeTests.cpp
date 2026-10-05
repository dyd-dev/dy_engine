#include <dyf/RHI.h>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace dyf::RHI;
static void Check(bool ok, const char* message) { if(!ok) throw std::runtime_error(message); }

int main(int argc, char** argv) try
{
    Check(argc == 2, "Expected test metallib path");
    std::ifstream file(argv[1], std::ios::binary);
    std::vector<char> binary((std::istreambuf_iterator<char>(file)), {});
    Check(!binary.empty(), "Missing test metallib");
    std::unique_ptr<IDevice> device(IDevice::Create({}));
    Check(device && device->Supports(Feature::Compute), "Metal Compute unavailable");
    ResourceScope resources(*device);
    auto shader = [&](ShaderStage stage, const char* entry) {
        return resources.Keep(device->CreateShader({stage, entry, binary.data(), binary.size()}));
    };
    auto* compute = shader(ShaderStage::Compute, "computeMain");
    auto* vertex = shader(ShaderStage::Vertex, "vertexMain");
    auto* fragment = shader(ShaderStage::Fragment, "fragmentMain");
    Check(compute && vertex && fragment, "Shader creation failed");
    ResourceBindingLayout writable{0, ResourceBindingType::ReadWriteStorageBuffer, 1, ShaderStageFlags::Compute, {}};
    const ResourceBindingLayout inputs[] = {writable,
        {1, ResourceBindingType::ReadOnlyStorageBuffer, 1, ShaderStageFlags::Compute, {}},
        {2, ResourceBindingType::ConstantBuffer, 1, ShaderStageFlags::Compute, {}}};
    ComputePipelineDesc desc{compute, {inputs, 3, sizeof(uint32_t), ShaderStageFlags::Compute, 15}, {4, 2, 2}};
    auto* pipeline = resources.Keep(device->CreateComputePipeline(desc));
    Check(pipeline != nullptr, "Compute pipeline rejected");
    for(uint32_t invalidSize : {0u, UINT32_MAX}) {
        auto invalid = desc; invalid.threadGroupSize[0] = invalidSize;
        Check(device->CreateComputePipeline(invalid) == nullptr, "Invalid group size accepted");
    }
    auto invalidBinding = writable;
    invalidBinding.binding = 15;
    auto invalidLayout = desc.layout; invalidLayout.bindings = &invalidBinding; invalidLayout.bindingCount = 1;
    Check(!device->Supports(invalidLayout), "Inline constant slot collision accepted");
    invalidBinding.binding = 31;
    Check(!device->Supports(invalidLayout), "Out-of-range buffer slot accepted");
    invalidBinding.binding = 0; invalidBinding.type = ResourceBindingType::StorageTexture;
    Check(!device->Supports(invalidLayout), "Unimplemented compute texture binding accepted");

    auto* buffer = resources.Keep(device->CreateBuffer({128 * 16, 16, BufferUsage::Storage, ResourceState::Undefined}));
    ResourceBinding binding{0, 0, buffer, nullptr, 0, 128 * 16, {}};
    auto* seed = resources.Keep(device->CreateBuffer({32, 16, BufferUsage::Storage, ResourceState::CopyDestination}));
    auto* factor = resources.Keep(device->CreateBuffer({4, 4, BufferUsage::Constant, ResourceState::CopyDestination}));
    const ResourceBinding computeBindings[] = {binding, {1, 0, seed, nullptr, 16, 16, {}}, {2, 0, factor, nullptr, 0, 4, {}}};
    auto* computeSet = resources.Keep(device->CreateResourceSet({pipeline, computeBindings, 3}));
    Check(buffer && computeSet, "Compute resource creation failed");
    ResourceBindingLayout readable{0, ResourceBindingType::ReadOnlyStorageBuffer, 1, ShaderStageFlags::Fragment, {}};
    ColorAttachmentDesc colorFormat{Format::R8G8B8A8_UNORM, {}, ColorWriteMask::All};
    GraphicsPipelineDesc graphics;
    graphics.vertexShader = vertex; graphics.fragmentShader = fragment;
    graphics.topology = PrimitiveTopology::TriangleList;
    graphics.raster = {FillMode::Solid, CullMode::None, FrontFace::CounterClockwise, 0, 0, 0};
    graphics.colorAttachments = &colorFormat; graphics.colorAttachmentCount = 1;
    graphics.layout = {&readable, 1, 0, ShaderStageFlags::None, 0};
    auto* display = resources.Keep(device->CreateGraphicsPipeline(graphics));
    auto* displaySet = resources.Keep(device->CreateResourceSet({display, &binding, 1}));
    TextureDesc texture; texture.width = 8; texture.height = 16;
    texture.format = colorFormat.format; texture.usage = TextureUsage::RenderTarget;
    auto* target = resources.Keep(device->CreateTexture(texture));
    Check(display && displaySet && target, "Readback renderer creation failed");

    auto* commands = resources.Keep(device->AcquireCommandList());
    Check(commands != nullptr, "Command list creation failed");
    for(uint32_t iteration = 0; iteration < 2; ++iteration) {
        if(iteration == 0) {
            const float initial[] = {0, .125f, .25f, .5f}; const uint32_t multiplier = 2;
            Check(device->UpdateBuffer(*commands, seed, 16, initial, sizeof(initial)) &&
                device->UpdateBuffer(*commands, factor, 0, &multiplier, sizeof(multiplier)), "Compute input upload failed");
            const ResourceBarrierDesc uploaded[] = {
                {seed, nullptr, ResourceState::CopyDestination, ResourceState::ShaderResource, {}},
                {factor, nullptr, ResourceState::CopyDestination, ResourceState::ConstantBuffer, {}}};
            commands->ResourceBarrier(uploaded, 2);
        }
        if(iteration) Check(device->ResetCommandList(commands), "Compute command reset failed");
        const ResourceBarrierDesc begin[] = {
            {buffer, nullptr, iteration ? ResourceState::ShaderResource : ResourceState::Undefined, ResourceState::UnorderedAccess, {}},
            {nullptr, target, iteration ? ResourceState::RenderTarget : ResourceState::Undefined, ResourceState::RenderTarget, {}}
        };
        commands->ResourceBarrier(begin, 2);
        commands->BindComputePipeline(pipeline); commands->BindResourceSet(computeSet);
        for(uint32_t pass = 0; pass < 2; ++pass) {
            if(pass) {
                ResourceBarrierDesc barrier{buffer, nullptr, ResourceState::UnorderedAccess, ResourceState::UnorderedAccess, {}};
                commands->ResourceBarrier(&barrier, 1);
            }
            commands->SetInlineConstants(0, sizeof(pass), &pass);
            commands->Dispatch(2, 2, 2);
        }
        ResourceBarrierDesc ready{buffer, nullptr, ResourceState::UnorderedAccess, ResourceState::ShaderResource, {}};
        commands->ResourceBarrier(&ready, 1);
        ColorAttachment color; color.texture = target; color.loadOp = LoadOp::Clear; color.storeOp = StoreOp::Store;
        commands->BeginRendering({&color, 1, nullptr});
        commands->BindGraphicsPipeline(display); commands->BindResourceSet(displaySet);
        commands->SetViewport({0, 0, 8, 16, 0, 1}); commands->SetScissor({0, 0, 8, 16});
        commands->DrawInstanced(3, 1, 0, 0); commands->EndRendering();
        Check(commands->Close() && device->Submit(&commands, 1) && device->WaitIdle(), "Compute/render submission failed");
        TextureReadback image;
        Check(device->ReadTexture(target, image), "Compute result readback failed");
        for(uint32_t y = 0; y < 16; ++y) for(uint32_t x = 0; x < 8; ++x) {
            const auto* pixel = image.pixels.data() + y * image.rowPitch + x * 4;
            const int expected = int(std::lround(float(x + y * 8 + 1) * 255 / 128));
            Check(std::abs(int(pixel[0]) - expected) <= 1 && pixel[1] == 64 && pixel[2] == 128 && pixel[3] == 255,
                "Incorrect compute result, group geometry, or compute-to-render visibility");
        }
    }
    // Commands are replayed at Submit; native validation may reject there instead of Close.
    for(int invalid = 0; invalid < 3; ++invalid) {
        auto* list = resources.Keep(device->AcquireCommandList());
        Check(list != nullptr, "Invalid-case command list creation failed");
        ResourceBarrierDesc writableAgain{buffer, nullptr, ResourceState::ShaderResource, ResourceState::UnorderedAccess, {}};
        list->ResourceBarrier(&writableAgain, 1);
        list->BindComputePipeline(pipeline);
        if(invalid != 0) list->BindResourceSet(computeSet);
        if(invalid != 1) { const uint32_t pass = 0; list->SetInlineConstants(0, sizeof(pass), &pass); }
        list->Dispatch(invalid == 2 ? 0 : 2, 2, 2);
        Check(!list->Close() || !device->Submit(&list, 1), "Invalid compute commands accepted");
    }
    std::puts("Metal compute buffer contracts passed");
    return 0;
}
catch(const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
