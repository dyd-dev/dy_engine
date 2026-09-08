#include "Graphics/Scene.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace dy::Graphics
{
	namespace
	{
		[[nodiscard]] bool ValidInstance(ModelInstanceID instance, const std::vector<ModelInstance>& instances)
		{
			return IsValid(instance) && ToIndex(instance) < instances.size();
		}

		[[nodiscard]] bool ValidEntity(EntityID entity, size_t entityCount)
		{
			return IsValid(entity) && ToIndex(entity) < entityCount;
		}
	}

	ModelInstanceID Scene::CreateModelInstance(ModelInstance instance)
	{
		const ModelAsset* asset = IsValid(instance.assetId) ? TryGetModelAsset(instance.assetId) : nullptr;
		if(IsValid(instance.assetId) && asset == nullptr) return ModelInstanceID::Invalid;
		const std::vector<ModelNode>& nodes = asset != nullptr ? asset->nodes : instance.nodes;
		instance.localPose.resize(nodes.size());
		instance.nodeMorphWeights.resize(nodes.size());
		for(size_t nodeIndex = 0; nodeIndex < nodes.size(); ++nodeIndex)
		{
			instance.localPose[nodeIndex] = nodes[nodeIndex].bindTransform;
			instance.nodeMorphWeights[nodeIndex] = nodes[nodeIndex].morphWeights;
		}
		instance.hierarchyScratch.visitState.reserve(nodes.size());
		instance.hierarchyScratch.nodeStack.reserve(nodes.size());
		if(!BuildGlobalNodeMatrices(nodes, instance.localPose, instance.globalPose, instance.hierarchyScratch))
			return ModelInstanceID::Invalid;

		m_modelInstances.push_back(std::move(instance));
		return static_cast<ModelInstanceID>(m_modelInstances.size() - 1u);
	}

	bool Scene::BindEntityToModel(
		ModelInstanceID instanceId,
		EntityID entity,
		uint32_t nodeIndex,
		uint32_t skinIndex,
		uint32_t assetMeshIndex)
	{
		if(!ValidInstance(instanceId, m_modelInstances) || !ValidEntity(entity, m_entityMeshes.size())) return false;
		ModelInstance& instance = m_modelInstances[ToIndex(instanceId)];
		const ModelAsset* asset = IsValid(instance.assetId) ? TryGetModelAsset(instance.assetId) : nullptr;
		const std::vector<ModelNode>& nodes = asset != nullptr ? asset->nodes : instance.nodes;
		const std::vector<ModelSkin>& skins = asset != nullptr ? asset->skins : instance.skins;
		if(nodeIndex >= nodes.size()) return false;
		if(skinIndex != kInvalidAnimationIndex && skinIndex >= skins.size()) return false;
		if(assetMeshIndex != kInvalidAnimationIndex
			&& (asset == nullptr || assetMeshIndex >= asset->meshes.size())) return false;

		for(const ModelInstance& existingInstance : m_modelInstances)
			for(const ModelEntityBinding& binding : existingInstance.bindings)
				if(binding.entity == entity) return false;

		ModelEntityBinding binding;
		binding.entity = entity;
		binding.nodeIndex = nodeIndex;
		binding.skinIndex = skinIndex;
		binding.assetMeshIndex = assetMeshIndex;
		instance.bindings.push_back(binding);
		instance.paletteCache.reserve(instance.bindings.size());
		return true;
	}

	AnimationUpdateReport Scene::UpdateAnimations(float deltaSeconds)
	{
		AnimationUpdateReport report;
		if(!std::isfinite(deltaSeconds))
		{
			report.failures.push_back(AnimationUpdateFailure{
				ModelInstanceID::Invalid,
				EntityID::Invalid,
				AnimationUpdateFailureReason::InvalidDeltaTime });
			return report;
		}
		m_jointPaletteMatrices.clear();
		std::fill(m_entitySkinPaletteOffsets.begin(), m_entitySkinPaletteOffsets.end(), kInvalidAnimationIndex);
		for(MeshData& morphedMesh : m_entityMorphedMeshes)
		{
			morphedMesh.vertices.clear();
			morphedMesh.indices.clear();
			morphedMesh.skinInfluences.clear();
		}

		for(size_t instanceIndex = 0; instanceIndex < m_modelInstances.size(); ++instanceIndex)
		{
			ModelInstance& instance = m_modelInstances[instanceIndex];
			const ModelInstanceID instanceId = static_cast<ModelInstanceID>(instanceIndex);
			const ModelAsset* asset = IsValid(instance.assetId) ? TryGetModelAsset(instance.assetId) : nullptr;
			const std::vector<ModelNode>& nodes = asset != nullptr ? asset->nodes : instance.nodes;
			const std::vector<ModelSkin>& skins = asset != nullptr ? asset->skins : instance.skins;
			const std::vector<AnimationClip>& clips = asset != nullptr ? asset->animations : instance.clips;
			instance.localPose.resize(nodes.size());
			for(size_t nodeIndex = 0; nodeIndex < nodes.size(); ++nodeIndex)
				instance.localPose[nodeIndex] = nodes[nodeIndex].bindTransform;
			instance.paletteCache.clear();

			const bool validClip = instance.playback.clipIndex < clips.size();
			if(validClip)
			{
				const AnimationClip& clip = clips[instance.playback.clipIndex];
				for(const MorphWeightTrack& track : clip.morphTracks)
				{
					if(track.nodeIndex >= nodes.size()
						|| track.nodeIndex >= instance.nodeMorphWeights.size()
						|| track.targetIndex >= instance.nodeMorphWeights[track.nodeIndex].size()) continue;
					const std::vector<float>& bindWeights = nodes[track.nodeIndex].morphWeights;
					instance.nodeMorphWeights[track.nodeIndex][track.targetIndex] =
						track.targetIndex < bindWeights.size() ? bindWeights[track.targetIndex] : 0.0f;
				}
				const float duration = std::max(clip.duration, 0.0f);
				if(instance.playback.playing)
				{
					const float advance = deltaSeconds * instance.playback.speed;
					instance.playback.time += advance;
					if(instance.playback.loop && duration > 0.0f)
					{
						instance.playback.time = std::fmod(instance.playback.time, duration);
						if(instance.playback.time < 0.0f) instance.playback.time += duration;
					}
					else
					{
						const float clamped = std::clamp(instance.playback.time, 0.0f, duration);
						if(duration <= 0.0f
							|| (advance > 0.0f && instance.playback.time >= duration)
							|| (advance < 0.0f && instance.playback.time <= 0.0f))
						{
							instance.playback.playing = false;
						}
						instance.playback.time = clamped;
					}
				}
				if(!SampleAnimationClip(
					clip,
					instance.playback.time,
					instance.localPose,
					instance.nodeMorphWeights))
				{
					report.failures.push_back(AnimationUpdateFailure{
						instanceId,
						EntityID::Invalid,
						AnimationUpdateFailureReason::InvalidClip });
					continue;
				}
			}

			if(!BuildGlobalNodeMatrices(nodes, instance.localPose, instance.globalPose, instance.hierarchyScratch))
			{
				report.failures.push_back(AnimationUpdateFailure{
					instanceId,
					EntityID::Invalid,
					AnimationUpdateFailureReason::InvalidHierarchy });
				continue;
			}
			++report.updatedInstances;
			for(ModelEntityBinding& binding : instance.bindings)
			{
				binding.paletteOffset = kInvalidAnimationIndex;
				if(!ValidEntity(binding.entity, m_entityTransforms.size()) || binding.nodeIndex >= instance.globalPose.size())
				{
					report.failures.push_back(AnimationUpdateFailure{
						instanceId,
						binding.entity,
						AnimationUpdateFailureReason::InvalidBinding });
					continue;
				}
				m_entityTransforms[ToIndex(binding.entity)].worldMatrix = instance.rootTransform * instance.globalPose[binding.nodeIndex];
				if(asset != nullptr && binding.assetMeshIndex != kInvalidAnimationIndex)
				{
					if(binding.assetMeshIndex >= asset->meshes.size()
						|| binding.nodeIndex >= instance.nodeMorphWeights.size())
					{
						report.failures.push_back(AnimationUpdateFailure{
							instanceId,
							binding.entity,
							AnimationUpdateFailureReason::InvalidMorph });
						continue;
					}
					const ModelAssetMesh& assetMesh = asset->meshes[binding.assetMeshIndex];
					if(!assetMesh.morphTargets.empty())
					{
						const std::vector<float>& weights = instance.nodeMorphWeights[binding.nodeIndex];
						const bool activeMorph = std::any_of(weights.begin(), weights.end(), [](float weight) {
							return weight != 0.0f;
						});
						if(activeMorph)
						{
							const MeshID meshId = m_entityMeshes[ToIndex(binding.entity)];
							if(!IsValid(meshId) || ToIndex(meshId) >= m_meshes.size()
								|| !EvaluateMorphTargets(
									m_meshes[ToIndex(meshId)],
									assetMesh.morphTargets,
									weights,
									m_entityMorphedMeshes[ToIndex(binding.entity)]))
							{
								report.failures.push_back(AnimationUpdateFailure{
									instanceId,
									binding.entity,
									AnimationUpdateFailureReason::InvalidMorph });
								continue;
							}
						}
					}
				}
				if(binding.skinIndex == kInvalidAnimationIndex) continue;
				if(binding.skinIndex >= skins.size())
				{
					report.failures.push_back(AnimationUpdateFailure{
						instanceId,
						binding.entity,
						AnimationUpdateFailureReason::InvalidSkin });
					continue;
				}

				const auto cachedPalette = std::find_if(
					instance.paletteCache.begin(),
					instance.paletteCache.end(),
					[&binding](const ModelPaletteCacheEntry& entry) {
						return entry.nodeIndex == binding.nodeIndex && entry.skinIndex == binding.skinIndex;
					});
				if(cachedPalette != instance.paletteCache.end())
				{
					binding.paletteOffset = cachedPalette->paletteOffset;
					m_entitySkinPaletteOffsets[ToIndex(binding.entity)] = binding.paletteOffset;
					continue;
				}

				if(!BuildSkinPalette(
					instance.globalPose[binding.nodeIndex],
					instance.globalPose,
					skins[binding.skinIndex],
					instance.skinPaletteScratch))
				{
					report.failures.push_back(AnimationUpdateFailure{
						instanceId,
						binding.entity,
						AnimationUpdateFailureReason::InvalidSkinPalette });
					continue;
				}
				binding.paletteOffset = static_cast<uint32_t>(m_jointPaletteMatrices.size());
				m_entitySkinPaletteOffsets[ToIndex(binding.entity)] = binding.paletteOffset;
				m_jointPaletteMatrices.insert(
					m_jointPaletteMatrices.end(),
					instance.skinPaletteScratch.begin(),
					instance.skinPaletteScratch.end());
				instance.paletteCache.push_back(ModelPaletteCacheEntry{
					binding.nodeIndex,
					binding.skinIndex,
					binding.paletteOffset });
			}
		}
		return report;
	}

	bool Scene::PlayAnimation(ModelInstanceID instanceId, uint32_t clipIndex, bool loop)
	{
		if(!ValidInstance(instanceId, m_modelInstances)) return false;
		ModelInstance& instance = m_modelInstances[ToIndex(instanceId)];
		const ModelAsset* asset = IsValid(instance.assetId) ? TryGetModelAsset(instance.assetId) : nullptr;
		const std::vector<AnimationClip>& clips = asset != nullptr ? asset->animations : instance.clips;
		if(clipIndex >= clips.size()) return false;
		instance.playback.clipIndex = clipIndex;
		instance.playback.time = 0.0f;
		instance.playback.playing = true;
		instance.playback.loop = loop;
		return true;
	}

	bool Scene::SetAnimationPaused(ModelInstanceID instanceId, bool paused)
	{
		if(!ValidInstance(instanceId, m_modelInstances)) return false;
		ModelInstance& instance = m_modelInstances[ToIndex(instanceId)];
		const ModelAsset* asset = IsValid(instance.assetId) ? TryGetModelAsset(instance.assetId) : nullptr;
		const std::vector<AnimationClip>& clips = asset != nullptr ? asset->animations : instance.clips;
		if(!paused && instance.playback.clipIndex >= clips.size()) return false;
		instance.playback.playing = !paused;
		return true;
	}

	bool Scene::SetAnimationSpeed(ModelInstanceID instanceId, float speed)
	{
		if(!ValidInstance(instanceId, m_modelInstances) || !std::isfinite(speed)) return false;
		m_modelInstances[ToIndex(instanceId)].playback.speed = speed;
		return true;
	}

	bool Scene::SetAnimationLoop(ModelInstanceID instanceId, bool loop)
	{
		if(!ValidInstance(instanceId, m_modelInstances)) return false;
		m_modelInstances[ToIndex(instanceId)].playback.loop = loop;
		return true;
	}

	bool Scene::GetAnimationPlayback(ModelInstanceID instanceId, AnimationPlayback& outPlayback) const
	{
		if(!ValidInstance(instanceId, m_modelInstances)) return false;
		outPlayback = m_modelInstances[ToIndex(instanceId)].playback;
		return true;
	}

	bool Scene::SetMorphWeight(
		ModelInstanceID instanceId,
		uint32_t nodeIndex,
		uint32_t targetIndex,
		float weight)
	{
		if(!ValidInstance(instanceId, m_modelInstances) || !std::isfinite(weight)) return false;
		ModelInstance& instance = m_modelInstances[ToIndex(instanceId)];
		if(nodeIndex >= instance.nodeMorphWeights.size()
			|| targetIndex >= instance.nodeMorphWeights[nodeIndex].size()) return false;
		instance.nodeMorphWeights[nodeIndex][targetIndex] = weight;
		return true;
	}

	bool Scene::IsValidModelInstance(ModelInstanceID instance) const
	{
		return ValidInstance(instance, m_modelInstances);
	}

	const ModelInstance* Scene::TryGetModelInstance(ModelInstanceID instance) const
	{
		return IsValidModelInstance(instance) ? &m_modelInstances[ToIndex(instance)] : nullptr;
	}

	ModelInstance* Scene::TryGetModelInstance(ModelInstanceID instance)
	{
		return IsValidModelInstance(instance) ? &m_modelInstances[ToIndex(instance)] : nullptr;
	}

	const ModelInstance& Scene::GetModelInstance(ModelInstanceID instance) const
	{
		const ModelInstance* modelInstance = TryGetModelInstance(instance);
		assert(modelInstance != nullptr && "Scene::GetModelInstance requires a valid ID");
		return *modelInstance;
	}

	ModelInstance& Scene::GetModelInstance(ModelInstanceID instance)
	{
		ModelInstance* modelInstance = TryGetModelInstance(instance);
		assert(modelInstance != nullptr && "Scene::GetModelInstance requires a valid ID");
		return *modelInstance;
	}

	bool Scene::IsEntitySkinned(EntityID entity) const
	{
		return ValidEntity(entity, m_entitySkinPaletteOffsets.size())
			&& m_entitySkinPaletteOffsets[ToIndex(entity)] != kInvalidAnimationIndex;
	}

	uint32_t Scene::GetEntitySkinPaletteOffset(EntityID entity) const
	{
		return ValidEntity(entity, m_entitySkinPaletteOffsets.size())
			? m_entitySkinPaletteOffsets[ToIndex(entity)]
			: kInvalidAnimationIndex;
	}

	const MeshData* Scene::TryGetEntityMorphedMesh(EntityID entity) const
	{
		if(!ValidEntity(entity, m_entityMorphedMeshes.size())) return nullptr;
		const MeshData& mesh = m_entityMorphedMeshes[ToIndex(entity)];
		return mesh.vertices.empty() ? nullptr : &mesh;
	}
}
