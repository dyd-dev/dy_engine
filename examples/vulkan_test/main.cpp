#include <iostream>
#include <memory>
#include <vector>
#include <cmath>
#include "Platform/Window.h"
#include "RHI/IDevice.h"
#include "RHI/ICommandList.h"
#include "Backends/Vulkan/VulkanDevice.h"
#include "Graphics/Mesh.h"
#include "Math/Math.h"

using namespace dy;

int main()
{
    try
    {
        Platform::Window window(1280, 720, "Vulkan OBJ Test");

        std::unique_ptr<VulkanDevice> device = std::make_unique<VulkanDevice>();

        if (device->InitializeForTest(window.GetHandle(), VULKAN_TEST_SHADER_DIR) != 0)
        {
            throw std::runtime_error("Failed to initialize VulkanDevice for test");
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

        // Flatten the mesh into the Vulkan test vertex layout.
        std::vector<float> flatVertices;
        flatVertices.reserve(meshData.vertices.size() * 8);
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
        }

        device->UploadTestMesh(flatVertices, meshData.indices);

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

            struct {
                Math::float4x4 viewProj;
                Math::float4x4 model;
            } pushData;

            pushData.viewProj = fixedViewProj;
            pushData.model = fixedModel;

            cmdList->SetPushConstants(sizeof(pushData), &pushData);
            cmdList->DrawInstanced(0, 1, 0, 0);

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
