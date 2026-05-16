#include <iostream>
#include <memory>
#include <vector>
#include <cmath>
#include <cstring>
#include <fstream>
#include <cstddef>
#include <cstdint>
#include "Platform/Window.h"
#include "RHI/IDevice.h"
#include "RHI/IBuffer.h"
#include "RHI/ICommandList.h"
#include "RHI/IPipelineState.h"
#include "Graphics/Material.h"
#include "Graphics/Mesh.h"
#include "Math/Math.h"

using namespace dy;

#ifndef DY_SHADER_DIR
#define DY_SHADER_DIR nullptr
#endif

namespace
{
    std::vector<char> ReadBinaryFile(const std::string& path)
    {
        std::ifstream file(path, std::ios::ate | std::ios::binary);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open shader file: " + path);
        }

        const size_t fileSize = static_cast<size_t>(file.tellg());
        std::vector<char> buffer(fileSize);
        file.seekg(0);
        file.read(buffer.data(), fileSize);
        return buffer;
    }
}

int main()
{
    try
    {
        Platform::Window window(1280, 720, "Vulkan OBJ Test");

        std::unique_ptr<RHI::IDevice> device(RHI::IDevice::Create(window.GetHandle()));
        if (!device)
        {
            throw std::runtime_error("Failed to initialize RHI device for Vulkan OBJ test");
        }

        const std::string shaderDir = DY_SHADER_DIR != nullptr ? DY_SHADER_DIR : "";
        const std::vector<char> vertexShader = ReadBinaryFile(shaderDir + "/triangle.vert.spv");
        const std::vector<char> pixelShader = ReadBinaryFile(shaderDir + "/triangle.frag.spv");
        RHI::GraphicsPipelineDesc pipelineDesc = {};
        pipelineDesc.vertexShader = vertexShader.data();
        pipelineDesc.vertexShaderSize = vertexShader.size();
        pipelineDesc.pixelShader = pixelShader.data();
        pipelineDesc.pixelShaderSize = pixelShader.size();
        pipelineDesc.depthEnable = true;
        RHI::IPipelineState* pipeline = device->CreateGraphicsPipeline(pipelineDesc);
        if (!pipeline)
        {
            throw std::runtime_error("Failed to create RHI graphics pipeline");
        }

        // Load the OBJ mesh from the example folder.
        Graphics::MeshData meshData;
        std::string objPath = "examples/vulkan_test/triangle.obj";
        if (!Graphics::Mesh::LoadFromOBJ(objPath, meshData))
        {
            std::cerr << "Failed to load OBJ file: " << objPath << std::endl;
            if (!Graphics::Mesh::LoadFromOBJ("../" + objPath, meshData))
            {
                if (!Graphics::Mesh::LoadFromOBJ("../../" + objPath, meshData))
                {
                    throw std::runtime_error("Could not find triangle.obj");
                }
            }
        }

        std::cout << "Loaded OBJ: " << meshData.vertices.size() << " vertices, "
                  << meshData.indices.size() << " indices." << std::endl;

        Material pbrMaterial = {};
        pbrMaterial.baseColor = Math::float4(0.95f, 0.72f, 0.42f, 1.0f);
        pbrMaterial.metallicFactor = 0.72f;
        pbrMaterial.roughnessFactor = 0.34f;
        pbrMaterial.normalScale = 0.18f;

        // Flatten the mesh into the Vulkan PBR vertex layout.
        std::vector<float> flatVertices;
        flatVertices.reserve(meshData.vertices.size() * 12);
        for (const auto& v : meshData.vertices)
        {
            flatVertices.push_back(v.position.x);
            flatVertices.push_back(v.position.y);
            flatVertices.push_back(v.position.z);
            flatVertices.push_back(v.normal.x);
            flatVertices.push_back(v.normal.y);
            flatVertices.push_back(v.normal.z);
            flatVertices.push_back(v.uv.x);
            flatVertices.push_back(v.uv.y);
            flatVertices.push_back(v.tangent.x);
            flatVertices.push_back(v.tangent.y);
            flatVertices.push_back(v.tangent.z);
            flatVertices.push_back(v.tangent.w);
        }

        const uint32_t vertexBufferSize = static_cast<uint32_t>(flatVertices.size() * sizeof(float));
        const uint32_t indexBufferSize = static_cast<uint32_t>(meshData.indices.size() * sizeof(uint32_t));
        const RHI::BufferUsage vertexStorageUsage = RHI::BufferUsage::Vertex | RHI::BufferUsage::Storage;
        const RHI::BufferUsage indexStorageUsage = RHI::BufferUsage::Index | RHI::BufferUsage::Storage;
        auto destroyBuffer = [devicePtr = device.get()](RHI::IBuffer* buffer)
        {
            if (buffer != nullptr) devicePtr->DestroyBuffer(buffer);
        };
        std::unique_ptr<RHI::IBuffer, decltype(destroyBuffer)> vertexBuffer(
            device->CreateBuffer(RHI::BufferDesc{ vertexBufferSize, 12u * static_cast<uint32_t>(sizeof(float)), vertexStorageUsage }),
            destroyBuffer
        );
        std::unique_ptr<RHI::IBuffer, decltype(destroyBuffer)> indexBuffer(
            device->CreateBuffer(RHI::BufferDesc{ indexBufferSize, static_cast<uint32_t>(sizeof(uint32_t)), indexStorageUsage }),
            destroyBuffer
        );
        if (!vertexBuffer || !indexBuffer)
        {
            throw std::runtime_error("Failed to create RHI mesh buffers");
        }

        void* vertexData = vertexBuffer->Map(0, vertexBufferSize);
        if (vertexData == nullptr) throw std::runtime_error("Failed to map vertex buffer");
        std::memcpy(vertexData, flatVertices.data(), vertexBufferSize);
        vertexBuffer->Unmap();

        void* indexData = indexBuffer->Map(0, indexBufferSize);
        if (indexData == nullptr) throw std::runtime_error("Failed to map index buffer");
        std::memcpy(indexData, meshData.indices.data(), indexBufferSize);
        indexBuffer->Unmap();

        const float scale = 0.1f;
        const float yRadians = 0.78539816f;
        const float xRadians = -0.61547971f;
        const float cosY = std::cos(yRadians);
        const float sinY = std::sin(yRadians);
        const float cosX = std::cos(xRadians);
        const float sinX = std::sin(xRadians);

        Math::float4x4 fixedModel = Math::float4x4::Identity();
        fixedModel.m[0] = scale * cosY;
        fixedModel.m[1] = scale * sinX * sinY;
        fixedModel.m[2] = scale * cosX * sinY;
        fixedModel.m[4] = 0.0f;
        fixedModel.m[5] = scale * cosX;
        fixedModel.m[6] = scale * -sinX;
        fixedModel.m[8] = scale * -sinY;
        fixedModel.m[9] = scale * sinX * cosY;
        fixedModel.m[10] = scale * cosX * cosY;

        Math::float4x4 fixedViewProj = Math::float4x4::Identity();
        fixedViewProj.m[0] = 0.85f;
        fixedViewProj.m[5] = 0.85f;
        fixedViewProj.m[10] = 0.5f;
        fixedViewProj.m[14] = 0.5f;

        while (window.IsRunning())
        {
            window.PollEvents();

            device->BeginFrame();
            RHI::ICommandList* cmdList = device->AcquireCommandList();
            cmdList->ClearColor(device->GetBackBuffer(), 0.05f, 0.07f, 0.10f, 1.0f);
            cmdList->BindGraphicsPipeline(pipeline);
            RHI::GeometryBinding geometry = {};
            geometry.vertexBuffer = vertexBuffer.get();
            geometry.vertexStride = 12u * static_cast<uint32_t>(sizeof(float));
            geometry.vertexOffset = 0;
            geometry.indexBuffer = indexBuffer.get();
            geometry.indexFormat = RHI::Format::R32_UINT;
            geometry.indexOffset = 0;
            cmdList->BindGeometry(geometry);

            struct PushData {
                Math::float4x4 viewProj;
                Math::float4x4 model;
                float drawMode;
                uint32_t firstIndex;
                int32_t vertexOffset;
                uint32_t firstVertex;
                float padding;
                Math::float4 baseColorFactor;
                Math::float4 materialParams;
            };
            static_assert(offsetof(PushData, firstIndex) == 132u, "Vulkan backend metadata offset must match shader layout.");
            static_assert(offsetof(PushData, baseColorFactor) == 160u, "PBR material constants must match shader layout.");
            static_assert(sizeof(PushData) == 192u, "Vulkan push constant range is 192 bytes.");
            PushData pushData = {};

            pushData.viewProj = fixedViewProj;
            pushData.model = fixedModel;
            pushData.drawMode = 0.0f;
            pushData.baseColorFactor = pbrMaterial.baseColor;
            pushData.materialParams = Math::float4(
                pbrMaterial.metallicFactor,
                pbrMaterial.roughnessFactor,
                pbrMaterial.normalScale,
                1.0f);

            cmdList->SetPushConstants(sizeof(pushData), &pushData);
            cmdList->DrawIndexedInstanced(static_cast<uint32_t>(meshData.indices.size()), 1, 0, 0, 0);

            cmdList->Close();

            RHI::ICommandList* submitLists[] = { cmdList };
            device->Submit(submitLists, 1);

            device->Present();
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "Exception: " << e.what() << std::endl;
        return -1;
    }

    return 0;
}
