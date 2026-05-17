#include <iostream>
#include <memory>
#include <cmath>
#include <stdexcept>
#include <string>
#include "Platform/Window.h"
#include "RHI/IDevice.h"
#include "Graphics/AssetManager.h"
#include "Graphics/Material.h"
#include "Graphics/Renderer.h"
#include "Graphics/Scene.h"
#include "Math/Math.h"

using namespace dy;

namespace
{
    float Dot(const Math::float3& a, const Math::float3& b)
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    Math::float3 Cross(const Math::float3& a, const Math::float3& b)
    {
        return Math::float3(
            a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x);
    }

    Math::float3 Subtract(const Math::float3& a, const Math::float3& b)
    {
        return Math::float3(a.x - b.x, a.y - b.y, a.z - b.z);
    }

    Math::float3 NormalizeOr(const Math::float3& value, const Math::float3& fallback)
    {
        const float lengthSquared = Dot(value, value);
        if (lengthSquared <= 1.0e-8f) return fallback;
        const float invLength = 1.0f / std::sqrt(lengthSquared);
        return Math::float3(value.x * invLength, value.y * invLength, value.z * invLength);
    }

    Math::float4x4 MultiplyColumnMajor(const Math::float4x4& lhs, const Math::float4x4& rhs)
    {
        Math::float4x4 result = {};
        for (int column = 0; column < 4; ++column)
        {
            for (int row = 0; row < 4; ++row)
            {
                float value = 0.0f;
                for (int k = 0; k < 4; ++k)
                {
                    value += lhs.m[k * 4 + row] * rhs.m[column * 4 + k];
                }
                result.m[column * 4 + row] = value;
            }
        }
        return result;
    }

    Math::float4x4 CreateLookAt(const Math::float3& eye, const Math::float3& target, const Math::float3& up)
    {
        const Math::float3 forward = NormalizeOr(Subtract(target, eye), Math::float3(0.0f, 1.0f, 0.0f));
        const Math::float3 right = NormalizeOr(Cross(forward, up), Math::float3(1.0f, 0.0f, 0.0f));
        const Math::float3 cameraUp = Cross(right, forward);

        Math::float4x4 view = Math::float4x4::Identity();
        view.m[0] = right.x;
        view.m[4] = right.y;
        view.m[8] = right.z;
        view.m[12] = -Dot(right, eye);
        view.m[1] = cameraUp.x;
        view.m[5] = cameraUp.y;
        view.m[9] = cameraUp.z;
        view.m[13] = -Dot(cameraUp, eye);
        view.m[2] = -forward.x;
        view.m[6] = -forward.y;
        view.m[10] = -forward.z;
        view.m[14] = Dot(forward, eye);
        return view;
    }

    Math::float4x4 CreateOrthographic(float width, float height, float nearPlane, float farPlane)
    {
        Math::float4x4 projection = Math::float4x4::Identity();
        projection.m[0] = 2.0f / width;
        projection.m[5] = -2.0f / height;
        projection.m[10] = 1.0f / (nearPlane - farPlane);
        projection.m[14] = nearPlane / (nearPlane - farPlane);
        return projection;
    }

    Math::float4x4 CreateTestModelMatrix()
    {
        const float scale = 0.095f;
        const float yRadians = 0.78539816f;
        const float cosY = std::cos(yRadians);
        const float sinY = std::sin(yRadians);

        Math::float4x4 model = Math::float4x4::Identity();
        model.m[0] = scale * cosY;
        model.m[1] = scale * sinY;
        model.m[2] = 0.0f;
        model.m[4] = 0.0f;
        model.m[5] = scale;
        model.m[6] = 0.0f;
        model.m[8] = scale * -sinY;
        model.m[9] = 0.0f;
        model.m[10] = scale * cosY;
        model.m[14] = 0.02f;
        return model;
    }

    Math::float4x4 CreateTestViewProjectionMatrix()
    {
        const Math::float3 eye(0.0f, -2.35f, 1.15f);
        const Math::float3 target(0.0f, 0.0f, 0.28f);
        const Math::float4x4 view = CreateLookAt(eye, target, Math::float3(0.0f, 0.0f, 1.0f));
        const Math::float4x4 projection = CreateOrthographic(2.45f, 1.6f, 0.1f, 7.0f);
        return MultiplyColumnMajor(projection, view);
    }

