// 확정된 측정 사용 흐름. 목표 Graphics API를 사용하며 현재 빌드 대상은 아니다.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <charconv>
#include <limits>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "Platform/Window.h"
#include "Graphics/Mesh.h"
#include "Graphics/Renderer.h"
#include "Graphics/Scene.h"
#include "Math/Math.h"

using namespace dy;

namespace
{
	using Clock = std::chrono::steady_clock;

	struct Options
	{
		uint32_t entityCount = 1000u;
		uint32_t warmUpFrameCount = 60u;
		uint32_t measuredFrameCount = 300u;
		uint32_t width = 1280u;
		uint32_t height = 720u;
		bool vsync = false;
		Graphics::RendererBindingMode bindingMode = Graphics::RendererBindingMode::PerDrawBind;
	};

	struct Statistics
	{
		double meanMilliseconds = 0.0;
		double minimumMilliseconds = 0.0;
		double p50Milliseconds = 0.0;
		double p95Milliseconds = 0.0;
		double maximumMilliseconds = 0.0;
	};

	uint32_t ParsePositiveArgument(int argc, char** argv, const char* name, uint32_t fallback)
	{
		const std::string prefix = std::string(name) + "=";
		for(int i = 1; i < argc; ++i)
		{
			const std::string argument = argv[i];
			if(argument.rfind(prefix, 0) != 0) continue;
			uint32_t value = 0;
			const auto result = std::from_chars(argument.data() + prefix.size(), argument.data() + argument.size(), value);
			if(result.ec != std::errc{} || result.ptr != argument.data() + argument.size() || value == 0)
				throw std::invalid_argument("Expected a positive uint32 for " + std::string(name));
			return value;
		}
		return fallback;
	}

	bool ParseBooleanArgument(int argc, char** argv, const char* name, bool fallback)
	{
		const std::string prefix = std::string(name) + "=";
		for(int i = 1; i < argc; ++i)
		{
			const std::string argument = argv[i] != nullptr ? argv[i] : "";
			if(argument == prefix + "0") return false;
			if(argument == prefix + "1") return true;
			if(argument.rfind(prefix, 0) == 0)
				throw std::invalid_argument("Expected 0 or 1 for " + std::string(name));
		}
		return fallback;
	}

	Options ParseOptions(int argc, char** argv)
	{
		Options options;
		options.entityCount = static_cast<uint32_t>(ParsePositiveArgument(argc, argv, "--count", options.entityCount));
		options.warmUpFrameCount = static_cast<uint32_t>(ParsePositiveArgument(argc, argv, "--warmup", options.warmUpFrameCount));
		options.measuredFrameCount = static_cast<uint32_t>(ParsePositiveArgument(argc, argv, "--frames", options.measuredFrameCount));
		options.width = static_cast<uint32_t>(ParsePositiveArgument(argc, argv, "--width", options.width));
		options.height = static_cast<uint32_t>(ParsePositiveArgument(argc, argv, "--height", options.height));
		options.vsync = ParseBooleanArgument(argc, argv, "--vsync", options.vsync);
		for(int i = 1; i < argc; ++i)
		{
			const std::string argument = argv[i];
			if(argument == "--binding=per-draw") options.bindingMode = Graphics::RendererBindingMode::PerDrawBind;
			else if(argument == "--binding=batched") options.bindingMode = Graphics::RendererBindingMode::BatchedBind;
			else if(argument == "--binding=bindless") options.bindingMode = Graphics::RendererBindingMode::Bindless;
			else if(argument.rfind("--binding=", 0) == 0) throw std::invalid_argument("Unknown binding mode");
		}
		return options;
	}

	const char* BuildConfiguration()
	{
#if defined(NDEBUG)
		return "Release (NDEBUG)";
#else
		return "Debug (assertions enabled)";
#endif
	}

	Statistics Summarize(std::vector<double> samples)
	{
		if(samples.empty()) throw std::runtime_error("No rendering samples were collected");

		double total = 0.0;
		for(double sample : samples) total += sample;
		std::sort(samples.begin(), samples.end());
		const size_t p50Index = static_cast<size_t>(std::ceil(0.50 * static_cast<double>(samples.size()))) - 1u;
		const size_t p95Index = static_cast<size_t>(std::ceil(0.95 * static_cast<double>(samples.size()))) - 1u;
		return Statistics{
			total / static_cast<double>(samples.size()),
			samples.front(),
			samples[p50Index],
			samples[p95Index],
			samples.back()
		};
	}

	void RenderFrames(
		Platform::Window& window,
		Graphics::Renderer& renderer,
		const Graphics::Scene& scene,
		const Graphics::CameraDesc& camera,
		uint32_t frameCount)
	{
		for(uint32_t frame = 0; frame < frameCount; ++frame)
		{
			if(!window.IsRunning()) throw std::runtime_error("Window closed during warm-up");
			window.PollEvents();
			if(!window.IsRunning()) throw std::runtime_error("Window closed before frame submission");
			if(!renderer.Render(scene, camera)) throw std::runtime_error("Frame was not submitted");
		}
	}

