#include <dyf/Extends/Model/ModelScene.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>

namespace dyf
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

    const std::vector<SkinInfluence>& ModelScene::GetMeshSkinInfluences(MeshID mesh) const
    {
        static const std::vector<SkinInfluence> empty;
        return IsValid(mesh) && ToIndex(mesh) < m_meshSkinInfluences.size()
            ? m_meshSkinInfluences[ToIndex(mesh)] : empty;
    }

    const std::vector<SkinJointMatrices>& ModelScene::JointPaletteMatrices() const { return m_jointPaletteMatrices; }

    uint32_t ModelScene::GetEntitySkinPaletteOffset(EntityID entity) const
    {
        return ValidEntity(entity, m_entitySkinPaletteOffsets.size())
            ? m_entitySkinPaletteOffsets[ToIndex(entity)] : UINT32_MAX;
    }

    const MeshData* ModelScene::TryGetEntityMorphedMesh(EntityID entity) const
    {
        if(!ValidEntity(entity, m_entityMorphedMeshes.size())) return nullptr;
        const auto& mesh = m_entityMorphedMeshes[ToIndex(entity)];
        return mesh.vertices.empty() ? nullptr : &mesh;
    }

	ModelInstanceID ModelScene::CreateModelInstance(ModelInstance instance)
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
		if(!BuildGlobalNodeMatrices(nodes, instance.localPose, instance.globalPose))
			return ModelInstanceID::Invalid;

		m_modelInstances.push_back(std::move(instance));
		return static_cast<ModelInstanceID>(m_modelInstances.size() - 1u);
	}

	bool ModelScene::BindEntityToModel(
		ModelInstanceID instanceId,
		EntityID entity,
		uint32_t nodeIndex,
		uint32_t skinIndex,
		uint32_t assetMeshIndex)
	{
		if(!ValidInstance(instanceId, m_modelInstances) || !ValidEntity(entity, this->GetEntityCount())) return false;
		ModelInstance& instance = m_modelInstances[ToIndex(instanceId)];
		const ModelAsset* asset = IsValid(instance.assetId) ? TryGetModelAsset(instance.assetId) : nullptr;
		const std::vector<ModelNode>& nodes = asset != nullptr ? asset->nodes : instance.nodes;
		const std::vector<ModelSkin>& skins = asset != nullptr ? asset->skins : instance.skins;
		if(nodeIndex >= nodes.size()) return false;
		if(skinIndex != UINT32_MAX && skinIndex >= skins.size()) return false;
		if(assetMeshIndex != UINT32_MAX
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
		return true;
	}

	bool ModelScene::UpdateAnimations(float deltaSeconds)
	{
		return UpdateAnimations(deltaSeconds, ModelInstanceID::Invalid);
	}

	bool ModelScene::UpdateAnimations(float deltaSeconds, ModelInstanceID resultInstance)
	{
		bool success = true;
		const auto fail = [&](ModelInstanceID instance, EntityID entity, const char* reason)
		{
			std::fprintf(stderr, "dyf: Animation update failed: %s (instance=%u, entity=%u)\n",
				reason, ToIndex(instance), ToIndex(entity));
			if(!IsValid(instance) || !IsValid(resultInstance) || instance == resultInstance) success = false;
		};
		if(!std::isfinite(deltaSeconds))
		{
			fail(ModelInstanceID::Invalid, EntityID::Invalid, "deltaSeconds must be finite");
			return success;
		}
		m_jointPaletteMatrices.clear();
        m_entitySkinPaletteOffsets.assign(GetEntityCount(), UINT32_MAX);
        m_entityMorphedMeshes.resize(GetEntityCount());
		for(MeshData& morphedMesh : this->m_entityMorphedMeshes)
		{
			morphedMesh.vertices.clear();
			morphedMesh.indices.clear();
		}

		std::vector<SkinJointMatrices> skinPalette;
		for(size_t instanceIndex = 0; instanceIndex < m_modelInstances.size(); ++instanceIndex)
		{
			ModelInstance& instance = m_modelInstances[instanceIndex];
			const ModelInstanceID instanceId = static_cast<ModelInstanceID>(instanceIndex);
			const ModelAsset* asset = IsValid(instance.assetId) ? TryGetModelAsset(instance.assetId) : nullptr;
			const std::vector<ModelNode>& nodes = asset != nullptr ? asset->nodes : instance.nodes;
			const std::vector<ModelSkin>& skins = asset != nullptr ? asset->skins : instance.skins;
			const std::vector<AnimationClip>& clips = asset != nullptr ? asset->animations : instance.clips;
			instance.localPose.resize(nodes.size());
			instance.nodeMorphWeights.resize(nodes.size());
			for(size_t nodeIndex = 0; nodeIndex < nodes.size(); ++nodeIndex)
			{
				instance.localPose[nodeIndex] = nodes[nodeIndex].bindTransform;
				instance.nodeMorphWeights[nodeIndex] = nodes[nodeIndex].morphWeights;
			}

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
					fail(instanceId, EntityID::Invalid, "invalid animation clip");
					continue;
				}
			}

			for(auto entry = m_morphOverrides.lower_bound({instanceId, 0u, 0u});
				entry != m_morphOverrides.end() && std::get<0>(entry->first) == instanceId; ++entry)
			{
				const auto node = std::get<1>(entry->first), target = std::get<2>(entry->first);
				if(node < instance.nodeMorphWeights.size() && target < instance.nodeMorphWeights[node].size())
					instance.nodeMorphWeights[node][target] = entry->second;
			}

			if(!BuildGlobalNodeMatrices(nodes, instance.localPose, instance.globalPose))
			{
				fail(instanceId, EntityID::Invalid, "invalid node hierarchy");
				continue;
			}
			for(auto current = instance.bindings.begin(); current != instance.bindings.end(); ++current)
			{
				ModelEntityBinding& binding = *current;
				binding.paletteOffset = UINT32_MAX;
				if(!ValidEntity(binding.entity, this->GetEntityCount()) || binding.nodeIndex >= instance.globalPose.size())
				{
					fail(instanceId, binding.entity, "invalid entity binding");
					continue;
				}
				if(!GetEntity(binding.entity).SetTransform(instance.rootTransform * instance.globalPose[binding.nodeIndex]))
                {
                    fail(instanceId, binding.entity, "invalid entity binding");
                    continue;
                }
				if(asset != nullptr && binding.assetMeshIndex != UINT32_MAX)
				{
					if(binding.assetMeshIndex >= asset->meshes.size()
						|| binding.nodeIndex >= instance.nodeMorphWeights.size())
					{
						fail(instanceId, binding.entity, "invalid morph target");
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
							const MeshID meshId = this->GetEntityMesh(binding.entity);
							if(!IsValid(meshId) || ToIndex(meshId) >= Meshes().size()
								|| !EvaluateMorphTargets(
									*Meshes()[ToIndex(meshId)],
									assetMesh.morphTargets,
									weights,
									this->m_entityMorphedMeshes[ToIndex(binding.entity)]))
							{
								fail(instanceId, binding.entity, "invalid morph target");
								continue;
							}
						}
					}
				}
				if(binding.skinIndex == UINT32_MAX) continue;
				if(binding.skinIndex >= skins.size())
				{
					fail(instanceId, binding.entity, "invalid skin");
					continue;
				}

				// 이번 갱신에서 앞서 처리한 바인딩의 팔레트만 공유한다.
				const auto cachedPalette = std::find_if(
					instance.bindings.begin(), current,
					[&binding](const ModelEntityBinding& entry) {
						return entry.nodeIndex == binding.nodeIndex && entry.skinIndex == binding.skinIndex
							&& entry.paletteOffset != UINT32_MAX;
					});
				if(cachedPalette != current)
				{
					binding.paletteOffset = cachedPalette->paletteOffset;
					this->m_entitySkinPaletteOffsets[ToIndex(binding.entity)] = binding.paletteOffset;
					continue;
				}

				if(!BuildSkinPalette(
					instance.globalPose[binding.nodeIndex],
					instance.globalPose,
					skins[binding.skinIndex],
					skinPalette))
				{
					fail(instanceId, binding.entity, "invalid joint palette");
					continue;
				}
				binding.paletteOffset = static_cast<uint32_t>(m_jointPaletteMatrices.size());
				this->m_entitySkinPaletteOffsets[ToIndex(binding.entity)] = binding.paletteOffset;
				m_jointPaletteMatrices.insert(
					m_jointPaletteMatrices.end(),
					skinPalette.begin(),
					skinPalette.end());
			}
		}
		return success;
	}

	bool ModelScene::PlayAnimation(ModelInstanceID instanceId, uint32_t clipIndex, bool loop)
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

	bool ModelScene::StopAnimation(ModelInstanceID instanceId)
	{
		if(!ValidInstance(instanceId, m_modelInstances)) return false;
		auto& playback = m_modelInstances[ToIndex(instanceId)].playback;
		playback.clipIndex = UINT32_MAX;
		playback.time = 0;
		playback.playing = false;
		return UpdateAnimations(0, instanceId);
	}

	bool ModelScene::ClearMorphWeightOverride(ModelInstanceID instanceId, uint32_t nodeIndex, uint32_t targetIndex)
	{
		if(!ValidInstance(instanceId, m_modelInstances)) return false;
		const auto& weights = m_modelInstances[ToIndex(instanceId)].nodeMorphWeights;
		if(nodeIndex >= weights.size() || targetIndex >= weights[nodeIndex].size()) return false;
		m_morphOverrides.erase({instanceId, nodeIndex, targetIndex});
		return true;
	}

	bool ModelScene::SetAnimationPaused(ModelInstanceID instanceId, bool paused)
	{
		if(!ValidInstance(instanceId, m_modelInstances)) return false;
		ModelInstance& instance = m_modelInstances[ToIndex(instanceId)];
		const ModelAsset* asset = IsValid(instance.assetId) ? TryGetModelAsset(instance.assetId) : nullptr;
		const std::vector<AnimationClip>& clips = asset != nullptr ? asset->animations : instance.clips;
		if(!paused && instance.playback.clipIndex >= clips.size()) return false;
		instance.playback.playing = !paused;
		return true;
	}

	bool ModelScene::SetAnimationSpeed(ModelInstanceID instanceId, float speed)
	{
		if(!ValidInstance(instanceId, m_modelInstances) || !std::isfinite(speed)) return false;
		m_modelInstances[ToIndex(instanceId)].playback.speed = speed;
		return true;
	}

	bool ModelScene::SetAnimationLoop(ModelInstanceID instanceId, bool loop)
	{
		if(!ValidInstance(instanceId, m_modelInstances)) return false;
		m_modelInstances[ToIndex(instanceId)].playback.loop = loop;
		return true;
	}

	bool ModelScene::GetAnimationPlayback(ModelInstanceID instanceId, AnimationPlayback& outPlayback) const
	{
		if(!ValidInstance(instanceId, m_modelInstances)) return false;
		outPlayback = m_modelInstances[ToIndex(instanceId)].playback;
		return true;
	}

	bool ModelScene::SetMorphWeight(
		ModelInstanceID instanceId,
		uint32_t nodeIndex,
		uint32_t targetIndex,
		float weight)
	{
		if(!ValidInstance(instanceId, m_modelInstances) || !std::isfinite(weight)) return false;
		ModelInstance& instance = m_modelInstances[ToIndex(instanceId)];
		if(nodeIndex >= instance.nodeMorphWeights.size()
			|| targetIndex >= instance.nodeMorphWeights[nodeIndex].size()) return false;
		m_morphOverrides[{instanceId, nodeIndex, targetIndex}] = weight;
		instance.nodeMorphWeights[nodeIndex][targetIndex] = weight;
		return true;
	}

	bool ModelScene::IsValidModelInstance(ModelInstanceID instance) const
	{
		return ValidInstance(instance, m_modelInstances);
	}

	const ModelInstance* ModelScene::TryGetModelInstance(ModelInstanceID instance) const
	{
		return IsValidModelInstance(instance) ? &m_modelInstances[ToIndex(instance)] : nullptr;
	}

	ModelInstance* ModelScene::TryGetModelInstance(ModelInstanceID instance)
	{
		return IsValidModelInstance(instance) ? &m_modelInstances[ToIndex(instance)] : nullptr;
	}

	const ModelInstance& ModelScene::GetModelInstance(ModelInstanceID instance) const
	{
		const ModelInstance* modelInstance = TryGetModelInstance(instance);
		assert(modelInstance != nullptr && "ModelScene::GetModelInstance requires a valid ID");
		return *modelInstance;
	}

	ModelInstance& ModelScene::GetModelInstance(ModelInstanceID instance)
	{
		ModelInstance* modelInstance = TryGetModelInstance(instance);
		assert(modelInstance != nullptr && "ModelScene::GetModelInstance requires a valid ID");
		return *modelInstance;
	}

}
