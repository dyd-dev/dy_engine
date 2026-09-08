#include "Graphics/Animation.h"

#include <algorithm>
#include <cmath>

namespace dy::Graphics
{
	namespace
	{
		template<typename Key>
		bool HasValidTimes(const std::vector<Key>& keys)
		{
			for(size_t index = 1; index < keys.size(); ++index)
				if(keys[index].time < keys[index - 1].time) return false;
			return true;
		}

		template<typename Key>
		size_t FindRightKey(const std::vector<Key>& keys, float time)
		{
			return static_cast<size_t>(std::upper_bound(
				keys.begin(), keys.end(), time,
				[](float sampleTime, const Key& key) { return sampleTime < key.time; }) - keys.begin());
		}

		Math::float3 Hermite(
			const Math::float3& from,
			const Math::float3& fromTangent,
			const Math::float3& to,
			const Math::float3& toTangent,
			float t,
			float duration)
		{
			const float t2 = t * t;
			const float t3 = t2 * t;
			const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
			const float h10 = t3 - 2.0f * t2 + t;
			const float h01 = -2.0f * t3 + 3.0f * t2;
			const float h11 = t3 - t2;
			return from * h00 + fromTangent * (h10 * duration)
				+ to * h01 + toTangent * (h11 * duration);
		}

		Math::quat Hermite(
			const Math::quat& from,
			const Math::quat& fromTangent,
			const Math::quat& to,
			const Math::quat& toTangent,
			float t,
			float duration)
		{
			const float t2 = t * t;
			const float t3 = t2 * t;
			const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
			const float h10 = t3 - 2.0f * t2 + t;
			const float h01 = -2.0f * t3 + 3.0f * t2;
			const float h11 = t3 - t2;
			return Math::Normalize(Math::quat(
				from.x * h00 + fromTangent.x * h10 * duration + to.x * h01 + toTangent.x * h11 * duration,
				from.y * h00 + fromTangent.y * h10 * duration + to.y * h01 + toTangent.y * h11 * duration,
				from.z * h00 + fromTangent.z * h10 * duration + to.z * h01 + toTangent.z * h11 * duration,
				from.w * h00 + fromTangent.w * h10 * duration + to.w * h01 + toTangent.w * h11 * duration));
		}

		float Hermite(float from, float fromTangent, float to, float toTangent, float t, float duration)
		{
			const float t2 = t * t;
			const float t3 = t2 * t;
			const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
			const float h10 = t3 - 2.0f * t2 + t;
			const float h01 = -2.0f * t3 + 3.0f * t2;
			const float h11 = t3 - t2;
			return from * h00 + fromTangent * h10 * duration
				+ to * h01 + toTangent * h11 * duration;
		}

		bool SampleVec3Keys(
			const std::vector<Vec3Key>& keys,
			AnimationInterpolation interpolation,
			float time,
			Math::float3& outValue)
		{
			if(keys.empty()) return true;
			if(!HasValidTimes(keys)) return false;
			if(keys.size() == 1 || time <= keys.front().time)
			{
				outValue = keys.front().value;
				return true;
			}
			if(time >= keys.back().time)
			{
				outValue = keys.back().value;
				return true;
			}

			const size_t rightIndex = FindRightKey(keys, time);
			const Vec3Key& left = keys[rightIndex - 1];
			const Vec3Key& right = keys[rightIndex];
			if(interpolation == AnimationInterpolation::Step)
			{
				outValue = left.value;
				return true;
			}

			const float duration = right.time - left.time;
			if(duration <= 0.0f)
			{
				outValue = right.value;
				return true;
			}
			const float alpha = (time - left.time) / duration;
			if(interpolation == AnimationInterpolation::CubicSpline)
				outValue = Hermite(left.value, left.outTangent, right.value, right.inTangent, alpha, duration);
			else
				outValue = left.value + (right.value - left.value) * alpha;
			return true;
		}

