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
        dy::Platform::Window window(width, height, "Graphics / Five light types");
        RendererDesc desc;
        desc.meshShaders = {
            {ShaderData::vertex, ShaderData::vertexSize, ShaderData::vertexEntryPoint},
            {ShaderData::fragment, ShaderData::fragmentSize, ShaderData::fragmentEntryPoint}, {}};

        auto renderer = Renderer::Create(window.GetHandle(), desc);
        if (!renderer) throw std::runtime_error("Renderer creation failed.");
        Scene scene;
        const auto mesh = scene.CreateMesh(CreateCubeMesh(1));
        MaterialDesc material;
        material.roughnessFactor = 0.65f;
        const auto surface = scene.CreateMaterial(material);
        (void)scene.CreateEntity(mesh, surface, Math::Translation({-0.7f,0,0}));
        (void)scene.CreateEntity(mesh, surface, Math::Translation({0.7f,0,0}));
        (void)scene.CreateEntity(mesh, surface, Math::Translation({0,0,-0.65f}) * Math::Scaling(Math::float3{3,3,0.1f}));

        DirectionalLight directional;
        directional.direction={0,-1,2}; directional.color={1,1,1}; directional.intensity=0.3f;
        (void)scene.CreateDirectionalLight(directional);
        PointLight point;
        point.position={-1,-1,1.4f}; point.range=5; point.color={1,0.2f,0.1f}; point.intensity=3;
        (void)scene.CreatePointLight(point);
        SpotLight spot;
        spot.position={1,-1,2}; spot.direction={-0.6f,0.3f,-1}; spot.range=5;
        spot.color={0.2f,0.3f,1}; spot.intensity=7; spot.innerConeRadians=0.35f; spot.outerConeRadians=0.8f;
        (void)scene.CreateSpotLight(spot);
        RectAreaLight rect;
        rect.position={0,1.4f,2}; rect.width=2; rect.height=1; rect.intensity=4;
        rect.color={1,0.7f,0.2f};
        (void)scene.CreateRectAreaLight(rect);
        DiscAreaLight disc;
        disc.position={-1.5f,0,2}; disc.radius=0.7f; disc.intensity=4; disc.color={0.2f,1,0.8f};
        (void)scene.CreateDiscAreaLight(disc);
        Camera camera;
        camera.position={2.6f,-4,2.7f};
        camera.view=Math::LookAtRH(camera.position,{0,0,0},{0,0,1});
        camera.projection=Math::PerspectiveRH_ZO(1.0f,640.0f/480,0.1f,100);

        while (window.IsRunning()) {
            window.PollEvents();
            if (!window.IsRunning()) break;
            (void)renderer->Render(scene,camera);
        }
    } catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
