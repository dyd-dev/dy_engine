#include "dyf/Extends/Model/ModelScene.h"
#include "dyf/RHI/Buffer.h"
#include "dyf/RHI/ICommandList.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/RenderGraph.h"



#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <locale>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

using namespace dyf;
using namespace dyf::RHI;

namespace
{
	constexpr char kCaseMagic[] = "DY_CPU_CASE_V1\n";
	constexpr size_t kMaxPayloadBytes = 1024u * 1024u;

	class CheckFailure final : public std::runtime_error
	{
	public:
		using std::runtime_error::runtime_error;
	};

	class InfraError final : public std::runtime_error
	{
	public:
		using std::runtime_error::runtime_error;
	};

	struct Runner
	{
		uint64_t checks = 0u;

		void Require(bool condition, const std::string& message)
		{
			++checks;
			if(!condition) throw CheckFailure(message);
		}
	};

	struct CaseData
	{
		std::string scenario;
		uint64_t seed = 0u;
		std::vector<uint8_t> payload;
	};

	class ByteWriter
	{
	public:
		void U8(uint8_t value) { m_bytes.push_back(value); }
		void U16(uint16_t value)
		{
			U8(static_cast<uint8_t>(value));
			U8(static_cast<uint8_t>(value >> 8u));
		}
		void U32(uint32_t value)
		{
			for(uint32_t shift = 0u; shift < 32u; shift += 8u)
				U8(static_cast<uint8_t>(value >> shift));
		}
		void U64(uint64_t value)
		{
			for(uint32_t shift = 0u; shift < 64u; shift += 8u)
				U8(static_cast<uint8_t>(value >> shift));
		}
		void F32(float value)
		{
			uint32_t bits = 0u;
			static_assert(sizeof(bits) == sizeof(value));
			std::memcpy(&bits, &value, sizeof(bits));
			U32(bits);
		}
		void String(const std::string& value)
		{
			if(value.size() > kMaxPayloadBytes) throw InfraError("case string exceeds limit");
			U32(static_cast<uint32_t>(value.size()));
			m_bytes.insert(m_bytes.end(), value.begin(), value.end());
		}
		void Bytes(const std::vector<uint8_t>& value)
		{
			if(value.size() > kMaxPayloadBytes) throw InfraError("case bytes exceed limit");
			U32(static_cast<uint32_t>(value.size()));
			m_bytes.insert(m_bytes.end(), value.begin(), value.end());
		}
		std::vector<uint8_t> Take()
		{
			if(m_bytes.size() > kMaxPayloadBytes) throw InfraError("case payload exceeds limit");
			return std::move(m_bytes);
		}

	private:
		std::vector<uint8_t> m_bytes;
	};

	class ByteReader
	{
	public:
		explicit ByteReader(const std::vector<uint8_t>& bytes)
			: m_bytes(bytes)
		{
		}

		uint8_t U8()
		{
			Need(1u);
			return m_bytes[m_position++];
		}
		uint16_t U16()
		{
			uint16_t value = U8();
			value |= static_cast<uint16_t>(U8()) << 8u;
			return value;
		}
		uint32_t U32()
		{
			uint32_t value = 0u;
			for(uint32_t shift = 0u; shift < 32u; shift += 8u)
				value |= static_cast<uint32_t>(U8()) << shift;
			return value;
		}
		uint64_t U64()
		{
			uint64_t value = 0u;
			for(uint32_t shift = 0u; shift < 64u; shift += 8u)
				value |= static_cast<uint64_t>(U8()) << shift;
			return value;
		}
		float F32()
		{
			const uint32_t bits = U32();
			float value = 0.0f;
			std::memcpy(&value, &bits, sizeof(value));
			return value;
		}
		std::string String(size_t limit = 4096u)
		{
			const uint32_t size = U32();
			if(size > limit) throw InfraError("case string exceeds parse limit");
			Need(size);
			const auto begin = m_bytes.begin() + static_cast<std::ptrdiff_t>(m_position);
			m_position += size;
			return std::string(begin, begin + size);
		}
		std::vector<uint8_t> Bytes(size_t limit = kMaxPayloadBytes)
		{
			const uint32_t size = U32();
			if(size > limit) throw InfraError("case bytes exceed parse limit");
			Need(size);
			const auto begin = m_bytes.begin() + static_cast<std::ptrdiff_t>(m_position);
			m_position += size;
			return std::vector<uint8_t>(begin, begin + size);
		}
		void Finish() const
		{
			if(m_position != m_bytes.size()) throw InfraError("case payload has trailing bytes");
		}

	private:
		void Need(size_t count) const
		{
			if(count > m_bytes.size() - m_position) throw InfraError("truncated case payload");
		}

		const std::vector<uint8_t>& m_bytes;
		size_t m_position = 0u;
	};

	bool IsScenario(std::string_view scenario)
	{
		return
			scenario == "Animation"
			|| scenario == "ModelImport" || scenario == "RenderGraph";
	}

	bool IsStrictDescendant(const std::filesystem::path& child, const std::filesystem::path& parent)
	{
		auto childPart = child.begin();
		for(auto parentPart = parent.begin(); parentPart != parent.end(); ++parentPart, ++childPart)
			if(childPart == child.end() || *childPart != *parentPart) return false;
		return childPart != child.end();
	}

	class OwnedRunDirectory final
	{
	public:
		OwnedRunDirectory(const std::filesystem::path& base, const std::string& prefix)
		{
			std::error_code error;
			std::filesystem::create_directories(base, error);
			if(error) throw InfraError("could not create case base: " + error.message());
			m_base = std::filesystem::weakly_canonical(base, error);
			if(error) throw InfraError("could not resolve case base: " + error.message());

			std::mt19937_64 random(
				static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())
				^ (static_cast<uint64_t>(std::random_device{}()) << 32u)
				^ static_cast<uint64_t>(std::random_device{}()));
			for(uint32_t attempt = 0u; attempt < 128u; ++attempt)
			{
				std::ostringstream name;
				name << prefix << '-' << std::hex << random() << '-' << attempt;
				const std::filesystem::path candidate = m_base / name.str();
				error.clear();
				if(std::filesystem::create_directory(candidate, error))
				{
					m_path = candidate;
					break;
				}
				if(error && error != std::errc::file_exists)
					throw InfraError("could not allocate owned run directory: " + error.message());
			}
			if(m_path.empty()) throw InfraError("could not allocate a unique run directory");
			m_resolvedPath = std::filesystem::weakly_canonical(m_path, error);
			if(error || !IsStrictDescendant(m_resolvedPath, m_base))
				throw InfraError("owned run directory escaped case base");
			m_caseDirectory = CreateOwnedChild("case");
			m_fixtureDirectory = CreateOwnedChild("fixtures");
		}

		~OwnedRunDirectory()
		{
			if(m_cleanup) CleanupNoThrow();
		}

