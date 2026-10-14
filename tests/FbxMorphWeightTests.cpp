#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <dyf/Extends/Model/Model.h>

namespace
{
	struct Checks
	{
		int failures = 0;

		bool Require(bool condition, const std::string& message)
		{
			if(!condition)
			{
				std::cerr << "FAIL: " << message << '\n';
				++failures;
			}
			return condition;
		}

		void Near(float actual, float expected, const std::string& message)
		{
			if(!std::isfinite(actual) || std::fabs(actual - expected) > 1.0e-5f)
			{
				std::cerr << "FAIL: " << message << ": expected " << expected << ", got " << actual << '\n';
				++failures;
			}
		}
	};

	bool CheckFixture(const dyf::ModelData& model, Checks& checks)
	{
		if(!checks.Require(model.meshes.size() == 1u, "fixture must load one mesh")) return false;
		const dyf::ModelMesh& mesh = model.meshes.front();
		if(!checks.Require(mesh.mesh.vertices.size() == 3u, "fixture must load one triangle")
			|| !checks.Require(mesh.nodeIndex < model.nodes.size(), "mesh must reference its model node")
			|| !checks.Require(mesh.morphTargets.size() == 1u, "fixture must load one morph target")
			|| !checks.Require(mesh.defaultMorphWeights.size() == 1u, "mesh must expose one default weight")
			|| !checks.Require(model.nodes[mesh.nodeIndex].morphWeights.size() == 1u,
				"node must expose one default weight")) return false;

		checks.Near(mesh.defaultMorphWeights.front(), 0.5f, "mesh default weight");
		checks.Near(model.nodes[mesh.nodeIndex].morphWeights.front(), 0.5f, "node default weight");
		return true;
	}

	void CheckPositions(
		const dyf::MeshData& mesh,
		const std::array<float, 3>& expectedX,
		const std::string& label,
		Checks& checks)
	{
		if(!checks.Require(mesh.vertices.size() == 3u, label + " vertex count")) return;
		const std::array<float, 3> expectedY = { 0.0f, 0.0f, 1.0f };
		for(size_t vertex = 0u; vertex < 3u; ++vertex)
		{
			const std::string prefix = label + " vertex " + std::to_string(vertex);
			checks.Near(mesh.vertices[vertex].position.x, expectedX[vertex], prefix + " x");
			checks.Near(mesh.vertices[vertex].position.y, expectedY[vertex], prefix + " y");
			checks.Near(mesh.vertices[vertex].position.z, 0.0f, prefix + " z");
		}
	}

	void CheckNonanimatedDefault(const std::filesystem::path& directory, Checks& checks)
	{
		dyf::ModelData model;
		if(!checks.Require(dyf::LoadModel((directory / "default-half.fbx").string(), model),
			"nonanimated FBX must load") || !CheckFixture(model, checks)) return;
		checks.Require(model.animations.empty(), "nonanimated fixture must have no animation clips");
		const dyf::ModelMesh& mesh = model.meshes.front();
		dyf::MeshData deformed;
		if(checks.Require(dyf::EvaluateMorphTargets(
			mesh.mesh, mesh.morphTargets, mesh.defaultMorphWeights, deformed), "default morph must evaluate"))
			CheckPositions(deformed, { 1.0f, 2.0f, 1.0f }, "nonanimated default", checks);
		std::cout << "Nonanimated default coefficient: " << mesh.defaultMorphWeights.front() << '\n';
	}

	void CheckAnimatedWeights(const std::filesystem::path& directory, Checks& checks)
	{
		dyf::ModelLoadOptions options;
		options.fbxBakeRate = 2.0f;
		dyf::ModelData model;
		if(!checks.Require(dyf::LoadModel((directory / "animated.fbx").string(), model, options),
			"animated FBX must load") || !CheckFixture(model, checks)) return;
		if(!checks.Require(model.animations.size() == 1u, "fixture must load one animation clip")) return;
		const dyf::ModelMesh& mesh = model.meshes.front();
		const dyf::AnimationClip& clip = model.animations.front();
		checks.Near(clip.duration, 1.0f, "animation duration");
		if(!checks.Require(clip.morphTracks.size() == 1u, "clip must expose one morph track")) return;
		const dyf::MorphWeightTrack& track = clip.morphTracks.front();
		if(!checks.Require(track.nodeIndex == mesh.nodeIndex && track.targetIndex == 0u,
			"morph track must reference the mesh target")
			|| !checks.Require(track.weights.size() == 3u, "two Hz bake must include all three samples")) return;

		// FBX keys are 0%, 50%, 100%; the target translates every vertex by +2 on X.
		// Literal expectations catch a duplicate percentage conversion without copying loader arithmetic.
		struct Sample
		{
			float time;
			float weight;
			std::array<float, 3> x;
		};
		const std::array<Sample, 3> samples = {{
			{ 0.0f, 0.0f, { 0.0f, 1.0f, 0.0f } },
			{ 0.5f, 0.5f, { 1.0f, 2.0f, 1.0f } },
			{ 1.0f, 1.0f, { 2.0f, 3.0f, 2.0f } },
		}};
		for(size_t sampleIndex = 0u; sampleIndex < samples.size(); ++sampleIndex)
		{
			const Sample& sample = samples[sampleIndex];
			const std::string label = "animation at " + std::to_string(sample.time);
			checks.Near(track.weights[sampleIndex].time, sample.time, label + " baked time");
			checks.Near(track.weights[sampleIndex].value, sample.weight, label + " baked coefficient");

			std::vector<dyf::NodeTransform> pose;
			std::vector<std::vector<float>> weights;
			for(const dyf::ModelNode& node : model.nodes)
			{
				pose.push_back(node.bindTransform);
				weights.push_back(node.morphWeights);
			}
			if(!checks.Require(dyf::SampleAnimationClip(clip, sample.time, pose, weights),
				label + " must sample")) continue;
			checks.Near(weights[mesh.nodeIndex].front(), sample.weight, label + " sampled coefficient");
			dyf::MeshData deformed;
			if(checks.Require(dyf::EvaluateMorphTargets(
				mesh.mesh, mesh.morphTargets, weights[mesh.nodeIndex], deformed), label + " morph must evaluate"))
				CheckPositions(deformed, sample.x, label, checks);
			std::cout << label << " coefficient: " << weights[mesh.nodeIndex].front() << '\n';
		}
	}
}

int main(int argc, char** argv)
{
	if(argc != 2)
	{
		std::cerr << "Usage: FbxMorphWeightTests FIXTURE_DIRECTORY\n";
		return 2;
	}
	Checks checks;
	CheckNonanimatedDefault(argv[1], checks);
	CheckAnimatedWeights(argv[1], checks);
	std::cout << "FBX morph weight failures: " << checks.failures << '\n';
	return checks.failures == 0 ? 0 : 1;
}
