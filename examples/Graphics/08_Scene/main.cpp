#include "vertex.h"
#include "fragment.h"
#include "Platform/Window.h"
#include <stdexcept>
#include "Graphics/Camera.h"
#include "Graphics/Renderer.h"
#include "Graphics/Scene.h"
#include <array>
#include <iostream>
#include <chrono>

int main()
{
    using namespace dy;
    using namespace dy::Graphics;
    try {
        constexpr uint32_t width = 640, height = 480;
        dy::Platform::Window window(width, height, "Graphics / Scene");
        RendererDesc desc;
        desc.meshShaders = {
            {ShaderData::vertex, ShaderData::vertexSize, ShaderData::vertexEntryPoint},
            {ShaderData::fragment, ShaderData::fragmentSize, ShaderData::fragmentEntryPoint}, {}};

        auto renderer=Renderer::Create(window.GetHandle(),desc);
        if(!renderer) throw std::runtime_error("Renderer creation failed.");
        Scene scene;
        const auto mesh=scene.CreateMesh(CreateCubeMesh(1));
        std::array<EntityID,3> entities;
        const std::array<Math::float4,3> colors={{{1,0.3f,0.2f,1},{0.2f,0.8f,1,1},{0.5f,1,0.3f,1}}};
        for(uint32_t i=0;i<entities.size();++i) {
            MaterialDesc material;
            material.baseColor=colors[i];
            entities[i]=scene.CreateEntity(mesh,scene.CreateMaterial(material));
        }
        DirectionalLight light;
        light.direction={0,-1,2}; light.color={1,1,1}; light.intensity=3;
        (void)scene.CreateDirectionalLight(light);
        Camera camera;
        camera.position={0,-3,1.6f};
        camera.view=Math::LookAtRH(camera.position,{0,0,0},{0,0,1});
        camera.projection=Math::PerspectiveRH_ZO(1.0f,640.0f/480,0.1f,100);

        float time=0;
        auto lastFrame = std::chrono::steady_clock::now();
        while (window.IsRunning()) {
            window.PollEvents();
            if (!window.IsRunning()) break;
            const auto parent=Math::RotationZ(time);
            scene.GetTransform(entities[0]).worldMatrix=parent*Math::Translation({-0.8f,0,0})*Math::RotationX(0.4f);
            scene.GetTransform(entities[1]).worldMatrix=parent*Math::Translation({0.6f,0,0.2f})*Math::Scaling(0.6f);
            scene.GetTransform(entities[2]).worldMatrix=parent*Math::Translation({0.6f,0,0.85f})*Math::Scaling(0.3f);
            if(renderer->Render(scene,camera)) {
                const auto now = std::chrono::steady_clock::now();
                const float delta = std::chrono::duration<float>(now - lastFrame).count();
                lastFrame = now; time+=delta;
            }
        }
    } catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
