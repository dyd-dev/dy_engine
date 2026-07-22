// 07_RenderPath - render-path / SIMD benchmark.
//
// Compares draw-submission strategies under a heavy, multi-model, multi-texture
// scene, plus an isolated SIMD matrix-multiply micro-benchmark.
//
//   Binding mode (runtime):  --per-draw | --batched | --bindless
//   Instance count (runtime): --count=N
//   SIMD (build flag):        cmake -DDY_ENABLE_SIMD=ON|OFF   (reported at startup)
//
// Models are reused from 05_LoadModel (copied into the working dir by CMake).
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "Core/Types.h"
#include "Graphics/Mesh.h"
#include "Graphics/Renderer.h"
#include "Graphics/Scene.h"
#include "Math/Math.h"
#include "Platform/Window.h"
#include "RHI/IDevice.h"

#ifndef DY_SHADER_DIR
#define DY_SHADER_DIR "./Shaders"
#endif

using namespace dy;

namespace
{
	const char* ShaderExt()
	{
#if defined(ENABLE_METAL)
	return ".metallib";
#elif defined(ENABLE_VULKAN)
	return ".spv";
#elif defined(ENABLE_D3D12)
	return ".dxil";
#else
		return ".glsl";
#endif
	}

	Graphics::RendererBindingMode SelectBindingMode(int argc, char** argv)
	{
		for(int i = 1; i < argc; ++i)
		{
			const std::string arg = argv[i] != nullptr ? argv[i] : "";
			if(arg == "--per-draw" || arg == "--binding=per-draw") return Graphics::RendererBindingMode::PerDrawBind;
			if(arg == "--batched"  || arg == "--binding=batched")  return Graphics::RendererBindingMode::BatchedBind;
			if(arg == "--bindless" || arg == "--binding=bindless") return Graphics::RendererBindingMode::Bindless;
		}
		return Graphics::RendererBindingMode::PerDrawBind;
	}

	const char* BindingModeName(Graphics::RendererBindingMode mode)
	{
		switch(mode)
		{
		case Graphics::RendererBindingMode::PerDrawBind: return "PerDrawBind";
		case Graphics::RendererBindingMode::BatchedBind: return "BatchedBind";
		case Graphics::RendererBindingMode::Bindless:    return "Bindless";
		}
		return "Unknown";
	}

	uint32_t ParseCount(int argc, char** argv, uint32_t fallback)
	{
		for(int i = 1; i < argc; ++i)
		{
			const std::string arg = argv[i] != nullptr ? argv[i] : "";
			const std::string key = "--count=";
			if(arg.rfind(key, 0) == 0)
			{
				const long value = std::strtol(arg.c_str() + key.size(), nullptr, 10);
				if(value > 0) return static_cast<uint32_t>(value);
			}
		}
		return fallback;
	}

	uint32_t HashUInt(uint32_t value)
	{
		value ^= value >> 16u; value *= 0x7feb352du;
		value ^= value >> 15u; value *= 0x846ca68bu;
		value ^= value >> 16u; return value;
	}

	float Random01(uint32_t index, uint32_t salt)
	{
		return static_cast<float>(HashUInt(index ^ (salt * 0x9e3779b9u)) & 0x00ffffffu) / static_cast<float>(0x00ffffffu);
	}

	// 렌더링과 분리된 SIMD 측정: 대량 행렬곱을 반복 실행한 시간(ms)을 돌려준다.
	// DY_ENABLE_SIMD 빌드 여부에 따라 float4x4::Multiply 가 SSE/NEON 또는 스칼라로 컴파일된다.
	double RunMatrixMulBenchmarkMs(uint32_t matrixCount, uint32_t iterations)
	{
		std::vector<Math::float4x4> a(matrixCount), b(matrixCount), out(matrixCount);
		for(uint32_t i = 0; i < matrixCount; ++i)
		{
			a[i] = Math::RotationZ(static_cast<float>(i) * 0.001f);
			b[i] = Math::Translation(Math::float3(static_cast<float>(i) * 0.01f, 1.0f, -1.0f));
		}

		const auto start = std::chrono::steady_clock::now();
		for(uint32_t iter = 0; iter < iterations; ++iter)
		{
			Math::MultiplyMatricesBatch(a.data(), b.data(), out.data(), matrixCount);
		}
		const auto end = std::chrono::steady_clock::now();

		// 죽은 코드 제거 방지: 결과 일부를 합산해 사용한다.
		volatile float sink = 0.0f;
		for(uint32_t i = 0; i < matrixCount; ++i) sink += out[i].m[12];
		(void)sink;

		return std::chrono::duration<double, std::milli>(end - start).count();
	}

