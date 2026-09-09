#include "vertex.h"
#include "fragment.h"
#include "Platform/Window.h"
#include <stdexcept>
#include "Graphics/Camera.h"
#include "Graphics/Renderer.h"
#include "Graphics/Scene.h"
#include <iostream>
#include <chrono>

int main()
{
    using namespace dy;
    using namespace dy::Graphics;
    try {
        constexpr uint32_t width = 640, height = 480;
        dy::Platform::Window window(width, height, "Graphics / Model and Animation");
        RendererDesc desc;
        desc.meshShaders = {
            {ShaderData::vertex, ShaderData::vertexSize, ShaderData::vertexEntryPoint},
            {ShaderData::fragment, ShaderData::fragmentSize, ShaderData::fragmentEntryPoint}, {}};

        auto renderer = Renderer::Create(window.GetHandle(), desc);
        if (!renderer) throw std::runtime_error("Renderer creation failed.");
        Scene scene;
        ModelSceneDesc model;
        model.path = DY_EXAMPLE_MODEL;
        model.normalize = model.yUpToZUp = false;
        model.transform = Math::Scaling(0.012f) * Math::RotationX(1.5707963f);
        ModelInstanceID instance;
        if (!AddModelToScene(scene, model, &instance) || !scene.PlayAnimation(instance, 0))
            throw std::runtime_error("Animated model loading failed.");
        DirectionalLight light;
        light.direction = {0, -1, 2};
        light.color = {1, 1, 1};
        light.intensity = 3;
        const auto lightId = scene.CreateDirectionalLight(light);
        (void)lightId;
        Camera camera;
        camera.position = {2.6f, -3, 1.8f};
        camera.view = Math::LookAtRH(camera.position, {0, 0, 0.4f}, {0, 0, 1});
        camera.projection = Math::PerspectiveRH_ZO(1.0f, 640.0f / 480, 0.1f, 100);

        auto lastFrame = std::chrono::steady_clock::now();
        while (window.IsRunning()) {
            window.PollEvents();
            if (!window.IsRunning()) break;
            if (renderer->Render(scene, camera)) {
                const auto now = std::chrono::steady_clock::now();
                const float delta = std::chrono::duration<float>(now - lastFrame).count();
                lastFrame = now;
                if (!scene.UpdateAnimations(delta).Succeeded()) throw std::runtime_error("Animation pose evaluation failed.");
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
