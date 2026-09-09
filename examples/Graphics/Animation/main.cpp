// 목표 Graphics API의 사용 예제. Renderer::Create / Render(scene, camera)는 아직 미구현이다.
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

#include "Platform/Window.h"
#include "Graphics/Animation.h"
#include "Graphics/Mesh.h"
#include "Graphics/Renderer.h"
#include "Graphics/Scene.h"

using namespace dy;

namespace
{
	struct Options
	{
		std::string path = "examples/Common/Models/Fox.glb";
		uint32_t clip = 0;
		float speed = 1.0f;
		bool paused = false;
		bool loop = true;
		bool computePreSkin = false;
		bool manualMorph = false;
		uint32_t morphNode = 0;
		uint32_t morphTarget = 0;
		float morphWeight = 0.0f;
	};

	uint32_t ParseIndex(const std::string& text)
	{
		uint32_t value = 0;
		const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
		if(result.ec != std::errc{} || result.ptr != text.data() + text.size())
			throw std::invalid_argument("Expected an unsigned index: " + text);
		return value;
	}

	float ParseFloat(const std::string& text)
	{
		size_t consumed = 0;
		const float value = std::stof(text, &consumed);
		if(consumed != text.size() || !std::isfinite(value))
			throw std::invalid_argument("Expected a finite number: " + text);
		return value;
	}

	Options ParseOptions(int argc, char** argv)
	{
		Options options;
		for(int i = 1; i < argc; ++i)
		{
			const std::string argument = argv[i];
			if(argument.rfind("--model=", 0) == 0) options.path = argument.substr(8);
			else if(argument.rfind("--clip=", 0) == 0) options.clip = ParseIndex(argument.substr(7));
			else if(argument.rfind("--speed=", 0) == 0) options.speed = ParseFloat(argument.substr(8));
			else if(argument == "--paused") options.paused = true;
			else if(argument == "--once") options.loop = false;
			else if(argument == "--compute-preskin") options.computePreSkin = true;
			else if(argument.rfind("--morph=", 0) == 0)
			{
				const std::string value = argument.substr(8);
				const size_t first = value.find(',');
				const size_t second = first == std::string::npos ? first : value.find(',', first + 1);
				if(first == std::string::npos || second == std::string::npos)
					throw std::invalid_argument("Expected --morph=node,target,weight");
				options.manualMorph = true;
				options.morphNode = ParseIndex(value.substr(0, first));
				options.morphTarget = ParseIndex(value.substr(first + 1, second - first - 1));
				options.morphWeight = ParseFloat(value.substr(second + 1));
			}
			else throw std::invalid_argument("Unknown option: " + argument);
		}
		if(options.path.empty()) throw std::invalid_argument("Model path must not be empty");
		return options;
	}
}

int main(int argc, char** argv)
{
	try
	{
		const Options options = ParseOptions(argc, argv);
		Platform::Window window(1280, 720, "Graphics - Animation");
		Graphics::RendererDesc rendererDesc = {};
		// 기존 compute preskin도 보존한다. 미지원 요청을 다른 경로로 조용히 바꾸지 않는다.
		rendererDesc.skinningExecutionMode = options.computePreSkin
			? Graphics::SkinningExecutionMode::ComputePreSkin
			: Graphics::SkinningExecutionMode::VertexShader;
		auto renderer = Graphics::Renderer::Create(window.GetHandle(), rendererDesc);
		if(!renderer) throw std::runtime_error("Failed to create renderer with requested skinning mode");

		Graphics::Scene scene;
		Graphics::ModelSceneDesc model = {};
		model.path = options.path;
		model.normalizedSize = 1.8f;
		ModelInstanceID character = ModelInstanceID::Invalid;
		if(!Graphics::AddModelToScene(scene, model, &character) || !IsValid(character))
			throw std::runtime_error("Failed to load animated model: " + options.path);

		const auto& asset = scene.GetModelAsset(scene.GetModelInstance(character).assetId);
		const auto& clips = asset.animations;
		for(size_t index = 0; index < clips.size(); ++index)
			std::cout << "clip " << index << ": " << clips[index].name << '\n';

		// 수동 morph는 bind pose에서 확인한다. 재생 중인 morph track과 덮어쓰기가 충돌하지 않는다.
		// 기본 Fox에는 morph target이 없으므로 해당 기능은 target을 가진 모델을 전달해야 한다.
		if(options.manualMorph)
		{
			// 로더가 자동 선택한 clip을 해제하여 morph track의 재평가를 막는다.
			auto& instance = scene.GetModelInstance(character);
			instance.playback = {};
			for(size_t node = 0; node < asset.nodes.size(); ++node)
				instance.nodeMorphWeights[node] = asset.nodes[node].morphWeights;
			if(!scene.SetMorphWeight(character, options.morphNode, options.morphTarget, options.morphWeight))
				throw std::runtime_error("Invalid morph node, target or weight");
		}
		else
		{
			if(options.clip >= clips.size())
				throw std::runtime_error("Requested animation clip does not exist");
			if(!scene.PlayAnimation(character, options.clip, options.loop)
				|| !scene.SetAnimationSpeed(character, options.speed)
				|| !scene.SetAnimationLoop(character, options.loop)
				|| !scene.SetAnimationPaused(character, options.paused))
				throw std::runtime_error("Failed to configure animation playback");
		}

		Graphics::DirectionalLight light = {};
		light.castShadow = false;
		(void)scene.CreateDirectionalLight(light);

		Graphics::CameraDesc camera = {};
		camera.eye = Math::float3(3.0f, 3.0f, 2.0f);
		camera.target = Math::float3(0.0f, 0.0f, 0.8f);
		camera.aspect = 1280.0f / 720.0f;

		auto previousFrame = std::chrono::steady_clock::now();
		while(window.IsRunning())
		{
			window.PollEvents();
			if(!window.IsRunning()) break;
			const auto now = std::chrono::steady_clock::now();
			const float deltaSeconds = std::chrono::duration<float>(now - previousFrame).count();
			previousFrame = now;
			// pose, morph mesh, skin palette를 Render 전에 갱신한다.
			if(!scene.UpdateAnimations(deltaSeconds).Succeeded())
				throw std::runtime_error("Animation pose, morph or skin update failed");
			if(!renderer->Render(scene, camera))
				throw std::runtime_error("Frame submission failed");
		}
	}
	catch(const std::exception& exception)
	{
		std::cerr << exception.what() << '\n';
		return 1;
	}
	return 0;
}
