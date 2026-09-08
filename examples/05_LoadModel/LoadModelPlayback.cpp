#include "LoadModelPlayback.h"

#include <cmath>
#include <ostream>

#include "LoadModelOptions.h"
#include "Graphics/Mesh.h"
#include "Graphics/Scene.h"

namespace
{
	const std::vector<dy::Graphics::AnimationClip>* ResolveClips(
		const dy::Graphics::Scene& scene,
		const dy::Graphics::ModelInstance& instance)
	{
		if(dy::IsValid(instance.assetId))
		{
			const dy::Graphics::ModelAsset* asset = scene.TryGetModelAsset(instance.assetId);
			return asset != nullptr ? &asset->animations : nullptr;
		}
		return &instance.clips;
	}
}

namespace dy::Examples
{
	bool ConfigureLoadModelAnimations(
		Graphics::Scene& scene,
		const std::vector<ModelInstanceID>& instances,
		const LoadModelOptions& options,
		std::ostream& output,
		std::string& outError,
		LoadModelPlaybackResult& outResult)
	{
		outError.clear();
		outResult = {};
		if(!std::isfinite(options.timeScale))
		{
			outError = "animation timescale must be finite";
			return false;
		}

		std::vector<ModelInstanceID> animatedInstances;
		animatedInstances.reserve(instances.size());
		for(const ModelInstanceID instanceId : instances)
		{
			const Graphics::ModelInstance* instance = scene.TryGetModelInstance(instanceId);
			if(instance == nullptr)
			{
				outError = "invalid model instance";
				return false;
			}
			const std::vector<Graphics::AnimationClip>* clips = ResolveClips(scene, *instance);
			if(clips == nullptr)
			{
				outError = "model instance refers to a missing asset";
				return false;
			}

			output << "Model instance " << ToIndex(instanceId) << " animation clips: " << clips->size() << '\n';
			for(size_t clipIndex = 0; clipIndex < clips->size(); ++clipIndex)
			{
				const Graphics::AnimationClip& clip = (*clips)[clipIndex];
				output << "  [" << clipIndex << "] " << clip.name << " duration=" << clip.duration << '\n';
			}
			if(clips->empty())
			{
				++outResult.staticInstances;
				continue;
			}
			if(options.clipIndex >= clips->size())
			{
				outError = "animation clip index " + std::to_string(options.clipIndex)
					+ " is out of range for model instance " + std::to_string(ToIndex(instanceId));
				return false;
			}
			animatedInstances.push_back(instanceId);
		}

		for(const ModelInstanceID instanceId : animatedInstances)
		{
			if(!scene.PlayAnimation(instanceId, options.clipIndex, options.loop)
				|| !scene.SetAnimationSpeed(instanceId, options.timeScale)
				|| !scene.SetAnimationLoop(instanceId, options.loop)
				|| !scene.SetAnimationPaused(instanceId, options.paused))
			{
				outError = "failed to configure animation playback for model instance "
					+ std::to_string(ToIndex(instanceId));
				return false;
			}
			++outResult.animatedInstances;
		}
		return true;
	}
}
