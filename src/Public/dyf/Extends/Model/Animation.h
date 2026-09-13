#pragma once

#include <cstdint>
#include <array>
#include <string>
#include <vector>

#include "dyf/Types.h"
#include "dyf/Mesh.h"

namespace dyf
{
	enum class ModelAssetID        : uint32_t { Invalid = 0xFFFFFFFF };
	enum class ModelInstanceID     : uint32_t { Invalid = 0xFFFFFFFF };

	struct alignas(16) SkinInfluence
	{
		std::array<uint32_t, 4> jointIndices = { 0u, 0u, 0u, 0u };
		Math::float4 weights = Math::float4(0.0f, 0.0f, 0.0f, 0.0f);
		float dqBlendWeight = 0.0f;
		std::array<float, 3> _padding = { 0.0f, 0.0f, 0.0f };
	};

	struct alignas(16) SkinJointMatrices
	{
		Math::float4x4 positionMatrix = Math::float4x4::Identity();
		Math::float4x4 normalMatrix = Math::float4x4::Identity();
		Math::DualQuaternionTRS dualQuaternion = {};
	};



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

	// 키의 time, 클립의 duration, 재생 위치 time은 모두 초 단위다. speed는 시간 배율이다.
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
		uint32_t nodeIndex = UINT32_MAX;
		AnimationInterpolation translationInterpolation = AnimationInterpolation::Linear;
		AnimationInterpolation rotationInterpolation = AnimationInterpolation::Linear;
		AnimationInterpolation scaleInterpolation = AnimationInterpolation::Linear;
		std::vector<Vec3Key> translations;
		std::vector<QuatKey> rotations;
		std::vector<Vec3Key> scales;
	};

	struct MorphWeightTrack
	{
		uint32_t nodeIndex = UINT32_MAX;
		uint32_t targetIndex = UINT32_MAX;
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



	struct AnimationPlayback
	{
		uint32_t clipIndex = UINT32_MAX;
		float time = 0.0f;
		float speed = 1.0f;
		bool playing = false;
		bool loop = true;
	};

	struct ModelEntityBinding
	{
		EntityID entity = EntityID::Invalid;
		uint32_t nodeIndex = UINT32_MAX;
		uint32_t skinIndex = UINT32_MAX;
		uint32_t assetMeshIndex = UINT32_MAX;
		uint32_t paletteOffset = UINT32_MAX;
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
		AnimationPlayback playback;
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
	[[nodiscard]] bool BuildSkinPalette(
		const Math::float4x4& meshNodeGlobal,
		const std::vector<Math::float4x4>& globalNodes,
		const ModelSkin& skin,
		std::vector<SkinJointMatrices>& outPalette);
}