		OwnedRunDirectory(const OwnedRunDirectory&) = delete;
		OwnedRunDirectory& operator=(const OwnedRunDirectory&) = delete;

		[[nodiscard]] const std::filesystem::path& CaseDirectory() const { return m_caseDirectory; }
		[[nodiscard]] const std::filesystem::path& FixtureDirectory() const { return m_fixtureDirectory; }

		void Preserve() { m_cleanup = false; }

		void Cleanup()
		{
			if(!m_cleanup) return;
			if(!ValidateCleanupTarget()) throw InfraError("refusing unsafe run-directory cleanup");
			std::error_code error;
			std::filesystem::remove_all(m_path, error);
			if(error) throw InfraError("could not remove owned run directory: " + error.message());
			m_cleanup = false;
		}

	private:
		std::filesystem::path CreateOwnedChild(const char* name)
		{
			const std::filesystem::path child = m_path / name;
			std::error_code error;
			if(!std::filesystem::create_directory(child, error) || error)
				throw InfraError(std::string("could not create owned ") + name + " directory");
			return child;
		}

		bool ValidateCleanupTarget() const
		{
			std::error_code error;
			const std::filesystem::file_status status = std::filesystem::symlink_status(m_path, error);
			if(error || !std::filesystem::is_directory(status) || std::filesystem::is_symlink(status)) return false;
			const std::filesystem::path resolved = std::filesystem::weakly_canonical(m_path, error);
			return !error && resolved == m_resolvedPath && IsStrictDescendant(resolved, m_base);
		}

		void CleanupNoThrow() noexcept
		{
			if(!ValidateCleanupTarget()) return;
			std::error_code error;
			std::filesystem::remove_all(m_path, error);
		}

