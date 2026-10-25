#include <dyf/Extends/Model/ModelScene.h>
#include "../../Platform/Profiler/CollectorInternal.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>
#include "AnimationSampling.h"

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

    bool ModelScene::HasEntityMorphTargets(EntityID entity) const
    {
        return ValidEntity(entity, m_entityHasMorphTargets.size()) && m_entityHasMorphTargets[ToIndex(entity)];
    }

	ModelInstanceID ModelScene::CreateModelInstance(ModelInstance instance)
	{
		const ModelAsset* asset = TryGetModelAsset(instance.assetId);
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
		const ModelAsset* asset = TryGetModelAsset(instance.assetId);
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
        m_entityHasMorphTargets.resize(GetEntityCount(), false);
        m_entityHasMorphTargets[ToIndex(entity)] = asset != nullptr && assetMeshIndex != UINT32_MAX &&
            !asset->meshes[assetMeshIndex].morphTargets.empty();
		return true;
	}

	bool ModelScene::UpdateAnimations(float deltaSeconds)
	{
		return UpdateAnimations(deltaSeconds, ModelInstanceID::Invalid);
	}

	bool ModelScene::UpdateAnimations(float deltaSeconds, ModelInstanceID resultInstance)
	{
#if defined(DY_LIVE_PROFILING_ENABLED) || defined(DY_PROFILER_TRACING_ENABLED)
        Platform::CpuScope timing(Platform::LiveStage::Data,"Model animation arrays");
#endif
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
        // Bindings may arrive through CreateModelInstance or mutable instance edits.
        m_entityHasMorphTargets.assign(GetEntityCount(), false);
        m_entityMorphedMeshes.resize(GetEntityCount());
		for(MeshData& morphedMesh : this->m_entityMorphedMeshes)
		{
			morphedMesh.vertices.clear();
			morphedMesh.indices.clear();
		}

		std::vector<SkinJointMatrices> skinPalette;
		std::unordered_map<uint64_t, uint32_t> paletteOffsets;
		// Keys cannot change while this synchronous update samples the instances.
		// Keep validation local to this call so edits before the next update are seen.
		std::unordered_set<const AnimationClip*> sampledClips;
		Detail::NodeHierarchyScratch hierarchyScratch;
		std::unordered_map<const ModelAsset*,std::vector<uint32_t>> nodeOrders;
		std::vector<uint8_t> assetUses;
		if(m_modelInstances.size()>1 && !m_modelAssets.empty())
		{
			assetUses.assign(m_modelAssets.size(),0u);
			for(const auto& instance:m_modelInstances)
				if(IsValidModelAsset(instance.assetId))
				{
					auto& uses=assetUses[ToIndex(instance.assetId)];
					if(uses<2u)++uses;
				}
		}
		for(size_t instanceIndex = 0; instanceIndex < m_modelInstances.size(); ++instanceIndex)
		{
			paletteOffsets.clear();
			ModelInstance& instance = m_modelInstances[instanceIndex];
			const ModelInstanceID instanceId = static_cast<ModelInstanceID>(instanceIndex);
			const ModelAsset* asset = TryGetModelAsset(instance.assetId);
			const bool sharedAsset=asset && !assetUses.empty() && assetUses[ToIndex(instance.assetId)]>1u;
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
				const float duration = std::max(clip.duration, 0.0f);
				if(instance.playback.playing)
				{
					const float advance = deltaSeconds * instance.playback.speed;
					const float nextTime = instance.playback.time + advance;
					if(!std::isfinite(advance) || !std::isfinite(nextTime))
					{
						fail(instanceId, EntityID::Invalid, "animation time exceeds the finite range");
						continue;
					}
					instance.playback.time = nextTime;
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
				const bool knownKeys=sharedAsset && sampledClips.count(&clip)!=0;
				const bool sampled=knownKeys
					? Detail::SampleAnimationClipValidatedKeys(clip,instance.playback.time,instance.localPose,instance.nodeMorphWeights)
					: SampleAnimationClip(clip,instance.playback.time,instance.localPose,instance.nodeMorphWeights);
				if(!sampled)
				{
					fail(instanceId, EntityID::Invalid, "invalid animation clip");
					continue;
				}
				if(sharedAsset && !knownKeys)sampledClips.insert(&clip);
			}

			for(auto entry = m_morphOverrides.lower_bound({instanceId, 0u, 0u});
				entry != m_morphOverrides.end() && std::get<0>(entry->first) == instanceId; ++entry)
			{
				const auto node = std::get<1>(entry->first), target = std::get<2>(entry->first);
				if(node < instance.nodeMorphWeights.size() && target < instance.nodeMorphWeights[node].size())
					instance.nodeMorphWeights[node][target] = entry->second;
			}

			bool validHierarchy=false;
			const auto knownOrder=sharedAsset ? nodeOrders.find(asset) : nodeOrders.end();
			if(knownOrder!=nodeOrders.end())
				validHierarchy=Detail::BuildGlobalNodeMatricesValidatedOrder(
					nodes,instance.localPose,instance.globalPose,knownOrder->second);
			else
			{
				validHierarchy=Detail::BuildGlobalNodeMatricesWithScratch(
					nodes,instance.localPose,instance.globalPose,hierarchyScratch,sharedAsset);
				if(validHierarchy && sharedAsset)nodeOrders.emplace(asset,std::move(hierarchyScratch.order));
			}
			if(!validHierarchy)
			{
				fail(instanceId, EntityID::Invalid, "invalid node hierarchy");
				continue;
			}
			for(auto current = instance.bindings.begin(); current != instance.bindings.end(); ++current)
			{
				ModelEntityBinding& binding = *current;
				binding.paletteOffset = UINT32_MAX;
				if(!ValidEntity(binding.entity, this->GetEntityCount()) || binding.nodeIndex >= instance.globalPose.size()
					|| !GetEntity(binding.entity).SetTransform(instance.rootTransform * instance.globalPose[binding.nodeIndex]))
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
                    m_entityHasMorphTargets[ToIndex(binding.entity)] = !assetMesh.morphTargets.empty();
					if(!assetMesh.morphTargets.empty())
					{
						const std::vector<float>& weights = instance.nodeMorphWeights[binding.nodeIndex];
						const bool activeMorph = std::any_of(weights.begin(), weights.end(), [](float weight) {
							return weight != 0.0f;
						});
						if(activeMorph)
						{
							const MeshID meshId = this->GetEntityMesh(binding.entity);
							auto& output=this->m_entityMorphedMeshes[ToIndex(binding.entity)];
							bool evaluated=false;
							try
							{
								evaluated=IsValid(meshId) && ToIndex(meshId)<Meshes().size()
									&& EvaluateMorphTargets(
									*Meshes()[ToIndex(meshId)],
									assetMesh.morphTargets,
									weights,
									output);
							}
							catch(...) { output.vertices.clear(); output.indices.clear(); throw; }
							if(!evaluated)
							{
								output.vertices.clear(); output.indices.clear();
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

				// Only successful palettes from this instance/update are reusable.
				// Exact node/skin keys avoid scanning the binding prefix for every mesh.
				const uint64_t paletteKey = (uint64_t(binding.nodeIndex) << 32) | binding.skinIndex;
				const auto cachedPalette = paletteOffsets.find(paletteKey);
				if(cachedPalette != paletteOffsets.end())
				{
					binding.paletteOffset = cachedPalette->second;
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
				if(m_jointPaletteMatrices.size() >= UINT32_MAX
					|| skinPalette.size() > UINT32_MAX - m_jointPaletteMatrices.size())
				{
					fail(instanceId, binding.entity, "joint palette exceeds the supported offset range");
					continue;
				}
				const uint32_t paletteOffset = static_cast<uint32_t>(m_jointPaletteMatrices.size());
				m_jointPaletteMatrices.insert(
					m_jointPaletteMatrices.end(),
					skinPalette.begin(),
					skinPalette.end());
				if(instance.bindings.size() > 1u) paletteOffsets.emplace(paletteKey, paletteOffset);
				binding.paletteOffset = paletteOffset;
				this->m_entitySkinPaletteOffsets[ToIndex(binding.entity)] = paletteOffset;
			}
		}
		return success;
	}

	bool ModelScene::PlayAnimation(ModelInstanceID instanceId, uint32_t clipIndex, bool loop)
	{
		if(!ValidInstance(instanceId, m_modelInstances)) return false;
		ModelInstance& instance = m_modelInstances[ToIndex(instanceId)];
		const ModelAsset* asset = TryGetModelAsset(instance.assetId);
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
		const ModelAsset* asset = TryGetModelAsset(instance.assetId);
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