    Vertex CreateFloorVertex(float x, float y, float z, float u, float v)
    {
        Vertex vertex = {};
        vertex.px = x;
        vertex.py = y;
        vertex.pz = z;
        vertex.u = u;
        vertex.v = v;
        vertex.nx = 0.0f;
        vertex.ny = 0.0f;
        vertex.nz = 1.0f;
        vertex.tx = 1.0f;
        vertex.ty = 0.0f;
        vertex.tz = 0.0f;
        vertex.tw = 1.0f;
        return vertex;
    }

    Mesh CreateFloorMesh()
    {
        Mesh mesh = {};
        mesh.vertices = {
            CreateFloorVertex(-1.25f, -1.05f, -0.015f, 0.0f, 0.0f),
            CreateFloorVertex( 1.25f, -1.05f, -0.015f, 1.0f, 0.0f),
            CreateFloorVertex( 1.25f,  1.05f, -0.015f, 1.0f, 1.0f),
            CreateFloorVertex(-1.25f,  1.05f, -0.015f, 0.0f, 1.0f)
        };
        mesh.indices = { 0, 1, 2, 0, 2, 3 };
        return mesh;
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

        Graphics::Scene scene;
        Graphics::AssetManager assets(scene);
        Graphics::MaterialDesc pbrMaterial = {};
        pbrMaterial.baseColor = Math::float4(0.95f, 0.72f, 0.42f, 1.0f);
        pbrMaterial.metallicFactor = 0.72f;
        pbrMaterial.roughnessFactor = 0.34f;
        pbrMaterial.normalScale = 0.18f;

        RenderFlags objectFlags = {};
        objectFlags.castShadow = true;
        objectFlags.receiveShadow = true;
        const Graphics::ObjectLoadResult object = assets.LoadObject("examples/vulkan_test/triangle.obj", pbrMaterial, CreateTestModelMatrix(), objectFlags);
        if (!object)
        {
            throw std::runtime_error("Could not load triangle.obj");
        }

        Graphics::MaterialDesc floorMaterial = {};
        floorMaterial.baseColor = Math::float4(0.52f, 0.57f, 0.60f, 1.0f);
        floorMaterial.roughnessFactor = 0.82f;
        const MeshID floorMesh = scene.CreateMesh(CreateFloorMesh());
        const MaterialID floorMaterialId = scene.CreateMaterial(floorMaterial);
        RenderFlags floorFlags = {};
        floorFlags.castShadow = false;
        floorFlags.receiveShadow = true;
        [[maybe_unused]] const EntityID floorEntity = scene.CreateEntity(floorMesh, floorMaterialId, Math::float4x4::Identity(), floorFlags);

        [[maybe_unused]] const uint32_t sunLight = scene.CreateDirectionalLight(
            Math::float3(0.36f, -0.54f, 0.76f),
            Math::float3(1.0f, 0.94f, 0.82f),
            4.0f,
            true,
            0.58f);

        Graphics::Renderer renderer;
        Graphics::RendererDesc rendererConfig = {};
        rendererConfig.viewProjectionMatrix = CreateTestViewProjectionMatrix();
        rendererConfig.cameraPosition = Math::float3(0.0f, -2.35f, 1.15f);
        rendererConfig.ambientIntensity = 0.045f;
        rendererConfig.pbr.minRoughness = 0.04f;
        rendererConfig.pbr.ambientSpecularStrength = 0.32f;
        rendererConfig.environment.specularColor = Math::float3(0.78f, 0.84f, 1.0f);
        rendererConfig.environment.specularIntensity = 0.85f;
        rendererConfig.enableShadows = true;
        rendererConfig.shadowMap.resolution = 2048;
        rendererConfig.shadowMap.nearPlane = 0.1f;
        rendererConfig.autoFitShadowMap = true;
        rendererConfig.shadowBoundsPadding = 0.35f;
        rendererConfig.shadowDepthBias = 0.0007f;
        rendererConfig.shadowSlopeBias = 0.003f;
        rendererConfig.shadowNormalBias = 0.0002f;
        rendererConfig.shadowPcfRadius = 1;
        if (!renderer.Initialize(device.get(), rendererConfig))
        {
            throw std::runtime_error("Failed to initialize renderer PBR pipeline");
        }

        while (window.IsRunning())
        {
            window.PollEvents();

            device->BeginFrame();
            renderer.Render(scene, device.get());
            device->Present();
        }

        renderer.Shutdown(device.get());
    }
    catch (const std::exception& e)
    {
        std::cerr << "Exception: " << e.what() << std::endl;
        return -1;
    }

    return 0;
}