		std::filesystem::path m_base;
		std::filesystem::path m_path;
		std::filesystem::path m_resolvedPath;
		std::filesystem::path m_caseDirectory;
		std::filesystem::path m_fixtureDirectory;
		bool m_cleanup = true;
	};

	void WriteCase(const std::filesystem::path& path, const CaseData& data)
	{
		if(!IsScenario(data.scenario) || data.payload.empty() || data.payload.size() > kMaxPayloadBytes)
			throw InfraError("invalid generated case");
		std::error_code statusError;
		const std::filesystem::file_status status = std::filesystem::symlink_status(path, statusError);
		if(statusError && statusError != std::errc::no_such_file_or_directory)
			throw InfraError("could not inspect case path: " + statusError.message());
		if(!statusError && status.type() != std::filesystem::file_type::not_found
			&& (!std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status)))
			throw InfraError("refusing non-regular or linked case path");
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		if(!output) throw InfraError("could not open case file: " + path.string());
		output.write(kCaseMagic, sizeof(kCaseMagic) - 1u);
		const uint8_t scenarioSize = static_cast<uint8_t>(data.scenario.size());
		output.put(static_cast<char>(scenarioSize));
		output.write(data.scenario.data(), scenarioSize);
		for(uint32_t shift = 0u; shift < 64u; shift += 8u)
			output.put(static_cast<char>(data.seed >> shift));
		const uint32_t payloadSize = static_cast<uint32_t>(data.payload.size());
		for(uint32_t shift = 0u; shift < 32u; shift += 8u)
			output.put(static_cast<char>(payloadSize >> shift));
		output.write(reinterpret_cast<const char*>(data.payload.data()), payloadSize);
		output.flush();
		if(!output) throw InfraError("could not write case file: " + path.string());
	}

	CaseData ReadCase(const std::filesystem::path& path)
	{
		std::error_code sizeError;
		const uintmax_t size = std::filesystem::file_size(path, sizeError);
		if(sizeError || size > kMaxPayloadBytes + 64u) throw InfraError("invalid replay file size");
		std::ifstream input(path, std::ios::binary);
		std::vector<uint8_t> bytes(
			(std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
		if(input.bad()) throw InfraError("could not read replay file");
		const size_t magicSize = sizeof(kCaseMagic) - 1u;
		if(bytes.size() < magicSize + 1u + 8u + 4u
			|| !std::equal(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(magicSize), kCaseMagic))
			throw InfraError("invalid replay header");
		size_t position = magicSize;
		const uint8_t scenarioSize = bytes[position++];
		if(scenarioSize == 0u || scenarioSize > 32u || position + scenarioSize + 12u > bytes.size())
			throw InfraError("invalid replay scenario");
		CaseData data;
		data.scenario.assign(
			bytes.begin() + static_cast<std::ptrdiff_t>(position),
			bytes.begin() + static_cast<std::ptrdiff_t>(position + scenarioSize));
		position += scenarioSize;
		if(!IsScenario(data.scenario)) throw InfraError("unknown replay scenario");
		for(uint32_t shift = 0u; shift < 64u; shift += 8u)
			data.seed |= static_cast<uint64_t>(bytes[position++]) << shift;
		uint32_t payloadSize = 0u;
		for(uint32_t shift = 0u; shift < 32u; shift += 8u)
			payloadSize |= static_cast<uint32_t>(bytes[position++]) << shift;
		if(payloadSize == 0u || payloadSize > kMaxPayloadBytes || position + payloadSize != bytes.size())
			throw InfraError("invalid replay payload size");
		data.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(position), bytes.end());
		return data;
	}

	bool Near(float lhs, float rhs, float epsilon = 1.0e-4f)
	{
		return std::fabs(lhs - rhs) <= epsilon;
	}

	std::vector<uint8_t> FixedPayload()
	{
		return { 0u };
	}


	void RunAnimationFixed(Runner& runner)
	{
		ModelInstance instance;
		instance.nodes.resize(1u);
		NodeAnimationTrack track;
		track.nodeIndex = 0u;
		track.translations = {
			Vec3Key{ 0.0f, {}, Math::float3(0.0f, 0.0f, 0.0f), {} },
			Vec3Key{ 1.0f, {}, Math::float3(2.0f, 0.0f, 0.0f), {} }
		};
		AnimationClip clip;
		clip.duration = 1.0f;
		clip.tracks.push_back(track);
		instance.clips.push_back(clip);
		ModelScene scene;
		const ModelInstanceID id = scene.CreateModelInstance(std::move(instance));
		runner.Require(IsValid(id) && scene.PlayAnimation(id, 0u, true), "animation could not start");
		runner.Require(scene.UpdateAnimations(0.25f), "animation update failed");
		runner.Require(Near(scene.GetModelInstance(id).localPose[0].translation.x, 0.5f), "linear interpolation failed");
		runner.Require(scene.SetAnimationPaused(id, true) && scene.UpdateAnimations(0.5f), "pause failed");
		runner.Require(Near(scene.GetModelInstance(id).playback.time, 0.25f), "paused animation advanced");
		runner.Require(scene.SetAnimationPaused(id, false) && scene.UpdateAnimations(1.0f), "resume failed");
		runner.Require(Near(scene.GetModelInstance(id).playback.time, 0.25f), "animation did not loop");
		runner.Require(scene.PlayAnimation(id, 0u, false) && scene.UpdateAnimations(2.0f), "non-looping update failed");
		runner.Require(Near(scene.GetModelInstance(id).localPose[0].translation.x, 2.0f)
			&& Near(scene.GetModelInstance(id).playback.time, 1.0f)
			&& !scene.GetModelInstance(id).playback.playing, "non-looping animation did not clamp and stop");
		runner.Require(scene.PlayAnimation(id, 0u, false) && scene.SetAnimationSpeed(id, -1.0f)
			&& scene.UpdateAnimations(0.25f), "negative playback failed");
		runner.Require(Near(scene.GetModelInstance(id).playback.time, 0.0f)
			&& !scene.GetModelInstance(id).playback.playing, "negative playback did not clamp at zero");
		runner.Require(!scene.PlayAnimation(id, 99u), "invalid animation clip accepted");
		runner.Require(!scene.SetAnimationSpeed(id, std::numeric_limits<float>::infinity()), "infinite speed accepted");
		runner.Require(!scene.UpdateAnimations(std::numeric_limits<float>::quiet_NaN()), "NaN delta accepted");

		std::vector<NodeTransform> pose(1u);
		runner.Require(SampleAnimationClip(clip, -2.0f, pose)
			&& Near(pose[0].translation.x, 0.0f), "negative sample time did not clamp to first key");
		AnimationClip badTrack = clip;
		badTrack.tracks[0].nodeIndex = 1u;
		runner.Require(!SampleAnimationClip(badTrack, 0.0f, pose), "out-of-range animation node accepted");
		badTrack = clip;
		std::swap(badTrack.tracks[0].translations[0], badTrack.tracks[0].translations[1]);
		runner.Require(!SampleAnimationClip(badTrack, 0.5f, pose), "descending animation times accepted");

		std::vector<ModelNode> nodes(1u);
		nodes[0].parentIndex = 0;
		std::vector<Math::float4x4> globals;
		runner.Require(!BuildGlobalNodeMatrices(nodes, std::vector<NodeTransform>(1u), globals), "cyclic hierarchy accepted");
		nodes[0].parentIndex = 2;
		runner.Require(!BuildGlobalNodeMatrices(nodes, std::vector<NodeTransform>(1u), globals), "out-of-range parent accepted");
	}

	std::vector<uint8_t> GenerateAnimationCase(std::mt19937_64& random)
	{
		ByteWriter writer;
		const uint8_t kind = static_cast<uint8_t>(1u + random() % 3u);
		writer.U8(kind);
		if(kind == 1u)
		{
			const float duration = static_cast<float>(1u + random() % 40u) * 0.25f;
			const float time = (static_cast<int>(random() % 201u) - 50) * duration / 50.0f;
			const float from = static_cast<float>(static_cast<int>(random() % 201u) - 100);
			const float to = static_cast<float>(static_cast<int>(random() % 201u) - 100);
			writer.F32(duration);
			writer.F32(time);
			writer.F32(from);
			writer.F32(to);
		}
		else if(kind == 2u)
		{
			const uint32_t poseCount = static_cast<uint32_t>(random() % 16u);
			writer.U32(poseCount);
			writer.U32(poseCount + 1u + static_cast<uint32_t>(random() % 16u));
		}
		else
		{
			writer.U32(1u + static_cast<uint32_t>(random() % 32u));
		}
		return writer.Take();
	}

	void RunAnimationCase(const std::vector<uint8_t>& payload, Runner& runner)
	{
		ByteReader reader(payload);
		const uint8_t kind = reader.U8();
		if(kind == 1u)
		{
			const float duration = reader.F32();
			const float time = reader.F32();
			const float from = reader.F32();
			const float to = reader.F32();
			reader.Finish();
			if(!std::isfinite(duration) || duration <= 0.0f || !std::isfinite(time)
				|| !std::isfinite(from) || !std::isfinite(to)) throw InfraError("invalid Animation sample case");
			AnimationClip clip;
			NodeAnimationTrack track;
			track.nodeIndex = 0u;
			track.translations = {
				Vec3Key{ 0.0f, {}, Math::float3(from, 0.0f, 0.0f), {} },
				Vec3Key{ duration, {}, Math::float3(to, 0.0f, 0.0f), {} }
			};
			clip.duration = duration;
			clip.tracks.push_back(track);
			std::vector<NodeTransform> pose(1u);
			runner.Require(SampleAnimationClip(clip, time, pose), "finite linear animation case rejected");
			const float alpha = std::clamp(time / duration, 0.0f, 1.0f);
			const float expected = from + (to - from) * alpha;
			runner.Require(std::isfinite(pose[0].translation.x) && Near(pose[0].translation.x, expected),
				"linear animation differs from reference interpolation");
			return;
		}
		if(kind == 2u)
		{
			const uint32_t poseCount = reader.U32();
			const uint32_t nodeIndex = reader.U32();
			reader.Finish();
			if(poseCount > 64u || nodeIndex < poseCount) throw InfraError("invalid Animation node case");
			AnimationClip clip;
			NodeAnimationTrack track;
			track.nodeIndex = nodeIndex;
			track.translations.push_back(Vec3Key{});
			clip.tracks.push_back(track);
			std::vector<NodeTransform> pose(poseCount);
			runner.Require(!SampleAnimationClip(clip, 0.0f, pose), "malformed animation node accepted");
			return;
		}
		if(kind == 3u)
		{
			const uint32_t count = reader.U32();
			reader.Finish();
			if(count == 0u || count > 64u) throw InfraError("invalid Animation cycle case");
			std::vector<ModelNode> nodes(count);
			for(uint32_t index = 1u; index < count; ++index) nodes[index].parentIndex = static_cast<int32_t>(index - 1u);
			nodes[0].parentIndex = static_cast<int32_t>(count - 1u);
			std::vector<Math::float4x4> globals;
			runner.Require(!BuildGlobalNodeMatrices(nodes, std::vector<NodeTransform>(count), globals),
				"cyclic animation hierarchy accepted");
			return;
		}
		throw InfraError("unknown Animation case kind");
	}

	bool Finite(const Math::float2& value)
	{
		return std::isfinite(value.x) && std::isfinite(value.y);
	}
	bool Finite(const Math::float3& value)
	{
		return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
	}
	bool Finite(const Math::float4& value)
	{
		return std::isfinite(value.x) && std::isfinite(value.y)
			&& std::isfinite(value.z) && std::isfinite(value.w);
	}
	bool Finite(const Math::quat& value)
	{
		return std::isfinite(value.x) && std::isfinite(value.y)
			&& std::isfinite(value.z) && std::isfinite(value.w);
	}
	bool Finite(const Math::float4x4& value)
	{
		return std::all_of(std::begin(value.m), std::end(value.m), [](float entry) { return std::isfinite(entry); });
	}
	bool Finite(const NodeTransform& value)
	{
		return Finite(value.translation) && Finite(value.rotation) && Finite(value.scale);
	}

	void ValidateModel(const ModelData& model, Runner& runner)
	{
		runner.Require(Finite(model.assetTransform), "imported asset transform is not finite");
		for(size_t nodeIndex = 0u; nodeIndex < model.nodes.size(); ++nodeIndex)
		{
			const ModelNode& node = model.nodes[nodeIndex];
			runner.Require(node.parentIndex >= -1
				&& (node.parentIndex < 0 || static_cast<size_t>(node.parentIndex) < model.nodes.size()),
				"imported node parent is out of range");
			runner.Require(Finite(node.bindTransform)
				&& std::all_of(node.morphWeights.begin(), node.morphWeights.end(), [](float v) { return std::isfinite(v); }),
				"imported node data is not finite");
		}
		for(const ModelSkin& skin : model.skins)
		{
			runner.Require(skin.jointNodeIndices.size() == skin.inverseBindMatrices.size(),
				"imported skin arrays have different sizes");
			for(uint32_t joint : skin.jointNodeIndices)
				runner.Require(joint < model.nodes.size(), "imported skin joint is out of range");
			for(const Math::float4x4& matrix : skin.inverseBindMatrices)
				runner.Require(Finite(matrix), "imported inverse bind matrix is not finite");
		}
		for(const ModelMaterialInfo& material : model.materials)
		{
			runner.Require(Finite(material.material.baseColor) && Finite(material.material.emissiveColor)
				&& std::isfinite(material.material.metallicFactor) && std::isfinite(material.material.roughnessFactor)
				&& std::isfinite(material.material.normalScale) && std::isfinite(material.material.occlusionStrength),
				"imported material data is not finite");
			for(uint32_t texture : material.textureIndices)
				runner.Require(texture == UINT32_MAX || texture < model.textures.size(),
					"imported material texture is out of range");
		}
		for(const ModelMesh& part : model.meshes)
		{
			runner.Require(part.materialIndex < model.materials.size(), "imported mesh material is out of range");
			runner.Require(part.nodeIndex == UINT32_MAX || part.nodeIndex < model.nodes.size(),
				"imported mesh node is out of range");
			runner.Require(part.skinIndex == UINT32_MAX || part.skinIndex < model.skins.size(),
				"imported mesh skin is out of range");
			runner.Require(part.skinInfluences.empty()
				|| part.skinInfluences.size() == part.mesh.vertices.size(),
				"imported skin influence count differs from vertex count");
			for(uint32_t index : part.mesh.indices)
				runner.Require(index < part.mesh.vertices.size(), "imported vertex index is out of range");
			for(const Vertex& vertex : part.mesh.vertices)
				runner.Require(Finite(vertex.position) && Finite(vertex.normal) && Finite(vertex.uv)
					&& Finite(vertex.color) && Finite(vertex.tangent), "imported vertex data is not finite");
			for(const SkinInfluence& influence : part.skinInfluences)
			{
				runner.Require(Finite(influence.weights) && std::isfinite(influence.dqBlendWeight),
					"imported skin influence is not finite");
				if(part.skinIndex != UINT32_MAX)
				{
					const size_t jointCount = model.skins[part.skinIndex].jointNodeIndices.size();
					for(size_t slot = 0u; slot < influence.jointIndices.size(); ++slot)
						if(influence.weights[slot] != 0.0f)
							runner.Require(influence.jointIndices[slot] < jointCount,
								"imported weighted joint is out of range");
				}
			}
			for(const MorphTarget& target : part.morphTargets)
			{
				for(const auto* values : { &target.positionDeltas, &target.normalDeltas, &target.tangentDeltas })
				{
					runner.Require(values->empty() || values->size() == part.mesh.vertices.size(),
						"imported morph target count differs from vertex count");
					for(const Math::float3& value : *values)
						runner.Require(Finite(value), "imported morph target is not finite");
				}
			}
			runner.Require(std::all_of(part.defaultMorphWeights.begin(), part.defaultMorphWeights.end(),
				[](float value) { return std::isfinite(value); }), "imported morph weight is not finite");
		}
		for(const AnimationClip& clip : model.animations)
		{
			runner.Require(std::isfinite(clip.duration), "imported animation duration is not finite");
			for(const NodeAnimationTrack& track : clip.tracks)
			{
				runner.Require(track.nodeIndex < model.nodes.size(), "imported animation node is out of range");
				for(const Vec3Key& key : track.translations)
					runner.Require(std::isfinite(key.time) && Finite(key.inTangent) && Finite(key.value)
						&& Finite(key.outTangent), "imported translation key is not finite");
				for(const Vec3Key& key : track.scales)
					runner.Require(std::isfinite(key.time) && Finite(key.inTangent) && Finite(key.value)
						&& Finite(key.outTangent), "imported scale key is not finite");
				for(const QuatKey& key : track.rotations)
					runner.Require(std::isfinite(key.time) && Finite(key.inTangent) && Finite(key.value)
						&& Finite(key.outTangent), "imported rotation key is not finite");
			}
			for(const MorphWeightTrack& track : clip.morphTracks)
			{
				runner.Require(track.nodeIndex < model.nodes.size(), "imported morph animation node is out of range");
				for(const FloatKey& key : track.weights)
					runner.Require(std::isfinite(key.time) && std::isfinite(key.inTangent)
						&& std::isfinite(key.value) && std::isfinite(key.outTangent),
						"imported morph key is not finite");
			}
		}
	}

	class CaptureCerr final
	{
	public:
		CaptureCerr()
			: m_previous(std::cerr.rdbuf(&m_sink))
		{
		}
		~CaptureCerr() { std::cerr.rdbuf(m_previous); }
		std::string Text() const { return m_sink.str(); }
	private:
		std::stringbuf m_sink;
		std::streambuf* m_previous = nullptr;
	};

	void WriteBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
	{
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		if(!output) throw InfraError("could not create model fixture: " + path.string());
		output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		if(!output) throw InfraError("could not write model fixture: " + path.string());
	}

	void WriteText(const std::filesystem::path& path, const std::string& text)
	{
		WriteBytes(path, std::vector<uint8_t>(text.begin(), text.end()));
	}

	std::vector<uint8_t> TriangleBytes(uint16_t thirdIndex = 2u)
	{
		ByteWriter writer;
		for(float value : { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f }) writer.F32(value);
		writer.U16(0u);
		writer.U16(1u);
		writer.U16(thirdIndex);
		return writer.Take();
	}

	std::string TriangleGltf(const std::string& bufferName)
	{
		return std::string(R"({
"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],
"nodes":[{"mesh":0}],"meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}],
"buffers":[{"uri":")") + bufferName + R"(","byteLength":42}],
"bufferViews":[{"buffer":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6}],
"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},
{"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}]
})";
	}

	bool EmptyModel(const ModelData& model)
	{
		return model.meshes.empty() && model.materials.empty() && model.textures.empty()
			&& model.nodes.empty() && model.skins.empty() && model.animations.empty();
	}

	void CheckLoadResult(
		const std::filesystem::path& path,
		bool requireSuccess,
		Runner& runner,
		const ModelLoadOptions& options = {})
	{
		ModelData model;
		model.meshes.push_back(ModelMesh{});
		model.materials.push_back(ModelMaterialInfo{});
		model.textures.push_back(Image{});
		model.nodes.push_back(ModelNode{});
		model.skins.push_back(ModelSkin{});
		model.animations.push_back(AnimationClip{});
		bool success = false;
		std::string diagnostics;
		{
			CaptureCerr capture;
			success = LoadModel(path.string(), model, options);
			diagnostics = capture.Text();
		}
		if(requireSuccess)
		{
			runner.Require(success, "valid model fixture was rejected: " + path.filename().string());
			runner.Require(!model.meshes.empty() && !model.meshes[0].mesh.vertices.empty(),
				"valid model fixture has no geometry: " + path.filename().string());
			ValidateModel(model, runner);
			return;
		}
		runner.Require(!success, "malformed model fixture was accepted: " + path.filename().string());
		runner.Require(EmptyModel(model), "rejected model left stale output");
		runner.Require(diagnostics.find(path.string() + ": ") != std::string::npos,
			"rejected model has no useful error diagnostic");
	}

	void CheckFuzzLoad(const std::filesystem::path& path, Runner& runner)
	{
		ModelData model;
		model.meshes.push_back(ModelMesh{});
		model.materials.push_back(ModelMaterialInfo{});
		model.textures.push_back(Image{});
		model.nodes.push_back(ModelNode{});
		model.skins.push_back(ModelSkin{});
		model.animations.push_back(AnimationClip{});
		bool success = false;
		std::string diagnostics;
		{
			CaptureCerr capture;
			success = LoadModel(path.string(), model);
			diagnostics = capture.Text();
		}
		if(success)
		{
			ValidateModel(model, runner);
			return;
		}
		runner.Require(EmptyModel(model), "rejected mutated model left stale output");
		runner.Require(diagnostics.find(path.string() + ": ") != std::string::npos,
			"rejected mutated model has no useful error diagnostic");
	}

	const std::string& BaseObj()
	{
		static const std::string value = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
		return value;
	}

	void RunModelFixed(const std::filesystem::path& directory, Runner& runner)
	{

		const std::filesystem::path obj = directory / "fixed-valid.obj";
		WriteText(obj, BaseObj());
		CheckLoadResult(obj, true, runner);

		const std::filesystem::path binary = directory / "fixed-base.bin";
		const std::filesystem::path gltf = directory / "fixed-valid.gltf";
		WriteBytes(binary, TriangleBytes());
		WriteText(gltf, TriangleGltf(binary.filename().string()));
		CheckLoadResult(gltf, true, runner);

		const std::filesystem::path badObj = directory / "fixed-nonfinite.obj";
		WriteText(badObj, "v nan 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
		CheckLoadResult(badObj, false, runner);
		const std::filesystem::path truncated = directory / "fixed-truncated.gltf";
		WriteText(truncated, "{\"asset\":");
		CheckLoadResult(truncated, false, runner);
		const std::filesystem::path invalidBinary = directory / "fixed-invalid.bin";
		const std::filesystem::path invalidGltf = directory / "fixed-invalid-index.gltf";
		WriteBytes(invalidBinary, TriangleBytes(999u));
		WriteText(invalidGltf, TriangleGltf(invalidBinary.filename().string()));
		CheckLoadResult(invalidGltf, false, runner);

		ModelLoadOptions limited;
		limited.maxSourceBytes = 1u;
		CheckLoadResult(obj, false, runner, limited);

		std::vector<uint8_t> mutation(BaseObj().begin(), BaseObj().end());
		mutation[0] = 0xffu;
		const std::filesystem::path mutated = directory / "fixed-mutated.obj";
		WriteBytes(mutated, mutation);
		CheckFuzzLoad(mutated, runner);
	}

	std::vector<uint8_t> Mutate(std::vector<uint8_t> bytes, std::mt19937_64& random)
	{
		const uint64_t operation = random() % 5u;
		if(operation == 0u && !bytes.empty())
		{
			const size_t count = 1u + static_cast<size_t>(random() % std::min<size_t>(8u, bytes.size()));
			for(size_t index = 0u; index < count; ++index)
			{
				const size_t position = static_cast<size_t>(random() % bytes.size());
				bytes[position] ^= static_cast<uint8_t>(1u + random() % 255u);
			}
		}
		else if(operation == 1u && !bytes.empty())
		{
			bytes.resize(static_cast<size_t>(random() % (bytes.size() + 1u)));
		}
		else if(operation == 2u && !bytes.empty())
		{
			const size_t first = static_cast<size_t>(random() % bytes.size());
			const size_t count = 1u + static_cast<size_t>(random() % (bytes.size() - first));
			bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(first),
				bytes.begin() + static_cast<std::ptrdiff_t>(first + count));
		}
		else if(operation == 3u && bytes.size() < kMaxPayloadBytes - 64u)
		{
			const size_t count = 1u + static_cast<size_t>(random() % 32u);
			const size_t position = bytes.empty() ? 0u : static_cast<size_t>(random() % (bytes.size() + 1u));
			std::vector<uint8_t> inserted(count);
			for(uint8_t& value : inserted) value = static_cast<uint8_t>(random());
			bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(position), inserted.begin(), inserted.end());
		}
		else
		{
			bytes.push_back(static_cast<uint8_t>(random()));
		}
		if(bytes.size() > kMaxPayloadBytes - 16u) bytes.resize(kMaxPayloadBytes - 16u);
		return bytes;
	}

	bool HasExternalObjDirective(const std::vector<uint8_t>& bytes);

	std::vector<uint8_t> GenerateModelCase(std::mt19937_64& random)
	{
		std::vector<uint8_t> bytes;
		do
		{
			bytes.assign(BaseObj().begin(), BaseObj().end());
			bytes = Mutate(std::move(bytes), random);
		}
		while(HasExternalObjDirective(bytes));
		ByteWriter writer;
		writer.U8(1u);
		writer.U8(0u);
		writer.Bytes(bytes);
		return writer.Take();
	}

	bool HasExternalObjDirective(const std::vector<uint8_t>& bytes)
	{
		for(size_t lineStart = 0u; lineStart < bytes.size();)
		{
			size_t position = lineStart;
			while(position < bytes.size() && (bytes[position] == ' ' || bytes[position] == '\t' || bytes[position] == '\r'))
				++position;
			std::string directive;
			while(position < bytes.size() && bytes[position] > ' ')
			{
				const unsigned char value = bytes[position++];
				directive.push_back(static_cast<char>(value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value));
			}
			if(directive == "mtllib" || directive == "maplib") return true;
			const auto newline = std::find(bytes.begin() + static_cast<std::ptrdiff_t>(lineStart), bytes.end(), '\n');
			lineStart = newline == bytes.end()
				? bytes.size()
				: static_cast<size_t>(newline - bytes.begin()) + 1u;
		}
		return false;
	}

	void RunModelCase(const std::vector<uint8_t>& payload, const std::filesystem::path& directory, Runner& runner)
	{
		ByteReader reader(payload);
		if(reader.U8() != 1u) throw InfraError("unknown ModelImport case kind");
		const uint8_t format = reader.U8();
		if(format != 0u) throw InfraError("unknown ModelImport format");
		const std::vector<uint8_t> bytes = reader.Bytes(kMaxPayloadBytes - 16u);
		reader.Finish();
		if(HasExternalObjDirective(bytes)) throw InfraError("ModelImport case contains an external OBJ directive");
		const std::filesystem::path path = directory / "fuzz-input.obj";
		WriteBytes(path, bytes);
		CheckFuzzLoad(path, runner);
	}

	void ExecuteGraph(RenderGraph& graph, IDevice& device, Runner& runner)
	{
		ICommandList* commands = device.AcquireCommandList();
		if(commands == nullptr) throw InfraError("could not acquire CPU graph command list");
		runner.Require(graph.Execute(commands), "compiled graph failed to record");
		runner.Require(commands->Close(), "graph command list failed to close");
		runner.Require(device.Submit(&commands, 1u), "graph resource states failed at submission");
		device.DestroyCommandList(commands);
	}

	void RunRenderGraphFixed(Runner& runner)
	{
		std::unique_ptr<IDevice> device(IDevice::Create(DeviceDesc{}));
		if(!device) throw InfraError("could not create CPU graph device");
		const auto texture = device->CreateTexture({ 1u, 1u, 1u, 1u, Format::R8G8B8A8_UNORM,
			TextureUsage::ShaderResource | TextureUsage::RenderTarget });
		const auto buffer = device->CreateBuffer({ 16u, 4u, BufferUsage::Storage, ResourceState::Undefined });
		if(!texture || !buffer) throw InfraError("could not create CPU graph resources");
		RenderGraph graph;
		auto a = graph.ImportTexture("A", texture, ResourceState::Undefined, ResourceState::ShaderResource);
		auto b = graph.ImportBuffer("B", buffer, ResourceState::Undefined, ResourceState::UnorderedAccess);
		runner.Require(a.IsValid() && b.IsValid(), "valid resource import failed");
		runner.Require(graph.ImportTexture("A", texture, ResourceState::Undefined, ResourceState::ShaderResource) == a,
			"duplicate resource name changed its handle");
		runner.Require(graph.ImportTexture("Alias", texture, ResourceState::Undefined, ResourceState::ShaderResource) == a,
			"resource alias changed its handle");
		runner.Require(!graph.ImportTexture("Null", nullptr, ResourceState::Undefined, ResourceState::ShaderResource).IsValid(),
			"null graph resource accepted");
		runner.Require(!graph.ImportTexture("Conflict", texture, ResourceState::Common, ResourceState::ShaderResource).IsValid(),
			"conflicting alias boundary states accepted");
		runner.Require(!graph.ImportBuffer("A", buffer, ResourceState::Undefined, ResourceState::UnorderedAccess).IsValid(),
			"conflicting resource name accepted");
		runner.Require(!graph.ImportTexture("Undefined final", texture, ResourceState::Undefined, ResourceState::Undefined).IsValid(),
			"undefined graph final state accepted");
		std::vector<std::string> executed;
		graph.AddPass("Producer").Write(a, ResourceState::RenderTarget)
			.Write(a, ResourceState::RenderTarget)
			.SetExecute([&](auto*) { executed.push_back("Producer"); });
		graph.AddPass("Consumer").Read(a, ResourceState::ShaderResource)
			.Write(b, ResourceState::UnorderedAccess)
			.SetExecute([&](auto*) { executed.push_back("Consumer"); });
		ICommandList* uncompiled = device->AcquireCommandList();
		if(!uncompiled) throw InfraError("could not acquire CPU graph command list");
		runner.Require(!graph.Execute(uncompiled) && executed.empty(), "uncompiled graph executed callbacks");
		device->DestroyCommandList(uncompiled);
		runner.Require(graph.Compile(), "producer-consumer graph did not compile");
		runner.Require(!graph.Execute(nullptr), "null graph command list accepted");
		ExecuteGraph(graph, *device, runner);
		runner.Require(executed == std::vector<std::string>{ "Producer", "Consumer" },
			"producer did not execute before consumer");
		ICommandList* repeated = device->AcquireCommandList();
		if(!repeated) throw InfraError("could not acquire repeated graph command list");
		runner.Require(graph.Execute(repeated) && repeated->Close(), "repeated graph failed to record");
		runner.Require(!device->Submit(&repeated, 1u), "graph execution ignored its initial resource states");
		device->DestroyCommandList(repeated);

		graph.Reset();
		a = graph.ImportTexture("A", texture, ResourceState::ShaderResource, ResourceState::ShaderResource);
		graph.AddPass("Read1").Read(a, ResourceState::ShaderResource).SetExecute([](auto*) {});
		graph.AddPass("Write1").Write(a, ResourceState::RenderTarget).SetExecute([](auto*) {});
		graph.AddPass("Write2").Write(a, ResourceState::RenderTarget).SetExecute([](auto*) {});
		runner.Require(graph.Compile() && graph.GetExecutionOrderNames()
			== std::vector<std::string>{ "Read1", "Write1", "Write2" },
			"read did not precede the next overwrite");
		ExecuteGraph(graph, *device, runner);

		graph.Reset();
		a = graph.ImportTexture("A", texture, ResourceState::ShaderResource, ResourceState::ShaderResource);
		b = graph.ImportBuffer("B", buffer, ResourceState::UnorderedAccess, ResourceState::UnorderedAccess);
		auto& first = graph.AddPass("First").Write(a, ResourceState::RenderTarget).SetExecute([](auto*) {});
		graph.AddPass("Second").Read(a, ResourceState::ShaderResource)
			.Write(b, ResourceState::UnorderedAccess).SetExecute([](auto*) {});
		runner.Require(graph.Compile(), "valid graph did not compile");
		first.Read(b, ResourceState::ShaderResource);
		runner.Require(!graph.IsCompiled(), "compiled graph mutation did not invalidate its plan");
		// Reads use previous contents, so a later writer does not create a backward dependency.
		runner.Require(graph.Compile() && graph.GetExecutionOrderNames() == std::vector<std::string>{ "First", "Second" },
			"external read was incorrectly treated as a dependency cycle");
		ExecuteGraph(graph, *device, runner);
		first.Read(a, ResourceState::ShaderResource);
		runner.Require(!graph.IsCompiled() && !graph.Compile(), "conflicting state mutation compiled");

		graph.Reset();
		graph.AddPass("Stale").Read(a, ResourceState::ShaderResource).SetExecute([](auto*) {});
		runner.Require(!graph.Compile(), "resource handle survived graph reset");
		graph.Reset();
		graph.AddPass("Invalid").Read({ UINT64_MAX }, ResourceState::ShaderResource).SetExecute([](auto*) {});
		runner.Require(!graph.Compile(), "out-of-range graph resource accepted");
		graph.Reset();
		graph.AddPass("Invalid sentinel").Write({}, ResourceState::RenderTarget).SetExecute([](auto*) {});
		runner.Require(!graph.Compile(), "invalid graph resource sentinel accepted");
		graph.Reset();
		graph.AddPass("Missing callback");
		runner.Require(!graph.Compile(), "graph pass without a callback compiled");
		graph.Reset();
		runner.Require(graph.Compile() && graph.GetExecutionOrderNames().empty(), "empty graph did not compile");
		ExecuteGraph(graph, *device, runner);
	}

	struct Edge
	{
		uint8_t from = 0u;
		uint8_t to = 0u;
	};

	std::vector<uint8_t> EncodeGraphCase(uint8_t kind, uint8_t count, const std::vector<Edge>& edges)
	{
		ByteWriter writer;
		writer.U8(kind);
		writer.U8(count);
		writer.U16(static_cast<uint16_t>(edges.size()));
		for(const Edge edge : edges)
		{
			writer.U8(edge.from);
			writer.U8(edge.to);
		}
		return writer.Take();
	}

	std::vector<uint8_t> GenerateGraphCase(std::mt19937_64& random)
	{
		const bool cycle = (random() % 4u) == 0u;
		const uint8_t count = static_cast<uint8_t>(2u + random() % (cycle ? 7u : 11u));
		std::vector<Edge> edges;
		if(cycle)
		{
			for(uint8_t index = 0u; index < count; ++index)
				edges.push_back({ index, static_cast<uint8_t>((index + 1u) % count) });
			return EncodeGraphCase(2u, count, edges);
		}

		std::vector<uint8_t> order(count);
		for(uint8_t index = 0u; index < count; ++index) order[index] = index;
		for(size_t index = order.size(); index > 1u; --index)
			std::swap(order[index - 1u], order[static_cast<size_t>(random() % index)]);
		std::array<std::array<bool, 16>, 16> present{};
		for(uint8_t index = 0u; index + 1u < count; ++index)
		{
			edges.push_back({ order[index], order[index + 1u] });
			present[order[index]][order[index + 1u]] = true;
		}
		for(uint8_t fromRank = 0u; fromRank < count; ++fromRank)
			for(uint8_t toRank = static_cast<uint8_t>(fromRank + 1u); toRank < count; ++toRank)
				if(!present[order[fromRank]][order[toRank]] && (random() % 4u) == 0u)
					edges.push_back({ order[fromRank], order[toRank] });
		return EncodeGraphCase(1u, count, edges);
	}

	void RunGraphCase(const std::vector<uint8_t>& payload, Runner& runner)
	{
		ByteReader reader(payload);
		const uint8_t kind = reader.U8();
		const uint8_t count = reader.U8();
		const uint16_t edgeCount = reader.U16();
		if((kind != 1u && kind != 2u) || count < 2u || count > 16u || edgeCount > 256u)
			throw InfraError("invalid RenderGraph case header");
		std::vector<Edge> edges;
		edges.reserve(edgeCount);
		for(uint16_t index = 0u; index < edgeCount; ++index)
		{
			const Edge edge{ reader.U8(), reader.U8() };
			if(edge.from >= count || edge.to >= count || edge.from == edge.to)
				throw InfraError("invalid RenderGraph edge");
			edges.push_back(edge);
		}
		reader.Finish();

		std::unique_ptr<IDevice> device(IDevice::Create(DeviceDesc{}));
		if(!device) throw InfraError("could not create CPU graph device");
		RenderGraph graph;
		std::vector<RenderGraphPass*> passes;
		std::vector<uint32_t> executed;
		passes.reserve(count);
		for(uint8_t index = 0u; index < count; ++index)
		{
			auto& pass = graph.AddPass("P" + std::to_string(index));
			pass.SetExecute([&, index](auto*) { executed.push_back(index); });
			passes.push_back(&pass);
		}
		for(size_t edgeIndex = 0u; edgeIndex < edges.size(); ++edgeIndex)
		{
			const auto buffer = device->CreateBuffer({ 16u, 4u, BufferUsage::Storage, ResourceState::ShaderResource });
			if(!buffer) throw InfraError("could not create generated graph resource");
			const auto resource = graph.ImportBuffer("E" + std::to_string(edgeIndex), buffer,
				ResourceState::ShaderResource, ResourceState::ShaderResource);
			runner.Require(resource.IsValid(), "generated graph resource import failed");
			passes[edges[edgeIndex].from]->Write(resource, ResourceState::UnorderedAccess);
			passes[edges[edgeIndex].to]->Read(resource, ResourceState::ShaderResource);
		}
		const bool compiled = graph.Compile();
		// Both DAG-shaped and cyclic declarations use the contents preceding each registered pass.
		runner.Require(compiled && graph.IsCompiled(), "ordered RenderGraph case failed to compile");
		const auto& order = graph.GetExecutionOrderIndices();
		runner.Require(order.size() == count, "RenderGraph execution order lost a pass");
		std::vector<uint32_t> position(count, count);
		for(uint32_t index = 0u; index < order.size(); ++index)
		{
			runner.Require(order[index] < count && position[order[index]] == count,
				"RenderGraph execution order contains an invalid or duplicate pass");
			position[order[index]] = index;
		}
		for(const Edge edge : edges)
			runner.Require(position[std::min(edge.from, edge.to)] < position[std::max(edge.from, edge.to)],
				"RenderGraph RAW/WAR dependency violated registration order");
		ExecuteGraph(graph, *device, runner);
		runner.Require(executed == order, "RenderGraph execution differs from compiled order");
	}

	void RunPayload(
		const CaseData& data,
		const std::filesystem::path& caseDirectory,
		Runner& runner)
	{
		if(data.payload == FixedPayload())
		{
			if(data.scenario == "Animation") RunAnimationFixed(runner);
			else if(data.scenario == "ModelImport") RunModelFixed(caseDirectory, runner);
			else if(data.scenario == "RenderGraph") RunRenderGraphFixed(runner);
			else throw InfraError("unknown scenario");
			return;
		}
		if(data.scenario == "Animation") RunAnimationCase(data.payload, runner);
		else if(data.scenario == "ModelImport") RunModelCase(data.payload, caseDirectory, runner);
		else if(data.scenario == "RenderGraph") RunGraphCase(data.payload, runner);
		else throw InfraError("unknown scenario");
	}

	std::vector<uint8_t> GeneratePayload(const std::string& scenario, std::mt19937_64& random)
	{
		if(scenario == "Animation") return GenerateAnimationCase(random);
		if(scenario == "ModelImport") return GenerateModelCase(random);
		if(scenario == "RenderGraph") return GenerateGraphCase(random);
		throw InfraError("unknown scenario");
	}

	uint64_t ParseSeed(std::string_view text)
	{
		uint64_t value = 0u;
		const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
		if(text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size())
			throw InfraError("--seed requires an unsigned integer");
		return value;
	}

	double ParseSeconds(const std::string& text)
	{
		std::istringstream input(text);
		input.imbue(std::locale::classic());
		double value = 0.0;
		if(!(input >> std::noskipws >> value) || !input.eof()
			|| !std::isfinite(value) || value < 0.0 || value > 60.0)
			throw InfraError("--seconds requires a finite value from 0 through 60");
		return value;
	}

	struct Arguments
	{
		bool listScenarios = false;
		bool all = false;
		bool replay = false;
		std::filesystem::path replayPath;
		std::string scenario;
		uint64_t seed = 0u;
		double seconds = 0.0;
		std::filesystem::path caseDirectory;
	};

	Arguments ParseArguments(int argc, char** argv)
	{
		if(argc == 2 && argv[1] != nullptr && std::string_view(argv[1]) == "--list-scenarios")
		{
			Arguments arguments;
			arguments.listScenarios = true;
			return arguments;
		}
		if(argc == 3 && std::string_view(argv[1]) == "--replay")
		{
			if(argv[2] == nullptr || std::string_view(argv[2]).empty()) throw InfraError("--replay requires a file");
			Arguments arguments;
			arguments.replay = true;
			arguments.replayPath = argv[2];
			return arguments;
		}
		Arguments arguments;
		bool haveScenario = false;
		bool haveSeed = false;
		bool haveSeconds = false;
		bool haveCaseDirectory = false;
		for(int index = 1; index < argc;)
		{
			if(argv[index] == nullptr)
				throw InfraError("expected --scenario NAME --seed UINT --seconds FLOAT --case-dir DIR");
			const std::string_view option = argv[index++];
			if(option == "--all" && !arguments.all)
			{
				arguments.all = true;
				continue;
			}
			if(index >= argc || argv[index] == nullptr)
				throw InfraError("expected a value after " + std::string(option));
			const std::string value = argv[index++];
			if(option == "--scenario" && !haveScenario)
			{
				arguments.scenario = value;
				haveScenario = true;
			}
			else if(option == "--seed" && !haveSeed)
			{
				arguments.seed = ParseSeed(value);
				haveSeed = true;
			}
			else if(option == "--seconds" && !haveSeconds)
			{
				arguments.seconds = ParseSeconds(value);
				haveSeconds = true;
			}
			else if(option == "--case-dir" && !haveCaseDirectory)
			{
				arguments.caseDirectory = value;
				haveCaseDirectory = !value.empty();
			}
			else throw InfraError("unknown or duplicate argument: " + std::string(option));
		}
		if(haveScenario == arguments.all || !haveSeed || !haveSeconds || !haveCaseDirectory
			|| (haveScenario && !IsScenario(arguments.scenario)))
			throw InfraError("expected (--scenario {Animation|ModelImport|RenderGraph} | --all) --seed UINT --seconds FLOAT --case-dir DIR");
		return arguments;
	}
}

