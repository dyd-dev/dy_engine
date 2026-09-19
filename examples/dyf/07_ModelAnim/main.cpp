#include "dyf.h"
#include <dyf/Extends/Model.h>

#include <cstdio>
#include <chrono>

#define DY_EXAMPLE_MODEL "Models/Fox.glb"

int main()
{
	const uint32_t width = 640, height = 480;

	dyf::Platform::Window window(width, height, "dyf / Model and Animation");
	if(!window.GetHandle()) return 1;

	auto renderer = dyf::Renderer::Create(window.GetHandle());
	if(!renderer) return 1;

	// 모델의 GPU 변형은 선택 확장이 준비하고 기본 Renderer에는 RHI 입력으로 전달한다.
	dyf::ModelRenderer modelRenderer(*renderer);
	dyf::ModelScene scene;
	// 파일과 배치 설정을 준비한 뒤 Scene에 추가된 인스턴스 ID를 받는다.
	dyf::ModelSceneDesc modelDesc;
	modelDesc.path = DY_EXAMPLE_MODEL;
	modelDesc.normalize = false;
	// 예제 모델의 크기와 표시 방향을 지정한다.
	const float modelScale = 0.012f, modelRotationX = 1.5707963f, modelRotationY = 3.14159265f;
	modelDesc.transform = dyf::Math::Scaling(modelScale)
		* dyf::Math::RotationX(modelRotationX) * dyf::Math::RotationY(modelRotationY);

	dyf::ModelInstanceID modelInstance;
	if(!dyf::AddModelToScene(scene, modelDesc, &modelInstance))
	{
		std::fprintf(stderr, "Animated model loading failed.\n");
		return 1;
	}
	if(!scene.PlayAnimation(modelInstance, 0))
	{
		std::fprintf(stderr, "Animation playback failed.\n");
		return 1;
	}

	dyf::DirectionalLight light;
	light.direction = {0, -1, 2};
	light.color = {1, 1, 1};
	light.intensity = 3;
	const auto lightId = scene.Add(light);
	if(!lightId) return 1;

	dyf::Camera camera;
	dyf::Math::float3 eye = {2.6f, -3, 1.8f}, target = {0, 0, 0.4f};
	if(!camera.LookAt(eye, target)) return 1;
	const float aspect = width / static_cast<float>(height);
	if(!camera.SetPerspective(aspect)) return 1;

	auto lastFrame = std::chrono::steady_clock::now();
	while(true)
	{
		window.PollEvents();
		if(!window.IsRunning()) break;

		const auto now = std::chrono::steady_clock::now();
		const float delta = std::chrono::duration<float>(now - lastFrame).count();
		lastFrame = now;
		if(!scene.UpdateAnimations(delta)) return 1;
		if(!modelRenderer.Render(scene, camera)) return 1;
	}
}
