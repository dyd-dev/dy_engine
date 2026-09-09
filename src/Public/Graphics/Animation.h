#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Core/Types.h"
#include "Math/Math.h"

namespace dy::Graphics
{
	inline constexpr uint32_t kInvalidAnimationIndex = 0xFFFFFFFFu;

	enum class AnimationInterpolation : uint8_t
	{
		Step,
		Linear,
		CubicSpline
	};

	struct NodeTransform
	{
		Math::float3 translation = Math::float3(0.0f, 0.0f, 0.0f);
		Math::quat rotation = Math::quat::Identity();
		Math::float3 scale = Math::float3(1.0f, 1.0f, 1.0f);
	};

	struct ModelNode
	{
		std::string name;
		int32_t parentIndex = -1;
		NodeTransform bindTransform = {};
		std::vector<float> morphWeights;
	};

	struct Vec3Key
	{
		float time = 0.0f;
		Math::float3 inTangent = Math::float3(0.0f, 0.0f, 0.0f);
		Math::float3 value = Math::float3(0.0f, 0.0f, 0.0f);
		Math::float3 outTangent = Math::float3(0.0f, 0.0f, 0.0f);
	};

	struct FloatKey
	{
		float time = 0.0f;
		float inTangent = 0.0f;
		float value = 0.0f;
		float outTangent = 0.0f;
	};

	struct QuatKey
	{
		float time = 0.0f;
		Math::quat inTangent = Math::quat(0.0f, 0.0f, 0.0f, 0.0f);
		Math::quat value = Math::quat::Identity();
		Math::quat outTangent = Math::quat(0.0f, 0.0f, 0.0f, 0.0f);
	};

	struct NodeAnimationTrack
	{
		uint32_t nodeIndex = kInvalidAnimationIndex;
		AnimationInterpolation translationInterpolation = AnimationInterpolation::Linear;
		AnimationInterpolation rotationInterpolation = AnimationInterpolation::Linear;
		AnimationInterpolation scaleInterpolation = AnimationInterpolation::Linear;
		std::vector<Vec3Key> translations;
		std::vector<QuatKey> rotations;
		std::vector<Vec3Key> scales;
	};

	struct MorphWeightTrack
	{
		uint32_t nodeIndex = kInvalidAnimationIndex;
		uint32_t targetIndex = kInvalidAnimationIndex;
		AnimationInterpolation interpolation = AnimationInterpolation::Linear;
		std::vector<FloatKey> weights;
	};

	struct AnimationClip
	{
		std::string name;
		float duration = 0.0f;
		std::vector<NodeAnimationTrack> tracks;
		std::vector<MorphWeightTrack> morphTracks;
	};

	enum class SkinningMethod : uint8_t
	{
		Linear,
		Rigid,
		DualQuaternion,
		BlendedDualQuaternion
	};

	struct ModelSkin
	{
		std::string name;
		SkinningMethod method = SkinningMethod::Linear;
		std::vector<uint32_t> jointNodeIndices;
		std::vector<Math::float4x4> inverseBindMatrices;
	};

	struct alignas(16) SkinJointMatrices
	{
		Math::float4x4 positionMatrix = Math::float4x4::Identity();
		Math::float4x4 normalMatrix = Math::float4x4::Identity();
		Math::DualQuaternionTRS dualQuaternion = {};
	};
	static_assert(sizeof(SkinJointMatrices) == 176u, "GLSL std430 skin joint entry must be 176 bytes");

	struct AnimationPlayback
	{
		uint32_t clipIndex = kInvalidAnimationIndex;
		float time = 0.0f;
		float speed = 1.0f;
		bool playing = false;
		bool loop = true;
	};

	struct ModelEntityBinding
	{
		EntityID entity = EntityID::Invalid;
		uint32_t nodeIndex = kInvalidAnimationIndex;
		uint32_t skinIndex = kInvalidAnimationIndex;
		uint32_t assetMeshIndex = kInvalidAnimationIndex;
		uint32_t paletteOffset = kInvalidAnimationIndex;
	};

	struct HierarchyEvaluationScratch
	{
		std::vector<uint8_t> visitState;
		std::vector<uint32_t> nodeStack;
	};

	struct ModelPaletteCacheEntry
	{
		uint32_t nodeIndex = kInvalidAnimationIndex;
		uint32_t skinIndex = kInvalidAnimationIndex;
		uint32_t paletteOffset = kInvalidAnimationIndex;
	};

	struct ModelInstance
	{
		ModelAssetID assetId = ModelAssetID::Invalid;
		Math::float4x4 rootTransform = Math::float4x4::Identity();
		std::vector<ModelNode> nodes;
		std::vector<ModelSkin> skins;
		std::vector<AnimationClip> clips;
		std::vector<NodeTransform> localPose;
		std::vector<std::vector<float>> nodeMorphWeights;
		std::vector<Math::float4x4> globalPose;
		std::vector<ModelEntityBinding> bindings;
		HierarchyEvaluationScratch hierarchyScratch;
		std::vector<SkinJointMatrices> skinPaletteScratch;
		std::vector<ModelPaletteCacheEntry> paletteCache;
		AnimationPlayback playback;
	};

	enum class AnimationUpdateFailureReason : uint8_t
	{
		InvalidDeltaTime,
		InvalidClip,
		InvalidHierarchy,
		InvalidBinding,
		InvalidSkin,
		InvalidSkinPalette,
		InvalidMorph
	};

	struct AnimationUpdateFailure
	{
		ModelInstanceID instance = ModelInstanceID::Invalid;
		EntityID entity = EntityID::Invalid;
		AnimationUpdateFailureReason reason = AnimationUpdateFailureReason::InvalidHierarchy;
	};

	struct AnimationUpdateReport
	{
		uint32_t updatedInstances = 0u;
		std::vector<AnimationUpdateFailure> failures;

		[[nodiscard]] bool Succeeded() const { return failures.empty(); }
	};

	[[nodiscard]] Math::float4x4 ComposeTransform(const NodeTransform& transform);
	[[nodiscard]] bool SampleAnimationClip(const AnimationClip& clip, float time, std::vector<NodeTransform>& pose);
	[[nodiscard]] bool SampleAnimationClip(
		const AnimationClip& clip,
		float time,
		std::vector<NodeTransform>& pose,
		std::vector<std::vector<float>>& morphWeights);
	[[nodiscard]] bool BuildGlobalNodeMatrices(
		const std::vector<ModelNode>& nodes,
		const std::vector<NodeTransform>& localPose,
		std::vector<Math::float4x4>& outGlobal);
	[[nodiscard]] bool BuildGlobalNodeMatrices(
		const std::vector<ModelNode>& nodes,
		const std::vector<NodeTransform>& localPose,
		std::vector<Math::float4x4>& outGlobal,
		HierarchyEvaluationScratch& scratch);
	[[nodiscard]] bool BuildSkinPalette(
		const Math::float4x4& meshNodeGlobal,
		const std::vector<Math::float4x4>& globalNodes,
		const ModelSkin& skin,
		std::vector<SkinJointMatrices>& outPalette);
}
