#include <dyf/RHI.h>

#ifdef DY_TEST_NATIVE_SHADERS
#include "tess_domain.h"
#include "tess_fragment.h"
#include "tess_hull.h"
#include "tess_vertex.h"
#endif

#include <array>
#include <cstdio>
#include <memory>
#include <stdexcept>

namespace RHI = dyf::RHI;

namespace
{
void Check(bool value, const char* message)
{
    if(!value) throw std::runtime_error(message);
}

RHI::ShaderHandle Shader(RHI::IDevice& device, RHI::ShaderStage stage,
    const char* entryPoint = "main", const void* shaderBinary = nullptr, std::size_t binarySize = 0)
{
    static constexpr unsigned char dummyBinary[] = {0x44, 0x59, 0x46, 0x00};
    if(shaderBinary == nullptr)
    {
        shaderBinary = dummyBinary;
        binarySize = sizeof(dummyBinary);
    }
    return device.CreateShader({stage, entryPoint, shaderBinary, binarySize});
}
}

int main()
{
    try
    {
        std::unique_ptr<RHI::IDevice> device(RHI::IDevice::Create({}));
        Check(device != nullptr, "Device creation failed");
        Check(device->Supports(RHI::Feature::Tessellation), "Tessellation feature was not exposed");
        Check(device->GetLimit(RHI::Limit::TessellationPatchControlPoints) >= 3,
            "Tessellation patch limit is too small");

#ifdef DY_TEST_NATIVE_SHADERS
        auto* vertex = Shader(*device, RHI::ShaderStage::Vertex,
            ShaderData::tess_vertexEntryPoint, ShaderData::tess_vertex, ShaderData::tess_vertexSize);
        auto* hull = Shader(*device, RHI::ShaderStage::Hull,
            ShaderData::tess_hullEntryPoint, ShaderData::tess_hull, ShaderData::tess_hullSize);
        auto* domain = Shader(*device, RHI::ShaderStage::Domain,
            ShaderData::tess_domainEntryPoint, ShaderData::tess_domain, ShaderData::tess_domainSize);
        auto* fragment = Shader(*device, RHI::ShaderStage::Fragment,
            ShaderData::tess_fragmentEntryPoint, ShaderData::tess_fragment, ShaderData::tess_fragmentSize);
#else
        auto* vertex = Shader(*device, RHI::ShaderStage::Vertex);
        auto* hull = Shader(*device, RHI::ShaderStage::Hull);
        auto* domain = Shader(*device, RHI::ShaderStage::Domain);
        RHI::ShaderHandle fragment = nullptr;
#endif
        Check(vertex && hull && domain, "Tessellation shader creation failed");
#ifdef DY_TEST_NATIVE_SHADERS
        Check(fragment != nullptr, "Tessellation fragment shader creation failed");
#endif

        RHI::GraphicsPipelineDesc desc;
        desc.vertexShader = vertex;
        desc.hullShader = hull;
        desc.domainShader = domain;
        desc.fragmentShader = fragment;
        desc.topology = RHI::PrimitiveTopology::PatchList;
        desc.patchControlPoints = 3;
        desc.raster.fillMode = RHI::FillMode::Solid;
        desc.raster.cullMode = RHI::CullMode::Back;
        desc.raster.frontFace = RHI::FrontFace::CounterClockwise;
#ifdef DY_TEST_NATIVE_SHADERS
        const RHI::VertexBufferLayout input{0, sizeof(float) * 2, RHI::VertexStepMode::Vertex};
        const RHI::VertexAttribute attribute{0, 0, RHI::Format::R32G32_FLOAT, 0};
        const RHI::ColorAttachmentDesc colorFormat{
            RHI::Format::R8G8B8A8_UNORM, {}, RHI::ColorWriteMask::All};
        desc.vertexBuffers = &input;
        desc.vertexBufferCount = 1;
        desc.vertexAttributes = &attribute;
        desc.vertexAttributeCount = 1;
        desc.colorAttachments = &colorFormat;
        desc.colorAttachmentCount = 1;
#endif
        Check(device->Supports(desc), "Valid tessellation pipeline was rejected");
        auto* pipeline = device->CreateGraphicsPipeline(desc);
        Check(pipeline != nullptr, "Valid tessellation pipeline creation failed");

        auto invalid = desc;
        invalid.domainShader = nullptr;
        Check(!device->Supports(invalid), "Unpaired Hull shader was accepted");
        invalid = desc;
        invalid.topology = RHI::PrimitiveTopology::TriangleList;
        Check(!device->Supports(invalid), "Tessellation shaders without PatchList were accepted");
        invalid = desc;
        invalid.patchControlPoints = 0;
        Check(!device->Supports(invalid), "Zero patch control points were accepted");

#ifdef DY_TEST_NATIVE_SHADERS
        const std::array<float, 6> points = {-0.8f, -0.8f, 0.8f, -0.8f, 0.0f, 0.8f};
        auto* vertices = device->CreateBuffer({static_cast<uint32_t>(sizeof(points)), sizeof(float) * 2,
            RHI::BufferUsage::Vertex, RHI::ResourceState::CopyDestination});
        Check(vertices != nullptr, "Tessellation vertex buffer creation failed");
        RHI::TextureDesc targetDesc;
        targetDesc.width = targetDesc.height = 64;
        targetDesc.format = RHI::Format::R8G8B8A8_UNORM;
        targetDesc.usage = RHI::TextureUsage::RenderTarget;
        auto* target = device->CreateTexture(targetDesc);
        Check(target != nullptr, "Tessellation render target creation failed");
        auto* commands = device->AcquireCommandList();
        Check(commands != nullptr, "Tessellation command list creation failed");
        Check(device->UpdateBuffer(*commands, vertices, 0, points.data(), sizeof(points)),
            "Tessellation vertex upload failed");
        const RHI::ResourceBarrierDesc barriers[] = {
            {vertices, nullptr, RHI::ResourceState::CopyDestination, RHI::ResourceState::VertexBuffer, {}},
            {nullptr, target, RHI::ResourceState::Undefined, RHI::ResourceState::RenderTarget, {}}
        };
        commands->ResourceBarrier(barriers, 2);
        RHI::ColorAttachment color;
        color.texture = target;
        color.loadOp = RHI::LoadOp::Clear;
        color.storeOp = RHI::StoreOp::Store;
        commands->BeginRendering({&color, 1, nullptr});
        commands->BindGraphicsPipeline(pipeline);
        commands->BindVertexBuffer(0, vertices, 0);
        commands->SetViewport({0, 0, 64, 64, 0, 1});
        commands->SetScissor({0, 0, 64, 64});
        commands->DrawInstanced(3, 1, 0, 0);
        commands->EndRendering();
        Check(commands->Close(), "Tessellation command recording failed");
        Check(device->Submit(&commands, 1), "Tessellation GPU submission failed");
        Check(device->WaitIdle(), "Tessellation GPU wait failed");
        RHI::TextureReadback pixels;
        Check(device->ReadTexture(target, pixels), "Tessellation readback failed");
        const auto* center = pixels.pixels.data() + std::size_t(32) * pixels.rowPitch + 32 * 4;
        Check(center[0] > 200 && center[1] > 200 && center[2] > 200,
            "Tessellation draw did not cover the render target center");
        device->DestroyCommandList(commands);
        device->DestroyTexture(target);
        device->DestroyBuffer(vertices);
#endif

        device->DestroyPipeline(pipeline);
#ifdef DY_TEST_NATIVE_SHADERS
        device->DestroyShader(fragment);
#endif
        device->DestroyShader(domain);
        device->DestroyShader(hull);
        device->DestroyShader(vertex);
        return 0;
    }
    catch(const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