	std::vector<double> MeasureRenderCalls(
		Platform::Window& window,
		Graphics::Renderer& renderer,
		const Graphics::Scene& scene,
		const Graphics::CameraDesc& camera,
		uint32_t frameCount)
	{
		std::vector<double> samples;
		samples.reserve(frameCount);
		for(uint32_t frame = 0; frame < frameCount; ++frame)
		{
			if(!window.IsRunning()) throw std::runtime_error("Window closed during measurement");
			window.PollEvents();

			const auto start = Clock::now();
			if(!renderer.Render(scene, camera)) throw std::runtime_error("Frame was not submitted");
			const auto end = Clock::now();
			samples.push_back(std::chrono::duration<double, std::milli>(end - start).count());
		}
		return samples;
	}
}

int main(int argc, char** argv)
{
	try
	{
		const Options options = ParseOptions(argc, argv);
		Platform::Window window(options.width, options.height, "Rendering performance");

		Graphics::RendererDesc rendererDesc = {};
		rendererDesc.vsync = options.vsync;
		// 지정한 binding mode가 미지원이면 생성 실패로 알린다. 측정 경로를 자동 대체하지 않는다.
		rendererDesc.bindingMode = options.bindingMode;
		rendererDesc.enableProfilerHud = false;
		rendererDesc.enableShadows = false;
		auto renderer = Graphics::Renderer::Create(window.GetHandle(), rendererDesc);
		if(!renderer) throw std::runtime_error("Failed to create Renderer");

		Graphics::Scene scene;
		const MeshID mesh = scene.CreateMesh(Graphics::CreateCubeMesh(1.0f));
		Graphics::MaterialDesc materialDesc = {};
		materialDesc.baseColor = Math::float4(0.75f, 0.42f, 0.20f, 1.0f);
		materialDesc.roughnessFactor = 0.6f;
		const MaterialID material = scene.CreateMaterial(materialDesc);

		const uint32_t side = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(options.entityCount))));
		const float spacing = 1.4f;
		const float origin = -0.5f * static_cast<float>(side - 1u) * spacing;
		for(uint32_t index = 0; index < options.entityCount; ++index)
		{
			const uint32_t x = index % side;
			const uint32_t y = index / side;
			const Math::float3 position(
				origin + static_cast<float>(x) * spacing,
				origin + static_cast<float>(y) * spacing,
				0.0f);
			(void)scene.CreateEntity(mesh, material, Math::Translation(position));
		}

		Graphics::DirectionalLight light = {};
		light.direction = Math::float3(0.4f, 0.5f, 0.8f);
		light.intensity = 3.0f;
		light.castShadow = false;
		(void)scene.CreateDirectionalLight(light);

		const float sceneExtent = static_cast<float>(side) * spacing;
		Graphics::CameraDesc camera = {};
		camera.eye = Math::float3(0.0f, -sceneExtent * 0.9f, sceneExtent * 0.9f);
		camera.target = Math::float3(0.0f, 0.0f, 0.0f);
		camera.up = Math::float3(0.0f, 0.0f, 1.0f);
		camera.aspect = static_cast<float>(options.width) / options.height;
		camera.farPlane = sceneExtent * 4.0f + 10.0f;

		std::cout << "Rendering performance\n"
		          << "  build       : " << BuildConfiguration() << '\n'
		          << "  input       : " << options.entityCount << " entities, one shared mesh and material\n"
		          << "  output      : " << options.width << 'x' << options.height
		          << ", vsync=" << (options.vsync ? "on" : "off") << '\n'
		          << "  binding     : " << static_cast<uint32_t>(options.bindingMode) << " (0=per-draw, 1=batched, 2=bindless)" << '\n'
		          << "  warm-up     : " << options.warmUpFrameCount << " frames\n"
		          << "  repeats     : " << options.measuredFrameCount << " frames\n"
		          << "  units       : ms per submitted Render call, us per input entity\n"
		          << "  scope       : CPU wall time including submission waits; not GPU time or displayed FPS\n\n";

		RenderFrames(window, *renderer, scene, camera, options.warmUpFrameCount);
		const Statistics statistics = Summarize(MeasureRenderCalls(
			window, *renderer, scene, camera, options.measuredFrameCount));

		std::cout << std::fixed << std::setprecision(3)
		          << "Renderer::Render CPU wall time\n"
		          << "  mean        : " << statistics.meanMilliseconds << " ms\n"
		          << "  min         : " << statistics.minimumMilliseconds << " ms\n"
		          << "  p50         : " << statistics.p50Milliseconds << " ms\n"
		          << "  p95         : " << statistics.p95Milliseconds << " ms\n"
		          << "  max         : " << statistics.maximumMilliseconds << " ms\n"
		          << "  mean/entity   : " << statistics.meanMilliseconds * 1000.0 / options.entityCount << " us\n"
		          << "GPU frame time             not collected in this CPU measurement\n"
		          << "binding comparison        repeat with per-draw, batched and bindless on the same input\n";

		return 0;
	}
	catch(const std::exception& exception)
	{
		std::cerr << exception.what() << '\n';
		return 1;
	}
}
