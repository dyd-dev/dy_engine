#include <dyf/RHI.h>

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

RHI::ShaderHandle Shader(RHI::IDevice& device, RHI::ShaderStage stage)
{
    static constexpr unsigned char binary[] = {0x44, 0x59, 0x46, 0x00};
    return device.CreateShader({stage, "main", binary, sizeof(binary)});
}
}

int main()
{
    try
    {
        std::unique_ptr<RHI::IDevice> device(RHI::IDevice::Create({}));
        Check(device != nullptr, "Null device creation failed");
        Check(device->Supports(RHI::Feature::Tessellation), "Tessellation feature was not exposed");
        Check(device->GetLimit(RHI::Limit::TessellationPatchControlPoints) >= 3,
            "Tessellation patch limit is too small");

        auto* vertex = Shader(*device, RHI::ShaderStage::Vertex);
        auto* hull = Shader(*device, RHI::ShaderStage::Hull);
        auto* domain = Shader(*device, RHI::ShaderStage::Domain);
        Check(vertex && hull && domain, "Tessellation shader creation failed");

        RHI::GraphicsPipelineDesc desc;
        desc.vertexShader = vertex;
        desc.hullShader = hull;
        desc.domainShader = domain;
        desc.topology = RHI::PrimitiveTopology::PatchList;
        desc.patchControlPoints = 3;
        desc.raster.fillMode = RHI::FillMode::Solid;
        desc.raster.cullMode = RHI::CullMode::Back;
        desc.raster.frontFace = RHI::FrontFace::CounterClockwise;
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

        device->DestroyPipeline(pipeline);
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