		bool SampleQuatKeys(
			const std::vector<QuatKey>& keys,
			AnimationInterpolation interpolation,
			float time,
			Math::quat& outValue)
		{
			if(keys.empty()) return true;
			if(!HasValidTimes(keys)) return false;
			if(keys.size() == 1 || time <= keys.front().time)
			{
				outValue = Math::Normalize(keys.front().value);
				return true;
			}
			if(time >= keys.back().time)
			{
				outValue = Math::Normalize(keys.back().value);
				return true;
			}

			const size_t rightIndex = FindRightKey(keys, time);
			const QuatKey& left = keys[rightIndex - 1];
			const QuatKey& right = keys[rightIndex];
			if(interpolation == AnimationInterpolation::Step)
			{
				outValue = Math::Normalize(left.value);
				return true;
			}

			const float duration = right.time - left.time;
			if(duration <= 0.0f)
			{
				outValue = Math::Normalize(right.value);
				return true;
			}
			const float alpha = (time - left.time) / duration;
			if(interpolation == AnimationInterpolation::CubicSpline)
				outValue = Hermite(left.value, left.outTangent, right.value, right.inTangent, alpha, duration);
			else
				outValue = Math::Slerp(Math::Normalize(left.value), Math::Normalize(right.value), alpha);
			return true;
		}

		bool SampleFloatKeys(
			const std::vector<FloatKey>& keys,
			AnimationInterpolation interpolation,
			float time,
			float& outValue)
		{
			if(keys.empty()) return true;
			if(!HasValidTimes(keys)) return false;
			for(const FloatKey& key : keys)
			{
				if(!std::isfinite(key.time) || !std::isfinite(key.inTangent)
					|| !std::isfinite(key.value) || !std::isfinite(key.outTangent)) return false;
			}
			if(keys.size() == 1u || time <= keys.front().time)
			{
				outValue = keys.front().value;
				return true;
			}
			if(time >= keys.back().time)
			{
				outValue = keys.back().value;
				return true;
			}

			const size_t rightIndex = FindRightKey(keys, time);
			const FloatKey& left = keys[rightIndex - 1u];
			const FloatKey& right = keys[rightIndex];
			if(interpolation == AnimationInterpolation::Step)
			{
				outValue = left.value;
				return true;
			}
			const float duration = right.time - left.time;
			if(duration <= 0.0f)
			{
				outValue = right.value;
				return true;
			}
			const float alpha = (time - left.time) / duration;
			outValue = interpolation == AnimationInterpolation::CubicSpline
				? Hermite(left.value, left.outTangent, right.value, right.inTangent, alpha, duration)
				: left.value + (right.value - left.value) * alpha;
			return std::isfinite(outValue);
		}
	}

	Math::float4x4 ComposeTransform(const NodeTransform& transform)
	{
		return Math::Translation(transform.translation)
			* Math::QuatToMatrix(Math::Normalize(transform.rotation))
			* Math::Scaling(transform.scale);
	}

	bool SampleAnimationClip(const AnimationClip& clip, float time, std::vector<NodeTransform>& pose)
	{
		if(!std::isfinite(time)) return false;
		for(const NodeAnimationTrack& track : clip.tracks)
		{
			if(track.nodeIndex >= pose.size()) return false;
			NodeTransform sampled = pose[track.nodeIndex];
			if(!SampleVec3Keys(track.translations, track.translationInterpolation, time, sampled.translation)) return false;
			if(!SampleQuatKeys(track.rotations, track.rotationInterpolation, time, sampled.rotation)) return false;
			if(!SampleVec3Keys(track.scales, track.scaleInterpolation, time, sampled.scale)) return false;
			pose[track.nodeIndex] = sampled;
		}
		return true;
	}

	bool BuildGlobalNodeMatrices(
		const std::vector<ModelNode>& nodes,
		const std::vector<NodeTransform>& localPose,
		std::vector<Math::float4x4>& outGlobal)
	{
		HierarchyEvaluationScratch scratch;
		return BuildGlobalNodeMatrices(nodes, localPose, outGlobal, scratch);
	}

