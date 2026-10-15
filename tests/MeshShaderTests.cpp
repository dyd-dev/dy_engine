#include <dyf/RHI.h>
#include "../src/RHI/Validation.h"

#ifdef DY_TEST_NATIVE_SHADERS
#include "mesh_test_ms.h"
#include "mesh_test_ps.h"
#endif

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
}

int main()
{
    try
    {
        std::unique_ptr<RHI::IDevice> device(RHI::IDevice::Create({}));
        Check(device != nullptr, "Device creation failed");
        if(!device->Supports(RHI::Feature::MeshShader)) return 77;
        Check(!device->Supports(RHI::Feature::TaskShader), "Task shaders are outside this implementation");
#ifdef DY_TEST_NATIVE_SHADERS
        auto* mesh = device->CreateShader({RHI::ShaderStage::Mesh, ShaderData::mesh_test_msEntryPoint,
            ShaderData::mesh_test_ms, ShaderData::mesh_test_msSize});
        auto* fragment = device->CreateShader({RHI::ShaderStage::Fragment, ShaderData::mesh_test_psEntryPoint,
            ShaderData::mesh_test_ps, ShaderData::mesh_test_psSize});
#else
        const unsigned char binary[] = {1};
        auto* mesh = device->CreateShader({RHI::ShaderStage::Mesh, "main", binary, sizeof(binary)});
        auto* fragment = device->CreateShader({RHI::ShaderStage::Fragment, "main", binary, sizeof(binary)});
        for(auto stage : {RHI::ShaderStage::Hull, RHI::ShaderStage::Domain})
        {
            auto* shader = device->CreateShader({stage, "main", binary, sizeof(binary)});
            Check(shader != nullptr, "Existing shader stage was lost during integration");
            device->DestroyShader(shader);
        }
#endif
        Check(mesh && fragment, "Mesh shader creation failed");
        RHI::ColorAttachmentDesc colorFormat{RHI::Format::R8G8B8A8_UNORM, {}, RHI::ColorWriteMask::All};
        RHI::MeshPipelineDesc desc;
        desc.meshShader = mesh;
        desc.fragmentShader = fragment;
        desc.raster.fillMode = RHI::FillMode::Solid;
        desc.raster.cullMode = RHI::CullMode::None;
        desc.raster.frontFace = RHI::FrontFace::CounterClockwise;
        desc.colorAttachments = &colorFormat;
        desc.colorAttachmentCount = 1;
        Check(device->Supports(desc), "Valid mesh pipeline was rejected");
        auto* pipeline = device->CreateMeshPipeline(desc);
        Check(pipeline && pipeline->IsMesh() && !pipeline->IsCompute(), "Mesh pipeline was confused with compute");

        auto stencil = desc;
        stencil.depthStencil.stencilEnabled = true;
        stencil.depthStencil.stencilReadMask = stencil.depthStencil.stencilWriteMask = 255;
        stencil.depthStencil.front = stencil.depthStencil.back = {
            RHI::StencilOp::Keep, RHI::StencilOp::Keep, RHI::StencilOp::Replace, RHI::CompareOp::Always};
        for(auto format : {RHI::Format::D24_UNORM_S8_UINT, RHI::Format::D32_FLOAT_S8_UINT})
        {
            stencil.depthStencil.format = format;
            Check(RHI::ValidateMeshPipelineDesc(stencil), "Mesh validation rejected a depth/stencil format");
        }
        stencil.depthStencil.format = RHI::Format::D32_FLOAT;
        Check(!RHI::ValidateMeshPipelineDesc(stencil), "Mesh stencil accepted a depth-only format");
        stencil.depthStencil.format = RHI::Format::D32_FLOAT_S8_UINT;
        stencil.depthStencil.front.passOp = RHI::StencilOp::Undefined;
        Check(!RHI::ValidateMeshPipelineDesc(stencil), "D32S8 bypassed stencil operation validation");

        for(auto stage : {RHI::ShaderStageFlags::Vertex, RHI::ShaderStageFlags::Hull,
                RHI::ShaderStageFlags::Domain, RHI::ShaderStageFlags::Compute})
        {
            auto invalid = desc;
            invalid.layout.inlineConstantSize = 4;
            invalid.layout.inlineConstantStages = stage;
            Check(!device->Supports(invalid), "Mesh pipeline accepted an unrelated shader stage");
        }
        auto invalid = desc;
        invalid.meshShader = fragment;
        Check(!device->Supports(invalid), "Fragment shader was accepted as a mesh shader");
        invalid = desc;
        invalid.depthStencil.depthWriteEnabled = true;
        Check(!device->Supports(invalid), "Depth writes without a depth attachment were accepted");
        colorFormat.writeMask = static_cast<RHI::ColorWriteMask>(255);
        Check(!device->Supports(desc), "Invalid color write mask was accepted");
        colorFormat.writeMask = RHI::ColorWriteMask::All;

        RHI::TextureDesc targetDesc;
        targetDesc.width = targetDesc.height = 64;
        targetDesc.format = colorFormat.format;
        targetDesc.usage = RHI::TextureUsage::RenderTarget;
        auto* target = device->CreateTexture(targetDesc);
        Check(target != nullptr, "Mesh render target creation failed");
        auto record = [&](bool invalidDraw) {
            auto* commands = device->AcquireCommandList();
            Check(commands != nullptr, "Mesh command list creation failed");
            const RHI::ResourceBarrierDesc barrier{
                nullptr, target, RHI::ResourceState::Undefined, RHI::ResourceState::RenderTarget, {}};
            commands->ResourceBarrier(&barrier, 1);
            RHI::ColorAttachment color;
            color.texture = target;
            color.loadOp = RHI::LoadOp::Clear;
            color.storeOp = RHI::StoreOp::Store;
            commands->BeginRendering({&color, 1, nullptr});
            commands->BindGraphicsPipeline(pipeline);
            commands->SetViewport({0, 0, 64, 64, 0, 1});
            commands->SetScissor({0, 0, 64, 64});
            if(invalidDraw) commands->DrawInstanced(3, 1, 0, 0);
            else commands->DispatchMesh(1, 1, 1);
            commands->EndRendering();
            return commands;
        };
        auto* rejected = record(true);
        Check(!rejected->Close(), "Vertex draw was accepted on a mesh pipeline");
        device->DestroyCommandList(rejected);
        auto* commands = record(false);
        Check(commands->Close(), "Mesh command recording failed");
        Check(device->Submit(&commands, 1), "Mesh submission failed");
        Check(device->WaitIdle(), "Mesh completion wait failed");
#ifdef DY_TEST_NATIVE_SHADERS
        RHI::TextureReadback pixels;
        Check(device->ReadTexture(target, pixels), "Mesh readback failed");
        const auto* center = pixels.pixels.data() + 32 * pixels.rowPitch + 32 * 4;
        Check(unsigned(center[0]) + center[1] + center[2] > 150,
            "Mesh shader did not render a colored triangle at the target center");
#endif
        device->DestroyCommandList(commands);
        device->DestroyTexture(target);
        device->DestroyPipeline(pipeline);
        device->DestroyShader(fragment);
        device->DestroyShader(mesh);
        return 0;
    }
    catch(const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
