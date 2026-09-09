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
        dy::Platform::Window window(width, height, "Graphics / Mesh and Material");
        RendererDesc desc;
        desc.meshShaders = {
            {ShaderData::vertex, ShaderData::vertexSize, ShaderData::vertexEntryPoint},
            {ShaderData::fragment, ShaderData::fragmentSize, ShaderData::fragmentEntryPoint}, {}};

        auto renderer = Renderer::Create(window.GetHandle(), desc);
        if (!renderer) throw std::runtime_error("Renderer creation failed.");
        Scene scene;
        const auto mesh = scene.CreateMesh(CreateCubeMesh(1));
        MaterialDesc textured;
        textured.baseColorTexture = scene.CreateTexture(DY_EXAMPLE_IMAGE);
        textured.roughnessFactor = 0.8f;
        MaterialDesc metallic;
        metallic.baseColor = {0.9f, 0.45f, 0.15f, 1};
        metallic.metallicFactor = 0.85f;
        metallic.roughnessFactor = 0.2f;
        const auto left = scene.CreateEntity(mesh, scene.CreateMaterial(textured), Math::Translation({-0.7f, 0, 0}) * Math::RotationZ(0.35f));
        const auto right = scene.CreateEntity(mesh, scene.CreateMaterial(metallic), Math::Translation({0.7f, 0, 0}) * Math::RotationZ(-0.35f));
        (void)left;
        (void)right;
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