	bool BuildGlobalNodeMatrices(
		const std::vector<ModelNode>& nodes,
		const std::vector<NodeTransform>& localPose,
		std::vector<Math::float4x4>& outGlobal,
		HierarchyEvaluationScratch& scratch)
	{
		if(nodes.size() != localPose.size()) return false;
		outGlobal.resize(nodes.size());
		scratch.visitState.assign(nodes.size(), 0u);
		scratch.nodeStack.clear();
		if(scratch.nodeStack.capacity() < nodes.size()) scratch.nodeStack.reserve(nodes.size());

		for(uint32_t startNode = 0u; startNode < nodes.size(); ++startNode)
		{
			if(scratch.visitState[startNode] == 2u) continue;
			scratch.nodeStack.push_back(startNode);
			while(!scratch.nodeStack.empty())
			{
				const uint32_t nodeIndex = scratch.nodeStack.back();
				const int32_t parentIndex = nodes[nodeIndex].parentIndex;
				if(parentIndex < -1 || (parentIndex >= 0 && static_cast<size_t>(parentIndex) >= nodes.size())) return false;

				if(scratch.visitState[nodeIndex] == 0u)
				{
					scratch.visitState[nodeIndex] = 1u;
					if(parentIndex >= 0)
					{
						const uint32_t parent = static_cast<uint32_t>(parentIndex);
						if(scratch.visitState[parent] == 1u) return false;
						if(scratch.visitState[parent] == 0u)
						{
							scratch.nodeStack.push_back(parent);
							continue;
						}
					}
				}

				const Math::float4x4 local = ComposeTransform(localPose[nodeIndex]);
				outGlobal[nodeIndex] = parentIndex >= 0
					? outGlobal[static_cast<uint32_t>(parentIndex)] * local
					: local;
				scratch.visitState[nodeIndex] = 2u;
				scratch.nodeStack.pop_back();
			}
		}
		return true;
	}

	bool SampleAnimationClip(
		const AnimationClip& clip,
		float time,
		std::vector<NodeTransform>& pose,
		std::vector<std::vector<float>>& morphWeights)
	{
		if(!SampleAnimationClip(clip, time, pose)) return false;
		for(const MorphWeightTrack& track : clip.morphTracks)
		{
			if(track.nodeIndex >= morphWeights.size()
				|| track.targetIndex >= morphWeights[track.nodeIndex].size()) return false;
			float sampled = morphWeights[track.nodeIndex][track.targetIndex];
			if(!SampleFloatKeys(track.weights, track.interpolation, time, sampled)) return false;
			morphWeights[track.nodeIndex][track.targetIndex] = sampled;
		}
		return true;
	}

	bool BuildSkinPalette(
		const Math::float4x4& meshNodeGlobal,
		const std::vector<Math::float4x4>& globalNodes,
		const ModelSkin& skin,
		std::vector<SkinJointMatrices>& outPalette)
	{
		if(skin.jointNodeIndices.size() != skin.inverseBindMatrices.size()) return false;
		Math::float4x4 inverseMesh = {};
		if(!Math::Inverse(meshNodeGlobal, inverseMesh)) return false;

		outPalette.resize(skin.jointNodeIndices.size());
		for(size_t jointIndex = 0; jointIndex < outPalette.size(); ++jointIndex)
		{
			const uint32_t nodeIndex = skin.jointNodeIndices[jointIndex];
			if(nodeIndex >= globalNodes.size()) return false;
			SkinJointMatrices& joint = outPalette[jointIndex];
			joint.positionMatrix = inverseMesh * globalNodes[nodeIndex] * skin.inverseBindMatrices[jointIndex];
			if(!Math::InverseTranspose(joint.positionMatrix, joint.normalMatrix)) return false;
			Math::float3 translation;
			Math::quat rotation;
			Math::float3 scale;
			if(Math::DecomposeTRS(joint.positionMatrix, translation, rotation, scale))
			{
				if(!Math::MakeDualQuaternionTRS(
					translation,
					rotation,
					scale,
					joint.dualQuaternion)) return false;
			}
			else if(skin.method == SkinningMethod::DualQuaternion
				|| skin.method == SkinningMethod::BlendedDualQuaternion)
			{
				return false;
			}
		}
		return true;
	}
}
