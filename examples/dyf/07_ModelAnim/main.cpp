#include "dyf.h"
#include <dyf/Extends/Model.h>
#include <chrono>

#define DY_EXAMPLE_MODEL "Models/Fox.glb"

int main()
{
	const uint32_t width = 640, height = 480;

	dyf::Platform::Window window(width, height, "dyf / Model and Animation");

	auto renderer = dyf::Renderer::Create(window.GetHandle());

	// 모델의 GPU 변형은 선택 확장이 준비하고 기본 Renderer에는 RHI 입력으로 전달한다.
	dyf::ModelRenderer modelRenderer(*renderer);
	dyf::ModelScene scene;
	// 파일과 배치 설정을 준비한 뒤 Scene에 추가된 인스턴스 ID를 받는다.
	dyf::ModelSceneDesc modelDesc;
	// Fox: © 2014 Public (CC0); animation © 2014 tomkranis,
	// glTF conversion © 2017 AsoboStudio/scurest (CC BY 4.0).
	// https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/Fox
	// https://creativecommons.org/licenses/by/4.0/
	modelDesc.path = DY_EXAMPLE_MODEL;
	modelDesc.normalize = false;
	// 예제 모델의 크기와 표시 방향을 지정한다.
	const float modelScale = 0.012f, modelRotationX = 1.5707963f, modelRotationY = 3.14159265f;
	modelDesc.transform = dyf::Math::Scaling(modelScale)
		* dyf::Math::RotationX(modelRotationX) * dyf::Math::RotationY(modelRotationY);

	dyf::ModelInstanceID modelInstance;
	dyf::AddModelToScene(scene, modelDesc, &modelInstance);
	scene.PlayAnimation(modelInstance, 0);

	dyf::DirectionalLight light;
	light.direction = {0, -1, 2};
	light.color = {1, 1, 1};
	light.intensity = 3;
	scene.Add(light);

	dyf::Camera camera;
	dyf::Math::float3 eye = {2.6f, -3, 1.8f}, target = {0, 0, 0.4f};
	camera.LookAt(eye, target);
	const float aspect = width / static_cast<float>(height);
	camera.SetPerspective(aspect);

	auto lastFrame = std::chrono::steady_clock::now();
	while(true)
	{
		window.PollEvents();
		if(!window.IsRunning()) break;

		const auto now = std::chrono::steady_clock::now();
		const float delta = std::chrono::duration<float>(now - lastFrame).count();
		lastFrame = now;
		scene.UpdateAnimations(delta);
		modelRenderer.Render(scene, camera);
	}
	return 0;
}