int main(int argc, char** argv)
{
	std::string scenario = "unknown";
	uint64_t seed = 0u;
	std::filesystem::path reproduction;
	try
	{
		const Arguments arguments = ParseArguments(argc, argv);
		if(arguments.listScenarios)
		{
			std::cout
				<< "Animation\nModelImport\nRenderGraph\n";
			return 0;
		}
		Runner runner;
		if(arguments.replay)
		{
			const CaseData data = ReadCase(arguments.replayPath);
			scenario = data.scenario;
			seed = data.seed;
			reproduction = arguments.replayPath;
			const std::filesystem::path replayBase = arguments.replayPath.parent_path().empty()
				? std::filesystem::current_path()
				: arguments.replayPath.parent_path();
			OwnedRunDirectory owned(replayBase, "replay-" + scenario);
			RunPayload(data, owned.FixtureDirectory(), runner);
			owned.Cleanup();
			std::cout << "scenario=" << scenario << " seed=" << seed
				<< " checks=" << runner.checks << " replay passed\n";
			return 0;
		}

		seed = arguments.seed;
		std::error_code directoryError;
		std::filesystem::create_directories(arguments.caseDirectory, directoryError);
		if(directoryError) throw InfraError("could not create case directory: " + directoryError.message());
		const std::vector<std::string> scenarios = arguments.all
			? std::vector<std::string>{
				"Animation", "ModelImport", "RenderGraph" }
			: std::vector<std::string>{ arguments.scenario };
		for(const std::string& selectedScenario : scenarios)
		{
			scenario = selectedScenario;
			OwnedRunDirectory owned(arguments.caseDirectory, "run-" + scenario);
			reproduction = owned.CaseDirectory() / (scenario + ".case");
			const uint64_t checksBefore = runner.checks;
			CaseData current{ scenario, seed, FixedPayload() };
			WriteCase(reproduction, current);
			try
			{
				RunPayload(current, owned.FixtureDirectory(), runner);
				if(arguments.seconds > 0.0)
				{
					std::mt19937_64 random(seed);
					const auto deadline = std::chrono::steady_clock::now()
						+ std::chrono::duration<double>(arguments.seconds);
					do
					{
						current.payload = GeneratePayload(scenario, random);
						WriteCase(reproduction, current);
						RunPayload(current, owned.FixtureDirectory(), runner);
					}
					while(std::chrono::steady_clock::now() < deadline);
				}
			}
			catch(const CheckFailure&)
			{
				owned.Preserve();
				throw;
			}
			owned.Cleanup();
			std::cout << "scenario=" << scenario << " seed=" << seed
				<< " checks=" << (runner.checks - checksBefore) << " passed\n";
		}
		return 0;
	}
	catch(const CheckFailure& failure)
	{
		std::cerr << "scenario=" << scenario << " seed=" << seed
			<< " failure=" << failure.what();
		if(!reproduction.empty()) std::cerr << " replay=" << reproduction.string();
		std::cerr << '\n';
		return 1;
	}
	catch(const std::exception& error)
	{
		std::cerr << "CiCpuChecks infrastructure error: " << error.what() << '\n';
		return 2;
	}
}
