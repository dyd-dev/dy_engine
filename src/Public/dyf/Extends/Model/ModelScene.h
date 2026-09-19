#pragma once

#include <dyf/Extends/Model/Model.h>
#include "dyf/Scene.h"
#include <map>
#include <tuple>

namespace dyf
{
	// Model 소유와 애니메이션은 선택 확장이 담당하며, Renderer에는 Scene의 CPU 입력을 전달한다.
	class ModelScene final : public Scene
	{
	public:
		[[nodiscard]] ModelInstanceID CreateModelInstance(ModelInstance instance);
		[[nodiscard]] ModelAssetID CreateModelAsset(ModelAsset asset)
		{
			m_modelAssets.push_back(std::move(asset));
			return static_cast<ModelAssetID>(m_modelAssets.size() - 1u);
		}
		[[nodiscard]] ModelAssetID FindModelAsset(const std::string& cacheKey) const
		{
			for(size_t assetIndex = 0; assetIndex < m_modelAssets.size(); ++assetIndex)
				if(m_modelAssets[assetIndex].cacheKey == cacheKey) return static_cast<ModelAssetID>(assetIndex);
			return ModelAssetID::Invalid;
		}
		[[nodiscard]] bool IsValidModelAsset(ModelAssetID asset) const
		{
			return IsValid(asset) && ToIndex(asset) < m_modelAssets.size();
		}
		[[nodiscard]] const ModelAsset* TryGetModelAsset(ModelAssetID asset) const
		{
			return IsValidModelAsset(asset) ? &m_modelAssets[ToIndex(asset)] : nullptr;
		}
		[[nodiscard]] const ModelAsset& GetModelAsset(ModelAssetID asset) const
		{
			return m_modelAssets[ToIndex(asset)];
		}
		[[nodiscard]] bool BindEntityToModel(
			ModelInstanceID instance,
			EntityID entity,
			uint32_t nodeIndex,
			uint32_t skinIndex = UINT32_MAX,
			uint32_t assetMeshIndex = UINT32_MAX);
		// 사용자 시계의 경과 초를 전달한다. 고정 간격도 사용할 수 있으며 자체 시계는 읽지 않는다.
		// 실패하면 원인을 stderr로 출력하고 false를 반환한다. 다른 인스턴스의 갱신은 계속한다.
		[[nodiscard]] bool UpdateAnimations(float deltaSeconds);
		[[nodiscard]] bool PlayAnimation(ModelInstanceID instance, uint32_t clipIndex, bool loop = true);
		[[nodiscard]] bool StopAnimation(ModelInstanceID instance);
		[[nodiscard]] bool ClearMorphWeightOverride(ModelInstanceID instance, uint32_t nodeIndex, uint32_t targetIndex);
		[[nodiscard]] bool SetAnimationPaused(ModelInstanceID instance, bool paused);
		[[nodiscard]] bool SetAnimationSpeed(ModelInstanceID instance, float speed);
		[[nodiscard]] bool SetAnimationLoop(ModelInstanceID instance, bool loop);
		[[nodiscard]] bool GetAnimationPlayback(ModelInstanceID instance, AnimationPlayback& outPlayback) const;
		[[nodiscard]] bool SetMorphWeight(
			ModelInstanceID instance,
			uint32_t nodeIndex,
			uint32_t targetIndex,
			float weight);
		[[nodiscard]] bool IsValidModelInstance(ModelInstanceID instance) const;
		[[nodiscard]] const ModelInstance* TryGetModelInstance(ModelInstanceID instance) const;
		[[nodiscard]] ModelInstance* TryGetModelInstance(ModelInstanceID instance);
		[[nodiscard]] const ModelInstance& GetModelInstance(ModelInstanceID instance) const;
		[[nodiscard]] ModelInstance& GetModelInstance(ModelInstanceID instance);
		[[nodiscard]] uint32_t GetModelAssetCount() const { return static_cast<uint32_t>(m_modelAssets.size()); }

        // Model 확장의 변형 입력이다. 기본 Scene과 Renderer는 이 개념을 알지 않는다.
        const std::vector<SkinInfluence>& GetMeshSkinInfluences(MeshID mesh) const;
        const std::vector<SkinJointMatrices>& JointPaletteMatrices() const;
        uint32_t GetEntitySkinPaletteOffset(EntityID entity) const;
        const MeshData* TryGetEntityMorphedMesh(EntityID entity) const;

	private:
		// StopAnimation은 지정한 인스턴스의 실패 여부만 반환하며 공유 포즈 저장소는 함께 갱신한다.
		bool UpdateAnimations(float deltaSeconds, ModelInstanceID resultInstance);
        // 확장 로더가 기반 Scene에 입력을 등록한다. 기본 구현의 저장소를 우회 참조하지 않는다.
        friend bool LoadModelAsset(ModelScene&,const std::string&,const ModelLoadOptions&,ModelAssetID*);
        friend bool InstantiateModel(ModelScene&,ModelAssetID,const ModelSceneDesc&,ModelInstanceID*);
        // Scene의 저장소를 변경하지 않고 Model 확장 안에서 변형 입력을 갱신한다.
        std::vector<std::vector<SkinInfluence>> m_meshSkinInfluences;
        std::vector<uint32_t> m_entitySkinPaletteOffsets;
        std::vector<MeshData> m_entityMorphedMeshes;
		std::vector<SkinJointMatrices> m_jointPaletteMatrices;
		std::vector<ModelAsset> m_modelAssets;
		std::vector<ModelInstance> m_modelInstances;
		std::map<std::tuple<ModelInstanceID, uint32_t, uint32_t>, float> m_morphOverrides;
	};
}
