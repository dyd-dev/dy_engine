#include "vertex.h"
#include "fragment.h"
#include "Platform/Window.h"
#include <stdexcept>
#include "Graphics/Camera.h"
#include "Graphics/Renderer.h"
#include "Graphics/Scene.h"
#include <iostream>

int main()
{
    using namespace dy;
    using namespace dy::Graphics;
    try {
        constexpr uint32_t width = 640, height = 480;
        dy::Platform::Window window(width, height, "Graphics / Geometry and Camera");
        RendererDesc desc;
        desc.meshShaders = {
            {ShaderData::vertex, ShaderData::vertexSize, ShaderData::vertexEntryPoint},
            {ShaderData::fragment, ShaderData::fragmentSize, ShaderData::fragmentEntryPoint}, {}};

        auto renderer = Renderer::Create(window.GetHandle(), desc);
        if (!renderer) throw std::runtime_error("Renderer creation failed.");

        Scene scene;
        MeshData triangle;
        for (const auto& position : {Math::float3{-0.9f, 0, -0.6f}, {0.9f, 0, -0.6f}, {0, 0, 0.9f}}) {
            Vertex vertex;
            vertex.position = position;
            vertex.normal = {0, -1, 0};
            vertex.uv = {0, 0};
            triangle.vertices.push_back(vertex);
        }
        triangle.indices = {0, 1, 2};
        MaterialDesc material;
        material.baseColor = {0.2f, 0.7f, 1, 1};
        const auto entity = scene.CreateEntity(scene.CreateMesh(triangle), scene.CreateMaterial(material));
        (void)entity;
        DirectionalLight light;
        light.direction = {0, -1, 2};
        light.color = {1, 1, 1};
        light.intensity = 3;
        const auto lightId = scene.CreateDirectionalLight(light);
        (void)lightId;
        Camera camera;
        camera.position = {0, -3, 1.6f};
        camera.view = Math::LookAtRH(camera.position, {0, 0, 0}, {0, 0, 1});
        camera.projection = Math::PerspectiveRH_ZO(1.0f, 640.0f / 480, 0.1f, 100);

        while (window.IsRunning()) {
            window.PollEvents();
            if (!window.IsRunning()) break;
            (void)renderer->Render(scene, camera);
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