	const char* SimdState()
	{
#if defined(DY_SIMD_ENABLED)
		return "ON";
#else
		return "OFF";
#endif
	}

	// 인덱스로부터 결정적으로 산출되는 인스턴스별 애니메이션 파라미터.
	struct Instance
	{
		EntityID entity = EntityID::Invalid;
		Math::float3 position;
		Math::float4x4 modelMatrix = Math::float4x4::Identity();
		float spin = 0.0f;
		float phase = 0.0f;
		float scale = 1.0f;
	};
}

int main(int argc, char** argv)
{
	try
	{
		const Graphics::RendererBindingMode bindingMode = SelectBindingMode(argc, argv);
		const uint32_t targetCount = ParseCount(argc, argv, 2000u);

		Platform::Window window(1280, 720, "RenderPath");

		RHI::DeviceDesc deviceDesc = {};
		std::unique_ptr<RHI::IDevice> device(RHI::IDevice::Create(window.GetHandle(), deviceDesc));
		if(!device) throw std::runtime_error("Failed to create RHI device");

		// ----- SIMD 마이크로벤치 (렌더링과 무관) -----
		const double simdMs = RunMatrixMulBenchmarkMs(100000u, 20u);

		// ----- 씬: 05 의 텍스처 모델들을 1회씩 로드 → 메시/머티리얼 템플릿 확보 -----
		Graphics::Scene scene;
		const char* modelPaths[] = {
			"Models/Duck/glTF/Duck.gltf",
			"Models/Avocado/glTF/Avocado.gltf",
			"Models/BoomBox/glTF/BoomBox.gltf",
			"Models/DamagedHelmet/glTF/DamagedHelmet.gltf",
			"Models/WaterBottle/glTF/WaterBottle.gltf",
			"Models/Lowpoly_tree/Lowpoly_tree.obj",
			"Models/shiba/scene.FBX",
		};
		uint32_t loadedModelCount = 0;
		for(const char* path : modelPaths)
		{
			Graphics::ModelSceneDesc desc = {};
			desc.path = path;
			desc.normalizedSize = 1.6f;
			if(Graphics::AddModelToScene(scene, desc)) ++loadedModelCount;
		}

		// 로드로 만들어진 엔티티들이 (메시, 머티리얼) 템플릿이 된다.
		const uint32_t templateCount = scene.GetEntityCount();
		if(templateCount == 0u)
		{
			// 모델 로드 실패 시 큐브로 폴백(벤치는 계속 동작).
			const MeshID cube = scene.CreateMesh(Graphics::CreateCubeMesh(1.0f));
			Graphics::MaterialDesc mat = {};
			mat.baseColor = Math::float4(0.8f, 0.4f, 0.3f, 1.0f);
			const MaterialID matId = scene.CreateMaterial(mat);
			(void)scene.CreateEntity(cube, matId);
		}
		const uint32_t baseCount = scene.GetEntityCount();

		// 템플릿을 재사용해 targetCount 까지 인스턴스를 채운다(재로딩/메시 중복 없음).
		std::vector<MeshID> templateMeshes(baseCount);
		std::vector<MaterialID> templateMaterials(baseCount);
		std::vector<Math::float4x4> templateTransforms(baseCount);
		for(uint32_t i = 0; i < baseCount; ++i)
		{
			const EntityID e = static_cast<EntityID>(i);
			templateMeshes[i] = scene.GetEntityMesh(e);
			templateMaterials[i] = scene.GetEntityMaterial(e);
			templateTransforms[i] = scene.GetTransform(e).worldMatrix;
		}
		for(uint32_t i = baseCount; i < targetCount; ++i)
		{
			const uint32_t t = i % baseCount;
			(void)scene.CreateEntity(templateMeshes[t], templateMaterials[t], templateTransforms[t]);
		}

		// 인스턴스별 배치/애니메이션 파라미터(그리드 + 회전). 인덱스 기반 결정적.
		const uint32_t instanceCount = scene.GetEntityCount();
		const uint32_t side = static_cast<uint32_t>(std::ceil(std::cbrt(static_cast<double>(instanceCount))));
		const float spacing = 3.0f;
		const float gridOrigin = -0.5f * static_cast<float>(side - 1u) * spacing;
		std::vector<Instance> instances(instanceCount);
		for(uint32_t i = 0; i < instanceCount; ++i)
		{
			const uint32_t gx = i % side;
			const uint32_t gy = (i / side) % side;
			const uint32_t gz = i / (side * side);
			Instance& inst = instances[i];
			inst.entity = static_cast<EntityID>(i);
			inst.modelMatrix = templateTransforms[i % baseCount];
			inst.position = Math::float3(
				gridOrigin + static_cast<float>(gx) * spacing,
				gridOrigin + static_cast<float>(gy) * spacing,
				gridOrigin + static_cast<float>(gz) * spacing);
			inst.spin = (Random01(i, 7u) - 0.5f) * 2.0f;
			inst.phase = Random01(i, 13u) * 6.2831853f;
			inst.scale = 0.9f + Random01(i, 23u) * 0.2f;
		}

		// ----- 렌더러 -----
		const std::string ext = ShaderExt();
		const std::string vsPath = std::string(DY_SHADER_DIR) + "/mesh_vs" + ext;
		const std::string fragmentShaderPath = std::string(DY_SHADER_DIR) + "/mesh_ps" + ext;

		Graphics::RendererDesc cfg = {};
		cfg.bindingMode = bindingMode;
		cfg.vertexShaderPath = vsPath.c_str();
		cfg.fragmentShaderPath = fragmentShaderPath.c_str();
		cfg.clearColor = Math::float4(0.02f, 0.03f, 0.05f, 1.0f);

		Graphics::Renderer renderer;
		if(!renderer.Initialize(device.get(), cfg)) throw std::runtime_error("Failed to initialize renderer");

		const float gridExtent = static_cast<float>(side) * spacing;
		Graphics::CameraDesc camera = {};
		camera.eye = Math::float3(gridExtent, -gridExtent * 1.2f, gridExtent * 0.7f);
		camera.aspect = 1280.0f / 720.0f;
		camera.farPlane = gridExtent * 4.0f + 50.0f;
		renderer.SetCamera(camera);

		Graphics::DirectionalLight light = {};
		light.direction = Math::float3(0.5f, 0.6f, 0.6f);
		light.intensity = 3.5f;
		light.castShadow = false;
		const DirectionalLightID lightId = scene.CreateDirectionalLight(light);

		// ----- 시작 정보 출력 -----
		std::cout << "=== RenderPath ===\n"
		          << "  binding mode : " << BindingModeName(bindingMode) << "\n"
		          << "  SIMD         : " << SimdState() << "\n"
		          << "  models loaded: " << loadedModelCount << ", template entities: " << baseCount << "\n"
		          << "  instances    : " << instanceCount << "\n"
		          << "  meshes       : " << scene.GetMeshCount() << ", materials: " << scene.GetMaterialCount()
		          << ", textures: " << scene.GetTextureCount() << "\n"
		          << "  SIMD matmul  : " << simdMs << " ms (2,000,000 muls)\n"
		          << "=========================\n";

		auto lastReport = std::chrono::steady_clock::now();
		const auto startTime = lastReport;
		uint32_t framesSinceReport = 0;

		while(window.IsRunning())
		{
			window.PollEvents();
			const auto now = std::chrono::steady_clock::now();
			const float seconds = std::chrono::duration<float>(now - startTime).count();

			// 매 프레임 인스턴스 변환 갱신(CPU 행렬 수학 부하 = SIMD 민감 구간).
			for(const Instance& inst : instances)
			{
				scene.GetTransform(inst.entity).worldMatrix =
					Math::Translation(inst.position) *
					Math::RotationZ(inst.phase + seconds * inst.spin) *
					Math::Scaling(inst.scale) *
					inst.modelMatrix;
			}

			// 그리드 중심 공전 카메라 + 함께 회전하는 라이트.
			const float a = seconds * 0.3f;
			camera.eye = Math::float3(
				camera.target.x + gridExtent * 1.5f * std::cos(a),
				camera.target.y + gridExtent * 1.5f * std::sin(a),
				camera.target.z + gridExtent * 0.7f);
			renderer.SetCamera(camera);
			Graphics::DirectionalLight rotated = light;
			rotated.direction = Math::float3(std::cos(a) * 0.6f, std::sin(a) * 0.6f, 0.6f);
			scene.SetDirectionalLight(ToIndex(lightId), rotated);

			if(!device->BeginFrame()) continue;
			renderer.Render(scene, device.get());
			device->Present();

			// 1초마다 프레임타임/FPS 출력.
			++framesSinceReport;
			const double sinceReport = std::chrono::duration<double>(now - lastReport).count();
			if(sinceReport >= 1.0)
			{
				const double fps = framesSinceReport / sinceReport;
				const double frameMs = 1000.0 / fps;
				std::cout << "[" << BindingModeName(bindingMode) << " SIMD=" << SimdState() << "] "
				          << fps << " fps, " << frameMs << " ms/frame\n";
				framesSinceReport = 0;
				lastReport = now;
			}
		}

		renderer.Shutdown(device.get());
		return 0;
	}
	catch(const std::exception& exception)
	{
		std::cerr << exception.what() << '\n';
		return -1;
	}
}
